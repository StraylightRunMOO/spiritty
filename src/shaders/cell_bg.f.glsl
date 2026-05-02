// Cell background fragment shader. Output is premultiplied in the
// active working space (linear or sRGB depending on specialization).

in  vec4 v_color;
out vec4 out_color;

void main() {
    out_color = v_color;
}
