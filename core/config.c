/* Reading GLIDULAT.CFG (see glidulator/config.h). Without C runtime
 * functions: the OVL has no initialised CRT. */

#include "glidulator/config.h"
#include "glidulator/platform.h"

static int upper(int c) {
    return (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
}

int glide_config_find(const char *text, const char *name, char *buf, size_t n) {
    if (!n)
        return 0;
    for (const char *line = text; *line;) {
        const char *p = line, *q = name;
        while (*p == ' ' || *p == '\t')
            p++;
        while (*q && upper((unsigned char) *p) == upper((unsigned char) *q)) {
            p++;
            q++;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*q && *p == '=') {
            size_t k = 0;
            p++;
            while (*p == ' ' || *p == '\t')
                p++;
            while (*p && *p != '\r' && *p != '\n' && *p != ' ' && *p != '\t' && k + 1 < n)
                buf[k++] = *p++;
            buf[k] = 0;
            return k > 0;
        }
        while (*line && *line != '\n')
            line++;
        if (*line)
            line++;
    }
    return 0;
}

/* Static instead of on the stack: the game's stack (OVL) is small, and
 * Glide calls come from one thread only. */
int glide_config_from_file(const char *file, const char *name, char *buf, size_t n) {
    static char cfg[GLIDE_CONFIG_MAX + 1];
    long len = platform_read_file(file, 0, cfg, sizeof cfg - 1);
    if (len <= 0)
        return 0;
    cfg[len] = 0;
    return glide_config_find(cfg, name, buf, n);
}
