/* leload.h - clean-room loader for DOS/4G LE DLLs (PRD D18).
 *
 * Written from the public LE/LX executable format description. Loads a
 * DLL such as GLIDE2X.OVL into the current flat address space, applies its
 * internal fixups and resolves exports by name, so a test program can
 * call either MGA-Glide or a retail runtime without the 3dfx import
 * library. Allocation and file access are callbacks so the same code can
 * run inside a DLL (the trace proxy) as well as in an EXE. */
#ifndef LELOAD_H
#define LELOAD_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *(*alloc)(size_t bytes);          /* zero-filled memory */
    void  (*free)(void *p);
    /* Read the whole file; returns a buffer from alloc() or NULL. */
    void *(*read_file)(const char *path, size_t *size);
    void  (*log)(const char *msg);         /* optional */
} le_io;

typedef struct le_module le_module;

le_module *le_load(const le_io *io, const char *path, char *err, size_t errlen);
void      *le_proc(const le_module *m, const char *name);   /* NULL if missing */
const char *le_module_name(const le_module *m);
uint32_t   le_module_flags(const le_module *m);
void      *le_entry(const le_module *m);                    /* initial EIP, or NULL */
int        le_object_count(const le_module *m);
void      *le_object_base(const le_module *m, int index);   /* 0-based */
uint32_t   le_object_size(const le_module *m, int index);
int        le_fixup_count(const le_module *m);
void       le_unload(le_module *m);

#endif
