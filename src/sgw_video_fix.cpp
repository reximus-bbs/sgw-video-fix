/**
 * @file   sgw_video_fix.cpp
 * @brief  Drop-in wmvcore.dll proxy that fixes the greyscale-upside-down
 *         WMV3 video bug in Sniper Ghost Warrior (Techland Chrome
 *         Engine 4, 2010) on modern Windows.
 *
 * The same fix applies to other Chrome Engine 4 titles affected by the
 * same bug (e.g. Call of Juarez: Bound in Blood) - the proxy keys off
 * the engine's import name `D3DXLoadSurfaceFromMemory`, not a hardcoded
 * RVA, so it works for any build that imports the function by name.
 *
 * ============================================================================
 * THE BUG IN ONE PARAGRAPH
 * ----------------------------------------------------------------------------
 * Chrome Engine 4 plays its WMV3 cinematics and menu background video
 * by handing each decoded frame to `D3DXLoadSurfaceFromMemory` with
 * `SrcFormat = D3DFMT_L8` (single-channel 8-bit luminance).  On
 * Windows XP / Vista / 7 the system `wmvcore.dll` returned each
 * decoded frame as packed RGB, so the engine's "L8" upload path was
 * actually carrying RGB bytes and looked correct.  On Windows 8 and
 * later the same `wmvcore.dll` returns I420 (planar YUV 4:2:0)
 * instead - the engine's L8 upload now copies the raw Y plane directly
 * into an A8R8G8B8 surface and the picture comes out greyscale and
 * upside-down (the engine flips its source rect for the RGB path).
 *
 * THE FIX IN ONE PARAGRAPH
 * ----------------------------------------------------------------------------
 * This DLL is named `wmvcore.dll` and lives next to `Sniper_x86.exe`.
 * Windows resolves the game's `wmvcore.dll` import to this DLL first.
 * It loads the real system `wmvcore.dll` and forwards every documented
 * export.  Behind the scenes it observes when WMV3 video is playing,
 * grabs each decoded I420 frame, runs a BT.601 limited-range integer
 * I420->BGRA conversion (with the vertical flip the engine's RGB path
 * expects) into a staging buffer, and patches the engine's
 * `D3DXLoadSurfaceFromMemory` IAT slot so the actual upload sees BGRA
 * with `SrcFormat = D3DFMT_A8R8G8B8` instead of L8.  Everything else
 * passes through unchanged.  The system `wmvcore.dll` is not modified;
 * the game files are not modified; uninstall = delete this DLL.
 *
 * ARCHITECTURE
 * ----------------------------------------------------------------------------
 * Five hooks, all installed at runtime, none of them in DllMain:
 *
 *   1. `IWMReader::Open` (vtable slot 3) - observe which video file
 *      the game is opening and wrap the game's callback object so we
 *      can see each decoded frame.
 *
 *   2. `IWMReader::SetOutputProps` (vtable slot 7) - capture the
 *      video output dimensions (width x height) once at open time.
 *
 *   3. `CDecoderBuffer` constructor in engine_x86.dll - a seven-byte
 *      x86 trampoline that grows the engine's allocation by 3/2 so
 *      the engine's W*H byte buffer fits all three I420 planes
 *      (Y + half-res U + half-res V).  No size fields are touched,
 *      only the malloc length argument is bumped.
 *
 *   4. `INSSBuffer::SetLength` and `::GetMaxLength` (vtable slots 5
 *      and 4) on the engine's `CDecoderBuffer` vtable - convince
 *      Windows Media Foundation that the buffer can hold the full
 *      I420 payload, otherwise WMF refuses to write the U/V planes.
 *
 *   5. `D3DXLoadSurfaceFromMemory` IAT slot in `engine_x86.dll`'s
 *      import of `d3dx9_39.dll` - substitute our pre-converted BGRA
 *      buffer for the engine's L8 upload when (and only when) the
 *      `pSrcMemory` pointer matches the currently-active decoder
 *      buffer.  Exact pointer-identity match, no heuristics, no
 *      false positives.
 *
 * Hooks 3-5 are installed lazily on the first `WMCreateReader` call,
 * not in `DllMain`, so `engine_x86.dll` is guaranteed to already be
 * loaded when we walk its PE import directory.
 *
 * @author  Rex - project direction, reverse engineering,
 *          BT.601 colour-math verification, in-game testing.
 * @author  Claude (Anthropic) - AI coding assistant, wrote the bulk
 *          of the C++ under Rex's direction.
 * @copyright  MIT (see LICENSE).  No DRM circumvention, no game asset
 *             redistribution.  This DLL only intercepts `wmvcore.dll`'s
 *             output and rewrites the engine's own per-frame upload.
 */

#define WIN32_LEAN_AND_MEAN
#define WINVER       0x0601
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <wmsdk.h>
#include <stdio.h>
#include <new>

// SSE2 is baseline on every CPU running Windows 8+ (the target
// platform for this fix).  Pulled in unconditionally so the SSE2
// conversion path can be toggled on via WMVPROXY_USE_SSE2 without a
// second include.
#include <emmintrin.h>

// =========================================================================
// Build-time verbosity and operating-mode switches
// =========================================================================

#ifndef WMVPROXY_VERBOSE
#define WMVPROXY_VERBOSE 0
#endif

// When 1 the D3DX hook forwards every call UNCHANGED but logs its
// arguments.  Used for the initial discovery run to see exactly what
// srcFmt / srcPitch / srcRect the engine passes for the video upload.
// When 0 the hook substitutes our BGRA staging buffer on match.
#ifndef WMVPROXY_D3DX_PASSIVE
#define WMVPROXY_D3DX_PASSIVE 0
#endif

// When 1 the I420->BGRA converter uses an SSE2 intrinsics path that
// processes 8 pixels per iteration via `_mm_madd_epi16` on
// [Y,V,Y,V,...] for R and [Y,U,Y,U,...] for B, plus a separate pass
// for G.  Drops the scalar converter's runtime from ~5-8 ms per
// 720p frame to well under 1 ms on any CPU with SSE2.  Output is
// bit-exact equivalent to the scalar path within the usual 8-bit
// rounding since the math uses the same 32-bit intermediate and
// the same >>8 + 128 rounding.
#ifndef WMVPROXY_USE_SSE2
#define WMVPROXY_USE_SSE2 1
#endif

// When 1 each call to the converter is timed via `QueryPerformanceCounter`
// and the running mean in microseconds is logged every 120 frames.
// Only takes effect when WMVPROXY_VERBOSE=1.
#ifndef WMVPROXY_TIME_CONVERT
#define WMVPROXY_TIME_CONVERT 0
#endif

// =========================================================================
// Engine-specific RVAs (SGW 2010 build of engine_x86.dll)
// =========================================================================

static const DWORD kEngineRva_CDecoderBufferCtor    = 0x2D0C0;
static const DWORD kEngineRva_CDecoderBufferVtable  = 0x3B26EC;

// Note: kEngineRva_D3D9DevicePtr is GONE.  The D3DX IAT hook replaces
// everything that required knowing the D3D9 device pointer.

static const GUID MEDIASUBTYPE_I420 =
    { 0x30323449, 0x0000, 0x0010,
      { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

static const DWORD kWMVIH_BMI_Offset = 48;

// =========================================================================
// File logger  (only compiled in when WMVPROXY_VERBOSE=1)
// =========================================================================
//
// Writes to `wmvproxy.log` in the same directory as the DLL itself.
// `OutputDebugStringA` mirrors every line so DebugView captures it
// without a file write.  All access is serialised through `g_logCs`
// because Windows Media Foundation can call into our hooks from the
// player worker thread while DllMain is still resolving exports on
// the main thread.

#if WMVPROXY_VERBOSE

static HANDLE           g_hLogFile   = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logCs;
static bool             g_logCsInit  = false;

static void LogOpenFile()
{
    if (!g_logCsInit)
    {
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = true;
    }
    HMODULE hSelf = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&LogOpenFile, &hSelf);
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(hSelf, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash)
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "wmvproxy.log");
    else
        strcpy_s(path, MAX_PATH, "wmvproxy.log");
    g_hLogFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                             NULL);
}

