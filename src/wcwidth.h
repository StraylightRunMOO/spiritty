/*
 * wcwidth.h — classify a Unicode codepoint as narrow (1) or wide (2).
 *
 * Covers the East-Asian Wide (W) and Fullwidth (F) blocks from UAX #11
 * that matter in practice:
 *
 *   U+1100..U+115F   Hangul Jamo (choseong)
 *   U+2E80..U+303E   CJK radicals + Kangxi + CJK symbols/punct
 *   U+3041..U+33FF   Hiragana, Katakana, Bopomofo, Hangul compat, CJK strokes
 *   U+3400..U+4DBF   CJK Unified Extension A
 *   U+4E00..U+9FFF   CJK Unified
 *   U+A000..U+A4CF   Yi Syllables
 *   U+AC00..U+D7A3   Hangul Syllables
 *   U+F900..U+FAFF   CJK Compatibility Ideographs
 *   U+FE30..U+FE4F   CJK Compatibility Forms
 *   U+FF00..U+FF60   Fullwidth ASCII variants
 *   U+FFE0..U+FFE6   Fullwidth signs
 *   U+20000..U+2FFFD CJK Unified Extensions B..F
 *   U+30000..U+3FFFD CJK Unified Extensions G..
 *
 * Not covered: emoji presentation width (U+2600+ etc.), ambiguous-width
 * class — those are terminal-configurable and vary between fonts. Narrow
 * is the safer default; apps that care tend to explicitly cols-measure.
 */
#ifndef SPIRITTY_WCWIDTH_H
#define SPIRITTY_WCWIDTH_H

#include <stdint.h>

static inline int sp_cp_width(uint32_t cp) {
    if (cp < 0x1100u) return 1;
    if ((cp >= 0x1100u && cp <= 0x115Fu) ||
        (cp >= 0x2E80u && cp <= 0x303Eu) ||
        (cp >= 0x3041u && cp <= 0x33FFu) ||
        (cp >= 0x3400u && cp <= 0x4DBFu) ||
        (cp >= 0x4E00u && cp <= 0x9FFFu) ||
        (cp >= 0xA000u && cp <= 0xA4CFu) ||
        (cp >= 0xAC00u && cp <= 0xD7A3u) ||
        (cp >= 0xF900u && cp <= 0xFAFFu) ||
        (cp >= 0xFE30u && cp <= 0xFE4Fu) ||
        (cp >= 0xFF00u && cp <= 0xFF60u) ||
        (cp >= 0xFFE0u && cp <= 0xFFE6u) ||
        (cp >= 0x20000u && cp <= 0x2FFFDu) ||
        (cp >= 0x30000u && cp <= 0x3FFFDu))
        return 2;
    return 1;
}

#endif /* SPIRITTY_WCWIDTH_H */
