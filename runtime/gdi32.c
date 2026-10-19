/* GDI32 on CoreGraphics/CoreText. Device contexts draw into 32-bit BGRA pixel buffers; a DC
 * on a high-resolution target scales the game's coordinates, so text and shapes come out
 * crisp at the window's real resolution. */
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <stdlib.h>
#include <strings.h>
#include "gdi.h"

enum { O_FREE, O_DC, O_BITMAP, O_FONT, O_PEN, O_BRUSH };
struct Dc;
typedef struct {
    int type;
    bool stock;
    GdiTarget bm;      /* bitmap */
    double font_px;    /* font: em size in logical pixels */
    int weight, italic;
    char face[64];
    uint32_t color;    /* pen / brush COLORREF 0x00BBGGRR */
    int style, width;
    struct Dc *dc;
} GObj;

typedef struct Dc {
    GdiTarget *t;
    CGContextRef cg;
    uint32_t bitmap, font, pen, brush;
    uint32_t text_color, bk_color, text_align;
    int bk_mode, stretch_mode;
    void (*release)(void *ctx);
    void *release_ctx;
} Dc;

#define MAXOBJ 8192
static GObj g_obj[MAXOBJ];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
#define HBASE 0x01000000u
static uint32_t h_of(int i) { return HBASE + (uint32_t)i * 4; }
static GObj *obj(uint32_t h) {
    if (h < HBASE) return NULL;
    uint32_t i = (h - HBASE) / 4;
    if (i >= MAXOBJ || g_obj[i].type == O_FREE) return NULL;
    return &g_obj[i];
}
static uint32_t new_obj(int type) {
    pthread_mutex_lock(&g_mu);
    for (int i = 32; i < MAXOBJ; i++)
        if (g_obj[i].type == O_FREE) {
            memset(&g_obj[i], 0, sizeof g_obj[i]);
            g_obj[i].type = type;
            pthread_mutex_unlock(&g_mu);
            return h_of(i);
        }
    pthread_mutex_unlock(&g_mu);
    rt_fatal("out of GDI objects");
}

