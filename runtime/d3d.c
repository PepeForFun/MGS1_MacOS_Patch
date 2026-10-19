/* IDirect3DDevice7: tracks render/texture-stage state and turns DrawPrimitive calls into
 * clip-space triangles for the Metal renderer. */
#include <stdlib.h>
#include "ddraw.h"

typedef struct {
    Surf *target;
    uint32_t rs[256];
    uint32_t tss[8][32];
    Surf *tex[8];
    float world[16], view[16], proj[16];
    int vp_x, vp_y, vp_w, vp_h;
    float vp_minz, vp_maxz;
    bool in_scene;
    uint32_t draws;
} Dev;

static void ident(float *m) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1;
}
static Dev *self(CPU *c) { return com_host(ARG(0)); }

/* ------------------------------------------------------------------ state */
static void build_state(Dev *d, RState *s) {
    uint32_t *rs = d->rs;
    memset(s, 0, sizeof *s);
    s->blend = rs[27] != 0;
    s->src_blend = (int)rs[19];
    s->dst_blend = (int)rs[20];
    s->alpha_test = rs[15] != 0;
    s->alpha_func = (int)rs[25];
    s->alpha_ref = (float)(rs[24] & 0xFF) / 255.0f;
    s->ztest = rs[7] != 0 && d->target && d->target->zbuf;
    s->zwrite = rs[14] != 0 && s->ztest;
    s->zfunc = (int)rs[23];
    s->cull = (int)rs[22];
    s->fog = rs[28] != 0;
    s->fog_color = rs[34];
    s->colorkey = rs[41] != 0;
    s->specular = rs[29] != 0;
    s->tfactor = rs[60];
    s->color_op = (int)d->tss[0][1];
    s->color_arg1 = (int)d->tss[0][2];
    s->color_arg2 = (int)d->tss[0][3];
    s->alpha_op = (int)d->tss[0][4];
    s->alpha_arg1 = (int)d->tss[0][5];
    s->alpha_arg2 = (int)d->tss[0][6];
    /* Smooth by default, even where the game asks for point sampling (menus, fonts):
     * MGS_FILTER=point restores the game's own choice. */
    static int mode = -1;
    if (mode < 0) mode = getenv("MGS_FILTER") && !strcmp(getenv("MGS_FILTER"), "point") ? 0 : 1;
    s->filter = mode ? true : d->tss[0][16] != 1;
    s->clamp_u = d->tss[0][13] == 3;
    s->clamp_v = d->tss[0][14] == 3;
}

