/* GLMTTEST.EXE: multi-core rendering through glide2x.dll on Windows 9x
 * (plan (c), step c2). Renders the same scene (textured, Gouraud, alpha
 * blending, depth buffer; deterministic) in several passes -- JIT and C
 * path, with all cores and with GLIDULATOR_THREADS=1 -- reads every frame
 * back with grLfbReadRegion and compares it pixel by pixel with the
 * one-core C pass: multi-core must be bit-identical. Differences are counted
 * per scanline residue (y % 3: with three workers, worker i draws the lines
 * y % 3 == i). After each pass the VxD's counters (second handle). The first
 * pass uses all cores, since GLIDULATOR_THREADS=1 at the first core query
 * keeps the VxD unloaded.
 * glide2x.dll and GLSMP.VXD next to the EXE; with the trace DLL,
 * GLIDULAT.LOG shows the "Multi-core:" and "Renderer:" lines. Log
 * GLMTTEST.TXT. */
#define _WIN32_WINNT 0x0400
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "glidulator/glide2x.h"

#define W        640
#define H        480
#define FRAMES   6
#define TRIS     3000
#define TEX      64

static void L(const char *fmt, ...){ va_list a; va_start(a,fmt);
  FILE *f=fopen("GLMTTEST.TXT","a"); if(f){vfprintf(f,fmt,a);fclose(f);} va_end(a); }

static unsigned g_seed;
static unsigned rnd(void){ g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
static float frnd(float lo, float hi){ return lo + (hi - lo) * (float)(rnd() & 0xFFFF) / 65535.0f; }

static FxU16 g_tex[TEX * TEX];
static FxU16 g_frame[W * H];
static FxU16 g_ref[FRAMES][W * H];
static int   g_have_ref;

static void vert(GrVertex *v, float cx, float cy){
  memset(v, 0, sizeof *v);
  v->x = cx + frnd(-90, 90); v->y = cy + frnd(-90, 90);
  if (v->x < 0) v->x = 0;
  if (v->x > W - 1) v->x = W - 1;
  if (v->y < 0) v->y = 0;
  if (v->y > H - 1) v->y = H - 1;
  v->r = frnd(40, 255); v->g = frnd(40, 255); v->b = frnd(40, 255);
  v->a = frnd(60, 255);
  v->oow = frnd(0.05f, 1.0f);
  v->ooz = v->oow * 65535.0f;
  v->tmuvtx[0].sow = frnd(0, 255) * v->oow;
  v->tmuvtx[0].tow = frnd(0, 255) * v->oow;
  v->tmuvtx[0].oow = v->oow;
}

/* one pass: returns the FNV-1a hash over all frames, time in *ms. store:
 * keep the frames as reference; else compare with it. *wrong = pixels that
 * differ from the reference. */
static unsigned pass(double *ms, int store, unsigned *wrong){
  LARGE_INTEGER f, t0, t1;
  unsigned h = 2166136261u;
  *ms = 1;
  if (!grSstWinOpen(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz, GR_COLORFORMAT_ARGB,
                    GR_ORIGIN_UPPER_LEFT, 2, 1)){
    L("grSstWinOpen failed\r\n");
    return 0;
  }
  GrTexInfo ti = { GR_LOD_64, GR_LOD_64, GR_ASPECT_1x1, GR_TEXFMT_RGB_565, g_tex };
  grTexDownloadMipMap(GR_TMU0, grTexMinAddress(GR_TMU0), GR_MIPMAPLEVELMASK_BOTH, &ti);
  grTexSource(GR_TMU0, grTexMinAddress(GR_TMU0), GR_MIPMAPLEVELMASK_BOTH, &ti);
  grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
  grTexMipMapMode(GR_TMU0, GR_MIPMAP_DISABLE, FXFALSE);
  grTexCombine(GR_TMU0, GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
               GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE, FXFALSE, FXFALSE);
  grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_LOCAL,
                 GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
  grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
  grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA, GR_BLEND_ONE, GR_BLEND_ZERO);
  grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER);
  grDepthBufferFunction(GR_CMP_LESS);
  grDepthMask(FXTRUE);
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t0);
  g_seed = 1;
  for (int fr = 0; fr < FRAMES; fr++){
    grBufferClear(0x00203040, 0, 0xFFFF);
    for (int i = 0; i < TRIS; i++){
      GrVertex a, b, c;
      float cx = frnd(0, W), cy = frnd(0, H);
      vert(&a, cx, cy); vert(&b, cx, cy); vert(&c, cx, cy);
      grDrawTriangle(&a, &b, &c);
    }
    grLfbReadRegion(GR_BUFFER_BACKBUFFER, 0, 0, W, H, W * 2, g_frame);
    for (int i = 0; i < W * H; i++){ h ^= g_frame[i]; h *= 16777619u; }
    if (store)
      memcpy(g_ref[fr], g_frame, sizeof g_frame);
    else if (g_have_ref){
      unsigned n = 0, res[3] = {0, 0, 0}, first = ~0u;
      for (int i = 0; i < W * H; i++)
        if (g_frame[i] != g_ref[fr][i]){
          if (!n) first = (unsigned)i;
          n++; res[(i / W) % 3]++;
        }
      *wrong += n;
      if (n)
        L("   frame %d: %u pixels differ (y%%3: %u/%u/%u), first at %u,%u: %04X instead of %04X\r\n",
          fr, n, res[0], res[1], res[2], first % W, first / W, g_frame[first], g_ref[fr][first]);
    }
    grBufferSwap(0);
  }
  QueryPerformanceCounter(&t1);
  *ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
  grSstWinClose();
  return h;
}