static void Log(const char* fmt, ...)
{
    char buf[768];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf_s(buf, sizeof(buf) - 2, _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) n = 0;
    OutputDebugStringA(buf);
    if (g_hLogFile == INVALID_HANDLE_VALUE) LogOpenFile();
    if (g_hLogFile == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_logCs);
    size_t cur = strlen(buf);
    if (cur + 2 < sizeof(buf))
    {
        buf[cur] = '\r';
        buf[cur + 1] = '\n';
        buf[cur + 2] = 0;
        cur += 2;
    }
    DWORD written = 0;
    SetFilePointer(g_hLogFile, 0, NULL, FILE_END);
    WriteFile(g_hLogFile, buf, (DWORD)cur, &written, NULL);
    FlushFileBuffers(g_hLogFile);
    LeaveCriticalSection(&g_logCs);
}

#else
#define Log(...) ((void)0)
#endif

// =========================================================================
// Real wmvcore.dll pass-through
// =========================================================================
//
// We name this DLL `wmvcore.dll` and place it next to the game .exe so
// the Windows loader resolves the game's wmvcore imports here first.
// On `DLL_PROCESS_ATTACH` we `LoadLibrary("%SystemRoot%\System32\
// wmvcore.dll")` and resolve every exported function we forward.
// Each `My_*` thunk in the .def-named export table is a one-liner
// that tail-calls the resolved system function pointer with the
// arguments unchanged.  `WMCreateReader` is the only export we
// actually do anything with - we install our hooks after the system
// call returns the IWMReader instance.

static HMODULE g_hReal = NULL;
static FARPROC g_pfnWMCreateReader                        = NULL;
static FARPROC g_pfnWMCreateSyncReader                    = NULL;
static FARPROC g_pfnWMCreateReaderPriv                    = NULL;
static FARPROC g_pfnWMCreateSyncReaderPriv                = NULL;
static FARPROC g_pfnWMCreateEditor                        = NULL;
static FARPROC g_pfnWMCreateWriter                        = NULL;
static FARPROC g_pfnWMCreateWriterFileSink                = NULL;
static FARPROC g_pfnWMCreateWriterNetworkSink             = NULL;
static FARPROC g_pfnWMCreateWriterPriv                    = NULL;
static FARPROC g_pfnWMCreateWriterPushSink                = NULL;
static FARPROC g_pfnWMCreateIndexer                       = NULL;
static FARPROC g_pfnWMCreateProfileManager                = NULL;
static FARPROC g_pfnWMCreateBackupRestorer                = NULL;
static FARPROC g_pfnWMCreateBackupRestorerPrivate         = NULL;
static FARPROC g_pfnWMCreateLicenseRevocationAgentPrivate = NULL;
static FARPROC g_pfnWMIsAvailableOffline                  = NULL;
static FARPROC g_pfnWMIsContentProtected                  = NULL;
static FARPROC g_pfnWMValidateData                        = NULL;
static FARPROC g_pfnWMCheckURLExtension                   = NULL;
static FARPROC g_pfnWMCheckURLScheme                      = NULL;
static FARPROC g_pfnDllRegisterServer                     = NULL;

static void LoadReal()
{
    if (g_hReal) return;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, "\\wmvcore.dll");
    g_hReal = LoadLibraryA(path);
    if (!g_hReal)
    {
        Log("[WMVProxy] FATAL: cannot load system wmvcore.dll");
        return;
    }
#define LOAD(n) g_pfn##n = GetProcAddress(g_hReal, #n)
    LOAD(WMCreateReader);          LOAD(WMCreateSyncReader);
    LOAD(WMCreateReaderPriv);      LOAD(WMCreateSyncReaderPriv);
    LOAD(WMCreateEditor);          LOAD(WMCreateWriter);
    LOAD(WMCreateWriterFileSink);  LOAD(WMCreateWriterNetworkSink);
    LOAD(WMCreateWriterPriv);      LOAD(WMCreateWriterPushSink);
    LOAD(WMCreateIndexer);         LOAD(WMCreateProfileManager);
    LOAD(WMCreateBackupRestorer);  LOAD(WMCreateBackupRestorerPrivate);
    LOAD(WMCreateLicenseRevocationAgentPrivate);
    LOAD(WMIsAvailableOffline);    LOAD(WMIsContentProtected);
    LOAD(WMValidateData);          LOAD(WMCheckURLExtension);
    LOAD(WMCheckURLScheme);        LOAD(DllRegisterServer);
#undef LOAD
}

// =========================================================================
// I420 -> BGRA BT.601 limited-range converter
// =========================================================================
//
// Why BT.601 limited-range?  WMV3 video produced for SD/PAL/NTSC
// content (which is everything Chrome Engine 4 ships) is encoded as
// BT.601 limited-range YCbCr.  Limited-range means Y is stored in
// 16..235 and Cb/Cr in 16..240, so the inverse uses Y'=Y-16 and
// Cb'/Cr' = Cb/Cr - 128 before the matrix multiply.  Coefficients
// `298/409/100/208/516` are the integer form of the limited-range
// BT.601 inverse matrix scaled by 256:
//
//     1.164  0.000  1.596       298/256  0/256       409/256
//     1.164 -0.391 -0.813   ==  298/256 -100/256    -208/256
//     1.164  2.018  0.000       298/256  516/256     0/256
//
// (The +128 in the rounding term and the >>8 produce a single
// banker's-style round-to-nearest within one LSB of the float-point
// reference.)  This is the same matrix Chrome Engine 4's own YUV
// pixel shader uses internally - the integer form was sanity-checked
// against the engine's HLSL constants and matches to within 1 LSB.

