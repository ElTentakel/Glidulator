/*
 * Logging proxy ddraw.dll for Windows 9x.
 *
 * Placed next to a game's EXE, it forwards everything to the system
 * DDRAW.DLL and writes DDPROXY.LOG next to itself: which module was
 * loaded, which DirectDraw/Direct3D interfaces the game asks for, the
 * methods it calls (first calls with arguments, totals at exit),
 * vertex types, render states and surface descriptions.
 *
 * IDirectDraw*, IDirect3D* and IDirect3DDevice* objects are wrapped
 * (wrapper object whose vtable consists of generic thunks); surfaces,
 * viewports, textures and so on stay the real objects.  A call through a
 * thunk logs on entry, swaps in the real `this', and returns through a
 * post stub that logs the result and wraps objects returned by
 * QueryInterface/CreateDevice.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define FW(n) extern void *p_##n;
#include "forward.h"
#undef FW

#include "inject.h"
#include <tlhelp32.h>

static HMODULE self_mod, real_mod;

/* "fakehal" in DDPROXY.CFG next to the DLL: pretend the software RGB
 * device is a hardware (HAL) device, for games that refuse to start
 * without 3D hardware.  Only for finding out what such a game asks for. */
static int fake_hal;
/* "fakepal8": additionally offer an 8-bit palettized texture format, to
 * see whether a game would use one. */
static int fake_pal8;
static char log_path[MAX_PATH];

static void dlog(const char *fmt, ...)
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

/* ---- interface types ------------------------------------------------ */

enum { T_DD, T_DD2, T_DD4, T_DD7, T_D3D, T_D3D2, T_D3D3, T_D3D7,
       T_DEV, T_DEV2, T_DEV3, T_DEV7, T_VB, T_VB7, T_MAT3, T_COUNT };

static const char *type_name[T_COUNT] = {
    "IDirectDraw", "IDirectDraw2", "IDirectDraw4", "IDirectDraw7",
    "IDirect3D", "IDirect3D2", "IDirect3D3", "IDirect3D7",
    "IDirect3DDevice", "IDirect3DDevice2", "IDirect3DDevice3",
    "IDirect3DDevice7", "IDirect3DVertexBuffer", "IDirect3DVertexBuffer7", "IDirect3DMaterial3" };

#define DD_COMMON "QueryInterface", "AddRef", "Release", "Compact", \
    "CreateClipper", "CreatePalette", "CreateSurface", "DuplicateSurface", \
    "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", \
    "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", \
    "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus", \
    "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", \
    "SetDisplayMode", "WaitForVerticalBlank"
static const char *m_dd[] = { DD_COMMON, NULL };
static const char *m_dd2[] = { DD_COMMON, "GetAvailableVidMem", NULL };
static const char *m_dd4[] = { DD_COMMON, "GetAvailableVidMem",
    "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
    "GetDeviceIdentifier", NULL };
static const char *m_dd7[] = { DD_COMMON, "GetAvailableVidMem",
    "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
    "GetDeviceIdentifier", "StartModeTest", "EvaluateMode", NULL };
static const char *m_d3d[] = { "QueryInterface", "AddRef", "Release",
    "Initialize", "EnumDevices", "CreateLight", "CreateMaterial",
    "CreateViewport", "FindDevice", NULL };
static const char *m_d3d2[] = { "QueryInterface", "AddRef", "Release",
    "EnumDevices", "CreateLight", "CreateMaterial", "CreateViewport",
    "FindDevice", "CreateDevice", NULL };
static const char *m_d3d3[] = { "QueryInterface", "AddRef", "Release",
    "EnumDevices", "CreateLight", "CreateMaterial", "CreateViewport",
    "FindDevice", "CreateDevice", "CreateVertexBuffer",
    "EnumZBufferFormats", "EvictManagedTextures", NULL };
static const char *m_d3d7[] = { "QueryInterface", "AddRef", "Release",
    "EnumDevices", "CreateDevice", "CreateVertexBuffer",
    "EnumZBufferFormats", "EvictManagedTextures", NULL };
static const char *m_dev[] = { "QueryInterface", "AddRef", "Release",
    "Initialize", "GetCaps", "SwapTextureHandles", "CreateExecuteBuffer",
    "GetStats", "Execute", "AddViewport", "DeleteViewport", "NextViewport",
    "Pick", "GetPickRecords", "EnumTextureFormats", "CreateMatrix",
    "SetMatrix", "GetMatrix", "DeleteMatrix", "BeginScene", "EndScene",
    "GetDirect3D", NULL };
#define DEV23_A "QueryInterface", "AddRef", "Release", "GetCaps"
#define DEV23_B "GetStats", "AddViewport", "DeleteViewport", "NextViewport", \
    "EnumTextureFormats", "BeginScene", "EndScene", "GetDirect3D", \
    "SetCurrentViewport", "GetCurrentViewport", "SetRenderTarget", \
    "GetRenderTarget", "Begin", "BeginIndexed", "Vertex", "Index", "End", \
    "GetRenderState", "SetRenderState", "GetLightState", "SetLightState", \
    "SetTransform", "GetTransform", "MultiplyTransform", "DrawPrimitive", \
    "DrawIndexedPrimitive", "SetClipStatus", "GetClipStatus"
static const char *m_dev2[] = { DEV23_A, "SwapTextureHandles", DEV23_B, NULL };
static const char *m_dev3[] = { DEV23_A, DEV23_B, "DrawPrimitiveStrided",
    "DrawIndexedPrimitiveStrided", "DrawPrimitiveVB",
    "DrawIndexedPrimitiveVB", "ComputeSphereVisibility", "GetTexture",
    "SetTexture", "GetTextureStageState", "SetTextureStageState",
    "ValidateDevice", NULL };