/* ------------------------------------------------------------------ targets */
static CGColorSpaceRef rgb(void) {
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    return cs;
}
static CGContextRef make_cg(GdiTarget *t) {
    CGContextRef cg = CGBitmapContextCreate(t->pixels, (size_t)t->pw, (size_t)t->ph, 8, (size_t)t->stride, rgb(),
                                            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (!cg) return NULL;
    /* GDI's y axis points down; work in the target's logical units */
    CGContextTranslateCTM(cg, 0, t->ph);
    CGContextScaleCTM(cg, t->scale, -t->scale);
    CGContextSetShouldAntialias(cg, true);
    CGContextSetShouldSmoothFonts(cg, false);
    CGContextSetInterpolationQuality(cg, kCGInterpolationHigh);
    return cg;
}
static void cg_color(CGContextRef cg, uint32_t c, bool fill) {
    CGFloat r = (c & 0xFF) / 255.0, g = ((c >> 8) & 0xFF) / 255.0, b = ((c >> 16) & 0xFF) / 255.0;
    if (fill) CGContextSetRGBFillColor(cg, r, g, b, 1);
    else CGContextSetRGBStrokeColor(cg, r, g, b, 1);
}

/* ------------------------------------------------------------------ stock objects */
static void init_stock(void) {
    static bool done;
    if (done) return;
    done = true;
    static const uint32_t brush[] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
    for (int i = 0; i < 5; i++) {
        g_obj[i].type = O_BRUSH;
        g_obj[i].stock = true;
        g_obj[i].color = brush[i];
    }
    g_obj[5].type = O_BRUSH; g_obj[5].stock = true; g_obj[5].style = 1;   /* NULL_BRUSH */
    g_obj[6].type = O_PEN; g_obj[6].stock = true; g_obj[6].color = 0xFFFFFF; g_obj[6].width = 1;
    g_obj[7].type = O_PEN; g_obj[7].stock = true; g_obj[7].color = 0; g_obj[7].width = 1;
    g_obj[8].type = O_PEN; g_obj[8].stock = true; g_obj[8].style = 5;     /* NULL_PEN */
    for (int i = 10; i <= 17; i++) {
        g_obj[i].type = O_FONT;
        g_obj[i].stock = true;
        g_obj[i].font_px = 13;
        snprintf(g_obj[i].face, sizeof g_obj[i].face, "%s", i == 10 || i == 11 ? "Courier New" : "Arial");
    }
}
void I_gdi32_GetStockObject(CPU *c) {
    init_stock();
    uint32_t i = ARG(0);
    RET(1, i < 18 && g_obj[i].type ? h_of((int)i) : 0);
}

/* ------------------------------------------------------------------ DCs */
static uint32_t new_dc(GdiTarget *t) {
    init_stock();
    uint32_t h = new_obj(O_DC);
    Dc *d = calloc(1, sizeof *d);
    obj(h)->dc = d;
    d->t = t;
    d->font = h_of(13);
    d->pen = h_of(7);
    d->brush = h_of(0);
    d->bk_color = 0xFFFFFF;
    d->bk_mode = 2; /* OPAQUE */
    if (t) d->cg = make_cg(t);
    return h;
}
static Dc *dc_of(uint32_t h) {
    GObj *o = obj(h);
    return o && o->type == O_DC ? o->dc : NULL;
}
uint32_t gdi_dc_for_target(GdiTarget *t, void (*release)(void *), void *ctx) {
    uint32_t h = new_dc(t);
    Dc *d = dc_of(h);
    d->release = release;
    d->release_ctx = ctx;
    return h;
}
void gdi_release_dc(uint32_t h) {
    Dc *d = dc_of(h);
    if (!d) return;
    if (d->cg) {
        CGContextFlush(d->cg);
        CGContextRelease(d->cg);
    }
    if (d->release) d->release(d->release_ctx);
    free(d);
    obj(h)->type = O_FREE;
}

void I_gdi32_CreateCompatibleDC(CPU *c) { RET(1, new_dc(NULL)); }
void I_gdi32_DeleteDC(CPU *c) { gdi_release_dc(ARG(0)); RET(1, 1); }

void I_gdi32_SelectObject(CPU *c) {
    Dc *d = dc_of(ARG(0));
    GObj *o = obj(ARG(1));
    if (!d || !o) { RET(2, 0); return; }
    uint32_t prev = 0;
    switch (o->type) {
    case O_BITMAP:
        prev = d->bitmap;
        d->bitmap = ARG(1);
        d->t = &o->bm;
        if (d->cg) CGContextRelease(d->cg);
        d->cg = make_cg(d->t);
        break;
    case O_FONT: prev = d->font; d->font = ARG(1); break;
    case O_PEN: prev = d->pen; d->pen = ARG(1); break;
    case O_BRUSH: prev = d->brush; d->brush = ARG(1); break;
    default: break;
    }
    RET(2, prev ? prev : h_of(0));
}

void I_gdi32_DeleteObject(CPU *c) {
    GObj *o = obj(ARG(0));
    if (o && !o->stock && o->type != O_DC) {
        if (o->type == O_BITMAP && o->bm.own) free(o->bm.pixels);
        o->type = O_FREE;
    }
    RET(1, 1);
}

/* ------------------------------------------------------------------ bitmaps */
static uint32_t new_bitmap(int w, int h) {
    uint32_t hb = new_obj(O_BITMAP);
    GdiTarget *t = &obj(hb)->bm;
    t->w = t->pw = w;
    t->h = t->ph = h;
    t->scale = 1;
    t->stride = w * 4;
    t->pixels = calloc((size_t)w * (size_t)h, 4);
    t->own = true;
    for (int i = 0; i < w * h; i++) t->pixels[i] = 0xFF000000u;
    return hb;
}
void I_gdi32_CreateCompatibleBitmap(CPU *c) {
    int w = (int32_t)ARG(1), h = (int32_t)ARG(2);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { RET(3, 0); return; }
    RET(3, new_bitmap(w, h));
}

/* BMP files and RT_BITMAP resources: BITMAPINFOHEADER + palette + bits */
static uint32_t bitmap_from_dib(const uint8_t *bi, const uint8_t *bits) {
    int32_t w = *(const int32_t *)(bi + 4), h = *(const int32_t *)(bi + 8);
    uint16_t bpp = *(const uint16_t *)(bi + 14);
    uint32_t comp = *(const uint32_t *)(bi + 16), hsize = *(const uint32_t *)bi;
    uint32_t ncol = *(const uint32_t *)(bi + 32);
    if (comp != 0 || w <= 0 || w > 8192 || h == 0) return 0;
    bool topdown = h < 0;
    if (topdown) h = -h;
    if (!ncol && bpp <= 8) ncol = 1u << bpp;
    const uint8_t *pal = bi + hsize;
    if (!bits) bits = pal + 4 * ncol;
    uint32_t hb = new_bitmap(w, h);
    GdiTarget *t = &obj(hb)->bm;
    int stride = ((w * bpp + 31) / 32) * 4;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = bits + (size_t)(topdown ? y : h - 1 - y) * (size_t)stride;
        uint32_t *out = t->pixels + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t b, g, r;
            if (bpp == 24) { b = row[3 * x]; g = row[3 * x + 1]; r = row[3 * x + 2]; }
            else if (bpp == 32) { b = row[4 * x]; g = row[4 * x + 1]; r = row[4 * x + 2]; }
            else {
                int idx = bpp == 8 ? row[x] : bpp == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15
                                                        : (row[x / 8] >> (7 - x % 8)) & 1;
                b = pal[4 * idx]; g = pal[4 * idx + 1]; r = pal[4 * idx + 2];
            }
            out[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return hb;
}
uint32_t gdi_load_bitmap_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    uint32_t h = 0;
    if (got > 54 && b[0] == 'B' && b[1] == 'M') h = bitmap_from_dib(b + 14, b + *(uint32_t *)(b + 10));
    free(b);
    return h;
}
uint32_t gdi_load_bitmap_resource(uint32_t id) {
    uint32_t a, sz;
    if (!rsrc_find(2, id, &a, &sz)) return 0;
    return bitmap_from_dib(M + a, NULL);
}

void I_gdi32_GetObjectA(CPU *c) { /* (h, size, buf) */
    GObj *o = obj(ARG(0));
    uint32_t buf = ARG(2), n = ARG(1);
    if (!o) { RET(3, 0); return; }
    if (o->type == O_BITMAP) {
        if (buf && n >= 24) {
            W32(buf, 0);
            W32(buf + 4, (uint32_t)o->bm.w);
            W32(buf + 8, (uint32_t)o->bm.h);
            W32(buf + 12, (uint32_t)o->bm.w * 4);
            W16(buf + 16, 1);
            W16(buf + 18, 32);
            W32(buf + 20, 0);
        }
        RET(3, 24);
        return;
    }
    RET(3, 0);
}

void I_gdi32_GetDIBits(CPU *c) { /* (hdc, hbm, start, lines, bits, bmi, usage) */
    GObj *o = obj(ARG(1));
    uint32_t start = ARG(2), lines = ARG(3), bits = ARG(4), bmi = ARG(5);
    if (!o || o->type != O_BITMAP) { RET(7, 0); return; }
    GdiTarget *t = &o->bm;
    if (!bits) {
        W32(bmi + 4, (uint32_t)t->w);
        W32(bmi + 8, (uint32_t)t->h);
        W16(bmi + 12, 1);
        if (!R16(bmi + 14)) W16(bmi + 14, 32);
        W32(bmi + 16, 0);
        W32(bmi + 20, (uint32_t)(t->w * t->h * 4));
        RET(7, (uint32_t)t->h);
        return;
    }
    int32_t bh = (int32_t)R32(bmi + 8);
    uint16_t bpp = R16(bmi + 14);
    bool topdown = bh < 0;
    int w = t->w;
    uint32_t stride = (uint32_t)(((w * bpp + 31) / 32) * 4);
    uint32_t done = 0;
    for (uint32_t k = 0; k < lines && start + k < (uint32_t)t->h; k++) {
        int y = topdown ? (int)(start + k) : t->h - 1 - (int)(start + k);
        const uint32_t *src = t->pixels + (size_t)y * (size_t)t->w;
        uint32_t row = bits + k * stride;
        for (uint32_t x = 0; x < (uint32_t)w; x++) {
            uint32_t p = src[x];
            if (bpp == 32) W32(row + 4 * x, p & 0xFFFFFF);
            else if (bpp == 24) {
                W8(row + 3 * x, (uint8_t)p);
                W8(row + 3 * x + 1, (uint8_t)(p >> 8));
                W8(row + 3 * x + 2, (uint8_t)(p >> 16));
            } else if (bpp == 16)
                W16(row + 2 * x, (uint16_t)(((p >> 9) & 0x7C00) | ((p >> 6) & 0x3E0) | ((p >> 3) & 0x1F)));
        }
        done++;
    }
    RET(7, done);
}

/* blit between DCs; each side's logical units map through its own scale */
static void blit(Dc *dd, int dx, int dy, int dw, int dh, Dc *sd, int sx, int sy, int sw, int sh, uint32_t rop) {
    if (!dd || !dd->t || !sd || !sd->t || !dd->cg) return;
    GdiTarget *s = sd->t, under;
    uint32_t *tmp = NULL;
    if (s->fill) {
        /* a render target: its picture with the GDI layer drawn so far on top */
        size_t n = (size_t)s->pw * (size_t)s->ph;
        tmp = malloc(n * 4);
        s->fill(s->fill_ctx, tmp);
        for (size_t i = 0; i < n; i++) {
            uint32_t o = s->pixels[i], a = o >> 24, u = tmp[i];
            if (!a) continue;
            uint32_t k = 255 - a;
            uint32_t r = ((o >> 16) & 0xFF) + ((u >> 16) & 0xFF) * k / 255, g = ((o >> 8) & 0xFF) + ((u >> 8) & 0xFF) * k / 255,
                     b = (o & 0xFF) + (u & 0xFF) * k / 255;
            tmp[i] = 0xFF000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
        under = *s;
        under.pixels = tmp;
        s = &under;
        static int logged;
        if (logged++ < 20) rt_log("GDI read from a render target (%dx%d)", s->w, s->h);
    }
    CGContextRef src = make_cg(s);
    if (!src) { free(tmp); return; }
    CGImageRef img = CGBitmapContextCreateImage(src);   /* may share tmp's bytes until released */
    CGContextRelease(src);
    CGRect r = CGRectMake(sx * s->scale, sy * s->scale, sw * s->scale, sh * s->scale);
    CGImageRef part = CGImageCreateWithImageInRect(img, r);
    CGContextSaveGState(dd->cg);
    CGContextSetInterpolationQuality(dd->cg, dd->stretch_mode == 3 ? kCGInterpolationNone : kCGInterpolationHigh);
    CGContextTranslateCTM(dd->cg, dx, dy + dh);
    CGContextScaleCTM(dd->cg, 1, -1);
    if (rop != 0x00CC0020u) rt_log("blit rop %08x drawn as SRCCOPY", rop);
    CGContextSetBlendMode(dd->cg, kCGBlendModeCopy);
    if (part) CGContextDrawImage(dd->cg, CGRectMake(0, 0, dw, dh), part);
    CGContextRestoreGState(dd->cg);
    if (part) CGImageRelease(part);
    CGImageRelease(img);
    free(tmp);
}
void I_gdi32_BitBlt(CPU *c) { /* (dst, x, y, w, h, src, sx, sy, rop) */
    int w = (int32_t)ARG(3), h = (int32_t)ARG(4);
    blit(dc_of(ARG(0)), (int32_t)ARG(1), (int32_t)ARG(2), w, h, dc_of(ARG(5)), (int32_t)ARG(6), (int32_t)ARG(7), w,
         h, ARG(8));
    RET(9, 1);
}
void I_gdi32_StretchBlt(CPU *c) { /* (dst, x, y, w, h, src, sx, sy, sw, sh, rop) */
    blit(dc_of(ARG(0)), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), dc_of(ARG(5)),
         (int32_t)ARG(6), (int32_t)ARG(7), (int32_t)ARG(8), (int32_t)ARG(9), ARG(10));
    RET(11, 1);
}
void I_gdi32_SetStretchBltMode(CPU *c) {
    Dc *d = dc_of(ARG(0));
    int prev = d ? d->stretch_mode : 0;
    if (d) d->stretch_mode = (int)ARG(1);
    RET(2, (uint32_t)(prev ? prev : 1));
}

/* ------------------------------------------------------------------ pens, brushes, shapes */
void I_gdi32_CreatePen(CPU *c) {
    uint32_t h = new_obj(O_PEN);
    obj(h)->style = (int)ARG(0);
    obj(h)->width = (int)ARG(1);
    obj(h)->color = ARG(2);
    RET(3, h);
}
void I_gdi32_CreateSolidBrush(CPU *c) {
    uint32_t h = new_obj(O_BRUSH);
    obj(h)->color = ARG(0);
    RET(1, h);
}
static bool set_pen(Dc *d) {
    GObj *p = obj(d->pen);
    if (!p || p->style == 5) return false;
    cg_color(d->cg, p->color, false);
    CGContextSetLineWidth(d->cg, p->width > 0 ? p->width : 1);
    return true;
}
static bool set_brush(Dc *d) {
    GObj *b = obj(d->brush);
    if (!b || b->style == 1) return false;
    cg_color(d->cg, b->color, true);
    return true;
}
static void path_points(Dc *d, uint32_t pts, uint32_t n, bool close) {
    CGContextBeginPath(d->cg);
    for (uint32_t i = 0; i < n; i++) {
        CGFloat x = (int32_t)R32(pts + 8 * i) + 0.5, y = (int32_t)R32(pts + 8 * i + 4) + 0.5;
        if (i == 0) CGContextMoveToPoint(d->cg, x, y);
        else CGContextAddLineToPoint(d->cg, x, y);
    }
    if (close) CGContextClosePath(d->cg);
}
void I_gdi32_Polygon(CPU *c) {
    Dc *d = dc_of(ARG(0));
    if (d && d->cg && ARG(2) > 1) {
        bool f = set_brush(d), s = set_pen(d);
        path_points(d, ARG(1), ARG(2), true);
        if (f || s) CGContextDrawPath(d->cg, f && s ? kCGPathFillStroke : f ? kCGPathFill : kCGPathStroke);
    }
    RET(3, 1);
}
void I_gdi32_Polyline(CPU *c) {
    Dc *d = dc_of(ARG(0));
    if (d && d->cg && ARG(2) > 1 && set_pen(d)) {
        path_points(d, ARG(1), ARG(2), false);
        CGContextStrokePath(d->cg);
    }
    RET(3, 1);
}
static void rect_shape(Dc *d, int l, int t, int r, int b, CGFloat rw, CGFloat rh) {
    if (!d || !d->cg || r <= l || b <= t) return;
    bool f = set_brush(d), s = set_pen(d);
    if (!f && !s) return;
    CGRect rc = CGRectMake(l + 0.5, t + 0.5, r - l - 1, b - t - 1);
    CGFloat cw = rw / 2 < rc.size.width / 2 ? rw / 2 : rc.size.width / 2;
    CGFloat ch = rh / 2 < rc.size.height / 2 ? rh / 2 : rc.size.height / 2;
    CGPathRef p = CGPathCreateWithRoundedRect(rc, cw < 0 ? 0 : cw, ch < 0 ? 0 : ch, NULL);
    CGContextAddPath(d->cg, p);
    CGContextDrawPath(d->cg, f && s ? kCGPathFillStroke : f ? kCGPathFill : kCGPathStroke);
    CGPathRelease(p);
}
void I_gdi32_Rectangle(CPU *c) {
    rect_shape(dc_of(ARG(0)), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), 0, 0);
    RET(5, 1);
}
void I_gdi32_RoundRect(CPU *c) {
    rect_shape(dc_of(ARG(0)), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4), (int32_t)ARG(5),
               (int32_t)ARG(6));
    RET(7, 1);
}

