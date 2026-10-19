/* DirectDraw 7 (+ IDirect3D7) on the Metal renderer. Every surface keeps its pixels in guest
 * memory for Lock; render targets also live on the GPU at the window's resolution. */
#include <stdlib.h>
#include "ddraw.h"
#include "gdi.h"
#include "hdtex.h"

#define DD_OK 0u
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTLOCKED 0x887602AEu
#define DDENUMRET_OK 1

#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_TEXTURE 0x1000
#define DDSCAPS_3DDEVICE 0x2000
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSCAPS_ZBUFFER 0x20000
#define DDSCAPS_MIPMAP 0x400000
#define DDSCAPS_LOCALVIDMEM 0x10000000

#define DDPF_ALPHAPIXELS 0x1
#define DDPF_PALETTEINDEXED8 0x20
#define DDPF_RGB 0x40
#define DDPF_ZBUFFER 0x400

static int g_disp_w = 640, g_disp_h = 480, g_disp_bpp = 16;
static uint32_t g_dd_obj, g_d3d_obj;
uint32_t ddraw_d3d_obj(void) { return g_d3d_obj; }
void ddraw_get_display(int *w, int *h, int *bpp) { *w = g_disp_w; *h = g_disp_h; *bpp = g_disp_bpp; }

/* ------------------------------------------------------------------ pixel formats */
static int shift_of(uint32_t m) { return m ? __builtin_ctz(m) : 0; }
static int bits_of(uint32_t m) { return __builtin_popcount(m); }
static uint32_t expand(uint32_t v, uint32_t m) {
    if (!m) return 0;
    uint32_t x = (v & m) >> shift_of(m);
    int n = bits_of(m);
    if (n >= 8) return x >> (n - 8);
    uint32_t r = x << (8 - n);
    for (int k = n; k < 8; k += n) r |= r >> n; /* replicate high bits into the low ones */
    return r & 0xFF;
}
static uint32_t pack(uint32_t c8, uint32_t m) {
    if (!m) return 0;
    int n = bits_of(m);
    uint32_t x = n >= 8 ? c8 << (n - 8) : c8 >> (8 - n);
    return (x << shift_of(m)) & m;
}
static uint32_t read_px(Surf *s, uint32_t a) {
    switch (s->bpp) {
    case 8: return R8(a);
    case 16: return R16(a);
    case 24: return R8(a) | (uint32_t)R8(a + 1) << 8 | (uint32_t)R8(a + 2) << 16;
    default: return R32(a);
    }
}
static void write_px(Surf *s, uint32_t a, uint32_t v) {
    switch (s->bpp) {
    case 8: W8(a, (uint8_t)v); break;
    case 16: W16(a, (uint16_t)v); break;
    case 24: W8(a, (uint8_t)v); W8(a + 1, (uint8_t)(v >> 8)); W8(a + 2, (uint8_t)(v >> 16)); break;
    default: W32(a, v);
    }
}
static uint32_t pal_entry(Surf *s, uint32_t i);
uint32_t surf_pixel_to_bgra(Surf *s, uint32_t v, bool *keyed) {
    *keyed = s->has_ck && v >= s->ck_lo && v <= s->ck_hi;
    if (s->pf_flags & DDPF_PALETTEINDEXED8) return pal_entry(s, v & 0xFF);
    uint32_t r = expand(v, s->rmask), g = expand(v, s->gmask), b = expand(v, s->bmask);
    uint32_t a = (s->pf_flags & DDPF_ALPHAPIXELS) && s->amask ? expand(v, s->amask) : 255;
    return a << 24 | r << 16 | g << 8 | b;
}
static uint32_t bgra_to_pixel(Surf *s, uint32_t c) {
    if (s->pf_flags & DDPF_PALETTEINDEXED8) return 0;
    uint32_t v = pack((c >> 16) & 0xFF, s->rmask) | pack((c >> 8) & 0xFF, s->gmask) | pack(c & 0xFF, s->bmask);
    if (s->amask) v |= pack(c >> 24, s->amask);
    return v;
}
static void put_pf(uint32_t p, Surf *s) {
    memset(GPTR(p), 0, 32);
    W32(p, 32);
    W32(p + 4, s->pf_flags);
    if (s->pf_flags & DDPF_ZBUFFER) {
        W32(p + 12, s->zbits);
        W32(p + 20, s->zbits >= 32 ? 0xFFFFFFFFu : (1u << s->zbits) - 1);
        return;
    }
    W32(p + 12, (uint32_t)s->bpp);
    W32(p + 16, s->rmask);
    W32(p + 20, s->gmask);
    W32(p + 24, s->bmask);
    W32(p + 28, s->amask);
}
static void set_display_format(Surf *s) {
    s->bpp = g_disp_bpp;
    s->pf_flags = DDPF_RGB;
    if (g_disp_bpp == 16) { s->rmask = 0xF800; s->gmask = 0x07E0; s->bmask = 0x001F; }
    else { s->rmask = 0xFF0000; s->gmask = 0xFF00; s->bmask = 0xFF; }
    s->amask = 0;
}

/* ------------------------------------------------------------------ palettes */
typedef struct { uint32_t entries[256]; } Pal;
static ComIface g_pal_iface;
static uint32_t pal_entry(Surf *s, uint32_t i) {
    Pal *p = s->palette ? com_host(s->palette) : NULL;
    if (!p) return 0xFF000000u | i * 0x010101u;
    uint32_t e = p->entries[i]; /* PALETTEENTRY r g b flags */
    return 0xFF000000u | (e & 0xFF) << 16 | (e & 0xFF00) | ((e >> 16) & 0xFF);
}
static void p_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, DD_OK); }
static void p_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void p_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static void p_GetEntries(CPU *c) { /* (this, flags, base, n, out) */
    Pal *p = com_host(ARG(0));
    for (uint32_t i = 0; i < ARG(3) && ARG(2) + i < 256; i++) W32(ARG(4) + 4 * i, p->entries[ARG(2) + i]);
    RET(5, DD_OK);
}
static void p_SetEntries(CPU *c) {
    Pal *p = com_host(ARG(0));
    for (uint32_t i = 0; i < ARG(3) && ARG(2) + i < 256; i++) p->entries[ARG(2) + i] = R32(ARG(4) + 4 * i);
    RET(5, DD_OK);
}
static const ComMethod g_pal_methods[] = {{"QueryInterface", p_QueryInterface}, {"AddRef", p_AddRef},
    {"Release", p_Release}, {"GetCaps", NULL}, {"GetEntries", p_GetEntries}, {"Initialize", NULL},
    {"SetEntries", p_SetEntries}};
static void pal_destroy(uint32_t obj, void *host) { (void)obj; free(host); }
static ComIface g_pal_iface = {"IDirectDrawPalette", 7, g_pal_methods, 0, pal_destroy};

