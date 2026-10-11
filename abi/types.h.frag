/* ---- Base types ------------------------------------------------------ */
typedef unsigned char  FxU8;
typedef signed char    FxI8;
typedef unsigned short FxU16;
typedef signed short   FxI16;
typedef unsigned int   FxU32;
typedef signed int     FxI32;
typedef FxI32          FxBool;
typedef float          FxFloat;
#define FXTRUE  1
#define FXFALSE 0

/* ---- Scalar Glide types (all passed as 32-bit stack slots) ----------- */
typedef FxU32 GrColor_t;
typedef FxU8  GrAlpha_t;
typedef FxU32 GrMipMapId_t;
typedef FxU8  GrFog_t;
typedef FxI32 GrChipID_t;
typedef FxI32 GrCombineFunction_t;
typedef FxI32 GrCombineFactor_t;
typedef FxI32 GrCombineLocal_t;
typedef FxI32 GrCombineOther_t;
typedef FxI32 GrAlphaSource_t;
typedef FxI32 GrColorCombineFnc_t;
typedef FxI32 GrAlphaBlendFnc_t;
typedef FxI32 GrAspectRatio_t;
typedef FxI32 GrBuffer_t;
typedef FxI32 GrChromakeyMode_t;
typedef FxI32 GrChromaRangeMode_t;
typedef FxI32 GrCmpFnc_t;
typedef FxI32 GrColorFormat_t;
typedef FxI32 GrCullMode_t;
typedef FxI32 GrDepthBufferMode_t;
typedef FxI32 GrDitherMode_t;
typedef FxI32 GrFogMode_t;
typedef FxU32 GrLock_t;
typedef FxI32 GrLfbBypassMode_t;
typedef FxI32 GrLfbWriteMode_t;
typedef FxI32 GrOriginLocation_t;
typedef FxI32 GrLOD_t;
typedef FxI32 GrMipMapMode_t;
typedef FxI32 GrSmoothingMode_t;
typedef FxI32 GrTextureClampMode_t;
typedef FxI32 GrTextureCombineFnc_t;
typedef FxI32 GrTextureFilterMode_t;
typedef FxI32 GrTextureFormat_t;
typedef FxU32 GrTexTable_t;
typedef FxU32 GrNCCTable_t;
typedef FxU32 GrTexBaseRange_t;
typedef FxU32 GrEnableMode_t;
typedef FxU32 GrCoordinateSpaceMode_t;
typedef FxI32 GrLfbSrcFmt_t;
typedef FxU32 GrHint_t;
typedef FxI32 GrSstType;
typedef FxU32 GrScreenResolution_t;
typedef FxU32 GrScreenRefresh_t;
typedef FxU32 GrMPTextureCombineFnc_t;

typedef void (GLIDE_API *GrProc)(void);
/* The error callback is invoked through an adapter in the runtime that
 * satisfies both register- and stack-convention callers. */
typedef void (*GrErrorCallbackFnc_t)(const char *string, FxBool fatal);

/* ---- Structures (layouts fixed by the shipped ABI; see docs/abi-facts.md) */
#define GLIDE_NUM_TMU        2
#define MAX_NUM_SST          4
#define GLIDE_STATE_PAD_SIZE 312

typedef struct {
    float sow;
    float tow;
    float oow;
} GrTmuVertex;

typedef struct {
    float x, y, z;              /* screen x, y; z is ignored */
    float r, g, b;              /* 0..255 */
    float ooz;                  /* 65535/Z */
    float a;                    /* 0..255 */
    float oow;                  /* 1/W */
    GrTmuVertex tmuvtx[GLIDE_NUM_TMU];
} GrVertex;

typedef struct {
    int                size;
    void              *lfbPtr;
    FxU32              strideInBytes;
    GrLfbWriteMode_t   writeMode;
    GrOriginLocation_t origin;
} GrLfbInfo_t;

typedef struct { char pad[GLIDE_STATE_PAD_SIZE]; } GrState;

typedef struct {
    GrLOD_t           smallLod;
    GrLOD_t           largeLod;
    GrAspectRatio_t   aspectRatio;
    GrTextureFormat_t format;
    void             *data;
} GrTexInfo;

typedef struct { int tmuRev; int tmuRam; } GrTMUConfig_t;

typedef struct {
    int           fbRam;
    int           fbiRev;
    int           nTexelfx;
    FxBool        sliDetect;
    GrTMUConfig_t tmuConfig[GLIDE_NUM_TMU];
} GrVoodooConfig_t;

typedef struct {
    int           fbRam;
    int           nTexelfx;
    GrTMUConfig_t tmuConfig;
} GrSst96Config_t;

typedef GrVoodooConfig_t GrVoodoo2Config_t;
typedef struct { int rev; } GrAT3DConfig_t;

typedef struct {
    int num_sst;
    struct {
        GrSstType type;
        union {
            GrVoodooConfig_t  VoodooConfig;
            GrSst96Config_t   SST96Config;
            GrAT3DConfig_t    AT3DConfig;
            GrVoodoo2Config_t Voodoo2Config;
        } sstBoard;
    } SSTs[MAX_NUM_SST];
} GrHwConfiguration;

typedef struct {
    FxU32 pixelsIn;
    FxU32 chromaFail;
    FxU32 zFuncFail;
    FxU32 aFuncFail;
    FxU32 pixelsOut;
} GrSstPerfStats_t;

typedef struct {
    FxU32             width, height;
    int               small_lod, large_lod;
    GrAspectRatio_t   aspect_ratio;
    GrTextureFormat_t format;
} Gu3dfHeader;

typedef struct {
    FxU8  yRGB[16];
    FxI16 iRGB[4][3];
    FxI16 qRGB[4][3];
    FxU32 packed_data[12];
} GuNccTable;

typedef struct { FxU32 data[256]; } GuTexPalette;

typedef union {
    GuNccTable   nccTable;
    GuTexPalette palette;
} GuTexTable;

typedef struct {
    Gu3dfHeader header;
    GuTexTable  table;
    void       *data;
    FxU32       mem_required;
} Gu3dfInfo;

typedef struct {
    int                   sst;
    FxBool                valid;
    int                   width, height;
    GrAspectRatio_t       aspect_ratio;
    void                 *data;
    GrTextureFormat_t     format;
    GrMipMapMode_t        mipmap_mode;
    GrTextureFilterMode_t magfilter_mode;
    GrTextureFilterMode_t minfilter_mode;
    GrTextureClampMode_t  s_clamp_mode;
    GrTextureClampMode_t  t_clamp_mode;
    FxU32                 tLOD;
    FxU32                 tTextureMode;
    FxU32                 lod_bias;
    GrLOD_t               lod_min, lod_max;
    int                   tmu;
    FxU32                 odd_even_mask;
    FxU32                 tmu_base_address;
    FxBool                trilinear;
    GuNccTable            ncc_table;
} GrMipMapInfo;

#define GR_NULL_MIPMAP_HANDLE ((GrMipMapId_t)-1)

/* fxpci (3dfx's PCI library, also exported by glide2x.dll and imported by
 * e.g. Nature95); layout per swlibs/newpci/pcilib/fxpci.h. */
typedef struct {
    FxU32 regAddress;
    FxU32 sizeInBytes;
    FxU32 rwFlag;
} PciRegister;
typedef int PciMemType;
