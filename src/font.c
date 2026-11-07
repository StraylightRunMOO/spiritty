#define SPIRITTY_BUILDING 1

#include "font.h"
#include "builtin_ttf.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct sp_font {
    stbtt_fontinfo info;
    const uint8_t* ttf;
    size_t         ttf_n;
    uint8_t*       owned;
    float          pixel_height;
    float          scale;
    int            ascent;     /* unscaled */
    int            descent;    /* unscaled, typically negative */
    int            line_gap;   /* unscaled */
    float          cell_w;
    float          cell_h;
    float          baseline;
};

static sp_font* font_from_bytes(const uint8_t* ttf, size_t n, float pixel_height, uint8_t* owned) {
    if (!ttf || n < 100 || pixel_height < 1.0f) {
        free(owned);
        return NULL;
    }
    sp_font* f = (sp_font*)calloc(1, sizeof(sp_font));
    if (!f) {
        free(owned);
        return NULL;
    }
    f->ttf = ttf;
    f->ttf_n = n;
    f->owned = owned;
    f->pixel_height = pixel_height;
    if (!stbtt_InitFont(&f->info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        sp_font_destroy(f);
        return NULL;
    }
    stbtt_GetFontVMetrics(&f->info, &f->ascent, &f->descent, &f->line_gap);
    /* CSS/Pillow font-size maps the em square, not the tight body. */
    f->scale = stbtt_ScaleForMappingEmToPixels(&f->info, pixel_height);

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->info, 'M', &adv, &lsb);
    if (adv <= 0)
        stbtt_GetCodepointHMetrics(&f->info, '0', &adv, &lsb);
    f->cell_w = roundf((float)adv * f->scale);
    if (f->cell_w < 1.0f) f->cell_w = 1.0f;

    /* 1.25em line box (matches the old 16px → 20px bake). Descent sits
     * in the leftover space so g/y/p stay inside the cell. Integer
     * baseline so 1:1 atlas samples land on pixel centres. */
    f->cell_h = ceilf(pixel_height * 1.25f);
    if (f->cell_h < 1.0f) f->cell_h = 1.0f;
    float desc = ceilf(fabsf((float)f->descent * f->scale));
    f->baseline = roundf(f->cell_h - desc);
    if (f->baseline < 1.0f) f->baseline = roundf(f->cell_h * 0.8f);
    return f;
}

sp_font* sp_font_create_from_memory(const uint8_t* ttf, size_t n, float pixel_height) {
    if (!ttf || n == 0) return NULL;
    uint8_t* copy = (uint8_t*)malloc(n);
    if (!copy) return NULL;
    memcpy(copy, ttf, n);
    return font_from_bytes(copy, n, pixel_height, copy);
}

sp_font* sp_font_create_builtin(float pixel_height) {
    return font_from_bytes(sp_builtin_ttf, sp_builtin_ttf_size, pixel_height, NULL);
}

bool sp_font_measure_builtin(float pixel_height, float* cell_w, float* cell_h) {
    sp_font* f = sp_font_create_builtin(pixel_height);
    if (!f) return false;
    sp_font_cell_metrics(f, cell_w, cell_h, NULL);
    sp_font_destroy(f);
    return true;
}

void sp_font_destroy(sp_font* f) {
    if (!f) return;
    free(f->owned);
    free(f);
}

void sp_font_cell_metrics(const sp_font* f, float* cell_w, float* cell_h, float* baseline) {
    if (!f) return;
    if (cell_w)   *cell_w   = f->cell_w;
    if (cell_h)   *cell_h   = f->cell_h;
    if (baseline) *baseline = f->baseline;
}

/* 1:1 coverage raster. The atlas is sampled NEAREST at framebuffer
 * resolution, so a 2× box downsample only fattens stems into gray
 * halos. stb already anti-aliases at the requested pixel size. */
bool sp_font_rasterize(sp_font* f, uint32_t codepoint, sp_glyph* out) {
    if (!f || !out || codepoint == 0) return false;
    memset(out, 0, sizeof(*out));

    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* src = stbtt_GetCodepointBitmap(
        &f->info, f->scale, f->scale, (int)codepoint,
        &w, &h, &xoff, &yoff);
    if (!src || w <= 0 || h <= 0) {
        if (src) stbtt_FreeBitmap(src, NULL);
        return false;
    }

    uint8_t* bmp = (uint8_t*)malloc((size_t)w * (size_t)h);
    if (!bmp) {
        stbtt_FreeBitmap(src, NULL);
        return false;
    }
    memcpy(bmp, src, (size_t)w * (size_t)h);
    stbtt_FreeBitmap(src, NULL);

    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->info, (int)codepoint, &adv, &lsb);

    out->codepoint  = codepoint;
    out->width_px   = (uint16_t)w;
    out->height_px  = (uint16_t)h;
    out->bearing_x  = (int16_t)xoff;
    out->bearing_y  = (int16_t)(lroundf(f->baseline) + yoff);
    out->advance    = (float)adv * f->scale;
    out->pixels     = bmp;
    return true;
}

void sp_font_free_glyph(sp_glyph* g) {
    if (!g || !g->pixels) return;
    free((void*)(uintptr_t)g->pixels);
    g->pixels = NULL;
}

bool sp_font_make_box_glyph(const sp_font* f, sp_glyph* out) {
    if (!f || !out) return false;
    int w = (int)f->cell_w;
    int h = (int)f->cell_h;
    if (w < 3) w = 3;
    if (h < 5) h = 5;
    uint8_t* px = (uint8_t*)calloc((size_t)w * (size_t)h, 1);
    if (!px) return false;
    for (int x = 1; x < w - 1; ++x) {
        px[1 * w + x] = 200;
        px[(h - 3) * w + x] = 200;
    }
    for (int y = 1; y < h - 2; ++y) {
        px[y * w + 1] = 200;
        px[y * w + (w - 2)] = 200;
    }
    memset(out, 0, sizeof(*out));
    out->codepoint = 0;
    out->width_px  = (uint16_t)w;
    out->height_px = (uint16_t)h;
    out->bearing_x = 0;
    out->bearing_y = 0;
    out->advance   = f->cell_w;
    out->pixels    = px;
    return true;
}

bool sp_font_seed_ascii(sp_font* f, sp_atlas* a) {
    if (!f || !a) return false;
    sp_glyph box;
    if (sp_font_make_box_glyph(f, &box)) {
        (void)sp_atlas_pack(a, &box);
        sp_font_free_glyph(&box);
    }
    for (uint32_t cp = 32; cp <= 126; ++cp) {
        sp_glyph g;
        if (!sp_font_rasterize(f, cp, &g)) continue;
        (void)sp_atlas_pack(a, &g);
        sp_font_free_glyph(&g);
    }
    return sp_atlas_get(a, 0) != NULL;
}
