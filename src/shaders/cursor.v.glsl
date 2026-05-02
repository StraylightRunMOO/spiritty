// Cursor vertex shader. Single quad. Style 0=block, 1=underline, 2=bar,
// 3=hollow block (rendered as a 4-strip outline by the host issuing 4
// thin rect draws — this shader handles a single rect at a time).
//
// To draw all four cursor styles we keep this shader minimal: the host
// uniformly sets cell_size_px-relative offset/size for the rect we're
// drawing, and we just transform.

uniform vec2 u_offset_px;   // top-left within cell (in cell coords)
uniform vec2 u_size_px;     // rect size

out vec4 v_color;

void main() {
    int vid = gl_VertexID;
    vec2 corner = vec2(float(vid == 1 || vid == 3),
                       float(vid == 2 || vid == 3));

    vec2 cell_origin = G.grid_padding + G.cursor_xy * G.cell_size_px;
    vec2 corner_px = cell_origin + u_offset_px + corner * u_size_px;

    gl_Position = G.projection * vec4(corner_px, 0.0, 1.0);
    v_color = prepare_color(G.cursor_color);
}
