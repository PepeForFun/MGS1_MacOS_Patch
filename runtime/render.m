/* Metal renderer for the DirectDraw/Direct3D layer.
 * Render targets are 4x MSAA at the window's pixel resolution; the game's own resolution is only
 * a coordinate system. All drawing of a frame is encoded into one render pass when possible. */
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <ImageIO/ImageIO.h>
#import <CoreServices/CoreServices.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "rt.h"
#include "render.h"
#include "hdtex.h"

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static id<MTLLibrary> g_lib;
static id<MTLRenderPipelineState> g_present_pso;
static id<MTLSamplerState> g_samp[2][4]; /* [filter][clampU | clampV<<1] */
static id<MTLSamplerState> g_samp_hd[4];  /* HD replacements: trilinear, anisotropic */
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *g_psos;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *g_dss;
static id<MTLTexture> g_white, g_gamma;
static const int MSAA = 4;

struct RTex {
    id<MTLTexture> t;
    int w, h;
    bool used, art;
    uint64_t key;          /* content hash of the last upload */
    id<MTLTexture> hd;     /* its HD replacement, for 3D world draws */
    bool hd_wait;          /* the replacement is still being decoded */
    uint32_t *src;         /* last upload, kept while dumping or while the lookup waits */
    bool dumped;
    uint64_t t_change;     /* when the content last changed */
    int quick;             /* changes in a row, each less than 0.25 s after the one before */
    bool later;            /* changed while animated: HD lookup waits until it holds still */
};
void r_tex_set_art(RTex *t, bool art) { t->art = art; }
struct RTarget {
    int lw, lh, pw, ph;
    id<MTLTexture> msaa, resolve, depth;
    id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> enc;
    bool cleared_once;
    id<MTLTexture> overlay[4];     /* GDI layer uploads; reused once the GPU is done with them */
    uint64_t overlay_cb[4];        /* serial of the command buffer that reads it */
    uint64_t version, snap_version;  /* snapshot of the target for copies from itself */
    id<MTLTexture> snap;
};

/* per-frame vertex/index storage */
#define RING (32u << 20)
static id<MTLBuffer> g_ring[3];
static int g_ring_i;
static uint32_t g_ring_off;