/* ------------------------------------------------------------------ clipper */
static void k_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, DD_OK); }
static void k_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void k_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static void k_SetHWnd(CPU *c) { RET(3, DD_OK); }
static void k_GetHWnd(CPU *c) { W32(ARG(1), 0x00010010u); RET(2, DD_OK); }
static void k_IsClipListChanged(CPU *c) { W32(ARG(1), 0); RET(2, DD_OK); }
static void k_SetClipList(CPU *c) { RET(3, DD_OK); }
static const ComMethod g_clip_methods[] = {{"QueryInterface", k_QueryInterface}, {"AddRef", k_AddRef},
    {"Release", k_Release}, {"GetClipList", NULL}, {"GetHWnd", k_GetHWnd}, {"Initialize", NULL},
    {"IsClipListChanged", k_IsClipListChanged}, {"SetClipList", k_SetClipList}, {"SetHWnd", k_SetHWnd}};
static ComIface g_clip_iface = {"IDirectDrawClipper", 9, g_clip_methods, 0};

/* ------------------------------------------------------------------ front buffer */
/* The primary surface is what the screen shows. Flip swaps it with the back buffer, as DirectDraw
 * does; anything the game draws straight onto the primary (the pause menu: colour fills and GDI
 * text, no Flip while it waits for input) is presented as well, at most once per refresh: a write
 * that comes sooner is shown by the next ddraw_idle (the game polls input and messages). */
static Surf *g_primary;
static uint32_t g_render_tid;   /* the thread that drives DirectDraw; the renderer is not thread-safe */
static bool g_front_dirty;
static uint64_t g_last_present;
uint32_t k32_current_tid(void);
static void present_front(void) {
    if (g_trace || rt_bltlog()) rt_log("present (front buffer)");
    surf_flush_overlay(g_primary);
    r_present(g_primary->rt);
    g_front_dirty = false;
    g_last_present = plat_ticks_us();
}
void ddraw_idle(void) {
    if (!g_front_dirty || !g_primary || !g_primary->rt || k32_current_tid() != g_render_tid) return;
    if (plat_ticks_us() - g_last_present < 15000) return;
    present_front();
}
static void front_written(Surf *s) {
    if (s != g_primary) return;
    g_front_dirty = true;
    ddraw_idle();
}

/* ------------------------------------------------------------------ surfaces */
static ComIface g_surf_iface;
Surf *surf_of(uint32_t obj) { return obj ? com_host(obj) : NULL; }
static Surf *self(CPU *c) { return com_host(ARG(0)); }

static Surf *surf_new(int w, int h, uint32_t caps) {
    Surf *s = calloc(1, sizeof *s);
    s->w = w > 0 ? w : 1;
    s->h = h > 0 ? h : 1;
    s->caps = caps;
    s->obj = com_new(&g_surf_iface, s, 0);
    return s;
}
static void surf_alloc(Surf *s) {
    int bytes = s->bpp == 24 ? 3 : s->bpp / 8;
    if (s->pf_flags & DDPF_ZBUFFER) bytes = (int)(s->zbits + 7) / 8;
    if (bytes < 1) bytes = 1;
    s->pitch = ((uint32_t)(s->w * bytes) + 3) & ~3u;
    s->mem = heap_alloc(s->pitch * (uint32_t)s->h, true);
}
static bool is_target(Surf *s) { return s->rt != NULL; }

/* Upload guest pixels (texture or CPU-drawn surface) into the GPU copy. */
static uint64_t g_sync_px, g_sync_n, g_sync_t;
/* the surface's pixels as BGRA, colour-keyed texels transparent (what the GPU copy holds) */
static uint32_t *surf_bgra(Surf *s) {
    uint32_t *buf = malloc((size_t)s->w * (size_t)s->h * 4);
    for (int y = 0; y < s->h; y++) {
        uint32_t row = s->mem + (uint32_t)y * s->pitch;
        int bytes = s->bpp == 24 ? 3 : s->bpp / 8;
        for (int x = 0; x < s->w; x++) {
            bool k;
            uint32_t c = surf_pixel_to_bgra(s, read_px(s, row + (uint32_t)(x * bytes)), &k);
            buf[(size_t)y * (size_t)s->w + (size_t)x] = k ? 0 : c;
        }
    }
    return buf;
}
void surf_sync_texture(Surf *s) {
    if (!s->tex) s->tex = r_tex_create(s->w, s->h), s->tex_dirty = true;
    if (!s->tex_dirty) return;
    g_sync_px += (uint64_t)s->w * (uint64_t)s->h;
    g_sync_n++;
    if (plat_ticks_us() - g_sync_t > 2000000) {
        rt_log("texture sync: %llu uploads, %llu Kpixels in 2s (last %dx%d bpp %d)", g_sync_n, g_sync_px / 1000, s->w, s->h, s->bpp);
        g_sync_n = g_sync_px = 0;
        g_sync_t = plat_ticks_us();
    }
    uint32_t *buf = surf_bgra(s);
    /* fonts, icons and HUD art use a handful of colours; photos and noisy backdrops do not */
    uint32_t seen[64];
    int nseen = 0;
    size_t total = (size_t)s->w * (size_t)s->h;
    for (size_t i = 0; i < total && nseen <= 40; i++) {
        uint32_t c = buf[i];
        if (!(c >> 24)) continue;
        int k;
        for (k = 0; k < nseen && seen[k] != c; k++) {}
        if (k == nseen && nseen < 64) seen[nseen++] = c;
    }
    r_tex_set_art(s->tex, nseen <= 40);
    r_tex_upload(s->tex, buf, s->w);
    free(buf);
    s->tex_dirty = false;
}

/* Bring a render target's guest pixels up to date (for Lock / CPU blits). */
static void target_to_cpu(Surf *s) {
    surf_flush_overlay(s);
    uint32_t *buf = malloc((size_t)s->w * (size_t)s->h * 4);
    r_readback(s->rt, buf, s->w, s->h);
    int bytes = s->bpp / 8;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            write_px(s, s->mem + (uint32_t)y * s->pitch + (uint32_t)(x * bytes), bgra_to_pixel(s, buf[(size_t)y * (size_t)s->w + (size_t)x]));
    free(buf);
}

void surf_flush_overlay(Surf *s) {
    if (!s->rt || !s->overlay || !s->ov_dirty) return;
    r_overlay_draw(s->rt, s->overlay, s->ov_w, s->ov_h);
    memset(s->overlay, 0, (size_t)s->ov_w * (size_t)s->ov_h * 4);
    s->ov_dirty = false;
}

static void make_target(Surf *s) {
    if (!s->rt) s->rt = r_target_create(s->w, s->h);
}

static void fill_desc(uint32_t d, Surf *s) {
    memset(GPTR(d + 4), 0, 120);
    W32(d, 124);
    W32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (s->has_ck ? 0x10000 : 0));
    W32(d + 8, (uint32_t)s->h);
    W32(d + 12, (uint32_t)s->w);
    W32(d + 16, s->pitch);
    W32(d + 20, s->back ? 1 : 0);
    W32(d + 36, s->mem);
    W32(d + 64, s->ck_lo);
    W32(d + 68, s->ck_hi);
    put_pf(d + 72, s);
    W32(d + 104, s->caps);
    W32(d + 108, s->caps2);
}