/* ------------------------------------------------------------------ vertices */
static void mat_mul(const float *a, const float *b, float *o) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(o, r, 64);
}
static int fvf_stride(uint32_t fvf) {
    int n = 0;
    switch (fvf & 0xE) {
    case 0x2: n = 12; break;
    case 0x4: n = 16; break;
    case 0x6: n = 16; break;
    case 0x8: n = 20; break;
    case 0xA: n = 24; break;
    case 0xC: n = 28; break;
    case 0xE: n = 32; break;
    }
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    n += 8 * (int)((fvf >> 8) & 0xF);
    return n;
}
static float rf(uint32_t a) { return RF32(a); }
static RVertex *convert(Dev *d, uint32_t fvf, uint32_t verts, int n) {
    RVertex *out = malloc(sizeof(RVertex) * (size_t)(n > 0 ? n : 1));
    int stride = fvf_stride(fvf);
    bool rhw = (fvf & 0xE) == 0x4;
    float wvp[16];
    if (!rhw) {
        float wv[16];
        mat_mul(d->world, d->view, wv);
        mat_mul(wv, d->proj, wvp);
    }
    float tw = (float)d->target->w, th = (float)d->target->h;
    for (int i = 0; i < n; i++) {
        uint32_t p = verts + (uint32_t)(i * stride);
        uint32_t o = 0;
        float x = rf(p), y = rf(p + 4), z = rf(p + 8);
        RVertex *v = &out[i];
        if (rhw) {
            float r = rf(p + 12);
            float w = r != 0 ? 1.0f / r : 1.0f;
            float nx = (x + 0.5f) / tw * 2 - 1, ny = 1 - (y + 0.5f) / th * 2;
            v->x = nx * w;
            v->y = ny * w;
            v->z = z * w;
            v->w = w;
            o = 16;
        } else {
            v->x = x * wvp[0] + y * wvp[4] + z * wvp[8] + wvp[12];
            v->y = x * wvp[1] + y * wvp[5] + z * wvp[9] + wvp[13];
            v->z = x * wvp[2] + y * wvp[6] + z * wvp[10] + wvp[14];
            v->w = x * wvp[3] + y * wvp[7] + z * wvp[11] + wvp[15];
            o = (fvf & 0xE) == 0x2 ? 12 : (uint32_t)(fvf_stride(fvf & 0xE));
        }
        if (fvf & 0x10) o += 12;
        if (fvf & 0x20) o += 4;
        v->diffuse = (fvf & 0x40) ? R32(p + o) : 0xFFFFFFFFu;
        if (fvf & 0x40) o += 4;
        v->specular = (fvf & 0x80) ? R32(p + o) : 0;
        if (fvf & 0x80) o += 4;
        if ((fvf >> 8) & 0xF) { v->u = rf(p + o); v->v = rf(p + o + 4); }
        else { v->u = v->v = 0; }
    }
    return out;
}
/* list of triangle (or line) indices for a D3D primitive type */
static uint16_t *make_indices(int type, const uint16_t *src, int nsrc, int *nout, int *kind) {
    int tri = 0;
    *kind = 0;
    if (type == 4) tri = nsrc / 3;
    else if (type == 5 || type == 6) tri = nsrc > 2 ? nsrc - 2 : 0;
    else if (type == 2 || type == 3) {
        *kind = 1;
        int segs = type == 2 ? nsrc / 2 : (nsrc > 1 ? nsrc - 1 : 0);
        uint16_t *o = malloc(sizeof(uint16_t) * (size_t)(segs * 2 + 1));
        for (int i = 0; i < segs; i++) {
            int a = type == 2 ? 2 * i : i, b = a + 1;
            o[2 * i] = src ? src[a] : (uint16_t)a;
            o[2 * i + 1] = src ? src[b] : (uint16_t)b;
        }
        *nout = segs * 2;
        return o;
    } else {
        *nout = 0;
        return NULL;
    }
    uint16_t *o = malloc(sizeof(uint16_t) * (size_t)(tri * 3 + 1));
    for (int i = 0; i < tri; i++) {
        int a, b, cc;
        if (type == 4) { a = 3 * i; b = a + 1; cc = a + 2; }
        else if (type == 5) { a = i; b = i + 1; cc = i + 2; if (i & 1) { int t = a; a = b; b = t; } }
        else { a = 0; b = i + 1; cc = i + 2; }
        o[3 * i] = src ? src[a] : (uint16_t)a;
        o[3 * i + 1] = src ? src[b] : (uint16_t)b;
        o[3 * i + 2] = src ? src[cc] : (uint16_t)cc;
    }
    *nout = tri * 3;
    return o;
}
static void draw(Dev *d, int type, uint32_t fvf, uint32_t verts, int nv, const uint16_t *idx, int ni) {
    if (!d->target || !d->target->rt || nv <= 0) return;
    surf_flush_overlay(d->target);
    RState st;
    build_state(d, &st);
    RTex *tex = NULL;
    if (d->tex[0] && st.color_op != 1) {
        surf_sync_texture(d->tex[0]);
        tex = d->tex[0]->tex;
    }
    RVertex *v = convert(d, fvf, verts, nv);
    /* The port transforms everything itself, so the 3D world arrives as pre-transformed quads
     * too; only flat 2D art (fonts, HUD, menus) has 1/w = 1 at every vertex */
    bool flat = (fvf & 0xE) == 0x4;
    for (int i = 0; i < nv && flat; i++) flat = v[i].w == 1.0f;
    st.world = !flat;
    /* 2D quads that stay inside their texture: clamp, so upscaled bilinear filtering cannot pull
     * texels from the opposite edge (visible seams between tiled sprites) */
    if ((fvf & 0xE) == 0x4 && tex) {
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < nv; i++) {
            lo = fminf(lo, fminf(v[i].u, v[i].v));
            hi = fmaxf(hi, fmaxf(v[i].u, v[i].v));
        }
        if (lo >= -0.01f && hi <= 1.01f) st.clamp_u = st.clamp_v = true;
        /* MGS_WORLD=bicubic or bilinear filters the world's textures that way instead of xBR
         * (sharp but blotchy on their dithering); 2D art keeps xBR */
        static int xbr = -1, world = -1;
        if (xbr < 0) xbr = !(getenv("MGS_XBR") && !strcmp(getenv("MGS_XBR"), "0"));
        if (world < 0) {
            const char *e = getenv("MGS_WORLD");
            world = e && !strcmp(e, "bicubic") ? 1 : e && !strcmp(e, "bilinear") ? 0 : 2;
        }
        st.xbr = xbr && (flat || world == 2);
        st.smooth = !flat && world == 1;
    }
    static int stats;
    if (stats < 1500 && rt_after("MGS_DRAWSTATS")) {
        /* one frame's draws: which ones are 2D sprites and which are the 3D world */
        stats++;
        float w0 = 1e9f, w1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
        for (int i = 0; i < nv; i++) {
            w0 = fminf(w0, v[i].w); w1 = fmaxf(w1, v[i].w);
            z0 = fminf(z0, v[i].z / v[i].w); z1 = fmaxf(z1, v[i].z / v[i].w);
        }
        rt_log("draw %s t%d nv %d zt %d zw %d bl %d/%d at %d ck %d tex %dx%d%s w %.3f..%.3f z %.4f..%.4f",
               (fvf & 0xE) == 0x4 ? "rhw" : "xyz", type, nv, st.ztest, st.zwrite, st.blend ? st.src_blend : 0,
               st.blend ? st.dst_blend : 0, st.alpha_test, st.colorkey, d->tex[0] ? d->tex[0]->w : 0,
               d->tex[0] ? d->tex[0]->h : 0, tex && st.xbr ? " xbr?" : "", w0, w1, z0, z1);
    }
    int n, kind;
    uint16_t *list = make_indices(type, idx, idx ? ni : nv, &n, &kind);
    if (list && n > 0) {
        bool rhw = (fvf & 0xE) == 0x4;
        if (rhw) r_draw(d->target->rt, &st, tex, v, nv, list, kind ? -n : n, 0, 0, d->target->w, d->target->h);
        else r_draw(d->target->rt, &st, tex, v, nv, list, kind ? -n : n, d->vp_x, d->vp_y, d->vp_w, d->vp_h);
    }
    free(list);
    free(v);
    d->draws++;
}