static NSString *const kShaders = @"\n"
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float4 pos [[attribute(0)]]; uchar4 diffuse [[attribute(1)]]; uchar4 spec [[attribute(2)]]; float2 uv [[attribute(3)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 col; float4 spec; float2 uv; };\n"
"struct U { int color_op, color_a1, color_a2, alpha_op, alpha_a1, alpha_a2; int alpha_func; float alpha_ref; int colorkey, has_tex, specular, xbr; float4 tfactor; float2 tsize; int smooth; };\n"
"vertex VOut vs_main(VIn v [[stage_in]]) {\n"
"  VOut o; o.pos = v.pos; o.col = float4(v.diffuse.zyxw) / 255.0; o.spec = float4(v.spec.zyxw) / 255.0; o.uv = v.uv; return o; }\n"
"static float4 arg(int a, float4 tex, float4 cur, float4 tf, float4 spec) {\n"
"  switch (a & 7) { case 0: return cur; case 1: return cur; case 2: return tex; case 3: return tf; case 4: return spec; default: return cur; } }\n"
"static float4 op(int o, float4 a, float4 b) {\n"
"  switch (o) { case 2: return a; case 3: return b; case 4: return a * b; case 5: return saturate(a * b * 2); case 6: return saturate(a * b * 4);\n"
"    case 7: return saturate(a + b); case 8: return saturate(a + b - 0.5); default: return a * b; } }\n"
"static bool cmp(int f, float a, float r) {\n"
"  switch (f) { case 1: return false; case 2: return a < r; case 3: return a == r; case 4: return a <= r; case 5: return a > r; case 6: return a != r; case 7: return a >= r; default: return true; } }\n"
"// xBR level 2 (after Hyllian's single-pass xBR-lv2), from the MechCommander port, extended to RGBA:\n"
"// edge-directed enlargement of pixel art (bitmap fonts, menus, HUD) instead of plain bilinear.\n"
"static float4 Wx(texture2d<float> t, float2 p, float2 size) { uint2 q = uint2(clamp(p, float2(0.0), size - 1.0)); return t.read(q); }\n"
"static float4 Luma4(float3 a, float3 b, float3 c, float3 d) { const float3 Y = float3(0.2126, 0.7152, 0.0722) * 48.0; return float4(dot(a, Y), dot(b, Y), dot(c, Y), dot(d, Y)); }\n"
"static float4 Df(float4 a, float4 b) { return abs(a - b); }\n"
"static bool4 Eq(float4 a, float4 b) { return Df(a, b) < 15.0; }\n"
"static bool4 Neq(float4 a, float4 b) { return !Eq(a, b); }\n"
"static float4 WD(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h) { return Df(a, b) + Df(a, c) + Df(d, e) + Df(d, f) + 4.0 * Df(g, h); }\n"
"static float CDf(float4 a, float4 b) { float4 d = abs(a - b); return d.r + d.g + d.b + d.a; }\n"
"static float4 Xbr(texture2d<float> t, float2 uv, float2 size, float scale) {\n"
"  const float2 pos = floor(uv); const float2 fp = uv - pos;\n"
"  const float4 A1 = Wx(t, pos + float2(-1, -2), size), B1 = Wx(t, pos + float2(0, -2), size), C1 = Wx(t, pos + float2(1, -2), size);\n"
"  const float4 A0 = Wx(t, pos + float2(-2, -1), size), A = Wx(t, pos + float2(-1, -1), size), B = Wx(t, pos + float2(0, -1), size);\n"
"  const float4 C = Wx(t, pos + float2(1, -1), size), C4 = Wx(t, pos + float2(2, -1), size);\n"
"  const float4 D0 = Wx(t, pos + float2(-2, 0), size), D = Wx(t, pos + float2(-1, 0), size), E = Wx(t, pos, size);\n"
"  const float4 F = Wx(t, pos + float2(1, 0), size), F4 = Wx(t, pos + float2(2, 0), size);\n"
"  const float4 G0 = Wx(t, pos + float2(-2, 1), size), G = Wx(t, pos + float2(-1, 1), size), H = Wx(t, pos + float2(0, 1), size);\n"
"  const float4 I = Wx(t, pos + float2(1, 1), size), I4 = Wx(t, pos + float2(2, 1), size);\n"
"  const float4 G5 = Wx(t, pos + float2(-1, 2), size), H5 = Wx(t, pos + float2(0, 2), size), I5 = Wx(t, pos + float2(1, 2), size);\n"
"  const float4 b = Luma4(B.rgb, D.rgb, H.rgb, F.rgb), c = Luma4(C.rgb, A.rgb, G.rgb, I.rgb), e = Luma4(E.rgb, E.rgb, E.rgb, E.rgb);\n"
"  const float4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;\n"
"  const float4 i4 = Luma4(I4.rgb, C1.rgb, A0.rgb, G5.rgb), i5 = Luma4(I5.rgb, C4.rgb, A1.rgb, G0.rgb), h5 = Luma4(H5.rgb, F4.rgb, B1.rgb, D0.rgb), f4 = h5.yzwx;\n"
"  const float4 Ao = float4(1.0, -1.0, -1.0, 1.0), Bo = float4(1.0, 1.0, -1.0, -1.0), Co = float4(1.5, 0.5, -0.5, 0.5);\n"
"  const float4 Ax = float4(1.0, -1.0, -1.0, 1.0), Bx = float4(0.5, 2.0, -0.5, -2.0), Cx = float4(1.0, 1.0, -0.5, 0.0);\n"
"  const float4 Ay = float4(1.0, -1.0, -1.0, 1.0), By = float4(2.0, 0.5, -2.0, -0.5), Cy = float4(2.0, 0.0, -1.0, 0.5);\n"
"  const float4 Ci = float4(0.25);\n"
"  const float4 fx = Ao * fp.y + Bo * fp.x, fxLeft = Ax * fp.y + Bx * fp.x, fxUp = Ay * fp.y + By * fp.x;\n"
"  const bool4 lv0 = (e != f) && (e != h);\n"
"  const bool4 lv1 = lv0 && ((Neq(f, b) && Neq(h, d)) || (Eq(e, i) && Neq(f, i4) && Neq(h, i5)) || Eq(e, g) || Eq(e, c));\n"
"  const bool4 lv2Left = (e != g) && (d != g), lv2Up = (e != c) && (b != c);\n"
"  const float4 delta = float4(1.0 / max(scale, 1.0));\n"
"  const float4 fx45i = saturate((fx + delta - Co - Ci) / (2.0 * delta)), fx45 = saturate((fx + delta - Co) / (2.0 * delta));\n"
"  const float4 fx30 = saturate((fxLeft + delta - Cx) / (2.0 * delta)), fx60 = saturate((fxUp + delta - Cy) / (2.0 * delta));\n"
"  const float4 wd1 = WD(e, c, g, i, h5, f4, h, f), wd2 = WD(h, d, i5, f, i4, b, e, i);\n"
"  const bool4 edri = (wd1 <= wd2) && lv0, edr = (wd1 < wd2) && lv1;\n"
"  const bool4 edrLeft = (2.0 * Df(f, g) <= Df(h, c)) && lv2Left && edr, edrUp = (Df(f, g) >= 2.0 * Df(h, c)) && lv2Up && edr;\n"
"  const float4 m45 = float4(edr) * fx45, m30 = float4(edrLeft) * fx30, m60 = float4(edrUp) * fx60, m45i = float4(edri) * fx45i;\n"
"  const bool4 px = Df(e, f) <= Df(e, h);\n"
"  const float4 mx = max(max(m30, m60), max(m45, m45i));\n"
"  float4 r1 = E; r1 = mix(r1, px.x ? F : H, mx.x); r1 = mix(r1, px.z ? D : B, mx.z);\n"
"  float4 r2 = E; r2 = mix(r2, px.y ? B : F, mx.y); r2 = mix(r2, px.w ? H : D, mx.w);\n"
"  return CDf(E, r1) >= CDf(E, r2) ? r1 : r2; }\n"
"// Catmull-Rom bicubic from 9 bilinear taps: magnified textures without bilinear's visible texel grid\n"
"static float4 CatRom(texture2d<float> t, sampler s, float2 uv, float2 size) {\n"
"  float2 sp = uv * size, tp = floor(sp - 0.5) + 0.5, f = sp - tp;\n"
"  float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
"  float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
"  float2 w12 = w1 + w2, t0 = (tp - 1.0) / size, t3 = (tp + 2.0) / size, t12 = (tp + w2 / w12) / size;\n"
"  float4 r = t.sample(s, float2(t0.x, t0.y)) * (w0.x * w0.y) + t.sample(s, float2(t12.x, t0.y)) * (w12.x * w0.y)\n"
"    + t.sample(s, float2(t3.x, t0.y)) * (w3.x * w0.y) + t.sample(s, float2(t0.x, t12.y)) * (w0.x * w12.y)\n"
"    + t.sample(s, float2(t12.x, t12.y)) * (w12.x * w12.y) + t.sample(s, float2(t3.x, t12.y)) * (w3.x * w12.y)\n"
"    + t.sample(s, float2(t0.x, t3.y)) * (w0.x * w3.y) + t.sample(s, float2(t12.x, t3.y)) * (w12.x * w3.y)\n"
"    + t.sample(s, float2(t3.x, t3.y)) * (w3.x * w3.y);\n"
"  return clamp(r, 0.0, 1.0); }\n"
"fragment float4 fs_main(VOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant U &u [[buffer(0)]]) {\n"
"  float4 tex = float4(1);\n"
"  if (u.has_tex) {\n"
"    if (u.xbr) { float2 tp = i.uv * u.tsize; float sc = 1.0 / max(length(dfdx(tp)), 1e-4); tex = sc > 1.5 ? Xbr(t, tp, u.tsize, sc) : t.sample(s, i.uv); }\n"
"    else if (u.smooth) tex = CatRom(t, s, i.uv, u.tsize);\n"
"    else tex = t.sample(s, i.uv);\n"
"    if (tex.a > 0.0) tex.rgb /= tex.a; }\n"
"  if (u.colorkey && u.has_tex && tex.a < 0.5) discard_fragment();\n"
"  float4 cur = i.col;\n"
"  float4 c;\n"
"  if (u.color_op == 1) c.rgb = cur.rgb; else c.rgb = op(u.color_op, arg(u.color_a1, tex, cur, u.tfactor, i.spec), arg(u.color_a2, tex, cur, u.tfactor, i.spec)).rgb;\n"
"  if (u.alpha_op == 1) c.a = cur.a; else c.a = op(u.alpha_op, arg(u.alpha_a1, tex, cur, u.tfactor, i.spec), arg(u.alpha_a2, tex, cur, u.tfactor, i.spec)).a;\n"
"  if (u.specular) c.rgb = saturate(c.rgb + i.spec.rgb);\n"
"  if (u.alpha_func && !cmp(u.alpha_func, c.a, u.alpha_ref)) discard_fragment();\n"
"  return c; }\n"
"struct QOut { float4 pos [[position]]; float2 uv; };\n"
"vertex QOut vs_quad(uint id [[vertex_id]], constant float4 *q [[buffer(0)]]) {\n"
"  float2 c = float2(id & 1, id >> 1); QOut o;\n"
"  o.pos = float4(mix(q[0].x, q[0].z, c.x), mix(q[0].y, q[0].w, c.y), q[2].x, 1);\n"
"  o.uv = float2(mix(q[1].x, q[1].z, c.x), mix(q[1].y, q[1].w, c.y)); return o; }\n"
"fragment float4 fs_blit(QOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 &k [[buffer(0)]]) {\n"
"  float4 c = t.sample(s, i.uv); if (k.x > 0.5 && c.a < 0.5) discard_fragment(); if (c.a > 0.0) c.rgb /= c.a; return float4(c.rgb, 1); }\n"
"fragment float4 fs_color(QOut i [[stage_in]], constant float4 &k [[buffer(0)]]) { return k; }\n"
"fragment float4 fs_overlay(QOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, i.uv); }\n"
"fragment float4 fs_present(QOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], texture2d<float> lut [[texture(1)]]) {\n"
"  float3 c = t.sample(s, i.uv).rgb; constexpr sampler ls(filter::linear, address::clamp_to_edge);\n"
"  float u0 = 0.5 / 256.0, k = 255.0 / 256.0;\n"
"  return float4(lut.sample(ls, float2(u0 + c.r * k, 0.5)).r, lut.sample(ls, float2(u0 + c.g * k, 0.5)).g, lut.sample(ls, float2(u0 + c.b * k, 0.5)).b, 1); }\n";

static MTLBlendFactor bf(int f) {
    switch (f) {
    case R_BLEND_ZERO: return MTLBlendFactorZero;
    case R_BLEND_ONE: return MTLBlendFactorOne;
    case R_BLEND_SRCCOLOR: return MTLBlendFactorSourceColor;
    case R_BLEND_INVSRCCOLOR: return MTLBlendFactorOneMinusSourceColor;
    case R_BLEND_SRCALPHA: return MTLBlendFactorSourceAlpha;
    case R_BLEND_INVSRCALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case R_BLEND_DESTALPHA: return MTLBlendFactorDestinationAlpha;
    case R_BLEND_INVDESTALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case R_BLEND_DESTCOLOR: return MTLBlendFactorDestinationColor;
    case R_BLEND_INVDESTCOLOR: return MTLBlendFactorOneMinusDestinationColor;
    case R_BLEND_SRCALPHASAT: return MTLBlendFactorSourceAlphaSaturated;
    default: return MTLBlendFactorOne;
    }
}

/* pipelines: kind 0 = 3D, 1 = blit, 2 = colour quad, 3 = overlay (premultiplied), 4 = present */
static id<MTLRenderPipelineState> pso(int kind, bool blend, int src, int dst, bool colorwrite) {
    uint32_t key = (uint32_t)kind | (blend ? 8u : 0) | ((uint32_t)src << 4) | ((uint32_t)dst << 8) | (colorwrite ? 0x1000u : 0);
    id<MTLRenderPipelineState> p = g_psos[@(key)];
    if (p) return p;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    const char *vs = kind == 0 ? "vs_main" : "vs_quad";
    const char *fs = kind == 0 ? "fs_main" : kind == 1 ? "fs_blit" : kind == 2 ? "fs_color" : kind == 3 ? "fs_overlay" : "fs_present";
    d.vertexFunction = [g_lib newFunctionWithName:@(vs)];
    d.fragmentFunction = [g_lib newFunctionWithName:@(fs)];
    if (kind == 0) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
        vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0;
        vd.attributes[1].format = MTLVertexFormatUChar4; vd.attributes[1].offset = 16;
        vd.attributes[2].format = MTLVertexFormatUChar4; vd.attributes[2].offset = 20;
        vd.attributes[3].format = MTLVertexFormatFloat2; vd.attributes[3].offset = 24;
        vd.layouts[0].stride = sizeof(RVertex);
        d.vertexDescriptor = vd;
    }
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (kind != 4) {
        d.rasterSampleCount = MSAA;
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    }
    d.colorAttachments[0].writeMask = colorwrite ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    if (blend || kind == 3) {
        d.colorAttachments[0].blendingEnabled = YES;
        MTLBlendFactor s = kind == 3 ? MTLBlendFactorOne : bf(src), t = kind == 3 ? MTLBlendFactorOneMinusSourceAlpha : bf(dst);
        d.colorAttachments[0].sourceRGBBlendFactor = s;
        d.colorAttachments[0].sourceAlphaBlendFactor = s;
        d.colorAttachments[0].destinationRGBBlendFactor = t;
        d.colorAttachments[0].destinationAlphaBlendFactor = t;
    }
    NSError *err = nil;
    p = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) rt_fatal("pipeline: %s", err.localizedDescription.UTF8String);
    g_psos[@(key)] = p;
    return p;
}
static id<MTLDepthStencilState> dss(bool test, bool write, int func) {
    uint32_t key = (test ? 1u : 0) | (write ? 2u : 0) | ((uint32_t)func << 4);
    id<MTLDepthStencilState> s = g_dss[@(key)];
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    static const MTLCompareFunction f[9] = {MTLCompareFunctionLessEqual, MTLCompareFunctionNever, MTLCompareFunctionLess,
        MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
        MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    d.depthCompareFunction = test ? f[func >= 1 && func <= 8 ? func : 4] : MTLCompareFunctionAlways;
    d.depthWriteEnabled = write;
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    g_dss[@(key)] = s;
    return s;
}

void r_init(void) {
    if (g_dev) return;
    CAMetalLayer *layer = (__bridge CAMetalLayer *)plat_metal_layer();
    g_dev = layer.device ? layer.device : MTLCreateSystemDefaultDevice();
    g_queue = [g_dev newCommandQueue];
    NSError *err = nil;
    g_lib = [g_dev newLibraryWithSource:kShaders options:nil error:&err];
    if (!g_lib) rt_fatal("shader compile failed: %s", err.localizedDescription.UTF8String);
    g_psos = [NSMutableDictionary new];
    g_dss = [NSMutableDictionary new];
    for (int f = 0; f < 2; f++)
        for (int k = 0; k < 4; k++) {
            MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
            s.minFilter = s.magFilter = f ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            s.sAddressMode = (k & 1) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
            s.tAddressMode = (k & 2) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
            g_samp[f][k] = [g_dev newSamplerStateWithDescriptor:s];
        }
    for (int k = 0; k < 4; k++) {
        MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
        s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear;
        s.mipFilter = MTLSamplerMipFilterLinear;
        s.maxAnisotropy = 8;
        s.sAddressMode = (k & 1) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        s.tAddressMode = (k & 2) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        g_samp_hd[k] = [g_dev newSamplerStateWithDescriptor:s];
    }
    hdtex_init();
    for (int i = 0; i < 3; i++) g_ring[i] = [g_dev newBufferWithLength:RING options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    g_white = [g_dev newTextureWithDescriptor:td];
    uint32_t w = 0xFFFFFFFFu;
    [g_white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&w bytesPerRow:4];
    uint16_t ident[768];
    for (int i = 0; i < 768; i++) ident[i] = (uint16_t)((i % 256) * 257);
    r_set_gamma(ident);
    rt_log("renderer: %s", g_dev.name.UTF8String);
}

static uint32_t ring_put(const void *p, uint32_t n, id<MTLBuffer> *buf) {
    uint32_t off = (g_ring_off + 15) & ~15u;
    if (off + n > RING) {
        r_flush();
        g_ring_i = (g_ring_i + 1) % 3;
        off = 0;
    }
    memcpy((uint8_t *)g_ring[g_ring_i].contents + off, p, n);
    g_ring_off = off + n;
    *buf = g_ring[g_ring_i];
    return off;
}

/* ------------------------------------------------------------------ textures */
static id<MTLTexture> new_tex(int w, int h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    return [g_dev newTextureWithDescriptor:d];
}
RTex *r_tex_create(int w, int h) {
    RTex *t = calloc(1, sizeof *t);
    t->w = w;
    t->h = h;
    t->t = new_tex(w, h);
    return t;
}
/* HD replacements are decoded and mipmapped (box-filtered levels, premultiplied) on a background
 * queue the first time a picture is seen; draws use the original until it is ready, so entering an
 * area does not stall. One texture per picture, shared by every surface showing it, and held only
 * by those surfaces: when an area unloads, its HD textures go too. */
static NSMapTable<NSNumber *, id<MTLTexture>> *g_hd_tex;   /* key -> texture (weak) */
static NSMutableArray<id<MTLTexture>> *g_hd_fresh;          /* decoded, not yet picked up by a surface */
static NSMutableDictionary<NSNumber *, id> *g_hd_state;    /* key -> @YES (decoding) or NSNull (unusable) */
static pthread_mutex_t g_hd_mu = PTHREAD_MUTEX_INITIALIZER;
static id<MTLTexture> hd_build(uint32_t *px, int hw, int hh) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)hw height:(NSUInteger)hh mipmapped:YES];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    uint32_t *cur = px;
    int cw = hw, ch = hh;
    for (NSUInteger l = 0; l < t.mipmapLevelCount; l++) {
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)cw, (NSUInteger)ch) mipmapLevel:l withBytes:cur bytesPerRow:(NSUInteger)cw * 4];
        int nw = cw > 1 ? cw / 2 : 1, nh = ch > 1 ? ch / 2 : 1;
        if (nw == cw && nh == ch) break;
        uint32_t *next = malloc((size_t)nw * (size_t)nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                uint32_t sum[4] = {0, 0, 0, 0};
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        int xx = 2 * x + i < cw ? 2 * x + i : cw - 1, yy = 2 * y + j < ch ? 2 * y + j : ch - 1;
                        uint32_t c = cur[(size_t)yy * (size_t)cw + (size_t)xx];
                        for (int b = 0; b < 4; b++) sum[b] += (c >> (8 * b)) & 0xFF;
                    }
                next[(size_t)y * (size_t)nw + (size_t)x] = (sum[0] + 2) / 4 | ((sum[1] + 2) / 4) << 8 |
                                                           ((sum[2] + 2) / 4) << 16 | ((sum[3] + 2) / 4) << 24;
            }
        if (cur != px) free(cur);
        cur = next;
        cw = nw;
        ch = nh;
    }
    if (cur != px) free(cur);
    return t;
}
static void hd_request(uint64_t key, const uint32_t *bgra, int w, int h, int stride_px) {
    pthread_mutex_lock(&g_hd_mu);
    if (!g_hd_tex) {
        g_hd_tex = [NSMapTable strongToWeakObjectsMapTable];
        g_hd_state = [NSMutableDictionary new];
        g_hd_fresh = [NSMutableArray new];
    }
    bool start = !g_hd_state[@(key)] && ![g_hd_tex objectForKey:@(key)];
    if (start) g_hd_state[@(key)] = @YES;
    pthread_mutex_unlock(&g_hd_mu);
    if (!start) return;
    uint32_t *copy = malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; y < h; y++) memcpy(copy + (size_t)y * (size_t)w, bgra + (size_t)y * (size_t)stride_px, (size_t)w * 4);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        int hw, hh;
        uint32_t *px = hdtex_load(key, copy, w, h, w, &hw, &hh);
        free(copy);
        id<MTLTexture> t = px ? hd_build(px, hw, hh) : nil;
        free(px);
        pthread_mutex_lock(&g_hd_mu);
        if (t) {
            [g_hd_tex setObject:t forKey:@(key)];
            [g_hd_state removeObjectForKey:@(key)];
            [g_hd_fresh addObject:t];   /* the weak table alone would drop it before a surface takes it */
            if (g_hd_fresh.count > 512) [g_hd_fresh removeObjectAtIndex:0];
        } else {
            g_hd_state[@(key)] = [NSNull null];
        }
        pthread_mutex_unlock(&g_hd_mu);
    });
}
/* the HD texture for key if decoded; *final is false while it is still being decoded */
static id<MTLTexture> hd_ready(uint64_t key, bool *final) {
    pthread_mutex_lock(&g_hd_mu);
    id<MTLTexture> t = [g_hd_tex objectForKey:@(key)];
    id st = g_hd_state[@(key)];
    if (t) [g_hd_fresh removeObjectIdenticalTo:t];   /* the surface asking holds it from now on */
    pthread_mutex_unlock(&g_hd_mu);
    *final = t || (st && ![st isEqual:@YES]);
    return t;
}