static const char *m_dev7[] = { "QueryInterface", "AddRef", "Release",
    "GetCaps", "EnumTextureFormats", "BeginScene", "EndScene",
    "GetDirect3D", "SetRenderTarget", "GetRenderTarget", "Clear",
    "SetTransform", "GetTransform", "SetViewport", "MultiplyTransform",
    "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "SetRenderState", "GetRenderState", "BeginStateBlock", "EndStateBlock",
    "PreLoad", "DrawPrimitive", "DrawIndexedPrimitive", "SetClipStatus",
    "GetClipStatus", "DrawPrimitiveStrided", "DrawIndexedPrimitiveStrided",
    "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ComputeSphereVisibility",
    "GetTexture", "SetTexture", "GetTextureStageState",
    "SetTextureStageState", "ValidateDevice", "ApplyStateBlock",
    "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "Load",
    "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane",
    "GetInfo", NULL };

static const char *m_vb[] = { "QueryInterface", "AddRef", "Release",
    "Lock", "Unlock", "ProcessVertices", "GetVertexBufferDesc", "Optimize",
    NULL };
static const char *m_vb7[] = { "QueryInterface", "AddRef", "Release",
    "Lock", "Unlock", "ProcessVertices", "GetVertexBufferDesc", "Optimize",
    "ProcessVerticesStrided", NULL };

static const char *m_mat3[] = { "QueryInterface", "AddRef", "Release",
    "SetMaterial", "GetMaterial", "GetHandle", NULL };

static const char **methods[T_COUNT] = { m_dd, m_dd2, m_dd4, m_dd7, m_d3d,
    m_d3d2, m_d3d3, m_d3d7, m_dev, m_dev2, m_dev3, m_dev7, m_vb, m_vb7,
    m_mat3 };

#define MAXM 64
static int nmethods[T_COUNT];
static unsigned long calls[T_COUNT][MAXM];
static unsigned long fails[T_COUNT][MAXM];

/* ---- known GUIDs ---------------------------------------------------- */

struct guid_name { DWORD d1; WORD d2, d3; const char *name; int type; };
static const struct guid_name guids[] = {
    { 0x6C14DB80, 0xA733, 0x11CE, "IID_IDirectDraw", T_DD },
    { 0xB3A6F3E0, 0x2B43, 0x11CF, "IID_IDirectDraw2", T_DD2 },
    { 0x9c59509a, 0x39bd, 0x11d1, "IID_IDirectDraw4", T_DD4 },
    { 0x15e65ec0, 0x3b9c, 0x11d2, "IID_IDirectDraw7", T_DD7 },
    { 0x3BBA0080, 0x2421, 0x11CF, "IID_IDirect3D", T_D3D },
    { 0x6aae1ec1, 0x662a, 0x11d0, "IID_IDirect3D2", T_D3D2 },
    { 0xbb223240, 0xe72b, 0x11d0, "IID_IDirect3D3", T_D3D3 },
    { 0xf5049e77, 0x4861, 0x11d2, "IID_IDirect3D7", T_D3D7 },
    { 0x64108800, 0x957d, 0x11d0, "IID_IDirect3DDevice", T_DEV },
    { 0x93281501, 0x8cf8, 0x11d0, "IID_IDirect3DDevice2", T_DEV2 },
    { 0xb0ab3b60, 0x33d7, 0x11d1, "IID_IDirect3DDevice3", T_DEV3 },
    { 0xf5049e79, 0x4861, 0x11d2, "IID_IDirect3DDevice7", T_DEV7 },
    { 0x84e63de0, 0x46aa, 0x11cf, "IID_IDirect3DHALDevice", -1 },
    { 0xA4665C60, 0x2673, 0x11CF, "IID_IDirect3DRGBDevice", -1 },
    { 0xF2086B20, 0x259F, 0x11CF, "IID_IDirect3DRampDevice", -1 },
    { 0x881949a1, 0xd6f3, 0x11d0, "IID_IDirect3DMMXDevice", -1 },
    { 0x50936643, 0x13e9, 0x11d1, "IID_IDirect3DRefDevice", -1 },
    { 0x8767df22, 0xbacc, 0x11d1, "IID_IDirect3DNullDevice", -1 },
    { 0xf5049e78, 0x4861, 0x11d2, "IID_IDirect3DTnLHalDevice", -1 },
    { 0x6C14DB81, 0xA733, 0x11CE, "IID_IDirectDrawSurface", -1 },
    { 0x57805885, 0x6eec, 0x11cf, "IID_IDirectDrawSurface2", -1 },
    { 0xDA044E00, 0x69B2, 0x11D0, "IID_IDirectDrawSurface3", -1 },
    { 0x0B2B8630, 0xAD35, 0x11D0, "IID_IDirectDrawSurface4", -1 },
    { 0x06675a80, 0x3b9b, 0x11d2, "IID_IDirectDrawSurface7", -1 },
    { 0x2CDCD9E0, 0x25A0, 0x11CF, "IID_IDirect3DTexture", -1 },
    { 0x93281502, 0x8cf8, 0x11d0, "IID_IDirect3DTexture2", -1 },
    { 0x7a503555, 0x4a83, 0x11d1, "IID_IDirect3DVertexBuffer", T_VB },
    { 0xf5049e7d, 0x4861, 0x11d2, "IID_IDirect3DVertexBuffer7", T_VB7 },
    { 0x4B9F0EE0, 0x0D7E, 0x11D0, "IID_IDirectDrawColorControl", -1 },
    { 0x69C11C3E, 0xB46B, 0x11D1, "IID_IDirectDrawGammaControl", -1 },
    { 0x00000000, 0x0000, 0x0000, "IID_IUnknown", -1 },
};

