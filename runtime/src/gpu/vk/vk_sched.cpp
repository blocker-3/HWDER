// Command scheduler of the Vulkan renderer: frames in flight and the timeline semaphore, the waiter thread
// that runs deferred work, the staging ring, and the graveyard for resources the presenter may still read.
// Moved text-for-text from vk_renderer.cpp.
#include "vk_renderer.h"
#include "host/timing.h"
#include "host/platform.h"

namespace gpu::vk {

Sched g_s;
void (*g_before_submit)() = nullptr;

Recorder g_rec;

namespace gputime {
namespace {
constexpr u32 kMaxQueries = 2048;
struct Agg {
    double ns = 0;
    u64 segs = 0, draws = 0, clears = 0;
};
struct GroupKey {
    u32 kind, w, h, fmt, dfmt, ncol;
    u64 prog;
    bool operator==(const GroupKey& o) const { return !memcmp(this, &o, sizeof(*this)); }
};
struct GroupKeyHash {
    size_t operator()(const GroupKey& k) const { return (size_t)hash_bytes(&k, sizeof(k)); }
};
struct State {
    std::unordered_map<GroupKey, Agg, GroupKeyHash> groups;
    double busy_ns = 0, idle_ns = 0;
    u64 segs = 0, cbs = 0, last_end = 0, last_present = 0, last_log_ms = 0;
} g_t;

double period_s() {
    static const double p = debug::value(debug::GpuTiming, 0.0);
    return p;
}

void write(Frame* f) {
    vkCmdWriteTimestamp(f->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f->qpool, (u32)f->segs.size());
}

void frame_begin(Frame* f) {
    if (!on()) return;
    if (!f->qpool) {
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = kMaxQueries;
        VK_CHECK(vkCreateQueryPool(dev(), &ci, nullptr, &f->qpool));
    }
    vkCmdResetQueryPool(f->cb, f->qpool, 0, kMaxQueries);
    f->segs.clear();
    write(f);
    f->segs.push_back({});
}

void frame_end(Frame* f) {
    if (f->qpool && !f->segs.empty()) write(f);
}

// Waiter thread, after the frame completed: read its timestamps and add them up; log every period.
void frame_done(Frame* f) {
    if (!f->qpool || f->segs.empty()) return;
    const size_t n = f->segs.size();
    std::vector<u64> ts(n + 1);
    VkResult r = vkGetQueryPoolResults(dev(), f->qpool, 0, (u32)(n + 1), ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return;
    const double tick_ns = display::device().props.limits.timestampPeriod;
    if (g_t.last_end && ts[0] > g_t.last_end) g_t.idle_ns += (ts[0] - g_t.last_end) * tick_ns;
    g_t.last_end = ts[n];
    g_t.cbs++;
    for (size_t i = 0; i < n; i++) {
        const Seg& s = f->segs[i];
        const double ns = ts[i + 1] >= ts[i] ? (ts[i + 1] - ts[i]) * tick_ns : 0.0;
        g_t.busy_ns += ns;
        g_t.segs++;
        Agg& a = g_t.groups[GroupKey{s.kind, s.w, s.h, s.fmt, s.dfmt, s.ncol, s.prog}];
        a.ns += ns;
        a.segs++;
        a.draws += s.draws;
        a.clears += s.clears;
    }
    const u64 now = host::ticks_ms();
    if (!g_t.last_log_ms) {
        g_t.last_log_ms = now;
        g_t.last_present = g_s.present_count;
    }
    if (now - g_t.last_log_ms < (u64)(period_s() * 1000.0)) return;
    const u64 presents = g_s.present_count - g_t.last_present;
    if (presents) {
        const double pf = 1e6 * presents;  // ns -> ms per frame
        std::vector<std::pair<GroupKey, Agg>> v(g_t.groups.begin(), g_t.groups.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
        double passes_ns = 0, other_ns = 0;
        u64 passes = 0;
        for (const auto& [k, a] : v) {
            if (k.kind == kPass) passes_ns += a.ns, passes += a.segs;
            else other_ns += a.ns;
        }
        hw_log("gpu-time: per frame (%llu presents): busy %.2f ms, idle between submissions %.2f ms, %.1f submissions, "
               "%.1f passes %.2f ms, outside passes %.2f ms", (unsigned long long)presents, g_t.busy_ns / pf, g_t.idle_ns / pf,
               (double)g_t.cbs / presents, (double)passes / presents, passes_ns / pf, other_ns / pf);
        for (size_t i = 0; i < v.size() && i < 30; i++) {
            const auto& [k, a] = v[i];
            hw_log("gpu-time:   %6.3f ms %5.1f/frame %6.1f draws %4.1f clears | %s %ux%u f%u d%u n%u prog %016llx", a.ns / pf,
                   (double)a.segs / presents, (double)a.draws / presents, (double)a.clears / presents, k.kind == kPass ? "pass " : "other",
                   k.w, k.h, k.fmt, k.dfmt, k.ncol, (unsigned long long)k.prog);
        }
    }
    g_t = State{};
    g_t.last_log_ms = now;
    g_t.last_present = g_s.present_count;
}
}  // namespace

bool on() { return period_s() > 0; }

void mark(u32 kind, u32 w, u32 h, u32 fmt, u32 dfmt, u32 ncol) {
    if (!on() || !g_s.cur || !g_s.cur->qpool) return;
    Frame* f = g_s.cur;
    if (f->segs.size() + 2 >= kMaxQueries) return;
    write(f);
    Seg s;
    s.kind = kind, s.w = w, s.h = h, s.fmt = fmt, s.dfmt = dfmt, s.ncol = ncol;
    f->segs.push_back(s);
}

void count_draw(u64 prog) {
    if (!on() || !g_s.cur || g_s.cur->segs.empty()) return;
    Seg& s = g_s.cur->segs.back();
    if (!s.prog) s.prog = prog;
    s.draws++;
}

void count_clear() {
    if (!on() || !g_s.cur || g_s.cur->segs.empty()) return;
    g_s.cur->segs.back().clears++;
}
}  // namespace gputime

namespace {
// Command buffers in flight (one per flush; a big battle frame flushes several times).
constexpr size_t kMaxFramesInFlight = 64;
Frame* new_frame() {
    Frame* f = nullptr;
    {
        std::unique_lock<std::mutex> l(g_s.m);
        // bound the number of frames in flight
        while (g_s.free_frames.empty() && g_s.in_flight.size() >= kMaxFramesInFlight) g_s.cv.wait(l);
        if (!g_s.free_frames.empty()) {
            f = g_s.free_frames.back();
            g_s.free_frames.pop_back();
        }
    }
    if (!f) {
        f = new Frame;
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        ci.queueFamilyIndex = display::device().queue_family;
        VK_CHECK(vkCreateCommandPool(dev(), &ci, nullptr, &f->pool));
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f->pool;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev(), &ai, &f->cb));
    }  // a recycled frame's pools were reset by the waiter thread when it retired
    f->dpool_index = 0;
    f->serial = g_rec.serial_;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f->cb, &bi));
    gputime::frame_begin(f);
    return f;
}
}  // namespace

