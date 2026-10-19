/* DirectDraw 7 surfaces shared by ddraw.c and d3d.c */
#pragma once
#include "com.h"
#include "render.h"

typedef struct Surf {
    uint32_t obj;                 /* guest COM object */
    int w, h;                     /* logical size */
    uint32_t caps, caps2;
    int bpp;
    uint32_t pf_flags, rmask, gmask, bmask, amask, zbits;
    uint32_t pitch, mem;          /* guest pixel buffer (what Lock returns) */
    bool has_ck;
    uint32_t ck_lo, ck_hi;        /* source colour key, in surface format */
    struct Surf *back, *zbuf, *mip_next, *front;
    uint32_t palette;             /* IDirectDrawPalette object */
    RTex *tex;                    /* GPU copy for texturing */
    bool tex_dirty;
    RTarget *rt;                  /* render target for primary/back buffer/3D surfaces */
    bool cpu_newer;               /* guest pixels newer than the GPU target */
    uint32_t *lock_copy;          /* snapshot taken at Lock of a render target */
    int locks;
    uint32_t *overlay;            /* GDI layer for render targets (pixel size, premultiplied BGRA) */
    int ov_w, ov_h;
    bool ov_dirty;
    uint32_t dc;
} Surf;

Surf *surf_of(uint32_t obj);
void surf_sync_texture(Surf *s);            /* upload guest pixels if they changed */
void surf_flush_overlay(Surf *s);           /* composite pending GDI drawing into the target */
uint32_t surf_pixel_to_bgra(Surf *s, uint32_t v, bool *keyed);
void ddraw_get_display(int *w, int *h, int *bpp);
uint32_t d3d_create_device(Surf *target);
int ddraw_dump_textures(void);   /* writes every live texture to the HD dump; returns how many */