/* IDirectDrawGammaControl: the in-game brightness setting; the ramp is applied when presenting */
static void g_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, DD_OK); }
static void g_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void g_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static uint16_t g_ramp[768];
static bool g_ramp_set;
static void g_GetGammaRamp(CPU *c) {
    uint32_t p = ARG(2);
    for (int i = 0; i < 768; i++) W16(p + 2u * (uint32_t)i, g_ramp_set ? g_ramp[i] : (uint16_t)((i % 256) * 257));
    RET(3, DD_OK);
}
static void g_SetGammaRamp(CPU *c) {
    uint32_t p = ARG(2);
    for (int i = 0; i < 768; i++) g_ramp[i] = R16(p + 2u * (uint32_t)i);
    g_ramp_set = true;
    r_set_gamma(g_ramp);
    RET(3, DD_OK);
}
static const ComMethod g_gamma_methods[] = {{"QueryInterface", g_QueryInterface}, {"AddRef", g_AddRef},
    {"Release", g_Release}, {"GetGammaRamp", g_GetGammaRamp}, {"SetGammaRamp", g_SetGammaRamp}};
static ComIface g_gamma_iface = {"IDirectDrawGammaControl", 5, g_gamma_methods, 0};

static void s_QueryInterface(CPU *c) {
    char g[40];
    guid_str(ARG(1), g);
    if (guid_eq(ARG(1), "69C11C3E-B46B-11D1-AD7A-00C04FC29B4E")) {
        static uint32_t gamma;
        if (!gamma) gamma = com_new(&g_gamma_iface, NULL, 0);
        com_addref(gamma);
        W32(ARG(2), gamma);
        RET(3, DD_OK);
        return;
    }
    if (guid_eq(ARG(1), "06675A80-3B9B-11D2-B92F-00609797EA5B") || guid_eq(ARG(1), "00000000-0000-0000-C000-000000000046")) {
        W32(ARG(2), ARG(0));
        com_addref(ARG(0));
        RET(3, DD_OK);
        return;
    }
    rt_log("surface QueryInterface(%s) -> no", g);
    W32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}
static void s_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void s_Release(CPU *c) { RET(1, com_release(ARG(0))); }
/* an attached surface (z buffer, back buffer) is held by the surface it is attached to */
static void s_AddAttachedSurface(CPU *c) {
    Surf *s = self(c), *a = surf_of(ARG(1));
    if (a) {
        Surf **slot = (a->caps & DDSCAPS_ZBUFFER) ? &s->zbuf : &s->back;
        com_addref(a->obj);
        if (*slot) com_release((*slot)->obj);
        *slot = a;
    }
    RET(2, DD_OK);
}
static void s_DeleteAttachedSurface(CPU *c) {
    Surf *s = self(c), *a = surf_of(ARG(2));
    if (s->zbuf && (!a || s->zbuf == a)) {
        Surf *z = s->zbuf;
        s->zbuf = NULL;
        com_release(z->obj);
    }
    RET(3, DD_OK);
}
static void s_GetAttachedSurface(CPU *c) { /* (this, caps*, out) */
    Surf *s = self(c);
    uint32_t want = R32(ARG(1));
    Surf *r = NULL;
    if (want & DDSCAPS_BACKBUFFER) r = s->back;
    else if (want & DDSCAPS_ZBUFFER) r = s->zbuf;
    else if (want & DDSCAPS_MIPMAP) r = s->mip_next;
    else if (want & DDSCAPS_FLIP) r = s->back;
    if (!r) { W32(ARG(2), 0); RET(3, DDERR_NOTFOUND); return; }
    com_addref(r->obj);
    W32(ARG(2), r->obj);
    RET(3, DD_OK);
}
static void s_EnumAttachedSurfaces(CPU *c) { /* (this, ctx, cb) cb(surf, desc, ctx) */
    Surf *s = self(c);
    uint32_t d = heap_alloc(124, true);
    Surf *list[3] = {s->back, s->zbuf, s->mip_next};
    for (int i = 0; i < 3; i++)
        if (list[i]) {
            fill_desc(d, list[i]);
            com_addref(list[i]->obj);
            if (!call_guest_ret(c, ARG(2), 3, list[i]->obj, d, ARG(1))) break;
        }
    heap_free(d);
    RET(3, DD_OK);
}
static void s_GetCaps(CPU *c) {
    Surf *s = self(c);
    W32(ARG(1), s->caps);
    W32(ARG(1) + 4, s->caps2);
    W32(ARG(1) + 8, 0);
    W32(ARG(1) + 12, 0);
    RET(2, DD_OK);
}
static void s_GetSurfaceDesc(CPU *c) { fill_desc(ARG(1), self(c)); RET(2, DD_OK); }
static void s_GetPixelFormat(CPU *c) { put_pf(ARG(1), self(c)); RET(2, DD_OK); }
static void s_SetColorKey(CPU *c) { /* (this, flags, ck) */
    Surf *s = self(c);
    uint32_t f = ARG(1);
    if (f & 8) { /* DDCKEY_SRCBLT */
        if (ARG(2)) {
            s->has_ck = true;
            s->ck_lo = R32(ARG(2));
            s->ck_hi = (f & 1) ? R32(ARG(2) + 4) : s->ck_lo;
        } else {
            s->has_ck = false;
        }
        s->tex_dirty = true;
    }
    RET(3, DD_OK);
}
static void s_GetColorKey(CPU *c) {
    Surf *s = self(c);
    if (!s->has_ck) { RET(3, 0x88760186u); return; } /* DDERR_NOCOLORKEY */
    W32(ARG(2), s->ck_lo);
    W32(ARG(2) + 4, s->ck_hi);
    RET(3, DD_OK);
}
static void s_IsLost(CPU *c) { RET(1, DD_OK); }
static void s_Restore(CPU *c) { RET(1, DD_OK); }
static void s_SetClipper(CPU *c) { RET(2, DD_OK); }
static void s_GetClipper(CPU *c) { W32(ARG(1), 0); RET(2, 0x887600FFu); }
static void s_SetPalette(CPU *c) {
    Surf *s = self(c);
    uint32_t old = s->palette;
    if (ARG(1)) com_addref(ARG(1));   /* the surface holds its palette, as DirectDraw does */
    s->palette = ARG(1);
    if (old) com_release(old);
    s->tex_dirty = true;
    RET(2, DD_OK);
}
static void s_GetPalette(CPU *c) {
    Surf *s = self(c);
    if (s->palette) com_addref(s->palette);
    W32(ARG(1), s->palette);
    RET(2, s->palette ? DD_OK : 0x887600FFu);
}
static void s_GetDDInterface(CPU *c) { com_addref(g_dd_obj); W32(ARG(1), g_dd_obj); RET(2, DD_OK); }
static void s_Nop1(CPU *c) { RET(1, DD_OK); }
static void s_Nop2(CPU *c) { RET(2, DD_OK); }
static void s_Nop3(CPU *c) { RET(3, DD_OK); }
static void s_GetFlipStatus(CPU *c) { RET(2, DD_OK); }
static void s_GetBltStatus(CPU *c) { RET(2, DD_OK); }
static void s_GetUniquenessValue(CPU *c) { W32(ARG(1), 1); RET(2, DD_OK); }

