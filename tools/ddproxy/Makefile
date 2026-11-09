# Logging proxy ddraw.dll for Windows 9x (see README.md).
CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -std=gnu99 -fno-strict-aliasing -Wno-cast-function-type
OUT = ../../build/ddproxy

all: $(OUT)/ddraw.dll $(OUT)/glddraw.dll $(OUT)/glrun.exe $(OUT)/ddtest.exe

SRC = ddproxy.c inject.c forward.S

# Same code twice: as DDRAW.DLL it is a drop-in proxy, as GLDDRAW.DLL it
# is loaded by GLRUN.EXE and redirects the imports.
$(OUT)/ddraw.dll: $(SRC) forward.h inject.h ddraw.def
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -shared -o $@ $(SRC) ddraw.def \
		-static-libgcc -Wl,--enable-stdcall-fixup -s

$(OUT)/glddraw.dll: $(SRC) forward.h inject.h ddraw.def
	mkdir -p $(OUT)
	sed 's/^LIBRARY DDRAW/LIBRARY GLDDRAW/' ddraw.def > $(OUT)/glddraw.def
	$(CC) $(CFLAGS) -shared -o $@ $(SRC) $(OUT)/glddraw.def \
		-static-libgcc -Wl,--enable-stdcall-fixup -s

$(OUT)/glrun.exe: glrun.c inject.c inject.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -mwindows -o $@ glrun.c inject.c -s

$(OUT)/ddtest.exe: ddtest.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ ddtest.c -s

clean:
	rm -rf $(OUT)
