#ifndef GLIDULATOR_VERSION_H
#define GLIDULATOR_VERSION_H

/* Product version: the only place to change it. The Makefile reads
 * GLIDULATOR_VERSION from here (make dist: package name), the DLL's
 * VERSIONINFO resource (targets/glide_dll/glide2x.rc) uses the numeric
 * parts, the trace/debug variants log it. */
#define GLIDULATOR_VERSION_MAJOR 0
#define GLIDULATOR_VERSION_MINOR 9
#define GLIDULATOR_VERSION_PATCH 0
#define GLIDULATOR_VERSION       "0.9.0"

#endif