static int g_lock_log;
static void s_Lock(CPU *c) { /* (this, rect, desc, flags, event) */
    Surf *s = self(c);
    if (rt_bltlog() && g_lock_log < 200) {
        g_lock_log++;
        rt_log("lock %dx%d caps %x%s rect %s flags %x (from %08x)", s->w, s->h, s->caps, s->rt ? ",rt" : "",
               ARG(1) ? "yes" : "no", ARG(3), R32(ESP));
    }
    uint32_t r = ARG(1), d = ARG(2);
    if (is_target(s) && s->locks == 0) {
        target_to_cpu(s);
        free(s->lock_copy);
        s->lock_copy = malloc(s->pitch * (uint32_t)s->h);
        memcpy(s->lock_copy, GPTR(s->mem), s->pitch * (uint32_t)s->h);
    }
    s->locks++;
    fill_desc(d, s);
    uint32_t p = s->mem;
    if (r) {
        int bytes = s->bpp == 24 ? 3 : s->bpp / 8;
        p += (uint32_t)(int32_t)R32(r + 4) * s->pitch + (uint32_t)((int32_t)R32(r) * bytes);
    }
    W32(d + 36, p);
    RET(5, DD_OK);
}
static void s_Unlock(CPU *c) {
    Surf *s = self(c);
    if (s->locks > 0) s->locks--;
    if (s->locks == 0) {
        s->tex_dirty = true;
        if (is_target(s) && s->lock_copy) {
            /* the game may have drawn into the frame: push changed pixels back to the GPU target */
            if (memcmp(s->lock_copy, GPTR(s->mem), s->pitch * (uint32_t)s->h)) {
                surf_sync_texture(s);
                r_blit(s->rt, s->tex, 0, 0, (float)s->w, (float)s->h, 0, 0, s->w, s->h, false, false);
                front_written(s);
            }
            free(s->lock_copy);
            s->lock_copy = NULL;
        }
    }
    RET(2, DD_OK);
}

/* ------------------------------------------------------------------ GetDC / ReleaseDC */
typedef struct { Surf *s; GdiTarget t; bool cpu; } DcCtx;
static void dc_release(void *p) {
    DcCtx *x = p;
    Surf *s = x->s;
    if (x->cpu) {
        /* write the BGRA buffer back to the surface's own format */
        int bytes = s->bpp == 24 ? 3 : s->bpp / 8;
        for (int y = 0; y < s->h; y++)
            for (int xx = 0; xx < s->w; xx++) {
                uint32_t c = x->t.pixels[(size_t)y * (size_t)s->w + (size_t)xx];
                write_px(s, s->mem + (uint32_t)y * s->pitch + (uint32_t)(xx * bytes), bgra_to_pixel(s, c | 0xFF000000u));
            }
        free(x->t.pixels);
        s->tex_dirty = true;
    } else {
        s->ov_dirty = true;
        front_written(s);
    }
    free(x);
}
/* GDI reads from a render target's DC (the pause menu saves the screen with StretchBlt): the
 * target's picture at overlay resolution, opaque; gdi32 composites the pending GDI layer on top */
static void dc_fill(void *p, uint32_t *out) {
    DcCtx *x = p;
    r_readback(x->s->rt, out, x->t.pw, x->t.ph);
}
static int g_dc_log;
static void s_GetDC(CPU *c) {
    Surf *s = self(c);
    if (g_dc_log < 40 || (rt_bltlog() && g_dc_log < 400)) {
        g_dc_log++;
        rt_log("GetDC on %dx%d caps %08x%s (from %08x)", s->w, s->h, s->caps, s->rt ? " target" : "", R32(ESP));
    }
    DcCtx *x = calloc(1, sizeof *x);
    x->s = s;
    if (is_target(s)) {
        /* GDI on a render target draws into a high-resolution overlay composited by the GPU */
        int pw, ph;
        r_target_size(s->rt, &pw, &ph);
        if (!s->overlay || s->ov_w != pw || s->ov_h != ph) {
            free(s->overlay);
            s->overlay = calloc((size_t)pw * (size_t)ph, 4);
            s->ov_w = pw;
            s->ov_h = ph;
        }
        x->t.pixels = s->overlay;
        x->t.w = s->w;
        x->t.h = s->h;
        x->t.pw = pw;
        x->t.ph = ph;
        x->t.stride = pw * 4;
        x->t.scale = (float)ph / (float)s->h;
        x->t.fill = dc_fill;
        x->t.fill_ctx = x;
    } else {
        x->cpu = true;
        x->t.w = x->t.pw = s->w;
        x->t.h = x->t.ph = s->h;
        x->t.stride = s->w * 4;
        x->t.scale = 1;
        x->t.pixels = malloc((size_t)s->w * (size_t)s->h * 4);
        for (int y = 0; y < s->h; y++)
            for (int xx = 0; xx < s->w; xx++) {
                bool k;
                int bytes = s->bpp == 24 ? 3 : s->bpp / 8;
                x->t.pixels[(size_t)y * (size_t)s->w + (size_t)xx] =
                    surf_pixel_to_bgra(s, read_px(s, s->mem + (uint32_t)y * s->pitch + (uint32_t)(xx * bytes)), &k) | 0xFF000000u;
            }
    }
    s->dc = gdi_dc_for_target(&x->t, dc_release, x);
    W32(ARG(1), s->dc);
    RET(2, DD_OK);
}
static void s_ReleaseDC(CPU *c) {
    if (rt_bltlog() && g_dc_log < 400) { g_dc_log++; rt_log("ReleaseDC on caps %08x (from %08x)", self(c)->caps, R32(ESP)); }
    gdi_release_dc(ARG(1));
    self(c)->dc = 0;
    RET(2, DD_OK);
}

