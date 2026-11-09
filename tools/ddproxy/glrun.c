/*
 * GLRUN.EXE <program> [arguments]
 *
 * Starts a program with GLDDRAW.DLL (from GLRUN's folder) loaded into it
 * before its entry point; the DLL redirects the program's DirectDraw
 * imports.  The working directory is the program's folder.  Messages go
 * to DDPROXY.LOG next to GLRUN.EXE.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "inject.h"

static char log_path[MAX_PATH];

static void glog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof buf - 3)
        n = sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    HANDLE h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD w;
    WriteFile(h, buf, n, &w, NULL);
    CloseHandle(h);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR args, int show)
{
    (void)inst; (void)prev; (void)show;
    char dll[MAX_PATH], dir[MAX_PATH], cmd[1024];
    GetModuleFileNameA(NULL, dll, sizeof dll);
    strcpy(log_path, dll);
    strcpy(strrchr(log_path, '\\') + 1, "DDPROXY.LOG");
    strcpy(strrchr(dll, '\\') + 1, "GLDDRAW.DLL");
    while (*args == ' ')
        args++;
    if (!*args) {
        MessageBoxA(NULL, "Usage: GLRUN <program> [arguments]", "GLRUN", MB_OK);
        return 1;
    }
    lstrcpynA(cmd, args, sizeof cmd);

    /* working directory = folder of the program */
    char exe[MAX_PATH], *fp;
    int i = 0;
    const char *p = args;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < MAX_PATH - 1)
            exe[i++] = *p++;
    } else {
        while (*p && *p != ' ' && i < MAX_PATH - 1)
            exe[i++] = *p++;
    }
    exe[i] = 0;
    if (!SearchPathA(NULL, exe, ".exe", sizeof dir, dir, &fp)) {
        glog("glrun: %s not found", exe);
        MessageBoxA(NULL, exe, "GLRUN: program not found", MB_OK);
        return 1;
    }
    *fp = 0;
    if (fp > dir + 3)
        fp[-1] = 0;

    glog("==== glrun: %s (dir %s, dll %s)", cmd, dir, dll);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!inject_create(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi,
                       dll, CreateProcessA, glog)) {
        glog("glrun: CreateProcess failed (error %lu)", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
