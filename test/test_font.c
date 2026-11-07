#include "test_framework.h"
#include "font.h"
#include "atlas.h"

#include <string.h>

SP_TEST(font, builtin_metrics) {
    float w = 0, h = 0;
    ASSERT_TRUE(sp_font_measure_builtin(16.0f, &w, &h));
    ASSERT_TRUE(w >= 8.0f && w <= 12.0f);
    ASSERT_TRUE(h >= 18.0f && h <= 22.0f);
}

SP_TEST(font, rasterize_ascii) {
    sp_font* f = sp_font_create_builtin(16.0f);
    ASSERT_TRUE(f != NULL);
    sp_glyph g;
    ASSERT_TRUE(sp_font_rasterize(f, (uint32_t)'A', &g));
    ASSERT_TRUE(g.width_px > 0 && g.height_px > 0);
    ASSERT_TRUE(g.pixels != NULL);
    int ink = 0;
    size_t n = (size_t)g.width_px * (size_t)g.height_px;
    for (size_t i = 0; i < n; ++i) if (g.pixels[i]) ++ink;
    ASSERT_TRUE(ink > 8);
    sp_font_free_glyph(&g);
    sp_font_destroy(f);
}

SP_TEST(font, atlas_pack_ascii) {
    sp_font* f = sp_font_create_builtin(16.0f);
    sp_atlas* a = sp_atlas_create(512, 512);
    ASSERT_TRUE(f && a);
    ASSERT_TRUE(sp_font_seed_ascii(f, a));
    ASSERT_TRUE(sp_atlas_get(a, 0) != NULL);
    ASSERT_TRUE(sp_atlas_get(a, (uint32_t)'M') != NULL);
    ASSERT_TRUE(sp_atlas_get(a, (uint32_t)'~') != NULL);
    ASSERT_TRUE(sp_atlas_get(a, 0x1F600u) == NULL);
    const sp_atlas_slot* m = sp_atlas_get(a, (uint32_t)'M');
    ASSERT_TRUE(m->w > 0 && m->h > 0);
    sp_atlas_destroy(a);
    sp_font_destroy(f);
}

SP_TEST(font, load_memory_roundtrip) {
    /* Builtin create_from_memory should reject junk. */
    static const uint8_t junk[16] = { 0 };
    ASSERT_TRUE(sp_font_create_from_memory(junk, sizeof junk, 16.0f) == NULL);
}

SP_TEST(font, descenders_fit_cell) {
    sp_font* f = sp_font_create_builtin(16.0f);
    ASSERT_TRUE(f != NULL);
    float cw = 0, ch = 0, bl = 0;
    sp_font_cell_metrics(f, &cw, &ch, &bl);
    ASSERT_TRUE(ch >= 18.0f);
    const char* sample = "gypqQMj";
    for (const char* p = sample; *p; ++p) {
        sp_glyph g;
        ASSERT_TRUE(sp_font_rasterize(f, (uint32_t)(unsigned char)*p, &g));
        ASSERT_TRUE((float)g.bearing_y + (float)g.height_px <= ch + 1.5f);
        ASSERT_TRUE((float)g.bearing_x + (float)g.width_px <= cw + 3.0f);
        sp_font_free_glyph(&g);
    }
    sp_font_destroy(f);
}
