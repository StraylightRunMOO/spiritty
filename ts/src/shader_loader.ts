/**
 * shader_loader.ts — fetches a Shadertoy-style fragment shader and hands it
 * to the WASM core via setCustomShader. The shader source is the user's
 * `mainImage(out vec4 fragColor, in vec2 fragCoord)` body; the C side
 * prepends the shadertoy_prefix.glsl uniform header automatically.
 */

import type { Spiritty } from "./spiritty.js";

export async function loadShader(spiritty: Spiritty, url: string): Promise<boolean> {
    const res = await fetch(url, { cache: "no-cache" });
    if (!res.ok) {
        throw new Error(`shader_loader: GET ${url} failed (${res.status})`);
    }
    const glsl = await res.text();
    return spiritty.setCustomShader(glsl);
}

/** Disable any custom post-process and return to the identity pass. */
export function clearShader(spiritty: Spiritty): boolean {
    // The C ABI accepts NULL via passing an empty string — embind copies
    // the empty std::string and sp_terminal_set_custom_shader treats a
    // zero-length payload as "restore default".
    return spiritty.setCustomShader("");
}