static inline BYTE ClampByte(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

/**
 * @brief  Scalar reference: BT.601 limited-range I420 -> BGRA + vflip.
 *
 * One multiply and a couple of adds per channel per pixel.  The
 * vertical flip is built into the row-index arithmetic so the output
 * is right-side-up when handed to the engine's RGB upload path.
 *
 * The inner loop is intentionally simple - this is the reference
 * against which the SSE2 path below is compared for correctness.
 */
static void I420toBGRA_Flip_scalar(const BYTE* srcI420, BYTE* dstBGRA,
                                   DWORD vw, DWORD vh)
{
    const DWORD uvW  = vw / 2;
    const BYTE* srcY = srcI420;
    const BYTE* srcU = srcI420 + vw * vh;
    const BYTE* srcV = srcI420 + vw * vh + uvW * (vh / 2);

    for (DWORD yOut = 0; yOut < vh; yOut++)
    {
        DWORD       yIn  = vh - 1 - yOut;
        const BYTE* rowY = srcY + yIn * vw;
        const BYTE* rowU = srcU + (yIn / 2) * uvW;
        const BYTE* rowV = srcV + (yIn / 2) * uvW;
        BYTE*       dst  = dstBGRA + yOut * vw * 4;

        for (DWORD x = 0; x < vw; x++)
        {
            int C = (int)rowY[x]      - 16;
            int D = (int)rowU[x >> 1] - 128;
            int E = (int)rowV[x >> 1] - 128;

            int R = (298 * C             + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D   - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D             + 128) >> 8;

            dst[x * 4 + 0] = ClampByte(B);
            dst[x * 4 + 1] = ClampByte(G);
            dst[x * 4 + 2] = ClampByte(R);
            dst[x * 4 + 3] = 0xFF;
        }
    }
}

#if WMVPROXY_USE_SSE2

/**
 * @brief  SSE2 BT.601 limited-range I420 -> BGRA with vertical flip.
 *
 * Processes 8 pixels per inner-loop iteration.  Uses
 * `_mm_madd_epi16` - the perfect fit for a dot-product of two int16
 * lanes into one int32 lane - to fold the multiply-accumulate into a
 * single instruction per pair of coefficients.
 *
 * Precomputes the constant coefficient vectors outside the inner
 * loop.  Constants are interleaved so that a single MADD consumes
 * `[Y, V, Y, V, Y, V, Y, V]` (for the R channel's
 * `298*Y + 409*V` formula) and produces `[298*Y0+409*V0, ...]` as
 * four int32 lanes.  The B channel's `298*Y + 516*U` is handled
 * identically with `[Y, U, Y, U, ...]` interleaving.  The G channel
 * needs three terms so we do two MADDs and add them:
 *
 *     madd1 = [298*Y0 - 100*U0, 298*Y1 - 100*U1, ...]
 *     madd2 = [       - 208*V0,        - 208*V1, ...]
 *     G32   = madd1 + madd2
 *
 * Output is packed via `_mm_packus_epi16` with saturation, then the
 * B/G/R/A lanes are interleaved with unpack instructions to match
 * the engine's A8R8G8B8 target (which is B,G,R,A in memory).
 *
 * Assumes `vw` is a multiple of 8 (true for every video SGW ships -
 * 1280 = 160*8 and 1024 = 128*8).  Falls back to the scalar path if
 * the assumption fails.
 */
static void I420toBGRA_Flip_sse2(const BYTE* srcI420, BYTE* dstBGRA,
                                 DWORD vw, DWORD vh)
{
    if ((vw & 7) || vw < 8)
    {
        I420toBGRA_Flip_scalar(srcI420, dstBGRA, vw, vh);
        return;
    }

    const DWORD uvW  = vw / 2;
    const BYTE* srcY = srcI420;
    const BYTE* srcU = srcI420 + vw * vh;
    const BYTE* srcV = srcI420 + vw * vh + uvW * (vh / 2);

    // Constant lanes.  Every coefficient is interleaved with its
    // partner so a single _mm_madd_epi16 produces the right dot.
    //
    // For R:  [Y, V, Y, V, Y, V, Y, V] . [298, 409, 298, 409, ...]
    //         -> [298*Y0+409*V0, 298*Y1+409*V1, 298*Y2+409*V2, 298*Y3+409*V3]
    //
    // For B:  [Y, U, Y, U, Y, U, Y, U] . [298, 516, 298, 516, ...]
    // For G:  two MADDs:
    //         madd1: [Y, U, ...] . [298, -100, 298, -100, ...]
    //         madd2: [V, _, ...] . [-208, 0, -208, 0, ...]   (zero-V below)
    const __m128i cR = _mm_set_epi16( 409, 298, 409, 298, 409, 298, 409, 298);
    const __m128i cB = _mm_set_epi16( 516, 298, 516, 298, 516, 298, 516, 298);
    const __m128i cG1 = _mm_set_epi16(-100, 298, -100, 298,
                                      -100, 298, -100, 298);
    const __m128i cG2 = _mm_set_epi16(   0,-208,    0,-208,
                                         0,-208,    0,-208);

    const __m128i off16  = _mm_set1_epi16(16);
    const __m128i off128 = _mm_set1_epi16(128);
    const __m128i round  = _mm_set1_epi32(128);
    const __m128i alpha  = _mm_set1_epi16((short)0xFF);

    for (DWORD yOut = 0; yOut < vh; yOut++)
    {
        DWORD       yIn  = vh - 1 - yOut;
        const BYTE* rowY = srcY + yIn * vw;
        const BYTE* rowU = srcU + (yIn / 2) * uvW;
        const BYTE* rowV = srcV + (yIn / 2) * uvW;
        BYTE*       dst  = dstBGRA + yOut * vw * 4;

        for (DWORD x = 0; x < vw; x += 8)
        {
            // Load 8 Y bytes.
            __m128i Y8 = _mm_loadl_epi64((const __m128i*)(rowY + x));
            // Zero-extend to 8 int16 and subtract offset.
            __m128i Y  = _mm_unpacklo_epi8(Y8, _mm_setzero_si128());
            Y = _mm_sub_epi16(Y, off16);

            // Load 4 U and 4 V bytes.  Chroma is half-res, each
            // value is reused for two adjacent output pixels.
            int uRaw = *(const int*)(rowU + (x >> 1));
            int vRaw = *(const int*)(rowV + (x >> 1));
            __m128i U4 = _mm_cvtsi32_si128(uRaw);
            __m128i V4 = _mm_cvtsi32_si128(vRaw);

            // Duplicate each byte: u0 u0 u1 u1 u2 u2 u3 u3 ->
            // then zero-extend to int16.
            __m128i Udup = _mm_unpacklo_epi8(U4, U4);
            __m128i Vdup = _mm_unpacklo_epi8(V4, V4);
            __m128i U    = _mm_unpacklo_epi8(Udup, _mm_setzero_si128());
            __m128i V    = _mm_unpacklo_epi8(Vdup, _mm_setzero_si128());
            U = _mm_sub_epi16(U, off128);
            V = _mm_sub_epi16(V, off128);

            // Interleave for MADD: [Y,V,Y,V,Y,V,Y,V], [Y,U,Y,U,...]
            __m128i YV = _mm_unpacklo_epi16(Y, V);
            __m128i YU = _mm_unpacklo_epi16(Y, U);
            __m128i YV_hi = _mm_unpackhi_epi16(Y, V);
            __m128i YU_hi = _mm_unpackhi_epi16(Y, U);

            // G needs [V, _, V, _, ...] for the -208*V half.
            __m128i V_pad = _mm_unpacklo_epi16(V, _mm_setzero_si128());
            __m128i V_pad_hi = _mm_unpackhi_epi16(V, _mm_setzero_si128());

            // R: 4 int32 lanes = 298*Y + 409*V
            __m128i R_lo = _mm_madd_epi16(YV,    cR);
            __m128i R_hi = _mm_madd_epi16(YV_hi, cR);

            // B: 4 int32 lanes = 298*Y + 516*U
            __m128i B_lo = _mm_madd_epi16(YU,    cB);
            __m128i B_hi = _mm_madd_epi16(YU_hi, cB);

            // G: 298*Y - 100*U - 208*V
            __m128i G_lo = _mm_add_epi32(
                _mm_madd_epi16(YU,    cG1),
                _mm_madd_epi16(V_pad, cG2));
            __m128i G_hi = _mm_add_epi32(
                _mm_madd_epi16(YU_hi,    cG1),
                _mm_madd_epi16(V_pad_hi, cG2));

            // Round-bias +128 and arithmetic shift right by 8.
            R_lo = _mm_srai_epi32(_mm_add_epi32(R_lo, round), 8);
            R_hi = _mm_srai_epi32(_mm_add_epi32(R_hi, round), 8);
            G_lo = _mm_srai_epi32(_mm_add_epi32(G_lo, round), 8);
            G_hi = _mm_srai_epi32(_mm_add_epi32(G_hi, round), 8);
            B_lo = _mm_srai_epi32(_mm_add_epi32(B_lo, round), 8);
            B_hi = _mm_srai_epi32(_mm_add_epi32(B_hi, round), 8);

            // Pack int32 pairs back to int16 (saturates to -32768..32767),
            // then to uint8 with saturation to 0..255.
            __m128i R16 = _mm_packs_epi32(R_lo, R_hi);
            __m128i G16 = _mm_packs_epi32(G_lo, G_hi);
            __m128i B16 = _mm_packs_epi32(B_lo, B_hi);

            __m128i R8 = _mm_packus_epi16(R16, R16);
            __m128i G8 = _mm_packus_epi16(G16, G16);
            __m128i B8 = _mm_packus_epi16(B16, B16);

            // Interleave into BGRA.  Start with BG and RA 16-bit
            // pairs, then merge.
            __m128i BG = _mm_unpacklo_epi8(B8, G8);                   // [B0,G0,B1,G1,..B7,G7]
            __m128i RA = _mm_unpacklo_epi8(R8, _mm_packus_epi16(alpha, alpha)); // [R0,255,R1,255,...]
            __m128i BGRA_lo = _mm_unpacklo_epi16(BG, RA);              // 4 pixels
            __m128i BGRA_hi = _mm_unpackhi_epi16(BG, RA);              // 4 pixels

            _mm_storeu_si128((__m128i*)(dst + x * 4),      BGRA_lo);
            _mm_storeu_si128((__m128i*)(dst + x * 4 + 16), BGRA_hi);
        }
    }
}
#endif  /* WMVPROXY_USE_SSE2 */

/**
 * @brief  Dispatching entry point - picks the SSE2 path when enabled,
 *         falls back to scalar otherwise.
 */
static void I420toBGRA_Flip(const BYTE* srcI420, BYTE* dstBGRA,
                            DWORD vw, DWORD vh)
{
#if WMVPROXY_USE_SSE2
    I420toBGRA_Flip_sse2(srcI420, dstBGRA, vw, vh);
#else
    I420toBGRA_Flip_scalar(srcI420, dstBGRA, vw, vh);
#endif
}

// =========================================================================
// Video state globals
// =========================================================================

static BYTE*  g_persistBuf     = NULL;   // BGRA staging (DLL-lifetime)
static DWORD  g_persistBufSize = 0;
static DWORD  g_videoWidth     = 0;
static DWORD  g_videoHeight    = 0;
static volatile LONG g_videoActive = 0;
static void*  g_cdbVtable      = NULL;   // captured CDecoderBuffer vtable

// Set in CCallbackProxy::OnSample to the data pointer of the current
// frame's CDecoderBuffer.  The D3DX IAT hook compares srcMem against
// this pointer to decide whether to substitute.
static volatile void* g_activeDecoderData = NULL;

static CRITICAL_SECTION g_persistCs;
static bool             g_persistCsInit = false;

static void EnsurePersistBuf(DWORD needed)
{
    if (!g_persistCsInit)
    {
        InitializeCriticalSection(&g_persistCs);
        g_persistCsInit = true;
    }
    EnterCriticalSection(&g_persistCs);
    if (!g_persistBuf || g_persistBufSize < needed)
    {
        delete[] g_persistBuf;
        g_persistBuf     = new (std::nothrow) BYTE[needed]();
        g_persistBufSize = g_persistBuf ? needed : 0;
        Log("[WMVProxy] persistBuf alloc %p size=%u",
            g_persistBuf, g_persistBufSize);
    }
    LeaveCriticalSection(&g_persistCs);
}

// =========================================================================
// CDecoderBuffer ctor trampoline
// =========================================================================
//
// `CDecoderBuffer::CDecoderBuffer(int byteSize)` lives at engine RVA
// 0x2D0C0 in the SGW build.  Its first seven instruction bytes are:
//
//     50           PUSH EAX                  ; the byteSize argument
//     C7 06 ?? ?? ?? ??   MOV [ESI], imm32   ; vtable pointer write
//     89 46 08            MOV [ESI+8], EAX   ; size = byteSize
//
// EAX holds W*H bytes for the Y plane.  We need EAX to be W*H*3/2 by
// the time the malloc inside the body of the constructor runs, so the
// buffer is big enough for Y + half-res U + half-res V.  We do NOT
// touch the `MOV [ESI+8], EAX` store - the engine MUST still see the
// original W*H value in the size field, otherwise it shifts row
// strides by 50%.
//
// Trampoline body (64 bytes from VirtualAlloc, RWX):
//
//     8B C8        MOV ECX, EAX        ; ECX = W*H
//     D1 E9        SHR ECX, 1          ; ECX = W*H/2
//     03 C8        ADD ECX, EAX        ; ECX = W*H*3/2
//     51           PUSH ECX            ; replaces the original PUSH EAX
//     C7 06 vt vt vt vt   MOV [ESI], vtable
//     E9 rel32     JMP back to ctor + 7
//
// We then replace the first 7 bytes of the original ctor with
// `E9 rel32 NOP NOP` so the engine's call to the ctor jumps into our
// trampoline.  We verify ALL seven bytes of the prologue (including
// the `MOV [ESI+8], EAX` after our patch region) before patching, so
// a future engine build with a different prologue is detected up
// front instead of producing garbage.

static BYTE* g_ctorTrampoline = NULL;

static bool PatchCDecoderBufferCtor(HMODULE hEngine)
{
    BYTE* ctor = (BYTE*)hEngine + kEngineRva_CDecoderBufferCtor;

    // Verify the full seven-byte prologue.
    if (ctor[0] != 0x50 || ctor[1] != 0xC7 || ctor[2] != 0x06
        || ctor[7] != 0x89 || ctor[8] != 0x46 || ctor[9] != 0x08)
    {
        Log("[WMVProxy] ctor patch MISS at RVA 0x%X: "
            "%02X %02X %02X .. %02X %02X %02X",
            kEngineRva_CDecoderBufferCtor,
            ctor[0], ctor[1], ctor[2],
            ctor[7], ctor[8], ctor[9]);
        return false;
    }

    const DWORD vtableImm = *(DWORD*)(ctor + 3);

    g_ctorTrampoline = (BYTE*)VirtualAlloc(NULL, 64,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_ctorTrampoline)
    {
        Log("[WMVProxy] ctor trampoline VirtualAlloc failed");
        return false;
    }

    BYTE* t = g_ctorTrampoline;
    int   i = 0;
    t[i++] = 0x8B; t[i++] = 0xC8;                   // MOV ECX, EAX
    t[i++] = 0xD1; t[i++] = 0xE9;                   // SHR ECX, 1
    t[i++] = 0x03; t[i++] = 0xC8;                   // ADD ECX, EAX
    t[i++] = 0x51;                                   // PUSH ECX
    t[i++] = 0xC7; t[i++] = 0x06;                   // MOV [ESI], imm32
    *(DWORD*)(t + i) = vtableImm; i += 4;
    t[i++] = 0xE9;                                   // JMP rel32
    DWORD returnTarget = (DWORD)(ctor + 0x07);
    DWORD fromHere     = (DWORD)(t + i + 4);
    *(DWORD*)(t + i) = returnTarget - fromHere; i += 4;

    DWORD old;
    if (!VirtualProtect(ctor, 7, PAGE_EXECUTE_READWRITE, &old))
    {
        Log("[WMVProxy] ctor patch VirtualProtect failed");
        return false;
    }
    ctor[0] = 0xE9;
    DWORD delta = (DWORD)t - (DWORD)(ctor + 5);
    *(DWORD*)(ctor + 1) = delta;
    ctor[5] = 0x90;
    ctor[6] = 0x90;
    VirtualProtect(ctor, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), ctor, 7);

    Log("[WMVProxy] CDecoderBuffer ctor trampoline @ %p", t);
    return true;
}

