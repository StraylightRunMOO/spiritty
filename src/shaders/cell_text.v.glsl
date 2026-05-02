// Instanced cell-text vertex shader.
//
// One triangle strip of 4 vertices, drawn `n_glyphs` times. Per-instance
// attributes carry glyph metrics, atlas position, and FG/BG colors.
//
// Atlas representation depends on specialization:
//   SPIRITTY_USE_MSDF   - a_atlas_xy/wh point into an RGB MSDF atlas
//   (default)           - a_atlas_xy/wh point into an R8 raster mask
//   SPIRITTY_HAS_COLOR_GLYPHS + a_flags bit 1 set - color emoji path

layout(location = 0) in vec2 a_grid_pos;     // col, row
layout(location = 1) in vec2 a_glyph_size;   // px (in cell coords)
layout(location = 2) in vec2 a_glyph_bearing;// px (left, top from cell origin)
layout(location = 3) in vec2 a_atlas_xy;     // px in atlas
layout(location = 4) in vec2 a_atlas_wh;     // px in atlas
layout(location = 5) in vec4 a_fg;           // sRGB straight alpha
layout(location = 6) in vec4 a_bg;           // sRGB straight alpha (cell BG, for contrast)
layout(location = 7) in uint a_flags;        // bit0=no_min_contrast, bit1=color_glyph

out vec2 v_atlas_uv;
out vec4 v_fg;
out vec4 v_bg;
flat out uint v_flags;

uniform vec2 u_atlas_size_px;

void main() {
    int vid = gl_VertexID;
    vec2 corner = vec2(float(vid == 1 || vid == 3),
                       float(vid == 2 || vid == 3));

    vec2 cell_origin = G.grid_padding + a_grid_pos * G.cell_size_px;
    vec2 glyph_origin = cell_origin + a_glyph_bearing;
    vec2 corner_px = glyph_origin + corner * a_glyph_size;

    gl_Position = G.projection * vec4(corner_px, 0.0, 1.0);

    vec2 atlas_uv_px = a_atlas_xy + corner * a_atlas_wh;
    v_atlas_uv = atlas_uv_px / u_atlas_size_px;

    v_fg = prepare_color(a_fg);
    v_bg = prepare_color(a_bg);

#ifdef SPIRITTY_USE_LINEAR_BLENDING
    if (G.min_contrast > 1.0 && (a_flags & 1u) == 0u) {
        v_fg = contrasted(G.min_contrast, v_fg, v_bg);
    }
#endif

    v_flags = a_flags;
}
