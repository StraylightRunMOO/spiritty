// Fullscreen-triangle post-process vertex shader.
out vec2 v_uv;

void main() {
    vec2 p;
    p.x = (gl_VertexID == 2) ?  3.0 : -1.0;
    p.y = (gl_VertexID == 0) ? -3.0 :  1.0;
    v_uv = (p * vec2(0.5, -0.5)) + vec2(0.5, 0.5);
    gl_Position = vec4(p, 0.0, 1.0);
}