// =========================================================================
// CDecoderBuffer INSSBuffer vtable hooks
// =========================================================================
//
// `CDecoderBuffer` implements the standard Windows Media Foundation
// `INSSBuffer` interface.  WMF's WMV3 decoder calls
// `INSSBuffer::GetMaxLength` to find out how many bytes it is allowed
// to write into the buffer, and `INSSBuffer::SetLength` to record the
// actual number of bytes written.  In the SGW build the engine
// hard-codes both methods to return / accept only the size field at
// `[this+0x0C]`, which we cannot enlarge without breaking row-stride
// arithmetic elsewhere.  We hook the two slots so:
//
//     - GetMaxLength returns size_field * 3/2 if `this` is a video
//       decoder buffer (vtable identity check + size matches W*H)
//     - SetLength clamps the recorded length to size_field so the
//       engine reads back exactly W*H, while the WMF writer above us
//       believes it wrote 3/2 * W*H
//
// The vtable identity check is essential: `INSSBuffer` is a generic
// interface and many other buffers (audio, metadata, headers) flow
// through the same code path with totally unrelated `[+0x08]` values.
// Hooking unconditionally would corrupt audio and metadata buffers.

typedef HRESULT (__stdcall *PFN_INSSGetMaxLength)(void*, DWORD*);
typedef HRESULT (__stdcall *PFN_INSSSetLength)   (void*, DWORD);

