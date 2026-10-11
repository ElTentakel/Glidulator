#ifndef GLIDULATOR_CAPTURE_H
#define GLIDULATOR_CAPTURE_H

/* Frame capture for debugging with real software: the debug variant
 * (-DGLIDULATOR_DEBUG) writes the texture memory once (TEXMEM.BIN) and, for
 * some frames, every clear and triangle with its register state (CAP00.BIN,
 * CAP01.BIN, ...). targets/glide_replay re-renders them on Linux with the
 * same pipeline, frame by frame or triangle by triangle.
 *
 * File format (little endian):
 *   TEXMEM.BIN  2 MB tex_mem[0], then 256 x uint32 palette
 *   CAPnn.BIN   glide_capture_header, then ncmd x glide_capture_cmd */

#include "glidulator/glide2x.h"

#include <stdint.h>

#define GLIDE_CAPTURE_MAGIC   0x50414347u /* "GCAP" */
#define GLIDE_CAPTURE_VERSION 1u

enum {
    GLIDE_CAPTURE_TRIANGLE = 1, /* v[0..2] = vertices in application order */
    GLIDE_CAPTURE_CLEAR    = 2  /* reg[CAP_CLEAR_COLOR/DEPTH], fbzMode for the write masks */
};

/* Index into glide_capture_cmd.reg */
enum {
    CAP_FBZMODE, CAP_FBZCOLORPATH, CAP_ALPHAMODE, CAP_FOGMODE,
    CAP_TEXTUREMODE, CAP_TLOD, CAP_TEXBASEADDR, CAP_COLOR0, CAP_COLOR1,
    CAP_ZACOLOR, CAP_CHROMAKEY, CAP_CLIP_LEFT, CAP_CLIP_RIGHT, CAP_CLIP_TOP,
    CAP_CLIP_BOTTOM, CAP_STW_HINT,
    CAP_NREGS,
    CAP_CLEAR_COLOR = CAP_COLOR0, /* for GLIDE_CAPTURE_CLEAR: ARGB */
    CAP_CLEAR_DEPTH = CAP_COLOR1
};

typedef struct {
    uint32_t magic, version, width, height, ncmd, frame;
} glide_capture_header;

typedef struct {
    uint32_t kind;
    uint32_t reg[CAP_NREGS];
    GrVertex v[3];
} glide_capture_cmd;

/* TABnn.BIN (belongs to CAPnn.BIN, next to TEXnn.BIN = 2 MB texture memory
 * of the same frame): tables that are not part of the command registers. */
typedef struct {
    uint32_t palette[256];  /* 0xAARRGGBB like g_voodoo.palette */
    uint32_t ncc[2][12];    /* NCC0/NCC1: Y0..3, I0..3, Q0..3 (register format) */
    uint8_t  fog[64][2];    /* fog table: fog, dfog */
    uint32_t fog_color;     /* 0x00RRGGBB */
} glide_capture_tables;

/* Called from core/glide_abi.c; only present with GLIDULATOR_DEBUG. */
void glide_capture_triangle(const GrVertex *a, const GrVertex *b, const GrVertex *c, const int clip[4]);
void glide_capture_clear(uint32_t argb, uint16_t depth);
void glide_capture_swap(void);

#endif /* GLIDULATOR_CAPTURE_H */