static const char *guid_str(const GUID *g, int *type)
{
    static char buf[8][48];
    static int k;
    if (type)
        *type = -1;
    if (!g)
        return "NULL";
    if (IsBadReadPtr(g, sizeof *g))
        return "(bad ptr)";
    for (unsigned i = 0; i < sizeof guids / sizeof guids[0]; i++)
        if (g->Data1 == guids[i].d1 && g->Data2 == guids[i].d2 &&
            g->Data3 == guids[i].d3 &&
            (guids[i].d1 || g->Data4[0] == 0xC0)) {
            if (type)
                *type = guids[i].type;
            return guids[i].name;
        }
    char *b = buf[k++ & 7];
    sprintf(b, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
            g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
            g->Data4[7]);
    return b;
}

/* ---- wrappers ------------------------------------------------------- */

typedef struct wrap {
    void **vtbl;
    void *real;
    int type;
} wrap;

static void *vtables[T_COUNT][MAXM];

#define MAXWRAP 1024
static wrap wraps[MAXWRAP];
static int nwraps;

static void *wrap_obj(void *real, int type)
{
    if (!real || type < 0)
        return real;
    for (int i = 0; i < nwraps; i++)
        if (wraps[i].real == real && wraps[i].type == type)
            return &wraps[i];
    if (nwraps == MAXWRAP) {
        dlog("!! wrapper table full, %s %p not wrapped", type_name[type], real);
        return real;
    }
    wrap *w = &wraps[nwraps++];
    w->vtbl = vtables[type];
    w->real = real;
    w->type = type;
    dlog("   wrapped %s real=%p -> %p", type_name[type], real, (void *)w);
    return w;
}

/* Per-call record between thunk entry and the post stub (single game
 * thread assumed; nested calls through enumeration callbacks stack). */
struct frame { void *ret; wrap *w; int idx; DWORD a[6]; };
static struct frame stack[64];
static int depth;

#define LOG_FIRST 4

static const char *mname(int type, int idx)
{
    return idx < nmethods[type] ? methods[type][idx] : "?";
}

static unsigned char rs_seen[T_COUNT][256];

/* Draw calls per (type, method, primitive, vertex type / FVF). */
#define MAXDRAW 64
static struct { int t, idx; DWORD prim, vt; unsigned long n; } draws[MAXDRAW];
static int ndraws;

static void count_draw(int t, int idx, DWORD prim, DWORD vt)
{
    for (int i = 0; i < ndraws; i++)
        if (draws[i].t == t && draws[i].idx == idx && draws[i].prim == prim &&
            draws[i].vt == vt) {
            draws[i].n++;
            return;
        }
    if (ndraws < MAXDRAW) {
        draws[ndraws].t = t;
        draws[ndraws].idx = idx;
        draws[ndraws].prim = prim;
        draws[ndraws].vt = vt;
        draws[ndraws++].n = 1;
    }
}
static unsigned char tss_seen[8][32];

static void log_surface_desc(const DWORD *d)
{
    if (!d || IsBadReadPtr(d, 108))
        return;
    dlog("   desc size=%lu flags=%08lx %lux%lu backbuf=%lu mip/z=%lu caps=%08lx"
         " pf.flags=%08lx bits=%lu masks=%08lx/%08lx/%08lx/%08lx fourcc=%08lx",
         d[0], d[1], d[3], d[2], d[5], d[6], d[26], d[19], d[21], d[22],
         d[23], d[24], d[25], d[20]);
}

/* Real objects must never see a wrapper: arguments that carry another
 * wrapped object (vertex buffer or device) are swapped for the real one. */
static void unwrap_arg(DWORD *slot)
{
    wrap *w = (wrap *)*slot;
    if (w >= wraps && w < wraps + nwraps)
        *slot = (DWORD)w->real;
}

/* ---- enumeration callbacks: log what the game gets offered ---------- */

typedef HRESULT (WINAPI *enumdev_t)(GUID *, char *, char *, DWORD *, DWORD *,
                                    void *);
typedef HRESULT (WINAPI *enumpf_t)(DWORD *, void *);
static enumdev_t game_enumdev;
static enumpf_t game_enumtex;
static int pal8_offered; /* once per EnumTextureFormats call */
static void *game_ctx;

/* D3DDEVICEDESC (DX5/6): dpcTriCaps starts at dword 25; dwRasterCaps is
 * dword 27, dwTextureCaps 33, dwTextureFilterCaps 34; dwFlags 1,
 * dwDevCaps 3. */
static void log_devdesc(const char *what, const DWORD *d)
{
    if (!d || IsBadReadPtr(d, 160) || d[0] < 160)
        return;
    DWORD rc = d[27];
    dlog("     %s: flags=%08lx devcaps=%08lx raster=%08lx%s%s%s%s texcaps=%08lx"
         "%s filter=%08lx", what, d[1], d[3], rc,
         rc & 0x80 ? " FOGVERTEX" : "", rc & 0x100 ? " FOGTABLE" : "",
         rc & 0x100000 ? " WFOG" : "", rc & 0x200000 ? " ZFOG" : "",
         d[33], d[33] & 0x80 ? " ALPHAPALETTE" : "", d[34]);
}

static const GUID iid_hal = { 0x84e63de0, 0x46aa, 0x11cf,
    { 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e } };