/* A texture whose content keeps changing (s15b's water caustics are redrawn every frame) has a new
 * key every frame: HD replacements could match only some frames, so its look would switch back and
 * forth, and the dump would fill with frames. After 3 changes in a row less than 0.25 s apart it
 * counts as animated and keeps its original look; once it has held still for a second it is looked
 * up like any texture. Dumps wait for half a second of stillness, so no animation frame is written. */
static bool tex_animated(const RTex *t) { return t->quick >= 3 && plat_ticks_us() - t->t_change < 1000000; }
bool r_tex_animated(const RTex *t) { return t && tex_animated(t); }
static void tex_lookup(RTex *t, const uint32_t *bgra, int stride_px) {
    t->hd_wait = hdtex_has(t->key);
    if (t->hd_wait) {
        bool final;
        hd_request(t->key, bgra, t->w, t->h, stride_px);
        t->hd = hd_ready(t->key, &final);
        t->hd_wait = !final;
    }
}

void r_tex_upload(RTex *t, const uint32_t *bgra, int stride_px) {
    uint64_t key = hdtex_key(bgra, t->w, t->h, stride_px);
    if (key != t->key) {
        uint64_t now = plat_ticks_us();
        t->quick = now - t->t_change < 250000 ? t->quick + (t->quick < 100) : 0;
        if (t->quick == 3) rt_log("animated texture %dx%d: original look until it holds still", t->w, t->h);
        t->t_change = now;
    }
    if (key != t->key || (!t->hd && !t->src && hdtex_dumping())) {
        t->key = key;
        t->hd = nil;
        t->hd_wait = false;
        t->later = tex_animated(t);
        if (!t->later) tex_lookup(t, bgra, stride_px);
        free(t->src);
        t->src = NULL;
        if (t->later || (hdtex_dumping() && !hdtex_has(key))) {
            t->src = malloc((size_t)t->w * (size_t)t->h * 4);
            for (int y = 0; y < t->h; y++)
                memcpy(t->src + (size_t)y * (size_t)t->w, bgra + (size_t)y * (size_t)stride_px, (size_t)t->w * 4);
            t->dumped = false;
        }
    }
    /* premultiply so filtering never bleeds colour out of transparent texels */
    size_t n = (size_t)t->w * (size_t)t->h;
    uint32_t *tmp = malloc(n * 4);
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            uint32_t c = bgra[(size_t)y * (size_t)stride_px + (size_t)x], a = c >> 24;
            if (a == 255) tmp[(size_t)y * (size_t)t->w + (size_t)x] = c;
            else {
                uint32_t r = ((c >> 16) & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, b = (c & 0xFF) * a / 255;
                tmp[(size_t)y * (size_t)t->w + (size_t)x] = a << 24 | r << 16 | g << 8 | b;
            }
        }
    /* a texture still referenced by queued GPU work gets a fresh backing texture instead */
    if (t->used) { t->t = new_tex(t->w, t->h); t->used = false; }
    [t->t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)t->w, (NSUInteger)t->h) mipmapLevel:0 withBytes:tmp bytesPerRow:(NSUInteger)t->w * 4];
    free(tmp);
}
void r_tex_destroy(RTex *t) {
    if (!t) return;
    t->t = nil;
    t->hd = nil;
    free(t->src);
    free(t);
}

