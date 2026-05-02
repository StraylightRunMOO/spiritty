/*
 * test_renderer.c — vtable contract & sp_terminal_render integration.
 *
 * Drives the null renderer through every public hook and asserts its
 * recorded counters match expected behavior.
 */

#include "test_framework.h"
#include "spiritty/spiritty.h"
#include "spiritty/spiritty_renderer.h"

#include <string.h>

/* ---- helpers ---------------------------------------------------------- */

static sp_renderer_opts default_opts(void) {
    sp_renderer_opts o = {
        .framebuffer_width   = 800,
        .framebuffer_height  = 600,
        .device_pixel_ratio  = 1.0f,
        .atlas_size          = 0,
        .premultiplied_alpha = true,
        .antialias           = true,
    };
    return o;
}

/* ---- vtable lifecycle ------------------------------------------------- */

static void renderer_null_create_destroy(void) {
    sp_renderer* r = sp_renderer_null_create();
    ASSERT_TRUE(r != NULL);
    ASSERT_TRUE(r->vt != NULL);
    ASSERT_TRUE(r->self != NULL);
    sp_renderer_null_destroy(r);
}

static void renderer_null_init_records_state(void) {
    sp_renderer* r = sp_renderer_null_create();
    ASSERT_TRUE(!sp_renderer_null_initialized(r));
    sp_renderer_opts o = default_opts();
    ASSERT_TRUE(r->vt->init(r->self, &o));
    ASSERT_TRUE(sp_renderer_null_initialized(r));
    sp_renderer_null_destroy(r);
}

static void renderer_null_init_rejects_null_opts(void) {
    sp_renderer* r = sp_renderer_null_create();
    ASSERT_TRUE(!r->vt->init(r->self, NULL));
    ASSERT_TRUE(!sp_renderer_null_initialized(r));
    sp_renderer_null_destroy(r);
}

static void renderer_null_resize_increments_counter(void) {
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);
    ASSERT_EQ_INT((long long)(0u), (long long)(sp_renderer_null_resizes(r)));
    r->vt->resize(r->self, 1024, 768);
    r->vt->resize(r->self,  640, 480);
    ASSERT_EQ_INT((long long)(2u), (long long)(sp_renderer_null_resizes(r)));
    sp_renderer_null_destroy(r);
}

/* ---- frame ordering --------------------------------------------------- */

static void renderer_null_frame_brackets(void) {
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);

    r->vt->begin_frame(r->self);
    r->vt->end_frame(r->self);
    ASSERT_EQ_INT((long long)(1u), (long long)(sp_renderer_null_begin_frames(r)));
    ASSERT_EQ_INT((long long)(1u), (long long)(sp_renderer_null_end_frames(r)));
    ASSERT_EQ_INT((long long)(0u), (long long)(sp_renderer_null_misordered_draws(r)));

    sp_renderer_null_destroy(r);
}

static void renderer_null_draw_outside_frame_recorded(void) {
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);

    sp_cell_batch b = { 0 };
    r->vt->draw_cells(r->self, &b);
    ASSERT_EQ_INT((long long)(1u), (long long)(sp_renderer_null_misordered_draws(r)));
    ASSERT_EQ_INT((long long)(0u), (long long)(sp_renderer_null_draw_calls(r)));

    sp_renderer_null_destroy(r);
}

/* ---- custom shader hot-swap ------------------------------------------ */

static void renderer_null_custom_shader_set_clear(void) {
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);

    ASSERT_TRUE(sp_renderer_null_last_custom_shader(r) == NULL);

    const char* src = "void mainImage(out vec4 c, in vec2 p){c=vec4(p,0,1);}";
    ASSERT_TRUE(r->vt->set_custom_shader(r->self, src));
    const char* got = sp_renderer_null_last_custom_shader(r);
    ASSERT_TRUE(got != NULL);
    ASSERT_TRUE(strcmp(got, src) == 0);

    /* Replace */
    const char* src2 = "void mainImage(out vec4 c, in vec2 p){c=vec4(1);}";
    ASSERT_TRUE(r->vt->set_custom_shader(r->self, src2));
    ASSERT_TRUE(strcmp(sp_renderer_null_last_custom_shader(r), src2) == 0);

    /* Clear */
    ASSERT_TRUE(r->vt->set_custom_shader(r->self, NULL));
    ASSERT_TRUE(sp_renderer_null_last_custom_shader(r) == NULL);

    sp_renderer_null_destroy(r);
}

/* ---- glyph upload ----------------------------------------------------- */

static void renderer_null_glyph_upload_counts(void) {
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);

    uint8_t pixels[16] = { 0 };
    sp_glyph g = {
        .codepoint = 'A',
        .width_px = 4, .height_px = 4,
        .bearing_x = 0, .bearing_y = 4,
        .advance = 4.0f,
        .pixels = pixels,
    };
    r->vt->upload_glyph(r->self, &g);
    r->vt->upload_glyph(r->self, &g);
    r->vt->upload_glyph(r->self, &g);
    ASSERT_EQ_INT((long long)(3u), (long long)(sp_renderer_null_glyph_uploads(r)));

    sp_renderer_null_destroy(r);
}

/* ---- sp_terminal_render integration ----------------------------------- */

static sp_terminal* make_terminal(int32_t cols, int32_t rows) {
    sp_options opts;
    sp_options_default(&opts);
    opts.cols = cols;
    opts.rows = rows;
    opts.scrollback_lines = 0;
    return sp_terminal_create(&opts);
}