static const GUID iid_rgb = { 0xA4665C60, 0x2673, 0x11CF,
    { 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56 } };

static HRESULT WINAPI my_enumdev(GUID *g, char *desc, char *name, DWORD *hal,
                                 DWORD *hel, void *ctx)
{
    (void)ctx;
    dlog("   device offered: %s \"%s\" (%s)", guid_str(g, NULL),
         name ? name : "", desc ? desc : "");
    log_devdesc("HAL", hal);
    log_devdesc("HEL", hel);
    HRESULT r = game_enumdev(g, desc, name, hal, hel, game_ctx);
    dlog("     game callback -> %ld", r);
    if (r && fake_hal && g && !memcmp(g, &iid_rgb, sizeof *g) &&
        hel && hel[0] <= 512) {
        /* offer the RGB device once more as "hardware" */
        DWORD fh[128], fs[128];
        memcpy(fh, hel, hel[0]);
        memcpy(fs, hel, hel[0]);
        fh[3] |= 0x80000 | 0x200 | 0x800; /* HWRASTERIZATION, TEXTUREVIDEOMEMORY, CANRENDERAFTERFLIP */
        dlog("   fake HAL device offered (RGB rasterizer underneath)");
        log_devdesc("fake HAL", fh);
        r = game_enumdev((GUID *)&iid_hal, "Glidulator fake HAL",
                         "Direct3D HAL", fh, fs, game_ctx);
        dlog("     game callback -> %ld", r);
    }
    return r;
}

/* DDPIXELFORMAT: dwSize, dwFlags, dwFourCC, dwRGBBitCount, masks... */
static HRESULT WINAPI my_enumtex(DWORD *pf, void *ctx)
{
    (void)ctx;
    if (pf && !IsBadReadPtr(pf, 32))
        dlog("   texture format: flags=%08lx%s bits=%lu masks=%08lx/%08lx/%08lx/%08lx",
             pf[1], pf[1] & 0x20 ? " PAL8" : pf[1] & 0x8 ? " PAL4" : "",
             pf[3], pf[4], pf[5], pf[6], pf[7]);
    HRESULT r = game_enumtex(pf, game_ctx);
    if (fake_pal8 && r && !pal8_offered) {
        DWORD p8[8] = { 32, 0x20 | 0x40, 0, 8, 0, 0, 0, 0 };
        pal8_offered = 1;
        dlog("   fake: offering PAL8 texture format");
        r = game_enumtex(p8, game_ctx);
        dlog("     game callback -> %ld", r);
    }
    return r;
}

/* IDirect3DDevice2::EnumTextureFormats passes a DDSURFACEDESC; the pixel
 * format starts at dword 18. */
static HRESULT WINAPI my_enumtex2(DWORD *sd, void *ctx)
{
    (void)ctx;
    if (sd && !IsBadReadPtr(sd, 108))
        dlog("   texture format: flags=%08lx%s bits=%lu masks=%08lx/%08lx/%08lx/%08lx",
             sd[19], sd[19] & 0x20 ? " PAL8" : sd[19] & 0x8 ? " PAL4" : "",
             sd[21], sd[22], sd[23], sd[24], sd[25]);
    HRESULT r = game_enumtex(sd, game_ctx);
    if (fake_pal8 && r && !pal8_offered) {
        DWORD d[27];
        memset(d, 0, sizeof d);
        d[0] = 108;
        d[1] = 0x1000 | 0x1;      /* DDSD_PIXELFORMAT | DDSD_CAPS */
        d[18] = 32;
        d[19] = 0x20 | 0x40;      /* PALETTEINDEXED8 | RGB */
        d[21] = 8;
        d[26] = 0x1000;           /* DDSCAPS_TEXTURE */
        pal8_offered = 1;
        dlog("   fake: offering PAL8 texture format");
        r = game_enumtex(d, game_ctx);
        dlog("     game callback -> %ld", r);
    }
    return r;
}

/* Fog render states, logged on every change (first 8 per state). */
static int is_fog_state(DWORD s)
{
    return s == 28 || (s >= 34 && s <= 38) || s == 48;
}
static DWORD fog_last[64];
static int fog_changes[64];

static const char *fog_name(DWORD s)
{
    switch (s) {
    case 28: return "FOGENABLE";
    case 34: return "FOGCOLOR";
    case 35: return "FOGTABLEMODE";
    case 36: return "FOGTABLESTART";
    case 37: return "FOGTABLEEND";
    case 38: return "FOGTABLEDENSITY";
    case 48: return "RANGEFOGENABLE";
    }
    return "?";
}