/* ------------------------------------------------------------------ targets */
static void target_alloc(RTarget *t) {
    int dw, dh;
    plat_drawable_size(&dw, &dh);
    /* fill the drawable's height with the 4:3 (or whatever) game image */
    t->ph = dh > 0 ? dh : t->lh;
    if (t->ph < t->lh) t->ph = t->lh;
    t->pw = (int)((int64_t)t->ph * t->lw / t->lh);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)t->pw height:(NSUInteger)t->ph mipmapped:NO];
    d.textureType = MTLTextureType2DMultisample;
    d.sampleCount = MSAA;
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    t->msaa = [g_dev newTextureWithDescriptor:d];
    d.pixelFormat = MTLPixelFormatDepth32Float;
    t->depth = [g_dev newTextureWithDescriptor:d];
    MTLTextureDescriptor *r = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)t->pw height:(NSUInteger)t->ph mipmapped:NO];
    r.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    r.storageMode = MTLStorageModePrivate;
    t->resolve = [g_dev newTextureWithDescriptor:r];
    t->cleared_once = false;
    rt_log("render target %dx%d -> %dx%d pixels, %dx MSAA", t->lw, t->lh, t->pw, t->ph, MSAA);
}
RTarget *r_target_create(int lw, int lh) {
    r_init();
    RTarget *t = calloc(1, sizeof *t);
    t->lw = lw;
    t->lh = lh;
    target_alloc(t);
    return t;
}
static void quad_from(RTarget *t, id<MTLTexture> tex, float u0, float v0, float u1, float v1, int dx0, int dy0,
                      int dx1, int dy1, bool keyed, bool filter);