static inline bool LooksLikeVideoBuf(void* pThis)
{
    __try
    {
        if (!pThis || !g_cdbVtable) return false;
        if (*(void**)pThis != g_cdbVtable) return false;
        DWORD sz08 = *(DWORD*)((BYTE*)pThis + 0x08);
        if (g_videoWidth && g_videoHeight
            && sz08 == g_videoWidth * g_videoHeight)
            return true;
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static HRESULT __stdcall Hook_INSSGetMaxLength(void* pThis, DWORD* pOut)
{
    if (!pOut) return E_POINTER;
    if (LooksLikeVideoBuf(pThis))
    {
        DWORD ySize = *(DWORD*)((BYTE*)pThis + 0x08);
        *pOut = ySize * 3 / 2;
        return S_OK;
    }
    *pOut = *(DWORD*)((BYTE*)pThis + 0x0C);
    return S_OK;
}

static HRESULT __stdcall Hook_INSSSetLength(void* pThis, DWORD length)
{
    if (LooksLikeVideoBuf(pThis))
        length = *(DWORD*)((BYTE*)pThis + 0x08);
    *(DWORD*)((BYTE*)pThis + 0x10) = length;
    return S_OK;
}

static bool HookCDecoderBufferVtable(HMODULE hEngine)
{
    BYTE** vtbl = (BYTE**)((BYTE*)hEngine + kEngineRva_CDecoderBufferVtable);
    DWORD old;
    if (!VirtualProtect(vtbl, sizeof(void*) * 8, PAGE_READWRITE, &old))
    {
        Log("[WMVProxy] CDecoderBuffer vtable VirtualProtect failed");
        return false;
    }
    vtbl[4] = (BYTE*)Hook_INSSSetLength;
    vtbl[5] = (BYTE*)Hook_INSSGetMaxLength;
    VirtualProtect(vtbl, sizeof(void*) * 8, old, &old);
    g_cdbVtable = vtbl;
    Log("[WMVProxy] CDecoderBuffer vtable hooked @ %p", vtbl);
    return true;
}

// =========================================================================
// D3DXLoadSurfaceFromMemory IAT hook
// =========================================================================
//
// This is the actual fix for the greyscale-upside-down bug.  We walk
// engine_x86.dll's PE import directory at runtime, find the IAT slot
// holding the address of `d3dx9_39.dll!D3DXLoadSurfaceFromMemory`,
// and overwrite that slot with a pointer to our replacement.  Walking
// the import directory by name (rather than hardcoding an RVA) means
// the same proxy works for any Chrome Engine 4 build that imports the
// function by name, including potential patches and the same engine
// in other titles.
//
// The match key between decoder output and the D3DX call is the
// `pSrcMemory` pointer.  In `CCallbackProxy::OnSample` we record the
// data pointer of the CDecoderBuffer that just received a fresh
// frame; in our D3DX hook we check if `pSrcMemory == g_activeDecoderData`.
// That is an exact pointer-identity check, not a heuristic - the
// engine cannot accidentally pass us anything else as `pSrcMemory`.
//
// The store to `g_activeDecoderData` happens AFTER the I420->BGRA
// conversion is complete, never before, so the hook can never read a
// half-written staging buffer.

#define D3DFMT_L8         50
#define D3DFMT_A8R8G8B8   21

typedef HRESULT (WINAPI *PFN_D3DXLoadSurfaceFromMemory)(
    void*        pDestSurface,
    const void*  pDestPalette,
    const RECT*  pDestRect,
    const void*  pSrcMemory,
    DWORD        SrcFormat,
    UINT         SrcPitch,
    const void*  pSrcPalette,
    const RECT*  pSrcRect,
    DWORD        Filter,
    DWORD        ColorKey);

static PFN_D3DXLoadSurfaceFromMemory g_origD3DXLoadSurfaceFromMemory = NULL;
static void**                        g_iatSlot                      = NULL;

/** @brief Decode a D3DFORMAT into a short label for the log. */
#define FCC4(a,b,c,d) ((DWORD)(a) | ((DWORD)(b)<<8) | ((DWORD)(c)<<16) | ((DWORD)(d)<<24))
static const DWORD kFCC_YV12 = FCC4('Y','V','1','2');
static const DWORD kFCC_I420 = FCC4('I','4','2','0');
static const DWORD kFCC_UYVY = FCC4('U','Y','V','Y');
static const DWORD kFCC_YUY2 = FCC4('Y','U','Y','2');
static const DWORD kFCC_NV12 = FCC4('N','V','1','2');

static const char* FmtName(DWORD f)
{
    if (f == 21) return "A8R8G8B8";
    if (f == 22) return "X8R8G8B8";
    if (f == 23) return "R5G6B5";
    if (f == 50) return "L8";
    if (f == 61) return "A8";
    if (f == 28) return "A8L8";
    if (f == 36) return "A4R4G4B4";
    if (f == 60) return "P8";
    if (f == 20) return "R8G8B8";
    if (f == kFCC_YV12) return "YV12";
    if (f == kFCC_I420) return "I420";
    if (f == kFCC_UYVY) return "UYVY";
    if (f == kFCC_YUY2) return "YUY2";
    if (f == kFCC_NV12) return "NV12";
    return "?";
}

static volatile LONG g_d3dxCallCount      = 0;
static volatile LONG g_d3dxSubstituteCount = 0;

/**
 * @brief Replacement for d3dx9_39.dll!D3DXLoadSurfaceFromMemory.
 *
 * Matches on `pSrcMemory == g_activeDecoderData`.  When it fires:
 *   - `pSrcMemory` -> `g_persistBuf`  (our pre-converted BGRA)
 *   - `SrcFormat`  -> `D3DFMT_A8R8G8B8`
 *   - `SrcPitch`   -> `W * 4`
 *   - `pSrcRect`   -> `(0, 0, W, H)` (full frame)
 *
 * Non-matching calls pass straight through unchanged.
 */
static HRESULT WINAPI My_D3DXLoadSurfaceFromMemory(
    void*        pDestSurface,
    const void*  pDestPalette,
    const RECT*  pDestRect,
    const void*  pSrcMemory,
    DWORD        SrcFormat,
    UINT         SrcPitch,
    const void*  pSrcPalette,
    const RECT*  pSrcRect,
    DWORD        Filter,
    DWORD        ColorKey)
{
    LONG n = InterlockedIncrement(&g_d3dxCallCount);

    const bool matchSrc =
        (pSrcMemory != NULL
         && pSrcMemory == g_activeDecoderData
         && g_videoWidth && g_videoHeight
         && g_persistBuf);

#if WMVPROXY_VERBOSE
    if (n <= 10 || matchSrc || (n % 500) == 0)
    {
        const RECT empty = {0};
        const RECT* sr = pSrcRect ? pSrcRect : &empty;
        const RECT* dr = pDestRect ? pDestRect : &empty;
        Log("[WMVProxy] D3DX #%ld srcMem=%p srcFmt=%lu(%s) "
            "srcPitch=%u srcRect=(%ld,%ld,%ld,%ld) "
            "dstSurf=%p dstRect=(%ld,%ld,%ld,%ld) match=%d",
            n, pSrcMemory, SrcFormat, FmtName(SrcFormat),
            SrcPitch,
            sr->left, sr->top, sr->right, sr->bottom,
            pDestSurface,
            dr->left, dr->top, dr->right, dr->bottom,
            (int)matchSrc);
    }
#endif

#if !WMVPROXY_D3DX_PASSIVE
    if (matchSrc)
    {
        RECT fullSrc = {0, 0, (LONG)g_videoWidth, (LONG)g_videoHeight};
        InterlockedIncrement(&g_d3dxSubstituteCount);
        return g_origD3DXLoadSurfaceFromMemory(
            pDestSurface,
            pDestPalette,
            pDestRect,
            g_persistBuf,
            D3DFMT_A8R8G8B8,
            g_videoWidth * 4,
            NULL,
            &fullSrc,
            Filter,
            ColorKey);
    }
#endif

    return g_origD3DXLoadSurfaceFromMemory(
        pDestSurface, pDestPalette, pDestRect,
        pSrcMemory, SrcFormat, SrcPitch,
        pSrcPalette, pSrcRect,
        Filter, ColorKey);
}

/**
 * @brief Parse engine_x86.dll's PE header and patch the IAT slot for
 *        `D3DXLoadSurfaceFromMemory`.
 *
 * Returns true on success.  Idempotent - if the slot already points
 * at our replacement we log and return true without touching anything.
 */
static bool HookD3DXLoadSurfaceFromMemoryIAT(HMODULE hEngine)
{
    BYTE* base = (BYTE*)hEngine;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        Log("[WMVProxy] D3DX IAT hook: bad DOS header");
        return false;
    }
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        Log("[WMVProxy] D3DX IAT hook: bad NT header");
        return false;
    }

    IMAGE_DATA_DIRECTORY& imp =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imp.VirtualAddress || !imp.Size)
    {
        Log("[WMVProxy] D3DX IAT hook: no import directory");
        return false;
    }

    IMAGE_IMPORT_DESCRIPTOR* desc =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + imp.VirtualAddress);

    for (; desc->Name; desc++)
    {
        const char* dllName = (const char*)(base + desc->Name);
        if (_strnicmp(dllName, "d3dx9", 5) != 0)
            continue;

        IMAGE_THUNK_DATA* oft = desc->OriginalFirstThunk
            ? (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk)
            : (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        IMAGE_THUNK_DATA* ft  = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);

        for (; oft->u1.AddressOfData; oft++, ft++)
        {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            IMAGE_IMPORT_BY_NAME* byName =
                (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (strcmp((const char*)byName->Name,
                       "D3DXLoadSurfaceFromMemory") != 0)
                continue;

            void** pIatSlot = (void**)&ft->u1.Function;

            if (*pIatSlot == (void*)My_D3DXLoadSurfaceFromMemory)
            {
                Log("[WMVProxy] D3DX IAT hook: slot %p already patched",
                    pIatSlot);
                return true;
            }

            g_origD3DXLoadSurfaceFromMemory =
                (PFN_D3DXLoadSurfaceFromMemory)(*pIatSlot);
            g_iatSlot = pIatSlot;

            DWORD old;
            if (!VirtualProtect(pIatSlot, sizeof(void*),
                                PAGE_READWRITE, &old))
            {
                Log("[WMVProxy] D3DX IAT hook: VirtualProtect failed");
                return false;
            }
            *pIatSlot = (void*)My_D3DXLoadSurfaceFromMemory;
            VirtualProtect(pIatSlot, sizeof(void*), old, &old);

            Log("[WMVProxy] D3DX IAT hook installed: "
                "slot=%p  dll=%s  orig=%p  hook=%p",
                pIatSlot, dllName,
                g_origD3DXLoadSurfaceFromMemory,
                My_D3DXLoadSurfaceFromMemory);
            return true;
        }
    }

    Log("[WMVProxy] D3DX IAT hook: D3DXLoadSurfaceFromMemory not imported");
    return false;
}

