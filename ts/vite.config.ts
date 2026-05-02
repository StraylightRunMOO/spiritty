import { defineConfig } from "vite";

/**
 * Spiritty browser demo — Vite config.
 *
 * The WASM module (spiritty.mjs + spiritty.wasm) is dropped into ./public
 * by the CMake build (see bindings/CMakeLists.txt RUNTIME_OUTPUT_DIRECTORY).
 * Vite serves /public at the site root in dev and copies it verbatim
 * during build, so the import path "/spiritty.mjs" works in both modes.
 *
 * `assetsInclude` makes Vite treat the .wasm file as a passthrough asset
 * so it doesn't try to inline it.
 */
export default defineConfig({
    root: ".",
    publicDir: "public",
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
