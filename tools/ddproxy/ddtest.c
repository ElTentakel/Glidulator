/* ddtest [hold|ps|ds]
 *   (no argument)  which DDRAW.DLL does the process get, does DirectDrawCreate
 *                  and QueryInterface work; "hold" keeps it loaded until Enter
 *   ps             list every process that has a module DDRAW.DLL loaded
 *   ds             DirectSoundCreate only, then the same list (DSOUND starts
 *                  DDHELP.EXE, which loads the system DDRAW.DLL) */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>

/* Lists every process that has a module named DDRAW.DLL loaded. */
static void list_ddraw(void)
{
    HANDLE ps = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32First(ps, &pe); ok; ok = Process32Next(ps, &pe)) {
        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pe.th32ProcessID);
        MODULEENTRY32 me;
        me.dwSize = sizeof me;
        int any = 0;
        for (BOOL k = Module32First(ms, &me); k; k = Module32Next(ms, &me))
            if (!lstrcmpiA(me.szModule, "DDRAW.DLL") ||
                !lstrcmpiA(me.szModule, "DDRAW")) {
                printf("  %-40s %p %s (refs %lu)\n", pe.szExeFile,
                       (void *)me.hModule, me.szExePath, me.GlblcntUsage);
                any = 1;
            }
        if (!any)
            printf("  %-40s -\n", pe.szExeFile);
        CloseHandle(ms);
    }
    CloseHandle(ps);
}

int main(int argc, char **argv)
{
    char p[MAX_PATH] = "";
    if (argc > 1 && !strcmp(argv[1], "ds")) {
        /* DirectSound only: does it make DDHELP load DDRAW.DLL? */
        typedef HRESULT (WINAPI *dsc_t)(GUID *, void **, void *);
        typedef HRESULT (WINAPI *coop_t)(void *, HWND, DWORD);
        HMODULE dsm = LoadLibraryA("DSOUND.DLL");
        dsc_t dsc = dsm ? (dsc_t)GetProcAddress(dsm, "DirectSoundCreate") : NULL;
        void *ds = NULL;
        HRESULT r = dsc ? dsc(NULL, &ds, NULL) : E_FAIL;
        printf("DirectSoundCreate = %08lx, %p\n", r, ds);
        if (ds) {
            r = ((coop_t)(*(void ***)ds)[6])(ds, GetForegroundWindow(), 2);
            printf("SetCooperativeLevel(PRIORITY) = %08lx\n", r);
        }
        list_ddraw();
        printf("press Enter\n");
        getchar();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "ps")) {
        list_ddraw();
        printf("press Enter\n");
        getchar();
        return 0;
    }
    HMODULE m = LoadLibraryA("DDRAW.DLL");
    if (m)
        GetModuleFileNameA(m, p, sizeof p);
    printf("LoadLibrary(DDRAW.DLL) = %p: %s\n", (void *)m, p);
    typedef HRESULT (WINAPI *ddc_t)(GUID *, void **, void *);
    ddc_t f = m ? (ddc_t)GetProcAddress(m, "DirectDrawCreate") : NULL;
    void *dd = NULL;
    HRESULT hr = f ? f(NULL, &dd, NULL) : E_FAIL;
    printf("DirectDrawCreate = %08lx, %p\n", hr, dd);
    if (dd) {
        /* IDirectDraw::QueryInterface(IID_IDirectDraw2), then IID_IDirect3D2 */
        static const GUID dd2 = { 0xB3A6F3E0, 0x2B43, 0x11CF,
            { 0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };
        static const GUID d3d2 = { 0x6aae1ec1, 0x662a, 0x11d0,
            { 0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a } };
        typedef HRESULT (WINAPI *qi_t)(void *, const GUID *, void **);
        typedef ULONG (WINAPI *rel_t)(void *);
        void *dd2p = NULL, *d3d = NULL;
        hr = ((qi_t)(*(void ***)dd)[0])(dd, &dd2, &dd2p);
        printf("QI IDirectDraw2 = %08lx, %p\n", hr, dd2p);
        hr = ((qi_t)(*(void ***)dd)[0])(dd, &d3d2, &d3d);
        printf("QI IDirect3D2 = %08lx, %p\n", hr, d3d);
        if (d3d)
            ((rel_t)(*(void ***)d3d)[2])(d3d);
        if (dd2p)
            ((rel_t)(*(void ***)dd2p)[2])(dd2p);
        printf("Release = %lu\n", ((rel_t)(*(void ***)dd)[2])(dd));
    }
    if (argc > 1 && !strcmp(argv[1], "hold")) {
        printf("holding, press Enter\n");
        getchar();
    }
    return 0;
}