// =========================================================================
// CCallbackProxy - IWMReaderCallback wrapper
// =========================================================================
//
// The game passes its own `IWMReaderCallback` to `IWMReader::Open`.
// We wrap it: WMF sees an instance of this proxy class, calls
// `OnSample` on us with each decoded video frame, we run the BGRA
// conversion into our staging buffer, set `g_activeDecoderData` so
// the D3DX IAT hook will recognise the upload, then forward to the
// game's real callback so the engine continues its normal pipeline.
// All other callback methods (`OnStatus`, `OnTime`, etc.) are pure
// pass-through.

class CCallbackProxy : public IWMReaderCallback,
                       public IWMReaderCallbackAdvanced
{
public:
    CCallbackProxy(IWMReaderCallback* real)
        : m_pReal(real), m_pRealAdv(NULL), m_cRef(1),
          m_videoOutput(0xFFFFFFFF), m_frameCount(0)
#if WMVPROXY_VERBOSE && WMVPROXY_TIME_CONVERT
        , m_convUsTotal(0), m_convUsMax(0)
#endif
    {
        m_pReal->AddRef();
        real->QueryInterface(IID_IWMReaderCallbackAdvanced,
                             (void**)&m_pRealAdv);
    }

    ~CCallbackProxy()
    {
        if (m_pRealAdv) { m_pRealAdv->Release(); m_pRealAdv = NULL; }
        if (m_pReal)    m_pReal->Release();
    }

    void SetVideoState(DWORD output, DWORD w, DWORD h)
    {
        m_videoOutput = output;
        g_videoWidth  = w;
        g_videoHeight = h;
        EnsurePersistBuf(w * h * 4);
        InterlockedExchange(&g_videoActive, 1);
        Log("[WMVProxy] Video output=%u %ux%u", output, w, h);
    }

    // ---- IUnknown --------------------------------------------------

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown
         || riid == IID_IWMReaderCallback
         || riid == IID_IWMStatusCallback)
        {
            *ppv = static_cast<IWMReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IWMReaderCallbackAdvanced)
        {
            *ppv = static_cast<IWMReaderCallbackAdvanced*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return m_pReal->QueryInterface(riid, ppv);
    }

    STDMETHODIMP_(ULONG) AddRef()
    { return InterlockedIncrement(&m_cRef); }

    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_cRef);
        if (!r) delete this;
        return (ULONG)r;
    }

    // ---- IWMStatusCallback ----------------------------------------

    STDMETHODIMP OnStatus(WMT_STATUS Status, HRESULT hr,
                          WMT_ATTR_DATATYPE dwType,
                          BYTE* pValue, void* pvContext)
    {
        Log("[WMVProxy] OnStatus %d hr=0x%08X", (int)Status, hr);
        if (Status == 12 /* WMT_EOF */ || Status == 13 /* WMT_CLOSED */)
        {
            InterlockedExchange(&g_videoActive, 0);
            InterlockedExchangePointer((void**)&g_activeDecoderData, NULL);
        }
        return m_pReal->OnStatus(Status, hr, dwType, pValue, pvContext);
    }

    // ---- IWMReaderCallback::OnSample ------------------------------

    STDMETHODIMP OnSample(DWORD dwOutputNum, QWORD cnsSampleTime,
                          QWORD cnsSampleDuration, DWORD dwFlags,
                          INSSBuffer* pSample, void* pvContext)
    {
        m_frameCount++;
        const bool isVideo = (dwOutputNum == m_videoOutput
                              && g_videoWidth && g_videoHeight);

        if (isVideo && pSample)
        {
            BYTE* pData = NULL;
            __try { pSample->GetBuffer(&pData); }
            __except(EXCEPTION_EXECUTE_HANDLER) {}

            if (pData && g_persistBuf)
            {
                __try
                {
#if WMVPROXY_VERBOSE && WMVPROXY_TIME_CONVERT
                    LARGE_INTEGER t0, t1, freq;
                    QueryPerformanceFrequency(&freq);
                    QueryPerformanceCounter(&t0);
#endif
                    I420toBGRA_Flip(pData, g_persistBuf,
                                    g_videoWidth, g_videoHeight);
#if WMVPROXY_VERBOSE && WMVPROXY_TIME_CONVERT
                    QueryPerformanceCounter(&t1);
                    LONGLONG usec = (t1.QuadPart - t0.QuadPart)
                                  * 1000000 / freq.QuadPart;
                    m_convUsTotal += usec;
                    m_convUsMax    = (usec > m_convUsMax) ? usec : m_convUsMax;
                    if ((m_frameCount % 120) == 0 && m_frameCount > 0)
                    {
                        Log("[WMVProxy] convert us: mean=%lld max=%lld "
                            "(last 120 frames) %s",
                            m_convUsTotal / 120, m_convUsMax,
                            WMVPROXY_USE_SSE2 ? "SSE2" : "scalar");
                        m_convUsTotal = 0;
                        m_convUsMax   = 0;
                    }
#endif
                }
                __except(EXCEPTION_EXECUTE_HANDLER)
                {
                    Log("[WMVProxy] BGRA conv AV frame=%d",
                        m_frameCount);
                }
            }

            // Publish the identity pointer the D3DX hook will match
            // against.  Set AFTER the conversion is complete so the
            // hook can't accidentally fire on a half-written buffer.
            InterlockedExchangePointer((void**)&g_activeDecoderData, pData);

            if (m_frameCount <= 5 || (m_frameCount % 240) == 0)
                Log("[WMVProxy] frame=%d pData=%p d3dxCalls=%ld subs=%ld",
                    m_frameCount, pData,
                    g_d3dxCallCount, g_d3dxSubstituteCount);
        }

        HRESULT hr = m_pReal->OnSample(dwOutputNum, cnsSampleTime,
                                       cnsSampleDuration, dwFlags,
                                       pSample, pvContext);
        if (FAILED(hr) && isVideo)
            Log("[WMVProxy] game OnSample hr=0x%08X frame=%d",
                hr, m_frameCount);
        return hr;
    }

    // ---- IWMReaderCallbackAdvanced forwarders ---------------------

    STDMETHODIMP AllocateForStream(WORD w, DWORD cb,
                                   INSSBuffer** pp, void* ctx)
    {
        return m_pRealAdv
            ? m_pRealAdv->AllocateForStream(w, cb, pp, ctx)
            : E_NOTIMPL;
    }

    STDMETHODIMP AllocateForOutput(DWORD out, DWORD cb,
                                   INSSBuffer** pp, void* ctx)
    {
        return m_pRealAdv
            ? m_pRealAdv->AllocateForOutput(out, cb, pp, ctx)
            : E_NOTIMPL;
    }

    STDMETHODIMP OnStreamSample(WORD w, QWORD t, QWORD d, DWORD f,
                                INSSBuffer* s, void* ctx)
    {
        return m_pRealAdv
            ? m_pRealAdv->OnStreamSample(w, t, d, f, s, ctx)
            : S_OK;
    }

    STDMETHODIMP OnTime(QWORD t, void* ctx)
    {
        return m_pRealAdv ? m_pRealAdv->OnTime(t, ctx) : S_OK;
    }

    STDMETHODIMP OnStreamSelection(WORD cnt, WORD* nums,
                                   WMT_STREAM_SELECTION* sels, void* ctx)
    {
        return m_pRealAdv
            ? m_pRealAdv->OnStreamSelection(cnt, nums, sels, ctx)
            : S_OK;
    }

    STDMETHODIMP OnOutputPropsChanged(DWORD out, WM_MEDIA_TYPE* mt, void* ctx)
    {
        return m_pRealAdv
            ? m_pRealAdv->OnOutputPropsChanged(out, mt, ctx)
            : S_OK;
    }

