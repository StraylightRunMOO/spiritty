/**
 * input.ts — DOM event wiring.
 *
 * Translates keyboard / mouse / clipboard events on a host element into
 * calls on a Spiritty instance. The mapping intentionally mirrors xterm
 * conventions (CSI sequences for special keys, modifier-encoded SS3 for
 * function keys, etc.) so existing curses apps Just Work.
 */

import type { Spiritty, SpSelMode } from "./spiritty.js";
import { SP_SEL_NORMAL, SP_SEL_WORD, SP_SEL_LINE } from "./spiritty.js";

/** Standard xterm CSI sequences for cursor + nav keys. */
const CSI_KEY_TABLE: Record<string, string> = {
    ArrowUp:    "\x1b[A",
    ArrowDown:  "\x1b[B",
    ArrowRight: "\x1b[C",
    ArrowLeft:  "\x1b[D",
    Home:       "\x1b[H",
    End:        "\x1b[F",
    PageUp:     "\x1b[5~",
    PageDown:   "\x1b[6~",
    Insert:     "\x1b[2~",
    Delete:     "\x1b[3~",
    F1:         "\x1bOP",
    F2:         "\x1bOQ",
    F3:         "\x1bOR",
    F4:         "\x1bOS",
    F5:         "\x1b[15~",
    F6:         "\x1b[17~",
    F7:         "\x1b[18~",
    F8:         "\x1b[19~",
    F9:         "\x1b[20~",
    F10:        "\x1b[21~",
    F11:        "\x1b[23~",
    F12:        "\x1b[24~",
};

function translateKey(ev: KeyboardEvent): string | null {
    // Modifier-only events have an empty key string we shouldn't echo.
    if (ev.key.length === 0) return null;

    // Ctrl + letter → standard 0x01..0x1A control codes.
    if (ev.ctrlKey && !ev.altKey && !ev.metaKey && ev.key.length === 1) {
        const ch = ev.key.toLowerCase().charCodeAt(0);
        if (ch >= 0x61 && ch <= 0x7A) {
            return String.fromCharCode(ch - 0x60);
        }
    }

    // Lookup table for navigation/function keys.
    const csi = CSI_KEY_TABLE[ev.key];
    if (csi) return csi;

    // Enter / Tab / Backspace / Escape — the C99 spellings xterm uses.
    if (ev.key === "Enter")     return "\r";
    if (ev.key === "Tab")       return "\t";
    if (ev.key === "Backspace") return "\x7f";
    if (ev.key === "Escape")    return "\x1b";

    // Printable: forward as-is (already a UTF-8-encodable string).
    if (ev.key.length === 1) return ev.key;

    return null;
}

export interface AttachInputArgs {
    /** Cell metrics in CSS pixels, used to convert mouse coords → grid. */
    cellWidthCss:  number;
    cellHeightCss: number;
    /** Loop keystrokes and pastes back into the terminal as local echo.
     *  Defaults to true because the browser demo has no backend yet;
     *  set false once a PTY / telnet session is wired up so echo doesn't
     *  double. */
    localEcho?: boolean;
}

export function attachInput(spiritty: Spiritty, args: AttachInputArgs): () => void {
    const canvas = spiritty.element;
    const localEcho = args.localEcho ?? true;

    // Make the canvas focusable so keydown lands here.
    if (!canvas.hasAttribute("tabindex")) canvas.tabIndex = 0;

    const onKeyDown = (ev: KeyboardEvent) => {
        const seq = translateKey(ev);
        if (seq === null) return;
        ev.preventDefault();
        spiritty.sendKey(seq);
        if (localEcho) {
            // Enter needs CRLF for the terminal to advance a row; everything
            // else is forwarded as-is. Control codes and CSI sequences the
            // parser understands will do the right thing in-grid.
            spiritty.write(seq === "\r" ? "\r\n" : seq);
        }
    };

    /** Convert a (clientX, clientY) into terminal (row, col), clamped. */
    const hitTest = (ev: MouseEvent): { row: number; col: number } => {
        const rect = canvas.getBoundingClientRect();
        const x = ev.clientX - rect.left;
        const y = ev.clientY - rect.top;
        const col = Math.max(0, Math.min(spiritty.handle.cols() - 1,
                                          Math.floor(x / args.cellWidthCss)));
        const row = Math.max(0, Math.min(spiritty.handle.rows() - 1,
                                          Math.floor(y / args.cellHeightCss)));
        return { row, col };
    };

    let dragging = false;

    const onMouseDown = (ev: MouseEvent) => {
        canvas.focus();
        const { row, col } = hitTest(ev);
        spiritty.sendMouse(row, col, ev.button, /*pressed=*/true);
        if (ev.button === 0) {
            const mode: SpSelMode = ev.detail >= 3 ? SP_SEL_LINE :
                                    ev.detail === 2 ? SP_SEL_WORD :
                                                      SP_SEL_NORMAL;
            spiritty.selectionBegin(row, col, mode);
            dragging = true;
        }
    };

    const onMouseMove = (ev: MouseEvent) => {
        if (!dragging) return;
        const { row, col } = hitTest(ev);
        spiritty.selectionExtend(row, col);
    };

    const onMouseUp = (ev: MouseEvent) => {
        const { row, col } = hitTest(ev);
        spiritty.sendMouse(row, col, ev.button, /*pressed=*/false);
        if (ev.button === 0 && dragging) {
            dragging = false;
            const text = spiritty.selectionText();
            if (text.length > 0 && navigator.clipboard) {
                void navigator.clipboard.writeText(text);
            }
        }
    };

    const deliverPaste = (text: string) => {
        if (text.length === 0) return;
        spiritty.sendKey(text);
        if (localEcho) {
            // Normalize bare \r or \n to CRLF so each pasted line advances
            // the row (the grid needs an explicit \n after \r).
            spiritty.write(text.replace(/\r\n?|\n/g, "\r\n"));
        }
    };

    const onPaste = async (ev: ClipboardEvent) => {
        const text = ev.clipboardData?.getData("text/plain") ?? "";
        if (text.length > 0) {
            ev.preventDefault();
            deliverPaste(text);
        }
    };

    const onContextMenu = (ev: MouseEvent) => {
        // Right-click pastes (xterm convention). Suppress the browser menu.
        ev.preventDefault();
        if (navigator.clipboard) {
            navigator.clipboard.readText().then(deliverPaste).catch(() => {
                /* clipboard read denied; ignore. */
            });
        }
    };

    canvas.addEventListener("keydown",     onKeyDown);
    canvas.addEventListener("mousedown",   onMouseDown);
    canvas.addEventListener("mousemove",   onMouseMove);
    canvas.addEventListener("mouseup",     onMouseUp);
    canvas.addEventListener("paste",       onPaste);
    canvas.addEventListener("contextmenu", onContextMenu);

    return () => {
        canvas.removeEventListener("keydown",     onKeyDown);
        canvas.removeEventListener("mousedown",   onMouseDown);
        canvas.removeEventListener("mousemove",   onMouseMove);
        canvas.removeEventListener("mouseup",     onMouseUp);
        canvas.removeEventListener("paste",       onPaste);
        canvas.removeEventListener("contextmenu", onContextMenu);
    };
}