static void terminal_render_with_no_renderer_is_noop(void) {
    sp_terminal* t = make_terminal(10, 4);
    sp_terminal_render(t);  /* must not crash */
    sp_terminal_destroy(t);
}

static void terminal_render_drives_full_vtable(void) {
    sp_terminal* t = make_terminal(20, 5);
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);

    sp_terminal_attach_renderer(t, r);
    sp_terminal_render(t);

    ASSERT_EQ_INT((long long)(1u), (long long)(sp_renderer_null_begin_frames(r)));
    ASSERT_EQ_INT((long long)(1u), (long long)(sp_renderer_null_end_frames(r)));
    ASSERT_EQ_INT((long long)(1u), (long long)(sp_renderer_null_draw_calls(r)));
    ASSERT_EQ_INT((long long)(0u), (long long)(sp_renderer_null_misordered_draws(r)));
    ASSERT_EQ_INT((long long)((size_t)(20 * 5)), (long long)(sp_renderer_null_last_cell_count(r)));

    sp_terminal_destroy(t);
    sp_renderer_null_destroy(r);
}

static void terminal_render_passes_cursor_state(void) {
    sp_terminal* t = make_terminal(8, 3);
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);
    sp_terminal_attach_renderer(t, r);

    sp_renderer_null_capture_cells(r, true);

    /* Move cursor and write some text. */
    sp_terminal_move_cursor(t, 1, 4);
    const char* msg = "hi";
    sp_terminal_write(t, (const uint8_t*)msg, 2);
    sp_terminal_set_time(t, 1.5f);
    sp_terminal_render(t);

    ASSERT_EQ_INT((long long)(1u), (long long)(sp_renderer_null_draw_calls(r)));
    ASSERT_EQ_INT((long long)((size_t)(8 * 3)), (long long)(sp_renderer_null_last_cell_count(r)));

    /* Captured cells should reflect the writes. */
    const sp_cell* cells = sp_renderer_null_captured_cells(r);
    ASSERT_TRUE(cells != NULL);
    /* Row 1, col 4 should now contain 'h' (write advanced from col 4). */
    ASSERT_EQ_INT((long long)((uint32_t)'h'), (long long)(cells[1 * 8 + 4].codepoint));
    ASSERT_EQ_INT((long long)((uint32_t)'i'), (long long)(cells[1 * 8 + 5].codepoint));

    sp_terminal_destroy(t);
    sp_renderer_null_destroy(r);
}

static void terminal_set_custom_shader_forwards_to_renderer(void) {
    sp_terminal* t = make_terminal(10, 3);
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);
    sp_terminal_attach_renderer(t, r);

    const char* src = "void mainImage(out vec4 c, in vec2 p){c=vec4(0);}";
    ASSERT_TRUE(sp_terminal_set_custom_shader(t, src));
    ASSERT_TRUE(sp_renderer_null_last_custom_shader(r) != NULL);
    ASSERT_TRUE(strcmp(sp_renderer_null_last_custom_shader(r), src) == 0);

    sp_terminal_destroy(t);
    sp_renderer_null_destroy(r);
}

static void terminal_render_after_resize_passes_new_dims(void) {
    sp_terminal* t = make_terminal(10, 4);
    sp_renderer* r = sp_renderer_null_create();
    sp_renderer_opts o = default_opts();
    r->vt->init(r->self, &o);
    sp_terminal_attach_renderer(t, r);

    sp_terminal_render(t);
    ASSERT_EQ_INT((long long)((size_t)(10 * 4)), (long long)(sp_renderer_null_last_cell_count(r)));

    sp_terminal_resize(t, 16, 6);
    sp_terminal_render(t);
    ASSERT_EQ_INT((long long)((size_t)(16 * 6)), (long long)(sp_renderer_null_last_cell_count(r)));
    ASSERT_EQ_INT((long long)(2u), (long long)(sp_renderer_null_draw_calls(r)));

    sp_terminal_destroy(t);
    sp_renderer_null_destroy(r);
}

/* ---- registration ----------------------------------------------------- */

SP_TEST(renderer, null_create_destroy)              { renderer_null_create_destroy(); }
SP_TEST(renderer, null_init_records_state)          { renderer_null_init_records_state(); }
SP_TEST(renderer, null_init_rejects_null_opts)      { renderer_null_init_rejects_null_opts(); }
SP_TEST(renderer, null_resize_increments_counter)   { renderer_null_resize_increments_counter(); }
SP_TEST(renderer, null_frame_brackets)              { renderer_null_frame_brackets(); }
SP_TEST(renderer, null_draw_outside_frame_recorded) { renderer_null_draw_outside_frame_recorded(); }
SP_TEST(renderer, null_custom_shader_set_clear)     { renderer_null_custom_shader_set_clear(); }
SP_TEST(renderer, null_glyph_upload_counts)         { renderer_null_glyph_upload_counts(); }
SP_TEST(renderer, term_render_no_renderer_noop)     { terminal_render_with_no_renderer_is_noop(); }
SP_TEST(renderer, term_render_drives_full_vtable)   { terminal_render_drives_full_vtable(); }
SP_TEST(renderer, term_render_passes_cursor_state)  { terminal_render_passes_cursor_state(); }
SP_TEST(renderer, term_set_custom_shader_forwards)  { terminal_set_custom_shader_forwards_to_renderer(); }
SP_TEST(renderer, term_render_after_resize)         { terminal_render_after_resize_passes_new_dims(); }
