# Building R1Q2 with Visual Studio 2022

Open `r1q2.sln`, select an **x86** configuration, build.

## One-time setup

1. Visual Studio 2022 with the **Desktop development with C++** workload
   (MSVC v143 + a Windows 10/11 SDK). DirectInput/DirectSound/DirectDraw headers
   and libs come from the Windows SDK; the old DirectX SDK is not needed.
2. zlib, libpng and libjpeg-turbo are pulled in through vcpkg manifest mode
   (`vcpkg.json`). If you don't have vcpkg set up yet:

       git clone https://github.com/microsoft/vcpkg C:\vcpkg
       C:\vcpkg\bootstrap-vcpkg.bat
       C:\vcpkg\vcpkg integrate install

   The first build compiles the three libraries (a few minutes, then cached in
   `vcpkg_installed\`). The projects use the `x86-windows-static-md` triplet
   (static libs, dynamic CRT), matching the original `/MD` + static `zlib.lib` setup.

## Projects and configurations

| Project  | Output          | Notes |
|----------|-----------------|-------|
| `r1q2`   | `r1q2.exe`      | client + server. `Dedicated` config builds `dedicated.exe` (`DEDICATED_ONLY`, client files excluded) |
| `ref_gl` | `ref_r1gl.dll`  | OpenGL renderer |
| `game`   | `gamex86.dll`   | baseq2 game DLL (copy into `baseq2\`) |

Configurations: `Debug`, `Release`, `Dedicated` (Dedicated only builds `r1q2`).
All output goes to `build\bin\<Config>\` so `r1q2.exe` finds `ref_r1gl.dll` next to it;
objects go to `build\obj\<project>\<Config>\`.

## Differences from the VC6 `.dsp` files

- **Source lists** were rebuilt from the tree: the `.dsp` omitted `unzip.c` and
  `ioapi.c` (required by `files.c`, so it could not link), plus the self-gated
  `cl_http.c` and `sv_anticheat.c`. They compile to nothing unless `USE_CURL` /
  `ANTICHEAT` is defined. The old stale reference to `qcommon/bspfile.h` is gone.
- **`/arch:IA32` (x87 math)** on x86. VS2022 defaults to SSE2, but the engine calls
  `_controlfp(_PC_24, _MCW_PC)` and predates SSE2. Switch to SSE2 only if you
  want to, and test netcode/prediction after.
- **`dsound.lib`** is linked into the client configs: `snd_win.c` calls `DirectSoundCreate`
  directly (its `LoadLibrary` path is commented out) and the `.dsp` never listed the library.
- **`GAME_DLL`** is defined for the game DLL in every config (the `.dsp` only set it in
  Debug; the Linux Makefile always sets it).
- **Debug uses `/MDd`** (the `.dsp` used `/MD`) so it links against vcpkg's debug libs.
  Runtime checks (`/RTC1`) are left off, as in the original.
- Optional features (`USE_CURL`, `USE_OPENSSL`, `USE_OPENAL`, `ANTICHEAT`, ...) are
  off, same as the `.dsp`. Add them to *Preprocessor Definitions* and supply the libs.

## Source edits needed for modern toolchains

- `ref_gl/gl_image.c`: libpng >= 1.5 made `png_info` opaque and removed
  `png_check_sig` / `png_set_gray_1_2_4_to_8`; libjpeg(-turbo) now declares its own
  `jpeg_mem_src`, so the engine's copy is renamed `GL_jpeg_mem_src`. Accessor-function
  rewrite; behavior unchanged. This was compile-checked against libpng 1.6.43 and
  libjpeg-turbo headers.
- `win32/q2.rc`: `afxres.h` (needs the optional MFC component) -> `winres.h`.

## Not included

- `client_dll/` (no project ever existed for it), the software renderer
  (`win32/rw_*.c` need the `ref_soft` directory that is not in this repo), and
  `server/sv_null.c`. `qcommon/net_common.c` is `#include`d by `net_wins.c`, not compiled alone.
- x64: the source has `_M_AMD64` paths but only Win32 x86 is set up here.
