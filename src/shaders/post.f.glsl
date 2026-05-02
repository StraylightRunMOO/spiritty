// Default identity post-process. The Shadertoy-compat prefix is
// prepended to user-supplied custom shaders before compilation.

in  vec2 v_uv;
out vec4 out_color;

uniform sampler2D iChannel0;

void main() {
    out_color = texture(iChannel0, v_uv);
}
