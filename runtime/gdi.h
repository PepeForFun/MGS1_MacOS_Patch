/* GDI drawing targets shared between gdi32.c and the DirectDraw surfaces. */
#pragma once
#include "rt.h"

typedef struct {
    uint32_t *pixels;   /* BGRA, premultiplied, host memory */
    int w, h;           /* logical size (what the game believes) */
    int pw, ph;         /* pixel size (w*scale, h*scale) */
    int stride;         /* bytes per row */
    float scale;        /* pixels per logical unit */
    bool own;           /* pixels were allocated by gdi32 */
    /* render-target DCs: pixels only hold what GDI drew since GetDC; fill(fill_ctx, out) writes the
     * target's own picture (pw x ph, opaque) for blits that read from the DC */
    void (*fill)(void *ctx, uint32_t *out);
    void *fill_ctx;
} GdiTarget;

/* A DC that draws into t. release(ctx) runs when the game releases the DC. */
uint32_t gdi_dc_for_target(GdiTarget *t, void (*release)(void *), void *ctx);
void gdi_release_dc(uint32_t hdc);
uint32_t gdi_load_bitmap_file(const char *host_path);
uint32_t gdi_load_bitmap_resource(uint32_t id);
