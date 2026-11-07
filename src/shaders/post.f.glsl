// Default identity post-process. The Shadertoy-compat prefix is
// prepended to user-supplied custom shaders before compilation.

in  vec2 v_uv;
out vec4 out_color;

uniform sampler2D iChannel0;

void main() {
    /* Identity blit at 1:1. texelFetch ignores the FBO filter so a
     * LINEAR scene texture (kept for custom Shadertoy passes) cannot
     * smear rasterized glyphs. v_uv is for those custom shaders. */
    out_color = texelFetch(iChannel0, ivec2(gl_FragCoord.xy), 0);
    out_color += vec4(v_uv, v_uv) * 0.0;
}
