#define SPIRITTY_BUILDING 1

#include "atlas.h"

#include <stdlib.h>
#include <string.h>

#define SP_ATLAS_PAD 1

typedef struct {
    uint32_t      cp;
    sp_atlas_slot slot;
    uint8_t       used;
} sp_atlas_entry;

struct sp_atlas {
    int     w, h;
    int     pack_x, pack_y, row_h;
    uint8_t* pixels;
    sp_atlas_entry* tab;
    size_t  cap;
    size_t  n;
};

static uint32_t hash_cp(uint32_t cp) {
    cp ^= cp >> 16;
    cp *= 0x7feb352du;
    cp ^= cp >> 15;
    cp *= 0x846ca68bu;
    cp ^= cp >> 16;
    return cp;
}

static bool atlas_grow(sp_atlas* a) {
    size_t ncap = a->cap ? a->cap * 2 : 64;
    sp_atlas_entry* ntab = (sp_atlas_entry*)calloc(ncap, sizeof(sp_atlas_entry));
    if (!ntab) return false;
    for (size_t i = 0; i < a->cap; ++i) {
        if (!a->tab[i].used) continue;
        size_t j = hash_cp(a->tab[i].cp) & (ncap - 1);
        while (ntab[j].used) j = (j + 1) & (ncap - 1);
        ntab[j] = a->tab[i];
    }
    free(a->tab);
    a->tab = ntab;
    a->cap = ncap;
    return true;
}

static sp_atlas_entry* atlas_find(const sp_atlas* a, uint32_t cp) {
    if (!a->tab || a->cap == 0) return NULL;
    size_t mask = a->cap - 1;
    size_t j = hash_cp(cp) & mask;
    for (size_t n = 0; n < a->cap; ++n) {
        if (!a->tab[j].used) return NULL;
        if (a->tab[j].cp == cp) return &a->tab[j];
        j = (j + 1) & mask;
    }
    return NULL;
}

sp_atlas* sp_atlas_create(int width, int height) {
    if (width < 32 || height < 32) return NULL;
    sp_atlas* a = (sp_atlas*)calloc(1, sizeof(sp_atlas));
    if (!a) return NULL;
    a->w = width;
    a->h = height;
    a->pack_x = SP_ATLAS_PAD;
    a->pack_y = SP_ATLAS_PAD;
    a->row_h = 0;
    a->pixels = (uint8_t*)calloc((size_t)width * (size_t)height, 1);
    if (!a->pixels) {
        free(a);
        return NULL;
    }
    if (!atlas_grow(a)) {
        free(a->pixels);
        free(a);
        return NULL;
    }
    return a;
}

void sp_atlas_destroy(sp_atlas* a) {
    if (!a) return;
    free(a->pixels);
    free(a->tab);
    free(a);
}

void sp_atlas_reset(sp_atlas* a) {
    if (!a) return;
    memset(a->pixels, 0, (size_t)a->w * (size_t)a->h);
    memset(a->tab, 0, a->cap * sizeof(sp_atlas_entry));
    a->n = 0;
    a->pack_x = SP_ATLAS_PAD;
    a->pack_y = SP_ATLAS_PAD;
    a->row_h = 0;
}

const uint8_t* sp_atlas_pixels(const sp_atlas* a) { return a ? a->pixels : NULL; }
int            sp_atlas_width(const sp_atlas* a)  { return a ? a->w : 0; }
int            sp_atlas_height(const sp_atlas* a) { return a ? a->h : 0; }

const sp_atlas_slot* sp_atlas_get(const sp_atlas* a, uint32_t codepoint) {
    if (!a) return NULL;
    sp_atlas_entry* e = atlas_find(a, codepoint);
    return e ? &e->slot : NULL;
}

const sp_atlas_slot* sp_atlas_pack(sp_atlas* a, const sp_glyph* g) {
    if (!a || !g || !g->pixels || g->width_px == 0 || g->height_px == 0)
        return NULL;

    sp_atlas_entry* existing = atlas_find(a, g->codepoint);
    if (existing) return &existing->slot;

    int gw = (int)g->width_px;
    int gh = (int)g->height_px;
    int need_w = gw + SP_ATLAS_PAD;
    int need_h = gh + SP_ATLAS_PAD;

    if (a->pack_x + need_w > a->w) {
        a->pack_y += a->row_h + SP_ATLAS_PAD;
        a->pack_x = SP_ATLAS_PAD;
        a->row_h = 0;
    }
    if (a->pack_x + need_w > a->w || a->pack_y + need_h > a->h)
        return NULL;

    int x = a->pack_x;
    int y = a->pack_y;
    for (int row = 0; row < gh; ++row) {
        memcpy(a->pixels + (size_t)(y + row) * (size_t)a->w + (size_t)x,
               g->pixels + (size_t)row * (size_t)gw,
               (size_t)gw);
    }

    if (a->n * 10 >= a->cap * 7) {
        if (!atlas_grow(a)) return NULL;
    }
    size_t mask = a->cap - 1;
    size_t j = hash_cp(g->codepoint) & mask;
    while (a->tab[j].used) j = (j + 1) & mask;
    a->tab[j].used = 1;
    a->tab[j].cp = g->codepoint;
    a->tab[j].slot.x = (uint16_t)x;
    a->tab[j].slot.y = (uint16_t)y;
    a->tab[j].slot.w = (uint16_t)gw;
    a->tab[j].slot.h = (uint16_t)gh;
    a->tab[j].slot.bearing_x = g->bearing_x;
    a->tab[j].slot.bearing_y = g->bearing_y;
    a->tab[j].slot.advance   = g->advance;
    a->n++;

    a->pack_x += need_w;
    if (gh > a->row_h) a->row_h = gh;
    return &a->tab[j].slot;
}
