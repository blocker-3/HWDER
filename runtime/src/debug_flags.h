// Debug switches: every HWDER_* environment variable the runtime reads, in one table, with accessors that
// reproduce the parse rule of the sites they replaced. Call sites keep their `static const` caching, so each
// variable is still read once, at the same point as before. docs/code-structure/debug-flags.md is the inventory;
// tools/gen_debug_flags_md.py renders this table into README.md and the F1 Tools tab lists it with the values.
#pragma once
#include <cstddef>

namespace debug {

enum Kind { kPresence, kOnUnlessZero, kNonzero, kInt, kFloat, kString };

enum Id {
    AllCores,  // HWDER_ALL_CORES
    AsyncRtPipelines,  // HWDER_ASYNC_RT_PIPELINES
    AsTrace,  // HWDER_AS_TRACE
    Autoplay,  // HWDER_AUTOPLAY
    AutoplayCam,  // HWDER_AUTOPLAY_CAM
    AutoplayCircle,  // HWDER_AUTOPLAY_CIRCLE
    AutoplayIdle,  // HWDER_AUTOPLAY_IDLE
    AutoplayPlus,  // HWDER_AUTOPLAY_PLUS
    AutoplaySkip,  // HWDER_AUTOPLAY_SKIP
    CacheProfile,  // HWDER_CACHE_PROFILE
    CacheSaveSync,  // HWDER_CACHE_SAVE_SYNC
    Drawlog,  // HWDER_DRAWLOG
    DumpAfter,  // HWDER_DUMP_AFTER
    DumpEvery,  // HWDER_DUMP_EVERY
    DumpFrames,  // HWDER_DUMP_FRAMES
    DumpShaders,  // HWDER_DUMP_SHADERS
    FdRaw,  // HWDER_FD_RAW
    Filmstrip,  // HWDER_FILMSTRIP
    FilmstripAfter,  // HWDER_FILMSTRIP_AFTER
    FilmstripBattle,  // HWDER_FILMSTRIP_BATTLE
    FilmstripFor,  // HWDER_FILMSTRIP_FOR
    FilmstripW,  // HWDER_FILMSTRIP_W
    ForceInterval1,  // HWDER_FORCE_INTERVAL1
    Fps,  // HWDER_FPS
    FsSelftest,  // HWDER_FS_SELFTEST
    FsTrace,  // HWDER_FS_TRACE
    GameInterp,  // HWDER_GAME_INTERP
    GiLog,  // HWDER_GI_LOG
    GiLogMs,  // HWDER_GI_LOG_MS
    GiNoObjects,  // HWDER_GI_NO_OBJECTS
    GiObjSync,  // HWDER_GI_OBJ_SYNC
    GiObjTrace,  // HWDER_GI_OBJ_TRACE
    GpuTrace,  // HWDER_GPU_TRACE
    GuestLibc,  // HWDER_GUEST_LIBC
    Hold,  // HWDER_HOLD
    IpcBt,  // HWDER_IPC_BT
    IpcDump,  // HWDER_IPC_DUMP
    IpcTrace,  // HWDER_IPC_TRACE
    Keys,  // HWDER_KEYS
    Log,  // HWDER_LOG
    Mute,  // HWDER_MUTE
    NoPrelaunch,  // HWDER_NO_PRELAUNCH
    Nsp,  // HWDER_NSP
    NvTrace,  // HWDER_NV_TRACE
    OcclusionOne,  // HWDER_OCCLUSION_ONE
    Offscreen,  // HWDER_OFFSCREEN
    Overlay,  // HWDER_OVERLAY
    PaceTrace,  // HWDER_PACE_TRACE
    PatchDt,  // HWDER_PATCH_DT
    PipelineCatalog,  // HWDER_PIPELINE_CATALOG
    PipelineCatalogRecord,  // HWDER_PIPELINE_CATALOG_RECORD
    PipelinePrewarm,  // HWDER_PIPELINE_PREWARM
    PrelaunchFrames,  // HWDER_PRELAUNCH_FRAMES
    PresentTrace,  // HWDER_PRESENT_TRACE
    ProfileMs,  // HWDER_PROFILE_MS
    ResScale,  // HWDER_RES_SCALE
    ResScaleX,  // HWDER_RES_SCALE_X
    ResScaleY,  // HWDER_RES_SCALE_Y
    SampleUs,  // HWDER_SAMPLE_US
    ShaderFallback,  // HWDER_SHADER_FALLBACK
    ShaderNoKil,  // HWDER_SHADER_NO_KIL
    Show,  // HWDER_SHOW
    SyncPipelines,  // HWDER_SYNC_PIPELINES
    SyncTrace,  // HWDER_SYNC_TRACE
    ThreadDump,  // HWDER_THREAD_DUMP
    ThreadStats,  // HWDER_THREAD_STATS
    Timeline,  // HWDER_TIMELINE
    TotalMemMb,  // HWDER_TOTAL_MEM_MB
    Update,  // HWDER_UPDATE
    UsedMemMb,  // HWDER_USED_MEM_MB
    Validation,  // HWDER_VALIDATION
    ViTrace,  // HWDER_VI_TRACE
    VkDumpCbuf,  // HWDER_VK_DUMP_CBUF
    VkDumpTex,  // HWDER_VK_DUMP_TEX
    VkFaceRule,  // HWDER_VK_FACE_RULE
    VkFrameDump,  // HWDER_VK_FRAME_DUMP
    VkFrameDumpAfter,  // HWDER_VK_FRAME_DUMP_AFTER
    VkFrameDumpCount,  // HWDER_VK_FRAME_DUMP_COUNT
    VkFrameDumpDelay,  // HWDER_VK_FRAME_DUMP_DELAY
    VkFrameDumpMinDraws,  // HWDER_VK_FRAME_DUMP_MIN_DRAWS
    VkFrameDumpSkip,  // HWDER_VK_FRAME_DUMP_SKIP
    VkInitDepth,  // HWDER_VK_INIT_DEPTH
    VkNoCull,  // HWDER_VK_NO_CULL
    VkSyncSubmit,  // HWDER_VK_SYNC_SUBMIT
    VkVerbose,  // HWDER_VK_VERBOSE
    VkWhiteTex,  // HWDER_VK_WHITE_TEX
    WaitTrace,  // HWDER_WAIT_TRACE
    WatchPerSerial,  // HWDER_WATCH_PER_SERIAL
    kCount
};

struct Flag {
    Id id;
    const char* name;     // the environment variable
    const char* meaning;  // one line
    Kind kind;
};

// The whole table (kCount entries, in Id order), for the README / F1 menu generators.
const Flag* table(size_t* count);

// The variable's value, or nullptr when unset (a plain getenv; sites with their own parse rule use this).
const char* str(Id id);
// Set at all (any value, including "" and "0").
bool flag(Id id);
// Set, non-empty and not starting with '0' (HWDER_FS_TRACE, HWDER_FS_SELFTEST).
bool nonzero(Id id);
// On unless set to exactly "0" (HWDER_PIPELINE_CATALOG, HWDER_PIPELINE_PREWARM).
bool on_unless_zero(Id id);
// atoi / atof / strtoull of the value when set, else the default.
int value(Id id, int def);
double value(Id id, double def);
unsigned long long value(Id id, unsigned long long def, int base);

}  // namespace debug