namespace {
std::atomic<u64> g_submits{0}, g_submit_ticks{0};

// vkQueueSubmit runs on its own thread, in order (~20 us of kernel time each, ~24 per busy battle frame). The GPU
// thread hands the frame over and moves on; the frame is already in_flight and its serial counts as submitted,
// which is safe with a timeline semaphore (waits may start before the signalling submit). HWDER_VK_SYNC_SUBMIT=1
// submits on the GPU thread as before.
struct SubmitQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Frame*> frames;
} g_sq;

void queue_submit(Frame* f) {
    u64 serial = f->serial;
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.signalSemaphoreValueCount = 1;
    ts.pSignalSemaphoreValues = &serial;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_s.timeline;
    SlowTimer timer("vkQueueSubmit", 30.0);
    std::lock_guard<std::mutex> q(display::device().queue_lock);
    host::Counter t0, t1;
    host::counter(&t0);
    VkResult r = vkQueueSubmit(display::device().queue, 1, &si, VK_NULL_HANDLE);
    host::counter(&t1);
    g_submits.fetch_add(1, std::memory_order_relaxed);
    g_submit_ticks.fetch_add((u64)(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    if (r != VK_SUCCESS) {
        hw_log("vk: vkQueueSubmit failed %d", (int)r);
        hw_fatal("vk: submit");
    }
}

void submit_thread() {
    host::name_thread(L"HWDER vk submit");
    for (;;) {
        Frame* f;
        {
            std::unique_lock<std::mutex> l(g_sq.m);
            g_sq.cv.wait(l, [] { return !g_sq.frames.empty(); });
            f = g_sq.frames.front();
            g_sq.frames.pop_front();
        }
        queue_submit(f);
    }
}

bool async_submit() {
    static const bool on = [] {
        if (debug::flag(debug::VkSyncSubmit)) return false;
        std::thread(submit_thread).detach();
        return true;
    }();
    return on;
}
}  // namespace
void submit_stats_reset(u64* submits, double* ms) {
    host::Counter f;
    host::counter_frequency(&f);
    *submits = g_submits.exchange(0);
    *ms = g_submit_ticks.exchange(0) * 1000.0 / f.QuadPart;
}

void wait_serial(u64 serial) {
    if (g_s.completed >= serial) return;
    SlowTimer timer("wait_serial", 30.0);
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g_s.timeline;
    wi.pValues = &serial;
    VkResult r = vkWaitSemaphores(dev(), &wi, 5000000000ull);
    if (r == VK_TIMEOUT) {
        hw_log("vk: GPU timeout waiting for serial %llu (device hang?)", (unsigned long long)serial);
        vkWaitSemaphores(dev(), &wi, UINT64_MAX);
    } else if (r != VK_SUCCESS) {
        hw_log("vk: vkWaitSemaphores failed %d (device lost?)", (int)r);
        hw_fatal("vk: device lost");
    }
}

void waiter_thread() {
    host::name_thread(L"HWDER vk waiter");
    for (;;) {
        Frame* f = nullptr;
        {
            std::unique_lock<std::mutex> l(g_s.m);
            g_s.cv.wait(l, [] { return !g_s.in_flight.empty(); });
            f = g_s.in_flight.front();
        }
        wait_serial(f->serial);
        gputime::frame_done(f);
        u64 v = 0;
        vkGetSemaphoreCounterValue(dev(), g_s.timeline, &v);
        for (auto& fn : f->deferred) fn();
        f->deferred.clear();
        // Reset the finished frame's pools here rather than on the GPU thread when it is reused (~0.4 ms per
        // submission in big battles). The frame belongs to this thread until it is back in free_frames.
        vkResetCommandPool(dev(), f->pool, 0);
        for (VkDescriptorPool p : f->dpools) vkResetDescriptorPool(dev(), p, 0);
        std::vector<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> l(g_s.m);
            g_s.in_flight.pop_front();
            g_s.free_frames.push_back(f);
            if (f->serial > g_s.completed) g_s.completed = f->serial;
            while (!g_s.idle.empty() && g_s.idle.front().first <= g_s.completed) {
                run.push_back(std::move(g_s.idle.front().second));
                g_s.idle.pop_front();
            }
        }
        g_s.cv.notify_all();
        for (auto& fn : run) fn();
    }
}

