/* leload.c - see leload.h. */
#include "leload.h"
#include <string.h>

#define MAX_OBJECTS 16

struct le_module {
    const le_io *io;
    uint8_t     *file;
    size_t       file_size;
    uint32_t     le;                  /* offset of the LE header */
    uint32_t     flags;
    int          nobj;
    uint8_t     *raw[MAX_OBJECTS];    /* allocation (unaligned) */
    uint8_t     *base[MAX_OBJECTS];   /* 4 KB aligned object base */
    uint32_t     size[MAX_OBJECTS];
    uint32_t     eip_obj, eip;
    char         name[64];
    int          nfixups;
};

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int fail(char *err, size_t n, const char *msg)
{
    if (err && n) {
        strncpy(err, msg, n - 1);
        err[n - 1] = 0;
    }
    return -1;
}

/* Apply one fixup record's value at base+off (off may be negative or run
 * past the page: records crossing a page appear in both pages and writing
 * the whole value twice is harmless). */
static int apply(le_module *m, uint8_t *page, int32_t off, uint8_t stype, uint32_t target)
{
    uint8_t *p = page + off;
    switch (stype & 0x0F) {
    case 0x07:                                  /* 32-bit offset */
        memcpy(p, &target, 4);
        break;
    case 0x08: {                                /* 32-bit self-relative */
        uint32_t rel = target - (uint32_t)(uintptr_t)(p + 4);
        memcpy(p, &rel, 4);
        break; }
    case 0x05: {                                /* 16-bit offset */
        uint16_t v = (uint16_t)target;
        memcpy(p, &v, 2);
        break; }
    default:
        return -1;
    }
    m->nfixups++;
    return 0;
}

le_module *le_load(const le_io *io, const char *path, char *err, size_t errlen)
{
    le_module *m;
    uint8_t *f, *h;
    uint32_t objtab, pagemap, fpt, frt, datapages, pagesize, lastpage, npages;
    int i;

    m = (le_module *)io->alloc(sizeof *m);
    if (!m) { fail(err, errlen, "out of memory"); return NULL; }
    memset(m, 0, sizeof *m);
    m->io = io;
    m->file = f = (uint8_t *)io->read_file(path, &m->file_size);
    if (!f) { fail(err, errlen, "cannot read file"); goto bad; }
    if (m->file_size < 0x40 || f[0] != 'M' || f[1] != 'Z') { fail(err, errlen, "no MZ header"); goto bad; }
    m->le = rd32(f + 0x3C);
    if (m->le + 0xC4 > m->file_size) { fail(err, errlen, "bad LE offset"); goto bad; }
    h = f + m->le;
    if (h[0] != 'L' || h[1] != 'E') { fail(err, errlen, "no LE header"); goto bad; }
    if (h[2] != 0 || h[3] != 0 || rd32(h + 0x04) != 0) { fail(err, errlen, "not little-endian / level 0"); goto bad; }
    m->flags   = rd32(h + 0x10);
    npages     = rd32(h + 0x14);
    m->eip_obj = rd32(h + 0x18);
    m->eip     = rd32(h + 0x1C);
    pagesize   = rd32(h + 0x28);
    lastpage   = rd32(h + 0x2C);
    objtab     = m->le + rd32(h + 0x40);
    m->nobj    = (int)rd32(h + 0x44);
    pagemap    = m->le + rd32(h + 0x48);
    fpt        = m->le + rd32(h + 0x68);
    frt        = m->le + rd32(h + 0x6C);
    datapages  = rd32(h + 0x80);
    if (rd32(h + 0x74) != 0) { fail(err, errlen, "DLL imports other modules"); goto bad; }
    if (m->nobj < 1 || m->nobj > MAX_OBJECTS) { fail(err, errlen, "bad object count"); goto bad; }

    /* Allocate and load every object. */
    for (i = 0; i < m->nobj; i++) {
        const uint8_t *o = f + objtab + 24 * i;
        uint32_t vsize = rd32(o), pmi = rd32(o + 12), pmc = rd32(o + 16), j;
        uint32_t alloc = ((vsize + 0xFFF) & ~0xFFFu) + 0x1000;
        if (alloc < pmc * pagesize + 0x1000)
            alloc = pmc * pagesize + 0x1000;
        m->raw[i] = (uint8_t *)io->alloc(alloc);
        if (!m->raw[i]) { fail(err, errlen, "out of memory for object"); goto bad; }
        m->base[i] = (uint8_t *)(((uintptr_t)m->raw[i] + 0xFFF) & ~(uintptr_t)0xFFF);
        m->size[i] = vsize;
        for (j = 0; j < pmc; j++) {
            uint32_t pg = pmi + j;                   /* 1-based page number */
            const uint8_t *pe = f + pagemap + 4 * (pg - 1);
            uint32_t num = ((uint32_t)pe[0] << 16) | ((uint32_t)pe[1] << 8) | pe[2];
            uint32_t len = (pg == npages) ? lastpage : pagesize;
            uint32_t src;
            if (pe[3] != 0) { fail(err, errlen, "unsupported page type"); goto bad; }
            if (num == 0) continue;
            src = datapages + (num - 1) * pagesize;
            if (src + len > m->file_size) { fail(err, errlen, "page beyond end of file"); goto bad; }
            memcpy(m->base[i] + j * pagesize, f + src, len);
        }
    }

    /* Fixups, page by page. */
    for (i = 0; i < m->nobj; i++) {
        const uint8_t *o = f + objtab + 24 * i;
        uint32_t pmi = rd32(o + 12), pmc = rd32(o + 16), j;
        for (j = 0; j < pmc; j++) {
            uint32_t pg = pmi + j - 1;               /* 0-based global page */
            uint32_t r = frt + rd32(f + fpt + 4 * pg), end = frt + rd32(f + fpt + 4 * (pg + 1));
            uint8_t *page = m->base[i] + j * pagesize;
            while (r < end) {
                uint8_t stype = f[r], tflags = f[r + 1];
                int list = (stype & 0x20) != 0, count = 1, k;
                int32_t srcoff = 0;
                uint32_t tobj, toff = 0, target;
                r += 2;
                if (list) count = f[r++];
                else { srcoff = (int16_t)rd16(f + r); r += 2; }
                if ((tflags & 0x03) != 0) { fail(err, errlen, "import or entry fixup"); goto bad; }
                if (tflags & 0x40) { tobj = rd16(f + r); r += 2; } else { tobj = f[r++]; }
                if ((stype & 0x0F) != 0x02) {
                    if (tflags & 0x10) { toff = rd32(f + r); r += 4; } else { toff = rd16(f + r); r += 2; }
                }
                if (tobj < 1 || (int)tobj > m->nobj) { fail(err, errlen, "fixup to bad object"); goto bad; }
                target = (uint32_t)(uintptr_t)m->base[tobj - 1] + toff;
                if (tflags & 0x04) {                 /* additive */
                    if (tflags & 0x20) { target += rd32(f + r); r += 4; } else { target += rd16(f + r); r += 2; }
                }
                for (k = 0; k < count; k++) {
                    if (list) { srcoff = (int16_t)rd16(f + r); r += 2; }
                    if (apply(m, page, srcoff, stype, target) < 0) {
                        fail(err, errlen, "unsupported fixup source type");
                        goto bad;
                    }
                }
            }
        }
    }

    /* Module name: resident-name table entry with ordinal 0. */
    {
        const uint8_t *rn = f + m->le + rd32(h + 0x58);
        size_t n = rn[0] < sizeof m->name ? rn[0] : sizeof m->name - 1;
        memcpy(m->name, rn + 1, n);
        m->name[n] = 0;
    }
    return m;
bad:
    le_unload(m);
    return NULL;
}

