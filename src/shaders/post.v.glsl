// Fullscreen-triangle post-process vertex shader.
//
// The scene FBO is rendered with the terminal's own +Y-down ortho
// matrix, so a scene pixel at y=0 lands at NDC y=+1 — i.e. at the TOP
// of the FBO texture, which is memory row (h-1), which samples at
// texture v=1. Preserve that orientation when blitting to the default
// framebuffer: NDC y=+1 (screen top) must map to v=1 (FBO top). That
// means a straight NDC→UV remap with NO Y-flip. An earlier version of
// this shader flipped Y (as you would for a loaded image with image-
// native top-left origin) and rendered the whole terminal upside down.
out vec2 v_uv;

void main() {
    vec2 p;
    p.x = (gl_VertexID == 2) ?  3.0 : -1.0;
    p.y = (gl_VertexID == 0) ? -3.0 :  1.0;
    v_uv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
