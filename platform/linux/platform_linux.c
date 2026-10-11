/* Reference/test implementation of platform.h for Linux. Render workers
 * (core/render_mt.c) run as pthreads. */

#define _DEFAULT_SOURCE

#include "glidulator/platform.h"
#include "../common/cpu_gcc.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

int platform_core_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int) n : 1;
}

/* Linux saves the SSE registers (FXSAVE/XSAVE). */
unsigned platform_cpu_features(void) {
    return cpu_features_gcc(1);
}

void *platform_alloc(size_t n) {
    return malloc(n);
}

void platform_free(void *p) {
    free(p);
}

void *platform_alloc_exec(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void platform_free_exec(void *p, size_t n) {
    if (p)
        munmap(p, n);
}

uint64_t platform_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

struct platform_worker {
    pthread_t          thread;
    platform_worker_fn fn;
    void              *arg;
};

static void *worker_main(void *p) {
    platform_worker *w = (platform_worker *) p;
    w->fn(w->arg);
    return NULL;
}

platform_worker *platform_spawn_worker(int core_id, platform_worker_fn fn, void *arg) {
    platform_worker *w = (platform_worker *) malloc(sizeof *w);
    (void) core_id;
    if (!w)
        return NULL;
    w->fn  = fn;
    w->arg = arg;
    if (pthread_create(&w->thread, NULL, worker_main, w)) {
        free(w);
        return NULL;
    }
    return w;
}

void platform_join_worker(platform_worker *w) {
    if (!w)
        return;
    pthread_join(w->thread, NULL);
    free(w);
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

void platform_prepare_window(void) {
}

void platform_display_prefer_deep(int on) {
    (void) on;
}

/* platform_blit() is NOT defined here: what happens to a finished frame
 * differs per target (a test captures it for verification, see
 * targets/glide_test_linux/main.c), while the other platform_* functions
 * are the same for every Linux program. */

void platform_display_open(int w, int h) {
    (void)w;
    (void)h;
}

void platform_display_close(void) {
}

int platform_display_lost(void) {
    return 0;
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