/* ------------------------------------------------------------------ methods */
static void m_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, S_OK); }
static void m_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void m_Release(CPU *c) { RET(1, com_release(ARG(0))); }
void d3d_device_desc(uint32_t p, const char *guid);
static void m_GetCaps(CPU *c) { d3d_device_desc(ARG(1), "84E63DE0-46AA-11CF-816F-0000C020156E"); RET(2, S_OK); }
static void m_EnumTextureFormats(CPU *c) { /* (this, cb, ctx) */
    static const uint32_t fmts[][6] = {
        /* flags, bits, r, g, b, a */
        {0x41, 16, 0x7C00, 0x03E0, 0x001F, 0x8000},       /* A1R5G5B5 */
        {0x40, 16, 0xF800, 0x07E0, 0x001F, 0},            /* R5G6B5 */
        {0x41, 16, 0x0F00, 0x00F0, 0x000F, 0xF000},       /* A4R4G4B4 */
        {0x40, 16, 0x7C00, 0x03E0, 0x001F, 0},            /* X1R5G5B5 */
        {0x41, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000u},  /* A8R8G8B8 */
        {0x40, 32, 0xFF0000, 0xFF00, 0xFF, 0},            /* X8R8G8B8 */
    };
    uint32_t pf = heap_alloc(32, true);
    for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
        memset(GPTR(pf), 0, 32);
        W32(pf, 32);
        W32(pf + 4, fmts[i][0]);
        W32(pf + 12, fmts[i][1]);
        W32(pf + 16, fmts[i][2]);
        W32(pf + 20, fmts[i][3]);
        W32(pf + 24, fmts[i][4]);
        W32(pf + 28, fmts[i][5]);
        if (!call_guest_ret(c, ARG(1), 2, pf, ARG(2))) break;
    }
    heap_free(pf);
    RET(3, S_OK);
}
static void m_BeginScene(CPU *c) { self(c)->in_scene = true; RET(1, S_OK); }
static void m_EndScene(CPU *c) { self(c)->in_scene = false; RET(1, S_OK); }
extern uint32_t ddraw_d3d_obj(void);
static void m_GetDirect3D(CPU *c) { com_addref(ddraw_d3d_obj()); W32(ARG(1), ddraw_d3d_obj()); RET(2, S_OK); }
/* the device holds its render target and the textures set on it, as Direct3D does: the game may
 * release a texture that is still set */
