# SGW Video Fix

A drop-in `wmvcore.dll` proxy that fixes the greyscale-upside-down WMV3
video bug in **Sniper Ghost Warrior** (Techland Chrome Engine 4, 2010)
on modern Windows.

The same fix also applies to other Chrome Engine 4 titles affected by
the same bug, including **Call of Juarez: Bound in Blood**.

| | |
|---|---|
| **Status** | v0.1 - production |
| **Target** | Sniper Ghost Warrior (Steam, GOG, retail) on Windows 8 / 10 / 11 |
| **Architecture** | x86 (32-bit) Win32 DLL, MSVC /MT static CRT |
| **Size** | ~88 KB |
| **License** | MIT |
| **Source** | One C++ file. Builds in under a second. |

---

## What it fixes

The opening cinematic, the Techland logo, and the main-menu background
video all play as a **greyscale, upside-down** picture on Windows 8
and later. With this DLL installed they play in full colour, right
side up, exactly the way they did on Windows XP / Vista / 7.

### For gamers (the layman version)

Sniper Ghost Warrior was built in 2010, when "modern Windows" meant
Windows 7. The game asks Windows to decode its `.wmv` cinematics into
ordinary colour pictures, hands those pictures to the graphics card,
and draws them on screen.

The catch is that on Windows 8 and later, Windows changed *how* it
hands the decoded video back. It started giving the picture in a
different storage layout - one that splits the brightness from the
colour. The game does not understand the new layout. It just grabs
the brightness half and treats it as if it were a finished colour
picture, which is why everything looks like a black-and-white photo.
The "upside down" part is a separate quirk of how the game's old
graphics path expected its frames to be loaded.

Nothing in the game itself is broken. The decoder is fine. The
graphics card is fine. The only piece that is wrong is the one tiny
adapter that sits between Windows' video decoder and the game's
graphics upload, and that adapter is what this DLL replaces. It
listens for Sniper Ghost Warrior's video frames, converts them back
into the colour layout the game's graphics path expects, and lets the
game draw them as it always did.

To install: drop `wmvcore.dll` next to `Sniper_x86.exe`. To uninstall:
delete it. The game's own files are never touched.

### For coders (the technical version)

#### Engine side

Chrome Engine 4 plays its WMV3 cinematics through the Windows Media
Format SDK. The engine creates an `IWMReader`, hands it a callback
object, and receives one decoded `INSSBuffer` per frame in
`OnSample`. The engine then uploads the buffer to a managed-pool
D3D9 surface using `D3DXLoadSurfaceFromMemory`, calling it like this:

```cpp
D3DXLoadSurfaceFromMemory(
    pDestSurface,                  // managed A8R8G8B8 surface
    NULL, NULL,                    // dest palette + dest rect
    pSrcMemory,                    // pointer into the decoder buffer
    D3DFMT_L8,                     // <-- ASSUMES THE FRAME IS L8
    width,                         // src pitch
    NULL, &flippedSrcRect,         // src rect with bottom > top
    D3DX_FILTER_NONE, 0);
```

There are two important assumptions hard-coded into that call:

1. The decoded video bytes can be read as `D3DFMT_L8` (8-bit
   single-channel luminance) and the loader will spread them across
   the destination's R, G and B channels. That happens to look correct
   if the source is actually packed grey-on-RGB - which it was on
   Windows XP / Vista / 7, where `wmvcore.dll` returned each WMV3
   frame in a packed RGB format that put luminance into each byte
   triple in a way the engine's RGB upload pipeline accepted.

