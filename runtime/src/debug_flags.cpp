#include "debug_flags.h"

#include <cstdlib>
#include <cstring>

namespace debug {

static const Flag kTable[] = {
    {AsyncRtPipelines, "HWDER_ASYNC_RT_PIPELINES", "Renderer: also compile pipelines asynchronously for draws that sample a render target (comparison; they default to synchronous).", kPresence},
    {AsTrace, "HWDER_AS_TRACE", "nvdrv: log GPU address-space reservations, mappings and InitializeEx (as-gpu:). Read on every call.", kPresence},
    {Autoplay, "HWDER_AUTOPLAY", "Unattended runs: 1 taps A through the menus; 2 drives into a battle and keeps fighting.", kInt},
    {AutoplayCam, "HWDER_AUTOPLAY_CAM", "Autoplay: also turn the camera with the right stick (2 s right, 2 s left, 2 s still).", kPresence},
    {AutoplayCircle, "HWDER_AUTOPLAY_CIRCLE", "Autoplay: run in a slow circle (one turn every 8 s) so the view covers every direction.", kPresence},
    {AutoplayIdle, "HWDER_AUTOPLAY_IDLE", "Autoplay: no input once in battle (experiments).", kPresence},
    {AutoplaySkip, "HWDER_AUTOPLAY_SKIP", "Autoplay: the 6 s cutscene-skip cycle for a save that starts with the story cutscenes.", kPresence},
    {CacheProfile, "HWDER_CACHE_PROFILE", "Pipeline cache / warmup save: log retrieval, snapshot, validation, serialization and disk timings (cache-save:). Read on every save.", kPresence},
    {CacheSaveSync, "HWDER_CACHE_SAVE_SYNC", "Exactly \"1\": save the warmup records synchronously instead of on the background task.", kString},
    {Drawlog, "HWDER_DRAWLOG", "With HWDER_FILMSTRIP: one line per draw into <filmstrip dir>/drawlog.txt while the capture window is open.", kPresence},
    {DumpAfter, "HWDER_DUMP_AFTER", "HWDER_DUMP_FRAMES: no dumps (and no dump stalls) before that many seconds.", kFloat},
    {DumpEvery, "HWDER_DUMP_EVERY", "HWDER_DUMP_FRAMES: dump every n-th presented frame (each dump waits for the GPU).", kInt},
    {DumpFrames, "HWDER_DUMP_FRAMES", "Directory for frame_<n>.bmp dumps of presented frames.", kString},
    {DumpShaders, "HWDER_DUMP_SHADERS", "Write every translated shader's SPIR-V and Maxwell listing beside the executable.", kPresence},
    {FdRaw, "HWDER_FD_RAW", "Frame dump: also write raw vertex streams, index buffers and vertex-stage cbufs into fd/.", kPresence},
    {Filmstrip, "HWDER_FILMSTRIP", "Directory for a 96x54 thumbnail of every presented frame (f_<n>.raw + index.txt).", kString},
    {FilmstripAfter, "HWDER_FILMSTRIP_AFTER", "Filmstrip: start capturing at that many seconds since boot.", kFloat},
    {FilmstripBattle, "HWDER_FILMSTRIP_BATTLE", "Filmstrip: start that many seconds (atof) after the first battle-sized frame (> 450 draws).", kString},
    {FilmstripFor, "HWDER_FILMSTRIP_FOR", "Filmstrip: stop capturing after that many seconds.", kFloat},
    {FilmstripW, "HWDER_FILMSTRIP_W", "Filmstrip: thumbnail width (16:9, at least 16).", kInt},
    {ForceInterval1, "HWDER_FORCE_INTERVAL1", "Presenter: release every frame on the 60 Hz grid even when the game asks for swap interval 2 (experiment).", kPresence},
    {Fps, "HWDER_FPS", "Presenter release rate for experiments; ignored by the 120 fps mode's per-frame rates.", kFloat},
    {FsSelftest, "HWDER_FS_SELFTEST", "RomFS: run the image self test after mounting.", kNonzero},
    {FsTrace, "HWDER_FS_TRACE", "File system service trace.", kNonzero},
    {GameInterp, "HWDER_GAME_INTERP", "0/1: override the 120 fps (game-drawn in-between frame) setting for tests.", kInt},
    {GiLog, "HWDER_GI_LOG", "120 fps mode: log its timing and skip reasons periodically.", kPresence},
    {GiLogMs, "HWDER_GI_LOG_MS", "Period of the HWDER_GI_LOG reports in ms (strtoull, base 10).", kInt},
    {GiNoObjects, "HWDER_GI_NO_OBJECTS", "120 fps mode: interpolate the camera only, no object/bone blending (comparison).", kPresence},
    {GiObjTrace, "HWDER_GI_OBJ_TRACE", "120 fps mode: once a second, trace the c3 blocks nearest the player and the other vertex cbufs with transforms.", kPresence},
    {GpuTrace, "HWDER_GPU_TRACE", "GPU command trace; \"2\": raw 3D method stream (first 200000); \"3\": host methods, class binds and submissions.", kString},
    {Hold, "HWDER_HOLD", "On a fatal error keep the window up showing the reason instead of exiting.", kPresence},
    {IpcBt, "HWDER_IPC_BT", "Log a backtrace for the IPC command named \"<Service>#<command>\" (e.g. IFile#4).", kString},
    {IpcDump, "HWDER_IPC_DUMP", "Dump the raw request/response bytes of IPC calls whose target service name contains the value (empty: all).", kString},
    {IpcTrace, "HWDER_IPC_TRACE", "Service call (IPC) trace, including Control requests.", kPresence},
    {Keys, "HWDER_KEYS", "prod.keys path (the loader reads it as a wide string; the startup screen only notes that it is set).", kPresence},
    {Log, "HWDER_LOG", "Log file path (set by test launchers); when set the window opens minimized unless HWDER_SHOW is set.", kPresence},
    {Mute, "HWDER_MUTE", "No audio output; also: no message boxes, and with HWDER_OFFSCREEN marks an automated run.", kPresence},
    {NoPrelaunch, "HWDER_NO_PRELAUNCH", "Skip the startup screen.", kPresence},
    {Nsp, "HWDER_NSP", "Game NSP path (read as a wide string by the loader; the startup screen only notes that it is set).", kPresence},
    {NvTrace, "HWDER_NV_TRACE", "nvdrv ioctl trace.", kPresence},
    {OcclusionOne, "HWDER_OCCLUSION_ONE", "Report the occlusion probe query as 1 sample instead of fully visible.", kPresence},
    {Offscreen, "HWDER_OFFSCREEN", "Open the window off-screen and never activate it (automated runs).", kPresence},
    {Overlay, "HWDER_OVERLAY", "Show the F1 overlay at startup.", kPresence},
    {PaceTrace, "HWDER_PACE_TRACE", "Frame pacing trace: one line per present (display) and per queued buffer (vi).", kPresence},
    {PatchDt, "HWDER_PATCH_DT", "Loader experiment: patch the game's frame-delta constants to 1/<Hz> (>= 30).", kFloat},
    {PipelineCatalog, "HWDER_PIPELINE_CATALOG", "Use pipeline_catalog.bin at startup (\"0\" disables).", kOnUnlessZero},
    {PipelineCatalogRecord, "HWDER_PIPELINE_CATALOG_RECORD", "Record a metadata pipeline catalog for the named scope (at most 128 chars).", kString},
    {PipelinePrewarm, "HWDER_PIPELINE_PREWARM", "Early pipeline prebuilding from pipeline_warmup.bin (\"0\" disables, recording stays on).", kOnUnlessZero},
    {PrelaunchFrames, "HWDER_PRELAUNCH_FRAMES", "Leave the startup screen after that many frames (tests).", kInt},
    {PresentTrace, "HWDER_PRESENT_TRACE", "One line per present: address switches, stale re-presents, movie cadence (ptrace:).", kPresence},
    {ProfileMs, "HWDER_PROFILE_MS", "Log the draw-path profile when a flush accumulated more than that many ms.", kFloat},
    {ResScale, "HWDER_RES_SCALE", "Internal resolution scale override (0.5 to 4).", kFloat},
    {ResScaleX, "HWDER_RES_SCALE_X", "Per-axis resolution scale override, X (test aid: emulate another window aspect).", kFloat},
    {ResScaleY, "HWDER_RES_SCALE_Y", "Per-axis resolution scale override, Y.", kFloat},
    {ShaderFallback, "HWDER_SHADER_FALLBACK", "\"all\": replace every stage, \"frag\": only fragment shaders, with the solid-magenta fallback.", kString},
    {ShaderNoKil, "HWDER_SHADER_NO_KIL", "Shader translation: turn every (conditional) KIL into a no-op (alpha-test diagnosis).", kPresence},
    {Show, "HWDER_SHOW", "Show the window normally even in an automated (HWDER_LOG) run.", kPresence},
    {SyncPipelines, "HWDER_SYNC_PIPELINES", "Compile every pipeline synchronously (the F1 async-shaders option off sets this too).", kPresence},
    {SyncTrace, "HWDER_SYNC_TRACE", "nvdrv syncpoint wait / signal trace.", kPresence},
    {ThreadDump, "HWDER_THREAD_DUMP", "Dump all guest threads to the log every n seconds.", kInt},
    {Validation, "HWDER_VALIDATION", "Enable the Vulkan validation layer (needs the Vulkan SDK).", kPresence},
    {ViTrace, "HWDER_VI_TRACE", "Display service (vi) parcel trace.", kPresence},
    {VkDumpCbuf, "HWDER_VK_DUMP_CBUF", "Frame dump: full cbuf contents of the fragment shader whose key starts with this hex prefix.", kString},
    {VkDumpTex, "HWDER_VK_DUMP_TEX", "Frame dump: write the guest bytes of every sampled texture into fd/.", kPresence},
    {VkFaceRule, "HWDER_VK_FACE_RULE", "Front-face experiment: atoi == 1 inverts on flip_y only, any other value never inverts.", kString},
    {VkFrameDump, "HWDER_VK_FRAME_DUMP", "Log every draw/clear/blit of one frame: <present index>, \"auto[<s>]\" (first 3D frame, not before <s>), \"t<s>\" (time), \"p<hex>\" (program key prefix).", kString},
    {VkFrameDumpAfter, "HWDER_VK_FRAME_DUMP_AFTER", "Frame dump \"p\" trigger: ignore matches before that many seconds.", kFloat},
    {VkFrameDumpCount, "HWDER_VK_FRAME_DUMP_COUNT", "Frame dump: n consecutive presents from the trigger (strtoull, base 0, at least 1).", kInt},
    {VkFrameDumpDelay, "HWDER_VK_FRAME_DUMP_DELAY", "Frame dump auto trigger: arm the time trigger that many seconds after the draw-count trigger.", kFloat},
    {VkFrameDumpMinDraws, "HWDER_VK_FRAME_DUMP_MIN_DRAWS", "Frame dump auto trigger: \"min[-max]\" draw-count bounds of the previous frame (strtoull, base 0).", kString},
    {VkFrameDumpSkip, "HWDER_VK_FRAME_DUMP_SKIP", "Frame dump auto trigger: dump the n-th present after the trigger (strtoull, base 0).", kInt},
    {VkInitDepth, "HWDER_VK_INIT_DEPTH", "Initial clear value of newly created depth images.", kFloat},
    {VkNoCull, "HWDER_VK_NO_CULL", "Disable face culling.", kPresence},
    {VkVerbose, "HWDER_VK_VERBOSE", "Verbose renderer logging: the first draws, texture handles and clears.", kPresence},
    {VkWhiteTex, "HWDER_VK_WHITE_TEX", "Bind the white dummy texture instead of every sampled texture.", kPresence},
    {WaitTrace, "HWDER_WAIT_TRACE", "Kernel: log slow WaitSynchronization calls.", kPresence},
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == kCount, "table and Id enum out of step");

const Flag* table(size_t* count) {
    if (count) *count = kCount;
    return kTable;
}

const char* str(Id id) { return getenv(kTable[id].name); }
bool flag(Id id) { return str(id) != nullptr; }
bool nonzero(Id id) {
    const char* e = str(id);
    return e && *e && *e != '0';
}
bool on_unless_zero(Id id) {
    const char* e = str(id);
    return !e || strcmp(e, "0") != 0;
}
int value(Id id, int def) {
    const char* e = str(id);
    return e ? atoi(e) : def;
}
double value(Id id, double def) {
    const char* e = str(id);
    return e ? atof(e) : def;
}
unsigned long long value(Id id, unsigned long long def, int base) {
    const char* e = str(id);
    return e ? strtoull(e, nullptr, base) : def;
}

}  // namespace debug
