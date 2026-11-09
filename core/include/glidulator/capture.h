#ifndef GLIDULATOR_CAPTURE_H
#define GLIDULATOR_CAPTURE_H

/* Frame capture for debugging with real software: the debug variant
 * (-DGLIDULATOR_DEBUG) writes the texture memory once (TEXMEM.BIN) and, for
 * some frames, every clear and triangle with its register state (CAP00.BIN,
 * CAP01.BIN, ...). targets/glide_replay re-renders them on Linux with the
 * same pipeline, frame by frame or triangle by triangle.
 *
 * File format (little endian):
 *   TEXMEM.BIN  tex_mem[0] (TEX_MEM_BYTES: 2 MB DOS, 4 MB Windows)
 *   PALETTE.BIN 256 x uint32 palette of TMU0
 *   CAPnn.BIN   glide_capture_header, then ncmd x glide_capture_cmd
 *   TEXnn.BIN, TABnn.BIN  texture memory and tables of TMU0 for CAPnn
 *   TX1nn.BIN, TB1nn.BIN  the same for TMU1 (two TMUs only)
 *   PALnn.BIN   every palette downloaded during CAPnn (256 x uint32 each,
 *               referenced by GLIDE_CALL_PALETTE records; v4)
 * Version 1 (before TMU1) has CAP_NREGS_V1 registers per command; the
 * replay still reads it. Version 4 adds GLIDE_CAPTURE_CALL commands: the
 * state and texture calls of the game in order between the triangles
 * (glide_replay --list prints them). */

#include "glidulator/glide2x.h"

#include <stdint.h>

#define GLIDE_CAPTURE_MAGIC   0x50414347u /* "GCAP" */
#define GLIDE_CAPTURE_VERSION 4u

enum {
    GLIDE_CAPTURE_TRIANGLE = 1, /* v[0..2] = vertices in application order */
    GLIDE_CAPTURE_CLEAR    = 2, /* reg[CAP_CLEAR_COLOR/DEPTH], fbzMode for the write masks */
    GLIDE_CAPTURE_CALL     = 3  /* v4: reg[0] = GLIDE_CALL_*, reg[1] = argument count,
                                   reg[2..] = arguments; no register state */
};

/* Functions recorded as GLIDE_CAPTURE_CALL (reg[0]). Append only: the
 * numbers are part of the file format. Names: glide_capture_call_name(). */
enum {
    GLIDE_CALL_ALPHA_BLEND = 1,      /* rgb_sf, rgb_df, alpha_sf, alpha_df */
    GLIDE_CALL_ALPHA_TEST_FUNC,      /* function */
    GLIDE_CALL_ALPHA_TEST_REF,       /* value */
    GLIDE_CALL_DEPTH_MASK,           /* mask */
    GLIDE_CALL_DEPTH_FUNC,           /* function */
    GLIDE_CALL_DEPTH_MODE,           /* mode */
    GLIDE_CALL_CHROMA_MODE,          /* mode */
    GLIDE_CALL_CHROMA_VALUE,         /* value */
    GLIDE_CALL_COLOR_COMBINE,        /* function, factor, local, other, invert */
    GLIDE_CALL_ALPHA_COMBINE,        /* function, factor, local, other, invert */
    GLIDE_CALL_TEX_COMBINE,          /* tmu, rgb_function, rgb_factor, alpha_function, alpha_factor, rgb_invert, alpha_invert */
    GLIDE_CALL_CONSTANT_COLOR,       /* value */
    GLIDE_CALL_ALPHA_ITRGB,          /* enable */
    GLIDE_CALL_COLOR_MASK,           /* rgb, a */
    GLIDE_CALL_CULL_MODE,            /* mode */
    GLIDE_CALL_FOG_MODE,             /* mode */
    GLIDE_CALL_TEX_SOURCE,           /* tmu, start, evenOdd, smallLod, largeLod, aspect, format */
    GLIDE_CALL_TEX_DOWNLOAD_MIPMAP,  /* tmu, start, evenOdd, smallLod, largeLod, aspect, format */
    GLIDE_CALL_TEX_DOWNLOAD_LEVEL,   /* tmu, start, thisLod, largeLod, aspect, format, evenOdd */
    GLIDE_CALL_TEX_DOWNLOAD_PARTIAL, /* tmu, start, thisLod, largeLod, aspect, format, evenOdd, start | end << 16 */
    GLIDE_CALL_TEX_DOWNLOAD_TABLE,   /* tmu, type, first, last */
    GLIDE_CALL_TEX_FILTER,           /* tmu, min, mag */
    GLIDE_CALL_TEX_CLAMP,            /* tmu, s, t */
    GLIDE_CALL_TEX_MIPMAP_MODE,      /* tmu, mode, lodBlend */
    GLIDE_CALL_TEX_MULTIBASE,        /* tmu, enable */
    GLIDE_CALL_TEX_MULTIBASE_ADDR,   /* tmu, range, start, evenOdd */
    GLIDE_CALL_PALETTE,              /* tmu, n: the TMU's whole palette after a download
                                        is entry n of PALnn.BIN (256 x uint32 each) */
    GLIDE_CALL_COUNT
};

/* Index into glide_capture_cmd.reg */
enum {
    CAP_FBZMODE, CAP_FBZCOLORPATH, CAP_ALPHAMODE, CAP_FOGMODE,
    CAP_TEXTUREMODE, CAP_TLOD, CAP_TEXBASEADDR, CAP_COLOR0, CAP_COLOR1,
    CAP_ZACOLOR, CAP_CHROMAKEY, CAP_CLIP_LEFT, CAP_CLIP_RIGHT, CAP_CLIP_TOP,
    CAP_CLIP_BOTTOM, CAP_STW_HINT,
    CAP_NREGS_V1,
    CAP_TEXTUREMODE1 = CAP_NREGS_V1, CAP_TLOD1, CAP_TEXBASEADDR1,
    CAP_NUM_TMUS, /* g_num_tmus (1 or 2) */
    CAP_NREGS_V2,
    /* version 3: detail (tDetail layout: max | bias << 8 | scale << 14) and
     * multibase addresses per TMU */
    CAP_DETAIL0 = CAP_NREGS_V2, CAP_DETAIL1,
    CAP_TEXBASE1_0, CAP_TEXBASE2_0, CAP_TEXBASE38_0,
    CAP_TEXBASE1_1, CAP_TEXBASE2_1, CAP_TEXBASE38_1,
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
/* n arguments follow, each a 32-bit integer (passed as int/unsigned) */
void glide_capture_call(uint32_t id, int n, ...);
/* After a palette download: records the TMU's whole palette (debug only). */
void glide_capture_palette(int tmu);
const char *glide_capture_call_name(uint32_t id);

/* Records a call in the debug variant: GLIDE_CAPTURE_CALL(GLIDE_CALL_X,
 * arg, ...); otherwise nothing. */
#ifdef GLIDULATOR_DEBUG
/* argument count (1..8) at compile time; Watcom does not accept a local
 * array with non-constant initialisers */
#    define GLIDE_CAP_NARG(...)  GLIDE_CAP_NARG_(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#    define GLIDE_CAP_NARG_(a1, a2, a3, a4, a5, a6, a7, a8, n, ...) n
#    define GLIDE_CAPTURE_CALL(id, ...) glide_capture_call((id), GLIDE_CAP_NARG(__VA_ARGS__), __VA_ARGS__)
#else
#    define GLIDE_CAPTURE_CALL(id, ...) ((void) 0)
#endif

#endif /* GLIDULATOR_CAPTURE_H */
