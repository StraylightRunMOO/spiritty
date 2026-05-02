// Instanced cell-background vertex shader.
//
// Geometry: one triangle strip of 4 vertices, drawn `n_cells` times.
// gl_VertexID picks the corner (0=TL, 1=TR, 2=BL, 3=BR). Per-instance
// attributes carry the cell's grid position and color. No per-fragment
// grid math, no SSBO, perfectly coalesced.

layout(location = 0) in vec2 a_grid_pos;   // col, row (per-instance)
layout(location = 1) in vec4 a_color;      // sRGB straight alpha (per-instance)

out vec4 v_color;

void main() {
    int vid = gl_VertexID;
    vec2 corner = vec2(float(vid == 1 || vid == 3),
                       float(vid == 2 || vid == 3));

    vec2 origin_px = G.grid_padding + a_grid_pos * G.cell_size_px;
    vec2 corner_px = origin_px + corner * G.cell_size_px;

    gl_Position = G.projection * vec4(corner_px, 0.0, 1.0);
    v_color = prepare_color(a_color);
}
