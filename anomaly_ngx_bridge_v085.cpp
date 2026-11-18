// Anomaly NGX Bridge v0.4.4: menu no-level frames retain native display dimensions, gameplay uses SR.
// Engine input targets are resized at construction; NGX produces native-resolution output.
// This build fixes the D3D11 viewport/scissor not restored in v0.3.7's final composite.
// Retains the proven shader-constant hooks. Adds an optional D3D11 vtable method hook only in test modes 1/2; no new EXE patch.
// Uses NVIDIA's official NGX SDK headers (installed separately by the user).
// Composite mode 2 proven in live testing; modes Quality/Balanced/Performance/Ultra
// are selected through the menu but require verified low-resolution scene inputs.
// Use only in a permitted offline/private game instance. Tested EXE signature required.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX // prevent Windows.h from defining min/max macros
#include <Windows.h>
#include <winver.h> // diagnostic DLL file-version reporting only
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cstring>
#include <vector>
#include <cmath>
#include <limits>
#include <TlHelp32.h>
#include <algorithm>
#include <intrin.h>
#include <wincrypt.h> // SHA-256 executable profile validation
#include "MinHook.h"
// IMPORTANT: Use NVIDIA's official nvsdk_ngx_d.lib SDK wrapper.
// Calling the private driver export Init_ProjectID with the public
// Init_with_ProjectID signature crashed in the previous build.
#define NVSDK_NGX_NO_CUDA 1
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers_d3d.h"
#include "nvsdk_ngx_helpers.h" // official optimal render-size query; capability parameters only

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib") // NVIDIA NGX SDK util-log.obj imports GetWindowThreadProcessId

namespace {
using RendererFn = void(__fastcall*)(void*);
static RendererFn g_original = nullptr;
using DecompressFn = void(__fastcall*)(void*, void*);
// X-Ray renderer's registered SSS shader constant handler for "ssfx_jitter".
using SSFXJitterFn = void(__fastcall*)(void*, void*);
static SSFXJitterFn g_originalSSFXJitter = nullptr;
static constexpr std::uintptr_t kSSFXJitterRva = 0x3636D0;
// v0.4.4 read-only engine video-restart / render-target manager probe.
// The stock console command `vid_restart` executes FUN_14009F150, which
// calls the engine device-reset vtable method. We do NOT trigger resets here.
using EngineVideoRestartFn = void(__fastcall*)();
using EngineTargetManagerFn = void*(__fastcall*)(void*,const char*,unsigned,unsigned,unsigned,unsigned,unsigned char);
static EngineVideoRestartFn g_originalEngineVideoRestart = nullptr;
static EngineTargetManagerFn g_originalTargetManager = nullptr;
static constexpr std::uintptr_t kEngineVideoRestartRva = 0x9F150;
static constexpr std::uintptr_t kEngineTargetManagerRva = 0x2B5850;
static bool g_srResetProbeEnabled = false;
// Renderer constructors can occur more than once in a running process.
// v044 can adopt a queued mode ONLY at that engine-managed construction boundary.
static bool g_srReconfigureAtConstructor = false;
static bool g_srLogConstructorStacks = false;
static volatile LONG g_srConstructorBusy = 0;
static volatile LONG64 g_srTransitionGeneration = 0;
static volatile LONG64 g_srResetCtorAtBegin = 0;
static bool g_srDeviceMismatchLogged = false;
static volatile LONG g_srRestartVtableLogged = 0;
static uintptr_t g_srLatestCompletedRenderer = 0;
static volatile LONG g_srOldRendererSkipLogged = 0;

static volatile LONG g_srResetDLAAHold = 0;
static bool g_srResetHookInstalled = false;
static bool g_srTargetManagerHookInstalled = false;
static volatile LONG g_srResetActive = 0;
static volatile LONG g_srResetWindow = 0;
static volatile LONG64 g_srVideoRestartCalls = 0;
static volatile LONG64 g_srManagerCallsAfterReset = 0;
static volatile LONG g_srManagerLoggedAfterReset = 0;

// v0.3.8: EARLY, READ-ONLY TARGET-ALLOCATION AUDIT.
// The renderer constructor FUN_140280880 invokes FUN_1402E3A50 with
// (wrapper slot, resource name, width, height, format, sample/config, flag).
// These types are confirmed by the supplied EXE's x64 call sites.
// NO texture dimensions are modified in this version. Scene/output scaling
// requires a coordinated pipeline change; reallocating only color/depth/MV
// would break multi-RT, depth, postprocessing, and shader UV contracts.
using RendererConstructorFn = void*(__fastcall*)(void* renderer);
using NamedTargetAllocationFn = void(__fastcall*)(void* slot, const char* name,
    unsigned width, unsigned height, unsigned format, unsigned config, unsigned char flag);
static RendererConstructorFn g_originalRendererConstructor = nullptr;
static NamedTargetAllocationFn g_originalNamedTargetAllocation = nullptr;
static constexpr std::uintptr_t kRendererConstructorRva = 0x280880;
static constexpr std::uintptr_t kNamedTargetAllocationRva = 0x2E3A50;
static bool g_srAllocationAuditEnabled = true;
static bool g_srConstructorHookInstalled = false;
static bool g_srAllocationHookInstalled = false;
static volatile LONG64 g_srAllocationCount = 0;
static volatile LONG64 g_srConstructorCount = 0;
static unsigned g_srAllocationLogLimit = 144;
static volatile LONG g_srAllocationLogged = 0;
static volatile LONG g_srAllocationCriticalCount = 0;
static uintptr_t g_srConstructingRenderer = 0;
static DWORD g_srConstructorThread = 0;
// v0.3.8 opt-in experimental scene/output split. The launcher does not need
// pre-D3D11 injection: v0.3.6 verified ctor interception on the later path.
using FullFrameFn = void(__fastcall*)(void*);
static FullFrameFn g_originalFullFrame = nullptr;
static constexpr std::uintptr_t kFullFrameRva = 0x2C2730;
static bool g_srScaleOptIn = true;
static bool g_srFullFrameHookInstalled = false;
static bool g_srScaleReady = false;
static bool g_srConstructorSawAll = false;
static int g_srStartupMode = 0;
static unsigned g_srStartupW = 0, g_srStartupH = 0;
static unsigned g_srDisplayStartupW = 0, g_srDisplayStartupH = 0;
static unsigned g_srCriticalMask = 0;
static volatile LONG g_srFrameInside = 0;
static volatile LONG g_srGlobalsReduced = 0;
static volatile LONG g_srFrameRestoredBeforeComposite = 0;
static LONG64 g_srFrameHooks = 0;
static LONG64 g_srCompositeRestores = 0;
static bool g_srWarnedRestartNeeded = false;
// v0.4.4: the original FUN_1402C2730 takes a direct native-resolution
// 2D rendering branch if either the active level or HUD render object is null.
// Those globals are read-only and are not guessed based on a menu texture.
// Keep both width/height globals native for that whole branch. The 3D scene
// targets remain startup-scaled and are only consumed when an active level
// enters the normal 3D path. This does not recreate textures on menu transitions.
static constexpr std::uintptr_t kActiveLevelRva = 0x183EF58;
static constexpr std::uintptr_t kHudRenderRva = 0x183EEA8;
static bool g_srNativeMenuOptIn = true;
// v079: MULTIPLAYER ONLY. Both these values are read exclusively after
// the known-MP SHA-256 branch. GAMMA never observes or uses this adapter.
static bool g_mpNativeMenusEnabled = true;
static bool g_mpMainMenuNativeFirst = true;
static volatile LONG64 g_mpBootstrapNativeCtors = 0;
static volatile LONG64 g_mpBootstrapPendingWrites = 0;
// v080: Lua dispatch receipt (diagnostic only). This is the successful
// MP pause Lua module, not an independent startup script.
static wchar_t g_mpBootstrapAckPath[MAX_PATH]{};
// v0.8.5 experiment: Windows schedules this low-priority worker on an available
// CPU. It observes two Lua marker files and can poll settings.ini, publishing
// only immutable file snapshots. NGX/D3D11, game constants, Lua and renderer
// state updates must remain on the original render thread.
static bool g_mpBackgroundMarkerRequested = false;
// v0.8.6: independently selectable file I/O offload; no game or D3D access.
static bool g_mpBackgroundSettingsRequested = false;
// v0.8.7 priority A/B experiment, multiplayer only. These values adjust
// Windows THREAD priorities, not hook-call priority or process priority.
// Worker modes: 0=below-normal (v0.85/86), 1=normal, 2=above-normal,
// 3=highest. Render modes: 0=do not alter game render thread (default),
// 1=above-normal, 2=highest. Never use TIME_CRITICAL or a real-time class.
static int g_mpWorkerPriorityMode = 0;
static int g_mpRenderPriorityMode = 0;
static volatile LONG g_mpRenderPriorityAttempted = 0;
static volatile LONG g_mpBackgroundMarkerRunning = 0;
static volatile LONG g_mpBackgroundMarkerStop = 0;
static volatile LONG g_mpBackgroundPauseDetected = 0;
static volatile LONG g_mpBackgroundBootstrapDetected = 0;
// v0.8.8, multiplayer-only, OPT-IN observation. No scheduling or rendering
// decisions are changed. Worker takes 1 Hz OS snapshots; renderer only
// publishes atomics and a bounded, sampled QPC timing record.
static bool g_v088DiagnosticsEnabled = false;
static unsigned g_v088IntervalMs = 1000;
static unsigned g_v088DurationSeconds = 180;
static unsigned g_v088SampleEvery = 120;
static wchar_t g_v088LogPath[MAX_PATH]{};
static volatile LONG g_v088FirstRenderTick = 0;
static volatile LONG g_v088RendererThread = 0;
static volatile LONG g_v088OtherThread = 0;
static volatile LONG64 g_v088OtherThreadCalls = 0;
static volatile LONG64 g_v088SampleCount = 0;
static volatile LONG64 g_v088HookTicks = 0;
static volatile LONG64 g_v088SettingsTicks = 0;
static volatile LONG64 g_v088NgxTicks = 0;
static volatile LONG64 g_v088CompositeTicks = 0;
static volatile LONG64 g_v088RenderCalls = 0;
static volatile LONG64 g_v088EvaluationCount = 0;
static volatile LONG g_v088RenderProcessor = -1;
static volatile LONG g_v088State = -999;
static volatile LONG g_v088Flags = 0;
static volatile LONG g_v088HandoffStreak = 0;
static volatile LONG g_v088AllocatedMode = -1;
static volatile LONG g_v088ActiveMode = -1;
static volatile LONG g_v088Preset = -1;
static volatile LONG g_v088AppliedPriorityOld = -100;
static volatile LONG g_v088AppliedPriorityNew = -100;
static volatile LONG g_v088AppliedPrioritySuccess = -1;
static DWORD g_v088DllAttachTick = 0;
// v0.8.9: diagnostic-only game-process thread census, done exclusively on
// the existing I/O worker. Never change priorities, affinities or game state.
static bool g_v089ThreadCensusEnabled = false;
static unsigned g_v089ThreadCensusIntervalMs = 2000;
// v0.90 targeted, opt-in hot-API census. Only sampled VirtualQuery calls
// are timed; no Windows API call, disk write or atomic op on normal calls.
// All WritableRange checks remain exactly as before (no caching of page state).
static bool g_v090VirtualQueryProbe = false;
// v0.91: selectable, multiplayer-only shader-write validation ordering.
// 0 = original two checks before reading. 1 = defer only DIRTY-flag check
// until the constant matches (buffer is ALWAYS checked before reading).
// 2 = mode 1 plus single-call reuse of a VirtualQuery region when it covers
// both the constant and dirty flag. No lifetime/page/frame cache exists.
// v0.91.1 EXPERIMENT ONLY: mode 3 skips per-stage VirtualQuery entirely,
// but retains guarded reads, constant-value matching and SEH write protection.
// A first fault permanently switches subsequent stages to checked mode 2.
static int g_v091ValidationMode = 3;
static volatile LONG g_v0911CheckedFallback = 0;
static volatile LONG64 g_v0911DirectWritePairs = 0;
static volatile LONG64 g_v0911ReadFaults = 0;
static volatile LONG64 g_v0911WriteFaults = 0;
static volatile LONG64 g_v091Candidates = 0;
static volatile LONG64 g_v091DecompressRejected = 0;
static volatile LONG64 g_v091SsfxRejected = 0;
static volatile LONG64 g_v091ReadRejected = 0;
static volatile LONG64 g_v091Matched = 0;
static volatile LONG64 g_v091SavedByDefer = 0;
static volatile LONG64 g_v091SavedByRegion = 0;

static volatile LONG64 g_v090VirtualQueriesBatched = 0;
static volatile LONG64 g_v090VirtualQuerySamples = 0;
static volatile LONG64 g_v090VirtualQueryTicks = 0;
static volatile LONG64 g_v090WritablePassSamples = 0;
static volatile LONG64 g_v090WritableFailSamples = 0;
static volatile LONG64 g_v090DecompressOriginalSamples = 0;
static volatile LONG64 g_v090DecompressOriginalTicks = 0;
static volatile LONG64 g_v090SsfxOriginalSamples = 0;
static volatile LONG64 g_v090SsfxOriginalTicks = 0;



static volatile LONG64 g_mpBootstrapAckEvents = 0;
static volatile LONG g_srMenuFrameInside = 0;
static bool g_srWasMenu = false;
static bool g_srMenuStateInitialized = false;
static unsigned long long g_srMenuFrames = 0;
static unsigned long long g_srMenuNgxSkipped = 0;
// v0.3.8: v0.3.7 changed global render dimensions back to native before the
// composite, but D3D11 retained the 3D 640x360 RS viewport. The full-res NGX
// output was therefore drawn into only the upper-left of the 1920x1080 target.
// v0.4.4 opt-in, read-only render-pass inspector. No new texture allocations,
// draw-call changes, or resource swaps. An external marker triggers a bounded
// snapshot of OM render target and RS viewport transitions while a menu is open.
// v0.4.4 experimental native-sized menu targets. The v043 trace identified
// both $user$generic0 and $user$generic1 as menu-pass outputs on the healthy
// DLAA path. Preserve ONLY their initial allocations; NEVER swap live COM
// resource pointers (the unsafe v040 target bank has been removed).
static bool g_srNativeGeneric01OptIn = false;
static bool g_srNativeGeneric0 = true;
static bool g_srNativeGeneric1 = true;
// v0.4.5: opt-in raster-state correction for native-sized UI render targets.
// Only the rasterizer viewport/scissor is changed; never replace pointers or
// change SR scene dimensions. Runs on the validated immediate render thread.
static bool g_srUiNativeRasterEnabled = true;
static bool g_srUiRasterGeneric01 = true;
static bool g_srUiRasterOutside = true;
static bool g_srUiRasterInsideOther = true; // experimental: native RT only, generic0/1 stay scaled
static bool g_srUiRasterNamedInsideOnly = false; // 1 = UI / viewport2 / scopeRT allowlist
static bool g_srUiRasterScissor = true;
static thread_local bool g_srUiRasterReentry = false;
static unsigned long long g_srUiRasterCorrections = 0;
static unsigned long long g_srUiRasterSkips = 0;
static bool g_uiPassTraceEnabled = false;
static bool g_uiPassHooksAttempted = false;
static bool g_uiPassHooksReady = false;
static volatile LONG g_uiPassRenderBudget = 0;
static volatile LONG g_uiPassViewportBudget = 0;
static volatile LONG g_uiPassDrawBudget = 0;
static volatile LONG g_uiPassRasterBudget = 0;
static bool g_uiPassDrawHooksReady = false;
// v053: manual, bounded-impact probe for the one confirmed pause/menu sprite
// rendering site. Never replace RTs, modify the generic target allocations,
// or change the raster state of gameplay/postprocessing draw calls.
static bool g_srPauseUiDrawProbeEnabled = false;
static volatile LONG g_srPauseUiDrawArmed = 0; // manual on/off; initially always off
static unsigned long long g_srPauseUiDrawApplied = 0;
static unsigned long long g_srPauseUiDrawMissed = 0;
// v053: manual A/B test of NGX-evaluation + final-composite handoff bypass.
// Does not resize/reallocate targets; the pause UI may remain low resolution.
// Explicitly armed by a marker file while a level is active. No keyboard hooks.
static bool g_srMenuHandoffBypassEnabled = false;
static volatile LONG g_srMenuHandoffBypassArmed = 0;
static bool g_srMenuHandoffBypassWasActive = false; // render thread only
static unsigned long long g_srMenuHandoffBypassFrames = 0;
static unsigned long long g_srMenuHandoffBypassTransitions = 0;
// v053: read-only test to distinguish a consumed marker from a bypassed render.
// FullFrame can keep ticking while RendererHook is absent during the pause menu.
// Counters cover only the initialized D3D11 immediate context. No UI resize.
// v0.5.1: passive pause-state fingerprint collection. This does NOT switch
// DLSS modes, write the user's settings file, or execute vid_restart.
static bool g_pauseTransitionDiagEnabled = false;
// v0.5.3: optional, in-memory pause DLAA override. Lua drives explicit
// main_menu_on_init / main_menu_on_quit; D3D rendering only confirms opening.
// No changes to the saved settings.ini. Lua alone dispatches vid_restart.
static bool g_pauseAutoEnabled = false;
static bool g_pauseAutoActive = false;
static wchar_t g_pauseAutoMarker[MAX_PATH+72]{};
static wchar_t g_pauseAutoAck[MAX_PATH+72]{};
static char g_pauseAutoPending[96]{};
static DWORD g_pauseAutoPendingTick = 0;
static unsigned g_pauseAutoNoLevelFrames = 0;

static wchar_t g_pauseDiagMarkerPath[MAX_PATH + 72]{};
static wchar_t g_pauseDiagStage[48]{L"unmarked"};
struct PauseDiagFrame {
    unsigned draw=0, indexed=0, uiQuad=0, uiText=0, uiComposite=0;
    unsigned uiOm=0, uiOutputBind=0, text54=0, text84=0, textLarge=0;
    unsigned scissorNonzero=0, scissorZero=0, sceneViewport=0, nativeViewport=0;
    unsigned sampleBudget=2;
};
struct PauseDiagWindow {
    unsigned frames=0, rendererFrames=0, noRendererFrames=0, levelFrames=0;
    unsigned long long rendererCalls=0, draw=0, indexed=0, uiQuad=0, uiText=0;
    unsigned long long uiComposite=0, uiOm=0, uiOutputBind=0;
    unsigned long long text54=0, text84=0, textLarge=0;
    unsigned long long scissorNonzero=0, scissorZero=0;
    unsigned long long sceneViewport=0, nativeViewport=0;
};
static PauseDiagFrame g_pauseDiagFrame{};
static PauseDiagWindow g_pauseDiagWindow{};
static bool g_pauseDiagHaveFrame=false;
static LONG64 g_pauseDiagLastRendererCalls=0;
static unsigned long long g_pauseDiagFullFrames=0;
static unsigned g_pauseDiagCandidateStreak=0;
static unsigned g_pauseDiagResumeStreak=0;
static bool g_pauseDiagCandidate=false;
static unsigned long long g_pauseDiagWindowCount=0;
static bool g_srMenuActivityDiagEnabled = false;
static volatile LONG64 g_srDiagFullFrames = 0;
static volatile LONG64 g_srDiagOmBinds = 0;
static volatile LONG64 g_srDiagDraw = 0;
static volatile LONG64 g_srDiagDrawIndexed = 0;
static volatile LONG64 g_srDiagSpriteDraw = 0;
static volatile LONG64 g_srDiagSpriteIndexed = 0;
static volatile LONG g_srDiagSpriteTraceCount = 0;
struct SrMenuActivitySnapshot {
    LONG64 fullFrames=0, renderer=0, om=0, draw=0, indexed=0;
    LONG64 spriteDraw=0, spriteIndexed=0;
};
static SrMenuActivitySnapshot g_srDiagAtOn{};
static bool g_srDiagIntervalOpen = false;


static unsigned long long g_uiPassMarkerPoll = 0;
static unsigned long long g_uiPassOMProbePoll = 0;
static wchar_t g_uiPassMarkerPath[MAX_PATH + 72]{};
static wchar_t g_uiPassLabel[64]{L"unmarked"};
static ID3D11Texture2D* g_uiPassKnownUi = nullptr; // non-owning identities only
static ID3D11Texture2D* g_uiPassKnownColor = nullptr;
static ID3D11Texture2D* g_uiPassKnownGeneric = nullptr;
static ID3D11Texture2D* g_uiPassKnownGeneric0 = nullptr;
static ID3D11Texture2D* g_uiPassKnownGeneric1 = nullptr;
static ID3D11Texture2D* g_uiPassKnownViewport = nullptr;
static ID3D11Texture2D* g_uiPassKnownScope = nullptr;
using OMSetRenderTargetsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView* const*,ID3D11DepthStencilView*);
using RSSetViewportsFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,const D3D11_VIEWPORT*);
static OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
static RSSetViewportsFn g_originalRSSetViewports = nullptr;
using DrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using DrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
static DrawFn g_originalDraw = nullptr;
static DrawIndexedFn g_originalDrawIndexed = nullptr;
// v053 opt-in pause-only native sidecars. These are PRIVATE D3D11 render
// targets, never injected into the engine render-target ownership table.
// Manual marker controls the probe; reconstructing or resuming gameplay
// disarms it automatically. Original generic0/1 remain 1280x720.
static bool g_srPauseSidecarEnabled = false;
static volatile LONG g_srPauseSidecarArmed = 0;
static volatile LONG g_srPauseSidecarNativeViewport = 1;
struct SrPauseSidecarTarget {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    LONG64 touchedFrame = -1;
};
static SrPauseSidecarTarget g_srPauseSidecar[2]{};
static ID3D11Texture2D* g_srPauseSidecarSources[2]{}; // borrowed identities only
static LONG64 g_srPauseSidecarCtorGeneration = -1;
static unsigned long long g_srPauseSidecarOmSwaps = 0;
static unsigned long long g_srPauseSidecarCompositeSwaps = 0;
static unsigned long long g_srPauseSidecarMisses = 0;
static unsigned g_srPauseSidecarFailureLogs = 0;
static bool g_srNativeViewportFix = true;
static bool g_srNativeScissorFix = true;
static unsigned long long g_srViewportFixCalls = 0;
static unsigned long long g_srViewportAfterOriginalMismatches = 0;



static DecompressFn g_originalDecompress = nullptr;
static constexpr std::uintptr_t kDecompressRva = 0x21BA20;
static constexpr std::uintptr_t kWidthRva = 0x185B8B8;
static constexpr std::uintptr_t kHeightRva = 0x185B8BC;
static constexpr std::uintptr_t kFovRva = 0x185BE8C;
static constexpr std::uintptr_t kAspectRva = 0x185BE90;
static constexpr std::uintptr_t kShaderTableRvas[6] = {
    0x185FAB8, 0x185FA48, 0x185FB28, 0x185FB98, 0x185FC08, 0x185FC78
};
static HANDLE g_activationEvent = nullptr;
static HANDLE g_guardMildEvent = nullptr;
static HANDLE g_guardStrongEvent = nullptr;
static int g_initialGuardMode = 0; // 0=off, 1=mild, 2=strong
static int g_lastGuardMode = -1;
static bool g_guardInitAttempted = false;
static bool g_guardReady = false;
static ID3D11ComputeShader* g_guardShader = nullptr;
static ID3D11ShaderResourceView* g_guardColorSrv = nullptr;
static ID3D11ShaderResourceView* g_guardNgxSrv = nullptr;
static ID3D11UnorderedAccessView* g_guardUav = nullptr;
static ID3D11Buffer* g_guardCBuffer = nullptr;
static ID3D11Texture2D* g_guardTarget = nullptr;

static bool g_allowZeroJitterFallback = true;
static bool g_enableManualJitterDetour = true;
static bool g_jitterHookActive = false;
static bool g_jitterDegradedToZero = false;
static unsigned g_lastUnmatchedFrame = 0;
static unsigned g_consecutiveUnmatchedFrames = 0;
static bool EffectiveJitter() { return g_jitterHookActive && !g_jitterDegradedToZero; }
static void* g_manualJitterAlloc = nullptr;
static wchar_t g_controlName[120]{};
static bool g_autoEnable = true;
static constexpr unsigned g_haltonPhases = 8u; // v0.91.5 fixed: no in-game/INI phase switching
static int g_jitterPercent = 100; // experimental 0..125; 100 matches validated baseline
static int g_ssfxSyncMode = 1; // v0.91.4: match release menu/INI default (1=Sync Halton, 0=Off)
static bool g_ssfxSyncHookInstalled = false;
static LONG64 g_ssfxCalls = 0;
static LONG64 g_ssfxWrites = 0;
static LONG64 g_ssfxSkippedExistingJitter = 0;
static unsigned g_ssfxLastPatchedFrame = 0;
static int g_sharpnessPercent = 0; // post-NGX spatial sharpening; disabled by default
static int g_preset = 11; // release default: K; supported J=10,K=11,L=12,M=13
static wchar_t g_settingsFile[MAX_PATH + 128]{};
static FILETIME g_settingsWriteTime{};
static DWORD g_lastSettingsCheckTick = 0;
static bool g_settingsPresent = false;
// The background worker owns file reading. A nonblocking reader lock publishes a
// complete POD snapshot; the render thread remains the ONLY writer of settings,
// NGX handles, mode state, jitter and engine render-target state.
struct MpSettingsSnapshot {
    FILETIME writeTime{};
    bool enabled = true;
    int preset = 0;
    int mode = 0;
    int jitterPercent = 100;
    int sharpnessPercent = 0;
    int syncMode = 1; // v0.91.4: keep worker snapshot consistent with checked release default
};
static SRWLOCK g_mpSettingsLock = SRWLOCK_INIT;
static MpSettingsSnapshot g_mpSettingsPublished{};
static LONG g_mpSettingsPublishedGeneration = 0; // protected by SRW lock
static LONG g_mpSettingsAppliedGeneration = 0; // render thread only
static std::uintptr_t g_moduleBase = 0;
static LONG64 g_decompressCalls = 0;
static LONG64 g_decompressWrites = 0;
static unsigned g_lastPatchedFrame = 0;
static LONG64 g_skippedForJitter = 0;
static LONG64 g_patchFailures = 0;
static float g_injectedJitterX = 0.f;
static float g_injectedJitterY = 0.f;
static constexpr std::uintptr_t kRendererRva = 0x3958A0;
static constexpr std::uintptr_t kCallsiteRva = 0x325925;
static constexpr std::uintptr_t kFrameCounterRva = 0x185B8D8; // observed engine frame counter
static constexpr BYTE kExpectedCall[] = {0xE8, 0x76, 0xFF, 0x06, 0x00};
static constexpr std::uintptr_t kColorSlot = 0x438;
static constexpr std::uintptr_t kMotionSlot = 0x668;
static constexpr std::uintptr_t kDepthSlot = 0x728;
static constexpr std::uintptr_t kTextureInWrapper = 0x20;
static bool g_gammaNativeTrialActive = false; // exact GAMMA profile only
static HMODULE g_self = nullptr;
static wchar_t g_logPath[MAX_PATH + 80]{};
static wchar_t g_cacheDir[MAX_PATH + 80]{};
static wchar_t g_dllDir[MAX_PATH + 80]{};
static const wchar_t* g_featurePaths[1] = {g_dllDir};
static NVSDK_NGX_FeatureCommonInfo g_featureInfo{};
static LONG64 g_calls = 0;
static bool g_evaluationEnabled = false; // managed by external named event; never a key
static bool g_fatalEvaluationError = false; // fail closed; restart after a D3D/NGX error
static bool g_resetPending = true;
static unsigned g_runFrames = 0;
static unsigned long long g_totalEvaluations = 0;
static unsigned g_lastEngineFrame = 0;
// SR readiness stage: read-only diagnostics. Does NOT change render dimensions,
// NGX quality mode, color/depth/motion textures, shader jitter, or presentation.
static bool g_srProbeEnabled = true;
static bool g_srPreCaptured = false;
static bool g_srPostCaptured = false;
static bool g_srResourcesCaptured = false;
static unsigned g_srNativeWidth = 0;
static unsigned g_srNativeHeight = 0;
// SR modes all share the same NGX and composite code; low-res scene allocation
// remains an engine-owned operation, NOT a downsample performed by this bridge.
// 0=DLAA, 1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance.
static int g_requestedMode = 0;
static int g_activeMode = 0;
static unsigned g_activeRenderWidth = 0, g_activeRenderHeight = 0;
static unsigned g_displayWidth = 0, g_displayHeight = 0;
static unsigned g_lastPendingFrame = 0;
struct SRModePlan {
    unsigned width=0, height=0, minWidth=0, minHeight=0, maxWidth=0, maxHeight=0;
    bool valid=false;
};
static SRModePlan g_srPlans[5]{};
static const wchar_t* ModeName(int mode) {
    switch (mode) {
      case 1: return L"QUALITY"; case 2: return L"BALANCED";
      case 3: return L"PERFORMANCE"; case 4: return L"ULTRA_PERFORMANCE";
      default: return L"DLAA";
    }
}
static int ParseMode(const wchar_t* name) {
    if (_wcsicmp(name,L"quality")==0) return 1;
    if (_wcsicmp(name,L"balanced")==0) return 2;
    if (_wcsicmp(name,L"performance")==0) return 3;
    if (_wcsicmp(name,L"ultra_performance")==0) return 4;
    return 0;
}
static int ModeForCurrentBuffers() {
    // Prior to the first constructor the menu request is merely a plan.
    // Once engine targets exist, never evaluate against a not-yet-allocated mode.
    if (g_srReconfigureAtConstructor &&
        InterlockedCompareExchange64(&g_srConstructorCount,0,0) != 0)
        return g_srStartupMode;
    return g_requestedMode;
}
static NVSDK_NGX_PerfQuality_Value QualityForMode(int mode) {
    switch(mode) {
      case 1: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
      case 2: return NVSDK_NGX_PerfQuality_Value_Balanced;
      case 3: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
      case 4: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
      default: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}
static const char* PresetKeyForMode(int mode) {
    switch(mode) {
      case 1: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
      case 2: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
      case 3: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
      case 4: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
      default: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
    }
}

static LARGE_INTEGER g_qpcFrequency{};
static LARGE_INTEGER g_previousQpc{};
static ID3D11Texture2D* g_output = nullptr; // reused, never maps or aliases input
static ID3D11Texture2D* g_firstColor = nullptr; // non-owning identity guard
static ID3D11Texture2D* g_firstMotion = nullptr;
static ID3D11Texture2D* g_firstDepth = nullptr;
static ID3D11Device1* g_device1 = nullptr;
static ID3D11DeviceContext1* g_context1 = nullptr;
static ID3DDeviceContextState* g_isolatedState = nullptr;
static DWORD g_renderTid = 0;
static LONG g_otherThreadLogged = 0;
static int g_state = 0; // 0=not tried, 1=ready, -1=failed; only written on render thread
static NVSDK_NGX_Handle* g_feature = nullptr;
static NVSDK_NGX_Parameter* g_params = nullptr;
static std::vector<unsigned char> g_scratch;
static ID3D11Device* g_ngxDevice = nullptr; // retained only on success
static ID3D11DeviceContext* g_ngxContext = nullptr;

// SR handoff test: establish direct NGX-output-to-composite sampling at native
// resolution BEFORE any attempt to resize the scene inputs. This is NOT SR yet.
// Never enable direct-copy bypass by default. The original scene-color CopyResource
// remains the fallback if a composite binding is not recognized.
using PSSetShaderResourcesFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                      ID3D11ShaderResourceView* const*);
static PSSetShaderResourcesFn g_originalPSSetShaderResources = nullptr;
static bool g_psSetHookInstalled = false;
static bool g_psSetHookAttempted = false;
static int g_handoffMode = 0; // maximum configured handoff capability: 2 required for true SR
// v083: Select the stable native CopyResource route when NGX is in DLAA.
// Do NOT overwrite g_handoffMode: renderer construction still needs mode 2
// available to enter a saved Quality/Balanced/Performance mode later.
static inline int MpEffectiveHandoffMode() {
    return g_activeMode == 0 ? 0 : g_handoffMode;
}
static volatile LONG g_insideGameComposite = 0;
static ID3D11Texture2D* g_handoffSceneColor = nullptr;  // borrowed, valid within the hook frame
static ID3D11Texture2D* g_handoffPresentedTexture = nullptr; // borrowed from owned NGX textures
static ID3D11Texture2D* g_handoffViewTexture = nullptr; // borrowed; SRV owns its resource
static ID3D11ShaderResourceView* g_handoffView = nullptr; // owned
static D3D11_SHADER_RESOURCE_VIEW_DESC g_handoffViewDesc{};
static bool g_handoffViewDescValid = false;
static bool g_handoffResultReadyThisFrame = false;
static bool g_handoffCopySkippedThisFrame = false;
static bool g_handoffForceCopyNextFrame = false;
static unsigned g_handoffBindingsThisFrame = 0;
static unsigned g_handoffLastGoodFrames = 0;
static unsigned long long g_handoffTotalBindings = 0;
static unsigned long long g_handoffTotalDirectFrames = 0;
static unsigned long long g_handoffTotalCopyFrames = 0;

// v0.8.3: opt-in sampled CPU profiling, shared by both verified executable adapters.
// QPC measures the CPU time spent IN these functions, not asynchronous GPU execution.
// No per-frame file I/O and no QueryPerformanceCounter on unsampled calls.
static void Log(const wchar_t* fmt, ...);
// v0.8.4 candidate: sampling instrumentation is not compiled into the release
// execution path. The separate v0.8.3 profiler archive remains available.
// Compile-time false allows the optimizer to eliminate the enormous number
// of per-draw / per-jitter sample-site checks.
static constexpr bool g_profilerEnabled = false;
static bool g_releaseDiagnosticsEnabled = false;
static volatile LONG g_releaseCriticalLogCount = 0;
static unsigned g_profilerSampleEvery = 120;
static unsigned g_profilerReportSamples = 16;
static LARGE_INTEGER g_profilerFrequency{};
enum ProfilerPhase {
    kProfileMpComposite = 0,
    kProfileSharedNgxEvaluate = 1,
    kProfileMpHandoffBind = 2,
    kProfilePhaseCount = 3
};
struct ProfileTotals {
    unsigned long long calls = 0;
    unsigned long long samples = 0;
    LONGLONG ticks = 0;
    LONGLONG maxTicks = 0;
};
// Each mode has independent counters; transitions never contaminate a sample.
static ProfileTotals g_profileTotals[5][kProfilePhaseCount]{};
static void ReportProfileSample(int mode) {
    if (!g_profilerFrequency.QuadPart) return;
    if (mode < 0 || mode > 4) return;
    const auto& mp = g_profileTotals[mode][kProfileMpComposite];
    const auto& ngx = g_profileTotals[mode][kProfileSharedNgxEvaluate];
    const auto& bind = g_profileTotals[mode][kProfileMpHandoffBind];
    const double scale = 1000.0 / double(g_profilerFrequency.QuadPart);
    const auto avg = [scale](const ProfileTotals& t) -> double {
        return t.samples ? double(t.ticks) * scale / double(t.samples) : 0.0;
    };
    Log(L"PROFILER_CPU_BY_MODE profile=%ls mode=%ls separate_modes=1 sample_every=%u "
        L"mp_calls=%llu mp_samples=%llu mp_avg_ms=%.4f mp_max_ms=%.4f "
        L"ngx_calls=%llu ngx_samples=%llu ngx_avg_ms=%.4f ngx_max_ms=%.4f "
        L"handoff_bind_calls=%llu handoff_bind_samples=%llu handoff_bind_avg_ms=%.4f handoff_bind_max_ms=%.4f "
        L"gpu_time_this_record=0 gpu_details=V083_GPU_FRAME shader_bind_other_calls_measured_in_v083=1",
        g_gammaNativeTrialActive ? L"GAMMA" : L"MULTIPLAYER", ModeName(mode), g_profilerSampleEvery,
        mp.calls,mp.samples,avg(mp),double(mp.maxTicks)*scale,
        ngx.calls,ngx.samples,avg(ngx),double(ngx.maxTicks)*scale,
        bind.calls,bind.samples,avg(bind),double(bind.maxTicks)*scale);
}
struct ProfileScope {
    ProfilerPhase phase;
    int mode;
    bool sampled = false;
    LARGE_INTEGER begin{};
    explicit ProfileScope(ProfilerPhase p) : phase(p), mode(g_activeMode) {
        if (!g_profilerEnabled || mode < 0 || mode > 4) return;
        auto& totals = g_profileTotals[mode][static_cast<unsigned>(phase)];
        ++totals.calls;
        if (totals.calls % g_profilerSampleEvery == 0) {
            QueryPerformanceCounter(&begin);
            sampled = true;
        }
    }
    ~ProfileScope() {
        if (!sampled) return;
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&end);
        auto& totals = g_profileTotals[mode][static_cast<unsigned>(phase)];
        const LONGLONG delta = end.QuadPart - begin.QuadPart;
        if (delta < 0) return;
        totals.ticks += delta;
        if (delta > totals.maxTicks) totals.maxTicks = delta;
        ++totals.samples;
        // Report outside the sampled outer MP hook; GAMMA has no MP hook.
        if (totals.samples % g_profilerReportSamples == 0 &&
            (phase == kProfileMpComposite ||
             (phase == kProfileSharedNgxEvaluate && g_gammaNativeTrialActive)))
            ReportProfileSample(mode);
    }
};

// v0.8.3 ONE-SHOT DIAGNOSTICS. Observational only: all query operations are
// sampled, nonblocking and executed on the EXISTING D3D11 immediate thread.
// Never Flush, never spin on GetData, never dispatch NGX from a worker.
// GPU segments cover NGX+handoff+final composite, NOT the whole game frame.
static constexpr unsigned kV083Points = 7;
static constexpr unsigned kV083GpuSlots = 8;
static constexpr unsigned kV083ModeCount = 6; // 0..4 enabled, 5 = DLSS off
static const wchar_t* V083ModeName(int m) {
    return m == 5 ? L"OFF" : (m >= 0 && m <= 4 ? ModeName(m) : L"UNKNOWN");
}
static int V083CurrentMode() {
    return !g_evaluationEnabled ? 5 : (g_activeMode >= 0 && g_activeMode <= 4 ? g_activeMode : 5);
}
static unsigned long long V083ThreadTicks(const FILETIME& value) {
    ULARGE_INTEGER i{};
    i.LowPart = value.dwLowDateTime; i.HighPart = value.dwHighDateTime;
    return i.QuadPart;
}
struct V083StageTotals {
    unsigned long long count = 0;
    double sum = 0.0;
    double maximum = 0.0;
    void Add(double v) {
        if (v < 0.0) return;
        ++count; sum += v;
        if (v > maximum) maximum = v;
    }
    double Average() const { return count ? sum / static_cast<double>(count) : 0.0; }
};
struct V083ModeTotals {
    unsigned long long gpuCompleted = 0;
    unsigned long long gpuDisjoint = 0;
    unsigned long long cpuSamples = 0;
    V083StageTotals gpuNgx, gpuAux, gpuCopyback, gpuComposite, gpuSpan, gpuWorld;
    V083StageTotals cpuSwapIn, cpuNgx, cpuAux, cpuCopyback, cpuSwapOut, cpuComposite, cpuSpan, cpuWorld;
};
struct V083GpuSlot {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* stamp[kV083Points]{};
    unsigned marks = 0;
    bool pending = false;
    bool gammaOnly = false;
    bool worldOnly = false;
    int mode = 0;
    unsigned long long submissionFrame = 0;
    LARGE_INTEGER cpuQpc[kV083Points]{};
    unsigned long long threadCyclesBegin = 0;
    unsigned long long threadCyclesEnd = 0;
};
struct V083ThreadTracking {
    unsigned long long frames = 0;
    unsigned long long sampledIntervals = 0;
    unsigned long long lastThread100ns = 0;
    LARGE_INTEGER lastWall{};
};
static V083GpuSlot g_v083Slots[kV083GpuSlots]{};
static V083GpuSlot* g_v083Current = nullptr;
static V083ModeTotals g_v083Totals[kV083ModeCount]{};
static V083ThreadTracking g_v083Threads[kV083ModeCount]{};
static unsigned long long g_v083FrameCounter = 0;
static unsigned long long g_v083GpuPollCalls = 0;
static unsigned long long g_v083GpuPollNotReady = 0;
static unsigned long long g_v083GpuNoFreeSlot = 0;
static unsigned long long g_v083GpuCreateErrors = 0;
static unsigned long long g_v083GpuReadErrors = 0;
static unsigned long long g_v083GpuQueryExpired = 0;
static unsigned long long g_v083GpuDisjoint = 0;
static double g_v083GpuPollMaxMs = 0.0;
static double g_v083GpuPollSumMs = 0.0;
static int g_v083ThreadPreviousMode = -1;
static unsigned g_v083GpuReportEvery = 8;
static bool g_v083GpuProfilingDisabled = false;
static unsigned long long g_v083SrvCalls[kV083ModeCount]{};
static unsigned long long g_v083SrvCompositeCalls[kV083ModeCount]{};
static unsigned long long g_v083SrvMatches[kV083ModeCount]{};
static V083StageTotals g_v083SrvCpu[kV083ModeCount]{};
static V083StageTotals g_v083SrvCompositeCpu[kV083ModeCount]{};
static V083StageTotals g_v083LogCpu[kV083ModeCount]{};
static unsigned long long g_v083LogCalls[kV083ModeCount]{};
static double V083ElapsedMs(LONGLONG ticks) {
    return g_profilerFrequency.QuadPart > 0 ?
        1000.0 * static_cast<double>(ticks) / static_cast<double>(g_profilerFrequency.QuadPart) : 0.0;
}
static double V083QpcDifference(const LARGE_INTEGER& before, const LARGE_INTEGER& after) {
    return V083ElapsedMs(after.QuadPart - before.QuadPart);
}
static void V083ReleaseQueries(V083GpuSlot& slot) {
    for (unsigned i=0; i<kV083Points; ++i) {
        if (slot.stamp[i]) { slot.stamp[i]->Release(); slot.stamp[i] = nullptr; }
    }
    if (slot.disjoint) { slot.disjoint->Release(); slot.disjoint = nullptr; }
    slot.pending = false; slot.marks = 0;
}
static bool V083EnsureQueries(V083GpuSlot& slot) {
    if (!g_ngxDevice || !g_ngxContext || g_v083GpuProfilingDisabled) return false;
    if (slot.disjoint) return true;
    D3D11_QUERY_DESC desc{};
    desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    HRESULT hr = g_ngxDevice->CreateQuery(&desc, &slot.disjoint);
    if (FAILED(hr) || !slot.disjoint) goto failed;
    desc.Query = D3D11_QUERY_TIMESTAMP;
    for (unsigned i=0; i<kV083Points; ++i) {
        hr = g_ngxDevice->CreateQuery(&desc, &slot.stamp[i]);
        if (FAILED(hr) || !slot.stamp[i]) goto failed;
    }
    return true;
failed:
    V083ReleaseQueries(slot);
    ++g_v083GpuCreateErrors;
    g_v083GpuProfilingDisabled = true;
    Log(L"V083_GPU_QUERIES_UNAVAILABLE hr=0x%08X gpu_measurement_disabled=1 cpu_measurement_continues=1",
        static_cast<unsigned>(hr));
    return false;
}
static void V083ReportHotHooks(int mode);
static void V083GpuReport(int mode) {
    if (mode < 0 || mode >= static_cast<int>(kV083ModeCount)) return;
    const auto& t = g_v083Totals[mode];
    Log(L"V083_LOG_COST mode=%ls log_calls=%llu log_sampled=%llu cpu_log_avg_ms=%.5f "
        L"cpu_log_max_ms=%.5f includes_file_open_close_and_debug_string=1",
        V083ModeName(mode),g_v083LogCalls[mode],g_v083LogCpu[mode].count,
        g_v083LogCpu[mode].Average(),g_v083LogCpu[mode].maximum);
    Log(L"V083_GPU_WORLD_SUMMARY mode=%ls gpu_fullframe_scope_samples=%llu "
        L"gpu_fullframe_scope_avg_ms=%.5f gpu_fullframe_scope_max_ms=%.5f "
        L"cpu_fullframe_scope_avg_ms=%.5f "
        L"scope=engine_fullframe_wrapper_not_dxgi_present",
        V083ModeName(mode),t.gpuWorld.count,t.gpuWorld.Average(),
        t.gpuWorld.maximum,t.cpuWorld.Average());
    Log(L"V083_GPU_SUMMARY mode=%ls valid=%llu disjoint=%llu "
        L"gpu_ngx_avg_ms=%.5f gpu_ngx_max_ms=%.5f gpu_aux_avg_ms=%.5f "
        L"gpu_copy_avg_ms=%.5f gpu_composite_avg_ms=%.5f gpu_pipeline_span_avg_ms=%.5f "
        L"cpu_swapin_avg_ms=%.5f cpu_ngx_avg_ms=%.5f cpu_aux_avg_ms=%.5f "
        L"cpu_copy_avg_ms=%.5f cpu_swapout_avg_ms=%.5f cpu_composite_avg_ms=%.5f "
        L"cpu_pipeline_span_avg_ms=%.5f srv_hook_calls=%llu srv_composite_calls=%llu "
        L"srv_replacements=%llu srv_hook_sample_avg_ms=%.5f srv_composite_sample_avg_ms=%.5f "
        L"query_pending_poll=%llu query_not_ready=%llu query_no_slot=%llu "
        L"query_read_errors=%llu query_expired=%llu query_poll_avg_ms=%.5f query_poll_max_ms=%.5f",
        V083ModeName(mode),t.gpuCompleted,t.gpuDisjoint,
        t.gpuNgx.Average(),t.gpuNgx.maximum,t.gpuAux.Average(),
        t.gpuCopyback.Average(),t.gpuComposite.Average(),t.gpuSpan.Average(),
        t.cpuSwapIn.Average(),t.cpuNgx.Average(),t.cpuAux.Average(),
        t.cpuCopyback.Average(),t.cpuSwapOut.Average(),t.cpuComposite.Average(),
        t.cpuSpan.Average(),g_v083SrvCalls[mode],g_v083SrvCompositeCalls[mode],
        g_v083SrvMatches[mode],g_v083SrvCpu[mode].Average(),
        g_v083SrvCompositeCpu[mode].Average(),
        g_v083GpuPollCalls,g_v083GpuPollNotReady,g_v083GpuNoFreeSlot,
        g_v083GpuReadErrors,g_v083GpuQueryExpired,
        g_v083GpuPollCalls ? g_v083GpuPollSumMs/static_cast<double>(g_v083GpuPollCalls) : 0.0,
        g_v083GpuPollMaxMs);
}
static void V083PollGpuQueries() {
    if (!g_profilerEnabled || !g_ngxContext) return;
    // One bounded scan every 8 renderer/composite ticks. No loop waiting for S_OK.
    if ((g_v083FrameCounter & 7ull) != 0ull) return;
    for (unsigned i=0; i<kV083GpuSlots; ++i) {
        V083GpuSlot& slot = g_v083Slots[i];
        if (!slot.pending || !slot.disjoint) continue;
        LARGE_INTEGER pollStart{}, pollStop{};
        QueryPerformanceCounter(&pollStart);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
        const HRESULT rc = g_ngxContext->GetData(slot.disjoint, &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        QueryPerformanceCounter(&pollStop);
        const double pollMs = V083QpcDifference(pollStart,pollStop);
        ++g_v083GpuPollCalls; g_v083GpuPollSumMs += pollMs;
        if (pollMs > g_v083GpuPollMaxMs) g_v083GpuPollMaxMs = pollMs;
        if (rc == S_FALSE) {
            ++g_v083GpuPollNotReady;
            if (g_v083FrameCounter - slot.submissionFrame <= 2400ull) continue;
            ++g_v083GpuQueryExpired;
            Log(L"V083_GPU_SAMPLE_EXPIRED mode=%ls age_frames=%llu nonblocking=1",
                V083ModeName(slot.mode),g_v083FrameCounter-slot.submissionFrame);
            slot.pending = false; continue;
        }
        if (FAILED(rc)) {
            ++g_v083GpuReadErrors; slot.pending = false;
            Log(L"V083_GPU_QUERY_FAILED hr=0x%08X mode=%ls",static_cast<unsigned>(rc),V083ModeName(slot.mode));
            continue;
        }
        auto& totals = g_v083Totals[slot.mode];
        if (d.Disjoint || !d.Frequency) {
            ++totals.gpuDisjoint; ++g_v083GpuDisjoint; slot.pending = false;
            if (totals.gpuDisjoint <= 3)
                Log(L"V083_GPU_DISJOINT mode=%ls frequency=%llu",V083ModeName(slot.mode),d.Frequency);
            continue;
        }
        UINT64 timestamp[kV083Points]{};
        bool stillPending = false;
        bool failed = false;
        for (unsigned t=0; t<kV083Points; ++t) {
            if (!(slot.marks & (1u << t))) continue;
            const HRESULT trc = g_ngxContext->GetData(slot.stamp[t],&timestamp[t],sizeof(UINT64),
                                                       D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (trc == S_FALSE) { stillPending = true; break; }
            if (FAILED(trc)) { failed = true; break; }
        }
        if (stillPending) { ++g_v083GpuPollNotReady; continue; }
        if (failed) { ++g_v083GpuReadErrors; slot.pending=false; continue; }
        const auto gpuMs = [&timestamp,&slot,&d](unsigned left,unsigned right)->double {
            if (!(slot.marks & (1u<<left)) || !(slot.marks & (1u<<right)) ||
                timestamp[right]<timestamp[left]) return -1.0;
            return 1000.0*static_cast<double>(timestamp[right]-timestamp[left])/
                   static_cast<double>(d.Frequency);
        };
        const auto cpuMs = [&slot](unsigned left,unsigned right)->double {
            if (!(slot.marks & (1u<<left)) || !(slot.marks & (1u<<right))) return -1.0;
            return V083QpcDifference(slot.cpuQpc[left],slot.cpuQpc[right]);
        };
        if (slot.worldOnly) {
            const double gpuWorld=gpuMs(0,6);
            const double cpuWorld=cpuMs(0,6);
            totals.gpuWorld.Add(gpuWorld);
            totals.cpuWorld.Add(cpuWorld);
            ++totals.gpuCompleted; ++totals.cpuSamples;
            Log(L"V083_GPU_WORLD_FRAME mode=%ls gpu_fullframe_scope_ms=%.5f "
                L"cpu_fullframe_scope_ms=%.5f readback_age_frames=%llu "
                L"timestamp_frequency=%llu disjoint=0 scope=engine_fullframe_wrapper",
                V083ModeName(slot.mode),gpuWorld,cpuWorld,
                g_v083FrameCounter-slot.submissionFrame,d.Frequency);
            slot.pending=false;
            if (totals.gpuCompleted % g_v083GpuReportEvery == 0) {
                V083GpuReport(slot.mode);
                V083ReportHotHooks(slot.mode);
            }
            continue;
        }
        const double gpuNgx=slot.gammaOnly?gpuMs(0,1):gpuMs(1,2);
        const double gpuAux=gpuMs(2,3);
        const double gpuCopy=gpuMs(3,4);
        const double gpuComp=gpuMs(5,6);
        const double gpuTotal=slot.gammaOnly?gpuMs(0,1):gpuMs(0,6);
        const double cpuSwapIn=cpuMs(0,1);
        const double cpuNgx=slot.gammaOnly?cpuMs(0,1):cpuMs(1,2);
        const double cpuAux=cpuMs(2,3);
        const double cpuCopy=cpuMs(3,4);
        const double cpuSwapOut=cpuMs(4,5);
        const double cpuComp=cpuMs(5,6);
        const double cpuTotal=slot.gammaOnly?cpuMs(0,1):cpuMs(0,6);
        totals.gpuNgx.Add(gpuNgx);totals.gpuAux.Add(gpuAux);
        totals.gpuCopyback.Add(gpuCopy);totals.gpuComposite.Add(gpuComp);
        totals.gpuSpan.Add(gpuTotal);
        totals.cpuSwapIn.Add(cpuSwapIn);totals.cpuNgx.Add(cpuNgx);
        totals.cpuAux.Add(cpuAux);totals.cpuCopyback.Add(cpuCopy);
        totals.cpuSwapOut.Add(cpuSwapOut);totals.cpuComposite.Add(cpuComp);
        totals.cpuSpan.Add(cpuTotal);
        ++totals.gpuCompleted; ++totals.cpuSamples;
        Log(L"V083_GPU_FRAME mode=%ls kind=%ls gpu_ngx_ms=%.5f gpu_aux_ms=%.5f "
            L"gpu_copy_ms=%.5f gpu_composite_ms=%.5f gpu_span_ms=%.5f "
            L"cpu_swapin_ms=%.5f cpu_ngx_ms=%.5f cpu_aux_ms=%.5f "
            L"cpu_copy_ms=%.5f cpu_swapout_ms=%.5f cpu_composite_ms=%.5f cpu_span_ms=%.5f "
            L"readback_age_frames=%llu timestamp_frequency=%llu disjoint=0",
            V083ModeName(slot.mode),slot.gammaOnly?L"GAMMA_NGX":
            (slot.mode==5?L"MP_DISABLED_COMPOSITE":L"MP_PIPELINE"),
            gpuNgx,gpuAux,gpuCopy,gpuComp,gpuTotal,
            cpuSwapIn,cpuNgx,cpuAux,cpuCopy,cpuSwapOut,cpuComp,cpuTotal,
            g_v083FrameCounter-slot.submissionFrame,d.Frequency);
        slot.pending=false;
        if (totals.gpuCompleted % g_v083GpuReportEvery == 0) {
            V083GpuReport(slot.mode);
            V083ReportHotHooks(slot.mode);
        }
    }
}
static void V083ThreadSample(int overrideMode=-1) {
    if (!g_profilerEnabled || !g_profilerFrequency.QuadPart) return;
    const int mode = overrideMode>=0 ? overrideMode : V083CurrentMode();
    if (mode < 0 || mode >= static_cast<int>(kV083ModeCount)) return;
    if (g_v083ThreadPreviousMode != mode) {
        g_v083Threads[mode].lastWall.QuadPart = 0;
        g_v083Threads[mode].frames = 0;
        g_v083ThreadPreviousMode=mode;
    }
    V083ThreadTracking& t = g_v083Threads[mode];
    if ((++t.frames % g_profilerSampleEvery)!=0) return;
    LARGE_INTEGER wall{}; QueryPerformanceCounter(&wall);
    FILETIME create{},exit{},kernel{},user{};
    if (!GetThreadTimes(GetCurrentThread(),&create,&exit,&kernel,&user)) return;
    const unsigned long long cpu100ns=V083ThreadTicks(kernel)+V083ThreadTicks(user);
    if (t.lastWall.QuadPart && cpu100ns>=t.lastThread100ns) {
        const double wallMs=V083QpcDifference(t.lastWall,wall);
        const double cpuMs=static_cast<double>(cpu100ns-t.lastThread100ns)/10000.0;
        const double busy=wallMs>0 ? 100.0*cpuMs/wallMs : 0.0;
        ++t.sampledIntervals;
        Log(L"V083_RENDER_THREAD mode=%ls tid=%u wall_interval_ms=%.3f thread_cpu_ms=%.3f "
            L"one_core_busy_pct=%.2f cpu_wait_or_other_thread_pct_approx=%.2f "
            L"processor_at_sample=%u interval_samples=%llu "
            L"interpretation=thread_cpu_time_over_elapsed_not_total_cpu_percent",
            V083ModeName(mode),GetCurrentThreadId(),wallMs,cpuMs,busy,
            busy<=100.0?100.0-busy:0.0,GetCurrentProcessorNumber(),t.sampledIntervals);
    }
    t.lastWall=wall; t.lastThread100ns=cpu100ns;
}
static void V083BeginSample(bool gammaOnly, bool offOnly=false, bool worldOnly=false) {
    if (!g_profilerEnabled || !g_ngxContext || !g_ngxDevice ||
        g_v083Current || !g_profilerFrequency.QuadPart) return;
    const int mode = offOnly?5:(gammaOnly?g_activeMode:V083CurrentMode());
    if (mode < 0 || mode >= static_cast<int>(kV083ModeCount)) return;
    // Callers invoke this for eligible frames only, avoiding sampling menu skips.
    // MP world frames contain the original MP composite, so stagger the two
    // samples by half a period. Simultaneous due samples would otherwise let
    // the enclosing world query starve the detailed NGX/composite samples.
    static unsigned long long eligible[kV083ModeCount][3]{};
    const unsigned category=worldOnly?1u:(gammaOnly?2u:0u);
    const unsigned long long call=++eligible[mode][category];
    const unsigned halfPeriod=(!worldOnly&&!gammaOnly)?g_profilerSampleEvery/2u:0u;
    if (((call+halfPeriod)%g_profilerSampleEvery)!=0ull) return;
    V083GpuSlot* found = nullptr;
    for (unsigned i=0;i<kV083GpuSlots;++i)
        if (!g_v083Slots[i].pending) { found=&g_v083Slots[i];break; }
    if (!found) { ++g_v083GpuNoFreeSlot; return; }
    if (!V083EnsureQueries(*found)) return;
    found->marks=0; found->mode=mode;found->gammaOnly=gammaOnly;found->worldOnly=worldOnly;
    found->submissionFrame=g_v083FrameCounter;
    for (unsigned i=0;i<kV083Points;++i) found->cpuQpc[i].QuadPart=0;
    g_ngxContext->Begin(found->disjoint);
    g_v083Current=found;
}
static void V083Mark(unsigned mark) {
    if (!g_v083Current || mark>=kV083Points || !g_ngxContext) return;
    // A full-world frame query can enclose a nested RendererHook call. Only
    // its enclosing full-frame wrapper may mark/end it; the MP stage markers
    // must not interfere with that GPU query or start a second disjoint group.
    if (g_v083Current->worldOnly && mark!=0 && mark!=6) return;
    if (g_v083Current->marks&(1u<<mark)) return;
    QueryPerformanceCounter(&g_v083Current->cpuQpc[mark]);
    g_ngxContext->End(g_v083Current->stamp[mark]);
    g_v083Current->marks |= (1u<<mark);
}
static void V083CompleteSample() {
    if (!g_v083Current || !g_ngxContext) return;
    g_ngxContext->End(g_v083Current->disjoint);
    g_v083Current->pending=true;
    g_v083Current=nullptr;
}
// Unlike the expensive SRV matching path, this also samples the MANY ordinary
// PSSetShaderResources calls that immediately forward to D3D11. It is the total
// hook tax, not just the matching replacement tax, we need to exclude.
struct V083SrvScope {
    int mode = -1;
    bool composite = false;
    bool sample = false;
    LARGE_INTEGER start{};
    V083SrvScope() {
        if (!g_profilerEnabled || !g_renderTid || GetCurrentThreadId()!=g_renderTid) return;
        mode = V083CurrentMode();
        if (mode<0 || mode>=static_cast<int>(kV083ModeCount)) {mode=-1;return;}
        composite = InterlockedCompareExchange(&g_insideGameComposite,0,0)!=0;
        const unsigned long long calls=++g_v083SrvCalls[mode];
        if (composite) ++g_v083SrvCompositeCalls[mode];
        sample=(calls%g_profilerSampleEvery)==0;
        if (sample) QueryPerformanceCounter(&start);
    }
    ~V083SrvScope() {
        if (!sample || mode<0) return;
        LARGE_INTEGER end{};QueryPerformanceCounter(&end);
        const double duration=V083QpcDifference(start,end);
        g_v083SrvCpu[mode].Add(duration);
        if (composite) g_v083SrvCompositeCpu[mode].Add(duration);
    }
};

// Sampling complete intercepted call costs is important: a very frequent
// lightweight detour can add substantial CPU work even when SRV matching is fast.
// These durations INCLUDE forwarding into the original D3D/engine function;
// they are not pure incremental hook overhead without an A/B baseline.
enum V083HotHookKind {
    kV083Jitter = 0, kV083Ssfx = 1, kV083Draw = 2,
    kV083DrawIndexed = 3, kV083OmTargets = 4, kV083RsViewports = 5,
    kV083WorldFrame = 6, kV083HotCount = 7
};
static const wchar_t* V083HotKindName(unsigned kind) {
    static const wchar_t* const names[kV083HotCount] = {
        L"JITTER_DECOMPRESS",L"SSFX_JITTER",L"D3D_DRAW",L"D3D_DRAW_INDEXED",
        L"D3D_OM_SET_RTS",L"D3D_RS_SET_VIEWPORTS",L"ENGINE_FULL_FRAME"
    };
    return kind < kV083HotCount ? names[kind] : L"UNKNOWN";
}
struct V083HotHookTotals {
    unsigned long long calls=0;
    V083StageTotals cpu;
};
static V083HotHookTotals g_v083HotHooks[kV083ModeCount][kV083HotCount]{};
static void V083ReportHotHooks(int mode) {
    if (mode < 0 || mode >= static_cast<int>(kV083ModeCount)) return;
    for (unsigned tag=0; tag<kV083HotCount; ++tag) {
        const auto& s=g_v083HotHooks[mode][tag];
        Log(L"V083_HOT_HOOK mode=%ls hook=%ls calls=%llu samples=%llu "
            L"cpu_call_avg_ms=%.6f cpu_call_max_ms=%.6f "
            L"includes_original_call=1 not_pure_hook_tax=1",
            V083ModeName(mode),V083HotKindName(tag),s.calls,s.cpu.count,
            s.cpu.Average(),s.cpu.maximum);
    }
}
struct V083HotHookScope {
    int mode=-1;
    unsigned kind=0;
    bool measured=false;
    LARGE_INTEGER before{};
    explicit V083HotHookScope(unsigned id):kind(id) {
        if (!g_profilerEnabled || g_gammaNativeTrialActive ||
            !g_renderTid || GetCurrentThreadId()!=g_renderTid ||
            id>=kV083HotCount) return;
        mode=V083CurrentMode();
        if (mode<0 || mode>=static_cast<int>(kV083ModeCount)) {mode=-1;return;}
        auto& t=g_v083HotHooks[mode][kind];
        ++t.calls;
        measured=(t.calls%(kind==kV083WorldFrame ? 120ull : 512ull))==0;
        if (measured) QueryPerformanceCounter(&before);
    }
    ~V083HotHookScope() {
        if (!measured || mode<0) return;
        LARGE_INTEGER after{};QueryPerformanceCounter(&after);
        g_v083HotHooks[mode][kind].cpu.Add(V083QpcDifference(before,after));
    }
};

// Identify actual installed DLLs without guessing the DLSS generation from the
// NVIDIA SDK API version or the requested preset. File version is diagnostic.
static void V083LogNvidiaRuntime() {
    const wchar_t* modules[] = { L"nvngx_dlss.dll", L"nvngx.dll", L"_nvngx.dll" };
    for (const auto* name : modules) {
        HMODULE module = GetModuleHandleW(name);
        if (!module) {
            Log(L"V083_NVIDIA_RUNTIME module=%ls loaded=0",name);
            continue;
        }
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(module,path,MAX_PATH)) {
            Log(L"V083_NVIDIA_RUNTIME module=%ls loaded=1 path=UNKNOWN",name);
            continue;
        }
        DWORD ignored=0;
        const DWORD bytes=GetFileVersionInfoSizeW(path,&ignored);
        VS_FIXEDFILEINFO* fixed=nullptr;
        UINT versionBytes=0;
        std::vector<unsigned char> version(bytes);
        if (bytes && GetFileVersionInfoW(path,0,bytes,version.data()) &&
            VerQueryValueW(version.data(),L"\\",reinterpret_cast<LPVOID*>(&fixed),&versionBytes) &&
            fixed && versionBytes>=sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature==0xFEEF04BD) {
            Log(L"V083_NVIDIA_RUNTIME module=%ls loaded=1 "
                L"file_version=%u.%u.%u.%u path=%ls model_requested=PRESET_K",
                name,HIWORD(fixed->dwFileVersionMS),LOWORD(fixed->dwFileVersionMS),
                HIWORD(fixed->dwFileVersionLS),LOWORD(fixed->dwFileVersionLS),path);
        } else {
            Log(L"V083_NVIDIA_RUNTIME module=%ls loaded=1 file_version=UNAVAILABLE path=%ls",
                name,path);
        }
    }
}

// Preserve the original PS SRV binding so the engine's cached state agrees
// with the actual D3D11 context after our temporary composite substitution.
struct HandoffSavedBinding {
    UINT slot = 0;
    ID3D11ShaderResourceView* original = nullptr;
    ID3D11ShaderResourceView* substituted = nullptr;
};
static HandoffSavedBinding g_handoffSaved[32]{};
static unsigned g_handoffSavedCount = 0;

// Use the NVIDIA SDK public entry points directly, as declared by nvsdk_ngx.h.
// Their implementations and core-DLL ABI adaptation come from nvsdk_ngx_d.lib.
// Do not resolve private driver-core exports manually.

// Production build: no bridge log files or debugger output. The Log API is
// retained to keep the GPU/renderer control flow identical to v0.91.1.
// To build a troubleshooting variant, use the archived diagnostic sources.
static void Log(const wchar_t*, ...) {}

// Hook the actual D3D11 immediate-context method (vtable slot 8), not a
// guessed engine instruction address. Only operate inside the already-hooked
// original composite, on the NGX render thread, and only when its SRV refers
// to our validated scene-color texture. All other calls are forwarded intact.
static void STDMETHODCALLTYPE HookPSSetShaderResources(
    ID3D11DeviceContext* ctx, UINT startSlot, UINT viewCount,
    ID3D11ShaderResourceView* const* views) {
    if (!g_originalPSSetShaderResources) return;
    V083SrvScope v083TotalSrvHookTax;
    // Native DLAA uses mode 0: no shader-resource inspection or substitutions.
    // Keep this fast path first because PSSetShaderResources is a hot D3D call.
    if (MpEffectiveHandoffMode() == 0) {
        g_originalPSSetShaderResources(ctx, startSlot, viewCount, views);
        return;
    }
    if (!InterlockedCompareExchange(&g_insideGameComposite, 0, 0) ||
        ctx != g_ngxContext || GetCurrentThreadId() != g_renderTid ||
        !g_handoffSceneColor || !g_handoffPresentedTexture ||
        !views || !viewCount || viewCount > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        g_originalPSSetShaderResources(ctx, startSlot, viewCount, views);
        return;
    }
    ProfileScope handoffProfile(kProfileMpHandoffBind);
    ID3D11ShaderResourceView* remapped[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    for (UINT i = 0; i < viewCount; ++i) remapped[i] = views[i];
    bool replaced = false;
    for (UINT i = 0; i < viewCount; ++i) {
        if (!views[i]) continue;
        ID3D11Resource* source = nullptr;
        views[i]->GetResource(&source);
        const bool matches = source == static_cast<ID3D11Resource*>(g_handoffSceneColor);
        if (source) source->Release();
        if (!matches) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        views[i]->GetDesc(&desc);
        // Preserve the game's source-view format and mip/subresource mapping;
        // blindly binding a default-UNORM view could alter color space.
        if (!g_handoffView || g_handoffViewTexture != g_handoffPresentedTexture ||
            !g_handoffViewDescValid ||
            std::memcmp(&g_handoffViewDesc, &desc, sizeof(desc)) != 0) {
            ID3D11ShaderResourceView* candidate = nullptr;
            const HRESULT hr = g_ngxDevice->CreateShaderResourceView(
                g_handoffPresentedTexture, &desc, &candidate);
            if (FAILED(hr) || !candidate) {
                static unsigned failedViews = 0;
                if (failedViews++ < 8)
                    Log(L"SR_HANDOFF_VIEW_UNSUPPORTED slot=%u format=%u hr=0x%08X copyback_remains_available=1",
                        startSlot + i, static_cast<unsigned>(desc.Format), static_cast<unsigned>(hr));
                continue;
            }
            if (g_handoffView) g_handoffView->Release();
            g_handoffView = candidate;
            g_handoffViewTexture = g_handoffPresentedTexture;
            g_handoffViewDesc = desc;
            g_handoffViewDescValid = true;
        }
        const UINT absoluteSlot = startSlot + i;
        if (absoluteSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) continue;
        unsigned savedIndex = 0;
        for (; savedIndex < g_handoffSavedCount; ++savedIndex)
            if (g_handoffSaved[savedIndex].slot == absoluteSlot) break;
        if (savedIndex == g_handoffSavedCount) {
            if (g_handoffSavedCount >= _countof(g_handoffSaved)) continue;
            ++g_handoffSavedCount;
        } else {
            if (g_handoffSaved[savedIndex].original)
                g_handoffSaved[savedIndex].original->Release();
            if (g_handoffSaved[savedIndex].substituted)
                g_handoffSaved[savedIndex].substituted->Release();
        }
        g_handoffSaved[savedIndex].slot = absoluteSlot;
        g_handoffSaved[savedIndex].original = views[i];
        g_handoffSaved[savedIndex].substituted = g_handoffView;
        views[i]->AddRef();
        g_handoffView->AddRef();
        remapped[i] = g_handoffView;
        replaced = true;
        ++g_handoffBindingsThisFrame;
        ++g_handoffTotalBindings;
        if (g_profilerEnabled && g_activeMode>=0 && g_activeMode<5)
            ++g_v083SrvMatches[g_activeMode];
        if (g_handoffTotalBindings <= 8 || g_handoffTotalBindings % 2400 == 0) {
            Log(L"SR_HANDOFF_SRV_BIND slot=%u input=%p ngx_output=%p viewformat=%u mode=%d total=%llu",
                startSlot + i, g_handoffSceneColor, g_handoffPresentedTexture,
                static_cast<unsigned>(desc.Format), g_handoffMode, g_handoffTotalBindings);
        }
    }
    g_originalPSSetShaderResources(ctx, startSlot, viewCount,
                                   replaced ? remapped : views);
}

// Restore only slots whose final binding is still OUR temporary substitute.
// If the engine subsequently bound something else, that later choice wins.
static void RestoreCompositeShaderBindings() {
    for (unsigned i = 0; i < g_handoffSavedCount; ++i) {
        auto& saved = g_handoffSaved[i];
        ID3D11ShaderResourceView* currentlyBound = nullptr;
        g_ngxContext->PSGetShaderResources(saved.slot, 1, &currentlyBound);
        if (currentlyBound == saved.substituted && g_originalPSSetShaderResources)
            g_originalPSSetShaderResources(g_ngxContext, saved.slot, 1, &saved.original);
        if (currentlyBound) currentlyBound->Release();
        if (saved.original) saved.original->Release();
        if (saved.substituted) saved.substituted->Release();
        saved = HandoffSavedBinding{};
    }
    g_handoffSavedCount = 0;
}

static bool InstallCompositeSRVHook() {
    if (g_psSetHookInstalled) return true;
    if (g_handoffMode == 0 || !g_ngxContext || !g_renderTid ||
        GetCurrentThreadId() != g_renderTid) return false;
    void** vtable = *reinterpret_cast<void***>(g_ngxContext);
    if (!vtable || !vtable[8]) return false;
    void* target = vtable[8]; // ID3D11DeviceContext::PSSetShaderResources
    const MH_STATUS created = MH_CreateHook(target,
        reinterpret_cast<void*>(&HookPSSetShaderResources),
        reinterpret_cast<void**>(&g_originalPSSetShaderResources));
    if (created != MH_OK) {
        Log(L"SR_HANDOFF_HOOK_UNAVAILABLE create_status=%d baseline_copyback_preserved=1", created);
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK) {
        Log(L"SR_HANDOFF_HOOK_UNAVAILABLE enable_status=%d baseline_copyback_preserved=1", enabled);
        MH_RemoveHook(target);
        g_originalPSSetShaderResources = nullptr;
        return false;
    }
    g_psSetHookInstalled = true;
    Log(L"SR_HANDOFF_HOOK_INSTALLED method=PSSetShaderResources vtable_slot=8 mode=%d target=%p",
        g_handoffMode, target);
    return true;
}

// Shared per-user configuration path is also passed to the game Lua VM by the MO2 launcher.
// This file lives outside MO2's virtualized gamedata tree and is writable by normal users.
static void ConfigureSettingsPath() {
    DWORD length = GetEnvironmentVariableW(L"ANOMALY_NGX_CONFIG", g_settingsFile,
                                            static_cast<DWORD>(_countof(g_settingsFile)));
    if (length && length < _countof(g_settingsFile)) return;
    // Compatibility with launches using the older menu/launcher. No settings are discarded.
    length = GetEnvironmentVariableW(L"XRRAZOM_DLAA_CONFIG", g_settingsFile,
                                      static_cast<DWORD>(_countof(g_settingsFile)));
    if (length && length < _countof(g_settingsFile)) return;
    wchar_t local[MAX_PATH]{};
    length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    wchar_t dir[MAX_PATH + 64]{};
    swprintf_s(dir, L"%ls\\anomaly_ngx", local);
    CreateDirectoryW(dir, nullptr);
    swprintf_s(g_settingsFile, L"%ls\\settings.ini", dir);
    wchar_t legacy[MAX_PATH + 128]{};
    swprintf_s(legacy, L"%ls\\xrrazom_dlaa\\settings.ini", local);
    // CopyFileW with failIfExists=TRUE: never overwrite new settings on subsequent starts.
    if (CopyFileW(legacy, g_settingsFile, TRUE))
        Log(L"CONFIG_MIGRATED previous=user_legacy new=%ls", g_settingsFile);
}
static int ParsePreset(const wchar_t* text) {
    // Release UI exposes only J/K/L/M. Previously saved DEFAULT, E/F and
    // recommended are normalized to K without editing the user's file.
    if (_wcsicmp(text, L"j") == 0) return 10;
    if (_wcsicmp(text, L"l") == 0) return 12;
    if (_wcsicmp(text, L"m") == 0) return 13;
    return 11; // K, including migrated legacy selections.
}
static const wchar_t* PresetName(int v) {
    switch (v) {
      case 1: return L"RECOMMENDED";
      case 5: return L"E_DEPRECATED";
      case 6: return L"F_DEPRECATED";
      case 10: return L"J";
      case 11: return L"K";
      case 12: return L"L";
      case 13: return L"M";
      default: return L"DEFAULT";
    }
}
// "recommended" is an explicit, stable DLSS 4.5 mode map rather than the
// driver-controlled Default, which can change after an OTA update.
static unsigned EffectivePresetForMode(int mode) {
    return g_preset != 1 ? static_cast<unsigned>(g_preset) :
        (mode == 4 ? 12u : (mode == 3 ? 13u : 11u));
}
// Configure a feature only for input dimensions already produced by the game.
// This bridge NEVER fabricates lower-res input by downsampling a 1080p render.
static void SetFeatureModeParams(int mode, unsigned renderW, unsigned renderH) {
    g_params->Set(NVSDK_NGX_Parameter_Width, renderW);
    g_params->Set(NVSDK_NGX_Parameter_Height, renderH);
    g_params->Set(NVSDK_NGX_Parameter_OutWidth, g_displayWidth);
    g_params->Set(NVSDK_NGX_Parameter_OutHeight, g_displayHeight);
    g_params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(QualityForMode(mode)));
    g_params->Set(PresetKeyForMode(mode), EffectivePresetForMode(mode));
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
        static_cast<unsigned int>(mode ? NVSDK_NGX_DLSS_Feature_Flags_MVLowRes : 0));
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
}
static bool RefreshScratchForMode() {
    size_t bytes=0;
    const NVSDK_NGX_Result result = NVSDK_NGX_D3D11_GetScratchBufferSize(
        NVSDK_NGX_Feature_SuperSampling, g_params, &bytes);
    if (NVSDK_NGX_FAILED(result) || bytes > 64ull*1024ull*1024ull) {
        Log(L"NGX_SCRATCH_MODE_REJECT result=0x%08X bytes=%llu",
            static_cast<unsigned>(result), static_cast<unsigned long long>(bytes));
        return false;
    }
    g_scratch.resize(bytes);
    g_params->Set(NVSDK_NGX_Parameter_Scratch,
        bytes ? static_cast<void*>(g_scratch.data()) : static_cast<void*>(nullptr));
    g_params->Set(NVSDK_NGX_Parameter_Scratch_SizeInBytes,
        static_cast<unsigned long long>(bytes));
    Log(L"NGX_SCRATCH_MODE result=0x%08X bytes=%llu",
        static_cast<unsigned>(result), static_cast<unsigned long long>(bytes));
    return true;
}
static bool RecreateFeature(int mode, unsigned renderW, unsigned renderH) {
    if (g_state != 1 || !g_params || !g_ngxContext || !g_feature ||
        !g_displayWidth || !g_displayHeight) return false;
    if (mode != 0 && (!g_srPlans[mode].valid ||
        renderW != g_srPlans[mode].width || renderH != g_srPlans[mode].height)) {
        Log(L"SR_REFUSED_RECREATE mode=%ls reason=unverified_input_size input=%ux%u",
            ModeName(mode), renderW, renderH);
        return false;
    }
    const int oldMode = g_activeMode;
    const unsigned oldW = g_activeRenderWidth, oldH = g_activeRenderHeight;
    ID3DDeviceContextState* previous = nullptr;
    if (g_context1 && g_isolatedState)
        g_context1->SwapDeviceContextState(g_isolatedState, &previous);
    NVSDK_NGX_Result rc = NVSDK_NGX_D3D11_ReleaseFeature(g_feature);
    g_feature = nullptr;
    Log(L"NGX_MODE_RELEASE previous=%ls result=0x%08X", ModeName(oldMode), static_cast<unsigned>(rc));
    SetFeatureModeParams(mode, renderW, renderH);
    if (RefreshScratchForMode())
        rc = NVSDK_NGX_D3D11_CreateFeature(g_ngxContext,
                    NVSDK_NGX_Feature_SuperSampling, g_params, &g_feature);
    else rc = NVSDK_NGX_Result_FAIL_InvalidParameter;
    Log(L"NGX_MODE_CREATE requested=%ls input=%ux%u output=%ux%u preset=%ls result=0x%08X handle=%p",
        ModeName(mode),renderW,renderH,g_displayWidth,g_displayHeight,PresetName(g_preset),
        static_cast<unsigned>(rc),g_feature);
    if (NVSDK_NGX_FAILED(rc) || !g_feature) {
        // Attempt to recover the previous known-good feature. No stale handle reuse.
        SetFeatureModeParams(oldMode, oldW, oldH);
        const bool scratchOkay = RefreshScratchForMode();
        const NVSDK_NGX_Result restore = scratchOkay ?
            NVSDK_NGX_D3D11_CreateFeature(g_ngxContext,
                NVSDK_NGX_Feature_SuperSampling, g_params, &g_feature) :
            NVSDK_NGX_Result_FAIL_InvalidParameter;
        Log(L"NGX_MODE_RESTORE mode=%ls result=0x%08X handle=%p",
            ModeName(oldMode), static_cast<unsigned>(restore),g_feature);
        if (NVSDK_NGX_FAILED(restore) || !g_feature) {
            g_state=-1; g_fatalEvaluationError=true; g_evaluationEnabled=false;
            Log(L"NGX_MODE_FATAL_DISABLED restart_required=1");
        }
        if (previous) { g_context1->SwapDeviceContextState(previous,nullptr); previous->Release(); }
        return false;
    }
    g_activeMode=mode;
    g_activeRenderWidth=renderW;g_activeRenderHeight=renderH;
    g_resetPending=true;g_lastEngineFrame=0;g_previousQpc.QuadPart=0;
    if (previous) { g_context1->SwapDeviceContextState(previous,nullptr); previous->Release(); }
    return true;
}
static void RecreateFeatureForPreset() {
    if (g_state != 1 || !g_feature) return;
    Log(L"NGX_PRESET_RECREATE_BEGIN active_mode=%ls preset=%ls", ModeName(g_activeMode), PresetName(g_preset));
    RecreateFeature(g_activeMode, g_activeRenderWidth, g_activeRenderHeight);
}
// Filesystem-only; legal from the worker. Any renderer or NGX state changes
// happen later in ReloadSettingsOnRenderThread on the original render thread.
static MpSettingsSnapshot ReadMpSettingsSnapshot(const FILETIME& writeTime) {
    MpSettingsSnapshot result{};
    result.writeTime = writeTime;
    result.enabled = GetPrivateProfileIntW(L"DLAA", L"enabled", 1, g_settingsFile) != 0;
    wchar_t text[32]{};
    GetPrivateProfileStringW(L"DLAA", L"preset", L"k", text, _countof(text), g_settingsFile);
    result.preset = ParsePreset(text);
    GetPrivateProfileStringW(L"DLAA", L"upscaler_mode", L"dlaa", text, _countof(text), g_settingsFile);
    result.mode = ParseMode(text);
    result.jitterPercent = std::clamp(static_cast<int>(GetPrivateProfileIntW(
        L"DLAA", L"jitter_percent", 100, g_settingsFile)), 0, 125);
    result.sharpnessPercent = std::clamp(static_cast<int>(GetPrivateProfileIntW(
        L"DLAA", L"sharpness_percent", 0, g_settingsFile)), 0, 100);
    result.syncMode = std::clamp(static_cast<int>(GetPrivateProfileIntW(
        L"DLAA", L"sss_jitter_sync", 1, g_settingsFile)), 0, 1);
    return result;
}

// Rare file and INI calls occur on the worker. A second mtime read rejects
// an interrupted write by the Lua settings menu; it is retried next poll.
static void MpReadSettingsOnWorker() {
    if (!g_mpBackgroundSettingsRequested || !g_settingsFile[0]) return;
    static DWORD lastCheckTick = 0;
    static bool sawFile = false;
    static FILETIME lastWriteTime{};
    const DWORD now = GetTickCount();
    if (now - lastCheckTick < 200u) return;
    lastCheckTick = now;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(g_settingsFile, GetFileExInfoStandard, &attr)) {
        sawFile = false;
        return;
    }
    if (sawFile && CompareFileTime(&attr.ftLastWriteTime, &lastWriteTime) == 0)
        return;
    const MpSettingsSnapshot next = ReadMpSettingsSnapshot(attr.ftLastWriteTime);
    WIN32_FILE_ATTRIBUTE_DATA verified{};
    if (!GetFileAttributesExW(g_settingsFile, GetFileExInfoStandard, &verified) ||
        CompareFileTime(&verified.ftLastWriteTime, &attr.ftLastWriteTime) != 0)
        return;
    AcquireSRWLockExclusive(&g_mpSettingsLock);
    g_mpSettingsPublished = next;
    ++g_mpSettingsPublishedGeneration;
    ReleaseSRWLockExclusive(&g_mpSettingsLock);
    lastWriteTime = verified.ftLastWriteTime;
    sawFile = true;
}

// GetThreadTimes is CPU execution time, NOT ready-to-run scheduler wait time.
// This diagnoses whether the render thread was doing CPU work or mostly NOT
// executing. Proving preemption/ready-time requires an ETW/WPR trace.
static unsigned long long V088Filetime100ns(const FILETIME& f) {
    ULARGE_INTEGER u{}; u.LowPart=f.dwLowDateTime; u.HighPart=f.dwHighDateTime;
    return u.QuadPart;
}
// v089: read-only per-thread CPU accounting. No activity on the render thread.
// This runs at most once per 2 seconds on the existing worker and has zero
// effect when the startup diagnostic thread_census switch is disabled.
// Thread CPU time and cycle counts do NOT reveal where execution occurs;
// use WPR CPU Usage (Sampled/Precise) to obtain stacks/wait reasons.
struct V089ThreadCpuSample {
    DWORD tid = 0;
    unsigned long long created = 0;
    unsigned long long user100ns = 0;
    unsigned long long kernel100ns = 0;
    unsigned long long cycles = 0;
    bool cyclesValid = false;
};
struct V089ThreadCpuDelta {
    DWORD tid = 0;
    double cpuPct = 0;
    double userPct = 0;
    double kernelPct = 0;
    double cyclesPerCpuMs = -1;
    int priority = THREAD_PRIORITY_ERROR_RETURN;
    LONG basePriority = 0;
    int idealCpu = -1;
    unsigned long long created = 0;
};
static void V089ThreadCensusPoll() {
    if (!g_v088DiagnosticsEnabled || !g_v089ThreadCensusEnabled) return;
    const DWORD start = static_cast<DWORD>(InterlockedCompareExchange(&g_v088FirstRenderTick,0,0));
    if (!start) return;
    const DWORD now = GetTickCount();
    const DWORD elapsed = DWORD(now-start);
    if (elapsed > g_v088DurationSeconds*1000u) return;
    static DWORD lastPoll = 0;
    if (lastPoll && DWORD(now-lastPoll) < g_v089ThreadCensusIntervalMs) return;
    const DWORD interval = lastPoll ? DWORD(now-lastPoll) : 0;
    lastPoll = now;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        Log(L"V089_CENSUS_FAIL elapsed_ms=%lu stage=snapshot error=%lu",elapsed,GetLastError());
        return;
    }
    const DWORD pid=GetCurrentProcessId();
    const DWORD renderTid=static_cast<DWORD>(InterlockedCompareExchange(&g_v088RendererThread,0,0));
    const DWORD workerTid=GetCurrentThreadId();
    static std::vector<V089ThreadCpuSample> previous;
    std::vector<V089ThreadCpuSample> current;
    std::vector<V089ThreadCpuDelta> deltas;
    THREADENTRY32 thread{};
    thread.dwSize=sizeof(thread);
    unsigned inaccessible=0;
    if (Thread32First(snapshot, &thread)) do {
        if (thread.th32OwnerProcessID != pid) continue;
        HANDLE h=OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread.th32ThreadID);
        if (!h) { ++inaccessible; continue; }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(h,&created,&exited,&kernel,&user)) {
            ++inaccessible;
            CloseHandle(h);
            continue;
        }
        V089ThreadCpuSample sample{};
        sample.tid=thread.th32ThreadID;
        sample.created=V088Filetime100ns(created);
        sample.user100ns=V088Filetime100ns(user);
        sample.kernel100ns=V088Filetime100ns(kernel);
        ULONG64 cycles=0;
        sample.cyclesValid=QueryThreadCycleTime(h,&cycles)!=0;
        if (sample.cyclesValid) sample.cycles=cycles;
        const int priority=GetThreadPriority(h);
        PROCESSOR_NUMBER ideal{};
        const int idealCpu=GetThreadIdealProcessorEx(h,&ideal)?int(ideal.Number):-1;
        CloseHandle(h);
        current.push_back(sample);
        const auto p=std::find_if(previous.begin(),previous.end(),
            [&](const V089ThreadCpuSample& old) {
                return old.tid==sample.tid && old.created==sample.created;
            });
        if (!interval || p==previous.end() ||
            sample.user100ns<p->user100ns || sample.kernel100ns<p->kernel100ns) continue;
        const unsigned long long du=sample.user100ns-p->user100ns;
        const unsigned long long dk=sample.kernel100ns-p->kernel100ns;
        const double denominator=10000.0*double(interval);
        V089ThreadCpuDelta delta{};
        delta.tid=sample.tid;
        delta.userPct=100.0*double(du)/denominator;
        delta.kernelPct=100.0*double(dk)/denominator;
        delta.cpuPct=delta.userPct+delta.kernelPct;
        delta.priority=priority;
        delta.basePriority=thread.tpBasePri;
        delta.idealCpu=idealCpu;
        delta.created=sample.created;
        if (sample.cyclesValid && p->cyclesValid && sample.cycles>=p->cycles && (du+dk)>0)
            delta.cyclesPerCpuMs=10000.0*double(sample.cycles-p->cycles)/double(du+dk);
        deltas.push_back(delta);
    } while(Thread32Next(snapshot, &thread));
    CloseHandle(snapshot);
    previous.swap(current);
    if (!interval) {
        Log(L"V089_CENSUS_START elapsed_ms=%lu tracked_threads=%u inaccessible=%u",
            elapsed,static_cast<unsigned>(previous.size()),inaccessible);
        return;
    }
    std::sort(deltas.begin(),deltas.end(),
        [](const V089ThreadCpuDelta& a,const V089ThreadCpuDelta& b){return a.cpuPct>b.cpuPct;});
    double knownCores=0;
    unsigned active=0;
    double renderPct=-1,workerPct=-1;
    for (const auto& d:deltas) {
        knownCores+=d.cpuPct/100.0;
        if(d.cpuPct>=5.0) ++active;
        if(d.tid==renderTid) renderPct=d.cpuPct;
        if(d.tid==workerTid) workerPct=d.cpuPct;
    }
    Log(L"V089_CENSUS elapsed_ms=%lu interval_ms=%lu tracked=%u measured=%u "
        L"inaccessible=%u active_above_5pct=%u known_cpu_cores=%.3f "
        L"render_tid=%lu render_cpu_pct=%.2f worker_tid=%lu worker_cpu_pct=%.2f",
        elapsed,interval,static_cast<unsigned>(previous.size()),
        static_cast<unsigned>(deltas.size()),inaccessible,active,knownCores,
        renderTid,renderPct,workerTid,workerPct);
    // Top 8 CPU consumers, and additionally the diagnostic worker if not in top 8.
    unsigned printed=0;
    for (const auto& d:deltas) {
        if (printed>=8 && d.tid!=workerTid && d.tid!=renderTid) continue;
        const wchar_t* role=d.tid==renderTid?L"render":d.tid==workerTid?L"diagnostic_worker":L"other";
        Log(L"V089_THREAD elapsed_ms=%lu rank=%u role=%ls tid=%lu "
            L"cpu_pct=%.2f user_pct=%.2f kernel_pct=%.2f "
            L"cycles_per_cpu_ms=%.0f priority=%d base_priority=%ld ideal_cpu=%d created_100ns=%llu",
            elapsed,printed+1,role,d.tid,d.cpuPct,d.userPct,d.kernelPct,
            d.cyclesPerCpuMs,d.priority,d.basePriority,d.idealCpu,d.created);
        ++printed;
    }
}

static void V088StartupDiagnosticPoll() {
    if (!g_v088DiagnosticsEnabled) return;
    static DWORD lastPoll = 0;
    static DWORD lastRenderTick = 0;
    static DWORD lastTid = 0;
    static HANDLE renderThread = nullptr;
    static unsigned long long lastRenderCpu = 0;
    static unsigned long long lastProcessCpu = 0;
    static LONG64 lastRenderCalls = 0;
    static LONG64 lastEvals = 0;
    static bool completed = false;
    if (completed) return;
    const DWORD now = GetTickCount();
    if (DWORD(now-lastPoll) < g_v088IntervalMs) return;
    lastPoll = now;
    const DWORD first = static_cast<DWORD>(InterlockedCompareExchange(&g_v088FirstRenderTick,0,0));
    if (!first) {
        static unsigned waitingReports = 0;
        if (waitingReports++ < 8 && (waitingReports==1 || waitingReports%2==0))
            Log(L"V088_WAIT_FIRST_RENDER ms_uptime=%lu worker_tid=%lu",now,GetCurrentThreadId());
        return;
    }
    const DWORD elapsed = DWORD(now-first);
    if (elapsed > g_v088DurationSeconds*1000u) {
        Log(L"V088_CAPTURE_COMPLETE duration_ms=%lu pid=%lu", elapsed, GetCurrentProcessId());
        if (renderThread) CloseHandle(renderThread);
        renderThread=nullptr;
        completed=true;
        return;
    }
    const DWORD tid=static_cast<DWORD>(InterlockedCompareExchange(&g_v088RendererThread,0,0));
    if (tid && tid!=lastTid) {
        if (renderThread) CloseHandle(renderThread);
        renderThread=OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
        lastTid=tid;
        lastRenderCpu=0;
        lastRenderTick=0;
        Log(L"V088_RENDER_THREAD_OPEN tid=%lu success=%d error=%lu",tid,
            renderThread?1:0,renderThread?0:GetLastError());
    }
    FILETIME create{},exitTime{},kernel{},user{};
    unsigned long long renderCpu = 0;
    bool threadTimeOk = false;
    int renderPriority = THREAD_PRIORITY_ERROR_RETURN;
    int boostDisabled = -1;
    unsigned long long groupMask = 0;
    unsigned groupNumber = 0;
    int ideal = -1;
    if (renderThread) {
        threadTimeOk=GetThreadTimes(renderThread,&create,&exitTime,&kernel,&user)!=0;
        if (threadTimeOk) renderCpu=V088Filetime100ns(kernel)+V088Filetime100ns(user);
        renderPriority=GetThreadPriority(renderThread);
        BOOL boost=FALSE;
        if (GetThreadPriorityBoost(renderThread,&boost)) boostDisabled=boost?1:0;
        GROUP_AFFINITY ga{};
        if (GetThreadGroupAffinity(renderThread,&ga)) {
            groupMask=static_cast<unsigned long long>(ga.Mask);
            groupNumber=ga.Group;
        }
        PROCESSOR_NUMBER proc{};
        if (GetThreadIdealProcessorEx(renderThread,&proc)) ideal=int(proc.Number);
    }
    FILETIME pc{},pe{},pk{},pu{};
    unsigned long long processCpu=0;
    bool procOk=GetProcessTimes(GetCurrentProcess(),&pc,&pe,&pk,&pu)!=0;
    if (procOk) processCpu=V088Filetime100ns(pk)+V088Filetime100ns(pu);
    const DWORD interval = lastRenderTick ? DWORD(now-lastRenderTick) : 0;
    const double renderCorePct=(threadTimeOk && lastRenderCpu && interval) ?
        100.0*double(renderCpu-lastRenderCpu)/(10000.0*double(interval)) : -1.0;
    const double processCores=(procOk && lastProcessCpu && interval) ?
        double(processCpu-lastProcessCpu)/(10000.0*double(interval)) : -1.0;
    lastRenderTick=now;
    if(threadTimeOk) lastRenderCpu=renderCpu;
    if(procOk) lastProcessCpu=processCpu;
    const LONG64 calls=InterlockedCompareExchange64(&g_v088RenderCalls,0,0);
    const LONG64 evals=InterlockedCompareExchange64(&g_v088EvaluationCount,0,0);
    const LONG64 others=InterlockedCompareExchange64(&g_v088OtherThreadCalls,0,0);
    const LONG64 samples=InterlockedCompareExchange64(&g_v088SampleCount,0,0);
    const double fpsProxy=interval ? 1000.0*double(calls-lastRenderCalls)/double(interval) : -1.0;
    const double evalRate=interval ? 1000.0*double(evals-lastEvals)/double(interval) : -1.0;
    lastRenderCalls=calls;lastEvals=evals;
    LARGE_INTEGER fq{};QueryPerformanceFrequency(&fq);
    const double usPerTick=fq.QuadPart>0 ? 1000000.0/double(fq.QuadPart) : 0.0;
    const DWORD processPriority=GetPriorityClass(GetCurrentProcess());
    DWORD_PTR processMask=0,systemMask=0;
    const BOOL affinityOk=GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask);
    Log(L"V088_SCHED elapsed_ms=%lu render_tid=%lu render_priority=%d boost_disabled=%d "
        L"process_priority=0x%lX render_cpu_core_pct=%.2f process_cpu_cores=%.2f "
        L"hook_calls_per_s=%.2f ngx_evals_per_s=%.2f other_tid_calls_total=%lld "
        L"other_tid_last=%ld render_cpu_valid=%d group=%u group_mask=0x%llX "
        L"ideal_cpu=%d last_render_cpu=%ld worker_cpu=%u affinity_ok=%d "
        L"process_mask=0x%llX system_mask=0x%llX "
        L"render_priority_attempted=%ld priority_previous=%ld priority_post=%ld priority_success=%ld",
        elapsed,tid,renderPriority,boostDisabled,processPriority,renderCorePct,processCores,
        fpsProxy,evalRate,others,InterlockedCompareExchange(&g_v088OtherThread,0,0),
        threadTimeOk?1:0,groupNumber,groupMask,ideal,
        InterlockedCompareExchange(&g_v088RenderProcessor,0,0),GetCurrentProcessorNumber(),
        affinityOk?1:0,static_cast<unsigned long long>(processMask),
        static_cast<unsigned long long>(systemMask),
        InterlockedCompareExchange(&g_mpRenderPriorityAttempted,0,0),
        InterlockedCompareExchange(&g_v088AppliedPriorityOld,0,0),
        InterlockedCompareExchange(&g_v088AppliedPriorityNew,0,0),
        InterlockedCompareExchange(&g_v088AppliedPrioritySuccess,0,0));
    if (samples) {
        const LONG64 hook=InterlockedCompareExchange64(&g_v088HookTicks,0,0);
        const LONG64 settings=InterlockedCompareExchange64(&g_v088SettingsTicks,0,0);
        const LONG64 ngx=InterlockedCompareExchange64(&g_v088NgxTicks,0,0);
        const LONG64 composite=InterlockedCompareExchange64(&g_v088CompositeTicks,0,0);
        const LONG flags=InterlockedCompareExchange(&g_v088Flags,0,0);
        Log(L"V088_FRAME elapsed_ms=%lu samples=%lld hook_us=%.2f settings_us=%.2f "
            L"ngx_attempt_us=%.2f original_composite_us=%.2f state=%ld "
            L"flags=0x%lX handoff_streak=%ld allocated_mode=%ld active_mode=%ld "
            L"preset=%ld sampled_cpu=%ld last_eval_count=%lld",
            elapsed,samples,hook*usPerTick,settings*usPerTick,ngx*usPerTick,
            composite*usPerTick,InterlockedCompareExchange(&g_v088State,0,0),
            flags,InterlockedCompareExchange(&g_v088HandoffStreak,0,0),
            InterlockedCompareExchange(&g_v088AllocatedMode,0,0),
            InterlockedCompareExchange(&g_v088ActiveMode,0,0),
            InterlockedCompareExchange(&g_v088Preset,0,0),
            InterlockedCompareExchange(&g_v088RenderProcessor,0,0),evals);
    }
}

// v0.90: 1 Hz worker-side report of the sampled hot VirtualQuery API cost.
// Rates are approximate (counts published every 256 queries per caller thread).
static void V090VirtualQueryPoll() {
    if (!g_v088DiagnosticsEnabled || !g_v090VirtualQueryProbe) return;
    const DWORD started = static_cast<DWORD>(InterlockedCompareExchange(
        &g_v088FirstRenderTick,0,0));
    if (!started) return;
    const DWORD now = GetTickCount();
    const DWORD elapsed = DWORD(now-started);
    if (elapsed > g_v088DurationSeconds*1000u) return;
    static DWORD lastMs=0;
    static LONG64 lastCount=0, lastSamples=0, lastTicks=0;
    static LONG64 lastPass=0, lastFail=0, lastDecompress=0, lastSSFX=0;
    static LONG64 lastDecompressOriginalSamples=0, lastDecompressOriginalTicks=0;
    static LONG64 lastSsfxOriginalSamples=0, lastSsfxOriginalTicks=0;
    if (lastMs && DWORD(now-lastMs)<1000u) return;
    const DWORD span=lastMs?DWORD(now-lastMs):0;
    lastMs=now;
    const LONG64 count=InterlockedCompareExchange64(&g_v090VirtualQueriesBatched,0,0);
    const LONG64 samples=InterlockedCompareExchange64(&g_v090VirtualQuerySamples,0,0);
    const LONG64 ticks=InterlockedCompareExchange64(&g_v090VirtualQueryTicks,0,0);
    const LONG64 pass=InterlockedCompareExchange64(&g_v090WritablePassSamples,0,0);
    const LONG64 fail=InterlockedCompareExchange64(&g_v090WritableFailSamples,0,0);
    const LONG64 decompress=InterlockedCompareExchange64(&g_decompressCalls,0,0);
    const LONG64 ssfx=InterlockedCompareExchange64(&g_ssfxCalls,0,0);
    const LONG64 decompressSamples=InterlockedCompareExchange64(
        &g_v090DecompressOriginalSamples,0,0);
    const LONG64 decompressTicks=InterlockedCompareExchange64(
        &g_v090DecompressOriginalTicks,0,0);
    const LONG64 ssfxSamples=InterlockedCompareExchange64(
        &g_v090SsfxOriginalSamples,0,0);
    const LONG64 ssfxTicks=InterlockedCompareExchange64(
        &g_v090SsfxOriginalTicks,0,0);
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const LONG64 ds=samples-lastSamples;
    const double queryUs=ds>0 && frequency.QuadPart>0 ?
        double(ticks-lastTicks)*1000000.0/(double(frequency.QuadPart)*double(ds)) : -1.0;
    const double rate=span?double(count-lastCount)*1000.0/double(span):-1.0;
    const double estimatedCpuMsPerSecond=(rate>=0 && queryUs>=0)? rate*queryUs/1000.0:-1.0;
    Log(L"V090_VIRTUAL_QUERY elapsed_ms=%lu interval_ms=%lu approx_calls_per_s=%.1f "
        L"samples=%lld sampled_query_us=%.3f estimated_query_wall_ms_per_s=%.2f "
        L"sampled_writable_pass=%lld sampled_writable_fail=%lld "
        L"decompress_calls_per_s=%.1f ssfx_calls_per_s=%.1f "
        L"cache_enabled=0 protection_validation_unchanged=1",
        elapsed,span,rate,ds,queryUs,estimatedCpuMsPerSecond,pass-lastPass,fail-lastFail,
        span?double(decompress-lastDecompress)*1000.0/double(span):-1.0,
        span?double(ssfx-lastSSFX)*1000.0/double(span):-1.0);
    lastCount=count;lastSamples=samples;lastTicks=ticks;
    const LONG64 dd=decompressSamples-lastDecompressOriginalSamples;
    const LONG64 sd=ssfxSamples-lastSsfxOriginalSamples;
    const double decompressUs=dd>0 && frequency.QuadPart>0 ?
        double(decompressTicks-lastDecompressOriginalTicks)*1000000.0/
        (double(frequency.QuadPart)*double(dd)):-1.0;
    const double ssfxUs=sd>0 && frequency.QuadPart>0 ?
        double(ssfxTicks-lastSsfxOriginalTicks)*1000000.0/
        (double(frequency.QuadPart)*double(sd)):-1.0;
    Log(L"V090_ORIGINAL_ENGINE_HOOK elapsed_ms=%lu original_decompress_samples=%lld "
        L"original_decompress_wall_us=%.3f original_ssfx_samples=%lld "
        L"original_ssfx_wall_us=%.3f sample_every=256 "
        L"original_calls_unchanged=1",
        elapsed,dd,decompressUs,sd,ssfxUs);
    lastDecompressOriginalSamples=decompressSamples;
    lastDecompressOriginalTicks=decompressTicks;
    lastSsfxOriginalSamples=ssfxSamples;
    lastSsfxOriginalTicks=ssfxTicks;
    lastPass=pass;lastFail=fail;lastDecompress=decompress;lastSSFX=ssfx;
}

// v0.91: worker-only verification of actual avoided page-protection checks.
// Each counter is monotonically increasing; rates exclude startup when compared.
static void V091OptimizationPoll() {
    if (!g_v088DiagnosticsEnabled) return;
    const DWORD started = static_cast<DWORD>(InterlockedCompareExchange(
        &g_v088FirstRenderTick, 0, 0));
    if (!started) return;
    const DWORD now = GetTickCount();
    const DWORD elapsed = DWORD(now - started);
    if (elapsed > g_v088DurationSeconds * 1000u) return;
    static DWORD lastMs = 0;
    static LONG64 last[7]{};
    if (lastMs && DWORD(now - lastMs) < 1000u) return;
    const DWORD span = lastMs ? DWORD(now - lastMs) : 0;
    lastMs = now;
    const LONG64 value[7] = {
        InterlockedCompareExchange64(&g_v091Candidates,0,0),
        InterlockedCompareExchange64(&g_v091DecompressRejected,0,0),
        InterlockedCompareExchange64(&g_v091SsfxRejected,0,0),
        InterlockedCompareExchange64(&g_v091ReadRejected,0,0),
        InterlockedCompareExchange64(&g_v091Matched,0,0),
        InterlockedCompareExchange64(&g_v091SavedByDefer,0,0),
        InterlockedCompareExchange64(&g_v091SavedByRegion,0,0)
    };
    LONG64 delta[7]{};
    for (int i = 0; i < 7; ++i) {
        delta[i] = value[i] - last[i];
        last[i] = value[i];
    }
    const double scale = span ? 1000.0 / double(span) : 0.0;
    Log(L"V091_HOTPATH elapsed_ms=%lu interval_ms=%lu mode=%d "
        L"candidates_per_s=%.1f rejected_decompress_per_s=%.1f "
        L"rejected_ssfx_per_s=%.1f read_rejects_per_s=%.1f "
        L"matched_per_s=%.1f deferred_skipped_pairs_per_s=%.1f "
        L"same_region_queries_saved_per_s=%.1f "
        L"no_lifetime_cache=1 before_every_write_checks=%d",
        elapsed,span,g_v091ValidationMode,
        double(delta[0])*scale,double(delta[1])*scale,
        double(delta[2])*scale,double(delta[3])*scale,
        double(delta[4])*scale,double(delta[5])*scale,
        double(delta[6])*scale,
        (g_v091ValidationMode!=3 || InterlockedCompareExchange(&g_v0911CheckedFallback,0,0)!=0)?1:0);
    // All counters are sampled on the existing worker. No added hot-path I/O.
    static LONG64 prevDirect=0;
    const LONG64 direct=InterlockedCompareExchange64(&g_v0911DirectWritePairs,0,0);
    const LONG64 readFaults=InterlockedCompareExchange64(&g_v0911ReadFaults,0,0);
    const LONG64 writeFaults=InterlockedCompareExchange64(&g_v0911WriteFaults,0,0);
    Log(L"V0911_FASTPATH elapsed_ms=%lu interval_ms=%lu mode=%d "
        L"direct_pairs_per_s=%.1f read_faults_total=%lld "
        L"write_faults_total=%lld checked_fallback=%ld "
        L"virtualquery_elision_only_in_mode3=1",
        elapsed,span,g_v091ValidationMode,double(direct-prevDirect)*scale,
        readFaults,writeFaults,
        InterlockedCompareExchange(&g_v0911CheckedFallback,0,0));
    prevDirect=direct;
}

static void ReloadSettingsOnRenderThread(bool force = false) {
    if (!g_settingsFile[0]) return;
    const DWORD tick = GetTickCount();
    if (!force && tick - g_lastSettingsCheckTick < 500) return;
    g_lastSettingsCheckTick = tick;
    MpSettingsSnapshot snapshot{};
    if (!force && g_mpBackgroundSettingsRequested &&
        InterlockedCompareExchange(&g_mpBackgroundMarkerRunning, 0, 0) != 0) {
        // Never make the renderer wait for filesystem I/O or worker publication.
        if (!TryAcquireSRWLockShared(&g_mpSettingsLock)) return;
        const LONG generation = g_mpSettingsPublishedGeneration;
        if (generation != g_mpSettingsAppliedGeneration) snapshot = g_mpSettingsPublished;
        ReleaseSRWLockShared(&g_mpSettingsLock);
        if (!generation || generation == g_mpSettingsAppliedGeneration) return;
        g_mpSettingsAppliedGeneration = generation;
        if (g_settingsPresent &&
            CompareFileTime(&snapshot.writeTime, &g_settingsWriteTime) == 0) return;
    } else {
        // Original synchronous fallback when the worker is disabled or unavailable.
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExW(g_settingsFile, GetFileExInfoStandard, &attr)) {
            if (force) Log(L"SETTINGS_NOT_FOUND path=%ls; defaults enabled=1 preset=DEFAULT halton=8", g_settingsFile);
            return;
        }
        if (g_settingsPresent && !force &&
            CompareFileTime(&attr.ftLastWriteTime, &g_settingsWriteTime) == 0) return;
        snapshot = ReadMpSettingsSnapshot(attr.ftLastWriteTime);
    }
    g_settingsPresent = true;
    g_settingsWriteTime = snapshot.writeTime;
    bool enabled = snapshot.enabled;
    if (g_srScaleReady && !enabled) {
        // With reduced scene targets an unprocessed, low-resolution input is
        // not a valid native display frame. Turning reconstruction off now
        // requires a restart to restore the original native allocations.
        enabled = true;
        Log(L"SR_DISABLE_REQUIRES_RESTART reconstruction_kept_on=1");
    }
    const int preset = snapshot.preset;
    int requestedMode = snapshot.mode;
    if (g_pauseAutoEnabled && g_pauseAutoActive) requestedMode = 0;
    if (!g_srReconfigureAtConstructor && g_srScaleReady && requestedMode != g_srStartupMode) {
        if (!g_srWarnedRestartNeeded) {
            Log(L"SR_RESTART_REQUIRED requested=%ls allocated=%ls; keeping_startup_mode=1",
                ModeName(requestedMode), ModeName(g_srStartupMode));
            g_srWarnedRestartNeeded = true;
        }
        requestedMode = g_srStartupMode;
    } else g_srWarnedRestartNeeded = false;
    const int jitterPercent = snapshot.jitterPercent;
    const int sharpnessPercent = snapshot.sharpnessPercent;
    const bool jitterChange = g_jitterPercent != jitterPercent;
    const int syncMode = snapshot.syncMode;
    const bool syncChange = g_ssfxSyncMode != syncMode;
    const bool sharpnessChange = g_sharpnessPercent != sharpnessPercent;
    const bool presetChange = g_preset != preset;
    const bool modeChange = g_requestedMode != requestedMode;
    const bool enabledChange = g_autoEnable != enabled;
    if (presetChange || modeChange || enabledChange || jitterChange || sharpnessChange || syncChange || force) {
        g_autoEnable = enabled;
        g_jitterPercent = jitterPercent;
        g_sharpnessPercent = sharpnessPercent;
        g_ssfxSyncMode = syncMode;
        g_preset = preset;
        g_requestedMode = requestedMode;
        if (presetChange || modeChange || jitterChange || syncChange) {
            // Reset history on the next NGX evaluation; no internal scaling is changed.
            g_resetPending = true;
            g_lastEngineFrame = 0;
            g_lastPatchedFrame = 0;
            g_consecutiveUnmatchedFrames = 0;
            g_previousQpc.QuadPart = 0;
        }
        Log(L"SETTINGS_APPLIED enabled=%d upscaler=%ls preset=%ls halton=%u jitter_percent=%d sharpness_percent=%d sss_sync=%d reset=%d file=%ls",
            enabled ? 1 : 0, ModeName(requestedMode), PresetName(preset), g_haltonPhases,
            g_jitterPercent, g_sharpnessPercent, g_ssfxSyncMode,
            (presetChange || modeChange || jitterChange || syncChange) ? 1 : 0, g_settingsFile);
        if (modeChange) {
            Log(L"SR_MODE_REQUEST mode=%ls activation_requires_engine_render_target_resize=1", ModeName(requestedMode));
            if (g_srReconfigureAtConstructor) {
                Log(L"SR_SWITCH_QUEUED requested=%ls allocated=%ls current_feature=%ls next_action=ENGINE_RENDERER_RECONSTRUCTION",
                    ModeName(requestedMode),ModeName(g_srStartupMode),ModeName(g_activeMode));
                // Do NOT recreate NGX until real scene color/depth/MV dimensions change.
                if (presetChange) RecreateFeatureForPreset();
            } else if (requestedMode == 0 && g_activeMode != 0)
                RecreateFeature(0, g_displayWidth, g_displayHeight);
            else if (presetChange && g_activeMode == 0)
                RecreateFeatureForPreset(); // Keep active DLAA preset in sync while SR is pending.
        } else if (presetChange) RecreateFeatureForPreset();
    }
}

static bool SafeRead(std::uintptr_t address, void* out, SIZE_T size) {
    SIZE_T actual = 0;
    return address && out && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<LPCVOID>(address), out, size, &actual) && actual == size;
}

template<typename T> static T ReadOr(std::uintptr_t address) {
    T value{};
    SafeRead(address, &value, sizeof(value));
    return value;
}

// v0.8.4 CPU hot-path: local guarded reads for the known MP renderer's
// shader-constant metadata and transient engine values. The previous code
// performed multiple ReadProcessMemory(GetCurrentProcess()) kernel calls for
// each shader constant update, ~100 times per rendered frame. This method
// preserves fail-closed access violation handling without crossing the
// ReadProcessMemory API boundary. It does not cache or retain raw pointers.
// WritableRange() and original-vector matching remain mandatory for writes.
static bool MpHotRead(std::uintptr_t address, void* out, SIZE_T count) {
    if (!address || !out || !count) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), count);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template<typename T> static T MpHotReadOr(std::uintptr_t address) {
    T value{};
    MpHotRead(address,&value,sizeof(value));
    return value;
}

static void LogResult(const wchar_t* label, NVSDK_NGX_Result result) {
    Log(L"%ls result=0x%08X success=%d", label, static_cast<unsigned>(result),
        NVSDK_NGX_SUCCEED(result) ? 1 : 0);
}

static bool SetupAdapter(ID3D11Device* device, LUID* outLuid) {
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice),
                                       reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr) || !dxgiDevice) {
        Log(L"DXGI_DEVICE_FAIL hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        Log(L"DXGI_ADAPTER_FAIL hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    DXGI_ADAPTER_DESC desc{};
    hr = adapter->GetDesc(&desc);
    adapter->Release();
    if (FAILED(hr)) {
        Log(L"DXGI_ADAPTER_DESC_FAIL hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    *outLuid = desc.AdapterLuid;
    Log(L"DXGI_ADAPTER name=%ls vendor=0x%04X device=0x%04X vram_mb=%llu",
        desc.Description, desc.VendorId, desc.DeviceId,
        static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)));
    return true;
}

// Query the true NVIDIA driver/model recommendations instead of hardcoding 66.7%,
// 58%, 50%, etc. The caller passes NGX *capability* parameters because those
// contain the optimal-settings callback. Never pass the separate feature params.
// This is a read-only design probe; no new NGX feature is created here.
static void LogOptimalSRModes(NVSDK_NGX_Parameter* capabilities, unsigned outputWidth,
                              unsigned outputHeight) {
    if (!g_srProbeEnabled || !capabilities || !outputWidth || !outputHeight) return;
    struct Mode { const wchar_t* name; NVSDK_NGX_PerfQuality_Value quality; };
    static const Mode modes[] = {
        {L"QUALITY", NVSDK_NGX_PerfQuality_Value_MaxQuality},
        {L"BALANCED", NVSDK_NGX_PerfQuality_Value_Balanced},
        {L"PERFORMANCE", NVSDK_NGX_PerfQuality_Value_MaxPerf},
        {L"ULTRA_PERFORMANCE", NVSDK_NGX_PerfQuality_Value_UltraPerformance}
    };
    Log(L"SR_CONTRACT_QUERY output=%ux%u feature_active=DLAA this_is_not_upscaling", outputWidth, outputHeight);
    for (const Mode& mode : modes) {
        unsigned optimalW=0, optimalH=0, minW=0, minH=0, maxW=0, maxH=0;
        float recommendedSharpness = 0.0f;
        const NVSDK_NGX_Result rc = NGX_DLSS_GET_OPTIMAL_SETTINGS(
            capabilities, outputWidth, outputHeight, mode.quality,
            &optimalW, &optimalH, &maxW, &maxH, &minW, &minH,
            &recommendedSharpness);
        const bool usable = NVSDK_NGX_SUCCEED(rc) && optimalW > 0 && optimalH > 0 &&
            optimalW <= outputWidth && optimalH <= outputHeight &&
            minW <= optimalW && optimalW <= maxW &&
            minH <= optimalH && optimalH <= maxH;
        const int idx = static_cast<int>(&mode - &modes[0]) + 1;
        if (idx >= 1 && idx <= 4) {
            g_srPlans[idx] = { optimalW, optimalH, minW, minH, maxW, maxH, usable };
        }
        Log(L"SR_MODE_PLAN name=%ls result=0x%08X valid=%u input=%ux%u output=%ux%u range_min=%ux%u range_max=%ux%u sdk_sharpness=%.3f",
            mode.name, static_cast<unsigned>(rc), usable ? 1u : 0u,
            optimalW, optimalH, outputWidth, outputHeight, minW, minH,
            maxW, maxH, recommendedSharpness);
    }
    Log(L"SR_CONTRACT mode=%ls resize_experiment=%d input_from_real_engine_targets=1 output_native=1",
        ModeName(g_requestedMode),g_srScaleReady?1:0);
}

// Called once on the already-proven renderer thread, before the original call.
static bool InitializeNgx(ID3D11Device* device, unsigned w, unsigned h) {
    Log(L"NGX_BEGIN device=%p resolution=%ux%u", device, w, h);
    g_displayWidth = w;g_displayHeight = h;
    g_activeRenderWidth = w;g_activeRenderHeight = h;g_activeMode=0;
    LUID adapterLuid{};
    if (!SetupAdapter(device, &adapterLuid)) return false;
    (void)adapterLuid; // adapter details are diagnostic only
    // Preserve the official public API's argument order and ABI.
    // NVIDIA's nvsdk_ngx_d.lib performs the correct NGX core dispatch.
    g_featureInfo.PathListInfo.Path = g_featurePaths;
    g_featureInfo.PathListInfo.Length = 1;
    constexpr const char* projectId = "f87463f9-f431-4ad6-9eaf-1a520602790b";
    Log(L"NGX_INIT_PUBLIC_API_BEGIN sdk=0x%08X featureInfo=%p sizeofFeatureInfo=%zu device=%p",
        static_cast<unsigned>(NVSDK_NGX_Version_API), &g_featureInfo, sizeof(g_featureInfo), device);
    NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(
        projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        // Historical SDK project ID: keep unchanged to avoid altering a known-good NGX session identity.
        "xrRazom-DLAA-Experimental-0.2.6",
        g_cacheDir, device, &g_featureInfo, NVSDK_NGX_Version_API);
    LogResult(L"NGX_INIT", result);
    if (NVSDK_NGX_FAILED(result)) return false;

    NVSDK_NGX_Parameter* capabilities = nullptr;
    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&capabilities);
    LogResult(L"NGX_CAPABILITIES_QUERY", result);
    if (NVSDK_NGX_SUCCEED(result) && capabilities) {
        int available = -1;
        int needsUpdate = -1;
        NVSDK_NGX_Result a = capabilities->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        NVSDK_NGX_Result b = capabilities->Get(
            NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdate);
        Log(L"NGX_DLSS_CAPABILITY available=%d available_result=0x%08X driver_update=%d update_result=0x%08X",
            available, static_cast<unsigned>(a), needsUpdate, static_cast<unsigned>(b));
        LogOptimalSRModes(capabilities, w, h);
        NVSDK_NGX_D3D11_DestroyParameters(capabilities);
        if (NVSDK_NGX_SUCCEED(a) && available == 0) {
            Log(L"NGX_FEATURE_SKIPPED reason=DLSS_not_available");
            return false;
        }
    }

    result = NVSDK_NGX_D3D11_AllocateParameters(&g_params);
    LogResult(L"NGX_ALLOC_PARAMS", result);
    if (NVSDK_NGX_FAILED(result) || !g_params) return false;
    g_params->Set(NVSDK_NGX_Parameter_Width, w);
    g_params->Set(NVSDK_NGX_Parameter_Height, h);
    g_params->Set(NVSDK_NGX_Parameter_OutWidth, w);
    g_params->Set(NVSDK_NGX_Parameter_OutHeight, h);
    g_params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));
    // LDR color; motion is full-size. Do not guess depth-inversion/jitter flags here.
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                  EffectivePresetForMode(0));
    Log(L"NGX_PRESET_INITIAL requested=%ls", PresetName(g_preset));
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, 0);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    g_params->Set(NVSDK_NGX_Parameter_RTXValue, 0);

    size_t scratchBytes = 0;
    result = NVSDK_NGX_D3D11_GetScratchBufferSize(NVSDK_NGX_Feature_SuperSampling, g_params, &scratchBytes);
    Log(L"NGX_SCRATCH_QUERY result=0x%08X size=%llu",
        static_cast<unsigned>(result), static_cast<unsigned long long>(scratchBytes));
    if (NVSDK_NGX_SUCCEED(result) && scratchBytes) {
        // Avoid allocating arbitrary amounts of memory on corrupted SDK output.
        if (scratchBytes > 64ull * 1024 * 1024) {
            Log(L"NGX_SCRATCH_REJECTED unreasonable_size=%llu",
                static_cast<unsigned long long>(scratchBytes));
            return false;
        }
        g_scratch.resize(scratchBytes);
        g_params->Set(NVSDK_NGX_Parameter_Scratch, static_cast<void*>(g_scratch.data()));
        g_params->Set(NVSDK_NGX_Parameter_Scratch_SizeInBytes,
            static_cast<unsigned long long>(scratchBytes));
    }

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (!context) {
        Log(L"D3D11_CONTEXT_MISSING");
        return false;
    }
    Log(L"NGX_FEATURE_CREATE_BEGIN immediate_context=%p width=%u height=%u quality=DLAA flags=0",
        context, w, h);
    result = NVSDK_NGX_D3D11_CreateFeature(context, NVSDK_NGX_Feature_SuperSampling, g_params, &g_feature);
    Log(L"NGX_FEATURE_CREATE result=0x%08X handle=%p",
        static_cast<unsigned>(result), g_feature);
    if (NVSDK_NGX_FAILED(result) || !g_feature) {
        context->Release();
        return false;
    }
    // Retain original device/context until process exit. This DLL is not hot-unloadable.
    device->AddRef();
    g_ngxDevice = device;
    g_ngxContext = context;
    Log(L"NGX_READY feature=SuperSampling quality=DLAA dims=%ux%u EVALUATION_READY_MENU_SETTINGS", w, h);
    V083LogNvidiaRuntime();
    return true;
}

static void TryHandshake(void* renderer) {
    if (!renderer || g_state) return;
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(renderer);
    const auto wrapper = MpHotReadOr<std::uintptr_t>(address + kColorSlot);
    const auto texturePtr = wrapper ? MpHotReadOr<std::uintptr_t>(wrapper + kTextureInWrapper) : 0;
    if (!texturePtr) return; // delay until renderer resources are allocated
    ID3D11Texture2D* color = reinterpret_cast<ID3D11Texture2D*>(texturePtr);
    ID3D11Device* device = nullptr;
    color->GetDevice(&device);
    if (!device) return;
    ID3D11DeviceContext* ctx = nullptr;
    device->GetImmediateContext(&ctx);
    if (!ctx) { device->Release(); return; }
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* bound = nullptr;
    if (rtv) rtv->GetResource(&bound);
    const bool correctTarget = bound == static_cast<ID3D11Resource*>(color);
    if (bound) bound->Release();
    if (rtv) rtv->Release();
    ctx->Release();
    D3D11_TEXTURE2D_DESC desc{};
    color->GetDesc(&desc);
    if ((!correctTarget && !g_gammaNativeTrialActive) || !desc.Width || !desc.Height || desc.SampleDesc.Count != 1 ||
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
        device->Release();
        return; // don't attempt NGX from an ambiguous rendering phase
    }
    const unsigned physicalW = g_srScaleReady ? g_srDisplayStartupW : desc.Width;
    const unsigned physicalH = g_srScaleReady ? g_srDisplayStartupH : desc.Height;
    g_srNativeWidth = physicalW;
    g_srNativeHeight = physicalH;
    if (g_srScaleReady)
        Log(L"SR_DISPLAY_SPLIT_CONFIRMED scene=%ux%u output=%ux%u startup_mode=%ls",
            desc.Width, desc.Height, physicalW, physicalH, ModeName(g_srStartupMode));
    g_state = -1; // no retries after NGX initialization failures
    if (InitializeNgx(device, physicalW, physicalH)) g_state = 1;
    else Log(L"NGX_INIT_OR_CREATE_FAILED; renderer will continue without NGX");
    device->Release();
}

// Pull an already-initialized renderer resource pointer from its known wrapper slot.
static ID3D11Texture2D* GetTextureFromSlot(void* renderer, std::uintptr_t slot) {
    const auto base = reinterpret_cast<std::uintptr_t>(renderer);
    const auto wrapper = ReadOr<std::uintptr_t>(base + slot);
    const auto tex = wrapper ? ReadOr<std::uintptr_t>(wrapper + kTextureInWrapper) : 0;
    return reinterpret_cast<ID3D11Texture2D*>(tex); // borrowed, never Release()
}

// MP-only SEH-guarded wrapper reads: safe on the exact SHA-256 validated
// multiplayer renderer; GAMMA retains original ReadProcessMemory semantics.
static ID3D11Texture2D* GetMpTextureFromSlot(void* renderer, std::uintptr_t slot) {
    const auto base = reinterpret_cast<std::uintptr_t>(renderer);
    const auto wrapper = MpHotReadOr<std::uintptr_t>(base + slot);
    const auto tex = wrapper ? MpHotReadOr<std::uintptr_t>(wrapper + kTextureInWrapper) : 0;
    return reinterpret_cast<ID3D11Texture2D*>(tex);
}

static unsigned ReadEngineFrame(); // existing engine frame counter helper, defined below
// Per-full-frame summary, not a GPU-state modification. The same immediate
// D3D11 context and known UI caller RVAs were verified in v0.4.9.
static void PauseDiagFlushWindow(const wchar_t* reason) {
    if (!g_pauseDiagWindow.frames) return;
    const auto& w=g_pauseDiagWindow;
    Log(L"SR_PAUSE_DIAG_WINDOW stage=%ls reason=%ls window=%llu frames=%u "
        L"renderer_frames=%u renderer_calls=%llu renderer_absent=%u level_frames=%u "
        L"draw=%llu indexed=%llu ui_quad=%llu ui_text=%llu ui_composite=%llu "
        L"ui_om=%llu output_bind=%llu text54=%llu text84=%llu text_large=%llu "
        L"scissor_nonzero=%llu scissor_zero=%llu scene_vp=%llu native_vp=%llu "
        L"allocated=%ls no_changes=1",
        g_pauseDiagStage,reason,++g_pauseDiagWindowCount,w.frames,
        w.rendererFrames,w.rendererCalls,w.noRendererFrames,w.levelFrames,
        w.draw,w.indexed,w.uiQuad,w.uiText,w.uiComposite,
        w.uiOm,w.uiOutputBind,w.text54,w.text84,w.textLarge,
        w.scissorNonzero,w.scissorZero,w.sceneViewport,w.nativeViewport,
        ModeName(g_srStartupMode));
    g_pauseDiagWindow=PauseDiagWindow{};
}
static void PauseDiagPollMarker() {
    if (!g_pauseDiagMarkerPath[0] || g_pauseDiagFullFrames%5!=0) return;
    HANDLE h=CreateFileW(g_pauseDiagMarkerPath,GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (h==INVALID_HANDLE_VALUE) return;
    char raw[48]{};DWORD read=0;
    const BOOL ok=ReadFile(h,raw,sizeof(raw)-1,&read,nullptr);
    CloseHandle(h);
    if (!ok) return;
    DeleteFileW(g_pauseDiagMarkerPath);
    PauseDiagFlushWindow(L"marker");
    for (unsigned i=0;i<47;++i) {
        const unsigned char c=static_cast<unsigned char>(raw[i]);
        if (!c || c=='\r' || c=='\n') {g_pauseDiagStage[i]=0;break;}
        g_pauseDiagStage[i]=(c>=32 && c<127 && c!='%')?wchar_t(c):L'_';
        g_pauseDiagStage[i+1]=0;
    }
    const auto level=MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva);
    const auto hud=MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva);
    Log(L"SR_PAUSE_DIAG_MARK stage=%ls fullframe=%llu engine_frame=%u "
        L"level=%p hud=%p renderer_calls=%lld allocated=%ls "
        L"candidate=%d source=external_label_no_automatic_reset=1",
        g_pauseDiagStage,g_pauseDiagFullFrames,ReadEngineFrame(),
        reinterpret_cast<void*>(level),reinterpret_cast<void*>(hud),
        InterlockedCompareExchange64(&g_calls,0,0),ModeName(g_srStartupMode),
        g_pauseDiagCandidate?1:0);
}
static void PauseDiagFrameStart() {
    if (!g_pauseTransitionDiagEnabled && !g_pauseAutoEnabled) return;
    ++g_pauseDiagFullFrames;
    if (g_pauseDiagHaveFrame) {
        const LONG64 now=InterlockedCompareExchange64(&g_calls,0,0);
        const LONG64 delta=now>=g_pauseDiagLastRendererCalls?
            now-g_pauseDiagLastRendererCalls:0;
        auto& w=g_pauseDiagWindow;
        const auto& f=g_pauseDiagFrame;
        ++w.frames;
        if (delta) ++w.rendererFrames; else ++w.noRendererFrames;
        w.rendererCalls+=static_cast<unsigned long long>(delta);
        const auto level=MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva);
        const auto hud=MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva);
        if (level&&hud) ++w.levelFrames;
        w.draw+=f.draw;w.indexed+=f.indexed;
        w.uiQuad+=f.uiQuad;w.uiText+=f.uiText;
        w.uiComposite+=f.uiComposite;w.uiOm+=f.uiOm;w.uiOutputBind+=f.uiOutputBind;
        w.text54+=f.text54;w.text84+=f.text84;w.textLarge+=f.textLarge;
        w.scissorNonzero+=f.scissorNonzero;w.scissorZero+=f.scissorZero;
        w.sceneViewport+=f.sceneViewport;w.nativeViewport+=f.nativeViewport;
        // Candidate only: other in-game overlays may share some of the same
        // signature. Labels for inventory/PDA/console are needed to exclude them.
        const bool plausible=level&&hud&&delta==0&&f.uiText>=4&&f.uiComposite!=0;
        if (plausible) {
            if(g_pauseDiagCandidateStreak<1000) ++g_pauseDiagCandidateStreak;
            g_pauseDiagResumeStreak=0;
        } else {
            g_pauseDiagCandidateStreak=0;
            if(g_pauseDiagResumeStreak<1000) ++g_pauseDiagResumeStreak;
        }
        if (!g_pauseDiagCandidate&&g_pauseDiagCandidateStreak==8) {
            g_pauseDiagCandidate=true;
            Log(L"SR_PAUSE_DIAG_CANDIDATE state=ENTER stage=%ls "
                L"fullframe=%llu engine_frame=%u evidence=no_renderer_ui_text_composite "
                L"confirmation_required=1 no_restart=1",
                g_pauseDiagStage,g_pauseDiagFullFrames,ReadEngineFrame());
        } else if(g_pauseDiagCandidate&&g_pauseDiagResumeStreak==3) {
            g_pauseDiagCandidate=false;
            Log(L"SR_PAUSE_DIAG_CANDIDATE state=EXIT stage=%ls "
                L"fullframe=%llu engine_frame=%u evidence=signature_absent "
                L"confirmation_required=1 no_restart=1",
                g_pauseDiagStage,g_pauseDiagFullFrames,ReadEngineFrame());
        }
    }
    g_pauseDiagLastRendererCalls=InterlockedCompareExchange64(&g_calls,0,0);
    g_pauseDiagHaveFrame=true;
    g_pauseDiagFrame=PauseDiagFrame{};
    PauseDiagPollMarker();
    if(g_pauseTransitionDiagEnabled && g_pauseDiagWindow.frames>=120)
        PauseDiagFlushWindow(L"periodic");
    else if(!g_pauseTransitionDiagEnabled && g_pauseDiagWindow.frames>=120)
        g_pauseDiagWindow=PauseDiagWindow{};
}
static void PauseDiagTraceOM(ID3D11DeviceContext* ctx,UINT count,
    ID3D11RenderTargetView* const* targets,void* caller) {
    if ((!g_pauseTransitionDiagEnabled && !g_pauseAutoEnabled) || ctx!=g_ngxContext ||
        GetCurrentThreadId()!=g_renderTid) return;
    const uintptr_t a=reinterpret_cast<uintptr_t>(caller);
    const uintptr_t rva=(a>=g_moduleBase&&a<g_moduleBase+0x2000000)?a-g_moduleBase:0;
    if(rva==0x2C26DD) ++g_pauseDiagFrame.uiOutputBind;
    if(rva!=0x25B7CE) return;
    ++g_pauseDiagFrame.uiOm;
    // We deliberately do not touch COM resource pointers; the exact UI
    // binding site itself is the signal, independent of texture reallocation.
    (void)count;(void)targets;
}
static void PauseDiagTraceDraw(ID3D11DeviceContext* ctx,void* caller,bool indexed,UINT count) {
    if ((!g_pauseTransitionDiagEnabled && !g_pauseAutoEnabled) || ctx!=g_ngxContext ||
        GetCurrentThreadId()!=g_renderTid) return;
    auto& f=g_pauseDiagFrame;
    if (indexed) ++f.indexed; else ++f.draw;
    const uintptr_t a=reinterpret_cast<uintptr_t>(caller);
    const uintptr_t rva=(a>=g_moduleBase&&a<g_moduleBase+0x2000000)?a-g_moduleBase:0;
    if(indexed&&rva==0x2C2710&&count==6) ++f.uiComposite;
    if(!indexed && rva==0x25B7F6 && count==6) ++f.uiQuad;
    if(!indexed || rva!=0x246161) return;
    ++f.uiText;
    if(count==54) ++f.text54;
    if(count==84) ++f.text84;
    if(count>=128) ++f.textLarge;
    if(!f.sampleBudget) return;
    --f.sampleBudget;
    D3D11_RECT rects{};UINT rn=1;
    ctx->RSGetScissorRects(&rn,&rects);
    if(rn==1 && rects.right>rects.left && rects.bottom>rects.top)
        ++f.scissorNonzero;
    else ++f.scissorZero;
    D3D11_VIEWPORT vp{};UINT vn=1;
    ctx->RSGetViewports(&vn,&vp);
    if(vn!=1) return;
    if(vp.Width==static_cast<float>(g_srStartupW)&&
       vp.Height==static_cast<float>(g_srStartupH)) ++f.sceneViewport;
    if(vp.Width==static_cast<float>(g_srDisplayStartupW)&&
       vp.Height==static_cast<float>(g_srDisplayStartupH)) ++f.nativeViewport;
}

// Called only on the game's full-frame render thread. We do not execute a
// renderer restart here; this merely supplies an acknowledged mode request for
// the already verified RendererConstructorHook when Lua asks the engine to reset.
static void PauseAutoAck(const char* verb,const char* token) {
    if (!g_pauseAutoAck[0]) return;
    HANDLE h=CreateFileW(g_pauseAutoAck,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) {
        Log(L"SR_PAUSE_AUTO_ACK_FAILED error=%lu",GetLastError());return;
    }
    char buffer[144]{};
    int n=sprintf_s(buffer,"%s %s\n",verb,token);
    DWORD written=0;
    if(n>0) WriteFile(h,buffer,static_cast<DWORD>(n),&written,nullptr);
    CloseHandle(h);
}
// All state besides the atomic ready flags is private to the original render
// thread. The worker never consumes, deletes or parses a marker file.
static DWORD WINAPI MpBackgroundMarkerWorker(LPVOID) {
    InterlockedExchange(&g_mpBackgroundMarkerRunning, 1);
    while (InterlockedCompareExchange(&g_mpBackgroundMarkerStop, 0, 0) == 0) {
        if (g_mpBackgroundMarkerRequested && g_pauseAutoEnabled && g_pauseAutoMarker[0] &&
            GetFileAttributesW(g_pauseAutoMarker) != INVALID_FILE_ATTRIBUTES)
            InterlockedExchange(&g_mpBackgroundPauseDetected, 1);
        if (g_mpBackgroundMarkerRequested && g_mpNativeMenusEnabled && g_mpBootstrapAckPath[0] &&
            GetFileAttributesW(g_mpBootstrapAckPath) != INVALID_FILE_ATTRIBUTES)
            InterlockedExchange(&g_mpBackgroundBootstrapDetected, 1);
        MpReadSettingsOnWorker(); // INI/stat only; never apply to renderer here
        V088StartupDiagnosticPoll(); // 1 Hz OS timing and atomic-only renderer snapshots
        V089ThreadCensusPoll(); // 2-second read-only per-thread CPU sampling on worker
        V090VirtualQueryPoll(); // 1 Hz sampled VirtualQuery cost, worker-only log
        V091OptimizationPoll(); // 1 Hz actual skipped-check counters, worker-only
        Sleep(12); // event-to-render-thread latency normally <= one or two frames
    }
    InterlockedExchange(&g_mpBackgroundMarkerRunning, 0);
    return 0;
}

// Worker failure and race fallback: the previous synchronous polling behavior
// remains intact when the experiment is disabled or the worker does not run.
// The once-per-second fallback also catches missed notifications while enabled.
static bool MpShouldProbeMarker(volatile LONG* noticed, DWORD& fallbackTick) {
    if (!g_mpBackgroundMarkerRequested ||
        InterlockedCompareExchange(&g_mpBackgroundMarkerRunning, 0, 0) == 0)
        return true;
    if (InterlockedExchange(noticed, 0) != 0) return true;
    const DWORD now = GetTickCount();
    if (now - fallbackTick >= 1000u) { fallbackTick = now; return true; }
    return false;
}

static void PauseAutoPoll() {
    if(!g_pauseAutoEnabled || !g_pauseAutoMarker[0])return;
    // A background watcher handles failed file-open probes (the usual case).
    // Keep all marker reading, deletion and pause state updates on this thread.
    static DWORD lastFallbackTick = 0;
    if (MpShouldProbeMarker(&g_mpBackgroundPauseDetected, lastFallbackTick)) {
    HANDLE h=CreateFileW(g_pauseAutoMarker,GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h!=INVALID_HANDLE_VALUE) {
        char data[96]{};DWORD n=0;
        const BOOL ok=ReadFile(h,data,sizeof(data)-1,&n,nullptr);
        CloseHandle(h);
        if(ok && n>0 && DeleteFileW(g_pauseAutoMarker)) {
            // Exactly one outstanding request, most recent wins.
            strcpy_s(g_pauseAutoPending,data);
            for (char &c : g_pauseAutoPending) {
                if(c=='\r'||c=='\n') {c=0;break;}
            }
            g_pauseAutoPendingTick=GetTickCount();
            Log(L"SR_PAUSE_AUTO_RECEIVED request=%hs candidate=%d allocated=%ls",
                g_pauseAutoPending,g_pauseDiagCandidate?1:0,ModeName(g_srStartupMode));
        }
    }
    } // end conditional marker file probe, not the pause state machine
    const auto level=MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva);
    const auto hud=MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva);
    if(!level || !hud) ++g_pauseAutoNoLevelFrames;
    else g_pauseAutoNoLevelFrames=0;
    // Main menu/loading: clear the in-memory override, without a renderer reset.
    // New gameplay will use the user's saved selection at the next constructor.
    if(g_pauseAutoNoLevelFrames==12 && g_pauseAutoActive) {
        g_pauseAutoActive=false;
        Log(L"SR_PAUSE_AUTO_CLEAR reason=NO_LEVEL saved_settings_unchanged=1");
    }
    if(!g_pauseAutoPending[0])return;
    char verb[24]{}, token[64]{};
    if(sscanf_s(g_pauseAutoPending,"%23s %63s",verb,(unsigned)_countof(verb),
        token,(unsigned)_countof(token))!=2) {
        g_pauseAutoPending[0]=0;return;
    }
    if(_stricmp(verb,"ready")==0) {
        g_pauseAutoPending[0]=0;
        PauseAutoAck("ready",token);
        Log(L"SR_PAUSE_AUTO_LUA_READY token=%hs independent_script=1",token);
        return;
    }
    if(_stricmp(verb,"clear")==0) {
        g_pauseAutoActive=false;
        g_pauseAutoPending[0]=0;
        PauseAutoAck("cleared",token);
        Log(L"SR_PAUSE_AUTO_CLEAR reason=LUA_DISCONNECT token=%hs",token);
        return;
    }
    if(_stricmp(verb,"restore")==0) {
        const bool wasActive=g_pauseAutoActive;
        g_pauseAutoActive=false;
        wchar_t preferred[40]{};
        GetPrivateProfileStringW(L"DLAA",L"upscaler_mode",L"dlaa",
            preferred,_countof(preferred),g_settingsFile);
        g_requestedMode=ParseMode(preferred);
        g_pauseAutoPending[0]=0;
        const bool needsRestart=wasActive && g_srStartupMode==0 && g_requestedMode!=0;
        PauseAutoAck(needsRestart?"restore":"noop",token);
        Log(L"SR_PAUSE_AUTO_RESTORE token=%hs preferred=%ls allocated=%ls was_active=%d restart_needed=%d saved_settings_unchanged=1",
            token,ModeName(g_requestedMode),ModeName(g_srStartupMode),wasActive?1:0,needsRestart?1:0);
        return;
    }
    if(_stricmp(verb,"native")!=0) {g_pauseAutoPending[0]=0;return;}
    if(GetTickCount()-g_pauseAutoPendingTick>3000) {
        Log(L"SR_PAUSE_AUTO_REJECT token=%hs reason=NO_CONFIRMED_PAUSE",token);
        g_pauseAutoPending[0]=0;PauseAutoAck("rejected",token);return;
    }
    // Two independent conditions: menu Lua callback and 8-frame D3D signature.
    // Reject regular PDA/inventory use and postpone until the actual pause path.
    if(!g_pauseDiagCandidate || !level || !hud)return;
    if(g_srStartupMode==0) {
        // A rapid resume/re-pause can cancel a pending restore without a
        // redundant renderer rebuild. Preserve native state for this menu.
        g_pauseAutoActive=true;
        g_requestedMode=0;
        g_pauseAutoPending[0]=0;
        PauseAutoAck("already_native",token);
        Log(L"SR_PAUSE_AUTO_NATIVE_ALREADY_ALLOCATED token=%hs",token);
        return;
    }
    if(g_pauseAutoActive) {
        g_pauseAutoPending[0]=0;PauseAutoAck("noop",token);return;
    }
    if(!g_srReconfigureAtConstructor || !g_srScaleOptIn ||
       InterlockedCompareExchange(&g_srConstructorBusy,0,0)!=0) {
        g_pauseAutoPending[0]=0;PauseAutoAck("rejected",token);return;
    }
    g_pauseAutoActive=true;
    g_requestedMode=0; // DLAA: native 1920x1080 on next engine reconstruction.
    g_pauseAutoPending[0]=0;
    PauseAutoAck("native",token);
    Log(L"SR_PAUSE_AUTO_ARMED token=%hs allocated=%ls requested=DLAA verified_pause=1 saved_settings_unchanged=1",
        token,ModeName(g_srStartupMode));
}

static SrMenuActivitySnapshot CaptureMenuActivity() {
    SrMenuActivitySnapshot s{};
    s.fullFrames=InterlockedCompareExchange64(&g_srDiagFullFrames,0,0);
    s.renderer=InterlockedCompareExchange64(&g_calls,0,0);
    s.om=InterlockedCompareExchange64(&g_srDiagOmBinds,0,0);
    s.draw=InterlockedCompareExchange64(&g_srDiagDraw,0,0);
    s.indexed=InterlockedCompareExchange64(&g_srDiagDrawIndexed,0,0);
    s.spriteDraw=InterlockedCompareExchange64(&g_srDiagSpriteDraw,0,0);
    s.spriteIndexed=InterlockedCompareExchange64(&g_srDiagSpriteIndexed,0,0);
    return s;
}
static void LogMenuActivityInterval(bool requestOn,bool accepted) {
    if (!g_srMenuActivityDiagEnabled) return;
    const auto now=CaptureMenuActivity();
    if (requestOn && accepted) {
        g_srDiagAtOn=now;
        g_srDiagIntervalOpen=true;
        InterlockedExchange(&g_srDiagSpriteTraceCount,0);
        Log(L"SR_MENU_ACTIVITY_START fullframe=%lld renderer=%lld om=%lld draw=%lld "
            L"indexed=%lld sprite_draw=%lld sprite_indexed=%lld",
            now.fullFrames,now.renderer,now.om,now.draw,now.indexed,
            now.spriteDraw,now.spriteIndexed);
    } else if (!requestOn) {
        if (!g_srDiagIntervalOpen) {
            Log(L"SR_MENU_ACTIVITY_END no_accepted_on_marker=1 "
                L"fullframe=%lld renderer=%lld om=%lld draw=%lld indexed=%lld",
                now.fullFrames,now.renderer,now.om,now.draw,now.indexed);
            return;
        }
        const auto& a=g_srDiagAtOn;
        Log(L"SR_MENU_ACTIVITY_END fullframe_delta=%lld renderer_delta=%lld "
            L"om_delta=%lld draw_delta=%lld indexed_delta=%lld "
            L"sprite_draw_delta=%lld sprite_indexed_delta=%lld "
            L"renderer_hook_bypassed_during_pause=%d original_targets_unchanged=1",
            now.fullFrames-a.fullFrames,now.renderer-a.renderer,now.om-a.om,
            now.draw-a.draw,now.indexed-a.indexed,
            now.spriteDraw-a.spriteDraw,now.spriteIndexed-a.spriteIndexed,
            now.renderer==a.renderer?1:0);
        g_srDiagIntervalOpen=false;
    }
}

// No ownership changes to the engine's generic0/1 targets. On first use,
// duplicate ONLY their texture descriptions into private native RT/SRV pairs.
static void ReleasePauseSidecars() {
    g_srPauseSidecarSources[0]=nullptr;
    g_srPauseSidecarSources[1]=nullptr;
    g_srPauseSidecarCtorGeneration=-1;
    for (auto& t:g_srPauseSidecar) {
        if (t.srv) { t.srv->Release(); t.srv=nullptr; }
        if (t.rtv) { t.rtv->Release(); t.rtv=nullptr; }
        if (t.texture) { t.texture->Release(); t.texture=nullptr; }
        t.touchedFrame=-1;
    }
}
static bool EnsurePauseSidecars(ID3D11DeviceContext* ctx) {
    if (!ctx || !g_ngxDevice || !g_uiPassKnownGeneric0 || !g_uiPassKnownGeneric1 ||
        !g_srDisplayStartupW || !g_srDisplayStartupH) return false;
    if (g_srPauseSidecar[0].srv && g_srPauseSidecar[1].srv &&
        g_srPauseSidecarCtorGeneration==InterlockedCompareExchange64(&g_srConstructorCount,0,0) &&
        g_srPauseSidecarSources[0]==g_uiPassKnownGeneric0 &&
        g_srPauseSidecarSources[1]==g_uiPassKnownGeneric1) return true;
    ReleasePauseSidecars();
    ID3D11Texture2D* sources[2]={g_uiPassKnownGeneric0,g_uiPassKnownGeneric1};
    for (int i=0;i<2;++i) {
        D3D11_TEXTURE2D_DESC desc{};
        sources[i]->GetDesc(&desc);
        if (desc.Width!=g_srStartupW || desc.Height!=g_srStartupH ||
            desc.SampleDesc.Count!=1 || desc.ArraySize!=1 ||
            desc.MipLevels!=1 || desc.Usage!=D3D11_USAGE_DEFAULT) break;
        desc.Width=g_srDisplayStartupW;
        desc.Height=g_srDisplayStartupH;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags=0;
        desc.CPUAccessFlags=0;
        const HRESULT hr=g_ngxDevice->CreateTexture2D(&desc,nullptr,&g_srPauseSidecar[i].texture);
        if (FAILED(hr) || !g_srPauseSidecar[i].texture) break;
        if (FAILED(g_ngxDevice->CreateRenderTargetView(g_srPauseSidecar[i].texture,nullptr,
            &g_srPauseSidecar[i].rtv))) break;
        if (FAILED(g_ngxDevice->CreateShaderResourceView(g_srPauseSidecar[i].texture,nullptr,
            &g_srPauseSidecar[i].srv))) break;
    }
    if (!g_srPauseSidecar[0].srv || !g_srPauseSidecar[1].srv) {
        ReleasePauseSidecars();
        if (g_srPauseSidecarFailureLogs++<4)
            Log(L"SR_PAUSE_SIDECAR_CREATE_FAILED private_targets_only=1 original_generic_unchanged=1");
        InterlockedExchange(&g_srPauseSidecarArmed,0);
        return false;
    }
    g_srPauseSidecarSources[0]=g_uiPassKnownGeneric0;
    g_srPauseSidecarSources[1]=g_uiPassKnownGeneric1;
    g_srPauseSidecarCtorGeneration=InterlockedCompareExchange64(&g_srConstructorCount,0,0);
    Log(L"SR_PAUSE_SIDECAR_CREATED scene=%ux%u private=%ux%u original_generic_unchanged=1",
        g_srStartupW,g_srStartupH,g_srDisplayStartupW,g_srDisplayStartupH);
    return true;
}
static bool PauseSidecarGuard(ID3D11DeviceContext* ctx) {
    return g_srPauseSidecarEnabled &&
        InterlockedCompareExchange(&g_srPauseSidecarArmed,0,0)!=0 &&
        g_srScaleReady && g_srConstructorSawAll && g_srStartupMode!=0 &&
        g_ngxContext && ctx==g_ngxContext && g_ngxDevice &&
        g_originalPSSetShaderResources && g_uiPassDrawHooksReady &&
        GetCurrentThreadId()==g_renderTid &&
        InterlockedCompareExchange(&g_srFrameInside,0,0)!=0 &&
        MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva)!=0 &&
        MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva)!=0;
}
static std::uintptr_t PauseSidecarCallerRva(void* caller) {
    const auto a=reinterpret_cast<std::uintptr_t>(caller);
    return a>=g_moduleBase && a<g_moduleBase+0x2000000?a-g_moduleBase:0;
}

// A trace is explicitly requested by writing %LOCALAPPDATA%\anomaly_ngx\ui_trace.trigger.
// The marker is consumed at the next frame entry, including no-level menu frames.
static void PollUIPassTraceMarker(bool force=false) {
    if ((!g_uiPassTraceEnabled && !g_srPauseUiDrawProbeEnabled && !g_srMenuHandoffBypassEnabled && !g_srPauseSidecarEnabled) || !g_uiPassMarkerPath[0]) return;
    if (!force && g_uiPassMarkerPoll++ % 10 != 0) return;
    HANDLE h = CreateFileW(g_uiPassMarkerPath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (h==INVALID_HANDLE_VALUE) return;
    char label[64]{}; DWORD n=0;
    const BOOL read=ReadFile(h,label,sizeof(label)-1,&n,nullptr);
    CloseHandle(h);
    if (!read) return;
    DeleteFileW(g_uiPassMarkerPath);
    unsigned k=0;
    while(k<n && k<sizeof(label)-1 && label[k]!='\r' && label[k]!='\n') ++k;
    label[k]=0;
    if (std::strcmp(label,"menu_bypass_on")==0 || std::strcmp(label,"menu_bypass_off")==0) {
        const bool requestOn=std::strcmp(label,"menu_bypass_on")==0;
        const std::uintptr_t activeLevel=MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva);
        const std::uintptr_t hud=MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva);
        const bool allowed = requestOn && g_srMenuHandoffBypassEnabled &&
            g_srScaleReady && g_srConstructorSawAll && g_srStartupMode!=0 &&
            activeLevel!=0 && hud!=0;
        const LONG old=InterlockedExchange(&g_srMenuHandoffBypassArmed,allowed?1:0);
        Log(L"SR_MENU_BYPASS_CONTROL request=%ls armed=%d old=%ld enabled=%d "
            L"level=%p hud=%p allocated=%ls input=%ux%u output=%ux%u "
            L"effect=SKIP_NGX_AND_DIRECT_HANDOFF no_resource_resize=1",
            requestOn?L"ON":L"OFF",allowed?1:0,old,g_srMenuHandoffBypassEnabled?1:0,
            reinterpret_cast<void*>(activeLevel),reinterpret_cast<void*>(hud),
            ModeName(g_srStartupMode),g_srStartupW,g_srStartupH,g_srDisplayStartupW,g_srDisplayStartupH);
        if(requestOn && !allowed)
            Log(L"SR_MENU_BYPASS_REFUSED reason=NO_ACTIVE_LEVEL_OR_SCALED_RENDERER_OR_DISABLED");
        // A CONTROL record confirms command delivery only. The interval
        // counters below establish whether the composite hook actually ran.
        LogMenuActivityInterval(requestOn,allowed);
        return;
    }
    if (std::strcmp(label,"pause_sidecar_native")==0 ||
        std::strcmp(label,"pause_sidecar_scene")==0 ||
        std::strcmp(label,"pause_sidecar_off")==0) {
        const bool requestOn=std::strcmp(label,"pause_sidecar_off")!=0;
        const bool native=std::strcmp(label,"pause_sidecar_native")==0;
        const bool allowed=requestOn && g_srPauseSidecarEnabled && g_srScaleReady &&
            g_srConstructorSawAll && g_srStartupMode!=0 && g_uiPassDrawHooksReady &&
            g_originalPSSetShaderResources && g_ngxDevice &&
            MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva)!=0 &&
            MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva)!=0;
        InterlockedExchange(&g_srPauseSidecarNativeViewport,native?1:0);
        InterlockedExchange(&g_srPauseSidecarArmed,allowed?1:0);
        Log(L"SR_PAUSE_SIDECAR_CONTROL request=%hs armed=%d native_viewport=%d "
            L"draw_hooks=%d scene=%ux%u output=%ux%u om_swaps=%llu composites=%llu "
            L"private_targets=1 no_engine_texture_resize=1",
            label,allowed?1:0,native?1:0,g_uiPassDrawHooksReady?1:0,
            g_srStartupW,g_srStartupH,g_srDisplayStartupW,g_srDisplayStartupH,
            g_srPauseSidecarOmSwaps,g_srPauseSidecarCompositeSwaps);
        return;
    }
    if (std::strcmp(label,"pause_probe_on")==0 || std::strcmp(label,"pause_probe_off")==0) {
        const bool requestOn=std::strcmp(label,"pause_probe_on")==0;
        const bool allow=requestOn && g_srPauseUiDrawProbeEnabled && g_srScaleReady &&
            g_srStartupMode!=0 && g_uiPassDrawHooksReady;
        InterlockedExchange(&g_srPauseUiDrawArmed,allow?1:0);
        Log(L"SR_PAUSE_UI_PROBE_CONTROL requested=%ls armed=%d enabled=%d "
            L"draw_hooks=%d mode=%ls scene=%ux%u output=%ux%u applied=%llu",
            requestOn?L"ON":L"OFF",allow?1:0,g_srPauseUiDrawProbeEnabled?1:0,
            g_uiPassDrawHooksReady?1:0,ModeName(g_srStartupMode),
            g_srStartupW,g_srStartupH,g_srDisplayStartupW,g_srDisplayStartupH,
            g_srPauseUiDrawApplied);
        return;
    }
    for (unsigned i=0;i<63;++i) {
        const unsigned char c=static_cast<unsigned char>(label[i]);
        if (!c) {g_uiPassLabel[i]=0;break;}
        g_uiPassLabel[i] = (c>=32 && c<127 && c!='%')?wchar_t(c):L'_';
        g_uiPassLabel[i+1]=0;
    }
    InterlockedExchange(&g_uiPassRenderBudget,120);
    InterlockedExchange(&g_uiPassViewportBudget,120);
    InterlockedExchange(&g_uiPassDrawBudget,180);
    InterlockedExchange(&g_uiPassRasterBudget,32);
    Log(L"SR_UI_TRACE_BEGIN label=%ls hooks_ready=%d draw_hooks=%d level=%p hud=%p internal=%ux%u output=%ux%u allocated=%ls requested=%ls active=%ls preserved_generic01=%d",
        g_uiPassLabel,g_uiPassHooksReady?1:0,g_uiPassDrawHooksReady?1:0,
        reinterpret_cast<void*>(MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva)),
        reinterpret_cast<void*>(MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva)),
        g_srStartupW,g_srStartupH,g_srDisplayStartupW,g_srDisplayStartupH,
        ModeName(g_srStartupMode),ModeName(g_requestedMode),ModeName(g_activeMode),
        g_srNativeGeneric01OptIn?1:0);
    if(g_srStartupMode==0) Log(L"SR_UI_TRACE_NATIVE_BASELINE label=%ls this_capture_cannot_prove_scaled_menu_failure=1",g_uiPassLabel);
}
static const wchar_t* UITraceTextureName(ID3D11Texture2D* tex) {
    if (!tex) return L"NONE";
    if (tex==g_uiPassKnownUi) return L"$user$ui";
    if (tex==g_uiPassKnownColor) return L"$user$albedo";
    if (tex==g_uiPassKnownGeneric) return L"$user$generic";
    if (tex==g_uiPassKnownGeneric0) return L"$user$generic0";
    if (tex==g_uiPassKnownGeneric1) return L"$user$generic1";
    if (tex==g_uiPassKnownViewport) return L"$user$viewport2";
    if (tex==g_uiPassKnownScope) return L"$user$scopeRT";
    if (tex==g_handoffPresentedTexture) return L"NGX_OUTPUT";
    return L"OTHER";
}
static void TraceUIPassState(ID3D11DeviceContext* ctx,const wchar_t* event,void* callsite) {
    if (!ctx || ctx!=g_ngxContext || GetCurrentThreadId()!=g_renderTid) return;
    ID3D11RenderTargetView* rtv=nullptr;
    ID3D11DepthStencilView* dsv=nullptr;
    ctx->OMGetRenderTargets(1,&rtv,&dsv);
    D3D11_VIEWPORT vp{}; UINT count=1;
    ctx->RSGetViewports(&count,&vp);
    ID3D11Resource* res=nullptr;
    ID3D11Texture2D* tex=nullptr;
    D3D11_TEXTURE2D_DESC d{};
    if (rtv) rtv->GetResource(&res);
    if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&tex))) && tex) tex->GetDesc(&d);
    const std::uintptr_t caller=reinterpret_cast<std::uintptr_t>(callsite);
    const unsigned long long rva=(caller>=g_moduleBase && caller<g_moduleBase+0x2000000)
        ?static_cast<unsigned long long>(caller-g_moduleBase):0;
    const bool menu=MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva)==0;
    const wchar_t* phase=InterlockedCompareExchange(&g_srMenuFrameInside,0,0)
        ?L"NO_LEVEL_FRAME" : InterlockedCompareExchange(&g_srFrameInside,0,0)
        ?L"WORLD_FRAME" : L"OUTSIDE_WORLD_FRAME";
    Log(L"SR_UI_TRACE label=%ls event=%ls phase=%ls no_level=%d target=%ls tex=%p size=%ux%u viewport=%.0f,%.0f:%.0fx%.0f dsv=%p caller_rva=0x%llX",
        g_uiPassLabel,event,phase,menu?1:0,UITraceTextureName(tex),tex,
        d.Width,d.Height,vp.TopLeftX,vp.TopLeftY,count?vp.Width:0.f,count?vp.Height:0.f,
        dsv,rva);
    if (tex) tex->Release();
    if (res) res->Release();
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
}
// Read-only bounded draw trace. The OM/RS trace identified where textures are
// bound, but it could not identify which shader actually draws the menu.
// Never change D3D state here. Track shader identities, first two PS inputs,
// current render target and scissor for the three manually marked scenes.
static void TraceUIPassDraw(ID3D11DeviceContext* ctx,const wchar_t* event,
    void* callsite,UINT verticesOrIndices) {
    if (!ctx || ctx!=g_ngxContext || GetCurrentThreadId()!=g_renderTid) return;
    ID3D11RenderTargetView* rtv=nullptr;
    ctx->OMGetRenderTargets(1,&rtv,nullptr);
    ID3D11Resource* targetRes=nullptr;
    ID3D11Texture2D* targetTex=nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    if(rtv) rtv->GetResource(&targetRes);
    if(targetRes && SUCCEEDED(targetRes->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&targetTex))) && targetTex) targetTex->GetDesc(&desc);
    const wchar_t* targetName=UITraceTextureName(targetTex);
    D3D11_VIEWPORT vp{}; UINT n=1;
    ctx->RSGetViewports(&n,&vp);
    D3D11_RECT scissors{}; UINT scn=1;
    ctx->RSGetScissorRects(&scn,&scissors);
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topology);
    ID3D11PixelShader* ps=nullptr;
    ID3D11VertexShader* vs=nullptr;
    ctx->PSGetShader(&ps,nullptr,nullptr);
    ctx->VSGetShader(&vs,nullptr,nullptr);
    ID3D11ShaderResourceView* srvs[2]{};
    ctx->PSGetShaderResources(0,2,srvs);
    ID3D11Resource* inputRes[2]{};
    ID3D11Texture2D* inputTex[2]{};
    D3D11_TEXTURE2D_DESC inputs[2]{};
    for(unsigned i=0;i<2;++i) {
        if (!srvs[i]) continue;
        srvs[i]->GetResource(&inputRes[i]);
        if (inputRes[i] && SUCCEEDED(inputRes[i]->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&inputTex[i]))) && inputTex[i])
            inputTex[i]->GetDesc(&inputs[i]);
    }
    const uintptr_t caller=reinterpret_cast<uintptr_t>(callsite);
    const unsigned long long rva=(caller>=g_moduleBase && caller<g_moduleBase+0x2000000)
        ?static_cast<unsigned long long>(caller-g_moduleBase):0;
    const wchar_t* phase=InterlockedCompareExchange(&g_srMenuFrameInside,0,0)
        ?L"NO_LEVEL_FRAME":InterlockedCompareExchange(&g_srFrameInside,0,0)
        ?L"WORLD_FRAME":L"OUTSIDE_WORLD_FRAME";
    Log(L"SR_UI_DRAW label=%ls event=%ls phase=%ls count=%u caller_rva=0x%llX topology=%u "
        L"target=%ls size=%ux%u viewport=%.0f,%.0f:%.0fx%.0f scissor=%ld,%ld:%ld,%ld "
        L"ps=%p vs=%p ps0=%ls ps0size=%ux%u ps1=%ls ps1size=%ux%u",
        g_uiPassLabel,event,phase,verticesOrIndices,rva,static_cast<unsigned>(topology),
        targetName,desc.Width,desc.Height,vp.TopLeftX,vp.TopLeftY,n?vp.Width:0.f,n?vp.Height:0.f,
        scn?scissors.left:0L,scn?scissors.top:0L,
        scn?scissors.right:0L,scn?scissors.bottom:0L,
        ps,vs,UITraceTextureName(inputTex[0]),inputs[0].Width,inputs[0].Height,
        UITraceTextureName(inputTex[1]),inputs[1].Width,inputs[1].Height);
    for(unsigned i=0;i<2;++i) {
        if(inputTex[i]) inputTex[i]->Release();
        if(inputRes[i]) inputRes[i]->Release();
        if(srvs[i]) srvs[i]->Release();
    }
    if(ps) ps->Release();
    if(vs) vs->Release();
    if(targetTex) targetTex->Release();
    if(targetRes) targetRes->Release();
    if(rtv) rtv->Release();
}

// The v044 pause trace captured 1920x1080 bound textures with a 1280x720
// rasterizer viewport; its main menu was already native. Avoid guessing from
// texture sizes alone: correct only (1) the known generic0/1 menu target,
// (2) a native target bound outside the 3D frame, or (3) explicitly enabled
// higher-risk native targets inside an active world frame. The underlying
// renderer still owns all D3D resources and subsequent viewport changes.
static void ApplyNativeUiRasterIfNeeded(ID3D11DeviceContext* ctx, const wchar_t* event, void* caller) {
    if (!g_srUiNativeRasterEnabled || g_srUiRasterReentry ||
        !g_srScaleReady || !g_srConstructorSawAll ||
        !g_srDisplayStartupW || !g_srDisplayStartupH ||
        !g_srStartupW || !g_srStartupH ||
        ctx != g_ngxContext || GetCurrentThreadId() != g_renderTid ||
        MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva) == 0) return;
    ID3D11RenderTargetView* rtv=nullptr;
    ctx->OMGetRenderTargets(1,&rtv,nullptr);
    if(!rtv) return;
    ID3D11Resource* resource=nullptr;
    rtv->GetResource(&resource); rtv->Release();
    if(!resource) return;
    ID3D11Texture2D* tex=nullptr;
    const HRESULT hr=resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&tex));
    resource->Release();
    if(FAILED(hr) || !tex) return;
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    const bool isGeneric01 = (tex==g_uiPassKnownGeneric0 || tex==g_uiPassKnownGeneric1);
    const bool namedInside = (tex==g_uiPassKnownUi || tex==g_uiPassKnownViewport || tex==g_uiPassKnownScope);
    const bool sceneSidecar = g_srPauseSidecarEnabled &&
        InterlockedCompareExchange(&g_srPauseSidecarArmed,0,0)!=0 &&
        InterlockedCompareExchange(&g_srPauseSidecarNativeViewport,0,0)==0 &&
        (tex==g_srPauseSidecar[0].texture || tex==g_srPauseSidecar[1].texture);
    const wchar_t* targetName=UITraceTextureName(tex);
    const void* targetAddress=tex;
    tex->Release();
    if(sceneSidecar || desc.Width != g_srDisplayStartupW || desc.Height != g_srDisplayStartupH) return;
    D3D11_VIEWPORT vp{}; UINT n=1;
    ctx->RSGetViewports(&n,&vp);
    if(n!=1 || vp.TopLeftX!=0.f || vp.TopLeftY!=0.f ||
       vp.Width != static_cast<float>(g_srStartupW) ||
       vp.Height != static_cast<float>(g_srStartupH)) return;
    const bool inside = InterlockedCompareExchange(&g_srFrameInside,0,0)!=0;
    const bool outside = !inside;
    const bool matchGeneric = isGeneric01 && g_srUiRasterGeneric01 && g_srNativeGeneric01OptIn;
    const bool matchOutside = outside && g_srUiRasterOutside;
    const bool matchOther = inside && !isGeneric01 && g_srUiRasterInsideOther &&
        (!g_srUiRasterNamedInsideOnly || namedInside);
    if (!matchGeneric && !matchOutside && !matchOther) {
        ++g_srUiRasterSkips;
        return;
    }
    // Do not reenter our own RSSetViewports hook when changing state.
    g_srUiRasterReentry=true;
    const D3D11_VIEWPORT native{0.f,0.f,
        static_cast<FLOAT>(g_srDisplayStartupW),
        static_cast<FLOAT>(g_srDisplayStartupH),0.f,1.f};
    ctx->RSSetViewports(1,&native);
    if (g_srUiRasterScissor) {
        const D3D11_RECT full{0,0,static_cast<LONG>(g_srDisplayStartupW),
            static_cast<LONG>(g_srDisplayStartupH)};
        ctx->RSSetScissorRects(1,&full);
    }
    g_srUiRasterReentry=false;
    const unsigned long long total=++g_srUiRasterCorrections;
    const bool marked = InterlockedCompareExchange(&g_uiPassRasterBudget,0,0)>0 &&
        InterlockedDecrement(&g_uiPassRasterBudget)>=0;
    if(total<=20 || total%1000==0 || marked) {
        const uintptr_t address=reinterpret_cast<uintptr_t>(caller);
        const unsigned long long rva=(address>=g_moduleBase && address<g_moduleBase+0x2000000)
            ? static_cast<unsigned long long>(address-g_moduleBase) : 0;
        Log(L"SR_UI_NATIVE_RASTER_FIX label=%ls event=%ls scope=%ls target=%ls tex=%p "
            L"before=%ux%u after=%ux%u generic=%d outside=%d "
            L"inside_other=%d scissor=%d caller_rva=0x%llX total=%llu skipped=%llu",
            g_uiPassLabel,event,inside?L"WORLD_FRAME":L"OUTSIDE_WORLD_FRAME",targetName,targetAddress,
            g_srStartupW,g_srStartupH,g_srDisplayStartupW,g_srDisplayStartupH,
            matchGeneric?1:0,matchOutside?1:0,matchOther?1:0,
            g_srUiRasterScissor?1:0,rva,total,g_srUiRasterSkips);
    }
}

// The engine passes its original 720p RTV, but ONLY while a pause marker is
// armed and at the validated 2D draw-pass call site, bind an isolated native
// RTV instead. Never overwrite engine wrapper pointers or original textures.
static ID3D11RenderTargetView* PauseSidecarSelectTarget(ID3D11DeviceContext* ctx,
    UINT count,ID3D11RenderTargetView* const* targets,void* caller) {
    if (!PauseSidecarGuard(ctx) || count!=1 || !targets || !targets[0] ||
        PauseSidecarCallerRva(caller)!=0x25B7CE) return nullptr;
    ID3D11Resource* res=nullptr;
    targets[0]->GetResource(&res);
    if (!res) return nullptr;
    const int index=res==static_cast<ID3D11Resource*>(g_uiPassKnownGeneric0)?0:
        res==static_cast<ID3D11Resource*>(g_uiPassKnownGeneric1)?1:-1;
    res->Release();
    if (index<0 || !EnsurePauseSidecars(ctx)) return nullptr;
    auto& side=g_srPauseSidecar[index];
    if (side.touchedFrame!=g_srFrameHooks) {
        const float clear[4]={0.f,0.f,0.f,0.f};
        ctx->ClearRenderTargetView(side.rtv,clear);
        side.touchedFrame=g_srFrameHooks;
    }
    const auto n=++g_srPauseSidecarOmSwaps;
    if(n<=12 || n%1000==0)
        Log(L"SR_PAUSE_SIDECAR_OM frame=%lld target=generic%d native=%ux%u "
            L"mode=%ls total=%llu original_rt_unchanged=1",
            g_srFrameHooks,index,g_srDisplayStartupW,g_srDisplayStartupH,
            InterlockedCompareExchange(&g_srPauseSidecarNativeViewport,0,0)?L"NATIVE":L"SCENE",n);
    return side.rtv;
}

static void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D11DeviceContext* ctx,UINT count,
    ID3D11RenderTargetView* const* targets,ID3D11DepthStencilView* depth) {
    V083HotHookScope v083Hot(kV083OmTargets);
    PauseDiagTraceOM(ctx,count,targets,_ReturnAddress());
    ID3D11RenderTargetView* const sidecar=PauseSidecarSelectTarget(ctx,count,targets,_ReturnAddress());
    if (g_originalOMSetRenderTargets) {
        if (sidecar) g_originalOMSetRenderTargets(ctx,1,&sidecar,depth);
        else g_originalOMSetRenderTargets(ctx,count,targets,depth);
    }
    if (!g_srUiRasterReentry) ApplyNativeUiRasterIfNeeded(ctx,L"OM_BIND",_ReturnAddress());
    if (g_srMenuActivityDiagEnabled && ctx==g_ngxContext)
        InterlockedIncrement64(&g_srDiagOmBinds);
    // The world frame hook can stop ticking while paused: allow the UI's
    // own render-target binds to consume an externally requested trace marker.
    if ((g_uiPassTraceEnabled || g_srPauseUiDrawProbeEnabled || g_srMenuActivityDiagEnabled || g_srPauseSidecarEnabled) && ctx==g_ngxContext && GetCurrentThreadId()==g_renderTid &&
        (++g_uiPassOMProbePoll % 64)==0) {
        // OM binds continue even in a paused menu with no 3D frame hook.
        PollUIPassTraceMarker(true);
    }
    if (g_uiPassTraceEnabled && InterlockedCompareExchange(&g_uiPassRenderBudget,0,0)>0 &&
        ctx==g_ngxContext && GetCurrentThreadId()==g_renderTid &&
        InterlockedDecrement(&g_uiPassRenderBudget)>=0)
        TraceUIPassState(ctx,L"OM_BIND",_ReturnAddress());
}
static void STDMETHODCALLTYPE HookRSSetViewports(ID3D11DeviceContext* ctx,UINT count,const D3D11_VIEWPORT* vps) {
    V083HotHookScope v083Hot(kV083RsViewports);
    if (g_originalRSSetViewports) g_originalRSSetViewports(ctx,count,vps);
    if (!g_srUiRasterReentry) ApplyNativeUiRasterIfNeeded(ctx,L"RS_VIEWPORT",_ReturnAddress());
    if (g_uiPassTraceEnabled && InterlockedCompareExchange(&g_uiPassViewportBudget,0,0)>0 &&
        ctx==g_ngxContext && GetCurrentThreadId()==g_renderTid &&
        InterlockedDecrement(&g_uiPassViewportBudget)>=0)
        TraceUIPassState(ctx,L"RS_VIEWPORT",_ReturnAddress());
}
// v053 manual UI viewport probe. The exact same sprite draw call sites exist
// in the successful no-level main menu and the low-resolution pause menu:
//   Draw         RVA 0x25B7F6  (UI quad, 6 vertices)
//   DrawIndexed  RVA 0x246161  (text/sprite batch)
// In the main menu they are drawn with the native-sized viewport even though
// generic0/1 remain scene-sized. Mimic ONLY this viewport per draw while armed.
// We restore the prior RS viewport immediately; leave scissor/blend/DS/RTs
// untouched. The manual probe must be explicitly disarmed before resuming.
static bool ScopedPauseUiViewportBegin(ID3D11DeviceContext* ctx, void* caller,
    bool indexed, UINT drawCount, D3D11_VIEWPORT& oldVp) {
    if (!g_srPauseUiDrawProbeEnabled ||
        InterlockedCompareExchange(&g_srPauseUiDrawArmed,0,0)==0 ||
        !g_srScaleReady || !g_srConstructorSawAll || g_srStartupMode==0 ||
        !g_srStartupW || !g_srStartupH || !g_srDisplayStartupW || !g_srDisplayStartupH ||
        ctx!=g_ngxContext || !g_originalRSSetViewports ||
        GetCurrentThreadId()!=g_renderTid ||
        InterlockedCompareExchange(&g_srFrameInside,0,0)==0 ||
        MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva)==0 ||
        !g_uiPassKnownGeneric0 || !g_uiPassKnownGeneric1) return false;
    const std::uintptr_t callsite=reinterpret_cast<std::uintptr_t>(caller);
    const std::uintptr_t rva=(callsite>=g_moduleBase && callsite<g_moduleBase+0x2000000)
        ? callsite-g_moduleBase : 0;
    // Exact known caller addresses prevent touching world geometry/HUD.
    // Index count is variable for the sprite/text batch; Draw is 6 vertices.
    if (!((!indexed && rva==0x25B7F6 && drawCount==6) ||
          (indexed && rva==0x246161))) return false;
    UINT count=1;
    ctx->RSGetViewports(&count,&oldVp);
    if (count!=1 || oldVp.TopLeftX!=0.f || oldVp.TopLeftY!=0.f ||
        oldVp.Width!=static_cast<float>(g_srStartupW) ||
        oldVp.Height!=static_cast<float>(g_srStartupH)) return false;
    ID3D11RenderTargetView* view=nullptr;
    ctx->OMGetRenderTargets(1,&view,nullptr);
    if(!view)return false;
    ID3D11Resource* res=nullptr;
    view->GetResource(&res);
    view->Release();
    if(!res)return false;
    ID3D11Texture2D* tex=nullptr;
    const HRESULT hr=res->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&tex));
    res->Release();
    if(FAILED(hr)||!tex)return false;
    const bool known=tex==g_uiPassKnownGeneric0 || tex==g_uiPassKnownGeneric1;
    D3D11_TEXTURE2D_DESC desc{};
    if(known)tex->GetDesc(&desc);
    const wchar_t* target=tex==g_uiPassKnownGeneric0?L"generic0":L"generic1";
    tex->Release();
    if(!known || desc.Width!=g_srStartupW || desc.Height!=g_srStartupH) return false;
    const D3D11_VIEWPORT native{0.f,0.f,
        static_cast<FLOAT>(g_srDisplayStartupW),static_cast<FLOAT>(g_srDisplayStartupH),
        oldVp.MinDepth,oldVp.MaxDepth};
    // This calls the original vtable function: do not fire our own RS hooks.
    g_originalRSSetViewports(ctx,1,&native);
    ++g_srPauseUiDrawApplied;
    if(g_srPauseUiDrawApplied<=20 || g_srPauseUiDrawApplied%1000==0)
        Log(L"SR_PAUSE_UI_DRAW_VIEWPORT caller_rva=0x%llX target=%ls "
            L"draw_count=%u prior=%ux%u temporary=%ux%u original_restored_after_draw=1 total=%llu",
            static_cast<unsigned long long>(rva),target,drawCount,
            g_srStartupW,g_srStartupH,g_srDisplayStartupW,g_srDisplayStartupH,
            g_srPauseUiDrawApplied);
    return true;
}

// Log up to 16 read-only shader/target snapshots at the two previously
// observed menu-sprite call sites, ONLY while the manual interval is active.
static void TraceMenuActivitySprite(ID3D11DeviceContext* ctx,void* caller,
    bool indexed,UINT count) {
    if (!g_srMenuActivityDiagEnabled || ctx!=g_ngxContext) return;
    if (indexed) InterlockedIncrement64(&g_srDiagDrawIndexed);
    else InterlockedIncrement64(&g_srDiagDraw);
    const uintptr_t a=reinterpret_cast<uintptr_t>(caller);
    const uintptr_t rva=(a>=g_moduleBase && a<g_moduleBase+0x2000000)
        ?a-g_moduleBase:0;
    const bool known=indexed ? rva==0x246161 : rva==0x25B7F6;
    if (!known) return;
    if (indexed) InterlockedIncrement64(&g_srDiagSpriteIndexed);
    else InterlockedIncrement64(&g_srDiagSpriteDraw);
    if (InterlockedCompareExchange(&g_srMenuHandoffBypassArmed,0,0)==0 ||
        GetCurrentThreadId()!=g_renderTid) return;
    const LONG sample=InterlockedIncrement(&g_srDiagSpriteTraceCount);
    if (sample<=16) {
        // Uses the existing read-only UI-draw inspection routine.
        TraceUIPassDraw(ctx,indexed?L"MENU_ACTIVITY_INDEXED":L"MENU_ACTIVITY_DRAW",
            caller,count);
    }
}
struct SrPauseSidecarSavedInputs {
    ID3D11ShaderResourceView* views[2]{}; // PSGetShaderResources owns references
    bool replaced=false;
};
static SrPauseSidecarSavedInputs PauseSidecarCompositeBegin(ID3D11DeviceContext* ctx,
    void* caller,UINT indexCount) {
    SrPauseSidecarSavedInputs saved{};
    if (!PauseSidecarGuard(ctx) || indexCount!=6 ||
        PauseSidecarCallerRva(caller)!=0x2C2710 ||
        !g_srPauseSidecar[0].srv || !g_srPauseSidecar[1].srv ||
        g_srPauseSidecar[0].touchedFrame!=g_srFrameHooks ||
        g_srPauseSidecar[1].touchedFrame!=g_srFrameHooks) return saved;
    // Verify this EXACT composite still consumes both original engine RTs.
    ctx->PSGetShaderResources(0,2,saved.views);
    bool match=true;
    ID3D11Texture2D* expected[2]={g_uiPassKnownGeneric0,g_uiPassKnownGeneric1};
    for(int i=0;i<2;++i) {
        if (!saved.views[i]) {match=false;break;}
        ID3D11Resource* res=nullptr;
        saved.views[i]->GetResource(&res);
        const bool same=res==static_cast<ID3D11Resource*>(expected[i]);
        if(res)res->Release();
        D3D11_SHADER_RESOURCE_VIEW_DESC d{},clone{};
        saved.views[i]->GetDesc(&d);
        g_srPauseSidecar[i].srv->GetDesc(&clone);
        // Fail closed instead of substituting an incompatible view format.
        if(!same || d.Format!=clone.Format || d.ViewDimension!=clone.ViewDimension ||
            d.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
            d.Texture2D.MostDetailedMip!=clone.Texture2D.MostDetailedMip) match=false;
    }
    if (!match) {
        if(g_srPauseSidecarMisses++<10)
            Log(L"SR_PAUSE_SIDECAR_COMPOSITE_SKIP reason=INPUT_RESOURCE_OR_VIEW_MISMATCH "
                L"auto_disarm=1 native_sidecar_not_presented=1");
        InterlockedExchange(&g_srPauseSidecarArmed,0);
        for(auto& v:saved.views)if(v){v->Release();v=nullptr;}
        return saved;
    }
    ID3D11ShaderResourceView* native[2]={g_srPauseSidecar[0].srv,g_srPauseSidecar[1].srv};
    // Call the ORIGINAL PSSet function; do not re-enter DLSS handoff hook.
    g_originalPSSetShaderResources(ctx,0,2,native);
    saved.replaced=true;
    const auto n=++g_srPauseSidecarCompositeSwaps;
    if(n<=12 || n%1000==0)
        Log(L"SR_PAUSE_SIDECAR_COMPOSITE frame=%lld inputs=native_generic0_native_generic1 "
            L"output=%ux%u total=%llu state_restored_after_draw=1",
            g_srFrameHooks,g_srDisplayStartupW,g_srDisplayStartupH,n);
    return saved;
}
static void PauseSidecarCompositeEnd(ID3D11DeviceContext* ctx,SrPauseSidecarSavedInputs& saved) {
    if(saved.replaced && g_originalPSSetShaderResources)
        g_originalPSSetShaderResources(ctx,0,2,saved.views);
    for(auto& v:saved.views)if(v){v->Release();v=nullptr;}
}

static void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* ctx,UINT vertexCount,UINT startVertex) {
    V083HotHookScope v083Hot(kV083Draw);
    PauseDiagTraceDraw(ctx,_ReturnAddress(),false,vertexCount);
    TraceMenuActivitySprite(ctx,_ReturnAddress(),false,vertexCount);
    if(g_uiPassTraceEnabled && InterlockedCompareExchange(&g_uiPassDrawBudget,0,0)>0 &&
       ctx==g_ngxContext && GetCurrentThreadId()==g_renderTid &&
       InterlockedDecrement(&g_uiPassDrawBudget)>=0)
        TraceUIPassDraw(ctx,L"Draw",_ReturnAddress(),vertexCount);
    D3D11_VIEWPORT saved{};
    const bool temporary=ScopedPauseUiViewportBegin(ctx,_ReturnAddress(),false,vertexCount,saved);
    if(g_originalDraw) g_originalDraw(ctx,vertexCount,startVertex);
    if(temporary && g_originalRSSetViewports) g_originalRSSetViewports(ctx,1,&saved);
}
static void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* ctx,UINT indexCount,
    UINT startIndex,INT baseVertex) {
    V083HotHookScope v083Hot(kV083DrawIndexed);
    PauseDiagTraceDraw(ctx,_ReturnAddress(),true,indexCount);
    TraceMenuActivitySprite(ctx,_ReturnAddress(),true,indexCount);
    if(g_uiPassTraceEnabled && InterlockedCompareExchange(&g_uiPassDrawBudget,0,0)>0 &&
       ctx==g_ngxContext && GetCurrentThreadId()==g_renderTid &&
       InterlockedDecrement(&g_uiPassDrawBudget)>=0)
        TraceUIPassDraw(ctx,L"DrawIndexed",_ReturnAddress(),indexCount);
    D3D11_VIEWPORT saved{};
    const bool temporary=ScopedPauseUiViewportBegin(ctx,_ReturnAddress(),true,indexCount,saved);
    auto inputSwap=PauseSidecarCompositeBegin(ctx,_ReturnAddress(),indexCount);
    if(g_originalDrawIndexed) g_originalDrawIndexed(ctx,indexCount,startIndex,baseVertex);
    PauseSidecarCompositeEnd(ctx,inputSwap);
    if(temporary && g_originalRSSetViewports) g_originalRSSetViewports(ctx,1,&saved);
}

static void InstallUIPassTraceHooks() {
    if ((!g_uiPassTraceEnabled && !g_srUiNativeRasterEnabled && !g_srPauseUiDrawProbeEnabled &&
         !g_srMenuActivityDiagEnabled && !g_srPauseSidecarEnabled &&
         !g_pauseTransitionDiagEnabled && !g_pauseAutoEnabled) || g_uiPassHooksAttempted || !g_ngxContext ||
        GetCurrentThreadId()!=g_renderTid) return;
    g_uiPassHooksAttempted=true;
    void** vtable=*reinterpret_cast<void***>(g_ngxContext);
    if (!vtable || !vtable[33] || !vtable[44]) return;
    void* targets[2]={vtable[33],vtable[44]};
    void* detours[2]={reinterpret_cast<void*>(&HookOMSetRenderTargets),
                    reinterpret_cast<void*>(&HookRSSetViewports)};
    void** originals[2]={reinterpret_cast<void**>(&g_originalOMSetRenderTargets),
                       reinterpret_cast<void**>(&g_originalRSSetViewports)};
    bool all=true;
    for(unsigned i=0;i<2;++i) {
        MH_STATUS rc=MH_CreateHook(targets[i],detours[i],originals[i]);
        if (rc==MH_OK) {
            rc=MH_EnableHook(targets[i]);
            if (rc!=MH_OK) MH_RemoveHook(targets[i]);
        }
        if(rc!=MH_OK) all=false;
        Log(L"SR_UI_TRACE_HOOK method=%ls slot=%u installed=%d status=%d",
            i==0?L"OMSetRenderTargets":L"RSSetViewports",i==0?33:44,rc==MH_OK?1:0,rc);
    }
    g_uiPassHooksReady=all;
    // The draw hooks are also the REQUIRED input of the multiplayer pause
    // detector. Turning OFF verbose pause diagnostics must NEVER disable them.
    if(g_uiPassTraceEnabled || g_srPauseUiDrawProbeEnabled || g_srMenuActivityDiagEnabled || g_srPauseSidecarEnabled ||
       g_pauseTransitionDiagEnabled || g_pauseAutoEnabled) {
        void* drawTargets[2]={vtable[12],vtable[13]};
        void* drawDetours[2]={reinterpret_cast<void*>(&HookDrawIndexed),
                              reinterpret_cast<void*>(&HookDraw)};
        void** drawOriginals[2]={reinterpret_cast<void**>(&g_originalDrawIndexed),
                                 reinterpret_cast<void**>(&g_originalDraw)};
        bool drawReady=true;
        for(unsigned i=0;i<2;++i) {
            MH_STATUS rc=drawTargets[i] ? MH_CreateHook(drawTargets[i],drawDetours[i],drawOriginals[i])
                                         : MH_ERROR_NOT_EXECUTABLE;
            if(rc==MH_OK) {
                rc=MH_EnableHook(drawTargets[i]);
                if(rc!=MH_OK) MH_RemoveHook(drawTargets[i]);
            }
            if(rc!=MH_OK) drawReady=false;
            Log(L"SR_UI_DRAW_TRACE_HOOK method=%ls slot=%u installed=%d status=%d read_only=1",
                i==0?L"DrawIndexed":L"Draw",i==0?12:13,rc==MH_OK?1:0,rc);
        }
        g_uiPassDrawHooksReady=drawReady;
        if (g_pauseAutoEnabled)
            Log(L"SR_MP_PAUSE_DETECTOR_HOOK_STATUS installed=%d draw=1 indexed=1 auto_native=1 diagnostics_optional=1",
                drawReady ? 1 : 0);
    }
}

// Non-invasive SR readiness inventory. These wrapper offsets are confirmed for
// the one validated multiplayer EXE profile; no attempt is made to interpret
// them for stock Anomaly. Only GetDesc/GetResource queries; no GPU copies, mapping,
// vtable hooks, viewport changes, or alteration of the existing DLAA pathway.
static void LogSRRendererResources(void* renderer) {
    if (!g_srProbeEnabled || !g_ngxDevice || !renderer || g_srResourcesCaptured) return;
    g_srResourcesCaptured = true;
    struct Slot { std::uintptr_t offset; const wchar_t* name; };
    static const Slot slots[] = {
        {0x438, L"scene_color_albedo"}, {0x448, L"accum_HDR_candidate"},
        {0x668, L"ssfx_motion_vectors"}, {0x728, L"temp_zb_depth"},
        {0x658, L"ssfx_taa"}, {0x660, L"ssfx_prev_frame"},
        {0x498, L"ui"}, {0x428, L"generic"},
        {0x468, L"generic0"}, {0x470, L"generic1"},
        {0x478, L"generic_temp"}, {0x480, L"viewport2"},
        {0x568, L"ssfx"}, {0x570, L"ssfx_temp"},
        {0x578, L"ssfx_temp2"}, {0x580, L"ssfx_temp3"},
        {0x588, L"ssfx_accum"}, {0x590, L"ssfx_ssr"},
        {0x598, L"ssfx_water"}, {0x5A8, L"ssfx_ao"},
        {0x530, L"hdr10_halfres0"}, {0x538, L"hdr10_halfres1"},
        {0x488, L"scopeRT"}, {0x490, L"dof"}
    };
    for (const Slot& entry : slots) {
        ID3D11Texture2D* t = GetMpTextureFromSlot(renderer, entry.offset);
        if (!t) {
            Log(L"SR_SLOT name=%ls offset=0x%llX state=MISSING", entry.name,
                static_cast<unsigned long long>(entry.offset));
            continue;
        }
        D3D11_TEXTURE2D_DESC d{};
        t->GetDesc(&d);
        const bool internalCandidate = d.Width && d.Height &&
            d.Width == g_srNativeWidth && d.Height == g_srNativeHeight;
        Log(L"SR_SLOT name=%ls offset=0x%llX tex=%p size=%ux%u format=%u bind=0x%X samples=%u matches_native=%u",
            entry.name, static_cast<unsigned long long>(entry.offset), t,
            d.Width, d.Height, static_cast<unsigned>(d.Format), d.BindFlags,
            d.SampleDesc.Count, internalCandidate ? 1u : 0u);
    }
}

static void LogSROutputSnapshot(const wchar_t* stage, void* renderer) {
    if (!g_srProbeEnabled || !g_ngxContext) return;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    g_ngxContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_ngxContext->RSGetViewports(&count, viewports);
    Log(L"SR_PIPELINE stage=%ls viewport_count=%u native_candidate=%ux%u",
        stage, count, g_srNativeWidth, g_srNativeHeight);
    for (UINT i = 0; i < count && i < 3; ++i) {
        Log(L"SR_VIEWPORT stage=%ls index=%u x=%.1f y=%.1f width=%.1f height=%.1f depth_min=%.3f depth_max=%.3f",
            stage, i, viewports[i].TopLeftX, viewports[i].TopLeftY,
            viewports[i].Width, viewports[i].Height,
            viewports[i].MinDepth, viewports[i].MaxDepth);
    }
    ID3D11Texture2D* color = GetMpTextureFromSlot(renderer, kColorSlot);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (!rtvs[i]) continue;
        ID3D11Resource* raw = nullptr;
        rtvs[i]->GetResource(&raw);
        ID3D11Texture2D* surface = nullptr;
        if (raw) raw->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&surface));
        if (surface) {
            D3D11_TEXTURE2D_DESC d{};
            surface->GetDesc(&d);
            Log(L"SR_BOUND_RTV stage=%ls index=%u tex=%p size=%ux%u format=%u bind=0x%X is_scene_color=%u",
                stage, i, surface, d.Width, d.Height, static_cast<unsigned>(d.Format),
                d.BindFlags, surface == color ? 1u : 0u);
            surface->Release();
        }
        if (raw) raw->Release();
        rtvs[i]->Release();
    }
    if (dsv) {
        ID3D11Resource* raw = nullptr;
        dsv->GetResource(&raw);
        ID3D11Texture2D* depth = nullptr;
        if (raw) raw->QueryInterface(__uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&depth));
        if (depth) {
            D3D11_TEXTURE2D_DESC d{};
            depth->GetDesc(&d);
            Log(L"SR_BOUND_DSV stage=%ls tex=%p size=%ux%u format=%u bind=0x%X",
                stage, depth, d.Width, d.Height, static_cast<unsigned>(d.Format), d.BindFlags);
            depth->Release();
        }
        if (raw) raw->Release();
        dsv->Release();
    }
}

// Match the real D3D11 rasterizer viewport to the full-resolution composite.
// Writing the engine's width/height globals alone (v0.3.7) is insufficient;
// RSGetViewports continued to report the low-resolution scene viewport.
// Called only on the verified renderer thread and only while actual scaling is active.
static bool ApplyNativeCompositeRasterState(const wchar_t* stage) {
    if (!g_srNativeViewportFix || !g_srScaleReady || !g_srConstructorSawAll ||
        !g_ngxContext || !g_srDisplayStartupW || !g_srDisplayStartupH ||
        !g_renderTid || GetCurrentThreadId() != g_renderTid) return false;
    D3D11_VIEWPORT before[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_ngxContext->RSGetViewports(&count, before);
    const float wasW = count ? before[0].Width : 0.f;
    const float wasH = count ? before[0].Height : 0.f;
    const bool wasNative = count == 1 && before[0].TopLeftX == 0.f &&
        before[0].TopLeftY == 0.f && wasW == float(g_srDisplayStartupW) &&
        wasH == float(g_srDisplayStartupH);
    if (!wasNative) {
        const D3D11_VIEWPORT native = {0.f, 0.f,
            static_cast<FLOAT>(g_srDisplayStartupW),
            static_cast<FLOAT>(g_srDisplayStartupH),0.f,1.f};
        g_ngxContext->RSSetViewports(1,&native);
    }
    if (g_srNativeScissorFix) {
        const D3D11_RECT full = {0, 0,
            static_cast<LONG>(g_srDisplayStartupW),
            static_cast<LONG>(g_srDisplayStartupH)};
        g_ngxContext->RSSetScissorRects(1,&full);
    }
    ++g_srViewportFixCalls;
    if (_wcsicmp(stage,L"AFTER_COMPOSITE") == 0 && !wasNative)
        ++g_srViewportAfterOriginalMismatches;
    if (g_srViewportFixCalls <= 12 || g_srViewportFixCalls % 2400 == 0 ||
        (g_srViewportAfterOriginalMismatches <= 3 &&
         _wcsicmp(stage,L"AFTER_COMPOSITE") == 0 && !wasNative)) {
        Log(L"SR_NATIVE_VIEWPORT_FIX stage=%ls before=%.0fx%.0f after=%ux%u changed=%d "
            L"scissor_native=%d original_reverts=%llu call=%llu",
            stage,wasW,wasH,g_srDisplayStartupW,g_srDisplayStartupH,
            wasNative?0:1,g_srNativeScissorFix?1:0,
            g_srViewportAfterOriginalMismatches,g_srViewportFixCalls);
    }
    return true;
}

static bool InspectTexture(ID3D11Texture2D* texture, const wchar_t* label,
                           D3D11_TEXTURE2D_DESC* out) {
    if (!texture || !out) { Log(L"EVAL_INPUT_MISSING name=%ls ptr=%p", label, texture); return false; }
    texture->GetDesc(out);
    ID3D11Device* owner = nullptr;
    texture->GetDevice(&owner);
    const bool same = owner && owner == g_ngxDevice;
    if (owner) owner->Release();
    Log(L"EVAL_INPUT name=%ls ptr=%p format=%u size=%ux%u samples=%u bind=0x%X same_device=%d",
        label, texture, static_cast<unsigned>(out->Format), out->Width, out->Height,
        out->SampleDesc.Count, out->BindFlags, same ? 1 : 0);
    return same && out->Width && out->Height && out->SampleDesc.Count == 1;
}

static bool EnsureIsolatedContext() {
    if (g_isolatedState && g_context1) return true;
    if (!g_ngxDevice || !g_ngxContext) return false;
    HRESULT h1 = g_ngxDevice->QueryInterface(__uuidof(ID3D11Device1),
                                           reinterpret_cast<void**>(&g_device1));
    HRESULT h2 = g_ngxContext->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                             reinterpret_cast<void**>(&g_context1));
    if (FAILED(h1) || FAILED(h2) || !g_device1 || !g_context1) {
        Log(L"EVAL_CONTEXT1_UNAVAILABLE device_hr=0x%08X context_hr=0x%08X",
            static_cast<unsigned>(h1), static_cast<unsigned>(h2));
        return false;
    }
    const D3D_FEATURE_LEVEL fl = g_ngxDevice->GetFeatureLevel();
    const UINT flags = (g_ngxDevice->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = g_device1->CreateDeviceContextState(
        flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1),
        &selected, &g_isolatedState);
    Log(L"EVAL_CREATE_CONTEXT_STATE hr=0x%08X feature_level=0x%X selected=0x%X state=%p",
        static_cast<unsigned>(hr), static_cast<unsigned>(fl),
        static_cast<unsigned>(selected), g_isolatedState);
    return SUCCEEDED(hr) && g_isolatedState;
}

// The game updates _DAT_14185B8D8 during normal frame processing.
// This is a guard against accidentally evaluating two render passes of the same frame.
// If the counter cannot be read / remains zero, one call per observed render hook is used.
static unsigned ReadEngineFrame() {
    const auto base = g_moduleBase ? g_moduleBase :
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_gammaNativeTrialActive) return ReadOr<unsigned>(base + 0x14D3808);
    return MpHotReadOr<unsigned>(base + kFrameCounterRva);
}

// A per-process named manual-reset event is controlled by anomaly_ngx_control_v053.exe.
// No keyboard state is read, so F9 remains entirely reserved for quickload.
static bool RequestedEnabled() {
    return g_autoEnable && InterlockedCompareExchange(&g_srResetDLAAHold,0,0)==0; // reset probe fails closed
}

static void SyncActivationState() {
    const bool requested = RequestedEnabled() && g_state == 1 && !g_fatalEvaluationError;
    if (requested == g_evaluationEnabled) return;
    g_evaluationEnabled = requested;
    if (requested) {
        g_runFrames = 0;
        g_resetPending = true;
        g_lastEngineFrame = 0;
        g_previousQpc.QuadPart = 0;
        Log(L"EXTERNAL_ON continuous=1 jitter_mode=%ls history_reset=1",
            EffectiveJitter() ? L"HALTON8_CONSTANT" : L"ZERO_FALLBACK");
    } else {
        g_resetPending = true;
        Log(L"EXTERNAL_OFF run_frames=%u total=%llu", g_runFrames, g_totalEvaluations);
    }
}

// Per-process external controls.  Distinct manual-reset events avoid intercepting
// any of the game's own keys (F9 remains exclusively quickload).
static int CurrentGuardMode() { return 0; } // user comparison: guard worsened aliasing

static const char* kGuardComputeHlsl = R"HLSL(
cbuffer GuardParams : register(b0) {
    float diffLow;
    float diffHigh;
    float protectionStrength;
    float reserved0;
};
Texture2D<float4> CurrentColor : register(t0);
Texture2D<float4> NgxColor     : register(t1);
RWTexture2D<float4> GuardedColor : register(u0);
[numthreads(16, 16, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint width, height;
    CurrentColor.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    float4 current = CurrentColor.Load(int3(tid.xy, 0));
    float4 reconstructed = NgxColor.Load(int3(tid.xy, 0));
    float3 difference = abs(current.rgb - reconstructed.rgb);
    float discrepancy = max(difference.r, max(difference.g, difference.b));
    float blend = saturate((discrepancy - diffLow) / max(diffHigh - diffLow, 0.00001f));
    blend = blend * blend * (3.0f - 2.0f * blend);
    // Mitigate temporal disappearance by preserving current-frame thin features.
    // This is NOT a verified vegetation mask or a correction to missing motion vectors.
    float3 rgb = lerp(reconstructed.rgb, current.rgb, blend * protectionStrength);
    GuardedColor[tid.xy] = float4(saturate(rgb), current.a);
}
)HLSL";

struct GuardConstants {
    float low;
    float high;
    float strength;
    float padding;
};
static_assert(sizeof(GuardConstants) == 16, "D3D11 constants must be 16-byte aligned");

static bool CreateGuardResources(ID3D11Texture2D* source, ID3D11Texture2D* ngxOutput) {
    if (g_guardInitAttempted) return g_guardReady;
    g_guardInitAttempted = true;
    if (!source || !ngxOutput || !g_ngxDevice) return false;
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.ArraySize != 1) {
        Log(L"GUARD_UNSUPPORTED_INPUT_FORMAT format=%u", static_cast<unsigned>(desc.Format));
        return false;
    }
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    HRESULT hr = g_ngxDevice->CreateTexture2D(&desc, nullptr, &g_guardTarget);
    if (FAILED(hr) || !g_guardTarget) { Log(L"GUARD_TARGET_FAIL hr=0x%08X", static_cast<unsigned>(hr)); return false; }
    hr = g_ngxDevice->CreateShaderResourceView(source, nullptr, &g_guardColorSrv);
    if (FAILED(hr) || !g_guardColorSrv) { Log(L"GUARD_COLOR_SRV_FAIL hr=0x%08X", static_cast<unsigned>(hr)); return false; }
    hr = g_ngxDevice->CreateShaderResourceView(ngxOutput, nullptr, &g_guardNgxSrv);
    if (FAILED(hr) || !g_guardNgxSrv) { Log(L"GUARD_NGX_SRV_FAIL hr=0x%08X", static_cast<unsigned>(hr)); return false; }
    hr = g_ngxDevice->CreateUnorderedAccessView(g_guardTarget, nullptr, &g_guardUav);
    if (FAILED(hr) || !g_guardUav) { Log(L"GUARD_UAV_FAIL hr=0x%08X", static_cast<unsigned>(hr)); return false; }
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    hr = D3DCompile(kGuardComputeHlsl, strlen(kGuardComputeHlsl), "anomaly_ngx_anti_smear_guard",
                    nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                    0, &blob, &errors);
    if (FAILED(hr) || !blob) {
        if (errors) Log(L"GUARD_HLSL_FAIL hr=0x%08X details=%hs", static_cast<unsigned>(hr),
                        static_cast<const char*>(errors->GetBufferPointer()));
        else Log(L"GUARD_HLSL_FAIL hr=0x%08X", static_cast<unsigned>(hr));
        if (errors) errors->Release();
        if (blob) blob->Release();
        return false;
    }
    if (errors) errors->Release();
    hr = g_ngxDevice->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, &g_guardShader);
    blob->Release();
    if (FAILED(hr) || !g_guardShader) { Log(L"GUARD_SHADER_FAIL hr=0x%08X", static_cast<unsigned>(hr)); return false; }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(GuardConstants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_ngxDevice->CreateBuffer(&cb, nullptr, &g_guardCBuffer);
    if (FAILED(hr) || !g_guardCBuffer) { Log(L"GUARD_CB_FAIL hr=0x%08X", static_cast<unsigned>(hr)); return false; }
    g_guardReady = true;
    Log(L"GUARD_RESOURCES_READY size=%ux%u shader=cs_5_0", desc.Width, desc.Height);
    return true;
}

// Runs within the already-isolated context after successful NGX evaluation.
// Never modifies NGX's own output or temporal history.
static bool RunGuard(int mode, ID3D11Texture2D* input, ID3D11Texture2D* output) {
    if (mode <= 0 || !g_ngxContext || !g_guardReady || !input || !output) return false;
    const GuardConstants constants = mode == 2
        ? GuardConstants{0.025f, 0.14f, 0.95f, 0.f}
        : GuardConstants{0.070f, 0.25f, 0.70f, 0.f};
    g_ngxContext->UpdateSubresource(g_guardCBuffer, 0, nullptr, &constants, 0, 0);
    ID3D11ShaderResourceView* srvs[2] = {g_guardColorSrv, g_guardNgxSrv};
    ID3D11UnorderedAccessView* uavs[1] = {g_guardUav};
    g_ngxContext->CSSetShader(g_guardShader, nullptr, 0);
    g_ngxContext->CSSetConstantBuffers(0, 1, &g_guardCBuffer);
    g_ngxContext->CSSetShaderResources(0, 2, srvs);
    g_ngxContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    D3D11_TEXTURE2D_DESC d{};
    input->GetDesc(&d);
    g_ngxContext->Dispatch((d.Width + 15) / 16, (d.Height + 15) / 16, 1);
    g_ngxContext->ClearState(); // avoid UAV/SRV CopyResource hazards
    return true;
}

// Optional post-NGX SPATIAL sharpening. The modern NGX sharpness parameter is
// deprecated/unsupported, so do NOT claim this is an NGX feature. Default 0%.
// This extra pass operates only on the produced DLAA output in the already
// isolated context; it cannot repair invalid motion vectors or alpha coverage.
static ID3D11ComputeShader* g_sharpShader = nullptr;
static ID3D11ShaderResourceView* g_sharpInputSrv = nullptr;
static ID3D11UnorderedAccessView* g_sharpOutputUav = nullptr;
static ID3D11Buffer* g_sharpParams = nullptr;
static ID3D11Texture2D* g_sharpTarget = nullptr;
static bool g_sharpInitAttempted = false;
static bool g_sharpReady = false;
static bool g_sharpPassLogged = false;

static const char* kSharpenHlsl = R"HLSL(
cbuffer SharpnessParams : register(b0) { float strength; float3 reserved; };
Texture2D<float4> SrcColor : register(t0);
RWTexture2D<float4> OutColor : register(u0);
[numthreads(16,16,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint w, h;
    SrcColor.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h) return;
    const int2 p = int2(tid.xy);
    const int2 endPixel = int2(w-1, h-1);
    const float4 center = SrcColor.Load(int3(p,0));
    float3 up = SrcColor.Load(int3(clamp(p+int2(0,-1),int2(0,0),endPixel),0)).rgb;
    float3 down = SrcColor.Load(int3(clamp(p+int2(0,1),int2(0,0),endPixel),0)).rgb;
    float3 left = SrcColor.Load(int3(clamp(p+int2(-1,0),int2(0,0),endPixel),0)).rgb;
    float3 right = SrcColor.Load(int3(clamp(p+int2(1,0),int2(0,0),endPixel),0)).rgb;
    float3 avg = 0.25f*(up+down+left+right);
    // Conservative unsharp masking. Respect local range to limit ringing.
    float3 localLo = min(center.rgb, min(min(up,down),min(left,right)));
    float3 localHi = max(center.rgb, max(max(up,down),max(left,right)));
    float3 result = clamp(center.rgb + strength*(center.rgb-avg), localLo-0.015f, localHi+0.015f);
    OutColor[tid.xy] = float4(saturate(result), center.a);
}
)HLSL";

struct SharpenConstants { float strength; float reserved[3]; };
static_assert(sizeof(SharpenConstants) == 16, "SharpenConstants alignment");

static bool CreateSharpenResources(ID3D11Texture2D* ngxOutput) {
    if (g_sharpInitAttempted) return g_sharpReady;
    g_sharpInitAttempted = true;
    if (!ngxOutput || !g_ngxDevice) return false;
    D3D11_TEXTURE2D_DESC desc{};
    ngxOutput->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 || desc.ArraySize != 1) {
        Log(L"SHARPEN_UNSUPPORTED_FORMAT format=%u", static_cast<unsigned>(desc.Format));
        return false;
    }
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    HRESULT hr = g_ngxDevice->CreateTexture2D(&desc, nullptr, &g_sharpTarget);
    if (FAILED(hr) || !g_sharpTarget) { Log(L"SHARPEN_TARGET_FAILED hr=0x%08X",static_cast<unsigned>(hr)); return false; }
    hr = g_ngxDevice->CreateShaderResourceView(ngxOutput, nullptr, &g_sharpInputSrv);
    if (FAILED(hr) || !g_sharpInputSrv) { Log(L"SHARPEN_SRV_FAILED hr=0x%08X",static_cast<unsigned>(hr)); return false; }
    hr = g_ngxDevice->CreateUnorderedAccessView(g_sharpTarget, nullptr, &g_sharpOutputUav);
    if (FAILED(hr) || !g_sharpOutputUav) { Log(L"SHARPEN_UAV_FAILED hr=0x%08X",static_cast<unsigned>(hr)); return false; }
    ID3DBlob* blob=nullptr;
    ID3DBlob* errors=nullptr;
    hr = D3DCompile(kSharpenHlsl, std::strlen(kSharpenHlsl), "anomaly_ngx_post_dlaa_sharpness",
                    nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr) || !blob) {
        if (errors) Log(L"SHARPEN_COMPILE_FAILED hr=0x%08X message=%hs", static_cast<unsigned>(hr),
                        static_cast<const char*>(errors->GetBufferPointer()));
        else Log(L"SHARPEN_COMPILE_FAILED hr=0x%08X", static_cast<unsigned>(hr));
        if (errors) errors->Release();
        if (blob) blob->Release();
        return false;
    }
    if (errors) errors->Release();
    hr = g_ngxDevice->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&g_sharpShader);
    blob->Release();
    if (FAILED(hr) || !g_sharpShader) { Log(L"SHARPEN_SHADER_FAILED hr=0x%08X",static_cast<unsigned>(hr)); return false; }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(SharpenConstants);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_ngxDevice->CreateBuffer(&cb,nullptr,&g_sharpParams);
    if (FAILED(hr) || !g_sharpParams) { Log(L"SHARPEN_PARAMS_FAILED hr=0x%08X",static_cast<unsigned>(hr)); return false; }
    g_sharpReady = true;
    Log(L"SHARPEN_RESOURCES_READY width=%u height=%u shader=cs_5_0",desc.Width,desc.Height);
    return true;
}

static bool RunSharpenPass(int percent, ID3D11Texture2D* output) {
    if (percent <= 0 || !g_ngxContext || !g_sharpReady || !output) return false;
    // A maximum of 0.35 is intentionally conservative to avoid recreating
    // fine-geometry aliasing that the user's prior guard had worsened.
    const SharpenConstants constants{0.35f*static_cast<float>(percent)/100.0f,{0,0,0}};
    g_ngxContext->UpdateSubresource(g_sharpParams,0,nullptr,&constants,0,0);
    g_ngxContext->CSSetShader(g_sharpShader,nullptr,0);
    g_ngxContext->CSSetConstantBuffers(0,1,&g_sharpParams);
    g_ngxContext->CSSetShaderResources(0,1,&g_sharpInputSrv);
    g_ngxContext->CSSetUnorderedAccessViews(0,1,&g_sharpOutputUav,nullptr);
    D3D11_TEXTURE2D_DESC d{};
    output->GetDesc(&d);
    g_ngxContext->Dispatch((d.Width+15)/16,(d.Height+15)/16,1);
    g_ngxContext->ClearState();
    if (!g_sharpPassLogged) {
        Log(L"SHARPEN_PASS_ACTIVE percent=%d spatial_gain=%.3f",percent,constants.strength);
        g_sharpPassLogged = true;
    }
    return true;
}

static float RadicalInverse(unsigned index, unsigned base) {
    float result = 0.f, inv = 1.f / static_cast<float>(base);
    while (index) {
        result += (index % base) * inv;
        index /= base;
        inv /= static_cast<float>(base);
    }
    return result;
}

static void HaltonJitter(unsigned frame, float& x, float& y) {
    // Frame and strength remain in the cache key: no stale result after
    // jitter-strength changes, skipped frames or a renderer restart.
    struct Cache {
        bool valid = false;
        unsigned frame = 0, phases = 0;
        int percent = 0;
        float x = 0.f, y = 0.f;
    };
    static thread_local Cache cache{};
    constexpr unsigned phases = 8u; // fixed validated Halton sequence; saved legacy phases ignored
    if (!cache.valid || cache.frame != frame || cache.phases != phases ||
        cache.percent != g_jitterPercent) {
        const unsigned phase = ((frame ? frame : 1u) - 1u) % phases + 1u;
        const float scale = static_cast<float>(g_jitterPercent) / 100.0f;
        cache.x = (RadicalInverse(phase, 2u) - 0.5f) * scale;
        cache.y = (RadicalInverse(phase, 3u) - 0.5f) * scale;
        cache.frame = frame;
        cache.phases = phases;
        cache.percent = g_jitterPercent;
        cache.valid = true;
    }
    x = cache.x; y = cache.y;
}

// The engine invokes the constant hook many times per frame. Inputs are
// re-read and revalidated on EACH call; only the exact float math is cached.
// Must never cache the mutable metadata, shader pointers, writable pages or
// output buffers: their lifetime is owned by the game.
static void ComputeMpDecompressConstants(unsigned frame, unsigned w, unsigned h,
    float fov, float aspect, float (&out)[4], float& jitterX, float& jitterY) {
    struct Cache {
        bool valid = false;
        unsigned frame = 0, width = 0, height = 0, phases = 0;
        int jitterPercent = 0;
        float fov = 0, aspect = 0, x = 0, y = 0, values[4]{};
    };
    static thread_local Cache cache{};
    if (!cache.valid || cache.frame != frame || cache.width != w ||
        cache.height != h || cache.phases != g_haltonPhases ||
        cache.jitterPercent != g_jitterPercent || cache.fov != fov ||
        cache.aspect != aspect) {
        float x = 0.f, y = 0.f;
        HaltonJitter(frame, x, y);
        constexpr float kPi = 3.14159265358979323846f;
        const float v = -std::tan((fov * .5f * kPi) / 180.f);
        const float sx = (-v / aspect);
        const float scaleX = 2.f * sx / static_cast<float>(w);
        const float scaleY = 2.f * v / static_cast<float>(h);
        cache.values[0] = sx + scaleX * x;
        cache.values[1] = v + scaleY * y;
        cache.values[2] = scaleX;
        cache.values[3] = scaleY;
        cache.x = x; cache.y = y;
        cache.frame = frame; cache.width = w; cache.height = h;
        cache.phases = g_haltonPhases; cache.jitterPercent = g_jitterPercent;
        cache.fov = fov; cache.aspect = aspect;
        cache.valid = true;
    }
    std::memcpy(out, cache.values, sizeof(cache.values));
    jitterX = cache.x; jitterY = cache.y;
}

// Mirrors the six sets of CPU shader constant slots used by FUN_14021BA20.
// The original function writes these; we replace only its 4-float output value.
static bool WritableRange(std::uintptr_t addr, size_t length,
                          MEMORY_BASIC_INFORMATION* regionOut = nullptr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!addr || !length) return false;
    // Preserve exact page-protection validation and one VirtualQuery per call.
    // Sample 1/256 Windows API calls to determine whether the cost varies
    // between fast and slow game launches; publish only batched atomics.
    // No measured query's success/failure result can be changed by this probe.
    if (g_v090VirtualQueryProbe) {
        static thread_local unsigned localCalls = 0;
        ++localCalls;
        const bool sample = (localCalls & 255u) == 0;
        LARGE_INTEGER before{}, after{};
        if (sample) QueryPerformanceCounter(&before);
        const SIZE_T queried = VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi));
        if (sample) {
            QueryPerformanceCounter(&after);
            InterlockedExchangeAdd64(&g_v090VirtualQueriesBatched, 256);
            InterlockedIncrement64(&g_v090VirtualQuerySamples);
            InterlockedExchangeAdd64(&g_v090VirtualQueryTicks, after.QuadPart-before.QuadPart);
        }
        if (!queried) return false;
        const DWORD protection = mbi.Protect & 0xFFu;
        const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const bool permitted = mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            (protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
             protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY) &&
            addr < end && length <= end-addr;
        if (sample) InterlockedIncrement64(permitted ?
            &g_v090WritablePassSamples : &g_v090WritableFailSamples);
        if (permitted && regionOut) *regionOut = mbi;
        return permitted;
    }
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    const DWORD protection = mbi.Protect & 0xFFu;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const bool permitted = addr < end && length <= end - addr;
    if (permitted && regionOut) *regionOut = mbi;
    return permitted;
}

// Release default (mode 3): no per-stage VirtualQuery, retain SEH write protection
// and one-way fallback to checked mode 2 on the first fault. Guard-page touches
// and partially completed paired writes remain possible; this is not proof
// of memory ownership or cross-process protection.
static int V0911WriteExceptionFilter(DWORD code) {
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ||
        code == 0x80000001u /* STATUS_GUARD_PAGE_VIOLATION */)
        return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}
static bool V0911GuardedWritePair(std::uintptr_t constantAddress,
                                  const float* values, unsigned floats,
                                  std::uintptr_t dirtyAddress) {
    if (!constantAddress || !values || !dirtyAddress || !floats || floats > 4)
        return false;
    const BYTE dirty=1;
    __try {
        std::memcpy(reinterpret_cast<void*>(constantAddress), values,
                    floats*sizeof(float));
        std::memcpy(reinterpret_cast<void*>(dirtyAddress), &dirty, sizeof(dirty));
        return true;
    } __except(V0911WriteExceptionFilter(GetExceptionCode())) {
        return false;
    }
}
static int V0911EffectiveValidationMode() {
    if (g_v091ValidationMode == 3 &&
        InterlockedCompareExchange(&g_v0911CheckedFallback,0,0) != 0)
        return 2;
    return g_v091ValidationMode;
}
static void V0911LatchFault(bool wasWrite) {
    if (wasWrite) InterlockedIncrement64(&g_v0911WriteFaults);
    else InterlockedIncrement64(&g_v0911ReadFaults);
    if (InterlockedCompareExchange(&g_v0911CheckedFallback,1,0)==0)
        Log(L"MP_SHADER_FASTPATH_FAULT kind=%ls fallback=checked_mode2 "
            L"may_have_consumed_guard_page=1 partial_write_possible=%d",
            wasWrite?L"write":L"read",wasWrite?1:0);
}

// Within one shader-constant update, a successful VirtualQuery result covers
// all pages with the same writable attributes. Check the dirty flag against
// that SAME result only when it lies in the reported region. This is NOT a
// cache: the region is a local, immediately discarded after this stage.
static bool V091DirtyFlagWritable(std::uintptr_t dirtyFlag,
                                 const MEMORY_BASIC_INFORMATION& constantRegion) {
    if (V0911EffectiveValidationMode() == 2) {
        const auto begin = reinterpret_cast<std::uintptr_t>(constantRegion.BaseAddress);
        if (dirtyFlag >= begin && dirtyFlag - begin < constantRegion.RegionSize) {
            if (g_v088DiagnosticsEnabled) InterlockedIncrement64(&g_v091SavedByRegion);
            return true;
        }
    }
    return WritableRange(dirtyFlag, 1);
}

static bool RewriteDecompressConstants(std::uintptr_t meta, const float values[4]) {
    const unsigned mask = MpHotReadOr<unsigned>(meta + 0x1C);
    if ((mask & 0x7Bu) == 0u) return false;
    const unsigned bits[6] = {1u, 2u, 8u, 16u, 32u, 64u};
    const unsigned shifts[6] = {16u, 12u, 8u, 20u, 24u, 28u};
    const unsigned descOffsets[6] = {0x20u, 0x24u, 0x28u, 0x2Cu, 0x30u, 0x34u};
    bool any = false;
    for (unsigned i = 0; i < 6; ++i) {
        if (!(mask & bits[i])) continue;
        const unsigned stage = (mask >> shifts[i]) & 0xFu;
        const std::uintptr_t stageState = MpHotReadOr<std::uintptr_t>(
            g_moduleBase + kShaderTableRvas[i] + stage * 8u);
        if (!stageState) continue;
        const std::uintptr_t buffer = MpHotReadOr<std::uintptr_t>(stageState + 0x78);
        if (!buffer) continue;
        const unsigned short offset = MpHotReadOr<unsigned short>(meta + descOffsets[i]);
        const short type = MpHotReadOr<short>(meta + descOffsets[i] + 2);
        if (type < 0 || type > 3) continue;
        const unsigned floats = type == 2 ? 3u : (type == 3 ? 2u : 4u);
        const std::uintptr_t address = buffer + offset;
        if (g_v088DiagnosticsEnabled) InterlockedIncrement64(&g_v091Candidates);
        // The constant buffer is ALWAYS validated before reading, including
        // PAGE_GUARD rejection. No unverified read or write is introduced.
        const int validationMode=V0911EffectiveValidationMode();
        MEMORY_BASIC_INFORMATION constantRegion{};
        if (validationMode != 3 &&
            !WritableRange(address, floats * sizeof(float),
                validationMode == 2 ? &constantRegion : nullptr)) continue;
        if (validationMode == 0 && !WritableRange(stageState + 0x80, 1))
            continue; // exact legacy protection-check order
        float before[4]{};
        if (!MpHotRead(address, before, floats * sizeof(float))) {
            if (validationMode == 3) V0911LatchFault(false);
            if (g_v088DiagnosticsEnabled) {
                InterlockedIncrement64(&g_v091ReadRejected);
                if (validationMode != 0) InterlockedIncrement64(&g_v091SavedByDefer);
            }
            continue;
        }
        // Only replace the same unjittered constant the game just emitted.
        if (!std::isfinite(before[0]) || !std::isfinite(before[1]) ||
            std::fabs(before[0] - (values[0] - values[2] * g_injectedJitterX)) > 0.005f ||
            std::fabs(before[1] - (values[1] - values[3] * g_injectedJitterY)) > 0.005f) {
            if (g_v088DiagnosticsEnabled) {
                InterlockedIncrement64(&g_v091DecompressRejected);
                if (g_v091ValidationMode != 0) InterlockedIncrement64(&g_v091SavedByDefer);
            }
            continue;
        }
        // Optimized: only the dirty flag's validation is deferred. The buffer
        // was checked BEFORE the read, as in the original implementation.
        if (validationMode != 3 && validationMode != 0 &&
            !V091DirtyFlagWritable(stageState + 0x80, constantRegion)) continue;
        if (validationMode == 3) {
            if (!V0911GuardedWritePair(address, values, floats, stageState + 0x80)) {
                V0911LatchFault(true);
                continue;
            }
            if (g_v088DiagnosticsEnabled) InterlockedIncrement64(&g_v0911DirectWritePairs);
        } else {
            // Identical checked writes for all original v0.91 modes 0, 1, 2.
            std::memcpy(reinterpret_cast<void*>(address), values, floats * sizeof(float));
            const BYTE dirty = 1;
            std::memcpy(reinterpret_cast<void*>(stageState + 0x80), &dirty, sizeof(dirty));
        }
        if (g_v088DiagnosticsEnabled) InterlockedIncrement64(&g_v091Matched);
        any = true;
        InterlockedIncrement64(&g_decompressWrites);
    }
    return any;
}

static void __fastcall DecompressHook(void* a, void* metadata) {
    V083HotHookScope v083Hot(kV083Jitter);
    if (g_originalDecompress) {
        if (g_v090VirtualQueryProbe) {
            static thread_local unsigned originalCallIndex = 0;
            const bool sample = ((++originalCallIndex & 255u) == 0);
            LARGE_INTEGER before{}, after{};
            if (sample) QueryPerformanceCounter(&before);
            g_originalDecompress(a, metadata);
            if (sample) {
                QueryPerformanceCounter(&after);
                InterlockedIncrement64(&g_v090DecompressOriginalSamples);
                InterlockedExchangeAdd64(&g_v090DecompressOriginalTicks,
                    after.QuadPart-before.QuadPart);
            }
        } else g_originalDecompress(a, metadata);
    }
    const LONG64 call = InterlockedIncrement64(&g_decompressCalls);
    if (InterlockedCompareExchange(&g_srMenuFrameInside,0,0) != 0) return;
    if (!EffectiveJitter() || !metadata || g_state != 1 || !RequestedEnabled() || g_fatalEvaluationError)
        return;
    // No cross-thread modifications of the renderer's constant-buffer staging area.
    if (g_renderTid && GetCurrentThreadId() != g_renderTid) return;
    const unsigned frame = ReadEngineFrame();
    if (!frame) return;
    const unsigned w = MpHotReadOr<unsigned>(g_moduleBase + kWidthRva);
    const unsigned h = MpHotReadOr<unsigned>(g_moduleBase + kHeightRva);
    const float fov = MpHotReadOr<float>(g_moduleBase + kFovRva);
    const float aspect = MpHotReadOr<float>(g_moduleBase + kAspectRva);
    if (w < 320 || w > 16384 || h < 200 || h > 16384 ||
        !std::isfinite(fov) || fov < 15.f || fov > 175.f ||
        !std::isfinite(aspect) || aspect < .1f || aspect > 10.f) {
        if (InterlockedIncrement64(&g_patchFailures) <= 5)
            Log(L"JITTER_GLOBAL_REJECT frame=%u w=%u h=%u fov=%.3f aspect=%.3f", frame, w, h, fov, aspect);
        return;
    }
    float x = 0.f, y = 0.f, out[4]{};
    ComputeMpDecompressConstants(frame, w, h, fov, aspect, out, x, y);
    g_injectedJitterX = x;
    g_injectedJitterY = y;
    if (!RewriteDecompressConstants(reinterpret_cast<std::uintptr_t>(metadata), out)) return;
    g_lastPatchedFrame = frame;
    if (call <= 8 || (call % 10000) == 0) {
        Log(L"JITTER_CONSTANT frame=%u phase=%u jitter=(%.6f,%.6f) output=(%.8f,%.8f) shader_calls=%lld writes=%lld",
            frame, (frame - 1) % g_haltonPhases + 1, x, y, out[0], out[1], call, g_decompressWrites);
    }
}

// Test-only bridge for SSS23 screen-space jitter uniform.
// FUN_1403636d0 writes a float4: x,y are clip-space offsets, z,w are SSS
// parameters.  The original engine uses a 4-point pattern / render dimensions
// when its TAA flag is set, but usually writes zero x/y with SSS TAA disabled.
// Preserve z/w (and all other shader constants).  Do not overwrite an active
// engine jitter pattern, which could double-apply TAA.
static bool RewriteSSFXJitterConstants(std::uintptr_t meta, float nx, float ny,
                                       float& previousX, float& previousY) {
    const unsigned mask = MpHotReadOr<unsigned>(meta + 0x1C);
    if ((mask & 0x7Bu) == 0u) return false;
    const unsigned bits[6] = {1u, 2u, 8u, 16u, 32u, 64u};
    const unsigned shifts[6] = {16u, 12u, 8u, 20u, 24u, 28u};
    const unsigned offsets[6] = {0x20u, 0x24u, 0x28u, 0x2Cu, 0x30u, 0x34u};
    bool wrote = false;
    for (unsigned i = 0; i < 6; ++i) {
        if ((mask & bits[i]) == 0) continue;
        const unsigned stage = (mask >> shifts[i]) & 0xFu;
        const std::uintptr_t stageState = MpHotReadOr<std::uintptr_t>(
            g_moduleBase + kShaderTableRvas[i] + stage * 8u);
        if (!stageState) continue;
        const std::uintptr_t buffer = MpHotReadOr<std::uintptr_t>(stageState + 0x78);
        if (!buffer) continue;
        const short type = MpHotReadOr<short>(meta + offsets[i] + 2);
        if (type < 0 || type > 3) continue;
        const unsigned count = type == 2 ? 3u : (type == 3 ? 2u : 4u);
        if (count < 2) continue;
        const unsigned short offset = MpHotReadOr<unsigned short>(meta + offsets[i]);
        const std::uintptr_t address = buffer + offset;
        if (g_v088DiagnosticsEnabled) InterlockedIncrement64(&g_v091Candidates);
        const int validationMode=V0911EffectiveValidationMode();
        MEMORY_BASIC_INFORMATION constantRegion{};
        if (validationMode != 3 &&
            !WritableRange(address, count * sizeof(float),
                validationMode == 2 ? &constantRegion : nullptr)) continue;
        if (validationMode == 0 && !WritableRange(stageState + 0x80, 1))
            continue;
        float before[4]{};
        if (!MpHotRead(address, before, count * sizeof(float))) {
            if (validationMode == 3) V0911LatchFault(false);
            if (g_v088DiagnosticsEnabled) {
                InterlockedIncrement64(&g_v091ReadRejected);
                if (validationMode != 0) InterlockedIncrement64(&g_v091SavedByDefer);
            }
            continue;
        }
        if (!std::isfinite(before[0]) || !std::isfinite(before[1]) ||
            std::fabs(before[0]) > 0.00000001f || std::fabs(before[1]) > 0.00000001f) {
            // Do not overwrite an active engine jitter or a mod-owned vector.
            if (g_v088DiagnosticsEnabled) {
                InterlockedIncrement64(&g_v091SsfxRejected);
                if (g_v091ValidationMode != 0) InterlockedIncrement64(&g_v091SavedByDefer);
            }
            const LONG64 skipped = InterlockedIncrement64(&g_ssfxSkippedExistingJitter);
            if (skipped == 1 || (skipped % 100000) == 0)
                Log(L"SSFX_SYNC_SKIPPED_NATIVE_JITTER original=(%.8f,%.8f) skipped=%lld",
                    before[0], before[1], skipped);
            continue;
        }
        if (validationMode != 3 && validationMode != 0 &&
            !V091DirtyFlagWritable(stageState + 0x80, constantRegion)) continue;
        previousX = before[0]; previousY = before[1];
        const float replacement[2] = {nx, ny};
        if (validationMode == 3) {
            if (!V0911GuardedWritePair(address, replacement, 2, stageState + 0x80)) {
                V0911LatchFault(true);
                continue;
            }
            if (g_v088DiagnosticsEnabled) InterlockedIncrement64(&g_v0911DirectWritePairs);
        } else {
            // Identical checked writes for all original v0.91 modes 0, 1, 2.
            std::memcpy(reinterpret_cast<void*>(address), replacement, sizeof(replacement));
            const BYTE dirty = 1;
            std::memcpy(reinterpret_cast<void*>(stageState + 0x80), &dirty, 1);
        }
        if (g_v088DiagnosticsEnabled) InterlockedIncrement64(&g_v091Matched);
        InterlockedIncrement64(&g_ssfxWrites);
        wrote = true;
    }
    return wrote;
}

static void __fastcall SSFXJitterHook(void* object, void* metadata) {
    V083HotHookScope v083Hot(kV083Ssfx);
    if (g_originalSSFXJitter) {
        if (g_v090VirtualQueryProbe) {
            static thread_local unsigned originalCallIndex = 0;
            const bool sample = ((++originalCallIndex & 255u) == 0);
            LARGE_INTEGER before{}, after{};
            if (sample) QueryPerformanceCounter(&before);
            g_originalSSFXJitter(object, metadata);
            if (sample) {
                QueryPerformanceCounter(&after);
                InterlockedIncrement64(&g_v090SsfxOriginalSamples);
                InterlockedExchangeAdd64(&g_v090SsfxOriginalTicks,
                    after.QuadPart-before.QuadPart);
            }
        } else g_originalSSFXJitter(object, metadata);
    }
    const LONG64 call = InterlockedIncrement64(&g_ssfxCalls);
    if (InterlockedCompareExchange(&g_srMenuFrameInside,0,0) != 0) return;
    if (g_ssfxSyncMode == 0 || !g_ssfxSyncHookInstalled || !metadata ||
        !g_autoEnable || !RequestedEnabled() || g_state != 1 ||
        !EffectiveJitter() || g_fatalEvaluationError ||
        !g_renderTid || GetCurrentThreadId() != g_renderTid) return;

    const unsigned frame = ReadEngineFrame();
    if (!frame) return;
    const unsigned width = MpHotReadOr<unsigned>(g_moduleBase + kWidthRva);
    const unsigned height = MpHotReadOr<unsigned>(g_moduleBase + kHeightRva);
    if (width < 320 || width > 16384 || height < 200 || height > 16384) return;
    float x = 0.f, y = 0.f;
    HaltonJitter(frame, x, y); // same engine frame and pixel-space sequence as NGX
    // SSS shader: hpos.xy += ssfx_jitter.xy * hpos.w.
    // Clip-space NDC = 2 * pixelOffset / renderDimension.
    // Y is normally inverted when translating top-left pixel coords to clip Y.
    const float clipX = 2.0f * x / static_cast<float>(width);
    const float clipY = -2.0f * y /
        static_cast<float>(height);
    float beforeX = 0.f, beforeY = 0.f;
    if (!RewriteSSFXJitterConstants(reinterpret_cast<std::uintptr_t>(metadata),
                                    clipX, clipY, beforeX, beforeY)) return;
    g_ssfxLastPatchedFrame = frame;
    if (call <= 8 || (call % 10000) == 0) {
        Log(L"SSFX_SYNC_WRITE frame=%u mode=%d pixel=(%.6f,%.6f) clip=(%.9f,%.9f) before=(%.9f,%.9f) calls=%lld writes=%lld",
            frame, g_ssfxSyncMode, x, y, clipX, clipY, beforeX, beforeY,
            call, g_ssfxWrites);
    }
}

static bool InstallSSFXJitterHook(void* target) {
    // Exact original validated DX11 AVX target prologue at RVA 0x3636D0.
    // Fail closed if another plugin already detoured the function.
    static constexpr BYTE expected[] = {
        0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x30
    };
    BYTE observed[sizeof(expected)]{};
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(target), observed, sizeof(observed)) ||
        std::memcmp(expected, observed, sizeof(expected)) != 0) {
        Log(L"SSFX_SYNC_NOT_INSTALLED ORIGINAL_PROLOGUE_MISMATCH; DLL_BASELINE_UNCHANGED");
        return false;
    }
    const MH_STATUS created = MH_CreateHook(target,
        reinterpret_cast<void*>(&SSFXJitterHook),
        reinterpret_cast<void**>(&g_originalSSFXJitter));
    if (created != MH_OK) {
        Log(L"SSFX_SYNC_NOT_INSTALLED MinHook_Create=%d; DLL_BASELINE_UNCHANGED", created);
        g_originalSSFXJitter = nullptr;
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK) {
        Log(L"SSFX_SYNC_NOT_INSTALLED MinHook_Enable=%d; DLL_BASELINE_UNCHANGED", enabled);
        MH_RemoveHook(target);
        g_originalSSFXJitter = nullptr;
        return false;
    }
    g_ssfxSyncHookInstalled = true;
    Log(L"SSFX_SYNC_HOOK_INSTALLED method=MINHOOK target=%p mode=%d", target, g_ssfxSyncMode);
    return true;
}

static float GetMeasuredDeltaMs() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (!g_qpcFrequency.QuadPart) QueryPerformanceFrequency(&g_qpcFrequency);
    float ms = 16.67f;
    if (g_previousQpc.QuadPart && g_qpcFrequency.QuadPart) {
        ms = static_cast<float>((now.QuadPart - g_previousQpc.QuadPart) *
            1000.0 / static_cast<double>(g_qpcFrequency.QuadPart));
        if (!(ms >= 1.0f && ms <= 100.0f)) {
            // Long pause can invalidate DLSS history.
            g_resetPending = true;
            ms = 16.67f;
        }
    }
    g_previousQpc = now;
    return ms;
}

static void AbortTrial(const wchar_t* stage) {
    g_fatalEvaluationError = true;
    g_evaluationEnabled = false;
    Log(L"TRIAL_ABORTED stage=%ls frames=%u total=%llu RESTART_REQUIRED",
        stage, g_runFrames, g_totalEvaluations);
}

// Shared NGX frame contract: the executable adapter supplies color/depth/MV,
// ownership, frame timing and where to present. NVIDIA's GPU-side evaluation
// and mode parameters are common to multiplayer and GAMMA.
struct SharedNgxFrame {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* color;
    ID3D11Texture2D* motion;
    ID3D11Texture2D* depth;
    ID3D11Texture2D* output;
    unsigned renderW, renderH;
    bool reset;
    float jitterX, jitterY;
    float deltaMs;
};
static NVSDK_NGX_Result EvaluateSharedNgxFrame(const SharedNgxFrame& frame) {
    ProfileScope profile(kProfileSharedNgxEvaluate);
    NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
    eval.Feature.pInColor=frame.color;
    eval.Feature.pInOutput=frame.output;
    eval.pInDepth=frame.depth;
    eval.pInMotionVectors=frame.motion;
    eval.InRenderSubrectDimensions.Width=frame.renderW;
    eval.InRenderSubrectDimensions.Height=frame.renderH;
    eval.InReset=frame.reset?1:0;
    eval.InJitterOffsetX=frame.jitterX;
    eval.InJitterOffsetY=frame.jitterY;
    eval.InMVScaleX=-static_cast<float>(frame.renderW);
    eval.InMVScaleY=-static_cast<float>(frame.renderH);
    eval.InFrameTimeDeltaInMsec=frame.deltaMs;
    if (g_gammaNativeTrialActive) {
        // Only shared NGX instrumentation; GAMMA adapter implementation untouched.
        ++g_v083FrameCounter;
        V083PollGpuQueries();
        V083ThreadSample(g_activeMode);
        V083BeginSample(true);
        V083Mark(0);
    }
    const NVSDK_NGX_Result v083Result = NGX_D3D11_EVALUATE_DLSS_EXT(
        frame.context,g_feature,g_params,&eval);
    if (g_gammaNativeTrialActive) {
        V083Mark(1);
        V083CompleteSample();
    }
    return v083Result;
}

// Runs only on the validated renderer thread, before the original final-composite call.
// The engine's input RTV is saved by SwapDeviceContextState, not manually overwritten.
static bool g_gammaCompositeEvalScope = false;
static void TryPresentDLAA(void* renderer, LONG64 call) {
    // GAMMA performs its continuous evaluation via its own isolated
    // private-input path and draw-time SRV handoff; MP is unchanged.
    if (g_gammaNativeTrialActive) return;
    if (InterlockedCompareExchange(&g_srConstructorBusy,0,0) != 0) return;
    if (!renderer || g_state != 1 || !g_evaluationEnabled || g_fatalEvaluationError || call < 2 ||
        (!EffectiveJitter() && !g_allowZeroJitterFallback))
        return;
    ID3D11Texture2D* color = GetMpTextureFromSlot(renderer, kColorSlot);
    ID3D11Texture2D* motion = GetMpTextureFromSlot(renderer, kMotionSlot);
    ID3D11Texture2D* depth = GetMpTextureFromSlot(renderer, kDepthSlot);
    if (!color || !motion || !depth) return;

    ID3D11RenderTargetView* rtv = nullptr;
    g_ngxContext->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* bound = nullptr;
    if (rtv) rtv->GetResource(&bound);
    const bool correctPhase = bound == static_cast<ID3D11Resource*>(color) ||
        (g_gammaNativeTrialActive && g_gammaCompositeEvalScope &&
         bound != nullptr && bound != static_cast<ID3D11Resource*>(color));
    if (bound) bound->Release();
    if (rtv) rtv->Release();
    if (!correctPhase) return;

    const unsigned engineFrame = ReadEngineFrame();
    if (engineFrame && engineFrame == g_lastEngineFrame) return;
    if (EffectiveJitter() && engineFrame && g_lastPatchedFrame != engineFrame) {
        g_resetPending = true;
        const LONG64 skipped = InterlockedIncrement64(&g_skippedForJitter);
        if (engineFrame != g_lastUnmatchedFrame) {
            g_lastUnmatchedFrame = engineFrame;
            ++g_consecutiveUnmatchedFrames;
        }
        if (skipped <= 5 || skipped % 600 == 0)
            Log(L"EVAL_SKIPPED_UNJITTERED_FRAME engine_frame=%u patched_frame=%u skipped=%lld consecutive=%u shader_calls=%lld",
                engineFrame, g_lastPatchedFrame, skipped, g_consecutiveUnmatchedFrames, g_decompressCalls);
        // Avoid remaining permanently disabled when the constant generator
        // is not on this render path. Give it 120 unique engine frames first.
        if (g_allowZeroJitterFallback && g_consecutiveUnmatchedFrames >= 120) {
            g_jitterDegradedToZero = true;
            g_lastEngineFrame = 0;
            Log(L"JITTER_NO_FRAME_MATCH_SWITCH_ZERO_FALLBACK unique_frames=%u DLAA_CONTINUES",
                g_consecutiveUnmatchedFrames);
        }
        return;
    }
    if (EffectiveJitter()) g_consecutiveUnmatchedFrames = 0;

    D3D11_TEXTURE2D_DESC c{}, m{}, d{};
    // Before checking retained resource identity, observe whether an engine-side
    // reallocation has actually occurred. A different pointer alone never proves
    // SR; only matching color/depth/motion dimensions and native output do.
    if (g_firstColor && (color != g_firstColor || motion != g_firstMotion || depth != g_firstDepth)) {
        D3D11_TEXTURE2D_DESC newC{}, newM{}, newD{};
        color->GetDesc(&newC); motion->GetDesc(&newM); depth->GetDesc(&newD);
        const bool engineReallocated = (newC.Width == newM.Width && newC.Width == newD.Width &&
            newC.Height == newM.Height && newC.Height == newD.Height &&
            newC.Width > 0 && newC.Height > 0);
        if (engineReallocated) {
            Log(L"SR_ENGINE_RECREATED_TARGETS size=%ux%u native=%ux%u requested=%ls",
                newC.Width,newC.Height,g_displayWidth,g_displayHeight,ModeName(ModeForCurrentBuffers()));
            if (g_handoffView) {g_handoffView->Release();g_handoffView=nullptr;}
            g_handoffViewTexture=nullptr;
            g_handoffViewDescValid=false;
            g_firstColor=nullptr;g_firstMotion=nullptr;g_firstDepth=nullptr;
            g_resetPending=true;g_lastEngineFrame=0;
        }
    }
    if (!g_firstColor) {
        if (!InspectTexture(color, L"color", &c) ||
            !InspectTexture(motion, L"motion", &m) ||
            !InspectTexture(depth, L"depth", &d)) {
            AbortTrial(L"initial_resource_inspection");
            return;
        }
    } else {
        if (color != g_firstColor || motion != g_firstMotion || depth != g_firstDepth) {
            AbortTrial(L"resource_identity_changed");
            return;
        }
        color->GetDesc(&c);
        motion->GetDesc(&m);
        depth->GetDesc(&d);
    }
    if (c.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
        m.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        d.Format != DXGI_FORMAT_R24G8_TYPELESS ||
        c.Width != m.Width || c.Height != m.Height ||
        c.Width != d.Width || c.Height != d.Height ||
        c.Width == 0 || c.Height == 0 || c.SampleDesc.Count != 1 ||
        m.SampleDesc.Count != 1 || d.SampleDesc.Count != 1 ||
        !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        AbortTrial(L"texture_desc_mismatch");
        return;
    }
    // When the engine genuinely recreated its inputs, retain the new identities
    // even when our independent native-size NGX output texture already exists.
    if (!g_firstColor && g_output) {
        g_firstColor=color; g_firstMotion=motion; g_firstDepth=depth;
    }
    // All four SR modes use the SAME proven direct NGX -> final-composite path.
    // SR is never enabled on native-sized input disguised as a low-res image.
    const int modeForBuffers = ModeForCurrentBuffers();
    if (modeForBuffers != 0) {
        const SRModePlan& plan = g_srPlans[modeForBuffers];
        const unsigned engineW = MpHotReadOr<unsigned>(g_moduleBase + kWidthRva);
        const unsigned engineH = MpHotReadOr<unsigned>(g_moduleBase + kHeightRva);
        const bool expected = plan.valid && c.Width == plan.width && c.Height == plan.height;
        // Native width/height are restored before the final composite, by
        // design. Verify that this frame actually ran with low globals.
        const bool splitRestored = g_srScaleReady && g_srConstructorSawAll &&
            g_srStartupMode == modeForBuffers &&
            InterlockedCompareExchange(&g_srFrameInside, 0, 0) != 0 &&
            InterlockedCompareExchange(&g_srFrameRestoredBeforeComposite, 0, 0) != 0 &&
            engineW == g_srDisplayStartupW && engineH == g_srDisplayStartupH;
        const bool globalsMatch = splitRestored || (engineW == c.Width && engineH == c.Height);
        // The 32-frame handoff proof is impossible before first SR evaluation:
        // low-resolution scene inputs cannot be evaluated as native DLAA.
        // The user already visually validated this exact composite path at
        // native size in v0.3.4. Allow first SR frame once the hook is installed.
        const bool readyHandoff = g_handoffMode == 2 && g_psSetHookInstalled &&
            (g_handoffLastGoodFrames >= 32 || splitRestored);
        if (!expected || !globalsMatch || !readyHandoff) {
            if (engineFrame != g_lastPendingFrame && (g_lastPendingFrame == 0 || engineFrame % 600 == 0)) {
                Log(L"SR_WAITING_FOR_ENGINE_RESIZE requested=%ls required=%ux%u observed=%ux%u engine_globals=%ux%u output=%ux%u handoff=%d fallback=DLAA",
                    ModeName(modeForBuffers),plan.width,plan.height,c.Width,c.Height,
                    engineW,engineH,g_displayWidth,g_displayHeight,readyHandoff?1:0);
                g_lastPendingFrame=engineFrame;
            }
            if (g_activeMode != 0) {
                Log(L"SR_ACTIVE_INPUT_MISMATCH fail_closed=1");
                return; // Never evaluate a feature with stale input sizes.
            }
        } else if (g_activeMode != modeForBuffers ||
                   g_activeRenderWidth != c.Width || g_activeRenderHeight != c.Height) {
            Log(L"SR_ENGINE_RENDER_SIZE_VERIFIED mode=%ls input=%ux%u output=%ux%u",
                ModeName(modeForBuffers),c.Width,c.Height,g_displayWidth,g_displayHeight);
            if (!RecreateFeature(modeForBuffers, c.Width, c.Height)) return;
        }
    }
    if (modeForBuffers == 0 && g_activeMode != 0) {
        if (c.Width != g_displayWidth || c.Height != g_displayHeight) return;
        if (!RecreateFeature(0, g_displayWidth, g_displayHeight)) return;
    }
    if (g_activeMode != 0 && (c.Width != g_activeRenderWidth || c.Height != g_activeRenderHeight))
        return; // Strict NGX contract; no false-success logs.
    if (g_activeMode == 0 && (c.Width != g_displayWidth || c.Height != g_displayHeight))
        return; // Never evaluate the native DLAA feature on reduced input textures.

    if (!EnsureIsolatedContext()) {
        AbortTrial(L"context_state_unavailable");
        return;
    }
    if (!g_output) {
        D3D11_TEXTURE2D_DESC outDesc = c;
        outDesc.Width = g_displayWidth;
        outDesc.Height = g_displayHeight;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE |
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        outDesc.CPUAccessFlags = 0;
        outDesc.MiscFlags = 0;
        const HRESULT hr = g_ngxDevice->CreateTexture2D(&outDesc, nullptr, &g_output);
        if (FAILED(hr) || !g_output) {
            Log(L"OUTPUT_CREATE_FAILED hr=0x%08X", static_cast<unsigned>(hr));
            AbortTrial(L"output_creation");
            return;
        }
        g_firstColor = color;
        g_firstMotion = motion;
        g_firstDepth = depth;
        Log(L"OUTPUT_CREATED ptr=%p size=%ux%u format=%u bind=0x%X",
            g_output, outDesc.Width, outDesc.Height, static_cast<unsigned>(c.Format), outDesc.BindFlags);
    }

    // All NGX operations run in a separate D3D11.1 state, on the game's immediate
    // context, and are serialized on the existing renderer thread.
    // Start timestamp group ONLY for validated MP frames, immediately before
    // state isolation. No GPU synchronization or worker-thread dispatch.
    V083BeginSample(false);
    V083Mark(0); // GPU/CPU pre-swap boundary
    ID3DDeviceContextState* previous = nullptr;
    g_context1->SwapDeviceContextState(g_isolatedState, &previous);
    V083Mark(1); // CPU state swap-in and GPU marker overhead boundary
    const float deltaMs = GetMeasuredDeltaMs();
    const bool reset = g_resetPending;

    float jitterX = 0.f, jitterY = 0.f;
    if (EffectiveJitter()) HaltonJitter(engineFrame, jitterX, jitterY);
    const SharedNgxFrame ngxFrame{g_ngxContext,color,motion,depth,g_output,
        c.Width,c.Height,reset,jitterX,jitterY,deltaMs};
    const NVSDK_NGX_Result result = EvaluateSharedNgxFrame(ngxFrame);
    V083Mark(2); // GPU queued NGX execution boundary
    if (NVSDK_NGX_SUCCEED(result)) {
        // Only clear the ISOLATED context state, not the game's original bindings.
        g_ngxContext->ClearState();
        const int guardMode = CurrentGuardMode();
        if (guardMode != g_lastGuardMode) {
            Log(L"GUARD_MODE_CHANGED mode=%ls", guardMode == 2 ? L"STRONG" :
                (guardMode == 1 ? L"MILD" : L"OFF"));
            g_lastGuardMode = guardMode;
        }
        ID3D11Texture2D* presented = g_output;
        if (guardMode != 0 && g_activeMode == 0) {
            if (CreateGuardResources(color, g_output) && RunGuard(guardMode, color, g_output))
                presented = g_guardTarget;
            else if (g_runFrames <= 5) Log(L"GUARD_DISABLED_AFTER_SETUP_FAILURE original_NGX_output_used");
        }
        // Optional spatial sharpening AFTER DLAA. The modern NGX sharpness
        // parameter is unsupported; never pass this setting into NGX directly.
        if (g_sharpnessPercent > 0 && presented == g_output) {
            if (CreateSharpenResources(g_output) && RunSharpenPass(g_sharpnessPercent, g_output))
                presented = g_sharpTarget;
            else if (g_runFrames <= 5) Log(L"SHARPEN_UNAVAILABLE_USING_UNMODIFIED_DLAA");
        }
        V083Mark(3); // ClearState, optional guard and sharpening GPU work
        // In v0.3.8 the NGX output can also be bound directly as the final
        // composite input (a prerequisite for future input/output size split).
        // Mode 1 keeps the known-good native-size copy as a safety net. Mode 2
        // is an opt-in, guarded same-size bypass after 32 consecutive matches.
        g_handoffPresentedTexture = presented;
        g_handoffResultReadyThisFrame = g_psSetHookInstalled && MpEffectiveHandoffMode() != 0;
        const bool directAllowed = MpEffectiveHandoffMode() == 2 && g_psSetHookInstalled &&
            g_handoffLastGoodFrames >= 32 && g_handoffView != nullptr &&
            !g_handoffForceCopyNextFrame;
        if (g_activeMode != 0 && !directAllowed && g_runFrames == 0) {
            // SR cannot use the native-size CopyResource fallback. During the
            // first 32 frames the *previous* frame's composite binding is not
            // yet confirmed, but the PS hook can still construct the NGX SRV
            // in the original composite on this frame. Report this as startup
            // warmup, not as a false "handoff lost" on every evaluated frame.
            Log(L"SR_HANDOFF_INITIAL_BIND_PENDING first_frame=1 "
                L"copyback_impossible=1 verify_next_composite=1");
        }
        if (g_gammaNativeTrialActive) {
            // GAMMA does not use the MP copyback path. Its own draw-time
            // SRV substitution handles presentation, without scene writes.
            // This branch is not entered by GAMMA, even if
            // an old INI mistakenly requests GAMMA copyback=1.
            g_handoffCopySkippedThisFrame = true;
            if (g_runFrames < 5)
                Log(L"GAMMA_SHADOW_COPY_SKIPPED upcoming_eval=%u color=%p output=%p no_resource_writes=1",
                    g_runFrames+1,color,presented);
        } else if (directAllowed || g_activeMode != 0) {
            ++g_handoffTotalDirectFrames;
            g_handoffCopySkippedThisFrame = true;
        } else {
            g_ngxContext->CopyResource(color, presented);
            ++g_handoffTotalCopyFrames;
        }
        V083Mark(4); // GPU DLAA CopyResource, or upscaling handoff decision
        g_handoffForceCopyNextFrame = false;
        if (g_runFrames < 5 || (g_runFrames % 1200) == 0)
            Log(L"SR_HANDOFF_EVAL mode=%d copyback=%d direct=%d proven_frames=%u view_ready=%d",
                MpEffectiveHandoffMode(), g_handoffCopySkippedThisFrame ? 0 : 1,
                g_handoffCopySkippedThisFrame ? 1 : 0,
                g_handoffLastGoodFrames, g_handoffView ? 1 : 0);
        g_resetPending = false;
        g_lastEngineFrame = engineFrame;
        ++g_runFrames;
        ++g_totalEvaluations;
        if (g_runFrames <= 5 || g_runFrames % 1200 == 0) {
            Log(L"NGX_PRESENTED frame=%u engine_frame=%u call=%lld feature=%ls jitter_mode=%ls jitter=(%.6f,%.6f) delta_ms=%.3f reset=%d result=0x%08X",
                g_runFrames, engineFrame, call, ModeName(g_activeMode),
                EffectiveJitter() ? L"HALTON8_CONSTANT" : L"ZERO_FALLBACK",
                jitterX, jitterY, deltaMs, reset ? 1 : 0, static_cast<unsigned>(result));
        }
    } else {
        Log(L"EVAL_FAILED result=0x%08X frame=%u", static_cast<unsigned>(result), g_runFrames);
        AbortTrial(L"ngx_evaluate");
    }

    // ALWAYS restore the game renderer state before the original final-composite function.
    if (previous) {
        g_context1->SwapDeviceContextState(previous, nullptr);
        previous->Release();
    }
    V083Mark(5); // includes CPU state swap-out; final composite follows
    if (g_runFrames <= 3 || g_runFrames % 1200 == 0) {
        Log(L"CONTEXT_RESTORED frame=%u", g_runFrames);
    }
    const HRESULT deviceStatus = g_ngxDevice->GetDeviceRemovedReason();
    if (FAILED(deviceStatus)) {
        Log(L"DEVICE_REMOVAL hr=0x%08X", static_cast<unsigned>(deviceStatus));
        AbortTrial(L"device_removed");
    }
}

static void RestoreSRNativeGlobalsForComposite() {
    if (!g_srScaleReady || InterlockedCompareExchange(&g_srFrameInside,0,0) == 0 ||
        InterlockedCompareExchange(&g_srGlobalsReduced,0,0) == 0) return;
    *reinterpret_cast<volatile unsigned*>(g_moduleBase + kWidthRva) = g_srDisplayStartupW;
    *reinterpret_cast<volatile unsigned*>(g_moduleBase + kHeightRva) = g_srDisplayStartupH;
    InterlockedExchange(&g_srGlobalsReduced, 0);
    InterlockedExchange(&g_srFrameRestoredBeforeComposite, 1);
    ++g_srCompositeRestores;
    if (g_srCompositeRestores <= 5 || g_srCompositeRestores % 1200 == 0)
        Log(L"SR_FRAME_COMPOSITE_NATIVE_RESTORE count=%lld display=%ux%u",
            g_srCompositeRestores,g_srDisplayStartupW,g_srDisplayStartupH);
}
static void __fastcall RendererHook(void* renderer) {
    ProfileScope profile(kProfileMpComposite);
    ++g_v083FrameCounter;
    V083PollGpuQueries();
    V083ThreadSample();
    if (InterlockedExchange(&g_srPauseSidecarArmed,0)!=0)
        Log(L"SR_PAUSE_SIDECAR_AUTO_DISARM reason=GAMEPLAY_COMPOSITE_RESUMED");
    RestoreSRNativeGlobalsForComposite();
    g_handoffResultReadyThisFrame = false;
    g_handoffCopySkippedThisFrame = false;
    g_handoffPresentedTexture = nullptr;
    const LONG64 n = InterlockedIncrement64(&g_calls);
    if (g_v088DiagnosticsEnabled) InterlockedExchange64(&g_v088RenderCalls,n);
    const DWORD current = GetCurrentThreadId();
    const DWORD prior = static_cast<DWORD>(InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&g_renderTid), static_cast<LONG>(current), 0));
    // OBSERVE first-caller ownership, never change it in the diagnostic build.
    if (g_v088DiagnosticsEnabled && prior==0) {
        InterlockedCompareExchange(&g_v088RendererThread, static_cast<LONG>(current),0);
        InterlockedCompareExchange(&g_v088FirstRenderTick,static_cast<LONG>(GetTickCount()),0);
    }
    if (g_v088DiagnosticsEnabled && prior && prior!=current) {
        InterlockedIncrement64(&g_v088OtherThreadCalls);
        InterlockedExchange(&g_v088OtherThread,static_cast<LONG>(current));
    }
    const bool v088Sample = g_v088DiagnosticsEnabled &&
        (prior==0 || prior==current) &&
        (n<=16 || (n%g_v088SampleEvery)==0);
    LARGE_INTEGER v088Begin{},v088PostSettings{},v088BeforeNgx{},v088AfterNgx{};
    if (v088Sample) QueryPerformanceCounter(&v088Begin);
    if (prior == 0 || prior == current) {
        // Apply once on the verified game render thread. The hook is synchronous
        // on this SAME thread; this prioritizes the entire game render thread,
        // including original game rendering, NOT just our DLSS handoff.
        if (g_mpRenderPriorityMode != 0 &&
            InterlockedCompareExchange(&g_mpRenderPriorityAttempted, 1, 0) == 0) {
            const int desired = g_mpRenderPriorityMode == 2 ?
                THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL;
            const int previous = GetThreadPriority(GetCurrentThread());
            const BOOL ok = SetThreadPriority(GetCurrentThread(), desired);
            const DWORD error = ok ? 0 : GetLastError();
            if (g_v088DiagnosticsEnabled) {
                InterlockedExchange(&g_v088AppliedPriorityOld,previous);
                InterlockedExchange(&g_v088AppliedPriorityNew,GetThreadPriority(GetCurrentThread()));
                InterlockedExchange(&g_v088AppliedPrioritySuccess,ok?1:0);
            }
            Log(L"V087_RENDER_PRIORITY mode=%d previous=%d requested=%d actual=%d success=%d error=%lu tid=%lu",
                g_mpRenderPriorityMode, previous, desired,
                GetThreadPriority(GetCurrentThread()), ok ? 1 : 0, error, current);
        }
        ReloadSettingsOnRenderThread();
        if (v088Sample) QueryPerformanceCounter(&v088PostSettings);
        if (g_srReconfigureAtConstructor && g_srLatestCompletedRenderer &&
            reinterpret_cast<uintptr_t>(renderer)!=g_srLatestCompletedRenderer) {
            if (InterlockedCompareExchange(&g_srOldRendererSkipLogged,1,0)==0)
                Log(L"SR_REBUILD_OLD_RENDERER_SKIP current=%p latest_complete=%p keeping_original_composite=1",
                    renderer,reinterpret_cast<void*>(g_srLatestCompletedRenderer));
            if (g_original) g_original(renderer);
            return;
        }
if (g_uiPassTraceEnabled || g_srMenuActivityDiagEnabled || g_srPauseSidecarEnabled ||
    g_pauseTransitionDiagEnabled) {
    g_uiPassKnownUi = GetMpTextureFromSlot(renderer,0x498);
    g_uiPassKnownColor = GetMpTextureFromSlot(renderer,kColorSlot);
    g_uiPassKnownGeneric = GetMpTextureFromSlot(renderer,0x428);
    g_uiPassKnownGeneric0 = GetMpTextureFromSlot(renderer,0x468);
    g_uiPassKnownGeneric1 = GetMpTextureFromSlot(renderer,0x470);
    g_uiPassKnownViewport = GetMpTextureFromSlot(renderer,0x480);
    g_uiPassKnownScope = GetMpTextureFromSlot(renderer,0x488);
}
        if (g_state == 0) TryHandshake(renderer);
        if (g_state == 1 && g_ngxDevice && g_srReconfigureAtConstructor &&
            InterlockedCompareExchange(&g_srConstructorBusy,0,0)==0) {
            // A renderer reconstruction may or may not recreate the D3D11
            // device. Never evaluate stale NGX objects against a NEW device.
            ID3D11Texture2D* currentColor = GetMpTextureFromSlot(renderer,kColorSlot);
            if (currentColor) {
                ID3D11Device* owner = nullptr;
                currentColor->GetDevice(&owner);
                if (owner && owner != g_ngxDevice) {
                    if (!g_srDeviceMismatchLogged)
                        Log(L"SR_REBUILD_DEVICE_CHANGED old=%p new=%p fail_closed=1 NGX_REINITIALIZATION_REQUIRED",
                            g_ngxDevice,owner);
                    g_srDeviceMismatchLogged=true;
                    g_fatalEvaluationError=true;
                    g_evaluationEnabled=false;
                    InterlockedExchange(&g_srResetDLAAHold,1);
                } else if (owner && InterlockedCompareExchange(&g_srResetDLAAHold,0,0)!=0 &&
                    InterlockedCompareExchange64(&g_srConstructorCount,0,0) >
                        InterlockedCompareExchange64(&g_srResetCtorAtBegin,0,0) &&
                    reinterpret_cast<uintptr_t>(renderer)==g_srLatestCompletedRenderer &&
                    !g_srDeviceMismatchLogged) {
                    // The engine actually created new resources, on the same
                    // D3D11 device. Allow NGX to validate the new textures.
                    InterlockedExchange(&g_srResetDLAAHold,0);
                    g_resetPending=true;
                    g_lastEngineFrame=0;
                    Log(L"SR_VIDEO_RESET_NGX_RESUMED same_device=1 ctor=%lld allocation_mode=%ls",
                        g_srConstructorCount,ModeName(g_srStartupMode));
                }
                if (owner) owner->Release();
            }
        }
        if (g_handoffMode != 0 && g_state == 1 && !g_psSetHookAttempted) {
            g_psSetHookAttempted = true;
            InstallCompositeSRVHook();
        }
        InstallUIPassTraceHooks();
        SyncActivationState();
        // The no-level path was already NGX-free in v039. Extend that proven
        // decision to a *manually* marked pause menu without touching engine
        // texture ownership, rasterizer state, shader inputs, or display size.
        const bool manualMenuBypass = g_srMenuHandoffBypassEnabled &&
            InterlockedCompareExchange(&g_srMenuHandoffBypassArmed,0,0)!=0;
        const bool noLevelMenu = InterlockedCompareExchange(&g_srMenuFrameInside,0,0)!=0;
        const bool bypassNGX = manualMenuBypass || noLevelMenu;
        if (bypassNGX != g_srMenuHandoffBypassWasActive) {
            g_srMenuHandoffBypassWasActive=bypassNGX;
            ++g_srMenuHandoffBypassTransitions;
            g_resetPending=true;
            g_lastEngineFrame=0;
            g_previousQpc.QuadPart=0;
            Log(L"SR_MENU_HANDOFF_TRANSITION active=%d reason=%ls "
                L"allocated=%ls ngx_feature=%ls history_reset_on_resume=1 transitions=%llu",
                bypassNGX?1:0,manualMenuBypass?L"MANUAL_PAUSE":
                (noLevelMenu?L"NATIVE_MAIN":L"RESUME_GAMEPLAY"),
                ModeName(g_srStartupMode),ModeName(g_activeMode),g_srMenuHandoffBypassTransitions);
        }
        if (v088Sample) QueryPerformanceCounter(&v088BeforeNgx);
        if (g_state == 1 && g_evaluationEnabled && !bypassNGX) {
            TryPresentDLAA(renderer, n);
        } else if (g_state == 1 && bypassNGX) {
            ++g_srMenuHandoffBypassFrames;
            if(g_srMenuHandoffBypassFrames <= 6 || g_srMenuHandoffBypassFrames % 600 == 0)
                Log(L"SR_MENU_HANDOFF_SKIPPED reason=%ls frame=%llu "
                    L"ngxeval=0 sr_redirect=0 original_engine_composite=1 "
                    L"scene_targets_unchanged=%ux%u display=%ux%u",
                    manualMenuBypass?L"MANUAL_PAUSE":L"NATIVE_MAIN",
                    g_srMenuHandoffBypassFrames,g_srStartupW,g_srStartupH,
                    g_srDisplayStartupW,g_srDisplayStartupH);
            if (noLevelMenu) {
                ++g_srMenuNgxSkipped;
                if (g_srMenuNgxSkipped <= 2)
                    Log(L"SR_MENU_NGX_SKIPPED frame=NO_LEVEL output=native");
            }
        }
        if (v088Sample) QueryPerformanceCounter(&v088AfterNgx);
    } else if (InterlockedCompareExchange(&g_otherThreadLogged, 1, 0) == 0) {
        Log(L"MULTITHREAD_WARNING: NGX limited to original renderer thread");
    }
    if (n % 1200 == 0 && g_ssfxSyncMode != 0) {
        Log(L"SSFX_SYNC_STATUS requested=%d installed=%d calls=%lld writes=%lld last_patched_frame=%u native_skips=%lld",
            g_ssfxSyncMode, g_ssfxSyncHookInstalled ? 1 : 0, g_ssfxCalls,
            g_ssfxWrites, g_ssfxLastPatchedFrame, g_ssfxSkippedExistingJitter);
    }
    if (n <= 5 || n % 1200 == 0) {
        Log(L"SR_MODE_STATUS requested=%ls allocated=%ls active=%ls actual_3d_scaled=%d planned_input=%ux%u output=%ux%u pending=%d",
            ModeName(g_requestedMode), ModeName(g_srStartupMode),ModeName(g_activeMode),g_activeMode!=0 ? 1:0,
            g_requestedMode ? g_srPlans[g_requestedMode].width : g_displayWidth,
            g_requestedMode ? g_srPlans[g_requestedMode].height : g_displayHeight,
            g_displayWidth,g_displayHeight,(g_requestedMode!=g_srStartupMode)?1:0);
    }
    if (n <= 5 || n % 1200 == 0) {
        Log(L"HOOK_CALL call=%lld renderer=%p ngx_state=%d enabled=%d run_frames=%u total=%llu fatal=%d",
            n, renderer, g_state, g_evaluationEnabled ? 1 : 0, g_runFrames,
            g_totalEvaluations, g_fatalEvaluationError ? 1 : 0);
    }
    if (g_srProbeEnabled && g_state == 1 && !g_srPreCaptured) {
        LogSRRendererResources(renderer);
        LogSROutputSnapshot(L"PRE_COMPOSITE", renderer);
        g_srPreCaptured = true;
    }
    // NGX evaluation swapped an isolated D3D11 context state and then
    // restored the game's original *low-resolution* viewport. Restore the
    // rasterizer's true native output area before the composite draw itself.
    // This also resets a stale 640x360 scissor rectangle, if present.
    ApplyNativeCompositeRasterState(L"BEFORE_COMPOSITE");
    // Only the original final composite is in scope. The PS hook leaves
    // all unrelated game and NVIDIA draw calls untouched.
    const bool handoffScope =
        InterlockedCompareExchange(&g_srMenuHandoffBypassArmed,0,0)==0 &&
        g_handoffResultReadyThisFrame &&
        g_psSetHookInstalled && MpEffectiveHandoffMode() != 0 &&
        g_handoffPresentedTexture && GetCurrentThreadId() == g_renderTid;
    if (handoffScope) {
        g_handoffSceneColor = GetMpTextureFromSlot(renderer, kColorSlot);
        g_handoffBindingsThisFrame = 0;
        InterlockedExchange(&g_insideGameComposite, 1);
    }
    // Disabled-DLSS baseline: sample the original composite on the SAME
    // GPU query path, but do not invoke NGX or modify the engine's rendering.
    if (!g_evaluationEnabled && !g_v083Current) {
        V083BeginSample(false, true);
        V083Mark(5);
    }
    LARGE_INTEGER v088BeforeComposite{},v088AfterComposite{};
    if (v088Sample) QueryPerformanceCounter(&v088BeforeComposite);
    if (g_original) g_original(renderer);
    if (g_v088DiagnosticsEnabled && (prior==0 || prior==current))
        InterlockedExchange64(&g_v088EvaluationCount,static_cast<LONG64>(g_totalEvaluations));
    if (v088Sample) {
        QueryPerformanceCounter(&v088AfterComposite);
        // Four independent sampled durations; publisher is this game's hook,
        // consumer is the file-only background worker. No render-thread log I/O.
        InterlockedExchange64(&g_v088HookTicks, v088AfterComposite.QuadPart-v088Begin.QuadPart);
        InterlockedExchange64(&g_v088SettingsTicks,
            v088PostSettings.QuadPart?v088PostSettings.QuadPart-v088Begin.QuadPart:0);
        InterlockedExchange64(&g_v088NgxTicks,
            v088AfterNgx.QuadPart&&v088BeforeNgx.QuadPart?
              v088AfterNgx.QuadPart-v088BeforeNgx.QuadPart:0);
        InterlockedExchange64(&g_v088CompositeTicks,
            v088AfterComposite.QuadPart-v088BeforeComposite.QuadPart);
        InterlockedExchange(&g_v088RenderProcessor,static_cast<LONG>(GetCurrentProcessorNumber()));
        InterlockedExchange(&g_v088State,g_state);
        LONG flags=(g_feature?1:0) | (g_evaluationEnabled?2:0) |
            (g_psSetHookInstalled?4:0) | (g_jitterHookActive?8:0) |
            (g_handoffResultReadyThisFrame?16:0) | (g_srScaleReady?32:0) |
            (g_fatalEvaluationError?64:0) | (g_handoffCopySkippedThisFrame?128:0);
        InterlockedExchange(&g_v088Flags,flags);
        InterlockedExchange(&g_v088HandoffStreak,static_cast<LONG>(g_handoffLastGoodFrames));
        InterlockedExchange(&g_v088AllocatedMode,g_srStartupMode);
        InterlockedExchange(&g_v088ActiveMode,g_activeMode);
        InterlockedExchange(&g_v088Preset,g_preset);
        InterlockedExchange64(&g_v088RenderCalls,n);
        InterlockedIncrement64(&g_v088SampleCount);
    }
    if (handoffScope) {
        InterlockedExchange(&g_insideGameComposite, 0);
        RestoreCompositeShaderBindings();
        if (g_handoffBindingsThisFrame > 0) {
            if (g_handoffLastGoodFrames < 1000000) ++g_handoffLastGoodFrames;
        } else {
            g_handoffLastGoodFrames = 0;
            // Cannot copy back after original composite; force native copy on
            // the next frame and record this frame's direct-path miss.
            g_handoffForceCopyNextFrame = true;
            if (g_handoffCopySkippedThisFrame)
                Log(L"SR_HANDOFF_DIRECT_MISS next_frame_native_fallback=1");
        }
        if (n <= 6 || n % 1200 == 0 ||
            (g_handoffBindingsThisFrame == 0 && g_handoffTotalBindings < 4 && n <= 30)) {
            Log(L"SR_HANDOFF_FRAME call=%lld mode=%d matched_srv_bindings=%u good_frames=%u copyback=%d total_redirects=%llu",
                n, g_handoffMode, g_handoffBindingsThisFrame, g_handoffLastGoodFrames,
                g_handoffCopySkippedThisFrame ? 0 : 1, g_handoffTotalBindings);
        }
        g_handoffSceneColor = nullptr;
        g_handoffPresentedTexture = nullptr;
    }
    // If the game's composite itself reinstated a low-resolution rasterizer
    // state, keep the HUD and subsequent native-resolution draws full size.
    // Record any such change: correcting *after* a draw cannot repair the
    // pixels already drawn, so a nonzero mismatch counter is actionable.
    ApplyNativeCompositeRasterState(L"AFTER_COMPOSITE");
    // Close the observational query AFTER the real engine composite. Query
    // results are read only on LATER frames with DONOTFLUSH and no polling loop.
    if (!g_v083Current || !g_v083Current->worldOnly) {
        V083Mark(6);
        V083CompleteSample();
    }
    if (g_srProbeEnabled && g_state == 1 && !g_srPostCaptured) {
        LogSROutputSnapshot(L"POST_COMPOSITE", renderer);
        g_srPostCaptured = true;
        Log(L"SR_PRESENT_CONTRACT native_input=%ux%u ngxdlaa_output=%ux%u post_target_unverified_swapchain=1",
            g_srNativeWidth, g_srNativeHeight, g_srNativeWidth, g_srNativeHeight);
    }
}


// MinHook may reject a target already modified by another mod, or reject a
// particular prologue. This fallback is ONLY allowed for the audited original
// six-byte prologue. Never chain over a foreign detour or unknown instructions.
static bool HasExpectedDecompressPrologue(void* target) {
    static constexpr BYTE expected[] = {
        0x40,0x57,0x48,0x83,0xEC,0x30,0xC5,0xFA,0x10,0x05,0x5E,0x04,0x64,0x01
    };
    BYTE runtime[sizeof(expected)]{};
    if (!SafeRead(reinterpret_cast<std::uintptr_t>(target), runtime, sizeof(runtime))) return false;
    if (std::memcmp(runtime, expected, sizeof(runtime)) == 0) return true;
    wchar_t hex[sizeof(runtime) * 4 + 1]{};
    for (unsigned i = 0; i < sizeof(runtime); ++i) {
        wchar_t temp[5]{};
        swprintf_s(temp, L"%02X ", static_cast<unsigned>(runtime[i]));
        wcscat_s(hex, temp);
    }
    Log(L"JITTER_FOREIGN_OR_CHANGED_PROLOGUE bytes=%ls REFUSING_PATCH", hex);
    return false;
}

static void* AllocateNear(void* target) {
    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    const std::uintptr_t gran = sys.dwAllocationGranularity;
    const auto center = reinterpret_cast<std::uintptr_t>(target) & ~(gran - 1);
    constexpr std::uintptr_t maxDistance = 0x60000000ULL; // comfortably < 2 GB
    for (std::uintptr_t delta = gran; delta < maxDistance; delta += gran) {
        const std::uintptr_t candidates[2] = {
            center >= delta ? center - delta : 0, center + delta
        };
        for (std::uintptr_t address : candidates) {
            if (!address) continue;
            void* allocation = VirtualAlloc(reinterpret_cast<void*>(address), 4096,
                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (allocation) return allocation;
        }
    }
    return nullptr;
}

static void AbsoluteJump(BYTE* location, std::uintptr_t destination) {
    location[0] = 0xFF; location[1] = 0x25;
    std::memset(location + 2, 0, 4);
    std::memcpy(location + 6, &destination, sizeof(destination));
}

// Pause other threads only for the six-byte patching window, similar to the
// strategy used by detour libraries. If any game thread can't be suspended,
// do not install the fallback patch.
static bool WriteAuditedDetour(void* target, BYTE* relay) {
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    std::vector<HANDLE> suspended;
    bool okay = true;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snapshot, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!thread) { okay = false; break; }
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                CloseHandle(thread); okay = false; break;
            }
            suspended.push_back(thread);
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread, &context)) { okay = false; break; }
            const auto ip = static_cast<std::uintptr_t>(context.Rip);
            const auto entry = reinterpret_cast<std::uintptr_t>(target);
            if (ip >= entry && ip < entry + 6) { okay = false; break; }
        } while (Thread32Next(snapshot, &te));
    } else okay = false;
    CloseHandle(snapshot);

    // NEVER attempt to overwrite another hook, even if it appeared after
    // our initial prologue check.
    static constexpr BYTE kExpectedPrologue[] = {
        0x40,0x57,0x48,0x83,0xEC,0x30,0xC5,0xFA,0x10,0x05,0x5E,0x04,0x64,0x01
    };
    if (okay && std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) == 0) {
        const auto p = reinterpret_cast<std::uintptr_t>(target);
        const auto d = reinterpret_cast<std::uintptr_t>(relay);
        const auto relative = static_cast<std::int64_t>(d) - static_cast<std::int64_t>(p + 5);
        if (relative < (std::numeric_limits<std::int32_t>::min)() ||
            relative > (std::numeric_limits<std::int32_t>::max)()) okay = false;
        else {
            DWORD oldProtect = 0;
            if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) okay = false;
            else {
                BYTE patch[6] = {0xE9,0,0,0,0,0x90};
                const std::int32_t rel32 = static_cast<std::int32_t>(relative);
                std::memcpy(patch + 1, &rel32, sizeof(rel32));
                std::memcpy(target, patch, sizeof(patch));
                FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
                DWORD unused = 0;
                VirtualProtect(target, 6, oldProtect, &unused);
            }
        }
    } else okay = false;

    for (auto it = suspended.rbegin(); it != suspended.rend(); ++it) {
        ResumeThread(*it);
        CloseHandle(*it);
    }
    return okay;
}

static bool InstallAuditedJitterFallback(void* target) {
    if (!HasExpectedDecompressPrologue(target)) return false;
    // Never name a variable "near": legacy Windows headers define that as a macro.
    BYTE* trampolineMemory = static_cast<BYTE*>(AllocateNear(target));
    if (!trampolineMemory) { Log(L"JITTER_FALLBACK_ALLOC_NEAR_FAILED"); return false; }
    // The donor-matched entry starts with PUSH RDI (2 bytes), SUB RSP,30h
    // (4 bytes). Neither instruction is RIP-relative, so a 6-byte trampoline
    // copies these exactly, then performs an absolute jump to original+6.
    std::memcpy(trampolineMemory, target, 6);
    AbsoluteJump(trampolineMemory + 6, reinterpret_cast<std::uintptr_t>(target) + 6);
    BYTE* relay = trampolineMemory + 64;
    AbsoluteJump(relay, reinterpret_cast<std::uintptr_t>(&DecompressHook));
    DWORD oldProtect = 0;
    if (!VirtualProtect(trampolineMemory, 4096, PAGE_EXECUTE_READ, &oldProtect)) {
        VirtualFree(trampolineMemory, 0, MEM_RELEASE);
        Log(L"JITTER_FALLBACK_RELAY_PROTECTION_FAILED windows_error=%lu", GetLastError());
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), trampolineMemory, 128);
    g_originalDecompress = reinterpret_cast<DecompressFn>(trampolineMemory);
    if (!WriteAuditedDetour(target, relay)) {
        g_originalDecompress = nullptr;
        VirtualFree(trampolineMemory, 0, MEM_RELEASE);
        Log(L"JITTER_FALLBACK_WRITE_REFUSED");
        return false;
    }
    g_manualJitterAlloc = trampolineMemory; // deliberately live until process exit
    Log(L"JITTER_HOOK_INSTALL_OK method=AUDITED_SIX_BYTE_DETOUR target=%p trampoline=%p relay=%p",
        target, trampolineMemory, relay);
    return true;
}

static bool InstallJitterHook(void* target) {
    if (!HasExpectedDecompressPrologue(target)) return false;
    MH_STATUS rc = MH_CreateHook(target, reinterpret_cast<void*>(&DecompressHook),
        reinterpret_cast<void**>(&g_originalDecompress));
    if (rc == MH_OK) {
        rc = MH_EnableHook(target);
        if (rc == MH_OK) {
            Log(L"JITTER_HOOK_INSTALL_OK method=MINHOOK target=%p", target);
            return true;
        }
        Log(L"JITTER_MINHOOK_ENABLE_FAILED status=%d", rc);
        MH_RemoveHook(target);
        g_originalDecompress = nullptr;
        return false;
    }
    Log(L"JITTER_MINHOOK_CREATE_FAILED status=%d", rc);
    if (rc == MH_ERROR_UNSUPPORTED_FUNCTION && g_enableManualJitterDetour)
        return InstallAuditedJitterFallback(target);
    return false;
}

// Called only during renderer/resource initialization. Log bounded data
// from the verified constructor thread, not from the hot D3D11 render path.
static void __fastcall NamedTargetAllocationAudit(void* slot, const char* name,
    unsigned width, unsigned height, unsigned format, unsigned config, unsigned char flag) {
    InterlockedIncrement64(&g_srAllocationCount);
    const uintptr_t address = reinterpret_cast<uintptr_t>(slot);
    const uintptr_t ctorBase = g_srConstructingRenderer;
    const bool insideCtor = ctorBase && g_srConstructorThread == GetCurrentThreadId() &&
        address >= ctorBase + 0x408 && address < ctorBase + 0x800 &&
        ((address - ctorBase) % 8) == 0;
    if (insideCtor) {
        const uintptr_t offset = address - ctorBase;
        const bool critical = offset == 0x438 || offset == 0x668 || offset == 0x728 ||
            offset == 0x430 || offset == 0x498 || offset == 0x448;
        const LONG seen = InterlockedIncrement(&g_srAllocationLogged);
        if (seen <= static_cast<LONG>(g_srAllocationLogLimit) || critical) {
            // All constructor names are statically stored inside the image's
            // .rdata section. Do not dereference dynamic/untrusted pointers.
            const uintptr_t p = reinterpret_cast<uintptr_t>(name);
            if (p >= g_moduleBase + 0x119F000 && p < g_moduleBase + 0x16B78F0) {
                Log(L"SR_ALLOC_NAMED target=0x%llX name=%hs size=%ux%u format=0x%X config=%u flag=%u critical=%u",
                    static_cast<unsigned long long>(offset), name, width, height, format, config,
                    static_cast<unsigned>(flag), critical ? 1u : 0u);
            } else {
                Log(L"SR_ALLOC_NAMED target=0x%llX name=OUTSIDE_RDATA size=%ux%u critical=%u",
                    static_cast<unsigned long long>(offset), width, height, critical ? 1u : 0u);
            }
        }
        if (critical) InterlockedIncrement(&g_srAllocationCriticalCount);
    }
    unsigned newW = width, newH = height;
    if (insideCtor && g_srScaleReady && name &&
        width >= 15 && height >= 15 &&
        width <= g_srDisplayStartupW && height <= g_srDisplayStartupH) {
        // Keep independently-sized surfaces and native output/UI targets.
        // The entire scene/SSS chain is otherwise scaled together, including
        // half/quarter intermediates, to avoid mismatched MRT/depth geometry.
        const bool nativeGeneric01 = g_srNativeGeneric01OptIn &&
            ((g_srNativeGeneric0 && !_stricmp(name,"$user$generic0")) ||
             (g_srNativeGeneric1 && !_stricmp(name,"$user$generic1")));
        const bool nativeTarget = !_stricmp(name,"$user$ui") ||
            !_stricmp(name,"$user$viewport2") ||
            !_stricmp(name,"$user$scopeRT") || nativeGeneric01;
        if (nativeGeneric01)
            Log(L"SR_UI_NATIVE_GENERIC_PRESERVED slot=0x%llX name=%hs native=%ux%u scene=%ux%u",
                static_cast<unsigned long long>(address-ctorBase),name,width,height,
                g_srStartupW,g_srStartupH);
        const bool independent = !_stricmp(name,"$user$ssfx_water_waves") ||
            !_stricmp(name,"$user$smap_depth");
        if (!nativeTarget && !independent) {
            newW = static_cast<unsigned>((static_cast<unsigned long long>(width) *
                g_srStartupW + g_srDisplayStartupW/2) / g_srDisplayStartupW);
            newH = static_cast<unsigned>((static_cast<unsigned long long>(height) *
                g_srStartupH + g_srDisplayStartupH/2) / g_srDisplayStartupH);
            if (newW < 1) newW=1;
            if (newH < 1) newH=1;
            const uintptr_t offset = address - ctorBase;
            if (width==g_srDisplayStartupW && height==g_srDisplayStartupH) {
                if (offset==0x430) g_srCriticalMask |= 1u;
                if (offset==0x728) g_srCriticalMask |= 2u;
                if (offset==0x438) g_srCriticalMask |= 4u;
                if (offset==0x448) g_srCriticalMask |= 8u;
                if (offset==0x668) g_srCriticalMask |= 16u;
            }
            Log(L"SR_RESIZE_ALLOCATION slot=0x%llX name=%hs from=%ux%u to=%ux%u",
                static_cast<unsigned long long>(offset),name,width,height,newW,newH);
        }
    }
    if (g_originalNamedTargetAllocation)
        g_originalNamedTargetAllocation(slot, name, newW, newH, format, config, flag);
}

// Multiplayer native-first main-menu bootstrap. An early renderer constructor
// runs before any level/HUD exists: allocating reduced generic buffers at this
// moment permanently blurs the main menu. Force native allocations ONLY if a
// verified per-launch Lua request can restore the saved DLSS mode after loading.
// No NGX call, live COM swap, or engine-initiated restart occurs here.
static bool MpWriteMenuBootstrapRequest() {
    wchar_t app[MAX_PATH]{};
    const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",app,MAX_PATH);
    if(!n || n>=MAX_PATH-100) {
        Log(L"SR_MP_MENU_BOOTSTRAP_REFUSED reason=NO_LOCALAPPDATA");return false;
    }
    char token[64]{};
    const DWORD t=GetEnvironmentVariableA("ANOMALY_NGX_BOOTSTRAP_V078_TOKEN",token,
        static_cast<DWORD>(sizeof(token)));
    if(!t || t>=sizeof(token)) {
        Log(L"SR_MP_MENU_BOOTSTRAP_REFUSED reason=NO_LAUNCHER_TOKEN");return false;
    }
    for(DWORD i=0;i<t;++i)
        if(!((token[i]>='0' && token[i]<='9') || token[i]=='_')) {
            Log(L"SR_MP_MENU_BOOTSTRAP_REFUSED reason=INVALID_TOKEN");return false;
        }
    wchar_t dir[MAX_PATH]{};
    _snwprintf_s(dir,_countof(dir),_TRUNCATE,L"%ls\\anomaly_ngx",app);
    if(!CreateDirectoryW(dir,nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) {
        Log(L"SR_MP_MENU_BOOTSTRAP_REFUSED reason=CREATE_DIR error=%lu",GetLastError());
        return false;
    }
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path,_countof(path),_TRUNCATE,L"%ls\\mp_bootstrap_v080.request",dir);
    HANDLE f=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                         CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) {
        Log(L"SR_MP_MENU_BOOTSTRAP_REFUSED reason=OPEN error=%lu",GetLastError());return false;
    }
    char data[96]{};
    const int length=sprintf_s(data,"pending %s\n",token);
    DWORD written=0;
    const BOOL ok=length>0 && WriteFile(f,data,static_cast<DWORD>(length),&written,nullptr);
    CloseHandle(f);
    if(!ok || written!=static_cast<DWORD>(length)) {
        DeleteFileW(path);Log(L"SR_MP_MENU_BOOTSTRAP_REFUSED reason=WRITE");return false;
    }
    InterlockedIncrement64(&g_mpBootstrapPendingWrites);
    Log(L"SR_MP_MENU_BOOTSTRAP_PENDING count=%lld path=%ls saved_mode_unchanged=1",
        InterlockedCompareExchange64(&g_mpBootstrapPendingWrites,0,0),path);
    return true;
}

// Poll the multiplayer Lua bootstrap's receipt from the already-established
// frame hook. Never call vid_restart from C++ or treat a receipt as proof that
// the renderer was actually rebuilt (the constructor log proves that).
static void MpPollMenuBootstrapAck() {
    if (!g_mpNativeMenusEnabled || !g_mpBootstrapAckPath[0]) return;
    static DWORD lastFallbackTick = 0;
    if (!MpShouldProbeMarker(&g_mpBackgroundBootstrapDetected, lastFallbackTick)) return;
    HANDLE f=CreateFileW(g_mpBootstrapAckPath,GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (f==INVALID_HANDLE_VALUE) return;
    char data[160]{}; DWORD n=0;
    const BOOL ok=ReadFile(f,data,sizeof(data)-1,&n,nullptr);
    CloseHandle(f);
    if (!ok || !n || !DeleteFileW(g_mpBootstrapAckPath)) return;
    char status[40]{}, token[64]{};
    if (sscanf_s(data,"%39s %63s",status,(unsigned)_countof(status),
                 token,(unsigned)_countof(token)) != 2) return;
    // Match the unique per-process launcher token, never a previous session.
    char expected[64]{};
    const DWORD t=GetEnvironmentVariableA("ANOMALY_NGX_BOOTSTRAP_V078_TOKEN",
        expected,static_cast<DWORD>(sizeof(expected)));
    if(!t || t>=sizeof(expected) || strcmp(expected,token)!=0) {
        Log(L"SR_MP_BOOTSTRAP_ACK_REJECT reason=TOKEN_MISMATCH");return;
    }
    const LONG64 total=InterlockedIncrement64(&g_mpBootstrapAckEvents);
    Log(L"SR_MP_BOOTSTRAP_LUA_ACK status=%hs token_verified=1 events=%lld "
        L"allocated=%ls requested=%ls constructors=%lld",
        status,total,ModeName(g_srStartupMode),ModeName(g_requestedMode),
        InterlockedCompareExchange64(&g_srConstructorCount,0,0));
}

static void LogRendererConstructorStack(LONG64 n) {
    if (!g_srLogConstructorStacks || n > 12) return;
    void* frames[10]{};
    const USHORT count = CaptureStackBackTrace(1,10,frames,nullptr);
    Log(L"SR_CTOR_STACK_BEGIN call=%lld frames=%u thread=%lu",n,static_cast<unsigned>(count),GetCurrentThreadId());
    for (USHORT i=0;i<count;++i) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]);
        if (address >= g_moduleBase && address < g_moduleBase+0x2000000)
            Log(L"SR_CTOR_STACK call=%lld index=%u exe_rva=0x%llX",n,static_cast<unsigned>(i),
                static_cast<unsigned long long>(address-g_moduleBase));
        else Log(L"SR_CTOR_STACK call=%lld index=%u other_module_address=%p",n,static_cast<unsigned>(i),frames[i]);
    }
}

static void* __fastcall RendererConstructorAudit(void* renderer) {
    if (InterlockedExchange(&g_srPauseSidecarArmed,0)!=0)
        Log(L"SR_PAUSE_SIDECAR_AUTO_DISARM reason=RENDERER_RECONSTRUCTION");
    // Constructor may execute on a different thread. Keep resources until
    // process exit or a safe render-thread rebuild; no stale sidecar is used.
    // Never carry a manually armed UI draw experiment across vid_restart.
    if (InterlockedExchange(&g_srMenuHandoffBypassArmed,0)!=0)
        Log(L"SR_MENU_BYPASS_DISARM reason=RENDERER_RECONSTRUCTION request_new_pause_probe=1");
    if (InterlockedExchange(&g_srPauseUiDrawArmed,0)!=0)
        Log(L"SR_PAUSE_UI_PROBE_AUTO_DISARM reason=RENDERER_RECONSTRUCTION");
    const LONG64 n = InterlockedIncrement64(&g_srConstructorCount);
    const DWORD renderThread = GetCurrentThreadId();
    LogRendererConstructorStack(n);
    const uintptr_t oldRenderer = g_srConstructingRenderer;
    const DWORD oldThread = g_srConstructorThread;
    g_srConstructingRenderer = reinterpret_cast<uintptr_t>(renderer);
    g_srConstructorThread = renderThread;
    InterlockedExchange(&g_srConstructorBusy,1);
    const unsigned displayW = MpHotReadOr<unsigned>(g_moduleBase + kWidthRva);
    const unsigned displayH = MpHotReadOr<unsigned>(g_moduleBase + kHeightRva);
    if (g_srReconfigureAtConstructor) {
        // This is the only point where we change allocation dimensions. The
        // engine owns the renderer lifecycle, unlike the failed v040 bank.
        // Refresh only the requested MODE from disk, avoiding NGX API calls
        // within a renderer constructor or a stale 500-ms settings poll.
        wchar_t requestText[40]{};
        if (g_settingsFile[0]) {
            GetPrivateProfileStringW(L"DLAA",L"upscaler_mode",L"dlaa",
                requestText,_countof(requestText),g_settingsFile);
            const int diskMode = (g_pauseAutoEnabled && g_pauseAutoActive)
                ? 0 : ParseMode(requestText);
            if (diskMode != g_requestedMode) {
                Log(L"SR_CTOR_READ_SETTINGS requested=%ls cached=%ls",ModeName(diskMode),ModeName(g_requestedMode));
                g_requestedMode=diskMode;
            }
        }
        // Main menu is a real native render phase, not an upscaled 3D frame.
        // Preserve the saved quality mode but allocate native render targets
        // while no level/HUD exists. Lua will perform one engine-owned
        // vid_restart after an actor starts updating, using the saved mode.
        // If token handshake is unavailable, keep v078 allocation behavior.
        const bool noPlayableLevel =
            MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva)==0 ||
            MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva)==0;
        const bool mpMenuBootstrap = g_mpNativeMenusEnabled && g_mpMainMenuNativeFirst &&
            noPlayableLevel && g_requestedMode>=1 && g_requestedMode<=4 &&
            g_srScaleOptIn && g_srFullFrameHookInstalled &&
            g_srAllocationHookInstalled && g_autoEnable && g_handoffMode==2 &&
            displayW==1920 && displayH==1080 && MpWriteMenuBootstrapRequest();
        if(mpMenuBootstrap) InterlockedIncrement64(&g_mpBootstrapNativeCtors);
        const int oldAllocated = g_srStartupMode;
        const bool admissible = g_srScaleOptIn && g_srFullFrameHookInstalled &&
            g_srAllocationHookInstalled && g_autoEnable && g_handoffMode == 2 &&
            displayW == 1920 && displayH == 1080;
        const int newAllocated = (admissible && !mpMenuBootstrap && g_requestedMode>=1 && g_requestedMode<=4)
            ? g_requestedMode : 0;
        if(mpMenuBootstrap) Log(L"SR_MP_MENU_CTOR_NATIVE call=%lld preferred=%ls allocation=DLAA level=0 pending_gameplay_restart=1",
            n,ModeName(g_requestedMode));
        static constexpr unsigned widths[] = {1920,1280,1114,960,640};
        static constexpr unsigned heights[] = {1080,720,626,540,360};
        g_srStartupMode=newAllocated;
        g_srStartupW=widths[newAllocated];
        g_srStartupH=heights[newAllocated];
        g_srDisplayStartupW=displayW;
        g_srDisplayStartupH=displayH;
        g_srScaleReady=newAllocated!=0;
        g_srCriticalMask=0;
        g_srConstructorSawAll=false;
        g_resetPending=true;
        g_lastEngineFrame=0;
        g_lastPatchedFrame=0;
        g_ssfxLastPatchedFrame=0;
        g_handoffLastGoodFrames=0;
        g_handoffForceCopyNextFrame=true;
        const LONG64 generation=InterlockedIncrement64(&g_srTransitionGeneration);
        Log(L"SR_CTOR_MODE_ADOPT call=%lld generation=%lld requested=%ls previous=%ls allocation=%ls internal=%ux%u native=%ux%u scaling_enabled=%d permitted=%d",
            n,generation,ModeName(g_requestedMode),ModeName(oldAllocated),ModeName(newAllocated),
            g_srStartupW,g_srStartupH,displayW,displayH,g_srScaleOptIn?1:0,admissible?1:0);
        if (g_requestedMode!=newAllocated) {
            if(mpMenuBootstrap) Log(L"SR_MP_MENU_NATIVE_DEFERRED requested=%ls allocated=DLAA reason=AWAIT_LUA_GAMEPLAY_REBUILD",
                ModeName(g_requestedMode));
            else Log(L"SR_CTOR_MODE_REFUSED requested=%ls chosen=%ls reason=UNSAFE_SCALING_CONTRACT",
                ModeName(g_requestedMode),ModeName(newAllocated));
        }
    } else if (g_srScaleOptIn && !g_srScaleReady && n == 1 && g_srFullFrameHookInstalled &&
        g_srAllocationHookInstalled && g_requestedMode >= 1 && g_requestedMode <= 4 &&
        g_autoEnable && displayW == 1920 && displayH == 1080 && g_handoffMode == 2) {
        static constexpr unsigned widths[]  = {1920,1280,1114,960,640};
        static constexpr unsigned heights[] = {1080,720,626,540,360};
        g_srStartupMode = g_requestedMode;
        g_srStartupW = widths[g_srStartupMode];
        g_srStartupH = heights[g_srStartupMode];
        g_srDisplayStartupW = displayW;
        g_srDisplayStartupH = displayH;
        g_srCriticalMask=0;
        g_srScaleReady=true;
        Log(L"SR_STARTUP_RESIZE_ENABLED mode=%ls scene=%ux%u display=%ux%u",
            ModeName(g_srStartupMode),g_srStartupW,g_srStartupH,displayW,displayH);
    } else if (g_srScaleOptIn && !g_srScaleReady && n == 1) {
        Log(L"SR_STARTUP_RESIZE_REFUSED requested=%ls enabled=%d display=%ux%u frame_hook=%d allocation_hook=%d handoff=%d",
            ModeName(g_requestedMode),g_autoEnable?1:0,displayW,displayH,
            g_srFullFrameHookInstalled?1:0,g_srAllocationHookInstalled?1:0,g_handoffMode);
    }
    Log(L"SR_ALLOC_CTOR_ENTER call=%lld renderer=%p global_size=%ux%u thread=%lu scale=%d",
        n,renderer,displayW,displayH,renderThread,g_srScaleReady?1:0);
    void* result = g_originalRendererConstructor ? g_originalRendererConstructor(renderer) : nullptr;
    g_srConstructorSawAll = g_srScaleReady && (g_srCriticalMask & 31u) == 31u;
    Log(L"SR_ALLOC_CTOR_EXIT call=%lld renderer=%p allocation_calls=%lld critical=%ld resized_mask=0x%X critical_complete=%d",
        n,renderer,g_srAllocationCount,g_srAllocationCriticalCount,g_srCriticalMask,g_srConstructorSawAll?1:0);
    if (g_srReconfigureAtConstructor) Log(L"SR_MODE_REBUILD_FINISHED generation=%lld allocated=%ls critical_complete=%d requested=%ls next=VALIDATE_INPUT_TEXTURES",
        g_srTransitionGeneration,ModeName(g_srStartupMode),g_srConstructorSawAll?1:0,ModeName(g_requestedMode));
    g_srLatestCompletedRenderer=reinterpret_cast<uintptr_t>(renderer);
    g_srConstructingRenderer=oldRenderer;
    g_srConstructorThread=oldThread;
    InterlockedExchange(&g_srConstructorBusy,0);
    return result;
}

// Declared here because the diagnostic implementation follows the frame hook.
static void LogVideoRestartDispatchCandidate();

// One-in-N GPU timestamp around the ORIGINAL world-frame function, including
// its nested renderer composite. A world-only query suppresses overlapping MP
// NGX queries, so there is never a nested timestamp-disjoint window.
struct V083WorldGpuScope {
    bool active=false;
    V083WorldGpuScope() {
        if (!g_profilerEnabled || g_gammaNativeTrialActive || !g_ngxContext ||
            !g_ngxDevice || !g_renderTid || GetCurrentThreadId()!=g_renderTid ||
            !g_moduleBase ||
            MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva)==0 ||
            MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva)==0) return;
        V083BeginSample(false,!g_evaluationEnabled,true);
        active=g_v083Current && g_v083Current->worldOnly;
        if (active) V083Mark(0);
    }
    ~V083WorldGpuScope() {
        if (!active) return;
        if (g_v083Current && g_v083Current->worldOnly) {
            V083Mark(6);
            V083CompleteSample();
        }
    }
};

static void __fastcall FullFrameResolutionHook(void* scene) {
    V083HotHookScope v083WorldCpu(kV083WorldFrame);
    V083WorldGpuScope v083WorldGpu;
    if (!g_originalFullFrame) return;
    PauseDiagFrameStart();
    PauseAutoPoll();
    MpPollMenuBootstrapAck();
    if (g_srMenuActivityDiagEnabled) InterlockedIncrement64(&g_srDiagFullFrames);
    if (g_srResetProbeEnabled) LogVideoRestartDispatchCandidate();
    PollUIPassTraceMarker();
    const LONG remaining = InterlockedCompareExchange(&g_srResetWindow,0,0);
    if (remaining > 0 && InterlockedDecrement(&g_srResetWindow)==0)
        Log(L"SR_RESET_WINDOW_COMPLETE manager_calls=%lld ctor_calls=%lld active_mode=%ls",
            g_srManagerCallsAfterReset,g_srConstructorCount,ModeName(g_srStartupMode));
    // FUN_1402C2730 checks these two pointers before taking its native 2D
    // / menu branch (calls FUN_14027FDE0 and exits early). Checking them at
    // frame entry keeps the original's native width/height contract intact.
    if (g_srNativeMenuOptIn && g_srScaleReady && g_srConstructorSawAll) {
        const std::uintptr_t activeLevel = MpHotReadOr<std::uintptr_t>(g_moduleBase+kActiveLevelRva);
        const std::uintptr_t hudRenderer = MpHotReadOr<std::uintptr_t>(g_moduleBase+kHudRenderRva);
        const bool noLevelPass = activeLevel == 0 || hudRenderer == 0;
        if (!g_srMenuStateInitialized || noLevelPass != g_srWasMenu) {
            g_srMenuStateInitialized = true;
            g_srWasMenu = noLevelPass;
            Log(L"SR_LEVEL_GATE_CHANGE path=%ls level=%p hud=%p native=%ux%u scene=%ux%u",
                noLevelPass ? L"NATIVE_MENU_2D" : L"SCALED_GAMEPLAY_3D",
                reinterpret_cast<void*>(activeLevel),reinterpret_cast<void*>(hudRenderer),
                g_srDisplayStartupW,g_srDisplayStartupH,g_srStartupW,g_srStartupH);
            g_resetPending = true;
            g_lastEngineFrame = 0;
            g_lastPatchedFrame = 0;
            g_ssfxLastPatchedFrame = 0;
        }
        if (noLevelPass) {
            // At frame entry the globals must be native; ensure the prior
            // frame cannot have leaked the internal dimensions. No engine
            // render targets are recreated or rewritten in this branch.
            *reinterpret_cast<volatile unsigned*>(g_moduleBase+kWidthRva) = g_srDisplayStartupW;
            *reinterpret_cast<volatile unsigned*>(g_moduleBase+kHeightRva) = g_srDisplayStartupH;
            InterlockedExchange(&g_srMenuFrameInside, 1);
            ++g_srMenuFrames;
            if (g_srMenuFrames <= 5 || g_srMenuFrames % 1200 == 0)
                Log(L"SR_NATIVE_MENU_FRAME count=%llu level=%p hud=%p display=%ux%u unchanged_targets=1",
                    g_srMenuFrames,reinterpret_cast<void*>(activeLevel),
                    reinterpret_cast<void*>(hudRenderer),g_srDisplayStartupW,g_srDisplayStartupH);
            // The game renders its menu UI via the 2D-only branch. NGX must
            // not consume a stale low-resolution scene as a new world frame.
            g_originalFullFrame(scene);
            InterlockedExchange(&g_srMenuFrameInside, 0);
            return;
        }
    }
    if (!g_srScaleReady || !g_srConstructorSawAll ||
        InterlockedCompareExchange(&g_srFrameInside,1,0) != 0) {
        g_originalFullFrame(scene);
        return;
    }
    const unsigned nativeW=MpHotReadOr<unsigned>(g_moduleBase+kWidthRva);
    const unsigned nativeH=MpHotReadOr<unsigned>(g_moduleBase+kHeightRva);
    if (nativeW!=g_srDisplayStartupW || nativeH!=g_srDisplayStartupH) {
        InterlockedExchange(&g_srFrameInside,0);
        if (g_srFrameHooks < 5)
            Log(L"SR_FRAME_RESIZE_SKIPPED native_globals_unexpected=%ux%u",nativeW,nativeH);
        g_originalFullFrame(scene);
        return;
    }
    InterlockedExchange(&g_srFrameRestoredBeforeComposite,0);
    *reinterpret_cast<volatile unsigned*>(g_moduleBase+kWidthRva) = g_srStartupW;
    *reinterpret_cast<volatile unsigned*>(g_moduleBase+kHeightRva) = g_srStartupH;
    InterlockedExchange(&g_srGlobalsReduced,1);
    ++g_srFrameHooks;
    if (g_srFrameHooks<=5 || g_srFrameHooks%1200==0)
        Log(L"SR_FRAME_SCENE_RESOLUTION frame=%lld input=%ux%u output=%ux%u",
            g_srFrameHooks,g_srStartupW,g_srStartupH,nativeW,nativeH);
    g_originalFullFrame(scene);
    // The composite should already have restored them, but never leak the
    // internal resolution into the UI/game logic if that call was skipped.
    *reinterpret_cast<volatile unsigned*>(g_moduleBase+kWidthRva) = nativeW;
    *reinterpret_cast<volatile unsigned*>(g_moduleBase+kHeightRva) = nativeH;
    InterlockedExchange(&g_srGlobalsReduced,0);
    InterlockedExchange(&g_srFrameInside,0);
}

// Observe renderer resets without modifying target dimensions or resource ownership.
static void __fastcall SrVideoRestartProbe() {
    const LONG64 n = InterlockedIncrement64(&g_srVideoRestartCalls);
    InterlockedExchange(&g_srResetActive,1);
    InterlockedExchange64(&g_srResetCtorAtBegin,InterlockedCompareExchange64(&g_srConstructorCount,0,0));
    InterlockedExchange(&g_srResetDLAAHold,1); // GPU reset invalidates NGX feature/context state; fail closed
    Log(L"SR_VIDEO_RESTART_NGX_SUSPENDED restart_game_to_resume_DLAA=1");
    InterlockedExchange(&g_srResetWindow,240); // keep auditing deferred allocations
    InterlockedExchange64(&g_srManagerCallsAfterReset,0);
    InterlockedExchange(&g_srManagerLoggedAfterReset,0);
    Log(L"SR_VIDEO_RESTART_BEGIN call=%lld frame_hooks=%lld scale_ready=%d startup_mode=%ls requested_mode=%ls",
        n,g_srFrameHooks,g_srScaleReady?1:0,ModeName(g_srStartupMode),ModeName(g_requestedMode));
    if (g_originalEngineVideoRestart) g_originalEngineVideoRestart();
    InterlockedExchange(&g_srResetActive,0);
    Log(L"SR_VIDEO_RESTART_RETURN call=%lld manager_calls_during_reset=%lld ctor_calls=%lld critical_mask=0x%X",
        n,g_srManagerCallsAfterReset,g_srConstructorCount,g_srCriticalMask);
}
static void* __fastcall SrTargetManagerProbe(void* manager,const char* name,
    unsigned width,unsigned height,unsigned format,unsigned config,unsigned char flag) {
    const bool window = InterlockedCompareExchange(&g_srResetActive,0,0) != 0 ||
                        InterlockedCompareExchange(&g_srResetWindow,0,0) > 0;
    if (window) {
        InterlockedIncrement64(&g_srManagerCallsAfterReset);
        const LONG logged = InterlockedIncrement(&g_srManagerLoggedAfterReset);
        if (logged <= 100) {
            const uintptr_t ptr = reinterpret_cast<uintptr_t>(name);
            // Observe the name only when it points to the EXE's known .rdata range.
            if (ptr >= g_moduleBase + 0x119F000 && ptr < g_moduleBase + 0x16B78F0)
                Log(L"SR_RESET_MANAGER_TARGET name=%hs width=%u height=%u format=0x%X active=%d sample=%ld",
                    name,width,height,format,InterlockedCompareExchange(&g_srResetActive,0,0)!=0,logged);
            else
                Log(L"SR_RESET_MANAGER_TARGET name=NON_IMAGE_STRING width=%u height=%u format=0x%X active=%d sample=%ld",
                    width,height,format,InterlockedCompareExchange(&g_srResetActive,0,0)!=0,logged);
        }
    }
    return g_originalTargetManager ? g_originalTargetManager(manager,name,width,height,format,config,flag) : nullptr;
}
static void LogVideoRestartDispatchCandidate() {
    if (InterlockedCompareExchange(&g_srRestartVtableLogged,1,0)!=0) return;
    // `_DAT_141859500` is used by vid_restart's handler to dispatch a
    // virtual method at offset 0x1D0. Log candidates only, never invoke them.
    const uintptr_t root=MpHotReadOr<uintptr_t>(g_moduleBase+0x1859500);
    const uintptr_t table=MpHotReadOr<uintptr_t>(root);
    const uintptr_t fn=MpHotReadOr<uintptr_t>(table+0x1D0);
    Log(L"SR_VIDEO_RESET_DISPATCH_CANDIDATE root=%p table=%p method=%p exe_rva=0x%llX observed_only=1",
        reinterpret_cast<void*>(root),reinterpret_cast<void*>(table),reinterpret_cast<void*>(fn),
        (fn>=g_moduleBase && fn<g_moduleBase+0x2000000) ?
            static_cast<unsigned long long>(fn-g_moduleBase) : 0ULL);
}

static void InstallEngineResetProbe(uintptr_t base) {
    if (!g_srResetProbeEnabled) return;
    struct Target {uintptr_t rva; const BYTE* sig; size_t len; void* detour; void** original; bool* flag; const wchar_t* label;};
    static constexpr BYTE restartSignature[] = {0x48,0x83,0xEC,0x28,0x48,0x8D,0x0D};
    static constexpr BYTE managerSignature[] = {0x48,0x89,0x6C,0x24,0x18,0x56,0x41,0x54,0x41,0x55};
    Target hooks[] = {
        {kEngineVideoRestartRva,restartSignature,sizeof(restartSignature),reinterpret_cast<void*>(&SrVideoRestartProbe),
            reinterpret_cast<void**>(&g_originalEngineVideoRestart),&g_srResetHookInstalled,L"VIDEO_RESTART"},
        {kEngineTargetManagerRva,managerSignature,sizeof(managerSignature),reinterpret_cast<void*>(&SrTargetManagerProbe),
            reinterpret_cast<void**>(&g_originalTargetManager),&g_srTargetManagerHookInstalled,L"TARGET_MANAGER"}
    };
    for (const auto& h : hooks) {
        BYTE observed[16]{};
        if (!SafeRead(base+h.rva,observed,h.len) || std::memcmp(observed,h.sig,h.len)!=0) {
            Log(L"SR_RESET_PROBE_HOOK name=%ls installed=0 reason=SIGNATURE_MISMATCH",h.label);
            continue;
        }
        void* target=reinterpret_cast<void*>(base+h.rva);
        MH_STATUS rc=MH_CreateHook(target,h.detour,h.original);
        if (rc==MH_OK) {
            rc=MH_EnableHook(target);
            if (rc==MH_OK) *h.flag=true;
            else MH_RemoveHook(target);
        }
        Log(L"SR_RESET_PROBE_HOOK name=%ls installed=%d status=%d rva=0x%llX",
            h.label,*h.flag?1:0,rc,static_cast<unsigned long long>(h.rva));
    }
}

static void InstallFullFrameHook(std::uintptr_t base) {
    if (!g_srScaleOptIn && !g_srResetProbeEnabled) return;
    static constexpr BYTE expected[] = {
        0x48,0x8B,0xC4,0x55,0x41,0x54,0x48,0x8D,0xA8,0x68,0xFF,0xFF,0xFF
    };
    BYTE observed[sizeof(expected)]{};
    if (!SafeRead(base+kFullFrameRva,observed,sizeof(observed)) ||
        std::memcmp(observed,expected,sizeof(expected))!=0) {
        Log(L"SR_FRAME_HOOK_SIGNATURE_MISMATCH disabled=1");
        return;
    }
    void* target = reinterpret_cast<void*>(base+kFullFrameRva);
    MH_STATUS rc = MH_CreateHook(target,reinterpret_cast<void*>(&FullFrameResolutionHook),
                                reinterpret_cast<void**>(&g_originalFullFrame));
    if (rc==MH_OK) {
        rc=MH_EnableHook(target);
        if (rc==MH_OK) g_srFullFrameHookInstalled=true;
        else MH_RemoveHook(target);
    }
    Log(L"SR_FRAME_HOOK installed=%d status=%d rva=0x%X",
        g_srFullFrameHookInstalled?1:0,rc,static_cast<unsigned>(kFullFrameRva));
}

static void InstallSRAllocationAudit(std::uintptr_t base) {
    if (!g_srAllocationAuditEnabled) return;
    const BYTE ctorSignature[] = {0x48,0x89,0x4C,0x24,0x08,0x55,0x53,0x56,0x57};
    const BYTE allocSignature[] = {0x48,0x89,0x5C,0x24,0x08,0x57,0x48,0x83,0xEC,0x40};
    BYTE observed[16]{};
    if (!SafeRead(base + kRendererConstructorRva, observed, sizeof(ctorSignature)) ||
        std::memcmp(observed, ctorSignature, sizeof(ctorSignature)) != 0 ||
        !SafeRead(base + kNamedTargetAllocationRva, observed, sizeof(allocSignature)) ||
        std::memcmp(observed, allocSignature, sizeof(allocSignature)) != 0) {
        Log(L"SR_ALLOC_AUDIT_SIGNATURE_MISMATCH no_allocation_hooks_installed");
        return;
    }
    // Install the target-creation observer before the constructor observer,
    // so even very fast constructor calls cannot reach an unhooked allocator.
    void* alloc = reinterpret_cast<void*>(base + kNamedTargetAllocationRva);
    MH_STATUS rc = MH_CreateHook(alloc, reinterpret_cast<void*>(&NamedTargetAllocationAudit),
                                reinterpret_cast<void**>(&g_originalNamedTargetAllocation));
    if (rc == MH_OK) {
        rc = MH_EnableHook(alloc);
        if (rc == MH_OK) g_srAllocationHookInstalled = true;
        else MH_RemoveHook(alloc);
    }
    Log(L"SR_ALLOC_HOOK allocator=%d status=%d rva=0x%X opt_in_resize=%d",
        g_srAllocationHookInstalled ? 1 : 0, rc, static_cast<unsigned>(kNamedTargetAllocationRva),
        g_srScaleOptIn?1:0);
    if (!g_srAllocationHookInstalled) return;
    void* constructor = reinterpret_cast<void*>(base + kRendererConstructorRva);
    rc = MH_CreateHook(constructor, reinterpret_cast<void*>(&RendererConstructorAudit),
                       reinterpret_cast<void**>(&g_originalRendererConstructor));
    if (rc == MH_OK) {
        rc = MH_EnableHook(constructor);
        if (rc == MH_OK) g_srConstructorHookInstalled = true;
        else MH_RemoveHook(constructor);
    }
    Log(L"SR_ALLOC_HOOK constructor=%d status=%d rva=0x%X opt_in_resize=%d",
        g_srConstructorHookInstalled ? 1 : 0, rc, static_cast<unsigned>(kRendererConstructorRva),
        g_srScaleOptIn?1:0);
}

// ------------------------------------------------------------
// v0.6.5: GAMMA executable profile and passive runtime hook probe.
// IMPORTANT: Gamma probes are never permitted to resize targets, alter
// globals/shader constants, call NGX, or trigger renderer reconstruction.
// The known multiplayer hash retains the proven v0.5.3 path below.
// ------------------------------------------------------------
static constexpr wchar_t kKnownMultiplayerSha256[] =
    L"6fa0446c26e14b246409027c6655f67e5adbdda17ccb6042d127bcaab7a5eb39";
static constexpr wchar_t kKnownGammaSha256[] =
    L"ade57887f93ca95e9398da60b374035b50e4c38295b9802a6f89bc33a0394bc6";

enum class SrExeProfile { Unsupported, MultiplayerV053, GammaPassiveV077 };

static bool SrComputeExeSha256(wchar_t (&hex)[65]) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE;
    if (ok) ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    BYTE buffer[65536]{};
    DWORD actual = 0;
    if (ok) {
        for (;;) {
            if (!ReadFile(file, buffer, sizeof(buffer), &actual, nullptr)) { ok = false; break; }
            if (!actual) break;
            if (!CryptHashData(hash, buffer, actual, 0)) { ok = false; break; }
        }
    }
    BYTE digest[32]{};
    DWORD length = sizeof(digest);
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) != FALSE && length == 32;
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    if (!ok) return false;
    for (unsigned i = 0; i < 32; ++i) swprintf_s(hex + 2*i, 65 - 2*i, L"%02x", unsigned(digest[i]));
    hex[64] = 0;
    return true;
}

struct GammaAob { const char* label; const char* signature; uintptr_t expectedRva; bool required; };
static constexpr GammaAob kGammaAobs[] = {
    {"constructor", "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 58 A3 F9", 0xAA86D0, true},
    {"allocator", "48 89 5C 24 08 57 48 83 EC 40 0F B6 84 24 80 00 00 00 48 8B D9 48 8B 0D ??", 0xB034B0, true},
    {"renderer", "48 8B C4 55 53 56 57 41 54 41 56 41 57 48 8D 68 A1 48 81 EC C0 00 00 00 48 8B 91 88 05 00 00 45 33 FF 44 89 7D 67 48 8B F9 C5 F8 29 70", 0xB9D9F0, true},
    {"full_frame", "40 55 48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 48 89 BC 24 A0 01 00", 0xAE4660, true},
    {"jitter", "40 57 48 83 EC 30 C5 FA 10 05 ?? ?? ?? ?? C5 FA 59 0D ?? ?? ?? ??", 0xA52C30, false},
    {"ssfx", "48 89 74 24 10 57 48 83 EC 30 C5 FA ?? ?? ?? ?? 7D 00 C5 E8 57 D2 C5 F8", 0xB73B00, false}
};

static bool GammaGetPe(std::uintptr_t base, const IMAGE_NT_HEADERS64*& nt,
                       const IMAGE_SECTION_HEADER*& sections) {
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return false;
    nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 32) return false;
    sections = IMAGE_FIRST_SECTION(nt);
    return true;
}

static bool GammaParsePattern(const char* pattern, std::vector<int>& bytes) {
    bytes.clear();
    for (const char* p = pattern; *p;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') { bytes.push_back(-1); p += 2; continue; }
        auto digit=[](char c)->int { if(c>='0'&&c<='9')return c-'0'; if(c>='A'&&c<='F')return c-'A'+10; if(c>='a'&&c<='f')return c-'a'+10; return -1; };
        if (!p[0] || !p[1] || digit(p[0])<0 || digit(p[1])<0) return false;
        bytes.push_back(digit(p[0])*16+digit(p[1])); p += 2;
    }
    return !bytes.empty();
}

// The module has already been authenticated by its whole-file SHA-256.
// Inspect only executable mapped PE sections, never the whole address space.
static unsigned GammaScanText(std::uintptr_t base, const char* aob, uintptr_t& foundRva) {
    const IMAGE_NT_HEADERS64* nt=nullptr;
    const IMAGE_SECTION_HEADER* sections=nullptr;
    if (!GammaGetPe(base, nt, sections)) return 0;
    std::vector<int> p;
    if (!GammaParsePattern(aob,p)) return 0;
    unsigned hits=0;
    const auto imageSize=nt->OptionalHeader.SizeOfImage;
    for (unsigned s=0; s<nt->FileHeader.NumberOfSections; ++s) {
        const auto& section=sections[s];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const auto rva=section.VirtualAddress;
        if (rva>=imageSize) continue;
        const SIZE_T size=std::min<SIZE_T>(section.Misc.VirtualSize,imageSize-rva);
        if (size<p.size()) continue;
        const BYTE* region=reinterpret_cast<const BYTE*>(base+rva);
        for (SIZE_T i=0; i+p.size()<=size; ++i) {
            if (p[0]>=0 && region[i]!=p[0]) continue;
            bool match=true;
            for (SIZE_T j=1;j<p.size();++j) if (p[j]>=0 && region[i+j]!=p[j]) {match=false;break;}
            if (match) { ++hits; foundRva=rva+i; if (hits>4) return hits; }
        }
    }
    return hits;
}

static bool GammaPdataStart(std::uintptr_t base, uintptr_t rva) {
    const IMAGE_NT_HEADERS64* nt=nullptr;
    const IMAGE_SECTION_HEADER* sections=nullptr;
    if (!GammaGetPe(base,nt,sections)) return false;
    const auto d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d.VirtualAddress || !d.Size || d.VirtualAddress+d.Size > nt->OptionalHeader.SizeOfImage) return false;
    const auto* entries=reinterpret_cast<const RUNTIME_FUNCTION*>(base+d.VirtualAddress);
    const unsigned count=d.Size/sizeof(RUNTIME_FUNCTION);
    for(unsigned i=0;i<count;++i) if(entries[i].BeginAddress==rva) return true;
    return false;
}

static bool GammaExactE8(std::uintptr_t base, uintptr_t callRva, uintptr_t destRva) {
    BYTE bytes[5]{};
    if (!SafeRead(base+callRva,bytes,sizeof(bytes)) || bytes[0]!=0xE8) return false;
    int32_t displacement=0;
    std::memcpy(&displacement,bytes+1,4);
    return static_cast<int64_t>(callRva)+5+displacement==static_cast<int64_t>(destRva);
}

static constexpr uintptr_t kGammaCtorRva = 0xAA86D0;
static constexpr uintptr_t kGammaAllocRva = 0xB034B0;
static constexpr uintptr_t kGammaRendererRva = 0xB9D9F0;
static constexpr uintptr_t kGammaRendererCallRva = 0xB3BB1A;
// v078: confirmed by three independent v070 call stacks and actual GAMMA E8 bytes.
// AE5F2C -> B38E70 is the active 3D renderer; B3DA60 -> B9F1A0 is
// its final output-composite entry. The inactive B9D9F0 hook is unchanged.
static constexpr uintptr_t kGammaActiveRendererRva = 0xB38E70;
static constexpr uintptr_t kGammaActiveRendererCallRva = 0xAE5F2C;
static constexpr uintptr_t kGammaFinalCompositeRva = 0xB9F1A0;
static constexpr uintptr_t kGammaFinalCompositeCallRva = 0xB3DA60;
static constexpr uintptr_t kGammaColorSlot = 0x438;
static constexpr uintptr_t kGammaMotionSlot = 0x668;
static constexpr uintptr_t kGammaDepthSlot = 0x728;

static RendererConstructorFn g_gammaOriginalCtor = nullptr;
static NamedTargetAllocationFn g_gammaOriginalAlloc = nullptr;
static RendererFn g_gammaOriginalRenderer = nullptr;
// GAMMA v078: observe the world-frame entry, which is independent of the
// optional renderer path seen at B3BB1A. D3D paths unchanged; engine-global width is scoped to verified composite.
static FullFrameFn g_gammaOriginalFullFrame = nullptr;
static volatile LONG64 g_gammaFullFrameCalls=0;
static std::uintptr_t g_gammaLatestCtorRenderer=0;
static constexpr uintptr_t kGammaFrameRva=0xAE4660;
static constexpr uintptr_t kGammaWidthRva=0x14D37E8;
static constexpr uintptr_t kGammaHeightRva=0x14D37EC;
static constexpr uintptr_t kGammaCounterRva=0x14D3808;
static constexpr uintptr_t kGammaLevelRva=0x14B6F10;
static constexpr uintptr_t kGammaHudRva=0x14B6E68;

using GammaFinalCompositeFn=void(__fastcall*)(void* renderer);
static GammaFinalCompositeFn g_gammaOriginalFinalComposite=nullptr;
static thread_local bool t_gammaScaledGlobalScope=false;
static thread_local bool t_gammaCompositeRestoredGlobals=false;
static LONG64 g_gammaFrameGlobalReduces=0;
static LONG64 g_gammaNativeCompositeRestores=0;
static LONG64 g_gammaFrameFallbackRestores=0;
static LONG64 g_gammaFinalCompositeCalls=0;
static LONG64 g_gammaUnexpectedDimensionRefusals=0;
static LONG64 g_gammaWorldUnscaledFrames=0;

static volatile LONG64 g_gammaCtorCalls=0;
// v078 detached-input DLAA/Quality: classify a candidate's *actual DXGI back-buffer
// ownership* before counting it toward the 48-frame stable composite proof.
// This explicitly fixes v060 rejecting two same-size draws in one frame.
// No injection into the conditional renderer
// entry, multiplayer engine hook preserved, and no gamma pause-mode toggles.
static bool g_gammaTrialEnabled = false;
static bool g_gammaTrialCopyback = false;
// GAMMA uses its own persistent detached-input NGX evaluator.
static bool g_gammaTrialHooksReady = false;
static bool g_gammaTrialRejected = false;
// v078: GAMMA-native renderer allocation scaling with native back-buffer draw raster.
// Mode is selected before the first constructor, and may only change after
// an engine-owned renderer reconstruction. Never downsample native input.
// v078: first renderer is deliberately native, even when the menu requests
// Quality. The one-time actor-owned vid_restart changes it to low-resolution
// targets after gameplay has initialized, reproducing v072's proven ordering.
static bool g_gammaBootstrapNativeFirst = false;
static bool g_gammaBootstrapPending = false;
static LONG64 g_gammaBootstrapNativeAttempts = 0;
static LONG64 g_gammaBootstrapQualityActivations = 0;
static bool GammaWriteBootstrapRequest() {
    wchar_t appData[MAX_PATH]{};
    const DWORD count=GetEnvironmentVariableW(L"LOCALAPPDATA",appData,MAX_PATH);
    if(count==0 || count>MAX_PATH-80) {
        Log(L"GAMMA_BOOTSTRAP_MARKER_FAILED reason=NO_LOCALAPPDATA");return false;
    }
    wchar_t dir[MAX_PATH]{};
    _snwprintf_s(dir,_countof(dir),_TRUNCATE,L"%ls\\anomaly_ngx",appData);
    if(!CreateDirectoryW(dir,nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS) {
        Log(L"GAMMA_BOOTSTRAP_MARKER_FAILED reason=CREATE_DIRECTORY code=%lu",GetLastError());return false;
    }
    wchar_t filename[MAX_PATH]{};
    _snwprintf_s(filename,_countof(filename),_TRUNCATE,L"%ls\\gamma_bootstrap_v078.request",dir);
    HANDLE file=CreateFileW(filename,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                            CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) {
        Log(L"GAMMA_BOOTSTRAP_MARKER_FAILED reason=OPEN code=%lu",GetLastError());return false;
    }
    char token[64]{};
    const DWORD tokenLen=GetEnvironmentVariableA("ANOMALY_NGX_BOOTSTRAP_V078_TOKEN",
                                                token,static_cast<DWORD>(sizeof(token)));
    if(tokenLen==0 || tokenLen>=sizeof(token)) {
        CloseHandle(file);
        DeleteFileW(filename);
        Log(L"GAMMA_BOOTSTRAP_MARKER_FAILED reason=NO_PER_LAUNCH_TOKEN");return false;
    }
    // Launcher produces only decimal digits and an underscore. Refuse other
    // strings so an unrelated environment value cannot activate Lua restart.
    for(DWORD i=0;i<tokenLen;++i) {
        const char c=token[i];
        if(!((c>='0'&&c<='9')||c=='_')) {
            CloseHandle(file);DeleteFileW(filename);
            Log(L"GAMMA_BOOTSTRAP_MARKER_FAILED reason=BAD_TOKEN");return false;
        }
    }
    char payload[96]{};
    const int length=sprintf_s(payload,"pending %s\n",token);
    DWORD written=0;
    const BOOL ok=length>0 && WriteFile(file,payload,static_cast<DWORD>(length),&written,nullptr);
    CloseHandle(file);
    if(!ok || written!=static_cast<DWORD>(length)) {
        DeleteFileW(filename);
        Log(L"GAMMA_BOOTSTRAP_MARKER_FAILED reason=WRITE");return false;
    }
    Log(L"GAMMA_BOOTSTRAP_MARKER_READY file=%ls",filename);
    return true;
}
// v078: identify verified GAMMA for Lua without assuming SetEnvironmentVariableW
// updates the C runtime's getenv() snapshot. The launcher establishes the
// per-process token BEFORE child creation; a stale marker cannot match it.
static bool GammaWriteMenuDeferralMarker() {
    wchar_t appData[MAX_PATH]{};
    const DWORD count=GetEnvironmentVariableW(L"LOCALAPPDATA",appData,MAX_PATH);
    if(!count || count>=MAX_PATH-90) return false;
    char token[64]{};
    const DWORD n=GetEnvironmentVariableA("ANOMALY_NGX_BOOTSTRAP_V078_TOKEN",
        token,static_cast<DWORD>(sizeof(token)));
    if(!n || n>=sizeof(token)) return false;
    for(DWORD i=0;i<n;++i)
        if(!((token[i]>='0' && token[i]<='9') || token[i]=='_')) return false;
    wchar_t dir[MAX_PATH]{};
    _snwprintf_s(dir,_countof(dir),_TRUNCATE,L"%ls\\anomaly_ngx",appData);
    if(!CreateDirectoryW(dir,nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
        return false;
    wchar_t path[MAX_PATH]{};
    _snwprintf_s(path,_countof(path),_TRUNCATE,L"%ls\\gamma_menu_v078.flag",dir);
    HANDLE f=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    char data[96]{};
    const int length=sprintf_s(data,"verified %s\n",token);
    DWORD written=0;
    const BOOL ok=length>0 && WriteFile(f,data,static_cast<DWORD>(length),&written,nullptr);
    CloseHandle(f);
    if(!ok || written!=static_cast<DWORD>(length)) {DeleteFileW(path);return false;}
    return true;
}
static int g_gammaSelectedMode = 0;     // allocated at current renderer constructor
// v078: per-launch, verified GAMMA-only pause menu native renderer mode.
// Lua owns menu lifecycle and vid_restart, while this engine-side adapter only
// selects the native renderer ALLOCATION MODE on the next constructor.
// It never rewrites settings.ini or changes live COM texture identities.
static bool g_gammaPauseNativeEnabled = true;
static bool g_gammaPauseRestartEnabled = true;
static bool g_gammaPauseNativeRequested = false;
static bool g_gammaPauseCtorNative = false;
static bool g_gammaPauseHadNativeCtor = false;
static DWORD g_gammaPauseLastPollMs = 0;
static LONG64 g_gammaPauseNativeFrames = 0;
static LONG64 g_gammaPauseMenuRebuilds = 0;
static LONG64 g_gammaPauseRestoredRebuilds = 0;
static bool GammaGetPauseToken(char (&token)[64]) {
    const DWORD n=GetEnvironmentVariableA("ANOMALY_NGX_BOOTSTRAP_V078_TOKEN",token,64);
    if(!n || n>=64) return false;
    for(DWORD i=0;i<n;++i) {
        const char c=token[i];
        if(!((c>='0'&&c<='9')||c=='_'))return false;
    }
    return true;
}
static bool GammaPausePath(wchar_t (&path)[MAX_PATH],const wchar_t* name) {
    wchar_t app[MAX_PATH]{};
    const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",app,MAX_PATH);
    if(!n || n>=MAX_PATH-96)return false;
    _snwprintf_s(path,_countof(path),_TRUNCATE,L"%ls\\anomaly_ngx\\%ls",app,name);
    return true;
}
static bool GammaWritePauseFlag() {
    wchar_t path[MAX_PATH]{};
    char token[64]{};
    if(!GammaPausePath(path,L"gamma_pause_v078.flag") || !GammaGetPauseToken(token))return false;
    wchar_t app[MAX_PATH]{};
    const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",app,MAX_PATH);
    if(!n || n>=MAX_PATH-96)return false;
    wchar_t dir[MAX_PATH]{};
    _snwprintf_s(dir,_countof(dir),_TRUNCATE,L"%ls\\anomaly_ngx",app);
    if(!CreateDirectoryW(dir,nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
        return false;
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    char data[120]{};
    const int length=sprintf_s(data,"verified %s %u\n",token,g_gammaPauseRestartEnabled?1u:0u);
    DWORD written=0;
    const BOOL ok=length>0 && WriteFile(file,data,DWORD(length),&written,nullptr);
    CloseHandle(file);
    return ok && written==DWORD(length);
}
static void GammaPollPauseCommand(bool force=false) {
    if(!g_gammaPauseNativeEnabled)return;
    const DWORD now=GetTickCount();
    // Poll promptly on CLOSE to avoid one gameplay frame with menu-native globals.
    // When inactive, 16 ms bounds filesystem probes without touching the render path.
    if(!force && !g_gammaPauseNativeRequested && DWORD(now-g_gammaPauseLastPollMs)<16)return;
    g_gammaPauseLastPollMs=now;
    char token[64]{};
    wchar_t path[MAX_PATH]{};
    if(!GammaGetPauseToken(token)||!GammaPausePath(path,L"gamma_pause_v078.command"))return;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    char data[160]{};
    DWORD n=0;
    const BOOL ok=ReadFile(file,data,sizeof(data)-1,&n,nullptr);
    CloseHandle(file);
    if(!ok || n==0)return;
    char requested[24]{},presented[64]{};
    if(sscanf_s(data,"%23s %63s",requested,(unsigned)_countof(requested),
                presented,(unsigned)_countof(presented))!=2)return;
    if(strcmp(presented,token)!=0)return; // stale previous-launch command
    const bool wantNative=strcmp(requested,"native")==0;
    if(!wantNative && strcmp(requested,"restore")!=0)return;
    if(g_gammaPauseNativeRequested!=wantNative) {
        g_gammaPauseNativeRequested=wantNative;
        Log(L"GAMMA_PAUSE_COMMAND state=%ls allocated_mode=%ls future_rebuild=%d token_verified=1",
            wantNative?L"NATIVE":L"RESTORE",ModeName(g_gammaSelectedMode),
            g_gammaPauseRestartEnabled?1:0);
    }
}
static void GammaWritePauseStatus() {
    if(!g_gammaPauseNativeEnabled)return;
    wchar_t path[MAX_PATH]{};
    char token[64]{};
    if(!GammaPausePath(path,L"gamma_pause_v078.status") || !GammaGetPauseToken(token))return;
    HANDLE file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    char data[160]{};
    const char* mode=g_gammaPauseCtorNative?"DLAA":
        (g_gammaSelectedMode==1?"QUALITY":g_gammaSelectedMode==2?"BALANCED":
        g_gammaSelectedMode==3?"PERFORMANCE":g_gammaSelectedMode==4?"ULTRA_PERFORMANCE":"DLAA");
    const int length=sprintf_s(data,"allocated %s %s\n",mode,token);
    DWORD written=0;
    if(length>0)WriteFile(file,data,DWORD(length),&written,nullptr);
    CloseHandle(file);
}
static bool g_gammaQualityRequested = false; // true for ANY low-res SR mode
static bool g_gammaQualityCtorActive = false;
static bool g_gammaQualityAllocated = false;
static unsigned g_gammaQualityCriticalMask = 0;
// Exact 1920x1080 optimal sizes from the MP-owned NGX mode query.
// At startup these are provisional; NGX's own plan verifies them before use.
static unsigned g_gammaInputW = 1920, g_gammaInputH = 1080;
static int g_gammaMenuRequestedMode = 0;// from SAME settings.ini used by menu/MP
static int g_gammaLastQueuedMode = -1;
static unsigned long g_gammaSettingsLastPoll = 0;
static bool g_gammaMenuEnabled = true;
static int g_gammaLastAppliedPreset = 0;
static bool g_gammaPresetRecreatePending = false;
// Engine globals follow the world-frame + verified final-composite boundary.
static constexpr unsigned kGammaKnownRasterMax = 53;
static ID3D11Texture2D* g_gammaKnownRasterTextures[kGammaKnownRasterMax]{}; // borrowed
static unsigned g_gammaKnownRasterRoles[kGammaKnownRasterMax]{}; // 1 scaled, 2 native
static unsigned g_gammaKnownRasterCount=0;
static unsigned g_gammaRasterCurrentRole=0;
static unsigned g_gammaRasterCurrentW=0,g_gammaRasterCurrentH=0;
static LONG64 g_gammaRasterOmBindings=0,g_gammaRasterViewportAdjusts=0;
static LONG64 g_gammaRasterScissorAdjusts=0,g_gammaRasterUnknownBinds=0;
static LONG64 g_gammaRasterViewportCalls=0,g_gammaRasterScissorCalls=0;
static LONG64 g_gammaRasterCompositeSamples=0;
using GammaRSSetViewportsFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,const D3D11_VIEWPORT*);
using GammaRSSetScissorRectsFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,const D3D11_RECT*);
static GammaRSSetViewportsFn g_gammaOriginalRSSetViewports=nullptr;
static GammaRSSetScissorRectsFn g_gammaOriginalRSSetScissorRects=nullptr;
static bool g_gammaRasterHooksReady=false;
// Unfiltered D3D11 counters run BEFORE GammaRasterScope; v069 incorrectly
// reported zero here even though the detours were installed successfully.
static volatile LONG64 g_gammaRawOmCalls=0;
static volatile LONG64 g_gammaRawViewportCalls=0;
static volatile LONG64 g_gammaRawScissorCalls=0;
static volatile LONG64 g_gammaRawIndexedSixCalls=0;
static unsigned g_gammaBoundarySampleGeneration=~0u;
static unsigned g_gammaBoundarySamplesThisGeneration=0;
static LONG64 g_gammaBoundarySamplesTotal=0;
static thread_local bool t_gammaWorldFrameInside = false;
// Set ONLY while the exact, hash-verified GAMMA final-composite function runs.
// This proves the draw's engine ownership, unlike driver DLL return addresses.
static thread_local bool t_gammaVerifiedFinalCompositeInside = false;
static LONG64 g_gammaDrawNativeViewportFixes = 0;
static LONG64 g_gammaDrawNativeViewportRestores = 0;
static LONG64 g_gammaBackbufferDrawProbes = 0;
static LONG64 g_gammaBackbufferDrawViewportAlreadyNative = 0;
static LONG64 g_gammaBackbufferDrawOtherViewport = 0;
static LONG64 g_gammaVerifiedCompositeFastQualifications = 0;
static LONG64 g_gammaNativePassRestores = 0;
static LONG64 g_gammaBackbufferBindings = 0;
static LONG64 g_gammaNativeRtvBindings = 0;
static LONG64 g_gammaFramesWithoutNativePass = 0; // historic counter, not used by v078
using GammaOMSetRenderTargetsFn=void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,
    ID3D11RenderTargetView* const*,ID3D11DepthStencilView*);
static GammaOMSetRenderTargetsFn g_gammaOriginalOMSetRenderTargets=nullptr;
static bool g_gammaNativePassHookReady=false;
static unsigned GammaModeWidth(int mode) {
    switch(mode){case 1:return 1280;case 2:return 1114;case 3:return 960;
      case 4:return 640;default:return 1920;}
}
static unsigned GammaModeHeight(int mode) {
    switch(mode){case 1:return 720;case 2:return 626;case 3:return 540;
      case 4:return 360;default:return 1080;}
}
static int GammaReadSharedRequestedMode() {
    wchar_t mode[64]{};
    GetPrivateProfileStringW(L"DLAA",L"upscaler_mode",L"dlaa",mode,
        _countof(mode),g_settingsFile);
    const int parsed=ParseMode(mode);
    if(parsed==0 && _wcsicmp(mode,L"dlaa")!=0)
        Log(L"GAMMA_SHARED_MENU_UNKNOWN value=%ls using=DLAA",mode);
    g_gammaMenuEnabled=GetPrivateProfileIntW(L"DLAA",L"enabled",1,g_settingsFile)!=0;
    wchar_t preset[32]{};
    GetPrivateProfileStringW(L"DLAA",L"preset",L"default",preset,
        _countof(preset),g_settingsFile);
    const int selectedPreset=ParsePreset(preset);
    if(selectedPreset!=g_preset) {
        g_preset=selectedPreset;
        if(g_gammaNativeTrialActive && g_state==1)
            g_gammaPresetRecreatePending=true;
        Log(L"GAMMA_SHARED_PRESET requested=%ls pending_recreate=%d",
            PresetName(selectedPreset),g_gammaPresetRecreatePending?1:0);
    }
    return parsed;
}
static void GammaPollSharedMenu(bool immediate=false) {
    if(!g_settingsFile[0])return;
    const DWORD now=GetTickCount();
    if(!immediate && static_cast<DWORD>(now-g_gammaSettingsLastPoll)<1000)return;
    g_gammaSettingsLastPoll=now;
    const int requested=GammaReadSharedRequestedMode();
    if(requested!=g_gammaMenuRequestedMode || immediate) {
        g_gammaMenuRequestedMode=requested;
        Log(L"GAMMA_SHARED_MENU mode=%ls active_allocations=%ls next=%ls enabled=%d source=%ls",
            ModeName(requested),ModeName(g_gammaSelectedMode),
            requested==g_gammaSelectedMode?L"CURRENT":L"WAIT_ENGINE_RECONSTRUCTION_OR_RESTART",
            g_gammaMenuEnabled?1:0,g_settingsFile);
    }
    if(requested!=g_gammaSelectedMode && requested!=g_gammaLastQueuedMode) {
        g_gammaLastQueuedMode=requested;
        Log(L"GAMMA_MODE_QUEUED requested=%ls allocated=%ls action=vid_restart_or_restart_game",
            ModeName(requested),ModeName(g_gammaSelectedMode));
    }
    if(!g_gammaMenuEnabled && g_gammaSelectedMode!=0 && immediate)
        Log(L"GAMMA_SHARED_MENU_DISABLE_DEFERRED mode=%ls reason=LIVE_LOW_RES_TARGETS_REQUIRE_RECONSTRUCTION",
            ModeName(g_gammaSelectedMode));
}
static DWORD g_gammaCtorThread = 0;
static bool g_gammaCompositeValidated = false;
static uintptr_t g_gammaValidatedCaller = 0;
static bool g_gammaHistoryResetPending = true;
static unsigned g_gammaLastSkipFrame = 0;
static unsigned g_gammaLastSkipReason = ~0u;
static LONG64 g_gammaSkipCounts[12]{};
static LONG64 g_gammaRendererGeneration=0;
static LONG64 g_gammaReinitialized=0;
static bool g_gammaQualityRasterActive=false;
static D3D11_VIEWPORT g_gammaQualityPriorViewport{};
static UINT g_gammaQualityPriorScissorCount=0;
static D3D11_RECT g_gammaQualityPriorScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};



static unsigned g_gammaQualifiedFrames = 0;
static unsigned g_gammaCandidateThisFrame = 0;
static unsigned g_gammaLastCandidateFrame = 0;
static uintptr_t g_gammaLastBackbufferCaller = 0;
static unsigned g_gammaRejectedReasons = 0;
static LONG64 g_gammaTrialDraws = 0;
static LONG64 g_gammaCandidateDraws = 0;
static LONG64 g_gammaBackbufferDraws = 0;
static LONG64 g_gammaNonBackbufferDraws = 0;
static LONG64 g_gammaUnknownUsageDraws = 0;
static LONG64 g_gammaTrialEvaluations = 0;
static ID3D11DeviceContext* g_gammaContext = nullptr; // borrowed from g_ngxContext
static bool g_gammaInsideDraw = false;
using GammaDrawFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using GammaDrawIndexedFn = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
static GammaDrawFn g_gammaOriginalDraw = nullptr;
static GammaDrawIndexedFn g_gammaOriginalDrawIndexed = nullptr;
static volatile LONG64 g_gammaAllocCalls=0;
static volatile LONG64 g_gammaRendererCalls=0;
static thread_local std::uintptr_t t_gammaCtorRenderer=0;
static thread_local unsigned t_gammaCtorTargets=0;
static std::uintptr_t g_gammaImageBase=0;
static uintptr_t g_gammaRdataStart=0, g_gammaRdataEnd=0;

static bool GammaReadName(const char* name, char (&buf)[80]) {
    const auto p=reinterpret_cast<uintptr_t>(name);
    if (p<g_gammaRdataStart || p>=g_gammaRdataEnd || p+sizeof(buf)>g_gammaRdataEnd) return false;
    if (!SafeRead(p,buf,sizeof(buf))) return false;
    const void* end=std::memchr(buf,0,sizeof(buf));
    if (!end) return false;
    return std::strncmp(buf,"$user$",6)==0;
}

static void __fastcall GammaNamedAllocAudit(void* slot, const char* name, unsigned w, unsigned h,
    unsigned format, unsigned config, unsigned char flag) {
    const LONG64 n=InterlockedIncrement64(&g_gammaAllocCalls);
    char readable[80]{};
    const bool named=GammaReadName(name,readable);
    const auto slotValue=reinterpret_cast<uintptr_t>(slot);
    const uintptr_t offset=t_gammaCtorRenderer && slotValue>=t_gammaCtorRenderer &&
       slotValue-t_gammaCtorRenderer<0x1000 ? slotValue-t_gammaCtorRenderer : 0;
    if (t_gammaCtorRenderer) ++t_gammaCtorTargets;
    if (n<=100 || (n%500)==0) {
        Log(L"GAMMA_ALLOC call=%lld name=%hs slot_offset=0x%llX size=%ux%u format=0x%X config=%u flag=%u mutation=0",
            n,named?readable:"UNVERIFIED_STRING",static_cast<unsigned long long>(offset),w,h,format,config,unsigned(flag));
    }
    unsigned targetW=w,targetH=h;
    const bool inCtor=t_gammaCtorRenderer &&
        GetCurrentThreadId()==g_gammaCtorThread &&
        slotValue>=t_gammaCtorRenderer+0x408 &&
        slotValue<t_gammaCtorRenderer+0x800 &&
        ((slotValue-t_gammaCtorRenderer)%8)==0;
    if(g_gammaQualityCtorActive && inCtor && named &&
       w>=15 && h>=15 && w<=1920 && h<=1080) {
        // Exactly the multiplayer scaling classification: keep native UI,
        // scope/viewport and independent shadow dimensions at their sizes.
        const bool nativeTarget=!_stricmp(readable,"$user$ui") ||
            !_stricmp(readable,"$user$viewport2") ||
            !_stricmp(readable,"$user$scopeRT");
        const bool independent=!_stricmp(readable,"$user$smap_depth") ||
            !_stricmp(readable,"$user$ssfx_water_waves");
        if(!nativeTarget && !independent) {
            targetW=static_cast<unsigned>((static_cast<unsigned long long>(w)*g_gammaInputW+960)/1920);
            targetH=static_cast<unsigned>((static_cast<unsigned long long>(h)*g_gammaInputH+540)/1080);
            targetW=std::max(1u,targetW);targetH=std::max(1u,targetH);
            if(w==1920 && h==1080) {
                if(offset==0x430)g_gammaQualityCriticalMask|=1u;
                if(offset==0x728)g_gammaQualityCriticalMask|=2u;
                if(offset==0x438)g_gammaQualityCriticalMask|=4u;
                if(offset==0x448)g_gammaQualityCriticalMask|=8u;
                if(offset==0x668)g_gammaQualityCriticalMask|=16u;
            }
            if(n<=100 || n%500==0)
                Log(L"GAMMA_QUALITY_ALLOC slot=0x%llX name=%hs from=%ux%u to=%ux%u",
                    static_cast<unsigned long long>(offset),readable,w,h,targetW,targetH);
        }
    }
    if (g_gammaOriginalAlloc) g_gammaOriginalAlloc(slot,name,targetW,targetH,format,config,flag);
}

static void GammaVisibleOnRendererRebuild(); // defined after GAMMA GPU helpers
static void GammaCacheKnownRasterTextures(void* renderer);

static void* __fastcall GammaConstructorAudit(void* renderer) {
    const LONG64 n=InterlockedIncrement64(&g_gammaCtorCalls);
    const auto prior=t_gammaCtorRenderer;
    const auto priorTargets=t_gammaCtorTargets;
    t_gammaCtorRenderer=reinterpret_cast<uintptr_t>(renderer);
    t_gammaCtorTargets=0;
    g_gammaCtorThread=GetCurrentThreadId();
    GammaPollSharedMenu(true);
    GammaPollPauseCommand(true); // exact per-launch token + Lua pause lifecycle
    const int requestedMenuMode=g_gammaMenuRequestedMode;
    const bool firstQualityCtor=(n==1 && g_gammaRendererGeneration==0 &&
        requestedMenuMode!=0 &&
        !ReadOr<uintptr_t>(g_gammaImageBase+kGammaLevelRva) &&
        !ReadOr<uintptr_t>(g_gammaImageBase+kGammaHudRva));
    if(firstQualityCtor) {
        // Do not change the shared menu INI. Bootstrap as native DLAA and
        // automatically request a single engine-owned renderer reconstruction.
        // This is the crucial distinction between continuous v072 and the
        // sparse-draw, directly-Quality-started v073-v075 runs.
        g_gammaBootstrapNativeFirst=true;
        g_gammaBootstrapPending=true;
        ++g_gammaBootstrapNativeAttempts;
        Log(L"GAMMA_BOOTSTRAP_NATIVE_FIRST requested=%ls request_deferred_until_ngx_ready=1 "
            L"ctor=%lld level=0 hud=0",ModeName(requestedMenuMode),n);
    }
    if(n>1 && g_gammaBootstrapPending) {
        g_gammaBootstrapPending=false;
        ++g_gammaBootstrapQualityActivations;
        Log(L"GAMMA_BOOTSTRAP_RENDERER_REBUILT ctor=%lld requested=%ls generation_before=%lld",
            n,ModeName(requestedMenuMode),g_gammaRendererGeneration);
    }
    const bool nativePauseCtor=g_gammaPauseNativeEnabled && g_gammaPauseRestartEnabled &&
        g_gammaPauseNativeRequested && n>1;
    g_gammaPauseCtorNative=nativePauseCtor;
    g_gammaSelectedMode=(g_gammaBootstrapPending || nativePauseCtor) ? 0 : requestedMenuMode;
    if(nativePauseCtor) {
        g_gammaPauseHadNativeCtor=true;
        ++g_gammaPauseMenuRebuilds;
        Log(L"GAMMA_PAUSE_CTOR_NATIVE generation=%lld requested=%ls mode=DLAA native_targets=1 saved_setting_unchanged=1",
            g_gammaRendererGeneration+1,ModeName(requestedMenuMode));
    } else if(n>1 && g_gammaPauseHadNativeCtor && !g_gammaPauseNativeRequested) {
        g_gammaPauseHadNativeCtor=false;
        ++g_gammaPauseRestoredRebuilds;
        Log(L"GAMMA_PAUSE_CTOR_SELECTED generation=%lld requested=%ls actual=%ls",
            g_gammaRendererGeneration+1,ModeName(requestedMenuMode),ModeName(g_gammaSelectedMode));
    }
    g_gammaQualityRequested=(g_gammaSelectedMode!=0);
    g_gammaInputW=GammaModeWidth(g_gammaSelectedMode);
    g_gammaInputH=GammaModeHeight(g_gammaSelectedMode);
    // Never mutate a non-native renderer's allocations: only exact 1080p.
    const unsigned currentW=ReadOr<unsigned>(g_gammaImageBase+kGammaWidthRva);
    const unsigned currentH=ReadOr<unsigned>(g_gammaImageBase+kGammaHeightRva);
    g_gammaQualityCtorActive=g_gammaQualityRequested && currentW==1920 && currentH==1080;
    g_gammaQualityCriticalMask=0;
    g_gammaQualityAllocated=false;
    Log(L"GAMMA_CTOR_ENTER call=%lld renderer=%p quality_request=%d globals=%ux%u",n,renderer,
        g_gammaQualityCtorActive?1:0,currentW,currentH);
    // Stop the bridge before the engine starts destroying/replacing target
    // resources, never after their old owners have been invalidated.
    if(g_gammaNativeTrialActive && g_state==1) GammaVisibleOnRendererRebuild();
    void* result=g_gammaOriginalCtor ? g_gammaOriginalCtor(renderer) : nullptr;
    g_gammaLatestCtorRenderer=reinterpret_cast<uintptr_t>(renderer);
    g_gammaQualityAllocated=g_gammaQualityCtorActive && (g_gammaQualityCriticalMask&31u)==31u;
    if(g_gammaQualityRequested && !g_gammaQualityAllocated)
        Log(L"GAMMA_QUALITY_ALLOCATION_UNVERIFIED ctor=%lld mask=0x%X native_ctor=%d activation_refused=1",
            n,g_gammaQualityCriticalMask,g_gammaQualityCtorActive?1:0);
    GammaCacheKnownRasterTextures(renderer);
    ++g_gammaRendererGeneration;
    GammaWritePauseStatus(); // Lua must verify allocated mode before another restart
    Log(L"GAMMA_CTOR_RETURN call=%lld renderer=%p original_result=%p named_alloc_calls=%u quality_allocated=%d critical_mask=0x%X generation=%lld",
        n,renderer,result,t_gammaCtorTargets,g_gammaQualityAllocated?1:0,
        g_gammaQualityCriticalMask,g_gammaRendererGeneration);
    t_gammaCtorRenderer=prior;
    t_gammaCtorTargets=priorTargets;
    return result;
}

static void __fastcall GammaRendererAudit(void* renderer) {
    const LONG64 n=InterlockedIncrement64(&g_gammaRendererCalls);
    if(n<=8 || n%1200==0) {
        uintptr_t color=0,motion=0,depth=0;
        const auto base=reinterpret_cast<uintptr_t>(renderer);
        if(base) {
            SafeRead(base+kGammaColorSlot,&color,sizeof(color));
            SafeRead(base+kGammaMotionSlot,&motion,sizeof(motion));
            SafeRead(base+kGammaDepthSlot,&depth,sizeof(depth));
        }
        Log(L"GAMMA_RENDERER_CALL count=%lld renderer=%p color_wrapper=%p motion_wrapper=%p depth_wrapper=%p no_d3d_methods=1 no_ngx=1",
            n,renderer,reinterpret_cast<void*>(color),reinterpret_cast<void*>(motion),reinterpret_cast<void*>(depth));
    }
    if(g_gammaOriginalRenderer) g_gammaOriginalRenderer(renderer);
}

// Verify that the opaque wrapper+0x20 pointer has a plausible COM vtable
// before invoking GetDesc on it (the previous build only read pointer bytes).
static bool GammaPlausibleComTexture(ID3D11Texture2D* t) {
    if(!t) return false;
    const auto p=reinterpret_cast<uintptr_t>(t);
    uintptr_t vt=0, method=0;
    if(!SafeRead(p,&vt,sizeof(vt)) || !vt ||
       !SafeRead(vt+8*10,&method,sizeof(method)) || !method) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if(!VirtualQuery(reinterpret_cast<void*>(method),&mbi,sizeof(mbi)) ||
       mbi.State!=MEM_COMMIT || (mbi.Protect & (PAGE_GUARD|PAGE_NOACCESS))) return false;
    const DWORD access=mbi.Protect & 0xff;
    return access==PAGE_EXECUTE_READ || access==PAGE_EXECUTE_READWRITE ||
           access==PAGE_EXECUTE_WRITECOPY || access==PAGE_EXECUTE;
}

// v078 architectural pivot: no writes to GAMMA engine width/height globals.
// Bind-time raster normalization uses actual engine-owned RTV texture sizes.
// A D3D11 target may be 1280x720 while projection, UI and swapchain remain
// 1920x1080. The OG multiplayer engine-hook path remains entirely unchanged.
// The per-render-target viewport normalizer never touches unrecognized targets.
static void GammaCacheKnownRasterTextures(void* renderer) {
    static constexpr uintptr_t slots[]={
        0x430,0x728,0x438,0x448,0x468,0x470,0x428,0x440,
        0x478,0x490,0x458,0x460,0x4A0,0x4A8,0x4B0,
        0x660,0x668,0x568,0x570,0x578,0x580,0x588,
        0x590,0x598,0x5A8,0x5B0,0x5B8,0x5C0,0x5C8,0x5D0,
        0x5D8,0x5E0,0x5E8,0x608,0x610,0x618,0x620,0x628,0x630,
        0x638,0x640,0x648,0x650,0x5F8,0x600,0x5F0,0x670,0x4E8,
        0x488,0x480,0x498
    };
    static_assert(sizeof(slots)/sizeof(slots[0])<=kGammaKnownRasterMax,
        "GAMMA raster target table exceeds reserved space");
    g_gammaKnownRasterCount=0;
    for(uintptr_t slot:slots) {
        ID3D11Texture2D* p=GetTextureFromSlot(renderer,slot);
        if(!p)continue;
        const unsigned role=(slot==0x488 || slot==0x480 || slot==0x498) ? 2u : 1u;
        g_gammaKnownRasterTextures[g_gammaKnownRasterCount]=p;
        g_gammaKnownRasterRoles[g_gammaKnownRasterCount]=role;
        ++g_gammaKnownRasterCount;
    }
    Log(L"GAMMA_RASTER_TARGET_CACHE targets=%u renderer=%p mode=%ls owned_refs=0",
        g_gammaKnownRasterCount,renderer,ModeName(g_gammaSelectedMode));
}
static unsigned GammaKnownRasterRole(ID3D11Texture2D* tex) {
    if(!tex)return 0;
    for(unsigned i=0;i<g_gammaKnownRasterCount;++i)
        if(g_gammaKnownRasterTextures[i]==tex)return g_gammaKnownRasterRoles[i];
    return 0;
}
// v078: diagnostics only. The v069 target-sized raster correction produced
// no qualifying calls in the supplied Quality log, and its active scope is
// not a verified composite boundary. Never attempt framebuffer mutation here.
static constexpr bool kGammaBoundaryTraceOnly = true;
static bool GammaRasterScope(ID3D11DeviceContext* ctx) {
    return !kGammaBoundaryTraceOnly && g_gammaRasterHooksReady && g_gammaQualityRequested &&
        g_gammaQualityAllocated && t_gammaWorldFrameInside &&
        ctx==g_ngxContext && GetCurrentThreadId()==g_renderTid &&
        g_gammaInputW && g_gammaInputH && g_displayWidth==1920 &&
        g_displayHeight==1080;
}
static void GammaNormalizeRaster(ID3D11DeviceContext* ctx,const wchar_t* why) {
    if(!GammaRasterScope(ctx) || !g_gammaRasterCurrentRole ||
       !g_gammaOriginalRSSetViewports || !g_gammaOriginalRSSetScissorRects)return;
    const unsigned tw=g_gammaRasterCurrentW,th=g_gammaRasterCurrentH;
    if(!tw || !th)return;
    const bool scaled=g_gammaRasterCurrentRole==1;
    const unsigned oldW=scaled ? unsigned((static_cast<unsigned long long>(tw)*1920u+g_gammaInputW/2u)/g_gammaInputW) : g_gammaInputW;
    const unsigned oldH=scaled ? unsigned((static_cast<unsigned long long>(th)*1080u+g_gammaInputH/2u)/g_gammaInputH) : g_gammaInputH;
    D3D11_VIEWPORT v{};
    UINT n=1;
    ctx->RSGetViewports(&n,&v);
    if(n==1 && v.TopLeftX==0.f && v.TopLeftY==0.f &&
       ((v.Width==float(oldW) && v.Height==float(oldH)) ||
        (scaled && tw==g_gammaInputW && th==g_gammaInputH &&
         v.Width==1920.f && v.Height==1080.f))) {
        if(v.Width!=float(tw) || v.Height!=float(th)) {
            const float priorW=v.Width,priorH=v.Height;
            v.Width=float(tw);v.Height=float(th);
            g_gammaOriginalRSSetViewports(ctx,1,&v);
            ++g_gammaRasterViewportAdjusts;
            if(g_gammaRasterViewportAdjusts<=12 || (g_gammaRasterViewportAdjusts%600)==0)
                Log(L"GAMMA_RASTER_VIEWPORT_FIX reason=%ls role=%ls from=%.0fx%.0f to=%ux%u count=%lld",
                    why,scaled?L"SCENE":L"NATIVE",priorW,priorH,tw,th,
                    g_gammaRasterViewportAdjusts);
        }
    }
    D3D11_RECT rect{};
    UINT count=1;
    ctx->RSGetScissorRects(&count,&rect);
    if(count==1 && rect.left==0 && rect.top==0 &&
       ((rect.right==LONG(oldW) && rect.bottom==LONG(oldH)) ||
        (scaled && tw==g_gammaInputW && th==g_gammaInputH &&
         rect.right==1920 && rect.bottom==1080))) {
        if(rect.right!=LONG(tw) || rect.bottom!=LONG(th)) {
            const LONG priorW=rect.right,priorH=rect.bottom;
            rect.right=LONG(tw);rect.bottom=LONG(th);
            g_gammaOriginalRSSetScissorRects(ctx,1,&rect);
            ++g_gammaRasterScissorAdjusts;
            if(g_gammaRasterScissorAdjusts<=12 || (g_gammaRasterScissorAdjusts%600)==0)
                Log(L"GAMMA_RASTER_SCISSOR_FIX reason=%ls role=%ls from=%ldx%ld to=%ux%u count=%lld",
                    why,scaled?L"SCENE":L"NATIVE",priorW,priorH,tw,th,
                    g_gammaRasterScissorAdjusts);
        }
    }
}
// The only reliable way to identify GAMMA's active final-composite function
// is to observe the unwindable executable frames underneath the actual
// verified swapchain DrawIndexed, not to assume MP's inactive renderer hook.
static void GammaLogRawD3D(const wchar_t* name,LONG64 n,ID3D11DeviceContext* ctx) {
    if(n>5 && n%250000!=0)return;
    Log(L"GAMMA_D3D_RAW_METHOD method=%ls count=%lld ctx=%p immediate=%p thread=%lu "
        L"render_tid=%lu world_scope=%d quality=%d renderer_gen=%lld scope_gate=%d",
        name,n,ctx,g_ngxContext,GetCurrentThreadId(),g_renderTid,
        t_gammaWorldFrameInside?1:0,g_gammaQualityRequested?1:0,
        g_gammaRendererGeneration,GammaRasterScope(ctx)?1:0);
}
static void STDMETHODCALLTYPE GammaRSSetViewportsAdapter(ID3D11DeviceContext* ctx,
    UINT count,const D3D11_VIEWPORT* viewports) {
    const LONG64 raw=InterlockedIncrement64(&g_gammaRawViewportCalls);
    GammaLogRawD3D(L"RSSetViewports",raw,ctx);
    if(g_gammaOriginalRSSetViewports)g_gammaOriginalRSSetViewports(ctx,count,viewports);
    if(GammaRasterScope(ctx)) {
        ++g_gammaRasterViewportCalls;
        GammaNormalizeRaster(ctx,L"RS_VIEWPORT");
    }
}
static void STDMETHODCALLTYPE GammaRSSetScissorRectsAdapter(ID3D11DeviceContext* ctx,
    UINT count,const D3D11_RECT* rects) {
    const LONG64 raw=InterlockedIncrement64(&g_gammaRawScissorCalls);
    GammaLogRawD3D(L"RSSetScissorRects",raw,ctx);
    if(g_gammaOriginalRSSetScissorRects)g_gammaOriginalRSSetScissorRects(ctx,count,rects);
    if(GammaRasterScope(ctx)) {
        ++g_gammaRasterScissorCalls;
        GammaNormalizeRaster(ctx,L"RS_SCISSOR");
    }
}
static void STDMETHODCALLTYPE GammaOMSetRenderTargetsAdapter(ID3D11DeviceContext* ctx,
    UINT count,ID3D11RenderTargetView* const* rtvs,ID3D11DepthStencilView* depth) {
    const LONG64 raw=InterlockedIncrement64(&g_gammaRawOmCalls);
    GammaLogRawD3D(L"OMSetRenderTargets",raw,ctx);
    if(g_gammaOriginalOMSetRenderTargets)
        g_gammaOriginalOMSetRenderTargets(ctx,count,rtvs,depth);
    if(!GammaRasterScope(ctx))return;
    g_gammaRasterCurrentRole=0;
    g_gammaRasterCurrentW=g_gammaRasterCurrentH=0;
    if(!count || count>8 || !rtvs)return;
    for(UINT i=0;i<count;++i) {
        if(!rtvs[i])continue;
        ID3D11Resource* resource=nullptr;
        rtvs[i]->GetResource(&resource);
        ID3D11Texture2D* tex=nullptr;
        if(resource) {
            resource->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&tex));
            resource->Release();
        }
        if(!tex)continue;
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        unsigned role=GammaKnownRasterRole(tex);
        bool swapchain=false;
        if(!role && d.Width==g_displayWidth && d.Height==g_displayHeight) {
            IDXGIResource* dxgi=nullptr;
            DXGI_USAGE usage=0;
            if(SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource),
                reinterpret_cast<void**>(&dxgi))) && dxgi) {
                swapchain=SUCCEEDED(dxgi->GetUsage(&usage)) &&
                    ((usage&DXGI_USAGE_BACK_BUFFER)!=0);
                dxgi->Release();
            }
            if(swapchain)role=2;
        }
        if(role && d.SampleDesc.Count==1 &&
           (d.BindFlags&D3D11_BIND_RENDER_TARGET) &&
           d.Width<=g_displayWidth && d.Height<=g_displayHeight) {
            g_gammaRasterCurrentRole=role;
            g_gammaRasterCurrentW=d.Width;g_gammaRasterCurrentH=d.Height;
            if(swapchain)++g_gammaBackbufferBindings;
            else ++g_gammaNativeRtvBindings;
            ++g_gammaRasterOmBindings;
            if(g_gammaRasterOmBindings<=12 || (g_gammaRasterOmBindings%900)==0)
                Log(L"GAMMA_RASTER_TARGET_BIND frame=%u role=%ls target=%p size=%ux%u slot=%u count=%lld engine_globals=1920x1080",
                    ReadEngineFrame(),role==1?L"SCENE":L"NATIVE",
                    tex,d.Width,d.Height,i,g_gammaRasterOmBindings);
        } else ++g_gammaRasterUnknownBinds;
        tex->Release();
        break; // primary RTV determines the viewport in this adapter
    }
    GammaNormalizeRaster(ctx,L"OM_BIND");
}
static bool GammaInstallNativePassHook(ID3D11DeviceContext* ctx) {
    if(g_gammaRasterHooksReady)return true;
    if(!ctx)return false;
    auto vt=*reinterpret_cast<void***>(ctx);
    if(!vt || !vt[33] || !vt[44] || !vt[45])return false;
    MH_STATUS om=MH_CreateHook(vt[33],
        reinterpret_cast<void*>(&GammaOMSetRenderTargetsAdapter),
        reinterpret_cast<void**>(&g_gammaOriginalOMSetRenderTargets));
    if(om==MH_OK)om=MH_EnableHook(vt[33]);
    MH_STATUS vp=MH_CreateHook(vt[44],
        reinterpret_cast<void*>(&GammaRSSetViewportsAdapter),
        reinterpret_cast<void**>(&g_gammaOriginalRSSetViewports));
    if(vp==MH_OK)vp=MH_EnableHook(vt[44]);
    MH_STATUS sc=MH_CreateHook(vt[45],
        reinterpret_cast<void*>(&GammaRSSetScissorRectsAdapter),
        reinterpret_cast<void**>(&g_gammaOriginalRSSetScissorRects));
    if(sc==MH_OK)sc=MH_EnableHook(vt[45]);
    g_gammaRasterHooksReady=(om==MH_OK && vp==MH_OK && sc==MH_OK);
    g_gammaNativePassHookReady=g_gammaRasterHooksReady;
    Log(L"GAMMA_RASTER_ADAPTER_HOOK om=%d viewport=%d scissor=%d ready=%d no_global_width_height_writes=1 passive_tracing=1",
        int(om),int(vp),int(sc),g_gammaRasterHooksReady?1:0);
    return g_gammaRasterHooksReady;
}

// v078: GAMMA adapter with common NGX core and verified draw-time backbuffer viewport correction
// principle at GAMMA's *verified Draw/DrawIndexed*, not at PSSetShaderResources
// (GAMMA's composite has already set its PS inputs before the draw hook).
// One persistent NGX feature and four BRIDGE-OWNED textures. The engine's
// textures are copied into private inputs each frame; they are never handed
// to NGX, overwritten, or retained. Output is substituted only for the
// precisely identified DXGI swapchain draw, then the binding is restored.
static LONG64 g_gammaVisibleEvalAttempts = 0;
static LONG64 g_gammaVisibleEvalSuccesses = 0;
// Only emit a skip when the actual back-buffer draw is affected. Event-driven
// plus rate-limited logs avoid the millions of regular geometry draw calls.
static void GammaRecordSkip(unsigned frame,unsigned code,const wchar_t* reason) {
    if(code>=12) return;
    const LONG64 n=++g_gammaSkipCounts[code];
    if(g_gammaLastSkipReason!=code || n<=3 || (n%120)==0) {
        Log(L"GAMMA_FRAME_SKIPPED frame=%u reason=%ls count=%lld sticky_verified=%d mode=%ls evals=%lld",
            frame,reason,n,g_gammaCompositeValidated?1:0,ModeName(g_gammaSelectedMode),
            g_gammaVisibleEvalSuccesses);
        g_gammaLastSkipReason=code;
    }
    g_gammaLastSkipFrame=frame;
}
static LONG64 g_gammaVisiblePresented = 0;
static LONG64 g_gammaVisibleRestoreFailures = 0;
static unsigned g_gammaVisibleLastFrame = 0;
static unsigned g_gammaVisiblePreviousSuccessFrame = 0;
static bool g_gammaVisibleFault = false;
static bool g_gammaVisibleInputsReady = false;
static ID3D11Texture2D* g_gammaVisibleInput[3]{};
static ID3D11Texture2D* g_gammaVisibleOutput = nullptr;
static ID3D11ShaderResourceView* g_gammaVisibleOutputSrv = nullptr;
static ID3D11Texture2D* g_gammaVisibleSource[3]{}; // BORROWED identities, never Release
static D3D11_SHADER_RESOURCE_VIEW_DESC g_gammaVisibleSrvDesc{};
static bool g_gammaVisibleSrvDescReady = false;
static bool g_gammaVisibleBindingActive = false;
static unsigned g_gammaVisibleBindCount = 0;
struct GammaVisibleBoundSlot {
    UINT slot=0;
    ID3D11ShaderResourceView* original=nullptr; // owned Get ref, until immediately after draw
};
static GammaVisibleBoundSlot g_gammaVisibleBound[4]{};

struct GammaD3DBindingSnapshot {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* sceneSrv = nullptr;
    UINT viewportCount = 0;
    D3D11_VIEWPORT viewport{};
};
static GammaD3DBindingSnapshot GammaSampleBindings(ID3D11DeviceContext* ctx) {
    GammaD3DBindingSnapshot sample{};
    if (!ctx) return sample;
    ctx->OMGetRenderTargets(1, &sample.rtv, nullptr);
    ctx->PSGetShaderResources(0, 1, &sample.sceneSrv);
    sample.viewportCount = 1;
    ctx->RSGetViewports(&sample.viewportCount, &sample.viewport);
    return sample;
}
static void GammaReleaseBindings(GammaD3DBindingSnapshot& b) {
    if (b.rtv) { b.rtv->Release(); b.rtv=nullptr; }
    if (b.sceneSrv) { b.sceneSrv->Release(); b.sceneSrv=nullptr; }
}
static bool GammaSameViewport(const GammaD3DBindingSnapshot& a,const GammaD3DBindingSnapshot& b) {
    return a.viewportCount==b.viewportCount &&
        (a.viewportCount==0 || (a.viewport.TopLeftX==b.viewport.TopLeftX &&
        a.viewport.TopLeftY==b.viewport.TopLeftY &&
        a.viewport.Width==b.viewport.Width && a.viewport.Height==b.viewport.Height &&
        a.viewport.MinDepth==b.viewport.MinDepth && a.viewport.MaxDepth==b.viewport.MaxDepth));
}
static bool GammaTextureOwnedByDevice(ID3D11Texture2D* t) {
    if (!t || !g_ngxDevice) return false;
    ID3D11Device* owner=nullptr;
    t->GetDevice(&owner);
    const bool same=(owner==g_ngxDevice);
    if(owner)owner->Release();
    return same;
}
static void GammaVisibleFault(const wchar_t* reason) {
    if (!g_gammaVisibleFault)
        Log(L"GAMMA_VISIBLE_ABORT reason=%ls attempts=%lld evaluated=%lld presented=%lld "
            L"fail_closed=1 game_rendering_unchanged=1",reason,
            g_gammaVisibleEvalAttempts,g_gammaVisibleEvalSuccesses,g_gammaVisiblePresented);
    g_gammaVisibleFault=true;
}
static void GammaVisibleReleaseBridgeTextures() {
    if (g_gammaVisibleOutputSrv) { g_gammaVisibleOutputSrv->Release(); g_gammaVisibleOutputSrv=nullptr; }
    for(auto& t:g_gammaVisibleInput)if(t){t->Release();t=nullptr;}
    if(g_gammaVisibleOutput){g_gammaVisibleOutput->Release();g_gammaVisibleOutput=nullptr;}
    g_gammaVisibleInputsReady=false;
}
// Called only when GAMMA reconstructs the renderer. Do not reinitialize NGX
// during a possibly active graphics reset. Releasing the private NGX feature
// under its isolated D3D state avoids touching the engine's current bindings.
static void GammaVisibleOnRendererRebuild() {
    if (!g_gammaNativeTrialActive) return;
    g_evaluationEnabled=false;
    bool featureRetired=g_feature==nullptr;
    if(g_feature && g_context1 && g_isolatedState && g_ngxContext &&
       GetCurrentThreadId()==g_renderTid && !g_gammaVisibleBindingActive) {
        ID3DDeviceContextState* previous=nullptr;
        g_context1->SwapDeviceContextState(g_isolatedState,&previous);
        if(previous) {
            const auto result=NVSDK_NGX_D3D11_ReleaseFeature(g_feature);
            featureRetired=NVSDK_NGX_SUCCEED(result);
            if(featureRetired)g_feature=nullptr;
            g_ngxContext->ClearState();
            g_context1->SwapDeviceContextState(previous,nullptr);
            previous->Release();
        }
    }
    if(featureRetired) {
        GammaVisibleReleaseBridgeTextures();
        for(auto& identity:g_gammaVisibleSource)identity=nullptr;
        g_gammaVisibleSrvDescReady=false;
        g_gammaVisiblePreviousSuccessFrame=0;
        g_gammaVisibleLastFrame=0;
        g_gammaCompositeValidated=false;
        g_gammaValidatedCaller=0;
        g_gammaQualifiedFrames=0;
        g_gammaLastCandidateFrame=0;
        g_gammaCandidateThisFrame=0;
        g_gammaHistoryResetPending=true;
        g_gammaVisibleFault=false;
        g_gammaTrialRejected=false;
        g_state=3; // NGX remains initialized; recreate *feature* after new targets exist
        Log(L"GAMMA_RENDERER_REBUILD_REARM released=1 new_generation=%lld next=RECREATE_FEATURE",
            g_gammaRendererGeneration+1);
    } else {
        g_gammaVisibleFault=true;
        g_gammaTrialRejected=true;
        g_state=2;
        Log(L"GAMMA_RENDERER_REBUILD_REFUSED reason=NGX_FEATURE_NOT_RELEASED fail_closed=1");
    }
}
static bool GammaVisibleValidateScene(ID3D11Texture2D* (&scene)[3],
                                      D3D11_TEXTURE2D_DESC (&d)[3]) {
    if(!g_gammaLatestCtorRenderer || !g_ngxDevice) return false;
    void* renderer=reinterpret_cast<void*>(g_gammaLatestCtorRenderer);
    const uintptr_t slots[3]={kGammaColorSlot,kGammaMotionSlot,kGammaDepthSlot};
    const DXGI_FORMAT expected[3]={DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R24G8_TYPELESS};
    for(int i=0;i<3;++i) {
        scene[i]=GetTextureFromSlot(renderer,slots[i]);
        if(!GammaPlausibleComTexture(scene[i]) || !GammaTextureOwnedByDevice(scene[i])) return false;
        scene[i]->GetDesc(&d[i]);
        const unsigned inputW=g_gammaInputW;
        const unsigned inputH=g_gammaInputH;
        if(d[i].Format!=expected[i] || d[i].Width!=inputW ||
           d[i].Height!=inputH || d[i].SampleDesc.Count!=1 ||
           d[i].MipLevels!=1 || d[i].ArraySize!=1 || d[i].Usage!=D3D11_USAGE_DEFAULT ||
           !(d[i].BindFlags&D3D11_BIND_SHADER_RESOURCE) ||
           (g_gammaVisibleInputsReady && g_gammaVisibleSource[i]!=scene[i])) return false;
    }
    return true;
}
static bool GammaVisibleEnsurePrivateTextures(ID3D11Texture2D* (&scene)[3],
                                              D3D11_TEXTURE2D_DESC (&d)[3]) {
    if(g_gammaVisibleInputsReady) return true;
    const wchar_t* names[3]={L"COLOR",L"MOTION",L"DEPTH"};
    for(int i=0;i<3;++i) {
        D3D11_TEXTURE2D_DESC clone=d[i];
        clone.CPUAccessFlags=0;
        clone.MiscFlags=0;
        const HRESULT hr=g_ngxDevice->CreateTexture2D(&clone,nullptr,&g_gammaVisibleInput[i]);
        Log(L"GAMMA_VISIBLE_INPUT_CREATE name=%ls hr=0x%08X texture=%p fmt=%u size=%ux%u",
            names[i],unsigned(hr),g_gammaVisibleInput[i],unsigned(clone.Format),clone.Width,clone.Height);
        if(FAILED(hr)||!g_gammaVisibleInput[i])return false;
        g_gammaVisibleSource[i]=scene[i];
    }
    D3D11_TEXTURE2D_DESC output=d[0];
    // True DLSS Quality: private input is a copy of ENGINE's 1280x720 scene,
    // while NGX's private output is the original native 1920x1080 display.
    output.Width=g_displayWidth;output.Height=g_displayHeight;
    output.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
    output.CPUAccessFlags=0; output.MiscFlags=0;
    const HRESULT hr=g_ngxDevice->CreateTexture2D(&output,nullptr,&g_gammaVisibleOutput);
    Log(L"GAMMA_VISIBLE_OUTPUT_CREATE hr=0x%08X texture=%p fmt=%u size=%ux%u",
        unsigned(hr),g_gammaVisibleOutput,unsigned(output.Format),output.Width,output.Height);
    if(FAILED(hr)||!g_gammaVisibleOutput)return false;
    g_gammaVisibleInputsReady=true;
    return true;
}
// Required only at the final already-bound compositor draw. The original
// binding is obtained through PSGet and retained until the original draw has
// finished, then restored without changing engine-owned objects.
static bool GammaVisibleSubstitute(ID3D11DeviceContext* ctx,ID3D11Texture2D* sceneColor) {
    if(g_gammaVisibleBindingActive || !g_gammaVisibleOutput || !g_gammaVisibleInputsReady) return false;
    ID3D11ShaderResourceView* views[4]{};
    ctx->PSGetShaderResources(0,4,views);
    unsigned slots[4]{};
    unsigned count=0;
    bool valid=true;
    D3D11_SHADER_RESOURCE_VIEW_DESC requested{};
    for(unsigned i=0;i<4;++i) {
        if(!views[i])continue;
        ID3D11Resource* resource=nullptr;
        views[i]->GetResource(&resource);
        const bool match=resource==static_cast<ID3D11Resource*>(sceneColor);
        if(resource)resource->Release();
        if(!match)continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        views[i]->GetDesc(&desc);
        if(desc.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2D ||
           desc.Texture2D.MostDetailedMip!=0 || desc.Texture2D.MipLevels!=1 ||
           (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM &&
            desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)) valid=false;
        if(count && (desc.Format!=requested.Format || desc.ViewDimension!=requested.ViewDimension ||
                     desc.Texture2D.MostDetailedMip!=requested.Texture2D.MostDetailedMip ||
                     desc.Texture2D.MipLevels!=requested.Texture2D.MipLevels)) valid=false;
        requested=desc;
        slots[count++]=i;
    }
    if(!valid || !count){for(auto& v:views)if(v)v->Release();return false;}
    if(!g_gammaVisibleOutputSrv) {
        const HRESULT hr=g_ngxDevice->CreateShaderResourceView(g_gammaVisibleOutput,&requested,&g_gammaVisibleOutputSrv);
        Log(L"GAMMA_VISIBLE_OUTPUT_SRV_CREATE hr=0x%08X view=%p fmt=%u slots=%u",
            unsigned(hr),g_gammaVisibleOutputSrv,unsigned(requested.Format),count);
        if(FAILED(hr)||!g_gammaVisibleOutputSrv){for(auto& v:views)if(v)v->Release();return false;}
        g_gammaVisibleSrvDesc=requested;
        g_gammaVisibleSrvDescReady=true;
    } else if(!g_gammaVisibleSrvDescReady ||
        requested.Format!=g_gammaVisibleSrvDesc.Format ||
        requested.ViewDimension!=g_gammaVisibleSrvDesc.ViewDimension ||
        requested.Texture2D.MostDetailedMip!=g_gammaVisibleSrvDesc.Texture2D.MostDetailedMip ||
        requested.Texture2D.MipLevels!=g_gammaVisibleSrvDesc.Texture2D.MipLevels) {
        for(auto& v:views)if(v)v->Release();return false;
    }
    for(unsigned i=0;i<count;++i) {
        g_gammaVisibleBound[i].slot=slots[i];
        g_gammaVisibleBound[i].original=views[slots[i]];
        views[slots[i]]=nullptr; // preserve its Get ref until original draw returns
    }
    g_gammaVisibleBindCount=count;
    for(auto& v:views)if(v)v->Release();
    for(unsigned i=0;i<count;++i) {
        auto& saved=g_gammaVisibleBound[i];
        ctx->PSSetShaderResources(saved.slot,1,&g_gammaVisibleOutputSrv);
    }
    // A hazard-driven driver unbind would make the next draw unsafe to alter.
    for(unsigned i=0;i<count;++i) {
        ID3D11ShaderResourceView* actual=nullptr;
        ctx->PSGetShaderResources(g_gammaVisibleBound[i].slot,1,&actual);
        const bool same=actual==g_gammaVisibleOutputSrv;
        if(actual)actual->Release();
        if(!same)valid=false;
    }
    if(!valid) {
        for(unsigned i=0;i<count;++i) {
            auto& saved=g_gammaVisibleBound[i];
            ctx->PSSetShaderResources(saved.slot,1,&saved.original);
            saved.original->Release(); saved=GammaVisibleBoundSlot{};
        }
        g_gammaVisibleBindCount=0;
        return false;
    }
    g_gammaVisibleBindingActive=true;
    return true;
}
// This mirrors the MP final-composite native viewport/scissor correction.
// It operates only at the authenticated DXGI back-buffer draw, not on other
// intermediate 720p targets, and restores every raster setting after draw.
static bool GammaPrepareQualityRaster(ID3D11DeviceContext* ctx,unsigned frame) {
    if(!g_gammaQualityRequested || !g_gammaQualityAllocated)return true;
    if(g_gammaQualityRasterActive)return false;
    UINT count=1;
    ctx->RSGetViewports(&count,&g_gammaQualityPriorViewport);
    if(count!=1) {
        GammaRecordSkip(frame,7,L"QUALITY_VIEWPORT_COUNT_NOT_ONE");return false;
    }
    g_gammaQualityPriorScissorCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&g_gammaQualityPriorScissorCount,g_gammaQualityPriorScissors);
    ++g_gammaRasterCompositeSamples;
    if(g_gammaRasterCompositeSamples<=8 || (g_gammaRasterCompositeSamples%600)==0)
        Log(L"GAMMA_RASTER_COMPOSITE frame=%u viewport_before=%.0fx%.0f origin=%.0f,%.0f "
            L"scissor_count=%u scissor_first=%ld,%ld,%ld,%ld output=1920x1080 "
            L"raster_target_role=%u raster_target=%ux%u",
            frame,g_gammaQualityPriorViewport.Width,g_gammaQualityPriorViewport.Height,
            g_gammaQualityPriorViewport.TopLeftX,g_gammaQualityPriorViewport.TopLeftY,
            g_gammaQualityPriorScissorCount,
            g_gammaQualityPriorScissorCount?g_gammaQualityPriorScissors[0].left:0,
            g_gammaQualityPriorScissorCount?g_gammaQualityPriorScissors[0].top:0,
            g_gammaQualityPriorScissorCount?g_gammaQualityPriorScissors[0].right:0,
            g_gammaQualityPriorScissorCount?g_gammaQualityPriorScissors[0].bottom:0,
            g_gammaRasterCurrentRole,g_gammaRasterCurrentW,g_gammaRasterCurrentH);
    const D3D11_VIEWPORT native{0.f,0.f,1920.f,1080.f,
        g_gammaQualityPriorViewport.MinDepth,g_gammaQualityPriorViewport.MaxDepth};
    ctx->RSSetViewports(1,&native);
    if(g_gammaQualityPriorScissorCount==1 &&
       g_gammaQualityPriorScissors[0].left==0 &&
       g_gammaQualityPriorScissors[0].top==0 &&
       g_gammaQualityPriorScissors[0].right==static_cast<LONG>(g_gammaInputW) &&
       g_gammaQualityPriorScissors[0].bottom==static_cast<LONG>(g_gammaInputH)) {
        const D3D11_RECT full{0,0,1920,1080};
        ctx->RSSetScissorRects(1,&full);
    }
    g_gammaQualityRasterActive=true;
    return true;
}
static void GammaRestoreQualityRaster(ID3D11DeviceContext* ctx) {
    if(!g_gammaQualityRasterActive)return;
    ctx->RSSetViewports(1,&g_gammaQualityPriorViewport);
    if(g_gammaQualityPriorScissorCount)
        ctx->RSSetScissorRects(g_gammaQualityPriorScissorCount,g_gammaQualityPriorScissors);
    g_gammaQualityRasterActive=false;
}
// v078: v071 proved native engine globals are restored but the D3D viewport
// remains 1280x720 AT THE ACTUAL 1920x1080 swap-chain draw. This corrective
// operation must not depend on NGX's initial 48-frame qualification: otherwise
// the picture never reaches the full back buffer, even on skipped frames.
// No vtable/OM/RS detours: use the immediate D3D11 context on the game's
// authenticated final-composite thread, and restore the exact prior raster state.
// v078: Draw-scoped native UI raster, never a persistent post-3D state change.
// v073/v074 switched RSSetViewports at B38E70 return: although the HUD was
// positioned correctly, it suppressed most real swap-chain Draw calls and
// left stale pixels. v072's continuous output remains the baseline.
// Only native-sized, verified UI targets AFTER the engine final-composite
// callback can be expanded, with full viewport/scissor restoration after
// the exact UI draw. The next scene frame observes the engine's own state.
static thread_local bool t_gammaUiAfterComposite = false;
static LONG64 g_gammaUiNativeDraws=0;
static LONG64 g_gammaUiNativeScissorFixes=0;
static LONG64 g_gammaUiNativeSkipped=0;
static LONG64 g_gammaUiNativeTargets=0;
static LONG64 g_gammaUiPreCompositeGeneration=0;
static LONG64 g_gammaUiPreCompositeSamples=0;
struct GammaScopedUiRaster {
    bool active=false;
    bool scissorChanged=false;
    D3D11_VIEWPORT original{};
    UINT scissorCount=0;
    D3D11_RECT scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
};
static GammaScopedUiRaster GammaBeginNativeUiDraw(ID3D11DeviceContext* ctx) {
    GammaScopedUiRaster scope{};
    // Observe early draws after every renderer switch. Do not change graphics
    // state based on this trace; it may include non-UI geometry.
    if(ctx && ctx==g_ngxContext && g_gammaQualityRequested &&
       g_gammaQualityAllocated && !g_gammaTrialRejected && g_state==1 &&
       g_gammaUiPreCompositeGeneration!=g_gammaRendererGeneration) {
        g_gammaUiPreCompositeGeneration=g_gammaRendererGeneration;
        g_gammaUiPreCompositeSamples=0;
    }
    if(ctx && ctx==g_ngxContext && g_gammaQualityRequested &&
       g_gammaQualityAllocated && !g_gammaTrialRejected && g_state==1 &&
       !t_gammaUiAfterComposite && !t_gammaVerifiedFinalCompositeInside &&
       g_gammaUiPreCompositeSamples<8) {
        D3D11_VIEWPORT earlyVp{};
        UINT earlyCount=1;
        ctx->RSGetViewports(&earlyCount,&earlyVp);
        ++g_gammaUiPreCompositeSamples;
        Log(L"GAMMA_UI_PRECOMPOSITE_DRAW_OBSERVE generation=%lld frame=%u sample=%lld "
            L"vp_count=%u vp=%.0fx%.0f scene=%ux%u output=%ux%u stage_after_composite=0",
            g_gammaRendererGeneration,ReadEngineFrame(),g_gammaUiPreCompositeSamples,
            earlyCount,earlyVp.Width,earlyVp.Height,g_gammaInputW,g_gammaInputH,
            g_displayWidth,g_displayHeight);
    }
    if(!ctx || ctx!=g_ngxContext || !g_gammaQualityRequested ||
       !g_gammaQualityAllocated || g_gammaTrialRejected || g_state!=1 ||
       !t_gammaUiAfterComposite || t_gammaVerifiedFinalCompositeInside ||
       GetCurrentThreadId()!=g_renderTid ||
       !g_gammaLatestCtorRenderer || g_gammaQualityRasterActive)
        return scope;
    // Identify the *actual* RTV, not merely a guessed post-scene boundary.
    ID3D11RenderTargetView* rtv=nullptr;
    ctx->OMGetRenderTargets(1,&rtv,nullptr);
    if(!rtv)return scope;
    ID3D11Resource* resource=nullptr;
    rtv->GetResource(&resource);
    rtv->Release();
    if(!resource)return scope;
    ID3D11Texture2D* tex=nullptr;
    const HRESULT hr=resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&tex));
    if(FAILED(hr) || !tex) {resource->Release();return scope;}
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    const bool native=desc.Width==g_displayWidth && desc.Height==g_displayHeight &&
        desc.SampleDesc.Count==1 && (desc.BindFlags&D3D11_BIND_RENDER_TARGET)!=0;
    bool eligible=false;
    if(native) {
        // Verified native UI buffers from the GAMMA renderer's target table.
        const void* renderer=reinterpret_cast<void*>(g_gammaLatestCtorRenderer);
        const auto ui=GetTextureFromSlot(const_cast<void*>(renderer),0x498);
        const auto viewport=GetTextureFromSlot(const_cast<void*>(renderer),0x480);
        const auto scopeRt=GetTextureFromSlot(const_cast<void*>(renderer),0x488);
        eligible=(tex==ui || tex==viewport || tex==scopeRt);
        // Some HUD geometry goes directly to the real swap-chain RTV.
        if(!eligible) {
            IDXGIResource* dxgi=nullptr;
            if(SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIResource),
                reinterpret_cast<void**>(&dxgi))) && dxgi) {
                DXGI_USAGE usage=0;
                eligible=SUCCEEDED(dxgi->GetUsage(&usage)) &&
                    (usage&DXGI_USAGE_BACK_BUFFER)!=0;
                dxgi->Release();
            }
        }
    }
    tex->Release();
    resource->Release();
    if(!eligible)return scope;
    const LONG64 target=++g_gammaUiNativeTargets;
    UINT count=1;
    ctx->RSGetViewports(&count,&scope.original);
    if(count!=1 || scope.original.TopLeftX!=0.f ||
       scope.original.TopLeftY!=0.f ||
       scope.original.Width!=float(g_gammaInputW) ||
       scope.original.Height!=float(g_gammaInputH) ||
       scope.original.MinDepth<0.f || scope.original.MaxDepth>1.f) {
        if(g_gammaUiNativeSkipped++ < 8)
            Log(L"GAMMA_UI_DRAW_VIEWPORT_SKIP frame=%u target=%lld count=%u vp=%.0fx%.0f expected_low=%ux%u native=%ux%u",
                ReadEngineFrame(),target,count,scope.original.Width,scope.original.Height,
                g_gammaInputW,g_gammaInputH,g_displayWidth,g_displayHeight);
        return scope;
    }
    scope.scissorCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&scope.scissorCount,scope.scissor);
    D3D11_VIEWPORT nativeVp=scope.original;
    nativeVp.Width=float(g_displayWidth);
    nativeVp.Height=float(g_displayHeight);
    ctx->RSSetViewports(1,&nativeVp);
    if(scope.scissorCount==1 && scope.scissor[0].left==0 &&
       scope.scissor[0].top==0 &&
       scope.scissor[0].right==LONG(g_gammaInputW) &&
       scope.scissor[0].bottom==LONG(g_gammaInputH)) {
        const D3D11_RECT nativeSc{0,0,LONG(g_displayWidth),LONG(g_displayHeight)};
        ctx->RSSetScissorRects(1,&nativeSc);
        scope.scissorChanged=true;
        ++g_gammaUiNativeScissorFixes;
    }
    scope.active=true;
    const LONG64 n=++g_gammaUiNativeDraws;
    if(n<=8 || n%300==0)
        Log(L"GAMMA_UI_DRAW_NATIVE_BEGIN frame=%u count=%lld low=%ux%u native=%ux%u scissor=%d target_verified=1 draw_scoped=1",
            ReadEngineFrame(),n,g_gammaInputW,g_gammaInputH,
            g_displayWidth,g_displayHeight,scope.scissorChanged?1:0);
    return scope;
}
static void GammaEndNativeUiDraw(ID3D11DeviceContext* ctx,const GammaScopedUiRaster& scope) {
    if(!scope.active || !ctx)return;
    ctx->RSSetViewports(1,&scope.original);
    if(scope.scissorChanged && scope.scissorCount)
        ctx->RSSetScissorRects(scope.scissorCount,scope.scissor);
    if(g_gammaUiNativeDraws<=8 || g_gammaUiNativeDraws%300==0)
        Log(L"GAMMA_UI_DRAW_NATIVE_RESTORED frame=%u count=%lld raster_restored=1",
            ReadEngineFrame(),g_gammaUiNativeDraws);
}

static bool GammaIsNativeQualityViewport(ID3D11DeviceContext* ctx) {
    if(!ctx)return false;
    D3D11_VIEWPORT vp{};
    UINT count=1;
    ctx->RSGetViewports(&count,&vp);
    return count==1 && vp.TopLeftX==0.f && vp.TopLeftY==0.f &&
           vp.Width==static_cast<float>(g_displayWidth) &&
           vp.Height==static_cast<float>(g_displayHeight);
}
static bool GammaScopeRealBackbufferRaster(ID3D11DeviceContext* ctx,UINT indices,bool indexed) {
    if(!g_gammaQualityRequested || !g_gammaQualityAllocated ||
       !t_gammaWorldFrameInside || !t_gammaVerifiedFinalCompositeInside ||
       !indexed || indices!=6 || !ctx || ctx!=g_ngxContext ||
       !g_ngxDevice || GetCurrentThreadId()!=g_renderTid ||
       g_gammaQualityRasterActive || g_gammaTrialRejected) return false;
    ID3D11RenderTargetView* rtv=nullptr;
    ctx->OMGetRenderTargets(1,&rtv,nullptr);
    if(!rtv)return false;
    ID3D11Resource* resource=nullptr;
    rtv->GetResource(&resource);
    rtv->Release();
    if(!resource)return false;
    ID3D11Texture2D* target=nullptr;
    const HRESULT texHr=resource->QueryInterface(__uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&target));
    resource->Release();
    if(FAILED(texHr) || !target)return false;
    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    const bool native=desc.Width==g_displayWidth && desc.Height==g_displayHeight &&
        desc.SampleDesc.Count==1 && (desc.BindFlags&D3D11_BIND_RENDER_TARGET);
    bool backbuffer=false;
    if(native) {
        IDXGIResource* dxgi=nullptr;
        if(SUCCEEDED(target->QueryInterface(__uuidof(IDXGIResource),
            reinterpret_cast<void**>(&dxgi))) && dxgi) {
            DXGI_USAGE usage=0;
            backbuffer=SUCCEEDED(dxgi->GetUsage(&usage)) &&
                (usage&DXGI_USAGE_BACK_BUFFER)!=0;
            dxgi->Release();
        }
    }
    target->Release();
    if(!backbuffer)return false;
    const LONG64 total=++g_gammaBackbufferDrawProbes;
    const unsigned frame=ReadEngineFrame();
    D3D11_VIEWPORT vp{};
    UINT count=1;
    ctx->RSGetViewports(&count,&vp);
    if(count!=1 || vp.TopLeftX!=0.f || vp.TopLeftY!=0.f ||
       vp.Width!=static_cast<float>(g_gammaInputW) ||
       vp.Height!=static_cast<float>(g_gammaInputH)) {
        if(count==1 && vp.TopLeftX==0.f && vp.TopLeftY==0.f &&
           vp.Width==static_cast<float>(g_displayWidth) &&
           vp.Height==static_cast<float>(g_displayHeight))
            ++g_gammaBackbufferDrawViewportAlreadyNative;
        else {
            ++g_gammaBackbufferDrawOtherViewport;
            if(g_gammaBackbufferDrawOtherViewport<=8)
                Log(L"GAMMA_BACKBUFFER_VIEWPORT_REFUSED frame=%u count=%u "
                    L"vp=%.0f,%.0f,%.0fx%.0f expected=%ux%u "
                    L"output=%ux%u no_mutation=1",frame,count,
                    vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height,
                    g_gammaInputW,g_gammaInputH,g_displayWidth,g_displayHeight);
        }
        return false;
    }
    const bool fixed=GammaPrepareQualityRaster(ctx,frame);
    if(fixed) {
        const LONG64 fixes=++g_gammaDrawNativeViewportFixes;
        if(fixes<=8 || (fixes%300)==0)
            Log(L"GAMMA_BACKBUFFER_VIEWPORT_FIX frame=%u count=%lld "
                L"before=%ux%u after=%ux%u qualified_engine_composite=1 "
                L"independent_of_ngx=1 backbuffer_draws_seen=%lld",
                frame,fixes,g_gammaInputW,g_gammaInputH,
                g_displayWidth,g_displayHeight,total);
    }
    return fixed;
}
static void GammaVisibleRestoreAfterDraw(ID3D11DeviceContext* ctx,unsigned frame) {
    if(!g_gammaVisibleBindingActive)return;
    bool restored=true;
    for(unsigned i=0;i<g_gammaVisibleBindCount;++i) {
        auto& saved=g_gammaVisibleBound[i];
        ID3D11ShaderResourceView* current=nullptr;
        ctx->PSGetShaderResources(saved.slot,1,&current);
        if(current==g_gammaVisibleOutputSrv) {
            ctx->PSSetShaderResources(saved.slot,1,&saved.original);
            ID3D11ShaderResourceView* after=nullptr;
            ctx->PSGetShaderResources(saved.slot,1,&after);
            if(after!=saved.original)restored=false;
            if(after)after->Release();
        } else restored=false; // do not override a newer game binding
        if(current)current->Release();
        if(saved.original)saved.original->Release();
        saved=GammaVisibleBoundSlot{};
    }
    g_gammaVisibleBindCount=0;
    g_gammaVisibleBindingActive=false;
    GammaRestoreQualityRaster(ctx);
    if(!restored) {
        ++g_gammaVisibleRestoreFailures;
        GammaVisibleFault(L"PS_SHADER_RESOURCE_RESTORE_MISMATCH");
    } else ++g_gammaVisiblePresented;
    if(g_gammaVisiblePresented<=5 || (g_gammaVisiblePresented%300)==0 || !restored)
        Log(L"GAMMA_VISIBLE_PRESENT frame=%u total=%lld eval_success=%lld "
            L"original_draw_returned=1 restored=%d copyback=0",
            frame,g_gammaVisiblePresented,g_gammaVisibleEvalSuccesses,restored?1:0);
}
static bool GammaVisibleEvaluateAndBind(ID3D11DeviceContext* ctx,unsigned frame) {
    if(!ctx || ctx!=g_ngxContext || g_gammaVisibleFault ||
       !g_ngxDevice || !g_feature || !g_params || g_state!=1 ||
       frame==g_gammaVisibleLastFrame || GetCurrentThreadId()!=g_renderTid) return false;
    if(g_gammaPresetRecreatePending) {
        // Shared MP feature-mode recreation, on the validated draw thread,
        // without changing engine texture sizes or the output binding.
        if(!EnsureIsolatedContext() || !RecreateFeature(
              g_gammaSelectedMode,g_gammaInputW,g_gammaInputH)) {
            GammaVisibleFault(L"SHARED_PRESET_FEATURE_RECREATE_FAILED");return false;
        }
        g_gammaPresetRecreatePending=false;
        g_gammaLastAppliedPreset=g_preset;
        g_gammaHistoryResetPending=true;
        Log(L"GAMMA_SHARED_PRESET_APPLIED preset=%ls mode=%ls",
            PresetName(g_preset),ModeName(g_gammaSelectedMode));
    }
    g_gammaVisibleLastFrame=frame; // one evaluation per verified engine frame
    ID3D11Texture2D* scene[3]{};
    D3D11_TEXTURE2D_DESC d[3]{};
    if(!GammaVisibleValidateScene(scene,d) || !EnsureIsolatedContext() ||
       !GammaVisibleEnsurePrivateTextures(scene,d)) {
        GammaVisibleFault(L"SCENE_OR_PRIVATE_TEXTURE_OR_CONTEXT_INVALID"); return false;
    }
    GammaD3DBindingSnapshot before=GammaSampleBindings(ctx);
    const auto beforeRtv=before.rtv;
    const auto beforeSrv=before.sceneSrv;
    const auto beforeCount=before.viewportCount;
    const auto beforeViewport=before.viewport;
    GammaReleaseBindings(before);
    ID3DDeviceContextState* previous=nullptr;
    g_context1->SwapDeviceContextState(g_isolatedState,&previous);
    if(!previous) {
        GammaVisibleFault(L"NO_PREVIOUS_CONTEXT_STATE");return false;
    }
    // NGX sees only the bridge's own textures. Reuse their identity and a
    // single DLAA feature across frames to preserve temporal history.
    for(int i=0;i<3;++i) ctx->CopyResource(g_gammaVisibleInput[i],scene[i]);
    // Preserve temporal history across short compositor gaps. Renderer changes
    // and genuine discontinuities reset it, but ordinary missing draws do not.
    const bool discontinuity=g_gammaVisiblePreviousSuccessFrame &&
        (frame<=g_gammaVisiblePreviousSuccessFrame ||
         frame-g_gammaVisiblePreviousSuccessFrame>180);
    const bool reset=(g_gammaHistoryResetPending || discontinuity);
    if(reset)Log(L"GAMMA_TEMPORAL_RESET frame=%u previous=%u reason=%ls",
        frame,g_gammaVisiblePreviousSuccessFrame,
        discontinuity?L"LARGE_OR_REVERSED_FRAME_GAP":L"INITIAL_OR_RENDERER_REBUILD");
    // GAMMA currently supplies no projection jitter; this matches visually
    // validated native DLAA. The same core handles nonzero MP jitter.
    const SharedNgxFrame ngxFrame{ctx,g_gammaVisibleInput[0],g_gammaVisibleInput[1],
        g_gammaVisibleInput[2],g_gammaVisibleOutput,
        d[0].Width,d[0].Height,reset,0.f,0.f,GetMeasuredDeltaMs()};
    ++g_gammaVisibleEvalAttempts;
    const auto result=EvaluateSharedNgxFrame(ngxFrame);
    const bool ok=NVSDK_NGX_SUCCEED(result);
    if(ok){++g_gammaVisibleEvalSuccesses;g_gammaVisiblePreviousSuccessFrame=frame;
        g_gammaHistoryResetPending=false;}
    if(g_gammaVisibleEvalAttempts<=5 || (g_gammaVisibleEvalAttempts%300)==0 || !ok)
        Log(L"GAMMA_VISIBLE_NGX_EVAL frame=%u attempt=%lld success=%d result=0x%08X "
            L"input_private=1 output_private=1 jitter=zero copyback=0",
            frame,g_gammaVisibleEvalAttempts,ok?1:0,unsigned(result));
    ctx->ClearState(); // isolate all NGX bindings
    if(!ok && g_feature) {
        const auto rc=NVSDK_NGX_D3D11_ReleaseFeature(g_feature);
        if(NVSDK_NGX_SUCCEED(rc)) g_feature=nullptr;
        Log(L"GAMMA_VISIBLE_FAILED_FEATURE_RELEASE result=0x%08X",unsigned(rc));
    }
    g_context1->SwapDeviceContextState(previous,nullptr);
    previous->Release();
    GammaD3DBindingSnapshot after=GammaSampleBindings(ctx);
    GammaD3DBindingSnapshot compare{};
    compare.rtv=beforeRtv; compare.sceneSrv=beforeSrv;
    compare.viewportCount=beforeCount; compare.viewport=beforeViewport;
    const bool same=compare.rtv==after.rtv && compare.sceneSrv==after.sceneSrv &&
                    GammaSameViewport(compare,after);
    GammaReleaseBindings(after);
    if(!same || !ok) {
        GammaVisibleFault(!same?L"RENDER_CONTEXT_NOT_RESTORED":L"NGX_EVALUATE_FAILED");
        return false;
    }
    // MP redirects a PS SRV. GAMMA has already bound that SRV when its
    // DrawIndexed hook fires, so substitute at the draw and restore after it.
    if(!GammaVisibleSubstitute(ctx,scene[0])) {
        GammaVisibleFault(L"OUTPUT_SRV_HANDOFF_UNAVAILABLE");return false;
    }
    return true;
}

// v078: high-level composite discovery. Calls from the driver often have
// return addresses outside AnomalyDX11AVX_gam.exe; unwind the complete stack
// to recover the engine frames. No modifying GPU calls here.
static void GammaTraceActualCompositeBoundary(ID3D11DeviceContext* ctx,
    unsigned frame,void* drawReturn,uintptr_t outputAddress,
    const D3D11_TEXTURE2D_DESC& desc) {
    const unsigned generation=static_cast<unsigned>(g_gammaRendererGeneration);
    if(g_gammaBoundarySampleGeneration!=generation) {
        g_gammaBoundarySampleGeneration=generation;
        g_gammaBoundarySamplesThisGeneration=0;
    }
    if(g_gammaBoundarySamplesThisGeneration>=3)return;
    ++g_gammaBoundarySamplesThisGeneration;
    ++g_gammaBoundarySamplesTotal;
    D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT vpCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT scCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&vpCount,viewport);
    ctx->RSGetScissorRects(&scCount,scissors);
    ID3D11ShaderResourceView* views[4]{};
    ctx->PSGetShaderResources(0,4,views);
    unsigned widths[4]{},heights[4]{};
    for(unsigned i=0;i<4;++i) {
        if(!views[i])continue;
        ID3D11Resource* resource=nullptr;
        views[i]->GetResource(&resource);
        if(resource) {
            ID3D11Texture2D* texture=nullptr;
            if(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture))) && texture) {
                D3D11_TEXTURE2D_DESC d{};
                texture->GetDesc(&d);
                widths[i]=d.Width;
                heights[i]=d.Height;
                texture->Release();
            }
            resource->Release();
        }
        views[i]->Release();
    }
    Log(L"GAMMA_BOUNDARY_CAPTURE sample=%lld gen=%u frame=%u mode=%ls "
        L"draw_return=%p output=%p size=%ux%u vp_count=%u vp0=%.0f,%.0f,%.0fx%.0f "
        L"sc_count=%u sc0=%ld,%ld,%ld,%ld ps0=%ux%u ps1=%ux%u ps2=%ux%u ps3=%ux%u "
        L"world_scope=%d render_tid=%lu thread=%lu globals=%ux%u "
        L"raw_om=%lld raw_vp=%lld raw_sc=%lld tracer_only=1",
        g_gammaBoundarySamplesTotal,generation,frame,ModeName(g_gammaSelectedMode),
        drawReturn,reinterpret_cast<void*>(outputAddress),desc.Width,desc.Height,
        vpCount,vpCount?viewport[0].TopLeftX:0.f,vpCount?viewport[0].TopLeftY:0.f,
        vpCount?viewport[0].Width:0.f,vpCount?viewport[0].Height:0.f,
        scCount,scCount?scissors[0].left:0,scCount?scissors[0].top:0,
        scCount?scissors[0].right:0,scCount?scissors[0].bottom:0,
        widths[0],heights[0],widths[1],heights[1],widths[2],heights[2],
        widths[3],heights[3],t_gammaWorldFrameInside?1:0,
        g_renderTid,GetCurrentThreadId(),
        ReadOr<unsigned>(g_gammaImageBase+kGammaWidthRva),
        ReadOr<unsigned>(g_gammaImageBase+kGammaHeightRva),
        InterlockedCompareExchange64(&g_gammaRawOmCalls,0,0),
        InterlockedCompareExchange64(&g_gammaRawViewportCalls,0,0),
        InterlockedCompareExchange64(&g_gammaRawScissorCalls,0,0));
    void* frames[24]{};
    const USHORT count=RtlCaptureStackBackTrace(0,24,frames,nullptr);
    unsigned exeFrames=0;
    for(USHORT i=0;i<count;++i) {
        HMODULE module=nullptr;
        const bool resolved=GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[i]),&module)!=FALSE;
        const uintptr_t address=reinterpret_cast<uintptr_t>(frames[i]);
        const uintptr_t base=resolved?reinterpret_cast<uintptr_t>(module):0;
        wchar_t path[MAX_PATH]{};
        const wchar_t* leaf=L"UNKNOWN";
        if(resolved && GetModuleFileNameW(module,path,MAX_PATH)) {
            leaf=path;
            for(const wchar_t* p=path;*p;++p)
                if(*p==L'\\'||*p==L'/')leaf=p+1;
        }
        if(base==g_gammaImageBase)++exeFrames;
        Log(L"GAMMA_BOUNDARY_STACK sample=%lld depth=%u module=%ls "
            L"rva=0x%llX address=%p is_game_exe=%d",
            g_gammaBoundarySamplesTotal,unsigned(i),leaf,
            static_cast<unsigned long long>(base?address-base:0),
            frames[i],base==g_gammaImageBase?1:0);
    }
    Log(L"GAMMA_BOUNDARY_STACK_DONE sample=%lld stack_count=%u exe_frames=%u "
        L"quality=%d diagnostic_no_gpu_mutation=1",
        g_gammaBoundarySamplesTotal,unsigned(count),exeFrames,
        g_gammaQualityRequested?1:0);
}

static bool GammaTryCompositeEvaluation(ID3D11DeviceContext* ctx,UINT primitives,bool indexed,void* returnAddress) {
    if(!g_gammaTrialHooksReady || g_gammaTrialRejected ||
       g_gammaInsideDraw || !g_gammaNativeTrialActive ||
       !g_gammaLatestCtorRenderer || ctx!=g_ngxContext ||
       GetCurrentThreadId()!=g_renderTid ||
       g_state!=1 || g_fatalEvaluationError) return false;
    // Compositors seen in the known working engine use 6 indexed indices.
    // We *discover* the matching draw using actual shader-resource identity.
    if ((indexed && primitives!=6) || (!indexed && primitives!=3 && primitives!=4)) return false;
    const uintptr_t mod=g_gammaImageBase;
    if (!ReadOr<uintptr_t>(mod+kGammaLevelRva) ||
        !ReadOr<uintptr_t>(mod+kGammaHudRva)) return false;
    ID3D11Texture2D* color=GetTextureFromSlot(reinterpret_cast<void*>(g_gammaLatestCtorRenderer),kGammaColorSlot);
    if (!color) return false;
    ID3D11ShaderResourceView* views[4]{};
    ctx->PSGetShaderResources(0,4,views);
    bool colorBound=false;
    for(unsigned i=0;i<4;++i) {
        if(views[i]) {
            ID3D11Resource* res=nullptr;
            views[i]->GetResource(&res);
            colorBound|=(res==static_cast<ID3D11Resource*>(color));
            if(res)res->Release();
            views[i]->Release();
        }
    }
    if(!colorBound) return false;
    // For the final composite the output RTV must be a separate full-native
    // target. Native-sized intermediate passes are not sufficient evidence;
    // the 48-frame ambiguity gate below must also succeed.
    ID3D11RenderTargetView* rtv=nullptr;
    ctx->OMGetRenderTargets(1,&rtv,nullptr);
    if(!rtv) return false;
    ID3D11Resource* output=nullptr;
    rtv->GetResource(&output);
    rtv->Release();
    const bool separate=output && output!=static_cast<ID3D11Resource*>(color);
    D3D11_TEXTURE2D_DESC outputDesc{};
    bool native=false, usageKnown=false, swapchainBackbuffer=false;
    DXGI_USAGE dxgiUsage=0;
    if(separate) {
        ID3D11Texture2D* outTex=nullptr;
        if(SUCCEEDED(output->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&outTex))) && outTex) {
            outTex->GetDesc(&outputDesc);
            native=outputDesc.Width==g_displayWidth && outputDesc.Height==g_displayHeight &&
                   outputDesc.SampleDesc.Count==1 && (outputDesc.BindFlags & D3D11_BIND_RENDER_TARGET);
            if(native) {
                // DXGI_USAGE_BACK_BUFFER is authoritative swapchain membership,
                // unlike native dimensions, target bind flags, or shader names.
                IDXGIResource* dxgiResource=nullptr;
                if(SUCCEEDED(outTex->QueryInterface(__uuidof(IDXGIResource),reinterpret_cast<void**>(&dxgiResource))) && dxgiResource) {
                    usageKnown=SUCCEEDED(dxgiResource->GetUsage(&dxgiUsage));
                    swapchainBackbuffer=usageKnown && ((dxgiUsage & DXGI_USAGE_BACK_BUFFER)!=0);
                    dxgiResource->Release();
                }
            }
            outTex->Release();
        }
    }
    const uintptr_t outputAddress=reinterpret_cast<uintptr_t>(output);
    if(output)output->Release();
    if(!native) return false;
    ++g_gammaCandidateDraws;
    const unsigned frame=ReadEngineFrame();
    if(!frame) return false;
    const uintptr_t ret=reinterpret_cast<uintptr_t>(returnAddress);
    const uintptr_t callerRva=ret>=g_gammaImageBase && ret-g_gammaImageBase<0x2000000 ? ret-g_gammaImageBase : 0;
    // Log BOTH same-size candidates before any filtering; this provides the
    // exact call-site, output identity, bind flags and authoritative DXGI usage.
    if(g_gammaCandidateDraws<=24 || (g_gammaCandidateDraws%1200)==0)
        Log(L"GAMMA_COMPOSITE_CLASSIFIED frame=%u index=%lld caller_rva=0x%llX caller_ptr=%p indexed=%d primitives=%u output=%p fmt=%u bind=0x%X dxgi_known=%d dxgi_usage=0x%X backbuffer=%d",
            frame,g_gammaCandidateDraws,static_cast<unsigned long long>(callerRva),returnAddress,
            indexed?1:0,primitives,reinterpret_cast<void*>(outputAddress),
            unsigned(outputDesc.Format),outputDesc.BindFlags,usageKnown?1:0,
            unsigned(dxgiUsage),swapchainBackbuffer?1:0);
    if(!usageKnown) ++g_gammaUnknownUsageDraws;
    if(!swapchainBackbuffer) {
        ++g_gammaNonBackbufferDraws;
        return false; // Never write to a native-sized intermediate just because it matches.
    }
    ++g_gammaBackbufferDraws;
    // Capture the precise ownership boundary before the 48-frame proof;
    // failed Quality runs rarely make it past that gate.
    GammaTraceActualCompositeBoundary(ctx,frame,returnAddress,outputAddress,outputDesc);
    if(g_gammaLastCandidateFrame!=frame) {
        // Initial 48-frame proof ONLY. Once verified it is sticky for this
        // renderer generation, so a missing compositor frame cannot black out
        // DLAA for the next 48 frames.
        if(g_gammaLastCandidateFrame && g_gammaCandidateThisFrame==1 &&
           frame==g_gammaLastCandidateFrame+1 && returnAddress &&
           reinterpret_cast<uintptr_t>(returnAddress)==g_gammaLastBackbufferCaller)
            ++g_gammaQualifiedFrames;
        else g_gammaQualifiedFrames=0;
        g_gammaCandidateThisFrame=0;
        g_gammaLastCandidateFrame=frame;
    }
    g_gammaLastBackbufferCaller=reinterpret_cast<uintptr_t>(returnAddress);
    ++g_gammaCandidateThisFrame;
    if(g_gammaCandidateThisFrame>1) {
        GammaRecordSkip(frame,4,L"MULTIPLE_BACKBUFFER_PASSES");
        // Multiple genuine swapchain draws still have ambiguous semantics.
        if(!g_gammaTrialRejected)
            Log(L"GAMMA_TRIAL_ABORT reason=MULTIPLE_BACKBUFFER_COMPOSITES frame=%u count=%u native_copyback=0",
                frame,g_gammaCandidateThisFrame);
        g_gammaTrialRejected=true;
        return false;
    }
    if(g_gammaBackbufferDraws<=10 || g_gammaBackbufferDraws%600==0)
        Log(L"GAMMA_SWAPCHAIN_COMPOSITE frame=%u caller_rva=0x%llX stable=%u backbuffer_draws=%lld copyback_enabled=%d",
            frame,static_cast<unsigned long long>(callerRva),g_gammaQualifiedFrames,
            g_gammaBackbufferDraws,g_gammaTrialCopyback?1:0);
    if(!g_gammaCompositeValidated) {
        if(t_gammaVerifiedFinalCompositeInside) {
            // v070 stack + verified GAMMA E8 final-composite hook is stronger
            // proof than 48 consecutive driver draw return addresses. In
            // Quality, that old gate never reached 48 even on a correct draw.
            g_gammaCompositeValidated=true;
            g_gammaValidatedCaller=ret;
            ++g_gammaVerifiedCompositeFastQualifications;
            Log(L"GAMMA_COMPOSITE_ENGINE_VERIFIED frame=%u caller=%p "
                L"verification=EXACT_FINAL_HOOK_AND_DXGI_BACKBUFFER "
                L"initial_48_proof_bypassed=1",frame,returnAddress);
        } else {
            if(g_gammaQualifiedFrames<48) {
                GammaRecordSkip(frame,0,L"INITIAL_48_FRAME_PROOF");
                return false;
            }
            g_gammaCompositeValidated=true;
            g_gammaValidatedCaller=ret;
            Log(L"GAMMA_COMPOSITE_STICKY_VERIFIED frame=%u stable=%u caller=%p next=NO_REWARM",
                frame,g_gammaQualifiedFrames,returnAddress);
        }
    }
    // Different call sites are not automatically safe even if the resource is
    // a DXGI backbuffer. Do not present on an unfamiliar compositor.
    if(g_gammaValidatedCaller!=ret) {
        GammaRecordSkip(frame,1,L"CALLSITE_CHANGED");return false;
    }
    if(g_gammaVisibleFault) {
        GammaRecordSkip(frame,2,L"GPU_EVALUATION_FAULT_LATCHED");return false;
    }
    if(!g_gammaMenuEnabled && !g_gammaQualityRequested) {
        GammaRecordSkip(frame,8,L"SHARED_MENU_DISABLED");return false;
    }
    g_gammaInsideDraw=true;
    // Native viewport setup is done by the surrounding verified Draw hook,
    // even if NGX is skipped; NEVER resize arbitrary draws from here.
    const bool rasterReady=!g_gammaQualityRequested || !g_gammaQualityAllocated ||
        g_gammaQualityRasterActive || GammaIsNativeQualityViewport(ctx);
    const bool bound=rasterReady && GammaVisibleEvaluateAndBind(ctx,frame);
    if(!bound)
        GammaRecordSkip(frame,3,g_gammaVisibleFault?L"NGX_OR_HANDOFF_FAILED":
            (rasterReady?L"EVAL_NOT_PREPARED":L"QUALITY_NATIVE_VIEWPORT_NOT_READY"));
    g_gammaInsideDraw=false;
    return bound;
}

static void STDMETHODCALLTYPE GammaDrawAudit(ID3D11DeviceContext* ctx,UINT vertices,UINT first) {
    ++g_gammaTrialDraws;
    // Nonindexed draws are not the verified six-index back-buffer composite.
    const bool prepared=GammaTryCompositeEvaluation(ctx,vertices,false,_ReturnAddress());
    if(prepared) {
        g_gammaInsideDraw=true;
        if(g_gammaVisiblePresented<5)Log(L"GAMMA_VISIBLE_ORIGINAL_DRAW_ENTER indexed=0 frame=%u",g_gammaVisibleLastFrame);
    }
    const GammaScopedUiRaster uiRaster=GammaBeginNativeUiDraw(ctx);
    if(g_gammaOriginalDraw)g_gammaOriginalDraw(ctx,vertices,first);
    GammaEndNativeUiDraw(ctx,uiRaster);
    if(prepared) {
        GammaVisibleRestoreAfterDraw(ctx,g_gammaVisibleLastFrame);
        g_gammaInsideDraw=false;
    }
}
static void STDMETHODCALLTYPE GammaDrawIndexedAudit(ID3D11DeviceContext* ctx,UINT indices,UINT first,INT baseVertex) {
    ++g_gammaTrialDraws;
    if(indices==6) {
        const LONG64 sample=InterlockedIncrement64(&g_gammaRawIndexedSixCalls);
        if(sample<=4 || sample%500000==0)
            Log(L"GAMMA_DRAW6_RAW count=%lld frame=%u ctx=%p immediate=%p "
                L"world_scope=%d thread=%lu mode=%ls",
                sample,ReadEngineFrame(),ctx,g_ngxContext,
                t_gammaWorldFrameInside?1:0,GetCurrentThreadId(),ModeName(g_gammaSelectedMode));
    }
    // Exact verified GAMMA final-composite + DXGI-owned back buffer: correct
    // the viewport for the ORIGINAL engine draw regardless of NGX readiness.
    const bool corrected=GammaScopeRealBackbufferRaster(ctx,indices,true);
    const bool prepared=GammaTryCompositeEvaluation(ctx,indices,true,_ReturnAddress());
    if(prepared) {
        g_gammaInsideDraw=true;
        if(g_gammaVisiblePresented<5)Log(L"GAMMA_VISIBLE_ORIGINAL_DRAW_ENTER indexed=1 frame=%u",g_gammaVisibleLastFrame);
    }
    const GammaScopedUiRaster uiRaster=GammaBeginNativeUiDraw(ctx);
    if(g_gammaOriginalDrawIndexed)g_gammaOriginalDrawIndexed(ctx,indices,first,baseVertex);
    GammaEndNativeUiDraw(ctx,uiRaster);
    if(prepared) {
        GammaVisibleRestoreAfterDraw(ctx,g_gammaVisibleLastFrame);
        g_gammaInsideDraw=false;
    }
    if(corrected) {
        // Retain v072's exact post-draw state restoration, including NGX
        // shader-resource cleanup, before crossing into native UI raster.
        GammaRestoreQualityRaster(ctx);
        const LONG64 restores=++g_gammaDrawNativeViewportRestores;
        if(restores<=8 || restores%300==0)
            Log(L"GAMMA_BACKBUFFER_VIEWPORT_RESTORED frame=%u count=%lld "
                L"ngx_presented=%d",ReadEngineFrame(),restores,prepared?1:0);
    }
}

// v078: passive observer for CHW::UpdateViews diagnostics. The engine error
// can stem from GetBuffer, CreateRTV, or an earlier failed swapchain reset;
// this only captures failures in CreateRTV, it does not claim to explain all.
using GammaCreateRtvFn=HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*,ID3D11Resource*,
    const D3D11_RENDER_TARGET_VIEW_DESC*,ID3D11RenderTargetView**);
static GammaCreateRtvFn g_gammaOriginalCreateRtv=nullptr;
static volatile LONG64 g_gammaCreateRtvCalls=0;
static volatile LONG64 g_gammaCreateRtvFailures=0;
static bool g_gammaRtvAuditInstalled=false;
static HRESULT STDMETHODCALLTYPE GammaCreateRtvAudit(ID3D11Device* dev,ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* desc,ID3D11RenderTargetView** out) {
    if(!g_gammaOriginalCreateRtv) return E_FAIL;
    const HRESULT hr=g_gammaOriginalCreateRtv(dev,resource,desc,out);
    const LONG64 count=InterlockedIncrement64(&g_gammaCreateRtvCalls);
    if(FAILED(hr)) InterlockedIncrement64(&g_gammaCreateRtvFailures);
    if(count<=5 || (FAILED(hr) && g_gammaCreateRtvFailures<=20))
        Log(L"GAMMA_RTV_CREATE_AUDIT count=%lld hr=0x%08X resource=%p desc=%p output=%p failed_total=%lld",
            count,static_cast<unsigned>(hr),resource,desc,out?*out:nullptr,g_gammaCreateRtvFailures);
    return hr;
}
static bool GammaInstallRtvAudit(ID3D11Device* dev) {
    if(g_gammaRtvAuditInstalled)return true;
    if(!dev)return false;
    void** vt=*reinterpret_cast<void***>(dev);
    if(!vt || !vt[9])return false;
    MH_STATUS a=MH_CreateHook(vt[9],reinterpret_cast<void*>(&GammaCreateRtvAudit),
                               reinterpret_cast<void**>(&g_gammaOriginalCreateRtv));
    if(a==MH_OK)a=MH_EnableHook(vt[9]);
    g_gammaRtvAuditInstalled=(a==MH_OK);
    Log(L"GAMMA_RTV_AUDIT_HOOK installed=%d status=%d method=CreateRenderTargetView index=9",
        g_gammaRtvAuditInstalled?1:0,int(a));
    return g_gammaRtvAuditInstalled;
}

static bool GammaInstallDrawHooks(ID3D11DeviceContext* ctx) {
    if(g_gammaTrialHooksReady)return true;
    auto vt=*reinterpret_cast<void***>(ctx);
    if(!vt || !vt[12] || !vt[13]) return false;
    MH_STATUS a=MH_CreateHook(vt[12],reinterpret_cast<void*>(&GammaDrawIndexedAudit),
                             reinterpret_cast<void**>(&g_gammaOriginalDrawIndexed));
    MH_STATUS b= a==MH_OK ? MH_CreateHook(vt[13],reinterpret_cast<void*>(&GammaDrawAudit),
                             reinterpret_cast<void**>(&g_gammaOriginalDraw)) : MH_ERROR_NOT_EXECUTABLE;
    if(a==MH_OK && b==MH_OK) {
        a=MH_EnableHook(vt[12]);
        if(a==MH_OK) b=MH_EnableHook(vt[13]);
    }
    g_gammaTrialHooksReady=a==MH_OK && b==MH_OK;
    Log(L"GAMMA_NATIVE_DRAW_HOOK indexed_status=%d draw_status=%d installed=%d",
        int(a),int(b),g_gammaTrialHooksReady?1:0);
    // A partially installed hook must still forward correctly. Never unload
    // its trampoline while the game can still execute it. The secondary
    // hook also checks the fail-closed trial flag before any GPU work.
    if(!g_gammaTrialHooksReady)g_gammaTrialRejected=true;
    return g_gammaTrialHooksReady;
}

static bool GammaRecreateAfterRendererRebuild(ID3D11Device* newDevice) {
    if(g_state!=3 || !g_gammaNativeTrialActive || !g_params ||
       !g_ngxContext || !g_ngxDevice || !newDevice || newDevice!=g_ngxDevice ||
       !g_context1 || !EnsureIsolatedContext()) {
        Log(L"GAMMA_REINIT_REFUSED reason=DEVICE_OR_NGX_CONTEXT_CHANGED state=%d",g_state);
        g_gammaTrialRejected=true;return false;
    }
    ID3DDeviceContextState* previous=nullptr;
    g_context1->SwapDeviceContextState(g_isolatedState,&previous);
    if(!previous) {g_gammaTrialRejected=true;return false;}
    const int mode=g_gammaSelectedMode;
    const unsigned w=g_gammaInputW;
    const unsigned h=g_gammaInputH;
    if(mode && (!g_srPlans[mode].valid || g_srPlans[mode].width!=w ||
                g_srPlans[mode].height!=h)) {
        Log(L"GAMMA_SHARED_MODE_REFUSED mode=%ls planned=%ux%u actual_sdk=%ux%u valid=%d",
            ModeName(mode),w,h,g_srPlans[mode].width,g_srPlans[mode].height,g_srPlans[mode].valid?1:0);
        g_gammaTrialRejected=true;return false;
    }
    SetFeatureModeParams(mode,w,h);
    const bool scratch=RefreshScratchForMode();
    const NVSDK_NGX_Result result=scratch ?
        NVSDK_NGX_D3D11_CreateFeature(g_ngxContext,NVSDK_NGX_Feature_SuperSampling,
            g_params,&g_feature) : NVSDK_NGX_Result_FAIL_InvalidParameter;
    g_ngxContext->ClearState();
    g_context1->SwapDeviceContextState(previous,nullptr);
    previous->Release();
    const bool ok=NVSDK_NGX_SUCCEED(result)&&g_feature;
    if(ok) {
        g_activeMode=mode;g_activeRenderWidth=w;g_activeRenderHeight=h;
        g_state=1;g_gammaHistoryResetPending=true;
        ++g_gammaReinitialized;
    } else {g_gammaTrialRejected=true;g_state=2;}
    Log(L"GAMMA_RENDERER_REINIT mode=%ls result=0x%08X success=%d generation=%lld count=%lld",
        ModeName(mode),unsigned(result),ok?1:0,g_gammaRendererGeneration,g_gammaReinitialized);
    return ok;
}
static void GammaTryNativeTrialStartup() {
    if(!g_gammaTrialEnabled || g_gammaTrialRejected ||
       (g_state!=0 && g_state!=3) || !g_gammaLatestCtorRenderer)return;
    const auto mod=g_gammaImageBase;
    if(!ReadOr<uintptr_t>(mod+kGammaLevelRva) || !ReadOr<uintptr_t>(mod+kGammaHudRva)) return;
    void* renderer=reinterpret_cast<void*>(g_gammaLatestCtorRenderer);
    ID3D11Texture2D* c=GetTextureFromSlot(renderer,kGammaColorSlot);
    ID3D11Texture2D* m=GetTextureFromSlot(renderer,kGammaMotionSlot);
    ID3D11Texture2D* d=GetTextureFromSlot(renderer,kGammaDepthSlot);
    if(!GammaPlausibleComTexture(c) || !GammaPlausibleComTexture(m) || !GammaPlausibleComTexture(d)) {
        Log(L"GAMMA_NATIVE_TRIAL_REFUSED reason=RESOURCE_NOT_PLAUSIBLE_COM");g_gammaTrialRejected=true;return;
    }
    D3D11_TEXTURE2D_DESC cd{},md{},dd{};
    c->GetDesc(&cd);m->GetDesc(&md);d->GetDesc(&dd);
    const unsigned requestedW=g_gammaInputW;
    const unsigned requestedH=g_gammaInputH;
    const bool descriptors=cd.Width==requestedW && cd.Height==requestedH &&
        (!g_gammaQualityRequested || g_gammaQualityAllocated) &&
        md.Width==cd.Width && dd.Width==cd.Width &&
        md.Height==cd.Height && dd.Height==cd.Height &&
        cd.Format==DXGI_FORMAT_R8G8B8A8_UNORM &&
        md.Format==DXGI_FORMAT_R16G16B16A16_FLOAT &&
        dd.Format==DXGI_FORMAT_R24G8_TYPELESS &&
        cd.SampleDesc.Count==1 && md.SampleDesc.Count==1 && dd.SampleDesc.Count==1 &&
        (cd.BindFlags&D3D11_BIND_RENDER_TARGET) &&
        (dd.BindFlags&D3D11_BIND_SHADER_RESOURCE);
    Log(L"GAMMA_NATIVE_RESOURCE_CHECK color=%ux%u fmt=%u motion=%ux%u fmt=%u depth=%ux%u fmt=%u desc_ok=%d",
        cd.Width,cd.Height,unsigned(cd.Format),md.Width,md.Height,unsigned(md.Format),
        dd.Width,dd.Height,unsigned(dd.Format),descriptors?1:0);
    if(!descriptors) {
        Log(L"GAMMA_NATIVE_TRIAL_REFUSED reason=TEXTURE_DESCRIPTOR_MISMATCH no_mutation=1");
        g_gammaTrialRejected=true;return;
    }
    ID3D11Device* device=nullptr;
    c->GetDevice(&device);
    if(!device) {g_gammaTrialRejected=true;return;}
    ID3D11DeviceContext* ctx=nullptr;
    device->GetImmediateContext(&ctx);
    if(!ctx){device->Release();g_gammaTrialRejected=true;return;}
    // NGX is created on the first actual world-frame render thread. We do not
    // assume the conditional GAMMA renderer detour is ever invoked.
    g_renderTid=GetCurrentThreadId();
    g_autoEnable=true;
    g_requestedMode=g_gammaSelectedMode;
    g_handoffMode=0; // GAMMA has an independent draw-time SRV handoff. MP is unaffected.
    g_allowZeroJitterFallback=true;
    const bool rebuilding=g_state==3;
    g_gammaNativeTrialActive=true;
    if(!rebuilding)g_state=-1;
    bool ok=false;
    if(rebuilding)ok=GammaRecreateAfterRendererRebuild(device);
    else {
        ok=InitializeNgx(device,1920,1080);
        if(ok) {
            g_state=1;
            if(g_gammaQualityRequested) {
                // The same MP Quality NGX mode-plan contract and feature
                // recreation, but real GAMMA 1280x720 engine targets.
                ok=EnsureIsolatedContext() && RecreateFeature(
                    g_gammaSelectedMode,g_gammaInputW,g_gammaInputH);
                Log(L"GAMMA_SHARED_MODE_FEATURE success=%d mode=%ls input=%ux%u output=1920x1080",
                    ok?1:0,ModeName(g_gammaSelectedMode),g_gammaInputW,g_gammaInputH);
            }
        }
    }
    if(ok) {
        g_gammaLastAppliedPreset=g_preset;
        g_gammaPresetRecreatePending=false;
        // The multiplayer engine-hook evaluator remains disabled for GAMMA.
        // Both paths call the shared NGX evaluation core, with adapter-specific inputs.
        g_evaluationEnabled=false;
        const bool rtvAuditReady=GammaInstallRtvAudit(device);
        // v078: do NOT install v069's speculative D3D11 raster hooks.
        // v070 verified they see zero OM/VP/SC calls, even without filters.
        // Instead native resolution is restored at the exact engine composite.
        const bool drawHooksReady=GammaInstallDrawHooks(ctx);
        if(g_gammaBootstrapPending && !g_gammaQualityRequested && drawHooksReady) {
            const bool requestReady=GammaWriteBootstrapRequest();
            Log(L"GAMMA_BOOTSTRAP_NATIVE_NGX_READY marker=%d scene_native=1 "
                L"draw_hooks_installed=1 next=ONE_ENGINE_VID_RESTART",
                requestReady?1:0);
        }
        Log(L"GAMMA_VISIBLE_READY shared_ngx=1 feature_created=1 draw_hooks=%d verified_engine_composite=1 rtv_audit=%d eval=continuous mode=%ls input=%ux%u output=1920x1080 copyback=0 reinit=%d",
            drawHooksReady?1:0,rtvAuditReady?1:0,
            ModeName(g_gammaSelectedMode),requestedW,requestedH,rebuilding?1:0);
    } else {
        g_gammaTrialRejected=true;
        Log(L"GAMMA_NATIVE_TRIAL_REFUSED reason=NGX_INITIALIZATION_FAILED original_rendering_retained=1");
    }
    ctx->Release();device->Release();
}

// v078 engine-level final composite, derived from v070's identical
// stack frames and E8 call at 0xB3DA60. This function reads the global
// dimensions at entry, so restoring *here* is earlier than any D3D target bind.
static void __fastcall GammaVerifiedFinalCompositeHook(void* renderer) {
    const LONG64 calls=++g_gammaFinalCompositeCalls;
    bool restored=false;
    const unsigned beforeW=ReadOr<unsigned>(g_gammaImageBase+kGammaWidthRva);
    const unsigned beforeH=ReadOr<unsigned>(g_gammaImageBase+kGammaHeightRva);
    if(t_gammaWorldFrameInside && t_gammaScaledGlobalScope &&
       renderer==reinterpret_cast<void*>(g_gammaLatestCtorRenderer) &&
       g_gammaQualityAllocated && g_gammaQualityRequested &&
       beforeW==g_gammaInputW && beforeH==g_gammaInputH) {
        *reinterpret_cast<volatile unsigned*>(g_gammaImageBase+kGammaWidthRva)=1920;
        *reinterpret_cast<volatile unsigned*>(g_gammaImageBase+kGammaHeightRva)=1080;
        t_gammaScaledGlobalScope=false;
        t_gammaCompositeRestoredGlobals=true;
        restored=true;
        ++g_gammaNativeCompositeRestores;
    } else if(t_gammaWorldFrameInside && t_gammaScaledGlobalScope) {
        ++g_gammaUnexpectedDimensionRefusals;
        if(g_gammaUnexpectedDimensionRefusals<=8)
            Log(L"GAMMA_ENGINE_COMPOSITE_REFUSED frame=%u actual=%ux%u "
                L"expected=%ux%u match_renderer=%d count=%lld",
                ReadEngineFrame(),beforeW,beforeH,g_gammaInputW,g_gammaInputH,
                renderer==reinterpret_cast<void*>(g_gammaLatestCtorRenderer)?1:0,
                g_gammaUnexpectedDimensionRefusals);
    }
    if(calls<=8 || calls%1200==0 || (restored && g_gammaNativeCompositeRestores<=8))
        Log(L"GAMMA_ENGINE_COMPOSITE_ENTER frame=%u renderer=%p low_before=%ux%u "
            L"native=%ux%u stored_renderer_dims=%ux%u restored=%d calls=%lld restorations=%lld",
            ReadEngineFrame(),renderer,beforeW,beforeH,
            ReadOr<unsigned>(g_gammaImageBase+kGammaWidthRva),
            ReadOr<unsigned>(g_gammaImageBase+kGammaHeightRva),
            ReadOr<unsigned>(reinterpret_cast<uintptr_t>(renderer)+0x8),
            ReadOr<unsigned>(reinterpret_cast<uintptr_t>(renderer)+0xC),
            restored?1:0,calls,g_gammaNativeCompositeRestores);
    const bool priorCompositeScope=t_gammaVerifiedFinalCompositeInside;
    t_gammaVerifiedFinalCompositeInside=t_gammaWorldFrameInside &&
        renderer==reinterpret_cast<void*>(g_gammaLatestCtorRenderer);
    if(g_gammaOriginalFinalComposite)g_gammaOriginalFinalComposite(renderer);
    t_gammaVerifiedFinalCompositeInside=priorCompositeScope;
    // Read-only stage marker; never set RS viewport at the renderer boundary.
    if(t_gammaWorldFrameInside &&
       renderer==reinterpret_cast<void*>(g_gammaLatestCtorRenderer) &&
       g_gammaQualityAllocated && g_gammaQualityRequested)
        t_gammaUiAfterComposite=true;
    if(restored && (g_gammaNativeCompositeRestores<=8 || g_gammaNativeCompositeRestores%1200==0))
        Log(L"GAMMA_ENGINE_COMPOSITE_RETURN frame=%u original_returned=1 "
            L"size=%ux%u count=%lld",
            ReadEngineFrame(),
            ReadOr<unsigned>(g_gammaImageBase+kGammaWidthRva),
            ReadOr<unsigned>(g_gammaImageBase+kGammaHeightRva),
            g_gammaNativeCompositeRestores);
}

// GAMMA verified world frame. Only low-resolution scene targets use the
// temporary scene globals; native output and menu always keep 1920x1080.
static void __fastcall GammaWorldFrameAudit(void* scene) {
    if(!t_gammaWorldFrameInside) {
        const unsigned frame=ReadEngineFrame();
        t_gammaUiAfterComposite=g_gammaCompositeValidated && g_gammaQualityRequested &&
            g_gammaQualityAllocated && g_gammaLastCandidateFrame &&
            frame>g_gammaLastCandidateFrame+4; // pause/no scene composite
    }
    const LONG64 n=InterlockedIncrement64(&g_gammaFullFrameCalls);
    const auto renderer=g_gammaLatestCtorRenderer;
    const auto module=g_gammaImageBase;
    const bool sample=n<=8 || n==32 || n==120 || n%600==0;
    if(sample) {
        unsigned width=0,height=0,frame=0;
        uintptr_t level=0,hud=0;
        if(module) {
            SafeRead(module+kGammaWidthRva,&width,sizeof(width));
            SafeRead(module+kGammaHeightRva,&height,sizeof(height));
            SafeRead(module+kGammaCounterRva,&frame,sizeof(frame));
            SafeRead(module+kGammaLevelRva,&level,sizeof(level));
            SafeRead(module+kGammaHudRva,&hud,sizeof(hud));
        }
        Log(L"GAMMA_WORLD_FRAME_ENTER count=%lld frame=%u scene=%p ctor_renderer=%p level=%p hud=%p size=%ux%u renderer_calls=%lld quality_allocated=%d ngx_ready=%d",
            n,frame,scene,reinterpret_cast<void*>(renderer),
            reinterpret_cast<void*>(level),reinterpret_cast<void*>(hud),width,height,
            InterlockedCompareExchange64(&g_gammaRendererCalls,0,0),
            g_gammaQualityAllocated?1:0,g_state==1?1:0);
        // Resource identities are read from known GAMMA renderer slots. No
        // COM methods are called: even wrapper+0x20 is yet to be validated.
        if(renderer) {
            const uintptr_t slots[]={kGammaColorSlot,kGammaMotionSlot,kGammaDepthSlot,0x498,0x468};
            const wchar_t* names[]={L"color",L"motion",L"depth",L"ui",L"generic0"};
            for(unsigned i=0;i<5;++i) {
                uintptr_t wrapper=0,texture=0;
                const bool slotRead=SafeRead(renderer+slots[i],&wrapper,sizeof(wrapper));
                bool texRead=false;
                if(slotRead && wrapper) texRead=SafeRead(wrapper+0x20,&texture,sizeof(texture));
                Log(L"GAMMA_WORLD_RESOURCE frame_call=%lld name=%ls slot=0x%llX wrapper=%p assumed_texture=%p slot_read=%d tex_read=%d no_com_calls=1",
                    n,names[i],static_cast<unsigned long long>(slots[i]),
                    reinterpret_cast<void*>(wrapper),reinterpret_cast<void*>(texture),
                    slotRead?1:0,texRead?1:0);
            }
        }
    }
    const uintptr_t level=ReadOr<uintptr_t>(module+kGammaLevelRva);
    const uintptr_t hud=ReadOr<uintptr_t>(module+kGammaHudRva);
    const unsigned displayW=ReadOr<unsigned>(module+kGammaWidthRva);
    const unsigned displayH=ReadOr<unsigned>(module+kGammaHeightRva);
    GammaPollSharedMenu();
    GammaPollPauseCommand();
    // During a verified in-game pause menu, treat the engine's UI layout as
    // native from the START of the frame. The usual low-res scene targets are
    // left untouched until Lua's optional engine-owned vid_restart recreates
    // them in DLAA mode. No per-frame changes to NGX feature or live COM RTs.
    const bool nativePauseFrame=g_gammaPauseNativeEnabled && g_gammaPauseNativeRequested;
    if(nativePauseFrame) {
        ++g_gammaPauseNativeFrames;
        if(g_gammaPauseNativeFrames<=8 || g_gammaPauseNativeFrames%600==0)
            Log(L"GAMMA_PAUSE_NATIVE_GLOBALS frame=%u active=1 mode=%ls "
                L"rebuild=%d allocated_native=%d count=%lld",
                ReadEngineFrame(),ModeName(g_gammaSelectedMode),
                g_gammaPauseRestartEnabled?1:0,g_gammaQualityRequested?0:1,
                g_gammaPauseNativeFrames);
    }
    // The v053 multiplayer pipeline's *engine-level* contract, now driven by
    // real verified GAMMA code instead of D3D11 heuristics:
    //   1. Scene uses the genuinely smaller renderer-owned targets and globals.
    //   2. B9F1A0 restores native globals BEFORE computing final output state.
    //   3. World frame always restores globals if that call is skipped.
    // No mutation on main menu/no-level frames or during recursive frames.
    const bool previousFrameScope=t_gammaWorldFrameInside;
    const bool priorReduced=t_gammaScaledGlobalScope;
    const bool priorCompositeRestored=t_gammaCompositeRestoredGlobals;
    t_gammaWorldFrameInside=true;
    g_gammaRasterCurrentRole=0;
    g_gammaRasterCurrentW=g_gammaRasterCurrentH=0;
    bool reducedThisFrame=false;
    if(!previousFrameScope && g_gammaTrialEnabled && !g_gammaTrialRejected &&
       g_gammaQualityRequested && g_gammaQualityAllocated && level && hud &&
       !nativePauseFrame && displayW==1920 && displayH==1080 &&
       g_gammaLatestCtorRenderer && g_gammaInputW>0 && g_gammaInputH>0 &&
       g_gammaOriginalFinalComposite) {
        // Exact display dimension checks protect against renderer changes.
        *reinterpret_cast<volatile unsigned*>(module+kGammaWidthRva)=g_gammaInputW;
        *reinterpret_cast<volatile unsigned*>(module+kGammaHeightRva)=g_gammaInputH;
        reducedThisFrame=true;
        t_gammaScaledGlobalScope=true;
        t_gammaCompositeRestoredGlobals=false;
        const LONG64 total=++g_gammaFrameGlobalReduces;
        if(total<=8 || total%1200==0)
            Log(L"GAMMA_ENGINE_SCENE_LOW frame=%u input=%ux%u display=%ux%u count=%lld "
                L"level=1 hud=1 verified_final_hook=1",
                ReadEngineFrame(),g_gammaInputW,g_gammaInputH,displayW,displayH,total);
    } else if(g_gammaQualityRequested && g_gammaQualityAllocated && !nativePauseFrame && !previousFrameScope && level && hud) {
        ++g_gammaWorldUnscaledFrames;
        if(g_gammaWorldUnscaledFrames<=8 || g_gammaWorldUnscaledFrames%1200==0)
            Log(L"GAMMA_ENGINE_SCENE_LOW_SKIPPED frame=%u current=%ux%u "
                L"expected_native=1920x1080 active_renderer=%d final_hook=%d count=%lld",
                ReadEngineFrame(),displayW,displayH,
                g_gammaLatestCtorRenderer?1:0,g_gammaOriginalFinalComposite?1:0,
                g_gammaWorldUnscaledFrames);
    }
    if(g_gammaOriginalFullFrame)g_gammaOriginalFullFrame(scene);
    // Fallback for menu/aborted compositor paths; never leak reduced dimensions.
    if(reducedThisFrame) {
        const unsigned actualW=ReadOr<unsigned>(module+kGammaWidthRva);
        const unsigned actualH=ReadOr<unsigned>(module+kGammaHeightRva);
        if(actualW!=displayW || actualH!=displayH) {
            *reinterpret_cast<volatile unsigned*>(module+kGammaWidthRva)=displayW;
            *reinterpret_cast<volatile unsigned*>(module+kGammaHeightRva)=displayH;
            const LONG64 total=++g_gammaFrameFallbackRestores;
            if(total<=8 || total%1200==0)
                Log(L"GAMMA_ENGINE_FRAME_FALLBACK_NATIVE frame=%u from=%ux%u "
                    L"to=%ux%u count=%lld composite_seen=%d",
                    ReadEngineFrame(),actualW,actualH,displayW,displayH,total,
                    t_gammaCompositeRestoredGlobals?1:0);
        }
        t_gammaScaledGlobalScope=false;
    }
    t_gammaWorldFrameInside=previousFrameScope;
    t_gammaScaledGlobalScope=priorReduced;
    t_gammaCompositeRestoredGlobals=priorCompositeRestored;
    g_gammaRasterCurrentRole=0;
    g_gammaRasterCurrentW=g_gammaRasterCurrentH=0;
    // Initialization is intentionally outside the reduced scene scope.
    if(g_gammaTrialEnabled && !g_gammaTrialRejected &&
       (g_state==0 || g_state==3) && n>=8)
        GammaTryNativeTrialStartup();
    // Distinguish a pause/no-composite gap from a repeated 48-frame proof.
    // This diagnostic samples world frames rather than hot Draw calls.
    const unsigned currentFrame=ReadEngineFrame();
    if(g_gammaCompositeValidated && g_state==1 && level && hud &&
       currentFrame>g_gammaLastCandidateFrame+2 &&
       currentFrame!=g_gammaLastSkipFrame)
        GammaRecordSkip(currentFrame,5,L"NO_BACKBUFFER_COMPOSITE_THIS_FRAME");
    if(n%600==0 && g_gammaTrialEnabled)
        Log(L"GAMMA_VISIBLE_STATUS world_frames=%lld candidate_draws=%lld backbuffer_draws=%lld non_backbuffer=%lld usage_unknown=%lld qualified_frames=%u eval_attempts=%lld eval_fault=%d eval_success=%lld presented=%lld rejected=%d ngx_state=%d hook_ready=%d total_draws=%lld rtv_calls=%lld rtv_failures=%lld sticky=%d mode=%ls renderer_generations=%lld reinitializations=%lld",
            n,g_gammaCandidateDraws,g_gammaBackbufferDraws,g_gammaNonBackbufferDraws,g_gammaUnknownUsageDraws,
            g_gammaQualifiedFrames,g_gammaVisibleEvalAttempts,g_gammaVisibleFault?1:0,
            g_gammaVisibleEvalSuccesses,g_gammaVisiblePresented,g_gammaTrialRejected?1:0,
            g_state,g_gammaTrialHooksReady?1:0,g_gammaTrialDraws,
            g_gammaCreateRtvCalls,g_gammaCreateRtvFailures,
            g_gammaCompositeValidated?1:0,ModeName(g_gammaSelectedMode),
            g_gammaRendererGeneration,g_gammaReinitialized);
    if(n%600==0 && g_gammaTrialEnabled)
        Log(L"GAMMA_BOOTSTRAP_STATUS frame=%u native_first=%d pending=%d native_attempts=%lld "
            L"quality_activations=%lld renderer_generations=%lld mode=%ls menu=%ls",
            currentFrame,g_gammaBootstrapNativeFirst?1:0,g_gammaBootstrapPending?1:0,
            g_gammaBootstrapNativeAttempts,g_gammaBootstrapQualityActivations,
            g_gammaRendererGeneration,ModeName(g_gammaSelectedMode),ModeName(g_gammaMenuRequestedMode));
    if(n%600==0 && g_gammaTrialEnabled)
        Log(L"GAMMA_UI_DRAW_SCOPE_STATUS frame=%u mode=%ls ui_draws_scoped=%lld "
            L"native_targets_seen=%lld low_viewport_misses=%lld scissor_fixes=%lld "
            L"backbuffer_draws=%lld ngx_presented=%lld engine_composites=%lld",
            currentFrame,ModeName(g_gammaSelectedMode),
            g_gammaUiNativeDraws,g_gammaUiNativeTargets,
            g_gammaUiNativeSkipped,g_gammaUiNativeScissorFixes,
            g_gammaBackbufferDraws,g_gammaVisiblePresented,g_gammaFinalCompositeCalls);
    if(n%600==0 && g_gammaTrialEnabled)
        Log(L"GAMMA_BACKBUFFER_RASTER_STATUS frame=%u mode=%ls probed=%lld fixed=%lld "
            L"restored=%lld already_native=%lld unexpected_vp=%lld "
            L"engine_scope_qualified=%lld ngx_evaluations=%lld presented=%lld",
            ReadEngineFrame(),ModeName(g_gammaSelectedMode),
            g_gammaBackbufferDrawProbes,g_gammaDrawNativeViewportFixes,
            g_gammaDrawNativeViewportRestores,
            g_gammaBackbufferDrawViewportAlreadyNative,
            g_gammaBackbufferDrawOtherViewport,
            g_gammaVerifiedCompositeFastQualifications,
            g_gammaVisibleEvalSuccesses,g_gammaVisiblePresented);
    if(n%600==0 && g_gammaTrialEnabled)
        Log(L"GAMMA_ENGINE_SCOPE_STATUS frame=%u mode=%ls "
            L"low_frames=%lld native_composites=%lld fallback_restores=%lld "
            L"composite_calls=%lld refused=%lld unscaled=%lld current=%ux%u",
            currentFrame,ModeName(g_gammaSelectedMode),
            g_gammaFrameGlobalReduces,g_gammaNativeCompositeRestores,
            g_gammaFrameFallbackRestores,g_gammaFinalCompositeCalls,
            g_gammaUnexpectedDimensionRefusals,g_gammaWorldUnscaledFrames,
            ReadOr<unsigned>(module+kGammaWidthRva),ReadOr<unsigned>(module+kGammaHeightRva));
    if(n%600==0 && g_gammaTrialEnabled)
        Log(L"GAMMA_BOUNDARY_STATUS world_frames=%lld mode=%ls native_backbuffer_draws=%lld "
            L"draw6_raw=%lld raw_om=%lld raw_vp=%lld raw_sc=%lld stack_samples=%lld "
            L"render_thread=%lu world_thread=%lu raster_mutation=0 engine_globals_scoped=1",
            n,ModeName(g_gammaSelectedMode),g_gammaBackbufferDraws,
            InterlockedCompareExchange64(&g_gammaRawIndexedSixCalls,0,0),
            InterlockedCompareExchange64(&g_gammaRawOmCalls,0,0),
            InterlockedCompareExchange64(&g_gammaRawViewportCalls,0,0),
            InterlockedCompareExchange64(&g_gammaRawScissorCalls,0,0),
            g_gammaBoundarySamplesTotal,g_renderTid,GetCurrentThreadId());
    if(n%600==0 && g_gammaQualityRequested)
        Log(L"GAMMA_RASTER_ADAPTER_STATUS target_binds=%lld viewport_fixes=%lld scissor_fixes=%lld "
            L"viewport_calls=%lld scissor_calls=%lld unknown_binds=%lld "
            L"engine_globals_scoped=1 mode=%ls requested=%ls",
            g_gammaRasterOmBindings,g_gammaRasterViewportAdjusts,g_gammaRasterScissorAdjusts,
            g_gammaRasterViewportCalls,g_gammaRasterScissorCalls,g_gammaRasterUnknownBinds,
            ModeName(g_gammaSelectedMode),ModeName(g_gammaMenuRequestedMode));
    if(sample) Log(L"GAMMA_WORLD_FRAME_RETURN count=%lld scene=%p original_returned=1",n,scene);
}

static bool GammaInstallOne(uintptr_t base,uintptr_t rva,LPVOID detour,LPVOID* orig,const wchar_t* label) {
    void* target=reinterpret_cast<void*>(base+rva);
    MH_STATUS rc=MH_CreateHook(target,detour,orig);
    if (rc==MH_OK) {
        rc=MH_EnableHook(target);
        if (rc!=MH_OK) MH_RemoveHook(target);
    }
    Log(L"GAMMA_PASSIVE_HOOK name=%ls installed=%d status=%d rva=0x%llX",
        label,rc==MH_OK?1:0,int(rc),static_cast<unsigned long long>(rva));
    return rc==MH_OK;
}

static void GammaDisableAutoPauseFlag() {
    wchar_t dir[MAX_PATH]{};
    const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",dir,MAX_PATH);
    if (!n || n>=MAX_PATH) return;
    wchar_t filename[MAX_PATH+96]{};
    swprintf_s(filename,L"%ls\\anomaly_ngx\\pause_auto_enabled.flag",dir);
    if (DeleteFileW(filename)) Log(L"GAMMA_LUA_GATE stale_auto_pause_flag_removed=1");
}

static DWORD GammaPassiveStartup(std::uintptr_t base) {
    g_gammaImageBase=base;
    g_moduleBase=base;
    ConfigureSettingsPath();
    GammaPollSharedMenu(true);
    Log(L"SR_PROFILE_SELECTED name=GAMMA_V078 adapter=GAMMA shared_ngx_core=1 common_menu=1 ngx_eval=continuous detached_inputs=1 vid_restart=ENGINE_ONLY");
    GammaDisableAutoPauseFlag();
    const IMAGE_NT_HEADERS64* nt=nullptr;
    const IMAGE_SECTION_HEADER* sections=nullptr;
    if(!GammaGetPe(base,nt,sections)) {Log(L"GAMMA_PROFILE_REJECT reason=INVALID_PE");return 11;}
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;++i) {
        if (std::memcmp(sections[i].Name,".rdata",6)==0) {
            g_gammaRdataStart=base+sections[i].VirtualAddress;
            g_gammaRdataEnd=g_gammaRdataStart+sections[i].Misc.VirtualSize;
        }
    }
    bool valid=g_gammaRdataStart!=0;
    for(const auto& anchor:kGammaAobs) {
        uintptr_t actual=0;
        const unsigned hits=GammaScanText(base,anchor.signature,actual);
        const bool match=hits==1 && actual==anchor.expectedRva && GammaPdataStart(base,actual);
        Log(L"GAMMA_AOB role=%hs matches=%u expected=0x%llX discovered=0x%llX pdata_start=%d required=%d",
            anchor.label,hits,static_cast<unsigned long long>(anchor.expectedRva),
            static_cast<unsigned long long>(actual),match?1:0,anchor.required?1:0);
        if(anchor.required && !match) valid=false;
    }
    const bool callValid=GammaExactE8(base,kGammaRendererCallRva,kGammaRendererRva);
    Log(L"GAMMA_CALLGRAPH target=0x%X callsite=0x%X e8_verified=%d",unsigned(kGammaRendererRva),unsigned(kGammaRendererCallRva),callValid?1:0);
    const bool activeCall=GammaExactE8(base,kGammaActiveRendererCallRva,kGammaActiveRendererRva);
    const bool compositeCall=GammaExactE8(base,kGammaFinalCompositeCallRva,kGammaFinalCompositeRva);
    const bool activePdata=GammaPdataStart(base,kGammaActiveRendererRva);
    const bool compositePdata=GammaPdataStart(base,kGammaFinalCompositeRva);
    Log(L"GAMMA_VERIFIED_FINAL_BOUNDARY active=0x%X active_call=0x%X active_e8=%d "
        L"final=0x%X final_call=0x%X final_e8=%d pdata=%d/%d",
        unsigned(kGammaActiveRendererRva),unsigned(kGammaActiveRendererCallRva),activeCall?1:0,
        unsigned(kGammaFinalCompositeRva),unsigned(kGammaFinalCompositeCallRva),compositeCall?1:0,
        activePdata?1:0,compositePdata?1:0);
    valid=valid && callValid && activeCall && compositeCall && activePdata && compositePdata;
    if(!valid) {Log(L"GAMMA_PROFILE_REJECT reason=RUNTIME_VALIDATION_FAILED hooks=0");return 12;}
    // Only a SHA-256 and callsite-verified GAMMA profile creates the marker.
    // It includes the launcher token so a previous session cannot enable MP.
    const bool gammaMenuFlag=GammaWriteMenuDeferralMarker();
    Log(L"GAMMA_MENU_RESET_PROFILE verified=1 token_marker=%d",
        gammaMenuFlag?1:0);
    wchar_t cfg[MAX_PATH+96]{};
    swprintf_s(cfg,L"%ls\\anomaly_ngx_v078.ini",g_dllDir);
    const bool enabled=GetPrivateProfileIntW(L"GAMMA_PASSIVE_PROBE",L"enabled",1,cfg)!=0;
    const bool rendererEnabled=GetPrivateProfileIntW(L"GAMMA_PASSIVE_PROBE",L"renderer_hook",1,cfg)!=0;
    const bool worldFrameEnabled=GetPrivateProfileIntW(L"GAMMA_PASSIVE_PROBE",L"world_frame_hook",1,cfg)!=0;
    g_gammaPauseNativeEnabled=GetPrivateProfileIntW(L"GAMMA_PAUSE_NATIVE",L"enabled",1,cfg)!=0;
    g_gammaPauseRestartEnabled=GetPrivateProfileIntW(L"GAMMA_PAUSE_NATIVE",L"rebuild_on_pause",1,cfg)!=0;
    const bool gammaPauseFlag=g_gammaPauseNativeEnabled && GammaWritePauseFlag();
    Log(L"GAMMA_PAUSE_NATIVE_PROFILE enabled=%d rebuild_on_pause=%d token_gate=%d menu=1920x1080",
        g_gammaPauseNativeEnabled?1:0,g_gammaPauseRestartEnabled?1:0,gammaPauseFlag?1:0);
    g_gammaTrialEnabled=GetPrivateProfileIntW(L"GAMMA_NATIVE_DLAA_TRIAL",L"enabled",1,cfg)!=0;
    // v078 intentionally ignores the deprecated GAMMA-only mode INI.
    // Mode ownership is the already-shipped Video > Basic Lua UI's settings.ini.
    g_gammaSelectedMode=g_gammaMenuRequestedMode;
    g_gammaQualityRequested=(g_gammaSelectedMode!=0);
    g_gammaInputW=GammaModeWidth(g_gammaSelectedMode);
    g_gammaInputH=GammaModeHeight(g_gammaSelectedMode);
    Log(L"GAMMA_MODE_SOURCE source=SHARED_MENU mode=%ls input=%ux%u output=1920x1080 deprecated_gamma_ini_ignored=1",
        ModeName(g_gammaSelectedMode),g_gammaInputW,g_gammaInputH);
    const bool requestedCopyback=GetPrivateProfileIntW(L"GAMMA_NATIVE_DLAA_TRIAL",L"copyback",0,cfg)!=0;
    g_gammaTrialCopyback=false;  // native GAMMA never overwrites an engine texture
    if(requestedCopyback)
        Log(L"GAMMA_VISIBLE_POLICY override_legacy_copyback=1 forced_copyback=0");
    if(!worldFrameEnabled && g_gammaTrialEnabled) {
        Log(L"GAMMA_NATIVE_TRIAL_REFUSED reason=WORLD_FRAME_HOOK_DISABLED");
        g_gammaTrialEnabled=false;
    }
    Log(L"GAMMA_VISIBLE_CONFIG enabled=%d copyback=%d ngx_eval=continuous detached_inputs=1 handoff=draw_time_srv mode=%ls input_contract=SDK_MODE_PLAN output=1920x1080 jitter_hook=0 pause=native_on_verified_menu strict_descriptors=1 warmup_once=48 sticky_after=1 output_filter=DXGI_USAGE_BACK_BUFFER rebuild_rearm=1",
        g_gammaTrialEnabled?1:0,g_gammaTrialCopyback?1:0,ModeName(g_gammaSelectedMode));
    Log(L"GAMMA_PROBE_CONFIG enabled=%d renderer_hook=%d world_frame_hook=%d constructor_and_allocator=1 quality_scaling=%d pause=0",enabled?1:0,rendererEnabled?1:0,worldFrameEnabled?1:0,g_gammaQualityRequested?1:0);
    if(!enabled) {Log(L"GAMMA_PROBE_DISABLED no_hooks=1");return 0;}
    const MH_STATUS init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED) {
        Log(L"GAMMA_PROFILE_REJECT reason=MINHOOK_INIT status=%d",int(init));return 13;
    }
    if(!GammaInstallOne(base,kGammaAllocRva,reinterpret_cast<LPVOID>(&GammaNamedAllocAudit),
        reinterpret_cast<LPVOID*>(&g_gammaOriginalAlloc),L"named_allocator")) return 14;
    if(!GammaInstallOne(base,kGammaCtorRva,reinterpret_cast<LPVOID>(&GammaConstructorAudit),
        reinterpret_cast<LPVOID*>(&g_gammaOriginalCtor),L"constructor")) return 15;
    if(rendererEnabled) {
        if(!GammaInstallOne(base,kGammaRendererRva,reinterpret_cast<LPVOID>(&GammaRendererAudit),
            reinterpret_cast<LPVOID*>(&g_gammaOriginalRenderer),L"renderer")) return 16;
    }
    if(worldFrameEnabled && g_gammaTrialEnabled) {
        if(!GammaInstallOne(base,kGammaFinalCompositeRva,
            reinterpret_cast<LPVOID>(&GammaVerifiedFinalCompositeHook),
            reinterpret_cast<LPVOID*>(&g_gammaOriginalFinalComposite),L"verified_final_composite")) return 18;
    }
    if(worldFrameEnabled) {
        if(!GammaInstallOne(base,kGammaFrameRva,reinterpret_cast<LPVOID>(&GammaWorldFrameAudit),
            reinterpret_cast<LPVOID*>(&g_gammaOriginalFullFrame),L"world_frame")) return 17;
    }
    Log(L"GAMMA_PASSIVE_READY verified_aobs=1 caller=1 allocator=1 constructor=1 renderer=%d world_frame=%d gamma_quality_requested=%d native_dlss_trial=%d",
        rendererEnabled?1:0,worldFrameEnabled?1:0,g_gammaQualityRequested?1:0,g_gammaTrialEnabled?1:0);
    return 0;
}

static DWORD WINAPI Startup(LPVOID) {
    wchar_t temp[MAX_PATH]{};
    const DWORD used = GetTempPathW(MAX_PATH, temp);
    if (!used || used >= MAX_PATH) return 1;
    if (wcscpy_s(g_logPath, temp) != 0 ||
        wcscat_s(g_logPath, L"anomaly_ngx_bridge_v085.log") != 0 ||
        wcscpy_s(g_cacheDir, temp) != 0 ||
        wcscat_s(g_cacheDir, L"anomaly_ngx_v085_cache") != 0) return 2;
    CreateDirectoryW(g_cacheDir, nullptr);
    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(g_self, modulePath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(modulePath, L'\\');
        if (slash) *slash = L'\0';
        wcscpy_s(g_dllDir, modulePath);
    }
    // One profiling policy for MP and GAMMA. The GAMMA v078 renderer adapter
    // and its legacy v078.ini settings lookup remain untouched.
    wchar_t profileConfig[MAX_PATH + 96]{};
    swprintf_s(profileConfig,L"%ls\\anomaly_ngx_v085.ini",g_dllDir);
    g_releaseDiagnosticsEnabled=GetPrivateProfileIntW(
        L"RELEASE",L"diagnostics",0,profileConfig)!=0;
    Log(L"V085_RELEASE_BUILD profile=MP_OR_GAMMA profiler_compiled=0 "
        L"routine_logging=%d guarded_mp_reads=1",g_releaseDiagnosticsEnabled?1:0);
    int every=GetPrivateProfileIntW(L"PROFILER",L"sample_every",120,profileConfig);
    int report=GetPrivateProfileIntW(L"PROFILER",L"report_samples",16,profileConfig);
    g_profilerSampleEvery=static_cast<unsigned>(std::max(30,std::min(1200,every)));
    g_profilerReportSamples=static_cast<unsigned>(std::max(4,std::min(128,report)));
    // Deep profiler was a one-shot instrumented release, not a shipping feature.
    // Do not initialize GPU queries, high-frequency counters or logging hooks.
    g_v083GpuReportEvery = static_cast<unsigned>(GetPrivateProfileIntW(
        L"PROFILER",L"gpu_report_samples",8,profileConfig));
    if (g_v083GpuReportEvery<2) g_v083GpuReportEvery=2;
    if (g_v083GpuReportEvery>200) g_v083GpuReportEvery=200;
    Log(L"V083_PROFILER_SETUP cpu_gpu=1 nonblocking_queries=1 query_flags=DONOTFLUSH "
        L"sample_every=%u gpu_report_samples=%u gpu_query_slots=%u "
        L"gpu_scope=NGX_TO_FINAL_COMPOSITE_NOT_FULL_FRAME",
        g_profilerSampleEvery,g_v083GpuReportEvery,kV083GpuSlots);
    Log(L"PROFILER_CONFIG enabled=%d sample_every=%u report_samples=%u cpu_wall_time_only=0",
        g_profilerEnabled?1:0,g_profilerSampleEvery,g_profilerReportSamples);
    Log(L"ANOMALY_NGX_V085 PROFILE_SELECT known_MP=1 known_GAMMA=1 foreign=FAIL_CLOSED");
    wchar_t exeSha256[65]{};
    if (!SrComputeExeSha256(exeSha256)) {
        Log(L"SR_PROFILE_REJECT reason=EXE_SHA256_READ_FAILURE no_hooks=1");
        return 10;
    }
    const SrExeProfile profile=wcscmp(exeSha256,kKnownMultiplayerSha256)==0 ?
        SrExeProfile::MultiplayerV053 :
        (wcscmp(exeSha256,kKnownGammaSha256)==0 ? SrExeProfile::GammaPassiveV077 : SrExeProfile::Unsupported);
    Log(L"SR_PROFILE_DETECT sha256=%ls selected=%ls",exeSha256,
        profile==SrExeProfile::MultiplayerV053?L"MULTIPLAYER_V053":
        profile==SrExeProfile::GammaPassiveV077?L"GAMMA_VISIBLE_V078":L"UNSUPPORTED");
    if(profile==SrExeProfile::Unsupported) {
        Log(L"SR_PROFILE_REJECT reason=UNKNOWN_EXE_SHA256 no_hooks=1");
        return 11;
    }
    if(profile==SrExeProfile::GammaPassiveV077) {
        return GammaPassiveStartup(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)));
    }
    // v0.8.8: diagnostics opt-in only after the verified MULTIPLAYER SHA gate.
    wchar_t v088Ini[MAX_PATH+96]{};
    swprintf_s(v088Ini,L"%ls\\anomaly_ngx_v085.ini",g_dllDir);
    g_v088DiagnosticsEnabled=GetPrivateProfileIntW(
        L"STARTUP_DIAGNOSTICS",L"enabled",0,v088Ini)!=0;
    if (g_v088DiagnosticsEnabled) {
        g_v088IntervalMs=static_cast<unsigned>(std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"STARTUP_DIAGNOSTICS",L"interval_ms",1000,v088Ini)),500,5000));
        g_v088DurationSeconds=static_cast<unsigned>(std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"STARTUP_DIAGNOSTICS",L"duration_seconds",180,v088Ini)),15,300));
        g_v088SampleEvery=static_cast<unsigned>(std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"STARTUP_DIAGNOSTICS",L"sample_every_hooks",120,v088Ini)),30,1000));
        g_v089ThreadCensusEnabled=GetPrivateProfileIntW(
            L"STARTUP_DIAGNOSTICS",L"thread_census",0,v088Ini)!=0;
        g_v089ThreadCensusIntervalMs=static_cast<unsigned>(std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"STARTUP_DIAGNOSTICS",L"thread_census_interval_ms",2000,v088Ini)),1000,10000));
        g_v090VirtualQueryProbe=GetPrivateProfileIntW(
            L"STARTUP_DIAGNOSTICS",L"virtual_query_probe",0,v088Ini)!=0;
        swprintf_s(g_v088LogPath,L"%ls\\anomaly_ngx_diag_v088_pid%lu_tick%lu.log",
            temp,GetCurrentProcessId(),GetTickCount());
        Log(L"V088_START pid=%lu interval_ms=%u duration_seconds=%u sample_every=%u "
            L"diagnostics_only=1 owner_thread_unchanged=1 gpu_calls_on_render=1 "
            L"startup_thread_ms_since_dll_attach=%lu path=%ls",
            GetCurrentProcessId(),g_v088IntervalMs,g_v088DurationSeconds,
            g_v088SampleEvery,DWORD(GetTickCount()-g_v088DllAttachTick),g_v088LogPath);
        Log(L"V089_CONFIG thread_census=%d interval_ms=%u worker_only=1 no_thread_changes=1",
            g_v089ThreadCensusEnabled?1:0,g_v089ThreadCensusIntervalMs);
        Log(L"V090_CONFIG virtual_query_probe=%d sample_every=256 "
            L"original_engine_hook_sampled=1 no_page_cache=1 writable_protections_unchanged=1 "
            L"build_tag=V0901_LOG_ROUTING_HOTFIX",
            g_v090VirtualQueryProbe?1:0);
    }
    // Startup-only MP fallback knob. Invalid or missing values use fast mode 3.
    const int requestedValidationMode = static_cast<int>(GetPrivateProfileIntW(
        L"MP_SHADER_HOTPATH", L"validation_mode", 3, v088Ini));
    g_v091ValidationMode = requestedValidationMode >= 0 &&
        requestedValidationMode <= 3 ? requestedValidationMode : 3;
    if (g_v088DiagnosticsEnabled)
        Log(L"V091_CONFIG validation_mode=%d requested=%d "
            L"build_tag=V091_MP_SHADER_VALIDATION "
            L"no_cross_call_cache=1 writable_checks_before_writes=%d gamma_unchanged=1",
            g_v091ValidationMode,requestedValidationMode,g_v091ValidationMode==3?0:1);
    if (g_v088DiagnosticsEnabled)
        Log(L"V0911_CONFIG requested=%d effective=%d "
            L"build_tag=V0911_CHECK_REMOVAL_EXPERIMENT "
            L"seh_write_pairs=1 unsafe_page_guard_touch_possible=1 "
            L"fallback_on_first_fault=2 stable_modes_unchanged=1",
            requestedValidationMode,g_v091ValidationMode);
    Log(L"SR_PROFILE_SELECTED name=MULTIPLAYER_V085 adapter=MP native_menu_bootstrap=1 native_pause_lua=1 shared_ngx_unchanged=1");
    Log(L"ANOMALY_NGX_V085 MULTIPLAYER_LIFECYCLE bootstrap_via_pause_script=1 restore_after_resume=1 gamma_unchanged=1");
    wchar_t settingsPath[MAX_PATH + 96]{};
    if (swprintf_s(settingsPath, L"%ls\\anomaly_ngx_v085.ini", g_dllDir) > 0) {
        g_mpBackgroundMarkerRequested = GetPrivateProfileIntW(
            L"THREADING_EXPERIMENT", L"background_marker_poll", 1, settingsPath) != 0;
        g_mpBackgroundSettingsRequested = GetPrivateProfileIntW(
            L"THREADING_EXPERIMENT", L"background_settings_read", 1, settingsPath) != 0;
        // Values are bounded; malformed/missing entries keep original priority.
        // These are startup-only options and do not modify GAMMA or process class.
        const int workerMode = static_cast<int>(GetPrivateProfileIntW(
            L"THREADING_EXPERIMENT", L"worker_priority", 0, settingsPath));
        g_mpWorkerPriorityMode = workerMode >= 0 && workerMode <= 3 ? workerMode : 0;
        const int renderMode = static_cast<int>(GetPrivateProfileIntW(
            L"THREADING_EXPERIMENT", L"render_thread_priority", 0, settingsPath));
        g_mpRenderPriorityMode = renderMode >= 0 && renderMode <= 2 ? renderMode : 0;
        g_autoEnable = GetPrivateProfileIntW(L"DLAA", L"auto_enable", 1, settingsPath) != 0;
        g_srProbeEnabled = GetPrivateProfileIntW(L"SR_PROBE", L"enabled", 1, settingsPath) != 0;
        g_srAllocationAuditEnabled = GetPrivateProfileIntW(L"SR_ALLOCATION_AUDIT", L"enabled", 1, settingsPath) != 0;
        g_srScaleOptIn = GetPrivateProfileIntW(L"SR_ENGINE_SCALING", L"enabled", 1, settingsPath) != 0;
        g_srResetProbeEnabled = GetPrivateProfileIntW(L"SR_RESET_PROBE", L"enabled", 0, settingsPath) != 0;
        g_srReconfigureAtConstructor = GetPrivateProfileIntW(L"SR_LIVE_SWITCH", L"apply_on_renderer_reconstruction", 1, settingsPath) != 0;
        g_srLogConstructorStacks = GetPrivateProfileIntW(L"SR_LIVE_SWITCH", L"log_constructor_callers", 0, settingsPath) != 0;
        g_srNativeViewportFix = GetPrivateProfileIntW(L"SR_NATIVE_COMPOSITE", L"restore_viewport", 1, settingsPath) != 0;
        g_srNativeScissorFix = GetPrivateProfileIntW(L"SR_NATIVE_COMPOSITE", L"restore_scissor", 1, settingsPath) != 0;
        g_srNativeMenuOptIn = GetPrivateProfileIntW(L"SR_NATIVE_MENU", L"enabled", 1, settingsPath) != 0;
        g_srNativeGeneric01OptIn = GetPrivateProfileIntW(L"SR_UI_NATIVE_GENERIC01",L"enabled",0,settingsPath)!=0;
        g_srNativeGeneric0 = GetPrivateProfileIntW(L"SR_UI_NATIVE_GENERIC01",L"generic0",1,settingsPath)!=0;
        g_srNativeGeneric1 = GetPrivateProfileIntW(L"SR_UI_NATIVE_GENERIC01",L"generic1",1,settingsPath)!=0;
        g_uiPassTraceEnabled = GetPrivateProfileIntW(L"SR_UI_PASS_TRACE",L"enabled",0,settingsPath)!=0;
        g_srPauseUiDrawProbeEnabled=GetPrivateProfileIntW(L"SR_PAUSE_UI_DRAW_PROBE",L"enabled",0,settingsPath)!=0;
        g_srPauseSidecarEnabled=GetPrivateProfileIntW(L"SR_PAUSE_NATIVE_SIDECAR",L"enabled",0,settingsPath)!=0;
        g_srMenuHandoffBypassEnabled=GetPrivateProfileIntW(L"SR_MENU_HANDOFF_BYPASS",L"enabled",0,settingsPath)!=0;
        g_srMenuActivityDiagEnabled=GetPrivateProfileIntW(L"SR_MENU_ACTIVITY_DIAGNOSTICS",L"enabled",0,settingsPath)!=0;
        g_pauseTransitionDiagEnabled=GetPrivateProfileIntW(L"SR_PAUSE_TRANSITION_DIAGNOSTICS",L"enabled",0,settingsPath)!=0;
        // New multiplayer-only menu contract. Defaults ON even when an older
        // v078 INI with the retired opt-in pause section is carried forward.
        // GAMMA has already returned from Startup and cannot reach this code.
        g_mpNativeMenusEnabled=GetPrivateProfileIntW(L"SR_MP_NATIVE_MENUS",L"enabled",1,settingsPath)!=0;
        g_mpMainMenuNativeFirst=g_mpNativeMenusEnabled &&
            GetPrivateProfileIntW(L"SR_MP_NATIVE_MENUS",L"main_menu_native",1,settingsPath)!=0;
        g_pauseAutoEnabled=g_mpNativeMenusEnabled &&
            GetPrivateProfileIntW(L"SR_MP_NATIVE_MENUS",L"pause_menu_native",1,settingsPath)!=0;
        g_srUiNativeRasterEnabled=GetPrivateProfileIntW(L"SR_UI_NATIVE_RASTER",L"enabled",1,settingsPath)!=0;
        g_srUiRasterGeneric01=GetPrivateProfileIntW(L"SR_UI_NATIVE_RASTER",L"generic01",0,settingsPath)!=0;
        g_srUiRasterOutside=GetPrivateProfileIntW(L"SR_UI_NATIVE_RASTER",L"outside_world",1,settingsPath)!=0;
        g_srUiRasterInsideOther=GetPrivateProfileIntW(L"SR_UI_NATIVE_RASTER",L"inside_world_other",1,settingsPath)!=0;
        g_srUiRasterScissor=GetPrivateProfileIntW(L"SR_UI_NATIVE_RASTER",L"restore_scissor",1,settingsPath)!=0;
        g_srUiRasterNamedInsideOnly=GetPrivateProfileIntW(L"SR_UI_NATIVE_RASTER",L"inside_world_named_only",0,settingsPath)!=0;
        if (g_srScaleOptIn || g_srReconfigureAtConstructor) g_srAllocationAuditEnabled=true;
        g_srAllocationLogLimit = static_cast<unsigned>(GetPrivateProfileIntW(L"SR_ALLOCATION_AUDIT", L"max_named_entries", 144, settingsPath));
        if (g_srAllocationLogLimit > 512) g_srAllocationLogLimit = 512;
        g_handoffMode = GetPrivateProfileIntW(L"SR_HANDOFF", L"mode", 2, settingsPath);
        if (g_handoffMode < 0 || g_handoffMode > 2) g_handoffMode = 0;
        g_allowZeroJitterFallback = GetPrivateProfileIntW(L"DLAA", L"zero_jitter_fallback", 1, settingsPath) != 0;
        g_enableManualJitterDetour = GetPrivateProfileIntW(L"DLAA", L"audited_hook_fallback", 1, settingsPath) != 0;
        g_initialGuardMode = GetPrivateProfileIntW(L"DLAA", L"guard_mode", 0, settingsPath);
        g_initialGuardMode = 0; // disabled by user preference
    }
Log(L"SR_MP_NATIVE_MENUS_CONFIG enabled=%d main_menu_native=%d pause_menu_native=%d gameplay_mode_saved=1",
    g_mpNativeMenusEnabled?1:0,g_mpMainMenuNativeFirst?1:0,g_pauseAutoEnabled?1:0);
wchar_t localApp[MAX_PATH]{};
if (GetEnvironmentVariableW(L"LOCALAPPDATA",localApp,MAX_PATH)>0)
    swprintf_s(g_uiPassMarkerPath,L"%ls\\anomaly_ngx\\ui_trace.trigger",localApp);
if (localApp[0])
    swprintf_s(g_pauseDiagMarkerPath,L"%ls\\anomaly_ngx\\pause_diag.trigger",localApp);
if (localApp[0]) {
    swprintf_s(g_pauseAutoMarker,L"%ls\\anomaly_ngx\\pause_auto.trigger",localApp);
    swprintf_s(g_pauseAutoAck,L"%ls\\anomaly_ngx\\pause_auto.ack",localApp);
    swprintf_s(g_mpBootstrapAckPath,L"%ls\\anomaly_ngx\\mp_bootstrap_v080.ack",localApp);
    // Single-source-of-truth opt-in: synchronize the Lua gate with the
    // DLL-local INI automatically. The user never creates a flag by hand.
    wchar_t flagPath[MAX_PATH]{};
    wchar_t appDir[MAX_PATH]{};
    swprintf_s(appDir,L"%ls\\anomaly_ngx",localApp);
    swprintf_s(flagPath,L"%ls\\pause_auto_enabled.flag",appDir);
    if (g_pauseAutoEnabled) {
        CreateDirectoryW(appDir,nullptr); // existing directory is fine
        HANDLE flag=CreateFileW(flagPath,GENERIC_WRITE,FILE_SHARE_READ,nullptr,
                                CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (flag!=INVALID_HANDLE_VALUE) {
            const char on[]="1\n";
            DWORD n=0;
            const BOOL ok=WriteFile(flag,on,sizeof(on)-1,&n,nullptr);
            CloseHandle(flag);
            Log(L"SR_PAUSE_AUTO_LUA_GATE enabled=1 created=%d path=%ls",
                (ok && n==sizeof(on)-1)?1:0,flagPath);
        } else {
            Log(L"SR_PAUSE_AUTO_LUA_GATE enabled=1 created=0 error=%lu path=%ls",
                GetLastError(),flagPath);
        }
    } else {
        DeleteFileW(flagPath);
        Log(L"SR_PAUSE_AUTO_LUA_GATE enabled=0 stale_flag_removed=1");
    }
    // Do not accept stale IPC across launcher sessions.
    if (g_mpNativeMenusEnabled) {
        DeleteFileW(g_mpBootstrapAckPath);
        wchar_t legacy[MAX_PATH]{};
        swprintf_s(legacy,L"%ls\\anomaly_ngx\\mp_bootstrap_v079.request",localApp);
        DeleteFileW(legacy);
    }
    if (g_pauseAutoEnabled) {
        DeleteFileW(g_pauseAutoMarker);
        DeleteFileW(g_pauseAutoAck);
    }
}
Log(L"SR_PAUSE_AUTO_CONFIG enabled=%d lua_events=1 confirmed_d3d_candidate=1 temp_dlaa_override=1 saved_settings_unchanged=1 trigger=%ls",
    g_pauseAutoEnabled?1:0,g_pauseAutoMarker);
Log(L"SR_UI_NATIVE_GENERIC_CONFIG enabled=%d generic0=%d generic1=%d startup_and_reconstruction_only=1 no_pointer_swaps=1",
    g_srNativeGeneric01OptIn?1:0,g_srNativeGeneric0?1:0,g_srNativeGeneric1?1:0);
Log(L"SR_UI_NATIVE_RASTER_CONFIG enabled=%d generic01=%d outside_world=%d inside_world_other=%d inside_named_only=%d scissor=%d native_targets_only=1",
    g_srUiNativeRasterEnabled?1:0,g_srUiRasterGeneric01?1:0,g_srUiRasterOutside?1:0,
    g_srUiRasterInsideOther?1:0,g_srUiRasterNamedInsideOnly?1:0,g_srUiRasterScissor?1:0);
Log(L"SR_PAUSE_DIAG_CONFIG enabled=%d marker=%ls window_frames=120 "
    L"candidate_requires_renderer_absent_ui_text_composite=1 "
    L"observe_only=1 vid_restart=0 settings_unchanged=1",
    g_pauseTransitionDiagEnabled?1:0,g_pauseDiagMarkerPath);
Log(L"SR_MENU_ACTIVITY_CONFIG enabled=%d read_only=1 bounded_sprite_traces=16 "
    L"counts=fullframe_renderer_om_draw_indexed no_texture_resize=1",
    g_srMenuActivityDiagEnabled?1:0);
Log(L"SR_MENU_HANDOFF_CONFIG enabled=%d initially_armed=0 "
    L"auto_no_level=1 pause_detection=MANUAL_MARKER eval_skip=1 redirect_skip=1 "
    L"input_textures_unmodified=1",
    g_srMenuHandoffBypassEnabled?1:0);
Log(L"SR_PAUSE_NATIVE_SIDECAR_CONFIG enabled=%d initially_armed=0 "
    L"private_targets_only=1 no_engine_generic_resizes=1 manual_marker=1",
    g_srPauseSidecarEnabled?1:0);
Log(L"SR_PAUSE_UI_DRAW_CONFIG enabled=%d initially_armed=0 targets=generic0_generic1 "
    L"ui_callers=0x25B7F6_0x246161 no_rt_swaps=1 prior_state_restored_per_draw=1",
    g_srPauseUiDrawProbeEnabled?1:0);
Log(L"SR_UI_TRACE_CONFIG enabled=%d marker=%ls budget=120_om_120_rs_180_draw read_only=1",
    g_uiPassTraceEnabled?1:0,g_uiPassMarkerPath);
    Log(L"SR_RESET_PROBE_CONFIG enabled=%d passive=1 vid_restart_is_manual=1 no_target_bank=1",g_srResetProbeEnabled?1:0);
    Log(L"SR_LIVE_SWITCH_CONFIG adopt_on_reconstruction=%d log_constructor_callers=%d trigger=ENGINE_MANAGED_ONLY auto_vid_restart=0",
        g_srReconfigureAtConstructor?1:0,g_srLogConstructorStacks?1:0);
    Log(L"SR_NATIVE_COMPOSITE_CONFIG viewport=%d scissor=%d scaled_only=1 output=1920x1080",
        g_srNativeViewportFix?1:0,g_srNativeScissorFix?1:0);
    Log(L"SR_NATIVE_MENU_CONFIG enabled=%d level_gate_rva=0x%llX hud_gate_rva=0x%llX "
        L"no_reallocation=1 original_menu_path=1",
        g_srNativeMenuOptIn?1:0,
        static_cast<unsigned long long>(kActiveLevelRva),
        static_cast<unsigned long long>(kHudRenderRva));
    Log(L"SR_SCALING_CONFIG enabled=%d new_constructors_can_apply_mode=%d tested_display=1920x1080 experimental=1",
        g_srScaleOptIn?1:0,g_srReconfigureAtConstructor?1:0);
    Log(L"SR_PROBE_CONFIG enabled=%u engine_target_resize_experimental=%d visual_behavior_baseline_when_disabled=1",
        g_srProbeEnabled ? 1u : 0u,g_srScaleOptIn?1:0);
    Log(L"SR_HANDOFF_CONFIG mode=%d baseline_copy_default=%d direct_output_suitable_for_real_SR=1",
        g_handoffMode, g_handoffMode == 0 ? 1 : 0);
    ConfigureSettingsPath();
    ReloadSettingsOnRenderThread(true);
    swprintf_s(g_controlName, L"Local\\anomaly_ngx_v053_%lu", GetCurrentProcessId());
    g_activationEvent = CreateEventW(nullptr, TRUE, g_autoEnable ? TRUE : FALSE, g_controlName);
    wchar_t guardEventName[150]{};
    swprintf_s(guardEventName, L"Local\\anomaly_ngx_v053_%lu_guard_mild", GetCurrentProcessId());
    g_guardMildEvent = CreateEventW(nullptr, TRUE, g_initialGuardMode == 1 ? TRUE : FALSE, guardEventName);
    swprintf_s(guardEventName, L"Local\\anomaly_ngx_v053_%lu_guard_strong", GetCurrentProcessId());
    g_guardStrongEvent = CreateEventW(nullptr, TRUE, g_initialGuardMode == 2 ? TRUE : FALSE, guardEventName);
    const DWORD eventError = GetLastError();
    if (!g_activationEvent) Log(L"ERROR CONTROL_EVENT_CREATION_FAIL windows_error=%lu", eventError);
    if (eventError == ERROR_ALREADY_EXISTS) Log(L"WARNING CONTROL_EVENT_ALREADY_EXISTS pid=%lu", GetCurrentProcessId());
    Log(L"CONTROL event=%ls auto_enable=%d continuous=1 zero_jitter_fallback=%d audited_hook_fallback=%d created=%d",
        g_controlName, g_autoEnable ? 1 : 0, g_allowZeroJitterFallback ? 1 : 0,
        g_enableManualJitterDetour ? 1 : 0, g_activationEvent ? 1 : 0);
    Log(L"GUARD_CONTROL initial=%d mild_event=%d strong_event=%d", g_initialGuardMode,
        g_guardMildEvent ? 1 : 0, g_guardStrongEvent ? 1 : 0);
    Log(L"NGX_FEATURE_SEARCH_DIR %ls", g_dllDir);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    g_moduleBase = base;
    BYTE observed[sizeof(kExpectedCall)]{};
    if (!SafeRead(base + kCallsiteRva, observed, sizeof(observed)) ||
        std::memcmp(observed, kExpectedCall, sizeof(observed)) != 0) {
        Log(L"ERROR EXE_SIGNATURE_MISMATCH no_hook");
        return 3;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK) { Log(L"ERROR MH_Initialize=%d", init); return 4; }
    InstallEngineResetProbe(base); // read-only: console vid_restart is issued manually
    InstallFullFrameHook(base); // must be installed before any scaled renderer constructor
    InstallSRAllocationAudit(base); // opt-in selective resize, always logs original dimensions
    void* target = reinterpret_cast<void*>(base + kRendererRva);
    MH_STATUS rc = MH_CreateHook(target, reinterpret_cast<void*>(&RendererHook),
                                reinterpret_cast<void**>(&g_original));
    if (rc != MH_OK) { Log(L"ERROR MH_CreateHook=%d", rc); return 5; }
    rc = MH_EnableHook(target);
    if (rc != MH_OK) { Log(L"ERROR MH_EnableHook=%d", rc); return 6; }
    void* const decompressTarget = reinterpret_cast<void*>(base + kDecompressRva);
    g_jitterHookActive = InstallJitterHook(decompressTarget);
    if (!g_jitterHookActive) {
        if (g_allowZeroJitterFallback) {
            Log(L"JITTER_UNAVAILABLE_ZERO_FALLBACK DLAA_STILL_ON jitter=0,0");
        } else {
            Log(L"JITTER_UNAVAILABLE_STRICT_MODE DLAA_DISABLED_TO_AVOID_MISMATCH");
            if (g_activationEvent) ResetEvent(g_activationEvent);
        }
    }
    InstallSSFXJitterHook(reinterpret_cast<void*>(base + kSSFXJitterRva));
    Log(L"INSTALL_OK base=%p target=%p ssfx_hook=%d",
        reinterpret_cast<void*>(base), target, g_ssfxSyncHookInstalled ? 1 : 0);
    if (g_v088DiagnosticsEnabled)
        Log(L"V088_HOOKS jitter=%d manual_jitter_detour=%d ssfx=%d "
            L"auto_enable=%d requested_mode=%d requested_preset=%d handoff_mode=%d",
            g_jitterHookActive?1:0,g_manualJitterAlloc?1:0,g_ssfxSyncHookInstalled?1:0,
            g_autoEnable?1:0,g_requestedMode,g_preset,g_handoffMode);
    bool backgroundWorkerCreated=false;
    if (g_mpBackgroundMarkerRequested || g_mpBackgroundSettingsRequested) {
        HANDLE worker=CreateThread(nullptr, 0, MpBackgroundMarkerWorker,
                                   nullptr, 0, nullptr);
        if (worker) {
            backgroundWorkerCreated=true;
            const int desired = g_mpWorkerPriorityMode == 3 ? THREAD_PRIORITY_HIGHEST :
                g_mpWorkerPriorityMode == 2 ? THREAD_PRIORITY_ABOVE_NORMAL :
                g_mpWorkerPriorityMode == 1 ? THREAD_PRIORITY_NORMAL :
                                             THREAD_PRIORITY_BELOW_NORMAL;
            const int previous = GetThreadPriority(worker);
            const BOOL ok = SetThreadPriority(worker, desired);
            const DWORD error = ok ? 0 : GetLastError();
            Log(L"V087_WORKER_PRIORITY mode=%d previous=%d requested=%d actual=%d success=%d error=%lu",
                g_mpWorkerPriorityMode, previous, desired,
                GetThreadPriority(worker), ok ? 1 : 0, error);
            CloseHandle(worker);
        } else {
            Log(L"ERROR MP_BACKGROUND_MARKER_WORKER_CREATE_FAILED error=%lu original_polling_fallback=1",
                GetLastError());
        }
    }
    DWORD_PTR processMask=0, systemMask=0;
    const BOOL affinityOk=GetProcessAffinityMask(GetCurrentProcess(),&processMask,&systemMask);
    Log(L"V085_THREADING_STATUS requested=%d worker_created=%d background_only=1 "
        L"gpu_calls_on_render_thread=1 polling_fallback=1 "
        L"process_affinity=0x%llX system_affinity=0x%llX affinity_ok=%d",
        g_mpBackgroundMarkerRequested?1:0, backgroundWorkerCreated?1:0,
        static_cast<unsigned long long>(processMask),
        static_cast<unsigned long long>(systemMask),affinityOk?1:0);
    Log(L"V086_SETTINGS_WORKER requested=%d created=%d render_thread_apply=1 "
        L"filesystem_only=1 fallback_sync=1", g_mpBackgroundSettingsRequested?1:0,
        backgroundWorkerCreated?1:0);
    Log(L"V087_PRIORITY_CONFIG worker_mode=%d render_mode=%d "
        L"worker_created=%d no_affinity=1 no_process_priority_change=1",
        g_mpWorkerPriorityMode, g_mpRenderPriorityMode, backgroundWorkerCreated?1:0);
    if(g_v088DiagnosticsEnabled)
        Log(L"V088_STARTUP_STATUS worker_mode=%d render_mode=%d worker_created=%d "
            L"background_marker=%d background_settings=%d process_affinity=0x%llX",
            g_mpWorkerPriorityMode,g_mpRenderPriorityMode,backgroundWorkerCreated?1:0,
            g_mpBackgroundMarkerRequested?1:0,g_mpBackgroundSettingsRequested?1:0,
            static_cast<unsigned long long>(processMask));
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        g_v088DllAttachTick=GetTickCount();
        DisableThreadLibraryCalls(instance);
        HANDLE t = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    if (reason == DLL_PROCESS_DETACH)
        InterlockedExchange(&g_mpBackgroundMarkerStop, 1);
    // Do not FreeLibrary while the hook is active. Unload by exiting the game.
    return TRUE;
}
