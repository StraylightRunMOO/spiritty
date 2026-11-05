/**
 * Type declarations for the Embind-generated spiritty.mjs module.
 *
 * Hand-written because emcc's `--emit-tsd` requires `tsc` in PATH at link
 * time, which would couple the C build to a JS toolchain. Keep this file
 * in sync with bindings/embind.cpp by convention; review in PR.
 */

export interface SpOptions {
    cols: number;
    rows: number;
    scrollbackLines: number;
    fontSize: number;
    fontFamily: string;
    /** 0xRRGGBBAA */
    colorFg: number;
    colorBg: number;
    colorCursor: number;
    colorSelection: number;
    gpuAcceleration: boolean;
    cursorBlink: boolean;
    /** 0=block, 1=underline, 2=bar */
    cursorStyle: number;
    customShaderPath: string;
}

export interface SpCursor {
    row: number;
    col: number;
}

/** sp_sel_mode in spiritty.h. The C enum is `int`-sized; we expose
 *  numeric literals so `verbatimModuleSyntax` can erase the import. */
export type SpSelMode = 0 | 1 | 2 | 3;

/**
 * Wraps an `sp_terminal*` plus its attached WebGL renderer. The JS GC
 * does not call destructors automatically — call `delete()` (provided
 * by Embind on every class binding) when you are finished with the
 * handle, or wrap creation/destruction in a `try`/`finally`.
 */
/** Only the names surfaced on the JS side are listed here; keep in
 *  sync with embind.cpp. */
export interface SpTerminal {
    valid(): boolean;

    /** Copies bytes from `data` (Uint8Array) into the terminal. */
    write(data: Uint8Array): void;

    /**
     * Zero-copy write: the caller has already placed `len` bytes at
     * `offset` inside Module.HEAPU8. Use `Module._malloc` to obtain a
     * stable offset; free it after the call returns.
     */
    writeHeap(offset: number, len: number): void;

    resize(cols: number, rows: number): void;
    /** Framebuffer (canvas backing-store) resize. Call after
     *  canvas.width/height change; pair with `resize` if cols/rows also
     *  changed. */
    resizeFramebuffer(fbW: number, fbH: number): void;
    clear(): void;
    reset(): void;
    render(): void;
    setTime(seconds: number): void;

    cols(): number;
    rows(): number;

    moveCursor(row: number, col: number): void;
    getCursor(): SpCursor;

    selectionBegin(row: number, col: number, mode: SpSelMode): void;
    selectionExtend(row: number, col: number): void;
    selectionClear(): void;
    selectionText(): string;

    /** UTF-8 encoded as a JS string; the C side measures by byte length. */
    sendKey(utf8: string): void;
    sendMouse(row: number, col: number, button: number, pressed: boolean): void;

    setCustomShader(glsl: string): boolean;

    /** Embind-provided dispose. Call exactly once. */
    delete(): void;
}

/** Subset of Emscripten's runtime `GL` module we rely on to attach a
 *  WebGL2 context to the WASM instance. Exposed by listing 'GL' in the
 *  emcc EXPORTED_RUNTIME_METHODS flag. */
export interface EmscriptenGL {
    createContext(
        canvas: HTMLCanvasElement,
        attrs: WebGLContextAttributes & { majorVersion?: number; minorVersion?: number },
    ): number;  /* opaque handle; 0 on failure */
    makeContextCurrent(handle: number): boolean;
}

export interface SpiritteyModule {
    /** Direct heap access for zero-copy paths. */
    HEAPU8: Uint8Array;
    HEAPU32: Uint32Array;
    HEAPF32: Float32Array;
    wasmMemory: WebAssembly.Memory;

    /** Emscripten WebGL plumbing — present because 'GL' is in the
     *  EXPORTED_RUNTIME_METHODS list (see bindings/CMakeLists.txt). */
    GL: EmscriptenGL;

    _malloc(bytes: number): number;
    _free(ptr: number): void;

    /** Framebuffer dimensions + DPR are needed up-front because the
     *  WebGL backend compiles shaders and sizes the scene FBO during
     *  init, which happens inside this call. */
    createTerminal(opts: SpOptions, fbW: number, fbH: number, dpr: number): SpTerminal;
    destroyTerminal(t: SpTerminal): void;

    version(): string;
}

export interface SpiritteyInitArgs {
    canvas?: HTMLCanvasElement;
    /** Override the .wasm fetch URL (default: alongside the .mjs). */
    locateFile?: (path: string, scriptDir: string) => string;
    /** Forwarded to Emscripten for stdout/stderr. */
    print?: (text: string) => void;
    printErr?: (text: string) => void;
}

declare const Spiritty: (args?: SpiritteyInitArgs) => Promise<SpiritteyModule>;
export default Spiritty;
