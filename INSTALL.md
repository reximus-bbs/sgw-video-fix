# Install / Uninstall

## Install

1. Download `wmvcore.dll` from the
   [Releases](https://github.com/reximus-bbs/sgw-video-fix/releases)
   page (or build it yourself with `build.bat`).

2. Locate your Sniper Ghost Warrior install folder. This is the
   folder that contains **`Sniper_x86.exe`**.

   **Steam:**
   ```
   <SteamLibrary>\steamapps\common\Sniper Ghost Warrior\
   ```
   You can find it from inside Steam: right-click the game ->
   *Manage* -> *Browse local files*.

   **GOG:**
   ```
   <GOG library>\Sniper Ghost Warrior\
   ```

   **Retail / disc install:**
   Wherever you installed it. The folder containing `Sniper_x86.exe`.

3. **Back up the existing `wmvcore.dll`** if there is one in that
   folder already. There usually is not - the system DLL of the same
   name lives in `C:\Windows\System32\` and the game does not need
   a local copy. But if you have one, save it as `wmvcore.dll.bak`
   first.

4. Drop `wmvcore.dll` (this project's build) into the same folder as
   `Sniper_x86.exe`.

5. Run the game. The Techland logo, the opening cinematic, and the
   main-menu background video should all play in colour, right side
   up.

That is the entire install. The system `wmvcore.dll` in
`C:\Windows\System32\` is **not** modified. The game's own files are
**not** modified. The fix is local to the game folder.

## Uninstall

Delete `wmvcore.dll` from the game folder. The game will go back to
using the system `wmvcore.dll` and the videos will go back to playing
greyscale-upside-down.

## Verify

The simplest verification is visual: launch the game and watch the
intro cinematic. If you see colour, you're done.

For a more rigorous check, build the diagnostic variant:

```cmd
set WMVPROXY_EXTRA_DEFS=/D WMVPROXY_VERBOSE=1
build.bat
```

Drop the resulting `wmvcore.dll` into the game folder, run the game
to the main menu, exit, and look for `wmvproxy.log` next to the DLL.
A successful run shows lines like:

```
[WMVProxy] engine_x86.dll base=00DB0000 - installing hooks
[WMVProxy] CDecoderBuffer ctor trampoline @ 18910000
[WMVProxy] CDecoderBuffer vtable hooked @ 011626EC
[WMVProxy] D3DX IAT hook installed: slot=011365CC ...
[WMVProxy] Open: <video file path>
[WMVProxy] SetOutputProps output=0 fcc='I420' 1280x720
[WMVProxy] D3DX #1 srcMem=... srcFmt=50(L8) srcPitch=1280 ... match=1
```

The key signal is `srcFmt=50(L8) srcPitch=1280 srcRect=(0,0,1280,720) match=1`
- that confirms the engine is calling `D3DXLoadSurfaceFromMemory`
exactly the way the bug analysis predicted, and our hook is
substituting our BGRA buffer on every match.

## Troubleshooting

**The game crashes on startup.**
You probably have a 64-bit `wmvcore.dll` in the game folder. This
proxy is x86 (32-bit) - the game itself is 32-bit. If you built it
yourself, make sure you ran `build.bat` from an **x86** Native Tools
Command Prompt, not an x64 one.

**The intro plays in colour but the menu video is still green.**
You probably have another mod (the YUV mod pak experiment, or
similar) that intercepts the same render path and conflicts with
this DLL. Remove the other mod first, then try again. This DLL is
designed to be the only fix you need - do not stack it with other
WMV-related fixes for the same game.

**The intro is in colour but upside down.**
Means the `srcRect` substitution isn't being applied. Build the
verbose variant and check `wmvproxy.log` for any line containing
`match=0` on a 1280x720 L8 call - that would indicate the IAT hook
isn't matching. File an issue with the log attached.

**Cinematics work but the main menu video is missing entirely.**
Means the engine is taking a YUV-shader code path that bypasses
`D3DXLoadSurfaceFromMemory` entirely (because some other mod
overrode the menu material). Same fix as above: remove the other
mod.

**Everything else.** Open an issue at
<https://github.com/reximus-bbs/sgw-video-fix/issues> with the
contents of `wmvproxy.log` (verbose build) attached.