private:
    IWMReaderCallback*         m_pReal;
    IWMReaderCallbackAdvanced* m_pRealAdv;
    LONG                       m_cRef;
    DWORD                      m_videoOutput;
    int                        m_frameCount;
#if WMVPROXY_VERBOSE && WMVPROXY_TIME_CONVERT
    LONGLONG                   m_convUsTotal;
    LONGLONG                   m_convUsMax;
#endif
};

// =========================================================================
// IWMReader Open / SetOutputProps hooks
// =========================================================================
//
// We hook these two methods on the IWMReader instance returned by the
// real `WMCreateReader`, by patching the vtable pointer at offsets 3
// and 7.  `Open` lets us swap the callback object the game passed in
// for our `CCallbackProxy` wrapper.  `SetOutputProps` is the only
// reliable place to read the video output dimensions before the
// first sample arrives - we capture them into `g_videoWidth` /
// `g_videoHeight` so the converter and the D3DX hook know the
// frame size.

#define VTBL_Open           3
#define VTBL_SetOutputProps 7

typedef HRESULT (WINAPI *PFN_Open)(
    void*, const WCHAR*, IWMReaderCallback*, void*);
typedef HRESULT (WINAPI *PFN_SetOutputProps)(
    void*, DWORD, IWMOutputMediaProps*);

static PFN_Open           g_origOpen           = NULL;
static PFN_SetOutputProps g_origSetOutputProps = NULL;
static CCallbackProxy*    g_activeProxy        = NULL;

static HRESULT WINAPI Hook_SetOutputProps(void*                pThis,
                                          DWORD                dwOutputNum,
                                          IWMOutputMediaProps* pOutput)
{
    if (!g_activeProxy || !pOutput)
        return g_origSetOutputProps(pThis, dwOutputNum, pOutput);

    DWORD cb = 0;
    pOutput->GetMediaType(NULL, &cb);
    if (cb)
    {
        WM_MEDIA_TYPE* pmt = (WM_MEDIA_TYPE*)new BYTE[cb]();
        if (pOutput->GetMediaType(pmt, &cb) == S_OK
            && pmt->majortype == WMMEDIATYPE_Video
            && pmt->pbFormat
            && pmt->cbFormat >= kWMVIH_BMI_Offset + 12)
        {
            LONG* bmi = (LONG*)(pmt->pbFormat + kWMVIH_BMI_Offset);
            DWORD w = (DWORD)abs(bmi[1]);
            DWORD h = (DWORD)abs(bmi[2]);
            DWORD fcc = pmt->subtype.Data1;
            Log("[WMVProxy] SetOutputProps output=%u fcc='%c%c%c%c' %ux%u",
                dwOutputNum,
                (char)(fcc & 0xff),         (char)((fcc >> 8)  & 0xff),
                (char)((fcc >> 16) & 0xff), (char)((fcc >> 24) & 0xff),
                w, h);
            if (w && h)
                g_activeProxy->SetVideoState(dwOutputNum, w, h);
        }
        delete[] (BYTE*)pmt;
    }
    return g_origSetOutputProps(pThis, dwOutputNum, pOutput);
}

