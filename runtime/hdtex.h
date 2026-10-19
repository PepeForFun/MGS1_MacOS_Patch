/* HD texture replacements: dump world textures by content key, load upscaled ones (hdtex.m) */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void hdtex_init(void);
uint64_t hdtex_key(const uint32_t *bgra, int w, int h, int stride_px);
bool hdtex_dumping(void);
bool hdtex_has(uint64_t key);
void hdtex_dump(uint64_t key, const uint32_t *bgra, int w, int h);   /* bgra: w*h, not premultiplied */
/* the replacement for this texture, premultiplied BGRA of *hw x *hh (malloc), or NULL */
uint32_t *hdtex_load(uint64_t key, const uint32_t *orig, int w, int h, int stride_px, int *hw, int *hh);