/* ------------------------------------------------------------------ Blt */
typedef struct { int x0, y0, x1, y1; } Rect;
static Rect rect_or_full(uint32_t r, Surf *s) {
    Rect o = {0, 0, s->w, s->h};
    if (r) { o.x0 = (int32_t)R32(r); o.y0 = (int32_t)R32(r + 4); o.x1 = (int32_t)R32(r + 8); o.y1 = (int32_t)R32(r + 12); }
    return o;
}
static void cpu_fill(Surf *d, Rect r, uint32_t v) {
    int bytes = d->bpp == 24 ? 3 : d->bpp / 8;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > d->w) r.x1 = d->w;
    if (r.y1 > d->h) r.y1 = d->h;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) write_px(d, d->mem + (uint32_t)y * d->pitch + (uint32_t)(x * bytes), v);
    d->tex_dirty = true;
}
static void cpu_blit(Surf *d, Rect dr, Surf *s, Rect sr, bool keyed, uint32_t klo, uint32_t khi) {
    int db = d->bpp == 24 ? 3 : d->bpp / 8, sb = s->bpp == 24 ? 3 : s->bpp / 8;
    int dw = dr.x1 - dr.x0, dh = dr.y1 - dr.y0, sw = sr.x1 - sr.x0, sh = sr.y1 - sr.y0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    bool same = d->bpp == s->bpp && d->rmask == s->rmask && d->gmask == s->gmask && d->bmask == s->bmask &&
                d->pf_flags == s->pf_flags;
    for (int y = 0; y < dh; y++) {
        int dy = dr.y0 + y, sy = sr.y0 + y * sh / dh;
        if (dy < 0 || dy >= d->h || sy < 0 || sy >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int dx = dr.x0 + x, sx = sr.x0 + x * sw / dw;
            if (dx < 0 || dx >= d->w || sx < 0 || sx >= s->w) continue;
            uint32_t v = read_px(s, s->mem + (uint32_t)sy * s->pitch + (uint32_t)(sx * sb));
            if (keyed && v >= klo && v <= khi) continue;
            if (!same) {
                bool k;
                v = bgra_to_pixel(d, surf_pixel_to_bgra(s, v, &k));
            }
            write_px(d, d->mem + (uint32_t)dy * d->pitch + (uint32_t)(dx * db), v);
        }
    }
    d->tex_dirty = true;
}
static int g_blt_log;
static void do_blt(Surf *d, Rect dr, Surf *s, Rect sr, uint32_t flags, uint32_t fx) {
    if (rt_bltlog() && g_blt_log < 400) {
        g_blt_log++;
        rt_log("blt %dx%d(caps %x%s) [%d,%d,%d,%d] <- %s%dx%d(caps %x%s) [%d,%d,%d,%d] flags %x", d->w, d->h, d->caps,
               d->rt ? ",rt" : "", dr.x0, dr.y0, dr.x1, dr.y1, s ? "" : "none ", s ? s->w : 0, s ? s->h : 0,
               s ? s->caps : 0, s && s->rt ? ",rt" : "", sr.x0, sr.y0, sr.x1, sr.y1, flags);
    }
    bool keyed = false;
    uint32_t klo = 0, khi = 0;
    if (s && (flags & 0x8000) && s->has_ck) { keyed = true; klo = s->ck_lo; khi = s->ck_hi; }   /* DDBLT_KEYSRC */
    if (s && (flags & 0x10000) && fx) { keyed = true; klo = R32(fx + 92); khi = R32(fx + 96); } /* KEYSRCOVERRIDE */
    if (flags & 0x400) { /* DDBLT_COLORFILL */
        uint32_t v = fx ? R32(fx + 80) : 0;
        if (is_target(d)) {
            bool k;
            surf_flush_overlay(d);
            r_clear(d->rt, true, surf_pixel_to_bgra(d, v, &k), false, 1, dr.x0, dr.y0, dr.x1, dr.y1);
            front_written(d);
        } else {
            cpu_fill(d, dr, v);
        }
        return;
    }
    if (flags & 0x2000000) { /* DDBLT_DEPTHFILL: clear the z buffer of the target using it */
        return;
    }
    if (!s) return;
    if (is_target(d)) {
        surf_flush_overlay(d);
        if (is_target(s)) {
            /* target to target (screen effects copy strips of the frame onto itself; the pause menu
             * saves the screen to the back buffer and back): stay on the GPU */
            surf_flush_overlay(s);
            r_copy_target(d->rt, s->rt, (float)sr.x0, (float)sr.y0, (float)sr.x1, (float)sr.y1, dr.x0, dr.y0, dr.x1,
                          dr.y1);
            front_written(d);
            return;
        }
        if (keyed && (!s->has_ck || s->ck_lo != klo || s->ck_hi != khi)) {
            bool had = s->has_ck;
            uint32_t a = s->ck_lo, b = s->ck_hi;
            s->has_ck = true; s->ck_lo = klo; s->ck_hi = khi; s->tex_dirty = true;
            surf_sync_texture(s);
            s->has_ck = had; s->ck_lo = a; s->ck_hi = b; s->tex_dirty = true;
        } else {
            bool had = s->has_ck;
            if (!keyed && had) { s->has_ck = false; s->tex_dirty = true; }
            surf_sync_texture(s);
            if (!keyed && had) { s->has_ck = true; s->tex_dirty = true; }
        }
        r_blit(d->rt, s->tex, (float)sr.x0, (float)sr.y0, (float)sr.x1, (float)sr.y1, dr.x0, dr.y0, dr.x1, dr.y1,
               keyed, true);
        front_written(d);
        return;
    }
    if (is_target(s)) target_to_cpu(s);
    cpu_blit(d, dr, s, sr, keyed, klo, khi);
}
static void s_Blt(CPU *c) { /* (this, dstrect, src, srcrect, flags, fx) */
    Surf *d = self(c), *s = surf_of(ARG(2));
    do_blt(d, rect_or_full(ARG(1), d), s, s ? rect_or_full(ARG(3), s) : (Rect){0, 0, 0, 0}, ARG(4), ARG(5));
    RET(6, DD_OK);
}
static void s_BltFast(CPU *c) { /* (this, x, y, src, srcrect, flags) */
    Surf *d = self(c), *s = surf_of(ARG(3));
    if (!s) { RET(6, DDERR_INVALIDPARAMS); return; }
    Rect sr = rect_or_full(ARG(4), s);
    int x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    Rect dr = {x, y, x + sr.x1 - sr.x0, y + sr.y1 - sr.y0};
    do_blt(d, dr, s, sr, (ARG(5) & 1) ? 0x8000 : 0, 0);
    RET(6, DD_OK);
}

static void s_Flip(CPU *c) { /* (this, target, flags) */
    Surf *s = self(c);
    if (s == g_primary && s->rt && s->back && s->back->rt) {
        /* the back buffer becomes the screen and the old screen the next back buffer; the 3D
         * device draws into whatever target its surface holds */
        surf_flush_overlay(s->back);
        surf_flush_overlay(s);
        RTarget *t = s->rt;
        s->rt = s->back->rt;
        s->back->rt = t;
        present_front();
    }
    RET(3, DD_OK);
}

