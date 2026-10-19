/* HD texture replacements. Every world texture is known by a hash of its pixels (after the
 * game's own palette conversion, colour-keyed texels transparent). Each one without an HD version
 * is written once to "<game folder>/HD Textures/new" (MGS_TEXDUMP=dir elsewhere, MGS_TEXDUMP=0
 * not at all), so the areas a player visits can be upscaled later (tools/hdtex_upscale.sh), as
 * <key>.png: opaque, transparent texels filled from their neighbours, and a border of
 * PAD texels wrapped from the opposite edge, so an upscaler sees tiling textures whole. The
 * upscaled files (any integer factor, same name) go into "<game folder>/HD Textures" (or
 * MGS_HDTEX=dir; MGS_HDTEX=0 turns them off); loading crops the border. Textures with transparent
 * texels also get <key>.a.png, their mask (white = opaque), upscaled the same way so cut-out
 * outlines come out smooth; without it the original transparency is enlarged. */
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <stdlib.h>
#include "rt.h"
#include "hdtex.h"

#define PAD 4

static NSString *g_dump_dir, *g_hd_dir;
static dispatch_queue_t g_dump_q;
static NSMutableSet<NSNumber *> *g_dumped, *g_available, *g_masks;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void hdtex_init(void) {
    static bool done;
    if (done) return;
    done = true;
    g_dumped = [NSMutableSet new];
    g_available = [NSMutableSet new];
    g_masks = [NSMutableSet new];
    const char *d = getenv("MGS_TEXDUMP");
    if (!d || strcmp(d, "0")) {
        g_dump_dir = d && *d ? [NSString stringWithUTF8String:d]
                             : [[NSString stringWithUTF8String:g_game_dir] stringByAppendingPathComponent:@"HD Textures/new"];
        g_dump_q = dispatch_queue_create("texture dump", DISPATCH_QUEUE_SERIAL);
        /* what an earlier run dumped is not written again */
        for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:g_dump_dir error:nil]) {
            unsigned long long k;
            if ([[NSScanner scannerWithString:f] scanHexLongLong:&k] && [f hasSuffix:@".png"]) [g_dumped addObject:@(k)];
        }
        rt_log("texture dump: %s (%lu there already)", g_dump_dir.UTF8String, (unsigned long)g_dumped.count);
    }
    const char *h = getenv("MGS_HDTEX");
    if (h && !strcmp(h, "0")) return;
    g_hd_dir = h && *h ? [NSString stringWithUTF8String:h]
                       : [[NSString stringWithUTF8String:g_game_dir] stringByAppendingPathComponent:@"HD Textures"];
    for (NSString *f in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:g_hd_dir error:nil]) {
        unsigned long long k;
        if (f.length == 20 && [f hasSuffix:@".png"] && [[NSScanner scannerWithString:f] scanHexLongLong:&k])
            [g_available addObject:@(k)];
        if (f.length == 22 && [f hasSuffix:@".a.png"] && [[NSScanner scannerWithString:f] scanHexLongLong:&k])
            [g_masks addObject:@(k)];
    }
    if (g_available.count) rt_log("HD textures: %lu in %s", (unsigned long)g_available.count, g_hd_dir.UTF8String);
}

uint64_t hdtex_key(const uint32_t *bgra, int w, int h, int stride_px) {
    uint64_t k = 1469598103934665603ull;   /* FNV-1a */
    uint32_t dims[2] = {(uint32_t)w, (uint32_t)h};
    const uint8_t *p = (const uint8_t *)dims;
    for (int i = 0; i < 8; i++) k = (k ^ p[i]) * 1099511628211ull;
    for (int y = 0; y < h; y++) {
        p = (const uint8_t *)(bgra + (size_t)y * (size_t)stride_px);
        for (size_t i = 0; i < (size_t)w * 4; i++) k = (k ^ p[i]) * 1099511628211ull;
    }
    return k;
}