Recorder& rec() { return g_rec; }

VkCommandBuffer Recorder::cmd_in_pass() {
    if (!g_s.cur) {
        ProfScope ps(kProfFrameBegin);
        g_s.cur = new_frame();
        cb_ = g_s.cur->cb;
    }
    return cb_;
}

VkCommandBuffer Recorder::cmd() {
    VkCommandBuffer cb = cmd_in_pass();
    if (rendering) end_rendering();
    return cb;
}

void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

void Recorder::end_rendering() {
    if (!rendering) return;
    vkCmdEndRendering(cb_);
    rendering = false;
    gputime::mark(gputime::kOther);
    static const bool exp_nb = getenv("HWDER_EXP_NOBARRIER") != nullptr;  // EXPERIMENT
    if (!exp_nb) full_barrier(cb_);
}

void Recorder::barrier() {
    VkCommandBuffer cb = cmd();
    full_barrier(cb);
}

u64 Recorder::completed() const { return g_s.completed; }

void Recorder::defer(std::function<void()> fn) {
    cmd_in_pass();
    g_s.cur->deferred.push_back(std::move(fn));
}

Slice Recorder::stage(u64 size, u64 align) {
    cmd_in_pass();
    Slice s;
    if (size > kSegSize / 2) {
        Buffer* b = new Buffer(create_buffer(size, kRingUsage, true));
        s.buf = b->buf;
        s.ptr = b->map;
        s.offset = 0;
        g_s.cur->deferred.push_back([b] {
            destroy_buffer(*b);
            delete b;
        });
        return s;
    }
    u64 head = (g_s.ring_head + align - 1) & ~(align - 1);
    u32 seg = (u32)(g_s.ring_head / kSegSize);
    // A head exactly at the ring end reads as segment 16: that is a wrap to segment 0 too.
    if (seg >= kRingSegments || head + size > (u64)(seg + 1) * kSegSize) {
        seg = seg >= kRingSegments ? 0 : (seg + 1) % kRingSegments;
        head = (u64)seg * kSegSize;
        u64 last = g_s.seg_serial[seg];
        if (last >= serial_) {  // ring exhausted by the current command buffer: submit it first
            flush();
            cmd_in_pass();
        }
        SlowTimer timer("staging ring wrap wait", 30.0);
        wait_serial(last);
    }
    g_s.seg_serial[seg] = serial_;
    g_s.ring_head = head + size;
    s.buf = g_s.ring.buf;
    s.offset = head;
    s.ptr = g_s.ring.map + head;
    return s;
}