static void s_GetDC(CPU *c);
static const ComMethod g_surf_methods[] = {
    {"QueryInterface", s_QueryInterface}, {"AddRef", s_AddRef}, {"Release", s_Release},
    {"AddAttachedSurface", s_AddAttachedSurface}, {"AddOverlayDirtyRect", NULL}, {"Blt", s_Blt}, {"BltBatch", NULL},
    {"BltFast", s_BltFast}, {"DeleteAttachedSurface", s_DeleteAttachedSurface},
    {"EnumAttachedSurfaces", s_EnumAttachedSurfaces}, {"EnumOverlayZOrders", NULL}, {"Flip", s_Flip},
    {"GetAttachedSurface", s_GetAttachedSurface}, {"GetBltStatus", s_GetBltStatus}, {"GetCaps", s_GetCaps},
    {"GetClipper", s_GetClipper}, {"GetColorKey", s_GetColorKey}, {"GetDC", s_GetDC},
    {"GetFlipStatus", s_GetFlipStatus}, {"GetOverlayPosition", NULL}, {"GetPalette", s_GetPalette},
    {"GetPixelFormat", s_GetPixelFormat}, {"GetSurfaceDesc", s_GetSurfaceDesc}, {"Initialize", NULL},
    {"IsLost", s_IsLost}, {"Lock", s_Lock}, {"ReleaseDC", s_ReleaseDC}, {"Restore", s_Restore},
    {"SetClipper", s_SetClipper}, {"SetColorKey", s_SetColorKey}, {"SetOverlayPosition", NULL},
    {"SetPalette", s_SetPalette}, {"Unlock", s_Unlock}, {"UpdateOverlay", NULL}, {"UpdateOverlayDisplay", NULL},
    {"UpdateOverlayZOrder", NULL}, {"GetDDInterface", s_GetDDInterface}, {"PageLock", s_Nop2},
    {"PageUnlock", s_Nop2}, {"SetSurfaceDesc", NULL}, {"SetPrivateData", NULL}, {"GetPrivateData", NULL},
    {"FreePrivateData", NULL}, {"GetUniquenessValue", s_GetUniquenessValue}, {"ChangeUniquenessValue", s_Nop1},
    {"SetPriority", s_Nop2}, {"GetPriority", NULL}, {"SetLOD", s_Nop2}, {"GetLOD", NULL},
};
/* The last Release: a flipping chain's back buffer and a mipmap's smaller levels belong to the
 * surface that created them; attached z buffer and palette lose this surface's reference */
static void surf_destroy(uint32_t obj, void *host) {
    (void)obj;
    Surf *s = host;
    if (s == g_primary) {
        g_primary = NULL;
        g_front_dirty = false;
    }
    if (s->back) com_release(s->back->obj);
    if (s->mip_next) com_release(s->mip_next->obj);
    if (s->zbuf) com_release(s->zbuf->obj);
    if (s->palette) com_release(s->palette);
    if (s->locks || s->dc) rt_log("surface %dx%d released while locked or with a DC", s->w, s->h);
    heap_free(s->mem);
    r_tex_destroy(s->tex);
    if (s->rt) r_target_destroy(s->rt);
    free(s->overlay);
    free(s->lock_copy);
    free(s);
}
static ComIface g_surf_iface = {"IDirectDrawSurface7", 49, g_surf_methods, 0, surf_destroy};

/* every texture the game holds now, drawn or not, to the HD dump (texture harvest); animated ones
 * (content changing every frame) are left out */
static void dump_one(void *host, void *ctx) {
    Surf *s = host;
    if (!(s->caps & DDSCAPS_TEXTURE) || (s->pf_flags & DDPF_ZBUFFER) || !s->mem || s->locks) return;
    if (r_tex_animated(s->tex)) return;
    uint32_t *buf = surf_bgra(s);
    hdtex_dump(hdtex_key(buf, s->w, s->h, s->w), buf, s->w, s->h);
    free(buf);
    (*(int *)ctx)++;
}
int ddraw_dump_textures(void) {
    int n = 0;
    com_each(&g_surf_iface, dump_one, &n);
    return n;
}

