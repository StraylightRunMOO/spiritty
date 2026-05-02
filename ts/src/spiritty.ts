/**
 * Spiritty — TypeScript wrapper around the WASM core.
 *
 * Thin layer:
 *   - Loads the Embind module.
 *   - Owns the Module + an SpTerminal handle.
 *   - Pumps the render loop on requestAnimationFrame.
 *   - Provides ergonomic write() that accepts strings or Uint8Arrays.
 *
 * DOM concerns (keyboard, mouse, clipboard, ResizeObserver) live in
 * input.ts so this module stays portable to e.g. a worker thread.
 */

import type {
    SpiritteyModule,
    SpiritteyInitArgs,
    SpOptions,
    SpTerminal,
    SpSelMode,
    SpCursor,
} from "./wasm/spiritty.js";

export type { SpOptions, SpCursor, SpSelMode };

/** Runtime constants for selection modes (mirror of sp_sel_mode in spiritty.h). */
export const SP_SEL_NONE:   SpSelMode = 0;
export const SP_SEL_NORMAL: SpSelMode = 1;
export const SP_SEL_WORD:   SpSelMode = 2;
export const SP_SEL_LINE:   SpSelMode = 3;

/**
 * Runtime URL for the Embind module. CMake drops spiritty.mjs into
 * ts/public, which Vite serves at the site root. Dynamic import keeps
 * the .mjs out of the TS module graph entirely (no bundler attempt to
 * statically analyze it). Override via SpirittyArgs.factoryUrl if
 * hosting under a non-root prefix.
 */
const DEFAULT_FACTORY_URL = "/spiritty.mjs";

type SpirittyFactory = (args?: SpiritteyInitArgs) => Promise<SpiritteyModule>;

async function loadFactory(url: string): Promise<SpirittyFactory> {
    /* @vite-ignore */
    const mod = await import(/* @vite-ignore */ url) as { default: SpirittyFactory };
    return mod.default;
}

const DEFAULT_OPTIONS: SpOptions = {
    cols:             80,
    rows:             24,
    scrollbackLines:  1000,
    fontSize:         14,
    fontFamily:       "monospace",
    colorFg:          0xE0E0E0FF,
    colorBg:          0x101010FF,
    colorCursor:      0xE0E0E0FF,
    colorSelection:   0x3060A0AA,
    gpuAcceleration:  true,
    cursorBlink:      true,
    cursorStyle:      0,
    customShaderPath: "",
};

export interface SpirittyArgs {
    canvas: HTMLCanvasElement;
    options?: Partial<SpOptions>;
    /** Forwarded to the Embind module loader. */
    locateFile?: SpiritteyInitArgs["locateFile"];
    /** Override the URL of the spiritty.mjs ES module (default: /spiritty.mjs). */
    factoryUrl?: string;
}

export class Spiritty {
    private readonly module: SpiritteyModule;
    private readonly term:   SpTerminal;
    private readonly canvas: HTMLCanvasElement;
    private readonly textEncoder = new TextEncoder();
    private rafHandle: number | null = null;
    private startTime: number;

    private constructor(module: SpiritteyModule, term: SpTerminal, canvas: HTMLCanvasElement) {
        this.module    = module;
        this.term      = term;
        this.canvas    = canvas;
        this.startTime = performance.now();
    }

    static async create(args: SpirittyArgs): Promise<Spiritty> {
        const opts: SpOptions = { ...DEFAULT_OPTIONS, ...args.options };

        // Tell Emscripten to use this canvas for its WebGL2 context.
        // The flag name is set in stone by emscripten — `Module.canvas`.
        const initArgs: SpiritteyInitArgs = {
            canvas: args.canvas,
            ...(args.locateFile ? { locateFile: args.locateFile } : {}),
        };

        const factory = await loadFactory(args.factoryUrl ?? DEFAULT_FACTORY_URL);
        const module  = await factory(initArgs);
        const term    = module.createTerminal(opts);
        if (!term.valid()) {
            term.delete();
            throw new Error("spiritty: createTerminal returned an invalid handle");
        }
        return new Spiritty(module, term, args.canvas);
    }

    /** Underlying module — useful for direct heap access in hot paths. */
    get raw(): SpiritteyModule { return this.module; }

    /** Underlying terminal handle. */
    get handle(): SpTerminal { return this.term; }

    /** Encode + write. For perf-critical paths use writeBytes / writeHeap. */
    write(data: string | Uint8Array): void {
        if (typeof data === "string") {
            this.term.write(this.textEncoder.encode(data));
        } else {
            this.term.write(data);
        }
    }

    /**
     * Zero-copy write for callers that already hold WASM heap memory
     * (e.g. a fetch reader streaming into a heap-resident buffer).
     */
    writeHeap(offset: number, len: number): void {
        this.term.writeHeap(offset, len);
    }

    resize(cols: number, rows: number): void {
        this.term.resize(cols, rows);
    }

    clear(): void { this.term.clear(); }
    reset(): void { this.term.reset(); }

    cursor(): SpCursor { return this.term.getCursor(); }

    sendKey(utf8: string): void { this.term.sendKey(utf8); }
    sendMouse(row: number, col: number, button: number, pressed: boolean): void {
        this.term.sendMouse(row, col, button, pressed);
    }

    selectionBegin(row: number, col: number, mode: SpSelMode): void {
        this.term.selectionBegin(row, col, mode);
    }
    selectionExtend(row: number, col: number): void {
        this.term.selectionExtend(row, col);
    }
    selectionClear(): void { this.term.selectionClear(); }
    selectionText(): string { return this.term.selectionText(); }

    setCustomShader(glsl: string): boolean {
        return this.term.setCustomShader(glsl);
    }

    /** Begin pumping render frames. Idempotent. */
    start(): void {
        if (this.rafHandle !== null) return;
        const tick = () => {
            this.rafHandle = requestAnimationFrame(tick);
            this.term.setTime((performance.now() - this.startTime) * 1e-3);
            this.term.render();
        };
        this.rafHandle = requestAnimationFrame(tick);
    }

    stop(): void {
        if (this.rafHandle !== null) {
            cancelAnimationFrame(this.rafHandle);
            this.rafHandle = null;
        }
    }

    /** Tear down the terminal and renderer. The instance is unusable after. */
    destroy(): void {
        this.stop();
        this.term.delete();
    }

    /** Implementation version baked into the WASM. */
    version(): string { return this.module.version(); }

    /** Reference to the canvas the terminal is rendering into. */
    get element(): HTMLCanvasElement { return this.canvas; }
}