void r_target_resize(RTarget *t, int lw, int lh) {
    r_flush();
    id<MTLTexture> old = t->resolve;
    t->lw = lw;
    t->lh = lh;
    target_alloc(t);
    /* keep the picture: a paused game (pause menu) draws on the front buffer without new frames */
    if (old) quad_from(t, old, 0, 0, 1, 1, 0, 0, lw, lh, false, true);
    t->version++;
}
void r_target_size(RTarget *t, int *pw, int *ph) { *pw = t->pw; *ph = t->ph; }
static void end_pass(void);
static RTarget *g_active;
void r_target_destroy(RTarget *t) {
    if (g_active == t) end_pass();
    t->msaa = t->resolve = t->depth = t->snap = nil;
    t->cb = nil;
    t->enc = nil;
    for (int i = 0; i < 4; i++) t->overlay[i] = nil;
    free(t);
}

/* g_active: target whose render pass is open */
static id<MTLCommandBuffer> g_cb;
static uint64_t g_cb_serial;              /* serial of g_cb, the command buffer being encoded */
static _Atomic uint64_t g_done_serial;    /* every command buffer up to this serial has completed */
static void need_cb(void) {
    if (g_cb) return;
    g_cb = [g_queue commandBuffer];
    uint64_t serial = ++g_cb_serial;
    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        (void)b;
        uint64_t d = atomic_load(&g_done_serial);
        while (d < serial && !atomic_compare_exchange_weak(&g_done_serial, &d, serial)) {}
    }];
}
static void end_pass(void) {
    if (!g_active) return;
    [g_active->enc endEncoding];
    g_active->enc = nil;
    g_active = NULL;
}
static void begin_pass(RTarget *t) {
    if (g_active == t) return;
    end_pass();
    need_cb();
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t->msaa;
    p.colorAttachments[0].resolveTexture = t->resolve;
    p.colorAttachments[0].loadAction = t->cleared_once ? MTLLoadActionLoad : MTLLoadActionClear;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    p.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    p.depthAttachment.texture = t->depth;
    p.depthAttachment.loadAction = t->cleared_once ? MTLLoadActionLoad : MTLLoadActionClear;
    p.depthAttachment.clearDepth = 1;
    p.depthAttachment.storeAction = MTLStoreActionStore;
    t->cleared_once = true;
    t->enc = [g_cb renderCommandEncoderWithDescriptor:p];
    [t->enc setDepthClipMode:MTLDepthClipModeClamp];
    g_active = t;
}
void r_flush(void) {
    end_pass();
    if (g_cb) {
        [g_cb commit];
        g_cb = nil;
    }
}

