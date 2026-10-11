/* Frame capture, see core/include/glidulator/capture.h. Only in the debug
 * variant; without GLIDULATOR_DEBUG this file is empty. */

#ifdef GLIDULATOR_DEBUG

#include "glidulator/capture.h"
#include "glidulator/platform.h"
#include "glidulator/trace.h"
#include "glide_internal.h"

#include <string.h>

/* From the first triangle on, record frames 5, 10, 15, ...: slow
 * environments (DOSBox) render only a few frames per second, and
 * time-based demos skip frames. A file CAPTURE.CFG in the current
 * directory ("first interval count", e.g. "200 10 16") overrides this for
 * later scenes; "0 1 0" disables recording. At most 100 (CAP00..CAP99). */
static unsigned long g_cap_first = 5, g_cap_every = 5, g_cap_max = 16;
#define CAPTURE_CMDS 5000 /* per frame (~1.2 MB static, debug variant only) */

/* Header and commands contiguous, so a frame reaches the disk in a single
 * write. */
static struct {
    glide_capture_header h;
    glide_capture_cmd    cmds[CAPTURE_CMDS];
} g_cap;
#define g_cmds g_cap.cmds
static uint32_t g_ncmd;
static int               g_seen_triangle;
static unsigned long     g_frame; /* frames since the first triangle */
static int               g_captured;
static int               g_texmem_written;

static int recording(void) {
    return g_seen_triangle && (unsigned long) g_captured < g_cap_max && g_frame >= g_cap_first &&
           (g_frame - g_cap_first) % g_cap_every == 0;
}

/* Next decimal number in *p (skipping non-digits); 0 = none. */
static int next_number(const char **p, const char *end, unsigned long *v) {
    while (*p < end && (**p < '0' || **p > '9'))
        (*p)++;
    if (*p >= end)
        return 0;
    *v = 0;
    while (*p < end && **p >= '0' && **p <= '9')
        *v = *v * 10 + (unsigned long) (*(*p)++ - '0');
    return 1;
}

static void read_config(void) {
    char          buf[64];
    long          n = platform_read_file("CAPTURE.CFG", 0, buf, sizeof buf);
    const char   *p = buf, *end = buf + (n > 0 ? n : 0);
    unsigned long first, every, max;
    if (n > 0 && next_number(&p, end, &first) && next_number(&p, end, &every) && next_number(&p, end, &max) &&
        every > 0) {
        g_cap_first = first;
        g_cap_every = every;
        g_cap_max   = max > 100 ? 100 : max;
        glide_trace_note("CAPTURE.CFG first/interval", g_cap_first, g_cap_every);
    }
}

static glide_capture_cmd *next_cmd(uint32_t kind) {
    if (!recording() || g_ncmd >= CAPTURE_CMDS)
        return NULL;
    glide_capture_cmd *c = &g_cmds[g_ncmd++];
    memset(c, 0, sizeof *c);
    c->kind                   = kind;
    c->reg[CAP_FBZMODE]       = g_voodoo.params.fbzMode;
    c->reg[CAP_FBZCOLORPATH]  = g_voodoo.params.fbzColorPath;
    c->reg[CAP_ALPHAMODE]     = g_voodoo.params.alphaMode;
    c->reg[CAP_FOGMODE]       = g_voodoo.params.fogMode;
    c->reg[CAP_TEXTUREMODE]   = g_voodoo.params.textureMode[0];
    c->reg[CAP_TLOD]          = g_voodoo.params.tLOD[0];
    c->reg[CAP_TEXBASEADDR]   = g_voodoo.params.texBaseAddr[0];
    c->reg[CAP_COLOR0]        = g_voodoo.params.color0;
    c->reg[CAP_COLOR1]        = g_voodoo.params.color1;
    c->reg[CAP_ZACOLOR]       = g_voodoo.params.zaColor;
    c->reg[CAP_CHROMAKEY]     = g_voodoo.params.chromaKey;
    c->reg[CAP_STW_HINT]      = g_sw.stw_hint;
    return c;
}

