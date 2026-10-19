/* Metal renderer used by the DirectDraw/Direct3D layer (render.m). Render targets are kept at
 * the window's pixel resolution while the game works in its own (logical) resolution. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct RTex RTex;
typedef struct RTarget RTarget;

typedef struct {
    float x, y, z, w;        /* clip space */
    uint32_t diffuse;        /* D3DCOLOR (A8R8G8B8) */
    uint32_t specular;
    float u, v;
} RVertex;

enum { R_BLEND_ZERO = 1, R_BLEND_ONE, R_BLEND_SRCCOLOR, R_BLEND_INVSRCCOLOR, R_BLEND_SRCALPHA, R_BLEND_INVSRCALPHA,
       R_BLEND_DESTALPHA, R_BLEND_INVDESTALPHA, R_BLEND_DESTCOLOR, R_BLEND_INVDESTCOLOR, R_BLEND_SRCALPHASAT };
enum { R_TOP_DISABLE = 1, R_TOP_SELECTARG1, R_TOP_SELECTARG2, R_TOP_MODULATE, R_TOP_MODULATE2X, R_TOP_MODULATE4X,
       R_TOP_ADD, R_TOP_ADDSIGNED };
enum { R_ARG_DIFFUSE = 0, R_ARG_CURRENT = 1, R_ARG_TEXTURE = 2, R_ARG_TFACTOR = 3, R_ARG_SPECULAR = 4 };

typedef struct {
    bool blend, alpha_test, ztest, zwrite, fog, colorkey, filter, specular;
    int src_blend, dst_blend;
    int zfunc, alpha_func;   /* D3DCMP_* (1..8) */
    float alpha_ref;         /* 0..1 */
    int cull;                /* 1 none, 2 cw, 3 ccw */
    int color_op, color_arg1, color_arg2, alpha_op, alpha_arg1, alpha_arg2;
    uint32_t fog_color;
    uint32_t tfactor;
    bool clamp_u, clamp_v;
    bool xbr;                /* edge-directed pixel-art enlargement (2D draws) */
    bool smooth;             /* bicubic magnification (3D world textures) */
    bool world;              /* a 3D world draw (not flat 2D art): HD texture replacements apply */
} RState;

void r_init(void);
RTex *r_tex_create(int w, int h);
void r_tex_upload(RTex *t, const uint32_t *bgra, int stride_px);  /* whole texture, BGRA premultiplied-free */
void r_tex_destroy(RTex *t);
void r_tex_set_art(RTex *t, bool pixel_art);   /* few colours: worth the xBR pixel-art filter */
bool r_tex_animated(const RTex *t);   /* content keeps changing: no HD replacement, not dumped */

RTarget *r_target_create(int lw, int lh);
void r_target_resize(RTarget *t, int lw, int lh);
void r_target_destroy(RTarget *t);
void r_target_size(RTarget *t, int *pw, int *ph);                  /* pixel size */
void r_clear(RTarget *t, bool color, uint32_t argb, bool depth, float z, int x0, int y0, int x1, int y1);
void r_draw(RTarget *t, const RState *s, RTex *tex, const RVertex *v, int nv, const uint16_t *idx, int ni,
            int vp_x, int vp_y, int vp_w, int vp_h);
/* copy src (texture of a logical w x h image) into a logical rectangle of the target */
void r_blit(RTarget *t, RTex *src, float sx0, float sy0, float sx1, float sy1, int dx0, int dy0, int dx1, int dy1,
            bool keyed, bool filter);
/* copy a logical rectangle of one target (possibly the same one) to another */
void r_copy_target(RTarget *dst, RTarget *src, float sx0, float sy0, float sx1, float sy1, int dx0, int dy0, int dx1,
                   int dy1);
void r_overlay_draw(RTarget *t, const uint32_t *bgra_premul, int pw, int ph);  /* composite a CPU layer now */
void r_readback(RTarget *t, uint32_t *bgra, int lw, int lh);                    /* downsampled to logical size */
void r_present(RTarget *t);
void r_set_gamma(const uint16_t ramp[768]);   /* DDGAMMARAMP: red[256], green[256], blue[256] */
void r_flush(void);