static void logical_to_ndc(RTarget *t, float x0, float y0, float x1, float y1, float *q) {
    q[0] = x0 / t->lw * 2 - 1;
    q[1] = 1 - y0 / t->lh * 2;
    q[2] = x1 / t->lw * 2 - 1;
    q[3] = 1 - y1 / t->lh * 2;
}
static void full_viewport(RTarget *t) {
    [t->enc setViewport:(MTLViewport){0, 0, (double)t->pw, (double)t->ph, 0, 1}];
    [t->enc setScissorRect:(MTLScissorRect){0, 0, (NSUInteger)t->pw, (NSUInteger)t->ph}];
}

void r_clear(RTarget *t, bool color, uint32_t argb, bool depth, float z, int x0, int y0, int x1, int y1) {
    if (!color && !depth) return;
    t->version++;
    begin_pass(t);
    full_viewport(t);
    float q[12] = {0};
    logical_to_ndc(t, (float)x0, (float)y0, (float)x1, (float)y1, q);
    q[8] = z;
    [t->enc setRenderPipelineState:pso(2, false, 0, 0, color)];
    [t->enc setDepthStencilState:dss(depth, depth, 8)];
    [t->enc setCullMode:MTLCullModeNone];
    [t->enc setVertexBytes:q length:sizeof q atIndex:0];
    float k[4] = {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, 1};
    [t->enc setFragmentBytes:k length:sizeof k atIndex:0];
    [t->enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

typedef struct { int color_op, color_a1, color_a2, alpha_op, alpha_a1, alpha_a2, alpha_func; float alpha_ref; int colorkey, has_tex, specular, xbr; float tf[4]; float tsize[2]; int smooth; float pad2; } U;
void r_draw(RTarget *t, const RState *s, RTex *tex, const RVertex *v, int nv, const uint16_t *idx, int ni,
            int vp_x, int vp_y, int vp_w, int vp_h) {
    bool lines = ni < 0;
    if (lines) ni = -ni;
    t->version++;
    begin_pass(t);
    float sx = (float)t->pw / t->lw, sy = (float)t->ph / t->lh;
    [t->enc setViewport:(MTLViewport){vp_x * sx, vp_y * sy, vp_w * sx, vp_h * sy, 0, 1}];
    [t->enc setScissorRect:(MTLScissorRect){0, 0, (NSUInteger)t->pw, (NSUInteger)t->ph}];
    [t->enc setRenderPipelineState:pso(0, s->blend, s->src_blend, s->dst_blend, true)];
    [t->enc setDepthStencilState:dss(s->ztest, s->zwrite, s->zfunc)];
    [t->enc setCullMode:MTLCullModeNone];
    id<MTLBuffer> vb, ib;
    uint32_t vo = ring_put(v, (uint32_t)(sizeof(RVertex) * (size_t)nv), &vb);
    uint32_t io = ring_put(idx, (uint32_t)(2 * ni), &ib);
    [t->enc setVertexBuffer:vb offset:vo atIndex:0];
    U u = {0};
    u.color_op = s->color_op ? s->color_op : 4;
    u.color_a1 = s->color_arg1;
    u.color_a2 = s->color_arg2;
    u.alpha_op = s->alpha_op ? s->alpha_op : 2;
    u.alpha_a1 = s->alpha_arg1;
    u.alpha_a2 = s->alpha_arg2;
    u.alpha_func = s->alpha_test ? s->alpha_func : 0;
    u.alpha_ref = s->alpha_ref;
    u.colorkey = s->colorkey;
    u.has_tex = tex != NULL;
    u.specular = s->specular;
    if (tex && tex->later && s->world && !tex_animated(tex)) {
        /* an animated texture that has held still for a second: looked up now, dumped below */
        tex->later = false;
        tex_lookup(tex, tex->src, tex->w);
        if (!hdtex_dumping() || hdtex_has(tex->key)) { free(tex->src); tex->src = NULL; }
    }
    if (tex && tex->hd_wait && s->world) {
        bool final;
        tex->hd = hd_ready(tex->key, &final);
        tex->hd_wait = !final;
    }
    bool hd = tex && tex->hd && s->world;
    /* MGS_HDSTATS=1: share of textured 3D draws that use an HD replacement, every 2 s */
    static int stats = -1;
    static uint64_t n_world, n_hd, t_stats;
    if (stats < 0) stats = getenv("MGS_HDSTATS") != NULL;
    if (stats && tex && s->world) {
        n_world++;
        n_hd += hd;
        if (plat_ticks_us() - t_stats > 2000000) {
            if (n_world) rt_log("HD coverage: %.1f%% of %llu textured 3D draws", 100.0 * (double)n_hd / (double)n_world, n_world);
            n_world = n_hd = 0;
            t_stats = plat_ticks_us();
        }
    }
    u.xbr = s->xbr && tex && tex->art && !hd;
    u.smooth = s->smooth && tex && s->filter && !hd;
    if (tex && s->world && tex->src && !tex->dumped && !tex->later && !tex_animated(tex)
        && plat_ticks_us() - tex->t_change >= 500000) {
        hdtex_dump(tex->key, tex->src, tex->w, tex->h);
        tex->dumped = true;
    }
    if (tex) { u.tsize[0] = (float)tex->w; u.tsize[1] = (float)tex->h; }
    u.tf[0] = ((s->tfactor >> 16) & 0xFF) / 255.0f;
    u.tf[1] = ((s->tfactor >> 8) & 0xFF) / 255.0f;
    u.tf[2] = (s->tfactor & 0xFF) / 255.0f;
    u.tf[3] = (s->tfactor >> 24) / 255.0f;
    [t->enc setFragmentBytes:&u length:sizeof u atIndex:0];
    [t->enc setFragmentTexture:hd ? tex->hd : tex ? tex->t : g_white atIndex:0];
    if (tex) tex->used = true;
    int clamp = (s->clamp_u ? 1 : 0) | (s->clamp_v ? 2 : 0);
    [t->enc setFragmentSamplerState:hd ? g_samp_hd[clamp] : g_samp[s->filter ? 1 : 0][clamp] atIndex:0];
    [t->enc drawIndexedPrimitives:lines ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle indexCount:(NSUInteger)ni
                        indexType:MTLIndexTypeUInt16 indexBuffer:ib indexBufferOffset:io];
}

void r_blit(RTarget *t, RTex *src, float sx0, float sy0, float sx1, float sy1, int dx0, int dy0, int dx1, int dy1,
            bool keyed, bool filter) {
    t->version++;
    src->used = true;
    quad_from(t, src->t, sx0 / src->w, sy0 / src->h, sx1 / src->w, sy1 / src->h, dx0, dy0, dx1, dy1, keyed, filter);
}
void r_copy_target(RTarget *dst, RTarget *src, float sx0, float sy0, float sx1, float sy1, int dx0, int dy0, int dx1,
                   int dy1) {
    if (!src->snap || src->snap_version != src->version || (int)src->snap.width != src->pw ||
        (int)src->snap.height != src->ph) {
        end_pass();
        need_cb();
        if (!src->snap || (int)src->snap.width != src->pw || (int)src->snap.height != src->ph) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)src->pw height:(NSUInteger)src->ph mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            src->snap = [g_dev newTextureWithDescriptor:d];
        }
        id<MTLBlitCommandEncoder> b = [g_cb blitCommandEncoder];
        [b copyFromTexture:src->resolve toTexture:src->snap];
        [b endEncoding];
        src->snap_version = src->version;
    }
    if (dst != src) dst->version++;
    quad_from(dst, src->snap, sx0 / src->lw, sy0 / src->lh, sx1 / src->lw, sy1 / src->lh, dx0, dy0, dx1, dy1, false, true);
}
static void quad_from(RTarget *t, id<MTLTexture> tex, float u0, float v0, float u1, float v1, int dx0, int dy0,
                      int dx1, int dy1, bool keyed, bool filter) {
    begin_pass(t);
    full_viewport(t);
    float q[12] = {0};
    logical_to_ndc(t, (float)dx0, (float)dy0, (float)dx1, (float)dy1, q);
    q[4] = u0;
    q[5] = v0;
    q[6] = u1;
    q[7] = v1;
    q[8] = 0;
    [t->enc setRenderPipelineState:pso(1, false, 0, 0, true)];
    [t->enc setDepthStencilState:dss(false, false, 8)];
    [t->enc setCullMode:MTLCullModeNone];
    [t->enc setVertexBytes:q length:sizeof q atIndex:0];
    float k[4] = {keyed ? 1.0f : 0.0f, 0, 0, 0};
    [t->enc setFragmentBytes:k length:sizeof k atIndex:0];
    [t->enc setFragmentTexture:tex atIndex:0];
    [t->enc setFragmentSamplerState:g_samp[filter ? 1 : 0][3] atIndex:0];
    [t->enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void r_overlay_draw(RTarget *t, const uint32_t *bgra, int pw, int ph) {
    t->version++;
    begin_pass(t);
    /* The upload is a CPU copy: a texture that a queued draw still has to read must not be
     * overwritten (the pause menu flushes GDI text several times before the GPU runs) */
    int pick = -1;
    while (pick < 0) {
        uint64_t done = atomic_load(&g_done_serial);
        for (int i = 0; i < 4 && pick < 0; i++)
            if (t->overlay[i] && (int)t->overlay[i].width == pw && (int)t->overlay[i].height == ph &&
                t->overlay_cb[i] <= done)
                pick = i;
        for (int i = 0; i < 4 && pick < 0; i++)
            if (!t->overlay[i] || (int)t->overlay[i].width != pw || (int)t->overlay[i].height != ph) {
                t->overlay[i] = new_tex(pw, ph);
                t->overlay_cb[i] = 0;
                pick = i;
            }
        if (pick < 0) {
            /* all four are queued: let the GPU catch up */
            id<MTLCommandBuffer> b = g_cb;
            r_flush();
            [b waitUntilCompleted];
            atomic_store(&g_done_serial, g_cb_serial);
            begin_pass(t);
        }
    }
    id<MTLTexture> ov = t->overlay[pick];
    t->overlay_cb[pick] = g_cb_serial;
    [ov replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)pw, (NSUInteger)ph) mipmapLevel:0 withBytes:bgra bytesPerRow:(NSUInteger)pw * 4];
    full_viewport(t);
    float q[12] = {-1, 1, 1, -1, 0, 0, 1, 1, 0, 0, 0, 0};
    [t->enc setRenderPipelineState:pso(3, true, 0, 0, true)];
    [t->enc setDepthStencilState:dss(false, false, 8)];
    [t->enc setVertexBytes:q length:sizeof q atIndex:0];
    [t->enc setFragmentTexture:ov atIndex:0];
    [t->enc setFragmentSamplerState:g_samp[0][3] atIndex:0];
    [t->enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

static id<MTLTexture> ident_lut(void) {
    static id<MTLTexture> lut;
    if (!lut) {
        id<MTLTexture> keep = g_gamma;
        uint16_t ident[768];
        for (int i = 0; i < 768; i++) ident[i] = (uint16_t)((i % 256) * 257);
        r_set_gamma(ident);
        lut = g_gamma;
        g_gamma = keep;
    }
    return lut;
}
void r_readback(RTarget *t, uint32_t *bgra, int lw, int lh) {
    /* downsample the resolved target to the game's resolution on the GPU, then copy out */
    end_pass();
    need_cb();
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)lw height:(NSUInteger)lh mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> small = [g_dev newTextureWithDescriptor:d];
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = small;
    p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:p];
    float q[12] = {-1, 1, 1, -1, 0, 0, 1, 1, 0, 0, 0, 0};
    [e setRenderPipelineState:pso(4, false, 0, 0, true)];
    [e setVertexBytes:q length:sizeof q atIndex:0];
    [e setFragmentTexture:t->resolve atIndex:0];
    [e setFragmentTexture:ident_lut() atIndex:1];
    [e setFragmentSamplerState:g_samp[1][3] atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    [g_cb commit];
    [g_cb waitUntilCompleted];
    g_cb = nil;
    [small getBytes:bgra bytesPerRow:(NSUInteger)lw * 4 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)lw, (NSUInteger)lh) mipmapLevel:0];
}