2. The `srcRect` is vertically flipped (its `bottom < top` after
   the engine's adjustment), so the loader does the vertical flip
   inside the same call. The engine never has to flip the bytes
   itself.

#### Windows side

On Windows 8 the system `wmvcore.dll` was rewritten on top of the
Media Foundation Transform infrastructure. The default output type
of the WMV3 decoder MFT changed from a packed RGB format (which is
what XP / Vista / 7 used to negotiate by default) to **I420** -
ITU-R BT.601 limited-range planar YUV 4:2:0. An I420 frame for a
1280x720 video lays out:

```
[ Y plane:     1280 * 720 = 921600 bytes  ]
[ U plane:      640 * 360 = 230400 bytes  ]
[ V plane:      640 * 360 = 230400 bytes  ]
```

The Y plane *alone* happens to be exactly `width * height` bytes -
the same byte count Chrome Engine 4 already allocates for its frame
buffers. So the engine never crashes, never reads out of bounds.
What it does is read the Y plane as if it were L8, splat the Y bytes
into R/G/B (giving you a flat greyscale picture - no colour
information at all because the U and V planes are simply ignored),
and use the engine's flipped `srcRect` on a buffer that no longer
needs flipping (because the planar layout puts row 0 at the top
already, the way the engine's old RGB path didn't).

End result: a greyscale, upside-down video.

Notably, the engine's CDecoderBuffer ctor allocates `width*height`
bytes - exactly the Y plane size, but **not** enough for all three
planes - so even if Windows wrote all of I420 into the buffer, the U
and V planes would overflow. WMF detects this via
`INSSBuffer::GetMaxLength` and refuses to write the chroma planes at
all on stock; the engine then receives only the Y plane.

#### Why it worked in the past but not on recent Windows

On Windows XP / Vista / 7 the system `wmvcore.dll` negotiated a
packed RGB output format with the WMV3 decoder by default. Inside
that DLL the format chain went WMV3 bitstream -> internal YUV ->
**RGB conversion** -> caller. The caller (Chrome Engine 4) received
finished RGB pixels and uploaded them as `D3DFMT_L8` - which happens
to copy bytes one-to-one without any swizzling, so the picture
displayed was the picture decoded.

On Windows 8 the rewrite removed that default RGB conversion. The
default became I420 because (a) it is the format the underlying H.264
and HEVC decoders also speak, so the Media Foundation pipeline became
uniform across codecs, and (b) modern callers handle YUV themselves
on the GPU. Sniper Ghost Warrior is a 2010 game with no idea this
change ever happened.

Microsoft has not advertised this as a breaking change because, from
a Media Foundation point of view, it isn't one - any caller using
`IMFTransform::SetOutputType` or negotiating output media types
properly will request whichever format it wants. The breakage only
affects callers that assumed the Windows 7 default would persist.

#### How this DLL fixes it

Five hooks, all installed at runtime, none of them in `DllMain`:

1. **`IWMReader::Open`** (vtable slot 3) - swap the game's
   `IWMReaderCallback` for our wrapper so we see every decoded frame.

2. **`IWMReader::SetOutputProps`** (vtable slot 7) - read the video's
   actual output dimensions from the WMF media type and remember them.

3. **`CDecoderBuffer` constructor** in `engine_x86.dll` - a 7-byte
   x86 trampoline that scales the engine's `width*height` malloc by
   3/2 so the buffer is large enough to hold a full I420 frame
   (Y + half-res U + half-res V). The engine's size *fields* are
   left unchanged so the engine still believes it owns a `width*height`
   buffer; only the underlying allocation grows.

4. **`INSSBuffer::SetLength` / `::GetMaxLength`** (vtable slots 5
   and 4) on the engine's `CDecoderBuffer` vtable - tell WMF that
   our enlarged buffer can hold the full I420 payload, while still
   reporting `width*height` to the engine. A vtable identity check
   guards both hooks so audio and metadata buffers (which share the
   same `INSSBuffer` interface) are not affected.

5. **`D3DXLoadSurfaceFromMemory`** IAT slot in `engine_x86.dll`'s
   import of `d3dx9_39.dll` - the actual fix. Walks the engine's PE
   import directory at runtime, finds the slot for the `d3dx9_39.dll`
   export `D3DXLoadSurfaceFromMemory`, and overwrites it with a
   pointer to our replacement. The replacement matches by exact
   pointer-identity against the currently-active decoder buffer; on
   match it substitutes:

   - `pSrcMemory` -> our pre-converted BGRA staging buffer
   - `SrcFormat` -> `D3DFMT_A8R8G8B8`
   - `SrcPitch` -> `width * 4`
   - `pSrcRect` -> `(0, 0, width, height)` (full frame)

   and forwards to the real D3DX. Non-matching calls pass straight
   through unchanged.

The colour-space conversion itself is a BT.601 limited-range I420 ->
BGRA integer kernel with the vertical flip baked into the row indexing
(because the engine's flipped `srcRect` is no longer suitable for our
top-row-first BGRA buffer, so we substitute a non-flipped `srcRect`
above and pre-flip the bytes here). Coefficients are `298/409/100/208/516`
- the integer form of the standard BT.601 inverse matrix scaled by 256
- which is the same matrix Chrome Engine 4's own YUV pixel shader
uses internally.

A scalar reference path is provided alongside an SSE2 path that
processes 8 pixels per inner-loop iteration via `_mm_madd_epi16`.
SSE2 is unconditionally available on every CPU running Windows 8 and
later, so the SSE2 path is the default. Per-frame conversion cost
on a 1280x720 frame is well under 1 ms on any modern CPU.

---

## Install

See [INSTALL.md](INSTALL.md).

Short version: place `wmvcore.dll` next to `Sniper_x86.exe` (the
folder that contains the executable, not the Steam library root).
For Sniper Ghost Warrior on Steam that is:

```
<SteamLibrary>\steamapps\common\Sniper Ghost Warrior\
```

Run the game. Uninstall by deleting the DLL.

## Build from source

See [BUILD.md](BUILD.md).

Short version:

```cmd
build.bat
```

Run from an **x86 Native Tools Command Prompt for VS 2022** (or
newer). Output is `build\wmvcore.dll`. The default build is silent;
set `WMVPROXY_EXTRA_DEFS=/D WMVPROXY_VERBOSE=1` before running
`build.bat` to produce a diagnostic build that writes `wmvproxy.log`
next to itself.

## Source layout

```
src/
  sgw_video_fix.cpp    Single C++ source file. ~1300 lines including
                       extensive doc comments. Builds standalone -
                       no third-party dependencies, no headers
                       outside the Windows SDK.

  sgw_video_fix.def    Module-definition file. Names the DLL
                       `wmvcore` and aliases each documented wmvcore
                       export to its corresponding `My_*` thunk.

build.bat              Build script. Invokes `cl.exe` directly.
LICENSE                MIT.
README.md              This file.
INSTALL.md             End-user install instructions.
BUILD.md               Developer build instructions.
```

## Compatibility

Tested on:

- Sniper Ghost Warrior, Steam edition, Windows 11 23H2, x64.

Should work on (same engine, same wmvcore.dll bug, same `D3DXLoadSurfaceFromMemory`
import name):

- Sniper Ghost Warrior, GOG / retail editions.
- Call of Juarez: Bound in Blood.
- Any other Chrome Engine 4 (2010) title that imports
  `D3DXLoadSurfaceFromMemory` from `d3dx9_39.dll`.

Will not affect any other game on the system - this DLL only takes
effect for the process it is loaded into.

## Test material

These are the video files Chrome Engine 4 plays through `wmvcore.dll`
on Sniper Ghost Warrior. They live under
`Game\Data\Menu\Movies\` in the install folder. The bug affects
every entry that has a real video stream.

| File | Size | Resolution | Codec | Duration | Bitrate |
|---|---:|---|---|---:|---:|
| `logo.wmv` | 1,525,856 | 1280 x 720 | WMV3 Main, yuv420p, 30 fps | 8.17 s | 1.5 Mbps |
| `intro.wmv` | 82,599,466 | 1280 x 720 | WMV3 Main, yuv420p, 30 fps | 123.60 s | 5.3 Mbps |
| `menumovie_0.wmv` | 5,920,947 | 1280 x 720 | WMV3 Main, yuv420p, 30 fps | 10.47 s | 4.5 Mbps |
| `menumovie_0_43.wmv` | 5,864,947 | 1024 x 768 | WMV3 Main, yuv420p, 30 fps | 10.47 s | 4.5 Mbps |
| `credits.wmv` | 56,286,344 | 1280 x 720 | WMV3 Main, yuv420p, 30 fps | 140.84 s | 3.2 Mbps |
| `initial_bg.wmv` | 22,983 | (no video) | WMA2 audio stub, 44.1 kHz stereo | 0.51 s | 192 kbps |

`menumovie_0_43.wmv` is the 4:3 menu background variant the engine
selects on non-widescreen displays. `initial_bg.wmv` is not actually
a video at all - it is a 0.51-second audio stub the engine plays once
during the splash sequence. Every other file is a `wmv3` (Windows
Media Video 9) Main-profile bitstream with a `yuv420p` source layout 
which is exactly the format that triggers the
greyscale-upside-down rendering bug on Windows 8 and later, because
the engine's `D3DXLoadSurfaceFromMemory` upload assumes the byte
layout that XP/Vista/7 returned (packed RGB) and not the planar
I420 the modern decoder produces.

### MD5 (Steam edition)

```
b7fdebf5e0d0ed88181b90b8969f7e8a  credits.wmv
1f012e55e6134b0779b9c1573af11281  initial_bg.wmv
c457d73ffa583a4ec904c30989ea37e1  intro.wmv
affbaef58a5fd7d25c2e5bb93c032203  logo.wmv
5e5b41dbcec7e6068a3b12b9842685dc  menumovie_0.wmv
6c3d020f7d3e6a612d7d23750cb61d6a  menumovie_0_43.wmv
```

### Steam vs. original disc release

Five of the six files in the Steam edition are byte-for-byte identical
to the same files extracted from the original 2010 retail / disc
release. Only `intro.wmv` differs:

| | Steam | 2010 disc |
|---|---:|---:|
| Size | 82,599,466 bytes | 110,761,967 bytes |
| Bitrate (avg) | 5.35 Mbps | 7.16 Mbps |
| Duration | 123.60 s | 123.80 s |
| Codec / dimensions | WMV3 Main, 1280x720, yuv420p | WMV3 Main, 1280x720, yuv420p |
| MD5 | `c457d73ffa583a4ec904c30989ea37e1` | `03b0867559adc602f85b42aacf2ca8d5` |

In other words, the Steam release shipped a re-encoded `intro.wmv`
at a lower bitrate to shrink the download (saving ~28 MB), but kept
the same codec, resolution, frame rate and pixel format. Both
encodings are affected by the bug identically because both are WMV3
`yuv420p` and the bug is in how the engine uploads the decoded I420
output, not in any property of the bitstream itself. **This DLL fixes
both editions equally.**

The codec / dimensions / MD5 information above was captured with
ffprobe (FFmpeg 6.x) and `md5sum`. If your local copies of these
files have different MD5s than the Steam list above, you almost
certainly have one of:

- The original 2010 retail / disc release (only `intro.wmv` will
  differ; the other five MD5s should still match).
- A localized regional release (some EU SKUs shipped a different
  `credits.wmv` audio track).
- A user-modded install (e.g. someone replaced `menumovie_0.wmv`
  with their own clip).

The bug and the fix do not depend on any of those - any
WMV3 / `yuv420p` source will hit the same engine code path.

## What this DLL does NOT do

- Re-encode the video. The WMV3 bitstream is decoded exactly once by
  the system `wmvcore.dll`, colour-space transformed exactly once by
  this DLL, and uploaded exactly once by the engine.
- Modify any game files.
- Modify the system `wmvcore.dll` or any other system file.
- Touch the network, the registry, the filesystem outside its own
  folder, or anything else outside its own process.
- Affect any DRM, content protection, or licensing path. The
  `WMValidate*` and DRM-related exports are forwarded to the system
  DLL unchanged.

## License

MIT. See [LICENSE](LICENSE).

## Credits

- **Rex** - project direction, reverse engineering, BT.601 colour-math
  verification, in-game testing.
- **Claude (Anthropic)** - AI coding assistant, wrote the bulk of the
  C++ under Rex's direction.
