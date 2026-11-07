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

/** Event emitted by the terminal through setEventCb. `data` is upstream
 *  bytes the host should forward to the application (telnet, PTY, VM). */
export type SpEvent =
    | { kind: "data";      bytes: Uint8Array }
    | { kind: "resize";    cols: number; rows: number }
    | { kind: "title";     text: string }
    | { kind: "bell" }
    | { kind: "cursor";    row: number; col: number; visible: boolean }
    | { kind: "scroll";    top: number }
    | { kind: "selection" };

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
    /** button: 0/1/2 = left/middle/right (press+release); 3 = motion no-button;
     *  64/65 = wheel up/down (press-only). Gated by DECSET 9/1000/1002/1003. */
    sendMouse(row: number, col: number, button: number, pressed: boolean): void;
    /** Bitmask of DECSET mouse modes currently enabled. bit0=X10(9),
     *  bit1=1000, bit2=1002, bit3=1003, bit4=SGR(1006), bit5=URXVT(1015). */
    mouseMode(): number;
    /** Delivers `text` as a paste. If DECSET 2004 (bracketed paste) is
     *  active the C side wraps the payload in ESC[200~ / ESC[201~.
     *  Embedded ESC bytes are stripped regardless, matching xterm. */
    sendPaste(text: string): void;

    /** OSC 8 hyperlink hit-test. Returns the URI string at `(row, col)`
     *  (viewport coords; respects scroll offset) or an empty string if
     *  no hyperlink is set on that cell. Scrollback cells always return
     *  "" (URIs are not carried into the ring in v1). */
    uriAt(row: number, col: number): string;

    /** Subscribe to terminal-generated events. `cb` receives:
     *    { kind: "data" | "resize" | "title" | "bell" | "cursor" | "scroll" | "selection", ... }
     *  "data" carries a fresh Uint8Array (bytes). Pass null/undefined to
     *  unsubscribe. */
    setEventCb(cb: ((ev: SpEvent) => void) | null | undefined): void;

    /** Scrollback viewport. `offset` = lines above the live bottom; 0 = pinned
     *  to bottom (normal). Any subsequent write() resets to 0. */
    scrollOffset(): number;
    scrollMax(): number;
    scrollTo(offset: number): void;
    scrollLines(delta: number): void;
    scrollHome(): void;
    scrollEnd(): void;
    scrollInfo(): { offset: number; max: number };

    /** Mutable 256-entry palette driving ANSI 0-15, xterm cube, greyscale,
     *  and 38;5;N / 48;5;N. OSC 4 writes into the same table. Values are
     *  0xRRGGBBAA. */
    setPaletteEntry(idx: number, rgba: number): void;
    getPaletteEntry(idx: number): number;
    setPalette(arr: number[]): void;
    getPalette(): number[];
    /** role: 0=FG, 1=BG, 2=CURSOR, 3=SELECTION (SP_COLOR_*). */
    setColor(role: number, rgba: number): void;
    getColor(role: number): number;

    /** Framebuffer pixels (includes devicePixelRatio). */
    cellWidth(): number;
    cellHeight(): number;
    /** `data` is a TTF/OTF byte array; empty/undefined reloads IBM Plex Mono. */
    loadFont(data: Uint8Array | null | undefined, pixelHeight: number): boolean;

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