/* Called by the thunk; returns the real method's address. */
void *__cdecl hook_pre(int idx, DWORD *fr)
{
    wrap *w = (wrap *)fr[1];
    int t = w->type;
    void *target = (*(void ***)w->real)[idx];
    calls[t][idx]++;
    unsigned long c = calls[t][idx];
    const char *n = mname(t, idx);

    if (c <= LOG_FIRST)
        dlog("%s::%s(%08lx %08lx %08lx %08lx %08lx %08lx) #%lu", type_name[t],
             n, fr[2], fr[3], fr[4], fr[5], fr[6], fr[7], c);

    if (!strcmp(n, "SetRenderState") && fr[2] < 256 && !rs_seen[t][fr[2]]) {
        rs_seen[t][fr[2]] = 1;
        dlog("   render state %lu = %08lx (first use)", fr[2], fr[3]);
    }
    if (!strcmp(n, "SetRenderState") && is_fog_state(fr[2]) &&
        (fog_changes[fr[2]] == 0 || fog_last[fr[2]] != fr[3]) &&
        fog_changes[fr[2]] < 8) {
        fog_changes[fr[2]]++;
        fog_last[fr[2]] = fr[3];
        dlog("   fog %s = %08lx (%g as float)", fog_name(fr[2]), fr[3],
             (double)*(float *)&fr[3]);
    }
    if (!strcmp(n, "EnumDevices") && (t == T_D3D2 || t == T_D3D3 ||
                                      t == T_D3D)) {
        game_enumdev = (enumdev_t)fr[2];
        game_ctx = (void *)fr[3];
        fr[2] = (DWORD)my_enumdev;
    }
    if (!strcmp(n, "EnumTextureFormats") && (t == T_DEV3 || t == T_DEV2)) {
        game_enumtex = (enumpf_t)fr[2];
        pal8_offered = 0;
        game_ctx = (void *)fr[3];
        fr[2] = (DWORD)(t == T_DEV3 ? (void *)my_enumtex : (void *)my_enumtex2);
    }
    if (fake_hal && !strcmp(n, "CreateDevice") && fr[2] &&
        !memcmp((void *)fr[2], &iid_hal, sizeof(GUID))) {
        dlog("   fake HAL: creating the RGB device instead");
        fr[2] = (DWORD)&iid_rgb;
    }
    if (fake_hal && !strcmp(n, "CreateSurface") && fr[2] &&
        !IsBadWritePtr((void *)fr[2], 108)) {
        DWORD *d = (DWORD *)fr[2];
        if (d[26] & (0x1000 | 0x20000)) { /* TEXTURE or ZBUFFER */
            d[26] &= ~(0x4000u | 0x10000000u | 0x8000000u);
            d[26] |= 0x800; /* SYSTEMMEMORY for the software rasterizer */
        } else if (d[26] & 0x200) { /* PRIMARY: let DirectDraw place it */
            d[26] &= ~(0x4000u | 0x10000000u);
        }
    }
    if (!strcmp(n, "CreatePalette"))
        dlog("   palette flags=%08lx", fr[2]);
    if (!strcmp(n, "SetTextureStageState") && fr[2] < 8 && fr[3] < 32 &&
        !tss_seen[fr[2]][fr[3]]) {
        tss_seen[fr[2]][fr[3]] = 1;
        dlog("   stage %lu state %lu = %08lx (first use)", fr[2], fr[3], fr[4]);
    }
    if (!strncmp(n, "Draw", 4))
        count_draw(t, idx, fr[2], fr[3]);
    if (!strncmp(n, "Draw", 4) && c <= LOG_FIRST)
        dlog("   prim=%lu vertextype/fvf=%08lx", fr[2], fr[3]);
    if (!strcmp(n, "CreateSurface"))
        log_surface_desc((const DWORD *)fr[2]);
    if (!strcmp(n, "SetDisplayMode"))
        dlog("   mode %lux%lux%lu", fr[2], fr[3], fr[4]);
    if (!strcmp(n, "SetCooperativeLevel"))
        dlog("   coop hwnd=%08lx flags=%08lx", fr[2], fr[3]);
    if (!strcmp(n, "QueryInterface"))
        dlog("   QI %s", guid_str((const GUID *)fr[2], NULL));
    if (!strcmp(n, "CreateDevice"))
        dlog("   device %s", guid_str((const GUID *)fr[2], NULL));
    if (!strcmp(n, "EndScene") && c % 1000 == 0)
        dlog("   %lu frames", c);

    /* fr[2] is the first argument after `this' */
    if (!strcmp(n, "ProcessVertices")) {
        unwrap_arg(&fr[2 + 3]);
        unwrap_arg(&fr[2 + 5]);
    } else if (!strcmp(n, "ProcessVerticesStrided")) {
        unwrap_arg(&fr[2 + 5]);
    } else if (!strcmp(n, "GetHandle")) {
        unwrap_arg(&fr[2]);
    } else if (!strcmp(n, "Optimize")) {
        unwrap_arg(&fr[2]);
    } else if (!strcmp(n, "DrawPrimitiveVB") ||
               !strcmp(n, "DrawIndexedPrimitiveVB")) {
        unwrap_arg(&fr[2 + 1]);
    }

    if (depth < 64) {
        struct frame *f = &stack[depth];
        f->ret = (void *)fr[0];
        f->w = w;
        f->idx = idx;
        memcpy(f->a, fr + 2, sizeof f->a);
        extern void post_stub(void);
        fr[0] = (DWORD)post_stub;
        depth++;
    } else {
        dlog("!! call stack full");
    }
    fr[1] = (DWORD)w->real;
    return target;
}

/* Called by the post stub with the method's result; returns the caller's
 * return address. */