/* VxD counters through a second handle (closed only at process end: its
 * close stops the APs) */
static HANDLE g_vxd = INVALID_HANDLE_VALUE;
static void vxd_status(void){
  DWORD st[13], r = 0;
  if (g_vxd == INVALID_HANDLE_VALUE){
    char full[MAX_PATH], path[MAX_PATH + 4] = "\\\\.\\";
    if (GetFullPathNameA("GLSMP.VXD", MAX_PATH, full, NULL) &&
        GetShortPathNameA(full, path + 4, MAX_PATH))
      g_vxd = CreateFileA(path, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (g_vxd == INVALID_HANDLE_VALUE){ L("   VxD: no second handle\r\n"); return; }
  }
  memset(st, 0, sizeof st);
  DeviceIoControl(g_vxd, 5, NULL, 0, st, sizeof st, &r, NULL);
  L("   VxD: running=%lu parked=%lu faults=%lu pd_added=%lu pd_clash=%lu locks=%lu pd_resync=%lu\r\n",
    st[1], st[3], st[5], st[7], st[8], st[9], st[12]);
}

int main(void){
  static const struct { const char *renderer, *threads; int store; } passes[] = {
    { "jit", NULL, 0 }, { "c", "1", 1 }, { "jit", "1", 0 },
    { "c", NULL, 0 }, { "jit", NULL, 0 }, { "c", NULL, 0 }, { "jit", NULL, 0 }, { "c", NULL, 0 },
  };
  int bad = 0;
  DeleteFileA("GLMTTEST.TXT");
  L("=== glmttest: %d frames x %d triangles, %dx%d ===\r\n", FRAMES, TRIS, W, H);
  for (int y = 0; y < TEX; y++)
    for (int x = 0; x < TEX; x++)
      g_tex[y * TEX + x] = (FxU16)((((x ^ y) & 8) ? 0xF800 : 0x07E0) | (x >> 1));
  for (unsigned p = 0; p < sizeof passes / sizeof passes[0]; p++){
    unsigned wrong = 0, h;
    double ms;
    SetEnvironmentVariableA("GLIDULATOR_RENDERER", passes[p].renderer);
    SetEnvironmentVariableA("GLIDULATOR_THREADS", passes[p].threads);
    h = pass(&ms, passes[p].store, &wrong);
    if (passes[p].store) g_have_ref = 1;
    L("%-3s threads=%-4s hash=%08X  %.0f ms (%.1f fps)  %s\r\n", passes[p].renderer,
      passes[p].threads ? passes[p].threads : "auto", h, ms, FRAMES * 1000.0 / ms,
      passes[p].store ? "reference" : !g_have_ref ? "(before the reference)" :
      wrong ? "DIFFERS" : "identical");
    if (wrong) bad++;
    vxd_status();
  }
  L("RESULT: %s\r\n", bad ? "FAILED" : "multi-core bit-identical to one core");
  grGlideShutdown();
  return bad;
}