void Recorder::flush() {
    if (!g_s.cur) return;
    if (g_before_submit) g_before_submit();
    end_rendering();
    full_barrier(cb_);
    gputime::frame_end(g_s.cur);
    VK_CHECK(vkEndCommandBuffer(cb_));
    Frame* f = g_s.cur;
    g_s.cur = nullptr;
    cb_ = VK_NULL_HANDLE;
    u64 serial = serial_++;
    f->serial = serial;
    if (async_submit()) {
        {
            std::lock_guard<std::mutex> l(g_sq.m);
            g_sq.frames.push_back(f);
        }
        g_sq.cv.notify_one();
    } else {
        queue_submit(f);
    }
    {
        std::lock_guard<std::mutex> l(g_s.m);
        g_s.in_flight.push_back(f);
        g_s.submitted = serial;
    }
    g_s.cv.notify_all();
}

void Recorder::wait_idle() {
    flush();
    wait_serial(serial_ - 1);
}

// Run after the GPU finished the current work AND a few frames were presented (presenter safety).
static void retire(std::function<void()> fn) {
    g_s.graveyard.emplace_back(g_rec.serial(), g_s.present_count, std::move(fn));
}

void run_graveyard() {
    auto& g = g_s.graveyard;
    size_t w = 0;
    for (size_t i = 0; i < g.size(); i++) {
        auto& [serial, pc, fn] = g[i];
        if (serial <= g_s.completed && g_s.present_count >= pc + 3) {
            fn();
        } else {
            if (w != i) g[w] = std::move(g[i]);
            w++;
        }
    }
    g.resize(w);
}

// texture cache destruction goes through the graveyard (presenter may still read the image)
void retire_resource(std::function<void()> fn) { retire(std::move(fn)); }
}  // namespace gpu::vk
