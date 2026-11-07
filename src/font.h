/*
 * font.h — TrueType rasterizer (stb_truetype) used by the GL/WebGL
 * text pass. Internal; not installed.
 */
#ifndef SPIRITTY_FONT_H
#define SPIRITTY_FONT_H

#include "spiritty/spiritty_renderer.h"
#include "atlas.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sp_font sp_font;

/* pixel_height is the distance from the highest ascender to the lowest
 * descender in framebuffer pixels (font_size * devicePixelRatio). */
sp_font* sp_font_create_from_memory(const uint8_t* ttf, size_t n, float pixel_height);
sp_font* sp_font_create_builtin(float pixel_height);
void     sp_font_destroy(sp_font* f);
bool     sp_font_measure_builtin(float pixel_height, float* cell_w, float* cell_h);

void sp_font_cell_metrics(const sp_font* f, float* cell_w, float* cell_h, float* baseline);

/* Rasterize one codepoint 1:1 at the current pixel size. out->pixels
 * is heap memory; free with sp_font_free_glyph. Returns false if the
 * font has no outline. */
bool sp_font_rasterize(sp_font* f, uint32_t codepoint, sp_glyph* out);
void sp_font_free_glyph(sp_glyph* g);

/* Hollow-box replacement glyph at cell size. out->pixels is heap memory. */
bool sp_font_make_box_glyph(const sp_font* f, sp_glyph* out);

/* Pack U+0000 fallback + ASCII 32..126 into the atlas. */
bool sp_font_seed_ascii(sp_font* f, sp_atlas* a);

#ifdef __cplusplus
}
#endif

#endif /* SPIRITTY_FONT_H */