/* ------------------------------------------------------------------ IDirectDraw7 */
static const int g_modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1600, 1200}};
static void dd_QueryInterface(CPU *c);
static void dd_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void dd_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static void dd_CreateClipper(CPU *c) { W32(ARG(2), com_new(&g_clip_iface, NULL, 0)); RET(4, DD_OK); }
static void dd_CreatePalette(CPU *c) { /* (this, flags, entries, out, outer) */
    Pal *p = calloc(1, sizeof *p);
    if (ARG(2)) for (int i = 0; i < 256; i++) p->entries[i] = R32(ARG(2) + 4u * (uint32_t)i);
    W32(ARG(3), com_new(&g_pal_iface, p, 0));
    RET(5, DD_OK);
}
static void parse_pf(Surf *s, uint32_t pf) {
    s->pf_flags = R32(pf + 4);
    if (s->pf_flags & DDPF_ZBUFFER) {
        s->zbits = R32(pf + 12);
        s->bpp = (int)s->zbits;
        return;
    }
    s->bpp = (int)R32(pf + 12);
    s->rmask = R32(pf + 16);
    s->gmask = R32(pf + 20);
    s->bmask = R32(pf + 24);
    s->amask = R32(pf + 28);
    if (s->pf_flags & DDPF_PALETTEINDEXED8) s->bpp = 8;
}
static void dd_CreateSurface(CPU *c) { /* (this, desc, out, outer) */
    uint32_t d = ARG(1);
    uint32_t flags = R32(d + 4), caps = R32(d + 104), caps2 = R32(d + 108);
    int w = (flags & 4) ? (int)R32(d + 12) : g_disp_w, h = (flags & 2) ? (int)R32(d + 8) : g_disp_h;
    bool primary = caps & DDSCAPS_PRIMARYSURFACE;
    if (primary) { w = g_disp_w; h = g_disp_h; }
    Surf *s = surf_new(w, h, caps | (primary ? DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY | 0x8000 : 0));
    s->caps2 = caps2;
    if ((flags & 0x1000) && !primary) parse_pf(s, d + 72);
    else if (caps & DDSCAPS_ZBUFFER) { s->pf_flags = DDPF_ZBUFFER; s->zbits = 16; s->bpp = 16; }
    else set_display_format(s);
    if ((caps & DDSCAPS_ZBUFFER) && (flags & 0x40)) { s->zbits = R32(d + 24); s->pf_flags = DDPF_ZBUFFER; }
    surf_alloc(s);
    if (flags & 0x10000) { s->has_ck = true; s->ck_lo = R32(d + 64); s->ck_hi = R32(d + 68); }
    if (primary || (caps & DDSCAPS_3DDEVICE)) make_target(s);
    if (primary) {
        g_primary = s;
        g_render_tid = k32_current_tid();
    }
    if (primary && (flags & 0x20) && R32(d + 20) > 0) {
        Surf *b = surf_new(w, h, DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY | DDSCAPS_FLIP |
                                     DDSCAPS_COMPLEX);
        set_display_format(b);
        surf_alloc(b);
        make_target(b);
        b->front = s;
        s->back = b;
    }
    if ((caps & DDSCAPS_MIPMAP) && (flags & 0x20000)) {
        Surf *prev = s;
        uint32_t n = R32(d + 24);
        for (uint32_t i = 1; i < n; i++) {
            int mw = prev->w / 2 > 0 ? prev->w / 2 : 1, mh = prev->h / 2 > 0 ? prev->h / 2 : 1;
            Surf *m = surf_new(mw, mh, caps);
            m->bpp = s->bpp; m->pf_flags = s->pf_flags;
            m->rmask = s->rmask; m->gmask = s->gmask; m->bmask = s->bmask; m->amask = s->amask;
            surf_alloc(m);
            prev->mip_next = m;
            prev = m;
        }
    }
    if (g_trace || primary || (caps & (DDSCAPS_3DDEVICE | DDSCAPS_ZBUFFER)))
        rt_log("CreateSurface %dx%d caps %08x bpp %d masks %x/%x/%x/%x%s -> %08x", w, h, caps, s->bpp, s->rmask,
               s->gmask, s->bmask, s->amask, s->back ? " +backbuffer" : "", s->obj);
    W32(ARG(2), s->obj);
    RET(4, DD_OK);
}
static void mode_desc(uint32_t d, int w, int h, int bpp) {
    Surf tmp = {0};
    tmp.w = w; tmp.h = h;
    int save = g_disp_bpp;
    g_disp_bpp = bpp;
    set_display_format(&tmp);
    g_disp_bpp = save;
    tmp.pitch = (uint32_t)(w * bpp / 8);
    memset(GPTR(d), 0, 124);
    W32(d, 124);
    W32(d + 4, 0x2 | 0x4 | 0x8 | 0x1000 | 0x40000);
    W32(d + 8, (uint32_t)h);
    W32(d + 12, (uint32_t)w);
    W32(d + 16, tmp.pitch);
    W32(d + 24, 60);
    put_pf(d + 72, &tmp);
}
static void dd_EnumDisplayModes(CPU *c) { /* (this, flags, desc, ctx, cb) */
    uint32_t d = heap_alloc(124, true);
    for (int b = 0; b < 2; b++)
        for (size_t i = 0; i < sizeof g_modes / sizeof g_modes[0]; i++) {
            mode_desc(d, g_modes[i][0], g_modes[i][1], b ? 32 : 16);
            if (call_guest_ret(c, ARG(4), 2, d, ARG(3)) != DDENUMRET_OK) goto done;
        }
done:
    heap_free(d);
    RET(5, DD_OK);
}
static void dd_GetDisplayMode(CPU *c) { mode_desc(ARG(1), g_disp_w, g_disp_h, g_disp_bpp); RET(2, DD_OK); }
static void dd_SetDisplayMode(CPU *c) { /* (this, w, h, bpp, refresh, flags) */
    g_disp_w = (int)ARG(1);
    g_disp_h = (int)ARG(2);
    g_disp_bpp = (int)ARG(3);
    rt_log("SetDisplayMode(%dx%dx%d)", g_disp_w, g_disp_h, g_disp_bpp);
    plat_set_game_size(g_disp_w, g_disp_h);
    RET(6, DD_OK);
}
static void dd_RestoreDisplayMode(CPU *c) { RET(1, DD_OK); }
static void dd_SetCooperativeLevel(CPU *c) {
    rt_log("SetCooperativeLevel(%08x)", ARG(2));
    /* On Windows, DDSCL_FULLSCREEN makes the game's window visible; the game never calls ShowWindow */
    plat_window_show();
    plat_window_fullscreen((ARG(2) & 1) != 0);   /* DDSCL_FULLSCREEN */
    RET(3, DD_OK);
}
static void dd_GetCaps(CPU *c) { /* (this, drv, hel) */
    for (int k = 1; k <= 2; k++) {
        uint32_t p = ARG(k);
        if (!p) continue;
        uint32_t size = R32(p);
        memset(GPTR(p + 4), 0, size > 4 ? size - 4 : 0);
        W32(p + 4, 0x00000001u | 0x40u | 0x200u | 0x400u | 0x10000u | 0x40000u | 0x80000u | 0x200000u | 0x4000000u);
        W32(p + 8, 0x00080000u | 0x200000u);
        W32(p + 12, 0x200u);         /* DDCKEYCAPS_SRCBLT */
        W32(p + 56, 0x400u | 0x200u | 0x100u);
        W32(p + 60, 256u << 20);
        W32(p + 64, 200u << 20);
        W32(p + 132, 0x7FFFFFFFu);
    }
    RET(3, DD_OK);
}
static void dd_GetAvailableVidMem(CPU *c) {
    if (ARG(2)) W32(ARG(2), 256u << 20);
    if (ARG(3)) W32(ARG(3), 200u << 20);
    RET(4, DD_OK);
}
static void dd_GetDeviceIdentifier(CPU *c) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 1068); /* DDDEVICEIDENTIFIER2: exactly this many bytes, the caller's locals follow */
    strcpy((char *)GPTR(p), "metal.drv");
    strcpy((char *)GPTR(p + 512), "Apple Metal");
    W32(p + 1032, 0x106B);
    RET(3, DD_OK);
}
static void dd_TestCooperativeLevel(CPU *c) { RET(1, DD_OK); }
static void dd_WaitForVerticalBlank(CPU *c) { RET(3, DD_OK); }
static void dd_GetMonitorFrequency(CPU *c) { W32(ARG(1), 60); RET(2, DD_OK); }
static void dd_GetVerticalBlankStatus(CPU *c) { W32(ARG(1), 1); RET(2, DD_OK); }
static void dd_GetFourCCCodes(CPU *c) { W32(ARG(1), 0); RET(3, DD_OK); }
static void dd_RestoreAllSurfaces(CPU *c) { RET(1, DD_OK); }
static void dd_FlipToGDISurface(CPU *c) { RET(1, DD_OK); }
static void dd_GetScanLine(CPU *c) { W32(ARG(1), 0); RET(2, DD_OK); }

static const ComMethod g_dd_methods[] = {
    {"QueryInterface", dd_QueryInterface}, {"AddRef", dd_AddRef}, {"Release", dd_Release}, {"Compact", NULL},
    {"CreateClipper", dd_CreateClipper}, {"CreatePalette", dd_CreatePalette}, {"CreateSurface", dd_CreateSurface},
    {"DuplicateSurface", NULL}, {"EnumDisplayModes", dd_EnumDisplayModes}, {"EnumSurfaces", NULL},
    {"FlipToGDISurface", dd_FlipToGDISurface}, {"GetCaps", dd_GetCaps}, {"GetDisplayMode", dd_GetDisplayMode},
    {"GetFourCCCodes", dd_GetFourCCCodes}, {"GetGDISurface", NULL}, {"GetMonitorFrequency", dd_GetMonitorFrequency},
    {"GetScanLine", dd_GetScanLine}, {"GetVerticalBlankStatus", dd_GetVerticalBlankStatus}, {"Initialize", NULL},
    {"RestoreDisplayMode", dd_RestoreDisplayMode}, {"SetCooperativeLevel", dd_SetCooperativeLevel},
    {"SetDisplayMode", dd_SetDisplayMode}, {"WaitForVerticalBlank", dd_WaitForVerticalBlank},
    {"GetAvailableVidMem", dd_GetAvailableVidMem}, {"GetSurfaceFromDC", NULL},
    {"RestoreAllSurfaces", dd_RestoreAllSurfaces}, {"TestCooperativeLevel", dd_TestCooperativeLevel},
    {"GetDeviceIdentifier", dd_GetDeviceIdentifier}, {"StartModeTest", NULL}, {"EvaluateMode", NULL},
};
static ComIface g_dd_iface = {"IDirectDraw7", 30, g_dd_methods, 0};

