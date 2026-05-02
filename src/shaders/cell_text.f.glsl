// Instanced cell-text fragment shader.
//
// Compiled in three flavors via specialization:
//   - default:                  raster grayscale atlas (R8), one-tap
//   - SPIRITTY_USE_MSDF:        multi-channel SDF, scalable, no zoom thrash
//   - SPIRITTY_HAS_COLOR_GLYPHS bit-flag at runtime: color atlas (RGBA8)
//
// SPIRITTY_USE_LINEAR_CORRECTION (raster path only) reverse-engineers an
// alpha that yields the same perceptual stroke weight as gamma-incorrect
// blending while keeping the framebuffer in linear space.

in  vec2 v_atlas_uv;
in  vec4 v_fg;
in  vec4 v_bg;
flat in uint v_flags;

uniform sampler2D u_atlas;
#ifdef SPIRITTY_HAS_COLOR_GLYPHS
uniform sampler2D u_atlas_color;
#endif

out vec4 out_color;

#ifdef SPIRITTY_USE_MSDF
uniform float u_msdf_px_range;  // pixel range used at atlas generation

float median(float a, float b, float c) {
    return max(min(a, b), min(max(a, b), c));
}
float msdf_alpha() {
    vec3 s = texture(u_atlas, v_atlas_uv).rgb;
    float sd = median(s.r, s.g, s.b);
    // Screen-space derivative gives anti-aliasing width regardless of zoom.
    vec2 unit = u_msdf_px_range / vec2(textureSize(u_atlas, 0));
    float screen_px_dist = max(0.5 / dot(unit, fwidth(v_atlas_uv)), 1.0);
    return clamp((sd - 0.5) * screen_px_dist + 0.5, 0.0, 1.0);
}
#endif

void main() {
#ifdef SPIRITTY_HAS_COLOR_GLYPHS
    if ((v_flags & 2u) != 0u) {
        vec4 c = texture(u_atlas_color, v_atlas_uv);  // assumed premul linear
      #ifndef SPIRITTY_USE_LINEAR_BLENDING
        if (c.a > 0.0) {
            c.rgb /= c.a;
            c = unlinearize(c);
            c.rgb *= c.a;
        }
      #endif
        out_color = c;
        return;
    }
#endif

#ifdef SPIRITTY_USE_MSDF
    float a = msdf_alpha();
#else
    float a = texture(u_atlas, v_atlas_uv).r;
#endif

    vec4 fg = v_fg;

#if defined(SPIRITTY_USE_LINEAR_BLENDING) && defined(SPIRITTY_USE_LINEAR_CORRECTION) && !defined(SPIRITTY_USE_MSDF)
    // Lifted from Ghostty: solve for the alpha that, under linear blending,
    // produces the luminance gamma-incorrect blending would have yielded.
    float fg_l = luminance(fg.rgb);
    float bg_l = luminance(v_bg.rgb);
    if (abs(fg_l - bg_l) > 0.001) {
        float blend_l = linearize1(unlinearize1(fg_l) * a +
                                   unlinearize1(bg_l) * (1.0 - a));
        a = clamp((blend_l - bg_l) / (fg_l - bg_l), 0.0, 1.0);
    }
#endif

#ifndef SPIRITTY_USE_LINEAR_BLENDING
    if (fg.a > 0.0) {
        fg.rgb /= fg.a;
        fg = unlinearize(fg);
        fg.rgb *= fg.a;
    }
#endif

    out_color = fg * a;
}
