/*
 * atlas.h — CPU-side R8 glyph atlas with row packing. Internal.
 */
#ifndef SPIRITTY_ATLAS_H
#define SPIRITTY_ATLAS_H

#include "spiritty/spiritty_renderer.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x, y, w, h;
    int16_t  bearing_x, bearing_y;
    float    advance;
} sp_atlas_slot;

typedef struct sp_atlas sp_atlas;

sp_atlas* sp_atlas_create(int width, int height);
void      sp_atlas_destroy(sp_atlas* a);
void      sp_atlas_reset(sp_atlas* a);

const uint8_t* sp_atlas_pixels(const sp_atlas* a);
int            sp_atlas_width(const sp_atlas* a);
int            sp_atlas_height(const sp_atlas* a);

const sp_atlas_slot* sp_atlas_get(const sp_atlas* a, uint32_t codepoint);
/* Pack g into the atlas. Returns the slot, or NULL if the atlas is full. */
const sp_atlas_slot* sp_atlas_pack(sp_atlas* a, const sp_glyph* g);

#ifdef __cplusplus
}
#endif

#endif /* SPIRITTY_ATLAS_H */
