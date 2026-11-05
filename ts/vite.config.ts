import { defineConfig } from "vite";

/**
 * Spiritty browser demo — Vite config.
 *
 * The WASM module (spiritty.mjs + spiritty.wasm) is dropped into
 * ./src/wasm by the CMake build (see bindings/CMakeLists.txt
 * RUNTIME_OUTPUT_DIRECTORY). spiritty.ts imports the .mjs as a normal
 * module; the Embind glue resolves the .wasm via
 * `new URL("./spiritty.wasm", import.meta.url)`.
 *
 * `assetsInclude` keeps Vite from trying to transform/inline the .wasm —
 * it's a passthrough asset that the .mjs fetches at module-init time.
 */
export default defineConfig({
    root: ".",
    publicDir: false,
    server: {
        port: 5173,
        strictPort: true,
        // SharedArrayBuffer (and a few other emscripten features) require
        // cross-origin isolation. Cheap to set; harmless if unused.
        headers: {
            "Cross-Origin-Opener-Policy":   "same-origin",
            "Cross-Origin-Embedder-Policy": "require-corp",
        },
    },
    build: {
        target: "es2022",
        outDir: "dist",
        emptyOutDir: true,
        sourcemap: true,
    },
    assetsInclude: ["**/*.wasm"],
});