void *__cdecl hook_post(DWORD hr)
{
    struct frame *f = &stack[--depth];
    int t = f->w->type;
    const char *n = mname(t, f->idx);
    if (hr && (hr & 0x80000000)) {
        fails[t][f->idx]++;
        if (fails[t][f->idx] <= LOG_FIRST)
            dlog("   -> %s::%s failed %08lx", type_name[t], n, hr);
    } else if (calls[t][f->idx] <= LOG_FIRST && f->idx > 2) {
        dlog("   -> %s::%s ok %08lx", type_name[t], n, hr);
    }
    if (hr == 0 && !strcmp(n, "GetCaps") && t <= T_DD7) {
        DWORD *c = (DWORD *)f->a[0];
        if (c && !IsBadWritePtr(c, 64)) {
            dlog("   driver caps=%08lx caps2=%08lx zbits=%08lx vidmem=%lu/%lu",
                 c[1], c[2], c[14], c[15], c[16]);
            if (fake_hal) {
                c[1] |= 1;       /* DDCAPS_3D */
                c[14] |= 0x400;  /* DDBD_16 z-buffer */
                if (c[15] < 4u << 20)
                    c[15] = c[16] = 4u << 20;
                dlog("   fake HAL: reporting DDCAPS_3D, 16-bit Z, 4 MB");
            }
        }
    }
    if (hr == 0 && !strcmp(n, "GetCaps") && t >= T_DEV && t <= T_DEV3) {
        log_devdesc("GetCaps HAL", (const DWORD *)f->a[0]);
        log_devdesc("GetCaps HEL", (const DWORD *)f->a[1]);
    }
    if (hr == 0) {
        if (!strcmp(n, "QueryInterface")) {
            int type;
            guid_str((const GUID *)f->a[0], &type);
            void **out = (void **)f->a[1];
            if (out)
                *out = wrap_obj(*out, type);
        } else if (!strcmp(n, "CreateDevice")) {
            void **out = (void **)f->a[2];
            int type = t == T_D3D2 ? T_DEV2 : t == T_D3D3 ? T_DEV3 : T_DEV7;
            if (out)
                *out = wrap_obj(*out, type);
        } else if (!strcmp(n, "CreateMaterial") && t == T_D3D3) {
            void **out = (void **)f->a[0];
            if (out)
                *out = wrap_obj(*out, T_MAT3);
        } else if (!strcmp(n, "CreateVertexBuffer")) {
            void **out = (void **)f->a[1];
            if (out)
                *out = wrap_obj(*out, t == T_D3D7 ? T_VB7 : T_VB);
        }
    }
    return f->ret;
}

/* Thunk i: push i; jmp common.  common: hook_pre swaps in the real this
 * and the post stub as return address and yields the target; the target
 * address replaces i on the stack and `ret' jumps there. */
__asm__(
    ".text\n"
    "thunk_common:\n"
    "  pushal\n"
    "  leal 36(%esp), %eax\n"
    "  pushl %eax\n"
    "  pushl 36(%esp)\n"
    "  call _hook_pre\n"
    "  addl $8, %esp\n"
    "  movl %eax, 32(%esp)\n"
    "  popal\n"
    "  ret\n"
    ".globl _post_stub\n"
    "_post_stub:\n"
    "  pushl %eax\n"
    "  pushal\n"
    "  pushl %eax\n"
    "  call _hook_post\n"
    "  addl $4, %esp\n"
    "  movl %eax, 32(%esp)\n"
    "  popal\n"
    "  ret\n"
    ".globl _thunk_base\n"
    "_thunk_base:\n"
    ".set i, 0\n"
    ".rept 64\n"
    "  .byte 0x6A, i\n"
    "  .byte 0xE9\n"
    "  .long thunk_common - . - 4\n"
    "  .set i, i+1\n"
    ".endr\n");
/* 64 thunks of 7 bytes each (push imm8, jmp rel32). */
extern unsigned char thunk_base[];

static void init_vtables(void)
{
    unsigned char *p = thunk_base;
    for (int t = 0; t < T_COUNT; t++) {
        int n = 0;
        while (methods[t][n])
            n++;
        nmethods[t] = n;
        for (int i = 0; i < MAXM; i++)
            vtables[t][i] = p + 7 * i;
    }
}

/* ---- exports -------------------------------------------------------- */

typedef HRESULT (WINAPI *ddc_t)(GUID *, void **, IUnknown *);
typedef HRESULT (WINAPI *ddcex_t)(GUID *, void **, REFIID, IUnknown *);
typedef HRESULT (WINAPI *dde_t)(void *, void *);

static FARPROC real_proc(const char *n)
{
    FARPROC p = real_mod ? GetProcAddress(real_mod, n) : NULL;
    if (!p)
        dlog("!! %s not found in the system DDRAW.DLL", n);
    return p;
}

HRESULT WINAPI px_DirectDrawCreate(GUID *g, void **out, IUnknown *u)
{
    dlog("DirectDrawCreate(%s)", guid_str(g, NULL));
    ddc_t f = (ddc_t)real_proc("DirectDrawCreate");
    if (!f)
        return E_FAIL;
    HRESULT hr = f(g, out, u);
    dlog("   -> %08lx", hr);
    if (hr == 0 && out)
        *out = wrap_obj(*out, T_DD);
    return hr;
}

HRESULT WINAPI px_DirectDrawCreateEx(GUID *g, void **out, REFIID iid,
                                     IUnknown *u)
{
    int type;
    const char *in = guid_str(iid, &type);
    dlog("DirectDrawCreateEx(%s, %s)", guid_str(g, NULL), in);
    ddcex_t f = (ddcex_t)real_proc("DirectDrawCreateEx");
    if (!f)
        return E_FAIL;
    HRESULT hr = f(g, out, iid, u);
    dlog("   -> %08lx", hr);
    if (hr == 0 && out)
        *out = wrap_obj(*out, type);
    return hr;
}

HRESULT WINAPI px_DirectDrawEnumerateA(void *cb, void *ctx)
{
    dlog("DirectDrawEnumerateA");
    dde_t f = (dde_t)real_proc("DirectDrawEnumerateA");
    return f ? f(cb, ctx) : E_FAIL;
}

/* ---- loading -------------------------------------------------------- */