static HRESULT WINAPI Hook_Open(void*              pThis,
                                const WCHAR*       url,
                                IWMReaderCallback* pCB,
                                void*              ctx)
{
    Log("[WMVProxy] Open: %S", url);
    if (g_activeProxy) { g_activeProxy->Release(); g_activeProxy = NULL; }
    CCallbackProxy* pProxy = new CCallbackProxy(pCB);
    g_activeProxy = pProxy;
    g_activeProxy->AddRef();
    HRESULT hr = g_origOpen(pThis, url, pProxy, ctx);
    pProxy->Release();
    return hr;
}

static void PatchIWMReaderVtable(IWMReader* pReader)
{
    void** vtbl = *(void***)pReader;
    if (vtbl[VTBL_Open] == (void*)Hook_Open) return;
    g_origOpen           = (PFN_Open)          vtbl[VTBL_Open];
    g_origSetOutputProps = (PFN_SetOutputProps)vtbl[VTBL_SetOutputProps];
    DWORD old;
    VirtualProtect(&vtbl[VTBL_Open], sizeof(void*) * 10,
                   PAGE_READWRITE, &old);
    vtbl[VTBL_Open]           = (void*)Hook_Open;
    vtbl[VTBL_SetOutputProps] = (void*)Hook_SetOutputProps;
    VirtualProtect(&vtbl[VTBL_Open], sizeof(void*) * 10, old, &old);
    Log("[WMVProxy] IWMReader vtable patched");
}

// =========================================================================
// Engine-side one-time setup
// =========================================================================

static bool InstallEngineHooks()
{
    HMODULE hEngine = GetModuleHandleA("engine_x86.dll");
    if (!hEngine)
    {
        Log("[WMVProxy] engine_x86.dll not loaded yet - hooks skipped");
        return false;
    }
    Log("[WMVProxy] engine_x86.dll base=%p - installing hooks",
        hEngine);

    static bool s_ctorOk = false;
    static bool s_vtblOk = false;
    static bool s_d3dxOk = false;

    if (!s_ctorOk) s_ctorOk = PatchCDecoderBufferCtor       (hEngine);
    if (!s_vtblOk) s_vtblOk = HookCDecoderBufferVtable      (hEngine);
    if (!s_d3dxOk) s_d3dxOk = HookD3DXLoadSurfaceFromMemoryIAT(hEngine);

    bool allOk = s_ctorOk && s_vtblOk && s_d3dxOk;
    if (!allOk)
        Log("[WMVProxy] hook install INCOMPLETE: "
            "ctor=%d vtbl=%d d3dx=%d", (int)s_ctorOk, (int)s_vtblOk, (int)s_d3dxOk);
    return allOk;
}

// =========================================================================
// DllMain and entry points
// =========================================================================
//
// `DllMain` does the absolute minimum allowed by loader-lock: it
// resolves the system wmvcore.dll and its exported function pointers,
// and that is all.  It does NOT walk import tables, install hooks,
// or call into any other DLL beyond the system wmvcore - those steps
// are deferred to the first `WMCreateReader` call when the loader is
// no longer holding its lock.

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        LoadReal();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (g_activeProxy) { g_activeProxy->Release(); g_activeProxy = NULL; }
        if (g_hReal)       { FreeLibrary(g_hReal);     g_hReal       = NULL; }
    }
    return TRUE;
}

HRESULT WINAPI My_WMCreateReader(IUnknown*   pCert,
                                 DWORD       rights,
                                 IWMReader** ppReader)
{
    if (!g_pfnWMCreateReader) return E_FAIL;
    typedef HRESULT (WINAPI *PFN)(IUnknown*, DWORD, IWMReader**);
    Log("[WMVProxy] WMCreateReader");
    HRESULT hr = ((PFN)g_pfnWMCreateReader)(pCert, rights, ppReader);
    if (SUCCEEDED(hr) && ppReader && *ppReader)
    {
        PatchIWMReaderVtable(*ppReader);
        static bool s_engineHooksDone = false;
        if (!s_engineHooksDone)
            s_engineHooksDone = InstallEngineHooks();
    }
    return hr;
}

// =========================================================================
// Pass-through wmvcore exports
// =========================================================================
//
// Each `My_*` thunk forwards directly to the resolved system function
// pointer.  These exports MUST be present in the export table or the
// game's import resolution will fail at startup.  The .def file aliases
// the documented wmvcore export name to each `My_*` thunk.

#define FWD1(n, t1, a1) \
    HRESULT WINAPI My_##n(t1 a1) { \
        typedef HRESULT (WINAPI *F)(t1); \
        return g_pfn##n ? ((F)g_pfn##n)(a1) : E_FAIL; }

#define FWD2(n, t1, a1, t2, a2) \
    HRESULT WINAPI My_##n(t1 a1, t2 a2) { \
        typedef HRESULT (WINAPI *F)(t1, t2); \
        return g_pfn##n ? ((F)g_pfn##n)(a1, a2) : E_FAIL; }

#define FWD3(n, t1, a1, t2, a2, t3, a3) \
    HRESULT WINAPI My_##n(t1 a1, t2 a2, t3 a3) { \
        typedef HRESULT (WINAPI *F)(t1, t2, t3); \
        return g_pfn##n ? ((F)g_pfn##n)(a1, a2, a3) : E_FAIL; }

FWD2(WMCreateSyncReader,                    IUnknown*,           pCert, DWORD,           rights)
FWD1(WMCreateReaderPriv,                    IWMReader**,         pp)
FWD1(WMCreateSyncReaderPriv,                void**,              pp)
FWD1(WMCreateEditor,                        IWMMetadataEditor**, pp)
FWD2(WMCreateWriter,                        IUnknown*,           pCert, IWMWriter**,     pp)
FWD1(WMCreateWriterFileSink,                IWMWriterFileSink**, pp)
FWD1(WMCreateWriterNetworkSink,             IWMWriterNetworkSink**, pp)
FWD1(WMCreateWriterPriv,                    IWMWriter**,         pp)
FWD1(WMCreateWriterPushSink,                IWMWriterPushSink**, pp)
FWD1(WMCreateIndexer,                       IWMIndexer**,        pp)
FWD1(WMCreateProfileManager,                IWMProfileManager**, pp)
FWD2(WMCreateBackupRestorer,                IUnknown*,           pCB,   void**,          pp)
FWD2(WMCreateBackupRestorerPrivate,         IUnknown*,           pCB,   void**,          pp)
FWD2(WMCreateLicenseRevocationAgentPrivate, IUnknown*,           pCB,   void**,          pp)
FWD3(WMIsAvailableOffline,                  const WCHAR*,        url,   const WCHAR*,    lang, BOOL*, pb)
FWD2(WMIsContentProtected,                  const WCHAR*,        fn,    BOOL*,           pb)
FWD2(WMValidateData,                        BYTE*,               data,  DWORD*,          size)
FWD1(WMCheckURLExtension,                   const WCHAR*,        url)
FWD1(WMCheckURLScheme,                      const WCHAR*,        url)

HRESULT WINAPI My_DllRegisterServer()
{
    typedef HRESULT (WINAPI *F)();
    return g_pfnDllRegisterServer ? ((F)g_pfnDllRegisterServer)() : E_FAIL;
}
