/* glide2x.dll entry point: creates the per-process library state on
 * attach and releases it on detach. The Glide exports themselves come from
 * core/ (names in glide2x.def). */

#define _WIN32_WINNT 0x0400
#include <windows.h>

#include "glidulator/glide_state.h"

static glide_state *g_gs = NULL;

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst;
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_gs = glide_init();
        break;
    case DLL_PROCESS_DETACH:
        glide_shutdown(g_gs);
        g_gs = NULL;
        break;
    default:
        break;
    }
    return TRUE;
}