/* ------------------------------------------------------------------ text */
static CTFontRef font_ref(GObj *f) {
    CFStringRef name = CFStringCreateWithCString(NULL, f->face[0] ? f->face : "Arial", kCFStringEncodingUTF8);
    CTFontRef base = CTFontCreateWithName(name, f->font_px, NULL);
    CFRelease(name);
    CTFontSymbolicTraits want = (f->weight >= 600 ? kCTFontBoldTrait : 0) | (f->italic ? kCTFontItalicTrait : 0);
    if (want) {
        CTFontRef t = CTFontCreateCopyWithSymbolicTraits(base, f->font_px, NULL, want, want);
        if (t) { CFRelease(base); base = t; }
    }
    return base;
}
void I_gdi32_CreateFontA(CPU *c) {
    /* (height, width, esc, orient, weight, italic, underline, strike, charset, outprec, clipprec,
        quality, pitch, face) */
    uint32_t h = new_obj(O_FONT);
    GObj *f = obj(h);
    int32_t height = (int32_t)ARG(0);
    f->weight = (int)ARG(4);
    f->italic = (int)ARG(5);
    snprintf(f->face, sizeof f->face, "%s", ARG(13) ? GSTR(ARG(13)) : "Arial");
    /* negative: character (em) height; positive: cell height (ascent + descent) */
    CTFontRef probe = CTFontCreateWithName(CFSTR("ArialMT"), 100, NULL);
    double cell = CTFontGetAscent(probe) + CTFontGetDescent(probe);
    CFRelease(probe);
    f->font_px = height < 0 ? -height : height > 0 ? height * 100.0 / cell : 16;
    rt_log("CreateFontA(%d, weight %d, %s) -> %.1f px", height, f->weight, f->face, f->font_px);
    RET(14, h);
}
void I_gdi32_AddFontResourceA(CPU *c) {
    rt_log("AddFontResourceA(%s)", GSTR(ARG(0)));
    RET(1, 1);
}
void I_gdi32_RemoveFontResourceA(CPU *c) { RET(1, 1); }
void I_gdi32_SetMapperFlags(CPU *c) { RET(2, 0); }
void I_gdi32_SetMapMode(CPU *c) { RET(2, 1); }

