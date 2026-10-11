#!/usr/bin/env python3
"""Generates core/include/glidulator/glide2x.h, core/glide_stubs.c and
targets/glide_dll/glide2x.def from abi/glide2x.api, abi/types.h.frag and
abi/constants.tsv (clean-room API description from ranulphus/MGA-Glide, MIT,
see abi/ATTRIBUTION.md).

Only declarations, the Windows export list and harmless stubs for every
function Glidulator does not implement are generated.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Implemented in core/glide_*.c: no stub is generated for these names (only
# the declaration in the header).
REAL_IMPL = {
    "grGlideInit", "grGlideShutdown", "grGlideGetVersion",
    "grSstQueryHardware", "grSstQueryBoards", "grSstWinOpen", "grSstWinClose",
    "grSstSelect", "grSstIdle", "grRenderBuffer",
    "grBufferClear", "grBufferSwap", "grDrawTriangle", "grClipWindow",
    "grErrorSetCallback", "grTexSource", "grTexDownloadMipMap",
    "grTexMinAddress", "grTexMaxAddress", "grTexTextureMemRequired", "grTexCalcMemRequired",
    # core/glide_modes.c
    "grColorCombine", "grAlphaCombine", "grTexCombine", "grConstantColorValue", "grConstantColorValue4",
    "grAlphaControlsITRGBLighting", "grAlphaBlendFunction", "grAlphaTestFunction", "grAlphaTestReferenceValue",
    "grDepthBufferMode", "grDepthBufferFunction", "grDepthMask", "grDepthBiasLevel", "grDitherMode",
    "grCullMode", "grColorMask", "grChromakeyMode", "grChromakeyValue", "grHints",
    # core/glide_tex.c
    "grTexDownloadMipMapLevel", "grTexDownloadMipMapLevelPartial", "grTexDownloadTable",
    "grTexDownloadTablePartial", "grTexFilterMode", "grTexClampMode", "grTexMipMapMode", "grTexLodBiasValue",
    # core/glide_lfb.c
    "grLfbWriteRegion", "grLfbReadRegion", "grLfbLock", "grLfbUnlock", "grLfbWriteColorFormat",
    "grLfbConstantAlpha", "grLfbConstantDepth",
    # core/glide_sst.c
    "grSstScreenWidth", "grSstScreenHeight", "grSstVRetraceOn", "grSstVideoLine", "grSstStatus",
    "grSstIsBusy", "grBufferNumPending", "grSstOrigin", "grDisableAllEffects", "grGlideGetState",
    "grGlideSetState", "grTriStats", "grResetTriStats", "grSstPerfStats", "grSstResetPerfStats",
    "grDrawPoint", "grDrawLine", "grGammaCorrectionValue", "grSstControl",
    # core/glide_gutex.c
    "guTexAllocateMemory", "guTexChangeAttributes", "guTexGetMipMapInfo", "guTexGetCurrentMipMap",
    "guTexSource", "guTexDownloadMipMap", "guTexDownloadMipMapLevel", "guTexMemReset", "guTexMemQueryAvail",
    "grTexCombineFunction", "guTexCombineFunction", "gu3dfGetInfo", "gu3dfLoad",
    # core/glide_modes.c (gu combine presets)
    "guColorCombineFunction", "guAlphaSource",
    # core/glide_fog.c, NCC in core/glide_tex.c
    "grFogMode", "grFogColorValue", "grFogTable", "guFogTableIndexToW", "guFogGenerateExp",
    "guFogGenerateExp2", "guFogGenerateLinear", "grTexNCCTable",
    # core/glide_abi.c (polygons)
    "grDrawPolygon", "grDrawPolygonVertexList", "grDrawPlanarPolygon", "grDrawPlanarPolygonVertexList",
    "guDrawTriangleWithClip", "guDrawPolygonVertexListWithClip",
    # core/glide_sst.c (fxpci)
    "pciOpen", "pciClose",
}

# Parameters passed by value that take more than 4 bytes of stack (__stdcall suffix).
BYVALUE_SIZE = {"PciRegister": 12}

PARAM_RE = re.compile(r"^(?P<type>.*?)(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?P<arr>\[\s*\d*\s*\])?$")


def parse_params(args):
    args = args.strip()
    if args in ("", "void"):
        return []
    out = []
    for i, a in enumerate(x.strip() for x in args.split(",")):
        m = PARAM_RE.match(a)
        t, n = m.group("type").strip(), m.group("name")
        if m.group("arr"):
            t += " *"
        if not t:
            t, n = n, "a%d" % i
        out.append((t, n))
    return out


def load_api(path):
    funcs = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        name, ret, args, tier, used, status = [x.strip() for x in line.split("|")]
        params = parse_params(args)
        nbytes = sum(BYVALUE_SIZE.get(t, 4) for t, _n in params)
        decorated = "_%s@%d" % (name.upper(), nbytes)
        funcs.append(dict(name=name, ret=ret, params=params, tier=int(tier),
                           used=used, status=status, decorated=decorated, nbytes=nbytes))
    return funcs


def default_return(ret):
    r = ret.strip()
    if r == "void":
        return None
    if r == "float":
        return "0.0f"
    if r == "FxBool":
        return "FXFALSE"
    if r.endswith("*"):
        return "0"
    return "0"


DEFINE_RE = re.compile(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)")


def gen_header(funcs, types_frag, constants_path, out_path):
    types_text = open(types_frag).read()
    already_defined = set(DEFINE_RE.match(l).group(1) for l in types_text.splitlines() if DEFINE_RE.match(l))
    lines = []
    lines.append("#ifndef GLIDULATOR_GLIDE2X_H")
    lines.append("#define GLIDULATOR_GLIDE2X_H")
    lines.append("")
    lines.append("/*")
    lines.append(" * GENERATED by tools/abi/gen_glide2x.py from abi/glide2x.api,")
    lines.append(" * abi/types.h.frag and abi/constants.tsv -- do not edit by hand.")
    lines.append(" * Origin/license: abi/ATTRIBUTION.md (MIT, ranulphus/MGA-Glide).")
    lines.append(" *")
    lines.append(" * Which of the %d functions are implemented (instead of a harmless" % len(funcs))
    lines.append(" * stub in core/glide_stubs.c) is listed in REAL_IMPL of the generator.")
    lines.append(" */")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append('extern "C" {')
    lines.append("#endif")
    lines.append("")
    lines.append("#if defined(__WATCOMC__)")
    lines.append("#    define GLIDE_API  __stdcall")
    lines.append("#    define GR_EXPORT  __export")
    lines.append("#elif defined(__GNUC__) && defined(__i386__)")
    lines.append("#    define GLIDE_API  __attribute__((stdcall))")
    lines.append("#    define GR_EXPORT")
    lines.append("#else")
    lines.append("#    define GLIDE_API")
    lines.append("#    define GR_EXPORT")
    lines.append("#endif")
    lines.append("")
    lines.append(types_text.rstrip())
    lines.append("")
    lines.append("/* ---- Constants (abi/constants.tsv) ------------------------------------ */")
    for line in open(constants_path):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        name, val = line.split("\t")[0], line.split("\t")[1]
        if name in already_defined:
            continue  # already defined by types.h.frag (same value)
        lines.append("#define %s %s" % (name, val))
    lines.append("")
    lines.append("/* ---- Functions (abi/glide2x.api, %d) ---------------------------------- */" % len(funcs))
    for f in funcs:
        params = ", ".join("%s %s" % (t, n) for t, n in f["params"]) or "void"
        lines.append("%s GR_EXPORT GLIDE_API %s(%s); /* %s, tier %d */" %
                      (f["ret"], f["name"], params, f["decorated"], f["tier"]))
    lines.append("")
    lines.append("/* NOT Glide API -- test hook for the test programs. Deliberately a name")
    lines.append(" * that cannot be mistaken for a real Glide function. */")
    lines.append("int GR_EXPORT GLIDE_API glidulatorDebugPeekPixel(int x, int y);")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* GLIDULATOR_GLIDE2X_H */")
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def gen_stubs(funcs, out_path):
    lines = []
    lines.append("/*")
    lines.append(" * GENERATED by tools/abi/gen_glide2x.py -- do not edit by hand.")
    lines.append(" * Harmless stubs for every Glide function that is not implemented: a")
    lines.append(" * game must find EVERY import in the export table, or it refuses to load")
    lines.append(" * the OVL/DLL. API origin/license: abi/ATTRIBUTION.md.")
    lines.append(" */")
    lines.append("")
    lines.append('#include "glidulator/glide2x.h"')
    lines.append('#include "glidulator/trace.h"')
    lines.append("")
    n = 0
    for f in funcs:
        if f["name"] in REAL_IMPL:
            continue
        n += 1
        params = ", ".join("%s %s" % (t, nm) for t, nm in f["params"]) or "void"
        lines.append("%s GR_EXPORT GLIDE_API %s(%s) {" % (f["ret"], f["name"], params))
        lines.append('    GLIDE_TRACE("%s");' % f["name"])
        for _t, nm in f["params"]:
            lines.append("    (void) %s;" % nm)
        d = default_return(f["ret"])
        if d is not None:
            lines.append("    return %s;" % d)
        lines.append("}")
        lines.append("")
    with open(out_path, "w") as f:
        f.write("\n".join(lines))
    return n


def gen_def(funcs, out_path):
    """Export list for glide2x.dll: the same names as 3dfx's glide2x.dll,
    __stdcall-decorated in original casing (_grGlideInit@0), which is how
    Windows programs import them. Without a .def file MinGW would export
    every global symbol, including internal ones."""
    lines = ["; GENERATED by tools/abi/gen_glide2x.py -- do not edit by hand.",
             "LIBRARY glide2x.dll", "EXPORTS"]
    names = [(f["name"], f["nbytes"]) for f in funcs]
    names.append(("glidulatorDebugPeekPixel", 8))
    for name, n in names:
        lines.append("    _%s@%d = %s@%d" % (name, n, name, n))
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    funcs = load_api(os.path.join(ROOT, "abi", "glide2x.api"))
    gen_header(funcs, os.path.join(ROOT, "abi", "types.h.frag"),
               os.path.join(ROOT, "abi", "constants.tsv"),
               os.path.join(ROOT, "core", "include", "glidulator", "glide2x.h"))
    n = gen_stubs(funcs, os.path.join(ROOT, "core", "glide_stubs.c"))
    gen_def(funcs, os.path.join(ROOT, "targets", "glide_dll", "glide2x.def"))
    print("gen_glide2x: %d functions, %d implemented (core/glide_*.c), %d stubs" %
          (len(funcs), len(REAL_IMPL), n))


if __name__ == "__main__":
    sys.exit(main())
