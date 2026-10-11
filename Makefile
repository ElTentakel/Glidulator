CC ?= gcc
SAN ?= -fsanitize=address,undefined
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -g $(SAN)
# -lpng16: only used by the Linux test tools (PNG dumps of rendered frames);
# core/, platform/ and the real targets do not depend on it.
LDFLAGS ?= $(SAN) -lm -lpng16

INCLUDES := -Icore/include -Icore/rasterizer -I/usr/include/libpng16

# Header dependencies: the GCC family (Linux, mingw, DJGPP) writes one .d
# file per object (-MMD -MP), included below; otherwise a header change
# (e.g. TEX_CACHE_MAX) rebuilds only some objects, giving inconsistent
# struct layouts. Watcom (OVL) cannot produce that format; there every
# object depends on all headers (ALL_HEADERS).
DEPFLAGS := -MMD -MP
ALL_HEADERS := $(wildcard core/*.h core/include/glidulator/*.h core/rasterizer/*.h platform/dos_ovl/*.h)

# core/rasterizer/vid_voodoo_*.c is ported 86Box code (see
# core/rasterizer/ATTRIBUTION.md): own relaxed warning flags instead of
# touching the original logic for -Wextra/-Wpedantic. The shift sanitizer is
# off because the original edge equations (dxAB/dxAC/dxBC) deliberately
# shift negative values (<<4): well-defined two's complement behaviour on
# every relevant compiler/target, only UB by the standard. Every file is
# compiled separately (see the object rules below) so the flags apply per
# file.
RASTER_CFLAGS := -std=c99 -Wall -g $(SAN) -fno-sanitize=shift

CORE_SRCS   := core/glide_abi.c core/glide_modes.c core/glide_tex.c core/glide_lfb.c core/glide_sst.c core/glide_gutex.c core/glide_fog.c core/glide_stubs.c core/trace.c core/capture.c core/config.c core/renderer.c core/osd.c core/render_mt.c core/fsaa.c core/vfilter.c core/crt.c core/splash.c core/splash_orig.c
RASTER_SRCS := core/rasterizer/vid_voodoo_render.c core/rasterizer/vid_voodoo_texture.c core/rasterizer/vid_voodoo_tables.c
LINUX_SRCS  := platform/linux/platform_linux.c
TEST_SRCS   := targets/glide_test_linux/main.c

BUILD := build/linux
OBJS  := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS) $(RASTER_SRCS) $(LINUX_SRCS) $(TEST_SRCS))

MINGW_CC             := i686-w64-mingw32-gcc
MINGW_WINDRES        := i686-w64-mingw32-windres
MINGW_STRIP          := i686-w64-mingw32-strip
MINGW_CFLAGS         := -std=c99 -Wall -Wextra -Wpedantic -g -O2 $(WIN98_DEFS)
MINGW_RASTER_CFLAGS  := -std=c99 -Wall -g -O2 $(WIN98_DEFS)
WIN98_SRCS   := platform/win98/platform_win32.c
DLL_SRCS     := targets/glide_dll/dllmain.c
BUILD_WIN98  := build/win98
WIN98_OBJS   := $(patsubst %.c,$(BUILD_WIN98)/%.o,$(CORE_SRCS) $(RASTER_SRCS) $(WIN98_SRCS) $(DLL_SRCS))

.PHONY: all test clean win98 win98-trace win98-debug win98-all dos ovl ovl-trace ovl-debug ovl-all ovl-demo menu dosbox-test replay replay32 bench dist

all: test

test: $(BUILD)/glide_test_linux $(BUILD)/video_test $(BUILD)/mtrr_test
	$(BUILD)/glide_test_linux
	$(BUILD)/video_test
	$(BUILD)/mtrr_test

# Replay of frames captured by the debug variant (core/capture.c,
# targets/glide_replay): debugging with real software on Linux.
REPLAY_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS) $(RASTER_SRCS) $(LINUX_SRCS) targets/glide_replay/main.c)

$(BUILD)/glide_replay: $(REPLAY_OBJS)
	$(CC) $(REPLAY_OBJS) -o $@ $(LDFLAGS)

replay: $(BUILD)/glide_replay

# Timing and path comparison of the rasterizer (glide_replay --compare
# --bench, glidulator/renderer.h): own build without sanitizers, -O2 like the
# DLL. Another capture: make bench BENCH_DIR=... BENCH_CAP=...
BENCH_DIR ?= build/compat/nature/run
BENCH_CAP ?= CAP02.BIN
# 32-bit replay with JIT (the JIT exists for 32-bit x86 only, as in DLL and
# OVL): make replay32, then e.g.
#   build/linux32/glide_replay <dir> CAP02.BIN out.png --compare --bench 20
# Needs gcc-multilib and libpng16-16:i386 (without the dev package: linked
# directly against the .so.16).
LIBPNG32 ?= /usr/lib/i386-linux-gnu/libpng16.so.16
replay32:
	$(MAKE) replay BUILD=build/linux32 SAN= CFLAGS="-std=c99 -Wall -Wextra -g -O2 -m32" \
	    RASTER_CFLAGS="-std=c99 -Wall -g -O2 -m32" LDFLAGS="-m32 -lm $(LIBPNG32)"

bench:
	$(MAKE) replay BUILD=build/bench SAN= CFLAGS="-std=c99 -Wall -Wextra -g -O2" RASTER_CFLAGS="-std=c99 -Wall -g -O2"
	build/bench/glide_replay $(BENCH_DIR) $(BENCH_CAP) build/bench/bench.png --compare --bench 20

# Portable part of the DOS output (VBE/mode 13h, platform/dos_ovl/video.c):
# runs without BIOS, so it is tested here under ASan/UBSan.
VIDEO_TEST_OBJS := $(BUILD)/platform/dos_ovl/video.o $(BUILD)/tests/video_test.o

# MTRR planner for the OVL's write-combining (platform/dos_ovl/mtrr_plan.c)
MTRR_TEST_OBJS := $(BUILD)/platform/dos_ovl/mtrr_plan.o $(BUILD)/tests/mtrr_test.o

$(BUILD)/mtrr_test: $(MTRR_TEST_OBJS)
	$(CC) $(MTRR_TEST_OBJS) -o $@ $(SAN)

$(BUILD)/video_test: $(VIDEO_TEST_OBJS)
	$(CC) $(VIDEO_TEST_OBJS) -o $@ $(SAN)

$(BUILD)/glide_test_linux: $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD)/core/rasterizer/%.o: core/rasterizer/%.c
	mkdir -p $(dir $@)
	$(CC) $(RASTER_CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/%.o: %.c
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

win98: $(BUILD_WIN98)/glide2x.dll

# Exports only through the generated .def (3dfx names, see gen_glide2x.py);
# libgcc static because a fresh Windows 98 has no libgcc_s_dw2-1.dll.
$(BUILD_WIN98)/glide2x.dll: $(WIN98_OBJS) $(BUILD_WIN98)/glide2x_rc.o targets/glide_dll/glide2x.def
	$(MINGW_CC) $(WIN98_OBJS) $(BUILD_WIN98)/glide2x_rc.o targets/glide_dll/glide2x.def -shared -static-libgcc -o $@ -Wl,--out-implib,$(BUILD_WIN98)/glide2x.lib -lgdi32

# Version information (Explorer: Properties > Version), see glide2x.rc
$(BUILD_WIN98)/glide2x_rc.o: targets/glide_dll/glide2x.rc core/include/glidulator/version.h
	mkdir -p $(dir $@)
	$(MINGW_WINDRES) $(WIN98_DEFS) -Icore/include $< -o $@

$(BUILD_WIN98)/core/rasterizer/%.o: core/rasterizer/%.c
	mkdir -p $(dir $@)
	$(MINGW_CC) $(MINGW_RASTER_CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_WIN98)/%.o: %.c
	mkdir -p $(dir $@)
	$(MINGW_CC) $(MINGW_CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# Trace DLL (like make ovl-trace) and debug DLL (like make ovl-debug); both
# write GLIDULAT.LOG.
win98-trace:
	$(MAKE) win98 BUILD_WIN98=build/win98_trace WIN98_DEFS=-DGLIDULATOR_TRACE

win98-debug:
	$(MAKE) win98 BUILD_WIN98=build/win98_debug WIN98_DEFS="-DGLIDULATOR_TRACE -DGLIDULATOR_DEBUG"

# All three DLLs
win98-all: win98 win98-trace win98-debug

# DJGPP: -std=gnu99 instead of c99 because DJGPP's <sys/time.h>/struct
# timeval need the BSD declarations hidden by __STRICT_ANSI__.
DJGPP_PREFIX := /opt/djgpp
DJGPP_CC             := $(DJGPP_PREFIX)/bin/i586-pc-msdosdjgpp-gcc
DJGPP_CFLAGS         := -std=gnu99 -Wall -Wextra -g
DJGPP_RASTER_CFLAGS  := -std=gnu99 -Wall -g -fno-sanitize=shift
DOS_SRCS    := platform/dos/platform_dos.c
DOS_TEST_SRCS := targets/glide_dos_test/main.c
BUILD_DOS   := build/dos
DOS_OBJS    := $(patsubst %.c,$(BUILD_DOS)/%.o,$(CORE_SRCS) $(RASTER_SRCS) $(DOS_SRCS) $(DOS_TEST_SRCS))

dos: $(BUILD_DOS)/glide_dos_test.exe

$(BUILD_DOS)/glide_dos_test.exe: $(DOS_OBJS)
	$(DJGPP_CC) $(DOS_OBJS) -o $@

$(BUILD_DOS)/core/rasterizer/%.o: core/rasterizer/%.c
	mkdir -p $(dir $@)
	$(DJGPP_CC) $(DJGPP_RASTER_CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DOS)/%.o: %.c
	mkdir -p $(dir $@)
	$(DJGPP_CC) $(DJGPP_CFLAGS) $(DEPFLAGS) $(INCLUDES) -c $< -o $@

# Open Watcom: GLIDE2X.OVL is a DOS/4G LE DLL, a format only wlink can
# produce (DJGPP/mingw cannot).
WATCOM := /opt/watcom
OWBIN  := $(WATCOM)/binl64
OWENV  := env WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h PATH=$(OWBIN):$(PATH)
WCC    := $(OWENV) $(OWBIN)/wcc386
WLINK  := $(OWENV) $(OWBIN)/wlink
# -5s: Pentium instruction selection with the stack/stdcall calling
# convention (required for the decorated export names, see glide2x.h).
# -bd -s -zl: DLL code, no stack probes, no default libraries (see
# glide2x.lnk.in). -obhilert = branch prediction, expensive optimisations,
# intrinsics, loops, inlining, scheduling, speed over size. Deliberately NOT
# -oa (relaxed aliasing), -on (numerically unstable), -om (inline math):
# rasterizer and Glide computations must stay bit-identical to the GCC
# builds.
OW_OPT    := -obhilert -5s
OW_CFLAGS := -bt=dos -mf $(OW_OPT) -fp5 -fpi87 -zri -ei -j -zastd=c99 -zq -we -bd -s -zl
OVL_SRCS    := platform/dos_ovl/platform_watcom.c platform/dos_ovl/video.c platform/dos_ovl/video_dpmi.c platform/dos_ovl/mtrr_plan.c platform/dos_ovl/wc_dos.c platform/dos_ovl/smp_dos.c platform/dos_ovl/r3_dos.c platform/dos_ovl/diag_dos.c targets/glide_ovl/dllmain.c
BUILD_OVL   := build/ovl
OVL_OBJS    := $(patsubst %.c,$(BUILD_OVL)/%.obj,$(CORE_SRCS) $(RASTER_SRCS) $(OVL_SRCS))

ovl: $(BUILD_OVL)/GLIDE2X.OVL

$(BUILD_OVL)/glide2x.lnk: targets/glide_ovl/glide2x.lnk.in
	mkdir -p $(dir $@)
	sed -e 's|@WATCOM@|$(WATCOM)|g' $< > $@

$(BUILD_OVL)/GLIDE2X.OVL: $(OVL_OBJS) $(BUILD_OVL)/glide2x.lnk
	$(WLINK) @$(BUILD_OVL)/glide2x.lnk name $@ option map=$(BUILD_OVL)/GLIDE2X.map $(addprefix file ,$(OVL_OBJS))
	python3 tools/abi/lefix.py $@

$(BUILD_OVL)/%.obj: %.c $(ALL_HEADERS)
	mkdir -p $(dir $@)
	$(WCC) $(OW_CFLAGS) $(OVL_DEFS) -i=core/include -i=core/rasterizer -i=$(BUILD_OVL) -fo=$@ $<

# Startup code of the additional CPU cores (16/32 bit,
# platform/dos_ovl/smp_dos.c): GNU as -> flat bytes -> C array. Wasm/wcc
# cannot put 16-bit code into a 32-bit LE object.
$(BUILD_OVL)/platform/dos_ovl/smp_dos.obj: $(BUILD_OVL)/ap_tramp.h
$(BUILD_OVL)/ap_tramp.h: platform/dos_ovl/ap_tramp.S
	mkdir -p $(dir $@)
	$(CC) -m32 -c $< -o $(BUILD_OVL)/ap_tramp.o
	objcopy -O binary -j .text $(BUILD_OVL)/ap_tramp.o $(BUILD_OVL)/ap_tramp.bin
	xxd -i -n ap_tramp_bin $(BUILD_OVL)/ap_tramp.bin | sed -e 's/^unsigned char/static const unsigned char/' -e '/_len =/d' > $@

# Three OVL variants, each in its own build directory:
#   make ovl        build/ovl        release, no logging
#   make ovl-trace  build/ovl_trace  measurement (GLIDULATOR_TRACE): GLIDULAT.LOG
#                   with performance lines every 100 frames, first Glide calls
#                   (core/include/glidulator/trace.h), renderer, display mode,
#                   MTRR, core start; no intervention in the system
#   make ovl-debug  build/ovl_debug  diagnostics (additionally GLIDULATOR_DEBUG):
#                   own crash handling and machine-check banks (diag_dos.c),
#                   exceptions of the extra cores, memory blocks,
#                   GLIDULATOR_MT_TEST, frame capture CAPnn.BIN for
#                   glide_replay (core/capture.c), verbose texture/triangle
#                   logs
ovl-trace:
	$(MAKE) ovl BUILD_OVL=build/ovl_trace OVL_DEFS=-dGLIDULATOR_TRACE

ovl-debug:
	$(MAKE) ovl BUILD_OVL=build/ovl_debug OVL_DEFS="-dGLIDULATOR_TRACE -dGLIDULATOR_DEBUG"

# All three OVLs
ovl-all: ovl ovl-trace ovl-debug

# Demo menu for the test stick (16-bit DOS, runs under FreeDOS and in the
# Windows 98 DOS box), tools/usbstick/menu/ (not part of the public
# repository)
ifneq ($(wildcard tools/usbstick/menu/menu.c),)
build/menu/GLMENU.EXE: tools/usbstick/menu/menu.c
	mkdir -p build/menu
	cd build/menu && $(OWENV) $(OWBIN)/wcl -bt=dos -ms -os -zq -we -wx -fe=GLMENU.EXE $(CURDIR)/tools/usbstick/menu/menu.c

menu: build/menu/GLMENU.EXE
endif

# Loads the real, Watcom-built GLIDE2X.OVL dynamically (the code path of a
# real DOS game: name lookup in the LE export table) and calls it, as a
# 32-bit Linux process via tools/leload/ (MIT), no DOSBox needed. See
# tools/leload_demo/main.c.
ovl-demo: $(BUILD_OVL)/GLIDE2X.OVL build/leload_demo
	build/leload_demo $(BUILD_OVL)/GLIDE2X.OVL

build/leload_demo: tools/leload_demo/main.c tools/leload/leload.c tools/leload/leload.h
	$(CC) -m32 -std=c99 -Wall -g -I tools/leload -o $@ tools/leload_demo/main.c tools/leload/leload.c

# Real execution under DOS: targets/glide_dos_ovltest (OVLTEST.EXE, a
# DOS/4GW program) loads GLIDE2X.OVL like a game and renders a test picture
# with the REAL DPMI/VBE layer, not the simulated BIOS of ovl-demo. Runs in
# DOSBox on an Xvfb display; checked by screenshot (tools/dosbox_test/).
# Needs dosbox, Xvfb, xwd, xwininfo. Four runs: VBE (machine=svga_s3) and
# mode 13h fallback (vgaonly), each under DOS/4GW and DOS/32A (the second
# extender checks the assumption "flat model, base 0" in
# platform/dos_ovl/video_dpmi.c).
OVLTEST_CFLAGS := -bt=dos -mf -3r -fp5 -zastd=c99 -zq -we -wx
BUILD_DOSBOX   := build/dosbox

dosbox-test: $(BUILD_DOSBOX)/OVLTEST.EXE $(BUILD_OVL)/GLIDE2X.OVL
	cp $(BUILD_OVL)/GLIDE2X.OVL $(BUILD_DOSBOX)/GLIDE2X.OVL
	cp $(WATCOM)/binw/dos4gw.exe $(BUILD_DOSBOX)/DOS4GW.EXE
	cp $(WATCOM)/binw/dos32a.exe $(BUILD_DOSBOX)/DOS32A.EXE
	tools/dosbox_test/run.sh $(BUILD_DOSBOX) svga_s3 DOS4GW vbe
	tools/dosbox_test/run.sh $(BUILD_DOSBOX) svga_s3 DOS32A vbe
	tools/dosbox_test/run.sh $(BUILD_DOSBOX) vgaonly DOS4GW mode13
	tools/dosbox_test/run.sh $(BUILD_DOSBOX) vgaonly DOS32A mode13

$(BUILD_DOSBOX)/OVLTEST.EXE: targets/glide_dos_ovltest/main.c tools/leload/leload.c tools/leload/leload.h platform/dos_ovl/video.h
	mkdir -p $(BUILD_DOSBOX)
	$(WCC) $(OVLTEST_CFLAGS) -fo=$(BUILD_DOSBOX)/ovltest.obj targets/glide_dos_ovltest/main.c
	$(WCC) $(OVLTEST_CFLAGS) -fo=$(BUILD_DOSBOX)/leload.obj tools/leload/leload.c
	$(WLINK) system dos4g option quiet name $@ file $(BUILD_DOSBOX)/ovltest.obj file $(BUILD_DOSBOX)/leload.obj

# Release package: tests first, then all six binaries.
#   build/dist/glidulator-<version>.zip          for users: BIN\ (release),
#       BIN\TRACE\, BIN\DEBUG\ (OVL and DLL each), GLIDULAT.CFG, texts
#       with CRLF line ends, DLLs without symbols
#   build/dist/glidulator-<version>-symbols.zip  for crash analysis: DLLs
#       with symbols and the OVL maps (tools/dosvm/ovladdr.py); keep it for
#       every package handed out
VERSION  := $(shell sed -n 's/^\#define GLIDULATOR_VERSION *"\(.*\)"/\1/p' core/include/glidulator/version.h)
DIST     := build/dist/glidulator-$(VERSION)
DIST_SYM := build/dist/glidulator-$(VERSION)-symbols
CRLF     := sed -e 's/\r$$//' -e 's/$$/\r/'

dist: test ovl-all win98-all
	rm -rf $(DIST) $(DIST_SYM) $(DIST).zip $(DIST_SYM).zip
	mkdir -p $(DIST)/BIN/TRACE $(DIST)/BIN/DEBUG $(DIST_SYM)/TRACE $(DIST_SYM)/DEBUG
	cp build/ovl/GLIDE2X.OVL $(DIST)/BIN/
	cp build/ovl_trace/GLIDE2X.OVL $(DIST)/BIN/TRACE/
	cp build/ovl_debug/GLIDE2X.OVL $(DIST)/BIN/DEBUG/
	$(MINGW_STRIP) -o $(DIST)/BIN/glide2x.dll build/win98/glide2x.dll
	$(MINGW_STRIP) -o $(DIST)/BIN/TRACE/glide2x.dll build/win98_trace/glide2x.dll
	$(MINGW_STRIP) -o $(DIST)/BIN/DEBUG/glide2x.dll build/win98_debug/glide2x.dll
	cp build/ovl/GLIDE2X.map build/win98/glide2x.dll $(DIST_SYM)/
	cp build/ovl_trace/GLIDE2X.map build/win98_trace/glide2x.dll $(DIST_SYM)/TRACE/
	cp build/ovl_debug/GLIDE2X.map build/win98_debug/glide2x.dll $(DIST_SYM)/DEBUG/
	sed -e "s/@VERSION@/$(VERSION)/" package/README.TXT | $(CRLF) > $(DIST)/README.TXT
	$(CRLF) package/GLIDULAT.CFG > $(DIST)/GLIDULAT.CFG
	$(CRLF) COPYING > $(DIST)/COPYING.TXT
	$(CRLF) docs/user-guide.md > $(DIST)/MANUAL.TXT
	$(CRLF) docs/compatibility.md > $(DIST)/COMPAT.TXT
	cd build/dist && zip -q -r -X glidulator-$(VERSION).zip glidulator-$(VERSION) && \
	    zip -q -r -X glidulator-$(VERSION)-symbols.zip glidulator-$(VERSION)-symbols
	@ls -l $(DIST).zip $(DIST_SYM).zip

clean:
	rm -rf build

-include $(shell find build -name '*.d' 2>/dev/null)