static CFAttributedStringRef attr_string(const char *s, int n, CTFontRef font, CGColorRef color) {
    CFStringRef str = CFStringCreateWithBytes(NULL, (const UInt8 *)s, n, kCFStringEncodingWindowsLatin1, false);
    if (!str) str = CFSTR("");
    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void *vals[] = {font, color};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, color ? 2 : 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef a = CFAttributedStringCreate(NULL, str, attrs);
    CFRelease(str);
    CFRelease(attrs);
    return a;
}
static void measure(GObj *f, const char *s, int n, double *w, double *asc, double *desc) {
    CTFontRef font = font_ref(f);
    CFAttributedStringRef a = attr_string(s, n, font, NULL);
    CTLineRef line = CTLineCreateWithAttributedString(a);
    CGFloat as, de, le;
    *w = CTLineGetTypographicBounds(line, &as, &de, &le);
    *asc = CTFontGetAscent(font);
    *desc = CTFontGetDescent(font);
    CFRelease(line);
    CFRelease(a);
    CFRelease(font);
}
void I_gdi32_GetTextExtentPoint32A(CPU *c) { /* (hdc, str, len, size*) */
    Dc *d = dc_of(ARG(0));
    GObj *f = d ? obj(d->font) : NULL;
    double w = 0, as = 0, de = 0;
    if (f) measure(f, GSTR(ARG(1)), (int)ARG(2), &w, &as, &de);
    W32(ARG(3), (uint32_t)(int)ceil(w));
    W32(ARG(3) + 4, (uint32_t)(int)ceil(as + de));
    RET(4, 1);
}
void I_gdi32_GetTextMetricsA(CPU *c) {
    Dc *d = dc_of(ARG(0));
    GObj *f = d ? obj(d->font) : NULL;
    uint32_t tm = ARG(1);
    memset(GPTR(tm), 0, 56);
    if (f) {
        double w, as, de;
        measure(f, "x", 1, &w, &as, &de);
        int a = (int)ceil(as), dsc = (int)ceil(de);
        W32(tm, (uint32_t)(a + dsc));
        W32(tm + 4, (uint32_t)a);
        W32(tm + 8, (uint32_t)dsc);
        W32(tm + 12, (uint32_t)(a + dsc - (int)f->font_px));
        W32(tm + 20, (uint32_t)(int)ceil(w));
        W32(tm + 24, (uint32_t)(int)ceil(f->font_px));
        W32(tm + 28, (uint32_t)(f->weight ? f->weight : 400));
        W8(tm + 44, ' ');
        W8(tm + 45, 255);
        W8(tm + 46, '?');
        W8(tm + 47, ' ');
        W8(tm + 48, (uint8_t)(f->italic != 0));
        W8(tm + 51, 0x26);
    }
    RET(2, 1);
}
void I_gdi32_TextOutA(CPU *c) { /* (hdc, x, y, str, len) */
    Dc *d = dc_of(ARG(0));
    int x = (int32_t)ARG(1), y = (int32_t)ARG(2), n = (int)ARG(4);
    const char *s = GSTR(ARG(3));
    static int logged;
    if (g_trace || logged < 60 || (rt_bltlog() && logged < 400)) { logged++; rt_log("TextOutA(%d,%d \"%.*s\")", x, y, n, s); }
    GObj *f = d ? obj(d->font) : NULL;
    if (!d || !d->cg || n <= 0 || !f) { RET(5, 1); return; }
    CTFontRef font = font_ref(f);
    uint32_t tc = d->text_color;
    CGColorRef col = CGColorCreateSRGB((tc & 0xFF) / 255.0, ((tc >> 8) & 0xFF) / 255.0, ((tc >> 16) & 0xFF) / 255.0, 1);
    CFAttributedStringRef a = attr_string(s, n, font, col);
    CTLineRef line = CTLineCreateWithAttributedString(a);
    CGFloat as, de, le;
    double w = CTLineGetTypographicBounds(line, &as, &de, &le);
    double asc = CTFontGetAscent(font), desc = CTFontGetDescent(font);
    uint32_t al = d->text_align;
    double ox = x, base = y + asc;     /* TA_TOP */
    if ((al & 6) == 6) ox = x - w / 2; /* TA_CENTER */
    else if (al & 2) ox = x - w;       /* TA_RIGHT */
    if ((al & 24) == 24) base = y;     /* TA_BASELINE */
    else if (al & 8) base = y - desc;  /* TA_BOTTOM */
    CGContextSaveGState(d->cg);
    if (d->bk_mode == 2) {
        cg_color(d->cg, d->bk_color, true);
        CGContextFillRect(d->cg, CGRectMake(ox, base - asc, w, asc + desc));
    }
    /* the CTM flips y; flip again around the baseline so glyphs are upright */
    CGContextTranslateCTM(d->cg, ox, base);
    CGContextScaleCTM(d->cg, 1, -1);
    CGContextSetTextPosition(d->cg, 0, 0);
    CTLineDraw(line, d->cg);
    CGContextRestoreGState(d->cg);
    CFRelease(line);
    CFRelease(a);
    CGColorRelease(col);
    CFRelease(font);
    RET(5, 1);
}
void I_gdi32_SetTextColor(CPU *c) {
    Dc *d = dc_of(ARG(0));
    uint32_t prev = d ? d->text_color : 0;
    if (d) d->text_color = ARG(1);
    RET(2, prev);
}
void I_gdi32_SetBkColor(CPU *c) {
    Dc *d = dc_of(ARG(0));
    uint32_t prev = d ? d->bk_color : 0;
    if (d) d->bk_color = ARG(1);
    RET(2, prev);
}
void I_gdi32_SetBkMode(CPU *c) {
    Dc *d = dc_of(ARG(0));
    int prev = d ? d->bk_mode : 0;
    if (d) d->bk_mode = (int)ARG(1);
    RET(2, (uint32_t)prev);
}
void I_gdi32_SetTextAlign(CPU *c) {
    Dc *d = dc_of(ARG(0));
    uint32_t prev = d ? d->text_align : 0;
    if (d) d->text_align = ARG(1);
    RET(2, prev);
}
void I_gdi32_GetTextAlign(CPU *c) {
    Dc *d = dc_of(ARG(0));
    RET(1, d ? d->text_align : 0);
}
void I_gdi32_GetDeviceCaps(CPU *c) {
    int w, h;
    plat_client_size(&w, &h);
    uint32_t i = ARG(1), v = 0;
    switch (i) {
    case 8: v = (uint32_t)w; break;   /* HORZRES */
    case 10: v = (uint32_t)h; break;  /* VERTRES */
    case 12: v = 32; break;           /* BITSPIXEL */
    case 14: v = 1; break;            /* PLANES */
    case 88: case 90: v = 96; break;  /* LOGPIXELSX/Y */
    case 38: v = 0x7E99; break;       /* RASTERCAPS */
    case 4: v = 320; break;           /* HORZSIZE mm */
    case 6: v = 240; break;           /* VERTSIZE mm */
    default: v = 0;
    }
    RET(2, v);
}
