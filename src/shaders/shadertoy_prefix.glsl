// Spiritty Shadertoy-compat prefix. Prepended to user GLSL by the host
// before the user's `void mainImage(out vec4 fragColor, in vec2 fragCoord)`
// body. Uniforms match Shadertoy where applicable; the host advances
// iTime/iFrame at refresh rate but skips frames when iDirty == 0 and the
// shader opts in via #pragma SPIRITTY_VSYNC_GATE. Animation that must
// keep ticking should ignore iDirty.

precision highp float;

uniform vec3  iResolution;     // px, .z = pixel aspect
uniform float iTime;           // seconds since start
uniform float iTimeDelta;      // seconds since last frame
uniform float iFrameRate;      // 1.0 / iTimeDelta, smoothed
uniform int   iFrame;          // monotonically increasing frame counter
uniform vec4  iMouse;          // xy = current, zw = click
uniform vec4  iDate;           // year, month, day, seconds-since-midnight
uniform float iSampleRate;     // unused; provided for compat (44100)

// --- Spiritty extensions ---
uniform vec4  iCurrentCursor;       // x,y in px, z,w = w,h
uniform vec4  iPreviousCursor;
uniform vec4  iCurrentCursorColor;
uniform vec4  iPreviousCursorColor;
uniform int   iCurrentCursorStyle;  // 0=block 1=hollow 2=bar 3=underline 4=lock
uniform int   iPreviousCursorStyle;
uniform int   iCursorVisible;
uniform float iTimeCursorChange;
uniform float iTimeFocus;
uniform int   iFocus;
uniform vec3  iBackgroundColor;
uniform vec3  iForegroundColor;
uniform vec3  iCursorColor;
uniform vec3  iCursorText;
uniform vec3  iSelectionForegroundColor;
uniform vec3  iSelectionBackgroundColor;
uniform vec3  iPalette[256];
uniform int   iDirty;          // 0 = scene unchanged since last frame, 1 = changed

uniform sampler2D iChannel0;
// iChannel1..3 reserved for future input attachments.

in  vec2 v_uv;
out vec4 _spiritty_frag_out;

#define texture2D texture

void mainImage(out vec4 fragColor, in vec2 fragCoord);

void main() {
    vec2 fragCoord = v_uv * iResolution.xy;
    mainImage(_spiritty_frag_out, fragCoord);
}
