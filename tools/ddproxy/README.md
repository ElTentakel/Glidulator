# ddproxy: logging DirectDraw proxy for Windows 9x

Feasibility tool for a possible Direct3D wrapper (not part of the release package).
`make -C tools/ddproxy` builds `build/ddproxy/ddraw.dll`, `glddraw.dll`,
`glrun.exe` and `ddtest.exe`.

- `ddraw.dll`: put it next to a game's EXE. It loads the system
  `DDRAW.DLL` by full path, forwards every export to it and writes
  `DDPROXY.LOG` next to itself: process, command line, which module was
  loaded, the interfaces asked for, the first calls of every method with
  arguments, render and texture stage states on first use, surface
  descriptions, and call totals at exit. `IDirectDraw*`, `IDirect3D*` and
  `IDirect3DDevice*` are wrapped; surfaces, textures and viewports stay the
  real objects, except vertex buffers (they get the device as an argument,
  so wrapped objects passed to them are swapped back for the real ones).
- `glrun.exe <program> [arguments]` with `glddraw.dll` (the same code under
  another module name) in GLRUN's folder: starts the program with
  `GLDDRAW.DLL` loaded before its entry point (entry point patched with
  `jmp $`, then a short `LoadLibraryA` stub there; works on Windows 9x
  without `VirtualAllocEx`/`CreateRemoteThread`). The DLL redirects the
  DirectDraw imports of all modules from the program's folder, plus
  `LoadLibraryA`/`GetProcAddress` (later modules, hand-made loaders such
  as Half-Life's `hw.dll`) and `CreateProcessA`/`WinExec` (child processes
  get the DLL as well). The log goes to `DDPROXY.LOG` next to GLRUN.
- `ddtest.exe`: `ddtest` (which `DDRAW.DLL` does this process get),
  `ddtest hold` (the same, stays loaded), `ddtest ps` (every process with a
  `DDRAW.DLL` module), `ddtest ds` (DirectSound only, then the list).

`forward.S`, `forward.h` and `ddraw.def` list the forwarded exports of the
DirectX 8.1 `DDRAW.DLL` of Windows 98 (without the thunk data exports).

Options in `DDPROXY.CFG` next to the DLL (any order):

- `fakehal`: report `DDCAPS_3D` and a 16-bit Z-buffer, offer the RGB
  software device a second time as the HAL device (`CreateDevice(HAL)`
  then creates the RGB device), and place textures and Z-buffers in system
  memory. For games that refuse to start without 3D hardware.
- `fakepal8`: offer an additional 8-bit palettized texture format.

The log also shows the devices offered with their caps (table/vertex/W/Z
fog), the texture formats, every change of the fog render states and
`MessageBoxA` texts with the caller's address. Vertex buffers and
`IDirect3DMaterial3` are wrapped as well, because they get the device as
an argument.

Under Wine, run games in a virtual desktop (`wine explorer /desktop=...`)
when they look for display modes such as 640x480x16.

## Limitation on Windows 9x

Windows 9x shares 32-bit modules between processes by module name. As soon
as any process has the system `DDRAW.DLL` loaded, every implicit import or
`LoadLibrary("ddraw.dll")` gets the system DLL, not the one in the program
folder. `DDHELP.EXE` keeps the system DLL loaded until the next reboot; it
is started by the first `DirectDrawCreate` or `DirectSoundCreate` of the
session. So the proxy only sees programs started before any DirectX use
since boot. `LoadLibrary` with a full path still loads a second module with
the same base name, which is how the proxy reaches the system DLL.

`glrun.exe` avoids this: the wrapper has its own module name, and the
program is redirected to it no matter what is loaded elsewhere.
