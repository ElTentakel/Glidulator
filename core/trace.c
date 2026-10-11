/* See core/include/glidulator/trace.h. */

#include "glidulator/trace.h"
#include "glidulator/platform.h"

int g_glide_mute;

static glide_trace_site *g_sites;
static unsigned long     g_swaps;

/* Minimal line building without sprintf (no CRT in the OVL). */
static char *put_str(char *p, char *end, const char *s) {
    while (*s && p < end)
        *p++ = *s++;
    return p;
}

static char *put_ulong(char *p, char *end, unsigned long v) {
    char tmp[12];
    int  n = 0;
    do {
        tmp[n++] = (char) ('0' + v % 10);
        v /= 10;
    } while (v && n < (int) sizeof tmp);
    while (n && p < end)
        *p++ = tmp[--n];
    return p;
}

void glide_trace_hit(glide_trace_site *site) {
    if (g_glide_mute)
        return;
    if (site->count++ == 0) {
        site->next = g_sites;
        g_sites    = site;
        char  line[96];
        char *end = line + sizeof line - 2;
        char *p   = put_str(line, end, "first call: ");
        p         = put_str(p, end, site->name);
        p         = put_str(p, end, " (after swap ");
        p         = put_ulong(p, end, g_swaps);
        p         = put_str(p, end, ")\n");
        *p        = 0;
        platform_log(line);
    }
}

void glide_trace_dump(const char *why) {
    char  line[96];
    char *end = line + sizeof line - 2;
    char *p   = put_str(line, end, "--- Counters (");
    p         = put_str(p, end, why);
    p         = put_str(p, end, ") ---\n");
    *p        = 0;
    platform_log(line);
    for (glide_trace_site *s = g_sites; s; s = s->next) {
        p  = put_str(line, end, "  ");
        p  = put_str(p, end, s->name);
        p  = put_str(p, end, " ");
        p  = put_ulong(p, end, s->count);
        p  = put_str(p, end, "\n");
        *p = 0;
        platform_log(line);
    }
}

void glide_trace_swap(void) {
    g_swaps++;
    for (unsigned long n = 1; n <= g_swaps && n <= 100000000ul; n *= 10) {
        if (g_swaps == n) {
            char  why[24];
            char *p = put_str(why, why + sizeof why - 1, "Swap ");
            p       = put_ulong(p, why + sizeof why - 1, g_swaps);
            *p      = 0;
            glide_trace_dump(why);
        }
    }
}

void glide_trace_note(const char *what, unsigned long a, unsigned long b) {
    char  line[96];
    char *end = line + sizeof line - 2;
    char *p   = put_str(line, end, what);
    p         = put_str(p, end, ": ");
    p         = put_ulong(p, end, a);
    p         = put_str(p, end, " x ");
    p         = put_ulong(p, end, b);
    p         = put_str(p, end, "\n");
    *p        = 0;
    platform_log(line);
}

void glide_trace_floats(const char *what, const float *v, int n) {
    char  line[192];
    char *end = line + sizeof line - 2;
    char *p   = put_str(line, end, what);
    for (int i = 0; i < n && i < 8; i++) {
        double x = v[i];
        p        = put_str(p, end, x < 0 ? " -" : " ");
        if (x < 0)
            x = -x;
        unsigned long whole = (unsigned long) x;
        unsigned long frac  = (unsigned long) ((x - (double) whole) * 1000.0 + 0.5);
        if (frac >= 1000) {
            whole++;
            frac -= 1000;
        }
        p = put_ulong(p, end, whole);
        p = put_str(p, end, ".");
        if (frac < 100)
            p = put_str(p, end, "0");
        if (frac < 10)
            p = put_str(p, end, "0");
        p = put_ulong(p, end, frac);
    }
    p  = put_str(p, end, "\n");
    *p = 0;
    platform_log(line);
}
