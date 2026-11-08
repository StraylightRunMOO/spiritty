/*
 * wcwidth.h — classify a Unicode codepoint as narrow (1) or wide (2).
 *
 * Covers the East-Asian Wide (W) and Fullwidth (F) blocks from UAX #11
 * plus the emoji-presentation blocks that default-wide per UAX #51 (any
 * cp with the Emoji_Presentation=Yes property). What we match:
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
 *   U+1F000..U+1F02F Mahjong Tiles
 *   U+1F0A0..U+1F0FF Playing Cards
 *   U+1F100..U+1F1FF Enclosed Alphanumeric + Regional Indicators
 *   U+1F200..U+1F2FF Enclosed Ideographic Supplement
 *   U+1F300..U+1F64F Misc Pictographs, Emoticons
 *   U+1F680..U+1F6FF Transport and Map
 *   U+1F900..U+1F9FF Supplemental Pictographs
 *   U+1FA00..U+1FAFF Symbols and Pictographs Extended-A / Chess / Objects
 *   U+20000..U+2FFFD CJK Unified Extensions B..F
 *   U+30000..U+3FFFD CJK Unified Extensions G..
 *
 * Not covered: ambiguous-width class (U+2600..U+27BF includes many
 * emoji that terminals vary on) — narrow is the safer default; apps
 * that care tend to explicitly cols-measure. Variation selectors,
 * ZWJ joiners and the regional-indicator pair sequences are also
 * best treated as width-1 per codepoint here; grapheme-aware width
 * is a renderer concern, not the buffer's.
 */
#ifndef SPIRITTY_WCWIDTH_H
#define SPIRITTY_WCWIDTH_H

#include <stdint.h>

static inline int sp_cp_width(uint32_t cp) {
    if (cp < 0x1100u) return 1;
    if ((cp >= 0x1100u  && cp <= 0x115Fu)  ||
        (cp >= 0x2E80u  && cp <= 0x303Eu)  ||
        (cp >= 0x3041u  && cp <= 0x33FFu)  ||
        (cp >= 0x3400u  && cp <= 0x4DBFu)  ||
        (cp >= 0x4E00u  && cp <= 0x9FFFu)  ||
        (cp >= 0xA000u  && cp <= 0xA4CFu)  ||
        (cp >= 0xAC00u  && cp <= 0xD7A3u)  ||
        (cp >= 0xF900u  && cp <= 0xFAFFu)  ||
        (cp >= 0xFE30u  && cp <= 0xFE4Fu)  ||
        (cp >= 0xFF00u  && cp <= 0xFF60u)  ||
        (cp >= 0xFFE0u  && cp <= 0xFFE6u)  ||
        (cp >= 0x1F000u && cp <= 0x1F02Fu) ||
        (cp >= 0x1F0A0u && cp <= 0x1F0FFu) ||
        (cp >= 0x1F100u && cp <= 0x1F1FFu) ||
        (cp >= 0x1F200u && cp <= 0x1F2FFu) ||
        (cp >= 0x1F300u && cp <= 0x1F64Fu) ||
        (cp >= 0x1F680u && cp <= 0x1F6FFu) ||
        (cp >= 0x1F900u && cp <= 0x1F9FFu) ||
        (cp >= 0x1FA00u && cp <= 0x1FAFFu) ||
        (cp >= 0x20000u && cp <= 0x2FFFDu) ||
        (cp >= 0x30000u && cp <= 0x3FFFDu))
        return 2;
    return 1;
}

#endif /* SPIRITTY_WCWIDTH_H */