void r_set_gamma(const uint16_t ramp[768]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Unorm width:256 height:1 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    uint16_t px[256 * 4];
    for (int i = 0; i < 256; i++) {
        px[4 * i] = ramp[i];
        px[4 * i + 1] = ramp[256 + i];
        px[4 * i + 2] = ramp[512 + i];
        px[4 * i + 3] = 65535;
    }
    [t replaceRegion:MTLRegionMake2D(0, 0, 256, 1) mipmapLevel:0 withBytes:px bytesPerRow:256 * 8];
    g_gamma = t;
}

/* MGS_SHOT_DIR=dir [MGS_SHOT_EVERY=n]: save every n-th presented frame as PNG (full resolution) */
static bool shot_due(int frame) {
    const char *dir = getenv("MGS_SHOT_DIR");
    if (!dir) return false;
    /* MGS_SHOT_FROM=s: only from s seconds after start (same clock as MGS_KEYS) */
    if (getenv("MGS_SHOT_FROM") && plat_ticks_us() < g_plat_t0 + (uint64_t)(atof(getenv("MGS_SHOT_FROM")) * 1e6)) return false;
    int every = getenv("MGS_SHOT_EVERY") ? atoi(getenv("MGS_SHOT_EVERY")) : 30;
    return every > 0 && frame % every == 0;
}
static void save_tex(id<MTLTexture> src, int frame) {
    const char *dir = getenv("MGS_SHOT_DIR");
    int pw = (int)src.width, ph = (int)src.height;
    r_flush();
    id<MTLCommandBuffer> cb = [g_queue commandBuffer];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:(NSUInteger)pw height:(NSUInteger)ph mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> cpu = [g_dev newTextureWithDescriptor:d];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:src toTexture:cpu];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    size_t n = (size_t)pw * (size_t)ph * 4;
    uint8_t *px = malloc(n);
    [cpu getBytes:px bytesPerRow:(NSUInteger)pw * 4 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)pw, (NSUInteger)ph) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(px, (size_t)pw, (size_t)ph, 8, (size_t)pw * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(cg);
    char path[1200];
    snprintf(path, sizeof path, "%s/frame_%05d.png", dir, frame);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, kUTTypePNG, 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    CGImageDestinationFinalize(dst);
    CFRelease(dst); CFRelease(url); CGImageRelease(img); CGContextRelease(cg); CGColorSpaceRelease(cs);
    free(px);
}
static int g_frame;
static uint64_t g_fps_t0;