static void load_real(void)
{
    char sys[MAX_PATH - 16], path[MAX_PATH], got[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    snprintf(path, sizeof path, "%s\\DDRAW.DLL", sys);
    real_mod = LoadLibraryA(path);
    got[0] = 0;
    if (real_mod)
        GetModuleFileNameA(real_mod, got, sizeof got);
    dlog("LoadLibrary(%s) = %p (%s)%s", path, (void *)real_mod, got,
         real_mod == self_mod ? " = this proxy!" : "");
    if (real_mod == self_mod) {
        FreeLibrary(real_mod);
        real_mod = NULL;
    }
    if (!real_mod) {
        /* Win9x matches modules by base name: load a copy under another
         * name next to the proxy. */
        char copy[MAX_PATH];
        strcpy(copy, log_path);
        strcpy(strrchr(copy, '\\') + 1, "DDSYS.DLL");
        BOOL ok = CopyFileA(path, copy, FALSE);
        real_mod = LoadLibraryA(copy);
        dlog("fallback copy %s (%s) -> LoadLibrary = %p", copy,
             ok ? "copied" : "copy failed", (void *)real_mod);
    }
#define FW(n) p_##n = (void *)GetProcAddress(real_mod, #n);
#include "forward.h"
#undef FW
}

/* ---- launcher mode (GLRUN.EXE loads this DLL as GLDDRAW.DLL) --------- */

/* Under its own name the DLL does not replace DDRAW.DLL; it redirects the
 * game's imports instead: DirectDraw entry points to the px_ functions,
 * and LoadLibraryA/GetProcAddress/CreateProcessA/WinExec to hooks that
 * keep later modules and child processes (Half-Life restarts itself)
 * redirected too.  Only modules from the game's folder (and below) are
 * patched: system DLLs may be shared between processes on Windows 9x,
 * also below 2 GB, and their imports do not concern us. */

static int launcher_mode;
static char self_path[MAX_PATH];

typedef HMODULE (WINAPI *lla_t)(LPCSTR);
typedef HMODULE (WINAPI *llea_t)(LPCSTR, HANDLE, DWORD);
typedef FARPROC (WINAPI *gpa_t)(HMODULE, LPCSTR);
typedef UINT (WINAPI *we_t)(LPCSTR, UINT);
static lla_t r_LoadLibraryA;
static llea_t r_LoadLibraryExA;
static gpa_t r_GetProcAddress;
static create_fn r_CreateProcessA;
static we_t r_WinExec;
typedef int (WINAPI *mb_t)(HWND, LPCSTR, LPCSTR, UINT);
static mb_t r_MessageBoxA;

static int WINAPI h_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT t)
{
    dlog("MessageBoxA(\"%s\", \"%s\") from %p", cap ? cap : "", text ? text : "",
         __builtin_return_address(0));
    return r_MessageBoxA(w, text, cap, t);
}
static void *r_dd[3];

static void patch_all(void);

static HMODULE WINAPI h_LoadLibraryA(LPCSTR n)
{
    HMODULE m = r_LoadLibraryA(n);
    patch_all();
    return m;
}

static HMODULE WINAPI h_LoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl)
{
    HMODULE m = r_LoadLibraryExA(n, f, fl);
    patch_all();
    return m;
}

static void *hook_for(void *real);

static FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR n)
{
    FARPROC p = r_GetProcAddress(m, n);
    void *h = p ? hook_for((void *)p) : NULL;
    if (h) {
        if ((DWORD)n >> 16)
            dlog("GetProcAddress(%p, %s) -> hook", (void *)m, n);
        return (FARPROC)h;
    }
    return p;
}

static BOOL WINAPI h_CreateProcessA(LPCSTR app, LPSTR cmd,
    LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD fl,
    LPVOID env, LPCSTR dir, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    dlog("CreateProcessA(%s, %s)", app ? app : "NULL", cmd ? cmd : "NULL");
    return inject_create(app, cmd, pa, ta, inh, fl, env, dir, si, pi,
                         self_path, r_CreateProcessA, dlog);
}

static UINT WINAPI h_WinExec(LPCSTR cmd, UINT show)
{
    char buf[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    dlog("WinExec(%s)", cmd);
    lstrcpynA(buf, cmd, sizeof buf);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = (WORD)show;
    if (!h_CreateProcessA(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si,
                          &pi))
        return 2;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 33;
}

static void *hook_for(void *real)
{
    if (!real)
        return NULL;
    if (real == (void *)r_LoadLibraryA) return (void *)h_LoadLibraryA;
    if (real == (void *)r_LoadLibraryExA) return (void *)h_LoadLibraryExA;
    if (real == (void *)r_GetProcAddress) return (void *)h_GetProcAddress;
    if (real == (void *)r_CreateProcessA) return (void *)h_CreateProcessA;
    if (real == (void *)r_WinExec) return (void *)h_WinExec;
    if (real == (void *)r_MessageBoxA) return (void *)h_MessageBoxA;
    if (real == r_dd[0]) return (void *)px_DirectDrawCreate;
    if (real == r_dd[1]) return (void *)px_DirectDrawCreateEx;
    if (real == r_dd[2]) return (void *)px_DirectDrawEnumerateA;
    return NULL;
}

#define MAXPATCHED 128
static DWORD patched[MAXPATCHED];
static int npatched;

static void patch_module(BYTE *base, const char *name)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *d =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!d->VirtualAddress)
        return;
    int count = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *id =
             (IMAGE_IMPORT_DESCRIPTOR *)(base + d->VirtualAddress);
         id->Name; id++) {
        void **iat = (void **)(base + id->FirstThunk);
        for (; *iat; iat++) {
            void *h = hook_for(*iat);
            if (!h)
                continue;
            DWORD old;
            if (VirtualProtect(iat, 4, PAGE_READWRITE, &old)) {
                *iat = h;
                VirtualProtect(iat, 4, old, &old);
                count++;
            } else {
                dlog("!! cannot unprotect the IAT of %s", name);
            }
        }
    }
    dlog("patched %d imports of %s (%p)", count, name, (void *)base);
}

