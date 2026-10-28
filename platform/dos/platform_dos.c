/* platform.h implementation for DOS with DJGPP (32-bit protected mode via
 * CWSDPMI). Only a build check of the portable core with a third compiler:
 * no output, no additional cores (the real DOS module is GLIDE2X.OVL,
 * platform/dos_ovl). */

#include "glidulator/platform.h"
#include "../common/cpu_gcc.h"

#include <stdio.h>

#include <dos.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

int platform_core_count(void) {
    return 1;
}

const char *platform_smp_mode(void) {
    return 0;
}

/* Never SSE: whether the DPMI host (CWSDPMI, Windows DOS box, ...) sets
 * CR4.OSFXSR cannot be checked from ring 3, and without it every SSE
 * instruction raises #UD. */
unsigned platform_cpu_features(void) {
    return cpu_features_gcc(0);
}

void *platform_alloc(size_t n) {
    return malloc(n);
}

void platform_free(void *p) {
    free(p);
}

/* DJGPP: flat model, DPMI memory is executable. */
void *platform_alloc_exec(size_t n) {
    return malloc(n);
}

void platform_free_exec(void *p, size_t n) {
    (void) n;
    free(p);
}

uint64_t platform_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t) tv.tv_sec * 1000000ull + (uint64_t) tv.tv_usec;
}

struct platform_worker {
    int unused;
};

/* no additional cores: core/ renders on one core */
platform_worker *platform_spawn_worker(int core_id, platform_worker_fn fn, void *arg) {
    (void) core_id;
    (void) fn;
    (void) arg;
    return NULL;
}

void platform_join_worker(platform_worker *w) {
    (void) w;
}

/* Threads do not die individually, and none can be aborted cleanly. */
int platform_worker_failed(platform_worker *w) {
    (void) w;
    return 0;
}

int platform_kill_worker(platform_worker *w) {
    (void) w;
    return 0;
}

/* Vsync: not implemented */
int platform_wait_vretrace(int n) {
    (void) n;
    return 0;
}

/* platform_blit() is not here: the test program defines it
 * (targets/glide_dos_test). */

void platform_display_open(int w, int h) {
    (void) w;
    (void) h;
}

void platform_display_close(void) {
}

int platform_display_lost(void) {
    return 0;
}

int platform_screenshot_key(void) {
    return 0;
}

void platform_prepare_window(void) {
}

void platform_display_prefer_deep(int on) {
    (void) on;
}

int platform_display_size(int *w, int *h) {
    (void) w;
    (void) h;
    return 0;
}

/* No own video output with gamma, see platform.h. */
void platform_set_gamma(const uint8_t lut[256]) {
    (void) lut;
}

int platform_get_config(const char *name, char *buf, size_t n) {
    const char *v = getenv(name);
    if (!v || !n)
        return 0;
    strncpy(buf, v, n - 1);
    buf[n - 1] = 0;
    return 1;
}

void platform_log(const char *line) {
    fputs(line, stderr);
}

int platform_write_file(const char *name, const void *data, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(data, 1, len, f);
    return (fclose(f) == 0 && n == len) ? 0 : -1;
}

long platform_read_file(const char *name, size_t offset, void *buf, size_t len) {
    FILE *f = fopen(name, "rb");
    if (!f)
        return -1;
    size_t n = 0;
    if (fseek(f, (long) offset, SEEK_SET) == 0)
        n = fread(buf, 1, len, f);
    fclose(f);
    return (long) n;
}