bool hdtex_dumping(void) { return g_dump_dir != nil; }
bool hdtex_has(uint64_t key) { return [g_available containsObject:@(key)]; }

static bool write_png(NSString *path, const uint32_t *bgra, int w, int h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate((void *)bgra, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                            kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(cg);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                                CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    bool ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    CGContextRelease(cg);
    CGColorSpaceRelease(cs);
    return ok;
}

static void dump_now(uint64_t key, const uint32_t *bgra, int w, int h);
void hdtex_dump(uint64_t key, const uint32_t *bgra, int w, int h) {
    if (!g_dump_dir || w < 4 || h < 4 || hdtex_has(key)) return;
    pthread_mutex_lock(&g_mu);
    bool seen = [g_dumped containsObject:@(key)];
    if (!seen) [g_dumped addObject:@(key)];
    pthread_mutex_unlock(&g_mu);
    if (seen) return;
    /* written on a background queue: a new area shows many new textures at once */
    uint32_t *copy = malloc((size_t)w * (size_t)h * 4);
    memcpy(copy, bgra, (size_t)w * (size_t)h * 4);
    dispatch_async(g_dump_q, ^{
        [[NSFileManager defaultManager] createDirectoryAtPath:g_dump_dir withIntermediateDirectories:YES attributes:nil error:nil];
        dump_now(key, copy, w, h);
        free(copy);
    });
}
static void dump_now(uint64_t key, const uint32_t *bgra, int w, int h) {
    /* fill transparent texels from opaque neighbours (a few rounds), so the upscaler does not
     * pull black into the edges of cut-out shapes */
    size_t n = (size_t)w * (size_t)h;
    uint32_t *c = malloc(n * 4);
    uint8_t *known = malloc(n);
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        c[i] = bgra[i] | 0xFF000000u;
        known[i] = (bgra[i] >> 24) != 0;
        any |= known[i];
    }
    for (int round = 0; any && round < 16; round++) {
        bool changed = false;
        uint8_t *next = malloc(n);
        memcpy(next, known, n);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                if (known[i]) continue;
                unsigned r = 0, g = 0, b = 0, m = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = (x + dx + w) % w, yy = (y + dy + h) % h;
                        size_t j = (size_t)yy * (size_t)w + (size_t)xx;
                        if (!known[j]) continue;
                        r += (c[j] >> 16) & 0xFF; g += (c[j] >> 8) & 0xFF; b += c[j] & 0xFF; m++;
                    }
                if (m) {
                    c[i] = 0xFF000000u | (r / m) << 16 | (g / m) << 8 | (b / m);
                    next[i] = 1;
                    changed = true;
                }
            }
        free(known);
        known = next;
        if (!changed) break;
    }
    int pw = w + 2 * PAD, ph = h + 2 * PAD;
    uint32_t *out = malloc((size_t)pw * (size_t)ph * 4);
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            int sx = ((x - PAD) % w + w) % w, sy = ((y - PAD) % h + h) % h;
            out[(size_t)y * (size_t)pw + (size_t)x] = c[(size_t)sy * (size_t)w + (size_t)sx];
        }
    NSString *path = [g_dump_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx.png", key]];
    write_png(path, out, pw, ph);
    bool cut = false;
    for (size_t i = 0; i < n && !cut; i++) cut = (bgra[i] >> 24) != 255;
    if (cut) {
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                int sx = ((x - PAD) % w + w) % w, sy = ((y - PAD) % h + h) % h;
                uint32_t a = bgra[(size_t)sy * (size_t)w + (size_t)sx] >> 24;
                out[(size_t)y * (size_t)pw + (size_t)x] = 0xFF000000u | a << 16 | a << 8 | a;
            }
        write_png([g_dump_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx.a.png", key]], out, pw, ph);
    }
    free(out);
    free(c);
    free(known);
}

/* an upscaled file as opaque BGRA; its size must be k times the padded original */
static uint32_t *decode(uint64_t key, NSString *suffix, int w, int h, int *kout, int *Wout, int *Hout) {
    NSString *path = [g_hd_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llx%@", key, suffix]];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], NULL);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (src) CFRelease(src);
    if (!img) return NULL;
    int W = (int)CGImageGetWidth(img), H = (int)CGImageGetHeight(img);
    int k = W / (w + 2 * PAD);
    if (k < 1 || W != k * (w + 2 * PAD) || H != k * (h + 2 * PAD)) {
        rt_log("HD texture %016llx%s: %dx%d is not a multiple of %dx%d, ignored", key, suffix.UTF8String, W, H,
               w + 2 * PAD, h + 2 * PAD);
        CGImageRelease(img);
        return NULL;
    }
    uint32_t *full = malloc((size_t)W * (size_t)H * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(full, (size_t)W, (size_t)H, 8, (size_t)W * 4, cs,
                                            kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGContextSetBlendMode(cg, kCGBlendModeCopy);
    CGContextDrawImage(cg, CGRectMake(0, 0, W, H), img);
    CGContextRelease(cg);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    *kout = k;
    *Wout = W;
    *Hout = H;
    return full;
}

uint32_t *hdtex_load(uint64_t key, const uint32_t *orig, int w, int h, int stride_px, int *hw, int *hh) {
    if (!hdtex_has(key)) return NULL;
    int k, W, H, mk = 0, MW = 0, MH = 0;
    uint32_t *full = decode(key, @".png", w, h, &k, &W, &H);
    if (!full) return NULL;
    uint32_t *mask = [g_masks containsObject:@(key)] ? decode(key, @".a.png", w, h, &mk, &MW, &MH) : NULL;
    if (mask && mk != k) { free(mask); mask = NULL; }
    /* crop the wrapped border; transparency comes from the original, enlarged (hard edges where
     * the original only had colour-key cut-outs), colours premultiplied like every texture */
    int ow = k * w, oh = k * h;
    bool alpha = false, binary = true;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t a = orig[(size_t)y * (size_t)stride_px + (size_t)x] >> 24;
            if (a != 255) alpha = true;
            if (a != 0 && a != 255) binary = false;
        }
    uint32_t *out = malloc((size_t)ow * (size_t)oh * 4);
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            uint32_t c = full[(size_t)(y + k * PAD) * (size_t)W + (size_t)(x + k * PAD)] | 0xFF000000u;
            if (alpha && mask) {
                uint32_t a = (mask[(size_t)(y + k * PAD) * (size_t)W + (size_t)(x + k * PAD)] >> 8) & 0xFF;
                if (binary) a = a >= 128 ? 255u : 0u;
                uint32_t r = ((c >> 16) & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, b = (c & 0xFF) * a / 255;
                c = a << 24 | r << 16 | g << 8 | b;
            } else if (alpha) {
                /* bilinear sample of the original alpha at this texel's centre (edges clamped) */
                float fx = ((float)x + 0.5f) / (float)k - 0.5f, fy = ((float)y + 0.5f) / (float)k - 0.5f;
                int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                float tx = fx - (float)x0, ty = fy - (float)y0;
                float s = 0;
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        int xx = x0 + i < 0 ? 0 : x0 + i >= w ? w - 1 : x0 + i;
                        int yy = y0 + j < 0 ? 0 : y0 + j >= h ? h - 1 : y0 + j;
                        float a = (float)(orig[(size_t)yy * (size_t)stride_px + (size_t)xx] >> 24);
                        s += a * (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                    }
                uint32_t a = binary ? (s >= 127.5f ? 255u : 0u) : (uint32_t)(s + 0.5f);
                uint32_t r = ((c >> 16) & 0xFF) * a / 255, g = ((c >> 8) & 0xFF) * a / 255, b = (c & 0xFF) * a / 255;
                c = a << 24 | r << 16 | g << 8 | b;
            }
            out[(size_t)y * (size_t)ow + (size_t)x] = c;
        }
    free(full);
    free(mask);
    *hw = ow;
    *hh = oh;
    return out;
}