void glide_capture_triangle(const GrVertex *a, const GrVertex *b, const GrVertex *c, const int clip[4]) {
    if (g_glide_mute)
        return;
    if (!g_seen_triangle)
        read_config();
    g_seen_triangle = 1;
    glide_capture_cmd *cmd = next_cmd(GLIDE_CAPTURE_TRIANGLE);
    if (!cmd)
        return;
    cmd->reg[CAP_CLIP_LEFT]   = (uint32_t) clip[0];
    cmd->reg[CAP_CLIP_RIGHT]  = (uint32_t) clip[1];
    cmd->reg[CAP_CLIP_TOP]    = (uint32_t) clip[2];
    cmd->reg[CAP_CLIP_BOTTOM] = (uint32_t) clip[3];
    cmd->v[0] = *a;
    cmd->v[1] = *b;
    cmd->v[2] = *c;
}

void glide_capture_clear(uint32_t argb, uint16_t depth) {
    if (g_glide_mute)
        return;
    glide_capture_cmd *cmd = next_cmd(GLIDE_CAPTURE_CLEAR);
    if (!cmd)
        return;
    cmd->reg[CAP_CLEAR_COLOR] = argb;
    cmd->reg[CAP_CLEAR_DEPTH] = depth;
}

/* TEXnn.BIN for every CAPnn.BIN: the texture memory of exactly this frame,
 * plus TABnn.BIN with the tables that are not part of the command
 * registers (layout: glide_capture_tables in capture.h). TEXMEM.BIN and
 * PALETTE.BIN are written once at the start. */
static struct {
    glide_capture_tables t;
} g_tab; /* static: 1.2 KB would be needlessly tight on the OVL's stack */

static void write_tables(const char *name) {
    for (int i = 0; i < 256; i++)
        g_tab.t.palette[i] = g_voodoo.palette[0][i].u;
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < 4; k++) {
            g_tab.t.ncc[t][k]     = g_voodoo.nccTable[0][t].y[k];
            g_tab.t.ncc[t][4 + k] = g_voodoo.nccTable[0][t].i[k];
            g_tab.t.ncc[t][8 + k] = g_voodoo.nccTable[0][t].q[k];
        }
    for (int i = 0; i < 64; i++) {
        g_tab.t.fog[i][0] = g_voodoo.params.fogTable[i].fog;
        g_tab.t.fog[i][1] = g_voodoo.params.fogTable[i].dfog;
    }
    g_tab.t.fog_color = ((uint32_t) g_voodoo.params.fogColor.r << 16) |
                        ((uint32_t) g_voodoo.params.fogColor.g << 8) | g_voodoo.params.fogColor.b;
    /* platform_write_file writes a whole file at once, hence separate files
     * for texture memory and tables. */
    platform_write_file(name, g_voodoo.tex_mem[0], TEX_MEM_BYTES);
    char tname[12];
    memcpy(tname, name, 12);
    tname[0] = 'T';
    tname[1] = 'A';
    tname[2] = 'B';
    platform_write_file(tname, &g_tab.t, sizeof g_tab.t);
}

static void write_texmem(void) {
    static uint32_t pal[256];
    for (int i = 0; i < 256; i++)
        pal[i] = g_voodoo.palette[0][i].u;
    platform_write_file("TEXMEM.BIN", g_voodoo.tex_mem[0], TEX_MEM_BYTES);
    platform_write_file("PALETTE.BIN", pal, sizeof pal);
}

void glide_capture_swap(void) {
    if (recording()) {
        char name[12];
        memcpy(name, "CAP00.BIN", 10);
        name[3] = (char) ('0' + g_captured / 10);
        name[4] = (char) ('0' + g_captured % 10);
        if (!g_texmem_written) {
            write_texmem();
            g_texmem_written = 1;
        }
        {
            char tex[12];
            memcpy(tex, "TEX00.BIN", 10);
            tex[3] = name[3];
            tex[4] = name[4];
            write_tables(tex); /* TEXnn.BIN + TABnn.BIN */
        }
        g_cap.h.magic   = GLIDE_CAPTURE_MAGIC;
        g_cap.h.version = GLIDE_CAPTURE_VERSION;
        g_cap.h.width   = (uint32_t) g_width;
        g_cap.h.height  = (uint32_t) g_height;
        g_cap.h.ncmd    = g_ncmd;
        g_cap.h.frame   = (uint32_t) g_frame;
        platform_write_file(name, &g_cap, sizeof g_cap.h + g_ncmd * sizeof(glide_capture_cmd));
        glide_trace_note("Frame captured (no./commands)", (unsigned long) g_captured, (unsigned long) g_ncmd);
        g_captured++;
    }
    g_ncmd = 0;
    if (g_seen_triangle)
        g_frame++;
}

#else
/* ISO C requires at least one declaration per translation unit. */
typedef int glide_capture_unused;
#endif
