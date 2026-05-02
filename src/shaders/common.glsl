// Spiritty shared shader header. Included by every other shader at the
// top, after the host-injected `#version` line and specialization
// `#define` block.
//
// Specialization defines (set by host before compile, never branched at
// runtime):
//   SPIRITTY_USE_LINEAR_BLENDING   - fb is linear; skip in-shader gamma
//   SPIRITTY_USE_LINEAR_CORRECTION - alpha-correct grayscale glyphs to
//                                    match gamma-incorrect perceptual weight
//   SPIRITTY_USE_DISPLAY_P3        - render in Display-P3 primaries
//   SPIRITTY_USE_MSDF              - text path samples MSDF, not raster mask
//   SPIRITTY_HAS_COLOR_GLYPHS      - compile the color-atlas branch
//
// All public globals live in one std140 UBO at binding 0. Per-pass storage
// (atlases, scene texture) takes binding 1+.

layout(std140) uniform Globals {
    mat4 projection;
    vec2 screen_size_px;
    vec2 cell_size_px;
    vec2 grid_size;            // cols, rows (as float for direct math)
    vec2 grid_padding;         // x = left, y = top
    vec4 default_bg;           // sRGB-encoded; linearize on use
    vec4 default_fg;
    vec4 cursor_color;
    vec2 cursor_xy;            // grid-space col, row
    int  cursor_style;         // 0 block, 1 underline, 2 bar, 3 hollow block
    int  cursor_visible;       // 0/1
    float min_contrast;        // 1.0 disables
    float pad_a_;
    float pad_b_;
    float pad_c_;
} G;

//----------------------------------------------------------------------//
// Color space
//----------------------------------------------------------------------//

float linearize1(float v) {
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}
vec3 linearize(vec3 c) {
    return vec3(linearize1(c.r), linearize1(c.g), linearize1(c.b));
}
vec4 linearize(vec4 c) { return vec4(linearize(c.rgb), c.a); }

float unlinearize1(float v) {
    return v <= 0.0031308 ? v * 12.92 : pow(v, 1.0 / 2.4) * 1.055 - 0.055;
}
vec3 unlinearize(vec3 c) {
    return vec3(unlinearize1(c.r), unlinearize1(c.g), unlinearize1(c.b));
}
vec4 unlinearize(vec4 c) { return vec4(unlinearize(c.rgb), c.a); }

#ifdef SPIRITTY_USE_DISPLAY_P3
// sRGB-linear -> Display-P3 linear (Bradford-adapted, D65->D65).
// Matrix from W3C CSS Color 4 (https://www.w3.org/TR/css-color-4/#color-conversion-code).
const mat3 SRGB_TO_P3 = mat3(
    0.8224621,  0.0331941, 0.0170827,
    0.1775380,  0.9668058, 0.0723974,
    0.0000000, -0.0000001, 0.9105199
);
vec3 to_display_p3_linear(vec3 srgb_linear) { return SRGB_TO_P3 * srgb_linear; }
#endif

// Single entry point used by every other shader. Takes a color as
// uploaded by the host (sRGB-encoded, straight alpha) and returns the
// color in the framebuffer's working space, premultiplied.
//
// Specialization eliminates the gamma branches per program variant.
vec4 prepare_color(vec4 srgb) {
#ifdef SPIRITTY_USE_LINEAR_BLENDING
    vec4 c = linearize(srgb);
  #ifdef SPIRITTY_USE_DISPLAY_P3
    c.rgb = to_display_p3_linear(c.rgb);
  #endif
#else
    vec4 c = srgb;
#endif
    c.rgb *= c.a;
    return c;
}

//----------------------------------------------------------------------//
// Contrast (WCAG)
//----------------------------------------------------------------------//

float luminance(vec3 linear_rgb) {
    return dot(linear_rgb, vec3(0.2126, 0.7152, 0.0722));
}

float contrast_ratio(vec3 a, vec3 b) {
    float la = luminance(a) + 0.05;
    float lb = luminance(b) + 0.05;
    return max(la, lb) / min(la, lb);
}

// Snap fg to white or black if it fails the WCAG ratio. Caller passes
// premultiplied colors in linear space.
vec4 contrasted(float min_ratio, vec4 fg, vec4 bg) {
    vec3 fg_un = fg.a > 0.0 ? fg.rgb / fg.a : fg.rgb;
    vec3 bg_un = bg.a > 0.0 ? bg.rgb / bg.a : bg.rgb;
    if (contrast_ratio(fg_un, bg_un) >= min_ratio) return fg;
    float wr = contrast_ratio(vec3(1.0), bg_un);
    float br = contrast_ratio(vec3(0.0), bg_un);
    vec3 picked = wr > br ? vec3(1.0) : vec3(0.0);
    return vec4(picked * fg.a, fg.a);
}