static void m_SetRenderTarget(CPU *c) {
    Dev *d = self(c);
    Surf *s = surf_of(ARG(1));
    if (s) {
        if (!s->rt) s->rt = r_target_create(s->w, s->h);
        com_addref(s->obj);
        if (d->target) com_release(d->target->obj);
        d->target = s;
    }
    RET(3, S_OK);
}
static void m_GetRenderTarget(CPU *c) {
    Dev *d = self(c);
    com_addref(d->target->obj);
    W32(ARG(1), d->target->obj);
    RET(2, S_OK);
}
static void m_Clear(CPU *c) { /* (this, count, rects, flags, color, z, stencil) */
    Dev *d = self(c);
    uint32_t n = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4);
    float z = RF32(ESP + 4 + 20);
    if (!d->target || !d->target->rt) { RET(7, S_OK); return; }
    surf_flush_overlay(d->target);
    if (!n || !rects) {
        r_clear(d->target->rt, flags & 1, color, (flags & 2) != 0, z, d->vp_x, d->vp_y, d->vp_x + d->vp_w,
                d->vp_y + d->vp_h);
    } else {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r = rects + 16 * i;
            r_clear(d->target->rt, flags & 1, color, (flags & 2) != 0, z, (int32_t)R32(r), (int32_t)R32(r + 4),
                    (int32_t)R32(r + 8), (int32_t)R32(r + 12));
        }
    }
    RET(7, S_OK);
}
static void m_SetTransform(CPU *c) {
    Dev *d = self(c);
    uint32_t t = ARG(1), m = ARG(2);
    float *dst = t == 1 || t == 256 ? d->world : t == 2 ? d->view : t == 3 ? d->proj : NULL;
    if (dst)
        for (int i = 0; i < 16; i++) dst[i] = RF32(m + 4u * (uint32_t)i);
    RET(3, S_OK);
}
static void m_GetTransform(CPU *c) {
    Dev *d = self(c);
    uint32_t t = ARG(1), m = ARG(2);
    float *src = t == 1 || t == 256 ? d->world : t == 2 ? d->view : t == 3 ? d->proj : d->world;
    for (int i = 0; i < 16; i++) WF32(m + 4u * (uint32_t)i, src[i]);
    RET(3, S_OK);
}
static void m_SetViewport(CPU *c) {
    Dev *d = self(c);
    uint32_t p = ARG(1);
    d->vp_x = (int)R32(p);
    d->vp_y = (int)R32(p + 4);
    d->vp_w = (int)R32(p + 8);
    d->vp_h = (int)R32(p + 12);
    d->vp_minz = RF32(p + 16);
    d->vp_maxz = RF32(p + 20);
    RET(2, S_OK);
}
static void m_GetViewport(CPU *c) {
    Dev *d = self(c);
    uint32_t p = ARG(1);
    W32(p, (uint32_t)d->vp_x);
    W32(p + 4, (uint32_t)d->vp_y);
    W32(p + 8, (uint32_t)d->vp_w);
    W32(p + 12, (uint32_t)d->vp_h);
    WF32(p + 16, d->vp_minz);
    WF32(p + 20, d->vp_maxz);
    RET(2, S_OK);
}
static void m_Nop2(CPU *c) { RET(2, S_OK); }
static void m_Nop3(CPU *c) { RET(3, S_OK); }
static void m_SetRenderState(CPU *c) {
    Dev *d = self(c);
    uint32_t s = ARG(1);
    if (s < 256) d->rs[s] = ARG(2);
    RET(3, S_OK);
}
static void m_GetRenderState(CPU *c) {
    Dev *d = self(c);
    uint32_t s = ARG(1);
    W32(ARG(2), s < 256 ? d->rs[s] : 0);
    RET(3, S_OK);
}
static void m_DrawPrimitive(CPU *c) { /* (this, type, fvf, verts, count, flags) */
    draw(self(c), (int)ARG(1), ARG(2), ARG(3), (int)ARG(4), NULL, 0);
    RET(6, S_OK);
}
static void m_DrawIndexedPrimitive(CPU *c) { /* (this, type, fvf, verts, nverts, idx, nidx, flags) */
    int ni = (int)ARG(6);
    uint16_t *idx = malloc(sizeof(uint16_t) * (size_t)(ni > 0 ? ni : 1));
    for (int i = 0; i < ni; i++) idx[i] = R16(ARG(5) + 2u * (uint32_t)i);
    draw(self(c), (int)ARG(1), ARG(2), ARG(3), (int)ARG(4), idx, ni);
    free(idx);
    RET(8, S_OK);
}
static void m_SetTexture(CPU *c) {
    Dev *d = self(c);
    uint32_t st = ARG(1);
    if (st < 8) {
        Surf *s = surf_of(ARG(2)), *old = d->tex[st];
        if (s != old) {
            if (s) com_addref(s->obj);
            d->tex[st] = s;
            if (old) com_release(old->obj);
        }
    }
    RET(3, S_OK);
}
static void m_GetTexture(CPU *c) {
    Dev *d = self(c);
    uint32_t st = ARG(1);
    Surf *s = st < 8 ? d->tex[st] : NULL;
    if (s) com_addref(s->obj);
    W32(ARG(2), s ? s->obj : 0);
    RET(3, S_OK);
}
static void m_SetTextureStageState(CPU *c) {
    Dev *d = self(c);
    uint32_t st = ARG(1), t = ARG(2);
    if (st < 8 && t < 32) d->tss[st][t] = ARG(3);
    RET(4, S_OK);
}
static void m_GetTextureStageState(CPU *c) {
    Dev *d = self(c);
    uint32_t st = ARG(1), t = ARG(2);
    W32(ARG(3), st < 8 && t < 32 ? d->tss[st][t] : 0);
    RET(4, S_OK);
}
static void m_ValidateDevice(CPU *c) { W32(ARG(1), 1); RET(2, S_OK); }
static void m_LightEnable(CPU *c) { RET(3, S_OK); }
static void m_PreLoad(CPU *c) { RET(2, S_OK); }
static void m_GetInfo(CPU *c) { RET(4, 1); }

