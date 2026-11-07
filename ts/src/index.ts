/**
 * Spiritty browser demo entrypoint. Boots the WASM module, attaches input
 * wiring, prints a hello banner, and starts the render loop.
 */

import { Spiritty } from "./spiritty.js";
import { attachInput } from "./input.js";

async function main(): Promise<void> {
    const canvas = document.getElementById("spiritty") as HTMLCanvasElement | null;
    if (!canvas) throw new Error("missing #spiritty canvas in document");

    // Size the backing store to a multiple of the cell metric so the
    // grid lands on integer pixels. Metrics come from the TTF after
    // create(); the 0.6em x 1.25em estimate is only for the first layout.
    const fontSize = 16;
    const dpr   = window.devicePixelRatio || 1;
    let cellW = Math.max(1, Math.round(fontSize * 0.6));
    let cellH = Math.max(1, Math.round(fontSize * 1.25));
    const cols  = 80;
    const rows  = 24;

    canvas.width  = cols * cellW * dpr;
    canvas.height = rows * cellH * dpr;
    canvas.style.width  = `${cols * cellW}px`;
    canvas.style.height = `${rows * cellH}px`;

    const spiritty = await Spiritty.create({
        canvas,
        options: { cols, rows, fontSize, fontFamily: "IBM Plex Mono" },
    });

    cellW = spiritty.handle.cellWidth() / dpr;
    cellH = spiritty.handle.cellHeight() / dpr;
    canvas.width  = cols * cellW * dpr;
    canvas.height = rows * cellH * dpr;
    canvas.style.width  = `${cols * cellW}px`;
    canvas.style.height = `${rows * cellH}px`;
    spiritty.handle.resizeFramebuffer(canvas.width, canvas.height);

    attachInput(spiritty, { cellWidthCss: cellW, cellHeightCss: cellH });

    spiritty.write("\x1b[1;32mSpiritty\x1b[0m WASM build ready.\r\n");
    spiritty.write(`Version: ${spiritty.version()}\r\n`);
    spiritty.write(`Grid: ${spiritty.handle.cols()}x${spiritty.handle.rows()}\r\n`);
    spiritty.write("Type to see keystrokes echo, right-click pastes.\r\n");

    spiritty.start();

    // Expose for ad-hoc inspection from devtools.
    (globalThis as unknown as { spiritty?: Spiritty }).spiritty = spiritty;
}

main().catch(err => {
    console.error("spiritty boot failed:", err);
    document.body.innerHTML =
        `<pre style="color:#f55">spiritty boot failed: ${String(err)}</pre>`;
});
