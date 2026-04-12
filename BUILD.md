# Build from source

## Requirements

- **Windows**. Build host needs to be Windows because the project
  uses MSVC and the Windows SDK headers.
- **Visual Studio 2022 (or newer)** with the **x86 (32-bit)** C++
  toolchain installed. Free Community edition is fine. Other MSVC
  versions back to 2015 should work but are not tested.
- **Windows SDK** (any recent version). Installed automatically by
  the VS C++ workload. The build needs `windows.h`, `wmsdk.h`,
  `emmintrin.h`.

The project intentionally has **no third-party dependencies**:

- No CMake, no MSBuild, no vcpkg.
- No external libraries beyond the Windows SDK and the static MSVC
  runtime (`/MT`).
- One C++ source file, one .def file, one .bat build script.

## Build (default - silent release)

1. Open the **x86 Native Tools Command Prompt for VS 2022**.
   *(Important: it MUST be the x86 prompt, not x64. The game is
   32-bit and so is this DLL.)*

2. `cd` into the repo root and run:

   ```cmd
   build.bat
   ```

3. Output is `build\wmvcore.dll` (~88 KB).

This is the silent production build. It logs nothing, has zero
runtime overhead beyond the converter, and is what end users should
install.

## Build (verbose - diagnostic)

To produce a build that writes `wmvproxy.log` next to itself:

```cmd
set WMVPROXY_EXTRA_DEFS=/D WMVPROXY_VERBOSE=1
build.bat
```

The verbose build is intended for debugging install issues or
verifying that the hooks are firing correctly. It writes to
`wmvproxy.log` from a critical section so the cost is small but
non-zero. Do not ship this build to end users.

## Build options

The build script honours one environment variable, `WMVPROXY_EXTRA_DEFS`,
which is appended verbatim to the `cl.exe` command line. The
following preprocessor switches are recognised by the source:

| Switch | Default | Effect |
|---|---|---|
| `WMVPROXY_VERBOSE` | `0` | Enables `wmvproxy.log` writes and `OutputDebugStringA` mirroring of every diagnostic message. |
| `WMVPROXY_USE_SSE2` | `1` | Use the SSE2 I420->BGRA path. SSE2 is unconditionally available on every CPU running Windows 8+. Set to `0` to force the scalar reference path. |
| `WMVPROXY_TIME_CONVERT` | `0` | Per-frame `QueryPerformanceCounter` timing of the colour-space conversion. Logged every 120 frames. Only takes effect when `WMVPROXY_VERBOSE=1`. |
| `WMVPROXY_D3DX_PASSIVE` | `0` | Make the D3DX hook log every call but never substitute. Discovery / sanity-check only - do **not** ship this build. |

Example - verbose with SSE2 timing:

```cmd
set WMVPROXY_EXTRA_DEFS=/D WMVPROXY_VERBOSE=1 /D WMVPROXY_TIME_CONVERT=1
build.bat
```

## Build options that are NOT exposed as switches

The following are hard-coded constants in `src/sgw_video_fix.cpp`
because they describe a specific binary - the SGW 2010 build of
`engine_x86.dll`. If you are porting the proxy to a different game
or a different engine build, these are the values to change:

| Constant | Value | Description |
|---|---|---|
| `kEngineRva_CDecoderBufferCtor` | `0x2D0C0` | RVA of `CDecoderBuffer::CDecoderBuffer(int)` inside `engine_x86.dll`. |
| `kEngineRva_CDecoderBufferVtable` | `0x3B26EC` | RVA of the `CDecoderBuffer` vtable inside `engine_x86.dll`. |

The `D3DXLoadSurfaceFromMemory` hook does NOT depend on any RVA - it
walks the engine's PE import directory at runtime and looks the
function up by name. It will work on any Chrome Engine 4 build that
imports `D3DXLoadSurfaceFromMemory` from `d3dx9_*.dll`.

## What the build does

`build.bat` invokes `cl.exe` directly:

```
cl /nologo /O2 /W3 /EHsc /MT /D WIN32 /D NDEBUG /D _WINDOWS \
   src\sgw_video_fix.cpp \
   /link /DLL /MACHINE:X86 /DEF:src\sgw_video_fix.def \
   /OUT:build\wmvcore.dll \
   kernel32.lib ole32.lib oleaut32.lib user32.lib strmiids.lib
```

Notes:

- **`/MT` (static CRT)**. The DLL must not depend on a vcredist
  install on the user's machine.
- **`/MACHINE:X86`**. 32-bit only. The game is 32-bit; a 64-bit DLL
  cannot be loaded into a 32-bit process.
- **`/DEF:`**. Names the DLL `wmvcore` (so the loader resolves the
  game's `wmvcore.dll` import to us) and aliases each documented
  wmvcore export to its `My_*` thunk in the source.
- **`strmiids.lib`**. Pulls in the GUIDs for `IID_IWMReaderCallback`,
  `IID_IWMReaderCallbackAdvanced`, etc.
- No PDB is generated for release builds. Add `/Zi /DEBUG` to the
  link line if you want one.

A successful build prints `SUCCESS: build\wmvcore.dll` and leaves
exactly one file in `build\`: `wmvcore.dll`. Intermediate `.obj`,
`.exp`, and `.lib` files are deleted automatically.

## Verifying the build

The default release build is approximately 88 KB. If your build is
significantly larger:

- Did you forget `/MT`? `/MD` would link against the dynamic CRT and
  pull in import stubs.
- Did you build x64 by accident? Re-open the x86 Native Tools prompt.
- Did `WMVPROXY_VERBOSE=1` slip in via a stale environment variable?
  Diagnostic builds are slightly larger.

To sanity-check the resulting DLL:

```cmd
dumpbin /exports build\wmvcore.dll
```

You should see all of `WMCreateReader`, `WMCreateSyncReader`,
`WMCreateWriter`, `WMCheckURLExtension`, etc., as documented exports.

```cmd
dumpbin /headers build\wmvcore.dll | findstr machine
```

Should print `14C machine (x86)`. (`8664 machine (x64)` means you
built the wrong architecture.)