void r_present(RTarget *t) {
    if (g_frame == 0) plat_window_show();   /* whatever the game did, a presented frame must be visible */
    if (g_frame % (getenv("MGS_FPS_EVERY") ? atoi(getenv("MGS_FPS_EVERY")) : 120) == 0) {
        uint64_t now = plat_ticks_us();
        int ev = getenv("MGS_FPS_EVERY") ? atoi(getenv("MGS_FPS_EVERY")) : 120;
        if (g_fps_t0) rt_log("present: %.1f frames/s (t=%.1fs)", ev * 1e6 / (double)(now - g_fps_t0), now / 1e6);
        g_fps_t0 = now;
    }
    CAMetalLayer *layer = (__bridge CAMetalLayer *)plat_metal_layer();
    end_pass();
    ++g_frame;
    /* MGS_SHOT_PRESENTED=1 captures what the window shows instead of the game's frame */
    if (shot_due(g_frame) && !getenv("MGS_SHOT_PRESENTED")) save_tex(t->resolve, g_frame);
    if (!layer) { r_flush(); return; }
    int dw, dh;
    plat_drawable_size(&dw, &dh);
    if (dw <= 0 || dh <= 0) { r_flush(); return; }
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (!drawable) { r_flush(); return; }
    need_cb();
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = drawable.texture;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:p];
    /* aspect-correct fit */
    double ta = (double)t->lw / t->lh, da = (double)dw / dh;
    double w = da > ta ? dh * ta : dw, h = da > ta ? dh : dw / ta;
    [e setViewport:(MTLViewport){(dw - w) / 2, (dh - h) / 2, w, h, 0, 1}];
    /* The PC port draws the PlayStation's 448-line picture into 480 lines and leaves a black strip at
     * the bottom; show the 448 lines at 4:3 as the PlayStation did. MGS_FULL480=1 shows all 480. */
    static int crop = -1;
    if (crop < 0) crop = getenv("MGS_FULL480") ? 0 : 1;
    float q[12] = {-1, 1, 1, -1, 0, 0, 1, crop ? 448.0f / 480.0f : 1.0f, 0, 0, 0, 0};
    [e setRenderPipelineState:pso(4, false, 0, 0, true)];
    [e setVertexBytes:q length:sizeof q atIndex:0];
    [e setFragmentTexture:t->resolve atIndex:0];
    [e setFragmentTexture:g_gamma atIndex:1];
    [e setFragmentSamplerState:g_samp[1][3] atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    if (shot_due(g_frame) && getenv("MGS_SHOT_PRESENTED")) save_tex(drawable.texture, g_frame);
    need_cb();   /* save_tex committed the frame's buffer */
    [g_cb presentDrawable:drawable];
    [g_cb commit];
    g_cb = nil;
    g_ring_i = (g_ring_i + 1) % 3;
    g_ring_off = 0;
    /* the window may have been resized: follow it */
    int want = dh;
    if (want != t->ph && want >= t->lh) r_target_resize(t, t->lw, t->lh);
}