/* Walk a name table for 'name'; returns its ordinal or 0. */
static uint16_t find_name(const uint8_t *t, const uint8_t *end, const char *name)
{
    size_t want = strlen(name);
    while ((!end || t < end) && t[0]) {
        uint8_t n = t[0];
        if (n == want && !memcmp(t + 1, name, n))
            return rd16(t + 1 + n);
        t += 3 + n;
    }
    return 0;
}

void *le_proc(const le_module *m, const char *name)
{
    const uint8_t *f = m->file, *h = f + m->le, *e;
    uint16_t ord = find_name(f + m->le + rd32(h + 0x58), NULL, name);
    uint16_t cur = 1;
    if (!ord && rd32(h + 0x88))
        ord = find_name(f + rd32(h + 0x88), f + rd32(h + 0x88) + rd32(h + 0x8C), name);
    if (!ord)
        return NULL;
    /* Entry table: bundles of (count, type[, object]) followed by entries. */
    e = f + m->le + rd32(h + 0x5C);
    while (e[0]) {
        uint8_t count = e[0], type = e[1], k;
        uint16_t obj = 0;
        int size = 0;
        e += 2;
        if (type == 0) { cur += count; continue; }
        obj = rd16(e); e += 2;
        switch (type) {
        case 1: size = 3; break;
        case 2: size = 5; break;
        case 3: size = 5; break;
        default: return NULL;
        }
        for (k = 0; k < count; k++, cur++, e += size) {
            if (cur == ord) {
                uint32_t off = (type == 3) ? rd32(e + 1) : rd16(e + 1);
                if (obj < 1 || obj > m->nobj)
                    return NULL;
                return m->base[obj - 1] + off;
            }
        }
    }
    return NULL;
}

const char *le_module_name(const le_module *m) { return m->name; }
uint32_t le_module_flags(const le_module *m) { return m->flags; }
int le_object_count(const le_module *m) { return m->nobj; }
void *le_object_base(const le_module *m, int i) { return (i >= 0 && i < m->nobj) ? m->base[i] : NULL; }
uint32_t le_object_size(const le_module *m, int i) { return (i >= 0 && i < m->nobj) ? m->size[i] : 0; }
int le_fixup_count(const le_module *m) { return m->nfixups; }

void *le_entry(const le_module *m)
{
    if (m->eip_obj < 1 || (int)m->eip_obj > m->nobj)
        return NULL;
    return m->base[m->eip_obj - 1] + m->eip;
}

void le_unload(le_module *m)
{
    int i;
    if (!m)
        return;
    for (i = 0; i < MAX_OBJECTS; i++)
        if (m->raw[i])
            m->io->free(m->raw[i]);
    if (m->file)
        m->io->free(m->file);
    m->io->free(m);
}