/* ------------------------------------------------------------------ IDirect3D7 */
#define IID_RGB "A4665C60-2673-11CF-A31A-00AA00B93356"
#define IID_HAL "84E63DE0-46AA-11CF-816F-0000C020156E"
#define IID_TNL "F5049E78-4861-11D2-A407-00A0C90629A8"
static void prim_caps(uint32_t p) {
    W32(p, 56);
    W32(p + 4, 0x2 | 0x10 | 0x20 | 0x40);
    /* DITHER ZTEST SUBPIXEL SUBPIXELX FOGVERTEX FOGTABLE MIPMAPLODBIAS ZBIAS FOGRANGE WFOG ZFOG */
    W32(p + 8, 0x1 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x2000 | 0x4000 | 0x10000 | 0x100000 | 0x200000);
    W32(p + 12, 0xFF);
    W32(p + 16, 0x1FFF);
    W32(p + 20, 0x1FFF);
    W32(p + 24, 0xFF);
    W32(p + 28, 0x8 | 0x200 | 0x4000 | 0x80000);
    W32(p + 32, 0x1 | 0x4 | 0x8 | 0x40 | 0x80);
    W32(p + 36, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x100 | 0x200 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000);
    W32(p + 40, 0xFF);
    W32(p + 44, 0x1F);
}
void d3d_device_desc(uint32_t p, const char *guid) {
    memset(GPTR(p), 0, 236);
    W32(p, 0x1 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x2000 | 0x8000 | 0x80000 | 0x20000 |
               (strcmp(guid, IID_TNL) ? 0 : 0x10000));
    prim_caps(p + 4);
    prim_caps(p + 60);
    W32(p + 116, 0x400 | 0x100);
    W32(p + 120, 0x400 | 0x200 | 0x100);
    W32(p + 124, 1);
    W32(p + 128, 1);
    W32(p + 132, 4096);
    W32(p + 136, 4096);
    W32(p + 140, 8192);
    W32(p + 144, 4096);
    W32(p + 148, 16);
    float gb[4] = {-8192, -8192, 8192, 8192};
    for (int i = 0; i < 4; i++) { uint32_t v; memcpy(&v, &gb[i], 4); W32(p + 152 + 4 * (uint32_t)i, v); }
    W32(p + 176, 8);
    W32(p + 180, 0x3FFFFFF);
    W16(p + 184, 8);
    W16(p + 186, 8);
    W32(p + 188, 8);
    float mw = 1e10f;
    uint32_t v;
    memcpy(&v, &mw, 4);
    W32(p + 192, v);
    guid_put(p + 196, guid);
}
static void d3_QueryInterface(CPU *c) { W32(ARG(2), ARG(0)); com_addref(ARG(0)); RET(3, DD_OK); }
static void d3_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
static void d3_Release(CPU *c) { RET(1, com_release(ARG(0))); }
static void d3_EnumDevices(CPU *c) { /* (this, cb, ctx) cb(desc, name, devdesc7, ctx) */
    static const char *names[3][2] = {{"RGB Emulation", IID_RGB}, {"Direct3D HAL", IID_HAL}, {"Direct3D T&L HAL", IID_TNL}};
    uint32_t dd = heap_alloc(236, true), s1 = guest_strdup("Metal"), s2;
    for (int i = 0; i < 3; i++) {
        d3d_device_desc(dd, names[i][1]);
        s2 = guest_strdup(names[i][0]);
        uint32_t r = call_guest_ret(c, ARG(1), 4, s1, s2, dd, ARG(2));
        rt_log("EnumDevices callback %08x(%s) -> %u", ARG(1), names[i][0], r);
        if (!r) break;
    }
    heap_free(dd);
    RET(3, DD_OK);
}
static void d3_CreateDevice(CPU *c) { /* (this, clsid, surface, out) */
    Surf *t = surf_of(ARG(2));
    char g[40];
    guid_str(ARG(1), g);
    rt_log("CreateDevice(%s) on %dx%d", g, t ? t->w : 0, t ? t->h : 0);
    if (!t) { RET(4, DDERR_INVALIDPARAMS); return; }
    make_target(t);
    W32(ARG(3), d3d_create_device(t));
    RET(4, DD_OK);
}
static void d3_EnumZBufferFormats(CPU *c) { /* (this, riid, cb, ctx) */
    uint32_t pf = heap_alloc(32, true);
    static const uint32_t depths[3] = {16, 24, 32};
    for (int i = 0; i < 3; i++) {
        memset(GPTR(pf), 0, 32);
        W32(pf, 32);
        W32(pf + 4, DDPF_ZBUFFER);
        W32(pf + 12, depths[i] == 24 ? 32 : depths[i]);
        W32(pf + 20, depths[i] >= 32 ? 0xFFFFFFFFu : (1u << depths[i]) - 1);
        if (!call_guest_ret(c, ARG(2), 2, pf, ARG(3))) break;
    }
    heap_free(pf);
    RET(4, DD_OK);
}
static void d3_EvictManagedTextures(CPU *c) { RET(1, DD_OK); }
static const ComMethod g_d3_methods[] = {
    {"QueryInterface", d3_QueryInterface}, {"AddRef", d3_AddRef}, {"Release", d3_Release},
    {"EnumDevices", d3_EnumDevices}, {"CreateDevice", d3_CreateDevice}, {"CreateVertexBuffer", NULL},
    {"EnumZBufferFormats", d3_EnumZBufferFormats}, {"EvictManagedTextures", d3_EvictManagedTextures},
};
static ComIface g_d3_iface = {"IDirect3D7", 8, g_d3_methods, 0};

static void dd_QueryInterface(CPU *c) {
    char g[40];
    guid_str(ARG(1), g);
    if (guid_eq(ARG(1), "F5049E77-4861-11D2-A407-00A0C90629A8")) { /* IID_IDirect3D7 */
        if (!g_d3d_obj) g_d3d_obj = com_new(&g_d3_iface, NULL, 0);
        com_addref(g_d3d_obj);
        W32(ARG(2), g_d3d_obj);
        RET(3, DD_OK);
        return;
    }
    if (guid_eq(ARG(1), "15E65EC0-3B9C-11D2-B92F-00609797EA5B")) { /* IID_IDirectDraw7 */
        com_addref(ARG(0));
        W32(ARG(2), ARG(0));
        RET(3, DD_OK);
        return;
    }
    rt_log("IDirectDraw7::QueryInterface(%s) -> no", g);
    W32(ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

/* ------------------------------------------------------------------ exports */
void I_ddraw_DirectDrawEnumerateExA(CPU *c) { /* (cb, ctx, flags) cb(guid, desc, name, ctx, hmon) */
    uint32_t desc = guest_strdup("Primary Display Driver"), name = guest_strdup("display");
    call_guest(c, ARG(0), 5, 0, desc, name, ARG(1), 0);
    RET(3, DD_OK);
}
void I_ddraw_DirectDrawCreateEx(CPU *c) { /* (guid, out, iid, outer) */
    r_init();
    if (!g_dd_obj) g_dd_obj = com_new(&g_dd_iface, NULL, 0);
    rt_log("DirectDrawCreateEx");
    W32(ARG(1), g_dd_obj);
    RET(4, DD_OK);
}