static const ComMethod g_dev_methods[] = {
    {"QueryInterface", m_QueryInterface}, {"AddRef", m_AddRef}, {"Release", m_Release}, {"GetCaps", m_GetCaps},
    {"EnumTextureFormats", m_EnumTextureFormats}, {"BeginScene", m_BeginScene}, {"EndScene", m_EndScene},
    {"GetDirect3D", m_GetDirect3D}, {"SetRenderTarget", m_SetRenderTarget}, {"GetRenderTarget", m_GetRenderTarget},
    {"Clear", m_Clear}, {"SetTransform", m_SetTransform}, {"GetTransform", m_GetTransform},
    {"SetViewport", m_SetViewport}, {"MultiplyTransform", NULL}, {"GetViewport", m_GetViewport},
    {"SetMaterial", m_Nop2}, {"GetMaterial", NULL}, {"SetLight", m_Nop3}, {"GetLight", NULL},
    {"SetRenderState", m_SetRenderState}, {"GetRenderState", m_GetRenderState}, {"BeginStateBlock", NULL},
    {"EndStateBlock", NULL}, {"PreLoad", m_PreLoad}, {"DrawPrimitive", m_DrawPrimitive},
    {"DrawIndexedPrimitive", m_DrawIndexedPrimitive}, {"SetClipStatus", m_Nop2}, {"GetClipStatus", NULL},
    {"DrawPrimitiveStrided", NULL}, {"DrawIndexedPrimitiveStrided", NULL}, {"DrawPrimitiveVB", NULL},
    {"DrawIndexedPrimitiveVB", NULL}, {"ComputeSphereVisibility", NULL}, {"GetTexture", m_GetTexture},
    {"SetTexture", m_SetTexture}, {"GetTextureStageState", m_GetTextureStageState},
    {"SetTextureStageState", m_SetTextureStageState}, {"ValidateDevice", m_ValidateDevice},
    {"ApplyStateBlock", NULL}, {"CaptureStateBlock", NULL}, {"DeleteStateBlock", NULL}, {"CreateStateBlock", NULL},
    {"Load", NULL}, {"LightEnable", m_LightEnable}, {"GetLightEnable", NULL}, {"SetClipPlane", NULL},
    {"GetClipPlane", NULL}, {"GetInfo", m_GetInfo},
};
static ComIface g_dev_iface = {"IDirect3DDevice7", 49, g_dev_methods, 0};

