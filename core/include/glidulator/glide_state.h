#ifndef GLIDULATOR_GLIDE_STATE_H
#define GLIDULATOR_GLIDE_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Per-process library state, created by the DLL entry point on attach
 * (targets/glide_dll) and by the test harness. The Glide state itself is
 * global (one board per process, as in 3dfx's Glide), see
 * core/glide_internal.h. */

typedef struct glide_state glide_state;

glide_state *glide_init(void);
void glide_shutdown(glide_state *gs);

#ifdef __cplusplus
}
#endif

#endif /* GLIDULATOR_GLIDE_STATE_H */