static char game_dir[MAX_PATH];

static void patch_all(void)
{
    if (!game_dir[0]) {
        GetModuleFileNameA(NULL, game_dir, sizeof game_dir);
        *(strrchr(game_dir, '\\') + 1) = 0;
    }
    int dl = lstrlenA(game_dir);
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32 me;
    me.dwSize = sizeof me;
    for (BOOL ok = Module32First(s, &me); ok; ok = Module32Next(s, &me)) {
        DWORD b = (DWORD)me.modBaseAddr;
        char pre[MAX_PATH];
        lstrcpynA(pre, me.szExePath, dl + 1);
        if (b >= 0x80000000u || me.hModule == self_mod ||
            lstrcmpiA(pre, game_dir))
            continue;
        int i;
        for (i = 0; i < npatched && patched[i] != b; i++)
            ;
        if (i < npatched || npatched == MAXPATCHED)
            continue;
        patched[npatched++] = b;
        patch_module(me.modBaseAddr, me.szModule);
    }
    CloseHandle(s);
}

static void launcher_init(void)
{
    HMODULE k = GetModuleHandleA("KERNEL32.DLL");
    r_LoadLibraryA = (lla_t)GetProcAddress(k, "LoadLibraryA");
    r_LoadLibraryExA = (llea_t)GetProcAddress(k, "LoadLibraryExA");
    r_GetProcAddress = (gpa_t)GetProcAddress(k, "GetProcAddress");
    r_CreateProcessA = (create_fn)GetProcAddress(k, "CreateProcessA");
    r_WinExec = (we_t)GetProcAddress(k, "WinExec");
    r_MessageBoxA = (mb_t)GetProcAddress(LoadLibraryA("USER32.DLL"),
                                         "MessageBoxA");
    r_dd[0] = (void *)GetProcAddress(real_mod, "DirectDrawCreate");
    r_dd[1] = (void *)GetProcAddress(real_mod, "DirectDrawCreateEx");
    r_dd[2] = (void *)GetProcAddress(real_mod, "DirectDrawEnumerateA");
    patch_all();
}

static void dump_totals(void)
{
    dlog("---- draw calls by primitive and vertex type (DX5: 1 VERTEX, 2 LVERTEX, 3 TLVERTEX; DX6+: FVF) ----");
    for (int i = 0; i < ndraws; i++)
        dlog("%-18s %-28s prim %lu type/fvf %08lx %10lu", type_name[draws[i].t],
             mname(draws[i].t, draws[i].idx), draws[i].prim, draws[i].vt,
             draws[i].n);
    dlog("---- call totals ----");
    for (int t = 0; t < T_COUNT; t++)
        for (int i = 0; i < MAXM; i++)
            if (calls[t][i])
                dlog("%-18s %-28s %10lu calls %8lu failed", type_name[t],
                     mname(t, i), calls[t][i], fails[t][i]);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH) {
        self_mod = h;
        GetModuleFileNameA(h, log_path, sizeof log_path);
        char me[MAX_PATH];
        strcpy(me, log_path);
        strcpy(strrchr(log_path, '\\') + 1, "DDPROXY.LOG");
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        OSVERSIONINFOA v;
        memset(&v, 0, sizeof v);
        v.dwOSVersionInfoSize = sizeof v;
        GetVersionExA(&v);
        dlog("==== ddproxy loaded: %s into %s (Windows %lu.%lu.%lu %s)", me,
             exe, v.dwMajorVersion, v.dwMinorVersion,
             v.dwBuildNumber & 0xffff, v.szCSDVersion);
        dlog("pid %08lx, command line: %s", GetCurrentProcessId(),
             GetCommandLineA());
        dlog("GetModuleHandle(DDRAW.DLL) = %p, self = %p",
             (void *)GetModuleHandleA("DDRAW.DLL"), (void *)h);
        init_vtables();
        load_real();
        {
            char cfg[MAX_PATH], buf[256] = "";
            strcpy(cfg, log_path);
            strcpy(strrchr(cfg, '\\') + 1, "DDPROXY.CFG");
            HANDLE fh = CreateFileA(cfg, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, 0, NULL);
            if (fh != INVALID_HANDLE_VALUE) {
                DWORD got = 0;
                ReadFile(fh, buf, sizeof buf - 1, &got, NULL);
                buf[got] = 0;
                CloseHandle(fh);
            }
            fake_hal = strstr(buf, "fakehal") != NULL;
            fake_pal8 = strstr(buf, "fakepal8") != NULL;
            if (fake_pal8)
                dlog("fake PAL8 texture format (DDPROXY.CFG)");
            if (fake_hal)
                dlog("fake HAL mode (DDPROXY.CFG)");
        }
        strcpy(self_path, me);
        const char *base = strrchr(me, '\\') + 1;
        if (lstrcmpiA(base, "DDRAW.DLL")) {
            launcher_mode = 1;
            dlog("launcher mode (%s)", base);
            launcher_init();
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        dump_totals();
        dlog("==== ddproxy unloaded (pid %08lx, %s)", GetCurrentProcessId(),
             res ? "process exit" : "FreeLibrary");
    }
    return TRUE;
}