uint32_t d3d_create_device(Surf *target) {
    Dev *d = calloc(1, sizeof *d);
    com_addref(target->obj);
    d->target = target;
    ident(d->world);
    ident(d->view);
    ident(d->proj);
    d->vp_w = target->w;
    d->vp_h = target->h;
    d->vp_maxz = 1;
    /* D3D7 defaults */
    d->rs[7] = 1;   /* ZENABLE */
    d->rs[14] = 1;  /* ZWRITEENABLE */
    d->rs[19] = 2;  /* SRCBLEND ONE */
    d->rs[20] = 1;  /* DESTBLEND ZERO */
    d->rs[22] = 3;  /* CULL CCW */
    d->rs[23] = 4;  /* ZFUNC LESSEQUAL */
    d->rs[25] = 8;  /* ALPHAFUNC ALWAYS */
    d->rs[137] = 1; /* LIGHTING */
    d->tss[0][1] = 4;  /* COLOROP MODULATE */
    d->tss[0][2] = 2;  /* COLORARG1 TEXTURE */
    d->tss[0][3] = 1;  /* COLORARG2 CURRENT(=diffuse) */
    d->tss[0][4] = 2;  /* ALPHAOP SELECTARG1 */
    d->tss[0][5] = 2;
    d->tss[0][6] = 1;
    d->tss[0][16] = 2; /* MAGFILTER LINEAR */
    d->tss[0][17] = 2;
    for (int i = 1; i < 8; i++) { d->tss[i][1] = 1; d->tss[i][4] = 1; }
    return com_new(&g_dev_iface, d, 0);
}
