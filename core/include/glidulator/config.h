#ifndef GLIDULATOR_CONFIG_H
#define GLIDULATOR_CONFIG_H

#include <stddef.h>

/* Settings file GLIDULAT.CFG: lines NAME=VALUE (e.g. "SST_GAMMA=1.0"),
 * names case-insensitive, blanks around '=' allowed, '#' at the start of a
 * line = comment. Shared reader for the platforms that use the file (DOS
 * OVL, Windows), see platform_get_config().
 *
 * glide_config_find(): value of name in text (zero-terminated) into buf;
 * returns 1 if found and not empty.
 * glide_config_from_file(): the same for a file (platform_read_file, first
 * GLIDE_CONFIG_MAX bytes, enough for the commented example file). */
#define GLIDE_CONFIG_MAX 8192
int glide_config_find(const char *text, const char *name, char *buf, size_t n);
int glide_config_from_file(const char *file, const char *name, char *buf, size_t n);

#endif /* GLIDULATOR_CONFIG_H */
