#ifndef INJECT_H
#define INJECT_H
#include <windows.h>

typedef BOOL (WINAPI *create_fn)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
                                 LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                 LPCSTR, LPSTARTUPINFOA,
                                 LPPROCESS_INFORMATION);
typedef void (*log_fn)(const char *fmt, ...);

/* CreateProcessA that loads `dll' into the new process before its entry
 * point runs.  Returns FALSE only when the process could not be created;
 * injection problems are logged and the process runs without the DLL. */
BOOL inject_create(const char *app, char *cmd, SECURITY_ATTRIBUTES *pa,
                   SECURITY_ATTRIBUTES *ta, BOOL inherit, DWORD flags,
                   void *env, const char *dir, STARTUPINFOA *si,
                   PROCESS_INFORMATION *pi, const char *dll,
                   create_fn create, log_fn log);

#endif
