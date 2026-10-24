// Object blending of the 120 fps mode (HWDER_GAME_INTERP): each draw's vertex-stage object/bone transforms are blended
// halfway between the previous real frame's copy of the same draw and the game-drawn in-between frame, with the
// HWDER_GI_OBJ_TRACE diagnostics. Moved text-for-text from vk_renderer.cpp.
#include <cmath>
#if defined(__AVX__)
#include <immintrin.h>
#endif
#include "host/timing.h"
#include "host/platform.h"

#include "interp/camera.h"
#include "vk_renderer.h"
#include "debug_flags.h"

namespace gpu::vk::detail {

// Farthest an object or bone may move in one tick and still be blended (world units; a running
// character moves about 10 per tick). Larger moves are teleports, respawns or a different object.
constexpr float kObjMaxMove = 80.0f;

// A 3x4 world transform (rows: rotation/scale, translation in the 4th column) in world space.
static bool is_world_3x4(const float* m) {
    // Cheapest tests first (most constant data fails on the translation column).
    float t0 = std::fabs(m[3]), t1 = std::fabs(m[7]), t2 = std::fabs(m[11]);
    if (!(t0 >= 1.0f && t0 < 1e6f && t1 >= 1.0f && t1 < 1e6f && t2 >= 1.0f && t2 < 1e6f)) return false;
    for (int r = 0; r < 3; r++) {
        float n2 = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(n2 > 0.0025f && n2 < 100.0f)) return false;
    }
    return true;
}

// Whole-block object blending: each c3 block is blended whole or not at all, with limits a running/attacking skeleton passes
// (palette grid by orthogonality, pairing by block contents). The earlier per-bone matching (nearest-
// other-bone fallback, partial blocks) produced the jagged flashes at 120 fps.
// Whole-block limits (world units). kObjMove: one bone's move per tick. kObjRigid: how far a bone's move
// may differ from the block's first bone (scaled to half the block's extent, at most kObjMaxMove).
constexpr float kObjMove = 60.0f, kObjRigid = 12.0f;

// How far a 3x4's rotation rows are from orthogonal: the sum of their squared pairwise cosines.
static float obj_ortho_err(const float* m) {
    auto dot = [&](int r, int s) { return m[r * 4] * m[s * 4] + m[r * 4 + 1] * m[s * 4 + 1] + m[r * 4 + 2] * m[s * 4 + 2]; };
    const float n0 = dot(0, 0), n1 = dot(1, 1), n2 = dot(2, 2);
    const float d01 = dot(0, 1), d02 = dot(0, 2), d12 = dot(1, 2);
    return d01 * d01 / (n0 * n1) + d02 * d02 / (n0 * n2) + d12 * d12 / (n1 * n2);
}

static bool obj_first_pos(const float* m, u64 nf, float out[3]) {
    for (u64 i = 0; i + 12 <= nf; i += 4)
        if (is_world_3x4(m + i)) {
            out[0] = m[i + 3], out[1] = m[i + 7], out[2] = m[i + 11];
            return true;
        }
    return false;
}

// Sum of |x[i] - y[i]| over n floats. Stops early, with a partial sum >= limit, once the sum reaches `limit`
// (checked every 64 floats). Pairing compares whole bone palettes (up to 1024 floats) against dozens of candidates,
// the largest single cost of the 120 fps mode's in-between frames.
static float abs_diff_sum(const float* x, const float* y, u64 n, float limit) {
    float s = 0.0f;
    u64 i = 0;
#if defined(__AVX__)
    const __m256 sign = _mm256_set1_ps(-0.0f);
    for (; i + 64 <= n; i += 64) {
        __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
        for (u64 j = i; j < i + 64; j += 16) {
            a0 = _mm256_add_ps(a0, _mm256_andnot_ps(sign, _mm256_sub_ps(_mm256_loadu_ps(x + j), _mm256_loadu_ps(y + j))));
            a1 = _mm256_add_ps(a1, _mm256_andnot_ps(sign, _mm256_sub_ps(_mm256_loadu_ps(x + j + 8), _mm256_loadu_ps(y + j + 8))));
        }
        const __m256 a = _mm256_add_ps(a0, a1);
        __m128 h = _mm_add_ps(_mm256_castps256_ps128(a), _mm256_extractf128_ps(a, 1));
        h = _mm_add_ps(h, _mm_movehl_ps(h, h));
        h = _mm_add_ss(h, _mm_shuffle_ps(h, h, 1));
        s += _mm_cvtss_f32(h);
        if (s >= limit) return s;
    }
#endif
    for (; i < n; i++) {
        s += std::fabs(x[i] - y[i]);
        if ((i & 3) == 3 && s >= limit) return s;
    }
    return s;
}

static const char* const kObjOutcomes[] = {"no previous frame", "no transform",   "key not in previous frame",
                                           "unpaired",          "ambiguous",      "offsets",
                                           "move",              "rigid",          "rotation",
                                           "blended"};
constexpr float kObjNear = 40.0f;  // HWDER_GI_OBJ_TRACE: blocks this close to the player are listed (world units)
enum ObjOutcome : u8 { kOutNoPrev, kOutNoTransform, kOutNoKey, kOutUnpaired, kOutAmbiguous, kOutOffsets, kOutMove, kOutRigid, kOutRotation, kOutBlended };

void VkRenderer::obj_trace_log() {
    static u64 last = 0;
    const u64 now = host::ticks_ms();
    if (now - last < 1000) return;
    last = now;
    for (ObjTraceRec* r : {&obj_tr_skin_, &obj_tr_any_}) {
        if (!r->valid) continue;
        std::string offs, moves;
        char tmp[32];
        for (u32 n = 0; n < std::min(r->nm, 48u); n++) {
            snprintf(tmp, sizeof tmp, "%s%u", n ? "," : "", r->offs[n] * 4u);
            offs += tmp;
            if (r->moves[n] >= 0) {
                snprintf(tmp, sizeof tmp, "%s%.1f", moves.empty() ? "" : ",", r->moves[n]);
                moves += tmp;
            }
        }
        hw_log("gi: trace %s: %s; %u bytes, %u matrices at bytes [%s]; %.1f from the camera target (%.1f %.1f %.1f); "
               "previous frame has %u blocks of this key (occurrence %u), pair distance %.1f, second %.1f; failed at "
               "matrix %d; moves since tick n-1 [%s]",
               r == &obj_tr_skin_ ? "nearest bone palette" : "nearest block", kObjOutcomes[r->outcome], r->bytes, r->nm,
               offs.c_str(), r->d_at, r->rel[0], r->rel[1], r->rel[2], r->nprev, r->occ, r->pair_d, r->second_d,
               r->fail_n == ~0u ? -1 : (int)r->fail_n, moves.c_str());
        r->valid = false;
    }
    std::string nearby;
    char tmp[96];
    for (const ObjNear& n : obj_near_last_) {
        snprintf(tmp, sizeof tmp, "%s%04llx/%u B/%u m/%.1f: %s", nearby.empty() ? "" : "; ", (unsigned long long)(n.key & 0xFFFF), n.bytes, n.nm,
                 n.d, kObjOutcomes[n.outcome]);
        nearby += tmp;
    }
    hw_log("gi: trace c3 blocks within %.0f of the player last frame (%zu): %s", kObjNear, obj_near_last_.size(), nearby.c_str());
    std::string cb;
    for (u32 i = 0; i < 18; i++) {
        if (!obj_cb_[i].blocks) continue;
        snprintf(tmp, sizeof tmp, "%sc%u: %u blocks, %u with transforms, %u near the player", cb.empty() ? "" : "; ", i, obj_cb_[i].blocks,
                 obj_cb_[i].with_tf, obj_cb_[i].close);
        cb += tmp;
        obj_cb_[i] = CbufCensus{};
    }
    hw_log("gi: trace other vertex constant buffers in extra frames over the last second: %s", cb.empty() ? "none" : cb.c_str());
}

// HWDER_GI_OBJ_TRACE: vertex-stage constant buffers other than c3 in in-between frames - do any hold world
// transforms near the player (which the object blending would not blend)?
void VkRenderer::obj_cbuf_census(u32 index, const u8* data, u64 bytes) {
    if (index >= 18) return;
    CbufCensus& c = obj_cb_[index];
    c.blocks++;
    const float* m = (const float*)data;
    bool tf = false, close = false;
    for (u64 i = 0; i + 12 <= bytes / 4; i += 4) {
        if (!is_world_3x4(m + i)) continue;
        tf = true;
        const float dx = m[i + 3] - obj_link_[0], dy = m[i + 7] - obj_link_[1], dz = m[i + 11] - obj_link_[2];
        if (obj_link_ok_ && dx * dx + dy * dy + dz * dz < kObjNear * kObjNear) {
            close = true;
            break;
        }
    }
    c.with_tf += tf;
    c.close += close;
}

void VkRenderer::obj_frame_begin(u64 tag) {
    if (tag == obj_tag_) return;
    // A real frame ended: it becomes the reference for the next in-between frame.
    if (obj_tag_ && !(obj_tag_ & 1)) {
        std::swap(obj_prev_, obj_cur_);
        obj_prev_.tag = obj_tag_;
    }
    obj_cur_.reset();
    obj_used_.clear();
    obj_occ_.clear();
    obj_tag_ = tag;
    if (!(tag & 1)) obj_st_.ticks++;
    static const bool trace = debug::flag(debug::GiObjTrace);
    if (trace && (tag & 1)) {
        obj_near_last_.swap(obj_near_cur_);
        obj_near_cur_.clear();
        obj_link_ok_ = obj_link_next_d_ < INFINITY;
        if (obj_link_ok_) memcpy(obj_link_, obj_link_next_, 12);
        obj_link_next_d_ = INFINITY;
        obj_trace_log();
        interp::CamSnap cs[1];
        obj_cam_ = interp::snapshots(cs, 1) == 1;
        if (obj_cam_) memcpy(obj_at_, cs[0].at, 12), memcpy(obj_eye_, cs[0].eye, 12);
    }
    static u64 frames = 0;
    if ((tag & 1) && ++frames % 600 == 0) {
        hw_log("gi: stage c over 600 extra frames: %llu c3 blocks, %llu matched the previous frame, %llu blended "
               "(%llu matrices); left at tick n: %llu ambiguous, %llu offsets, %llu move, "
               "%llu rigid, %llu rotation; skinned (8+ matrices) %llu matched, %llu blended; cost %.2f ms per tick",
               (unsigned long long)obj_st_.draws, (unsigned long long)obj_st_.matched, (unsigned long long)obj_st_.blended,
               (unsigned long long)obj_st_.mats, (unsigned long long)obj_st_.ambiguous,
               (unsigned long long)obj_st_.offsets, (unsigned long long)obj_st_.move, (unsigned long long)obj_st_.rigid,
               (unsigned long long)obj_st_.rotation, (unsigned long long)obj_st_.skin_matched,
               (unsigned long long)obj_st_.skin_blended, obj_st_.ticks ? obj_st_.ms / obj_st_.ticks : 0.0);
        obj_st_ = ObjStats{};
    }
}

void VkRenderer::obj_transforms(u32 index, u8* data, u64 bytes) {
    if (cur_stage_ != 0 || index != 3 || bytes < 64 || bytes > 4096) return;  // world matrix / bone palette sizes
    struct Timer {
        double& acc;
        host::Counter t0;
        explicit Timer(double& a) : acc(a) { host::counter(&t0); }
        ~Timer() {
            static host::Counter f = [] { host::Counter x; host::counter_frequency(&x); return x; }();
            host::Counter t1;
            host::counter(&t1);
            acc += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        }
    } timer(obj_st_.ms);
    static const bool off = debug::flag(debug::GiNoObjects);
    if (off) return;
    u64 tag = gpu::exec_gi_tag();
    if (!tag) return;
    obj_frame_begin(tag);
    const u64 nf = bytes / 4;
    float pb[3];
    const bool has = obj_first_pos((const float*)data, nf, pb);
    if (!(tag & 1)) {  // real frame: remember this draw's transforms
        ProfScope rps(kProfObjReal);
        if (!has) return;  // nothing to blend in it
        ObjEntry e{(u32)obj_cur_.arena.size(), (u32)bytes, {pb[0], pb[1], pb[2]}, true};
        obj_cur_.arena.insert(obj_cur_.arena.end(), data, data + bytes);
        obj_cur_.idx[cur_draw_key_].push_back(e);
        return;
    }
    // In-between frame of tick n: blend against real frame n-1. Many objects share a model (the same
    // draw key), and their order changes between frames: pair each block with the previous frame's
    // block of the same key whose first world transform is nearest (each used once).
    obj_st_.draws++;
    ProfScope xps(kProfObjExtra);
    // HWDER_GI_OBJ_TRACE: the block's outcome, recorded on every return when it is the nearest so far.
    static const bool trace = debug::flag(debug::GiObjTrace);
    u16 mats[1024 / 12 + 1];  // offsets (floats) of the block's matrices; outlives tr
    u32 nm = 0;
    struct Trace {
        VkRenderer* r;
        bool on;
        const float* b;
        u64 bytes;
        u8 outcome = kOutNoPrev;
        const float* a = nullptr;
        const u16* mats = nullptr;
        u32 nm = 0, fail_n = ~0u, nprev = 0, occ = 0;
        float pair_d = -1, second_d = -1;
        ~Trace() {
            if (!on || !r->obj_cam_) return;
            float p[3];
            if (!obj_first_pos(b, bytes / 4, p)) return;
            const float rel[3] = {p[0] - r->obj_at_[0], p[1] - r->obj_at_[1], p[2] - r->obj_at_[2]};
            const float d = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
            if (bytes >= 1024 && d < r->obj_link_next_d_) r->obj_link_next_d_ = d, memcpy(r->obj_link_next_, p, 12);
            if (r->obj_link_ok_ && r->obj_near_cur_.size() < 32) {
                const float lx = p[0] - r->obj_link_[0], ly = p[1] - r->obj_link_[1], lz = p[2] - r->obj_link_[2];
                const float ld = std::sqrt(lx * lx + ly * ly + lz * lz);
                if (ld < kObjNear) r->obj_near_cur_.push_back({r->cur_draw_key_, (u32)bytes, mats ? nm : 0, outcome, ld});
            }
            for (ObjTraceRec* t : {&r->obj_tr_any_, bytes >= 1024 ? &r->obj_tr_skin_ : nullptr}) {
                if (!t || (t->valid && t->d_at <= d)) continue;
                t->valid = true;
                t->outcome = outcome;
                t->bytes = (u32)bytes, t->nm = mats ? nm : 0, t->fail_n = fail_n, t->nprev = nprev, t->occ = occ;
                t->d_at = d, t->pair_d = pair_d, t->second_d = second_d;
                memcpy(t->rel, rel, 12);
                for (u32 n = 0; n < std::min(t->nm, 48u); n++) {
                    t->offs[n] = mats[n];
                    const float* x = a ? a + mats[n] : nullptr;
                    const float* y = b + mats[n];
                    if (!x) {
                        t->moves[n] = -1;
                        continue;
                    }
                    const float dx = y[3] - x[3], dy = y[7] - x[7], dz = y[11] - x[11];
                    // A blended block holds the halfway transform by now: report the full tick's move.
                    t->moves[n] = std::sqrt(dx * dx + dy * dy + dz * dz) * (outcome == kOutBlended ? 2.0f : 1.0f);
                }
            }
        }
    } tr{this, trace, (const float*)data, bytes};
    if (!has) tr.outcome = kOutNoTransform;
    if (!has || obj_prev_.tag != ((tag >> 1) - 1) << 1) return;
    tr.outcome = kOutNoKey;
    auto it = obj_prev_.idx.find(cur_draw_key_);
    if (it == obj_prev_.idx.end() || it->second.empty()) return;
    tr.outcome = kOutUnpaired;
    tr.nprev = (u32)it->second.size();
    auto& used = obj_used_[cur_draw_key_];
    if (used.size() < it->second.size()) used.resize(it->second.size(), false);  // cleared per frame, capacity kept
    // Same draw order first (the common case), else the nearest unused block among nearby occurrences.
    const u32 occ = obj_occ_[cur_draw_key_]++;
    tr.occ = occ;
    const auto& prevs = it->second;
    auto dist = [&](size_t k) {
        const ObjEntry& e = prevs[k];
        return (e.pos[0] - pb[0]) * (e.pos[0] - pb[0]) + (e.pos[1] - pb[1]) * (e.pos[1] - pb[1]) + (e.pos[2] - pb[2]) * (e.pos[2] - pb[2]);
    };
    // Blend the whole block or leave it byte-identical to tick n: a skinned mesh with some bones blended and
    // others not (or blended toward another enemy's bones) stretches across the gap. The nearest first
    // transforms only shortlist candidates. The same model is often drawn twice from one spot with different
    // poses, so the pair is the shortlisted block whose whole contents are closest; still ambiguous when a
    // different block is nearly as close.
    size_t lo = occ > 32 ? occ - 32 : 0, hi = std::min(prevs.size(), (size_t)occ + 33);
    if (prevs.size() <= 96) lo = 0, hi = prevs.size();
    // At most 96 candidates (the whole list when it is that short, else occ-32..occ+32).
    u32 cand[96];
    float cand_d[96];
    u32 ncand = 0;
    float cd = INFINITY;
    for (size_t k = lo; k < hi; k++)
        if (!used[k] && prevs[k].size == bytes) {
            const float d = dist(k);
            cd = std::min(cd, d);
            cand[ncand] = (u32)k, cand_d[ncand++] = d;
        }
    if (cd > kObjMaxMove * kObjMaxMove) return;
    const float* bb = (const float*)data;
    const float lim2 = std::max(4.0f * cd, 16.0f);
    // Nearest first: the best two pairs are almost always among the nearest, and once two are known the content
    // sums of the others stop early (a crowd of one model puts dozens of whole bone palettes here). Equal
    // distances keep the draw order, so identical blocks still pair as before.
    u32 nshort = 0;
    for (u32 i = 0; i < ncand; i++) {
        const u32 ki = cand[i];
        const float di = cand_d[i];
        if (di > lim2) continue;
        u32 j = nshort++;  // insertion sort in place: j <= i, and cand[i] is saved above
        for (; j > 0 && cand_d[j - 1] > di; j--) cand[j] = cand[j - 1], cand_d[j] = cand_d[j - 1];
        cand[j] = ki, cand_d[j] = di;
    }
    int c1 = -1, c2 = -1;
    float l1 = INFINITY, l2 = INFINITY;
    for (u32 ci = 0; ci < nshort; ci++) {
        const size_t k = cand[ci];
        const float* x = (const float*)(obj_prev_.arena.data() + prevs[k].off);
        // The sum only grows: once it reaches the second-best so far this block cannot change the outcome (crowds of one
        // model put dozens of candidates here, each a whole bone palette).
        const float s = abs_diff_sum(x, bb, nf, l2);
        if (s >= l2) continue;
        if (s < l1) l2 = l1, c2 = c1, l1 = s, c1 = (int)k;
        else if (s < l2) l2 = s, c2 = (int)k;
    }
    tr.pair_d = std::sqrt(cd), tr.second_d = c2 >= 0 ? l2 / std::max(l1, 1e-6f) : -1.0f;  // second: content ratio
    const bool dup = c2 >= 0 && !memcmp(obj_prev_.arena.data() + prevs[c1].off, obj_prev_.arena.data() + prevs[c2].off, bytes);
    if (c2 >= 0 && l2 <= 1.25f * l1 && !dup) {
        obj_st_.ambiguous++;
        tr.outcome = kOutAmbiguous;
        return;
    }
    if (c1 < 0) return;
    const int best = c1;
    used[best] = true;
    obj_st_.matched++;
    const float* a = (const float*)(obj_prev_.arena.data() + prevs[best].off);
    float* b = (float*)data;
    tr.a = a;
    // The same offsets must hold transforms in both blocks; collect the matrices to blend. Pick the
    // palette's 48-byte grid first: inside a palette the windows straddling two matrices pass
    // is_world_3x4 too, and whichever window matched first set the grid (it flipped between frames).
    // Real rotation rows are orthogonal, straddling windows mix two bones' rows; camera matrices
    // (viewProj, before an object's world matrix) are not orthogonal either.
    bool same = true;
    u32 nrun;  // mats[nrun..] are extras past the palette run, dropped (not blended) when a check fails
    {
        u32 cnt[3] = {};
        float err[3] = {};
        for (u64 i = 0; i + 12 <= nf; i += 4) {
            if (!is_world_3x4(b + i)) continue;
            const u32 ph = (u32)(i / 4 % 3);
            cnt[ph]++;
            err[ph] += obj_ortho_err(b + i);
        }
        u32 maxc = std::max({cnt[0], cnt[1], cnt[2]}), ph = 0;
        float be = INFINITY;
        for (u32 p = 0; p < 3; p++)  // the most orthogonal grid among those holding (nearly) the most matrices
            if (cnt[p] && cnt[p] + 2 >= maxc && err[p] / cnt[p] < be) be = err[p] / cnt[p], ph = p;
        // The palette is the longest unbroken run of transforms on that grid; it must match the previous
        // frame. Other transforms on the grid past it are often leftovers in the ring (moving thousands of
        // units a tick, which no drawn bone does): they join the blend only when they look like part of
        // the same object in both frames, and are otherwise left alone instead of rejecting the block.
        u16 grid[1024 / 12 + 1];
        bool in_a[1024 / 12 + 1];
        u32 ng = 0, run0 = 0, runn = 0;
        for (u64 i = ph * 4; i + 12 <= nf; i += 12)
            if (is_world_3x4(b + i)) grid[ng] = (u16)i, in_a[ng] = is_world_3x4(a + i), ng++;
        for (u32 k = 0; k < ng;) {
            u32 e = k + 1;
            while (e < ng && grid[e] == grid[e - 1] + 12) e++;
            if (e - k > runn) run0 = k, runn = e - k;
            k = e;
        }
        for (u32 k = run0; k < run0 + runn; k++) {
            if (!in_a[k]) same = false;
            mats[nm++] = grid[k];
        }
        nrun = nm;
        for (u32 k = 0; k < ng; k++) {
            if (k >= run0 && k < run0 + runn) continue;
            const float* x = a + grid[k];
            const float* y = b + grid[k];
            const float dx = y[3] - x[3], dy = y[7] - x[7], dz = y[11] - x[11];
            if (in_a[k] && dx * dx + dy * dy + dz * dz <= kObjMove * kObjMove) mats[nm++] = grid[k];
        }
    }
    tr.mats = mats, tr.nm = nm;
    if (nm >= 8) obj_st_.skin_matched++;
    if (!same) {
        obj_st_.offsets++;
        tr.outcome = kOutOffsets;
        return;
    }
    if (nm == 0) return;
    const float m0[3] = {b[mats[0] + 3] - a[mats[0] + 3], b[mats[0] + 7] - a[mats[0] + 7], b[mats[0] + 11] - a[mats[0] + 11]};
    float rigid;
    {  // limbs and weapons of a large skeleton swing further from its first bone
        float ext2 = 0.0f;
        for (u32 n = 1; n < nm; n++) {
            const float* y = b + mats[n];
            const float dx = y[3] - b[mats[0] + 3], dy = y[7] - b[mats[0] + 7], dz = y[11] - b[mats[0] + 11];
            ext2 = std::max(ext2, dx * dx + dy * dy + dz * dz);
        }
        rigid = std::clamp(0.5f * std::sqrt(ext2), kObjRigid, kObjMaxMove);
    }
    for (u32 n = 0; n < nm; n++) {
        const float* x = a + mats[n];
        const float* y = b + mats[n];
        const float mv[3] = {y[3] - x[3], y[7] - x[7], y[11] - x[11]};
        if (n >= nrun) {  // an extra: keep it only if it passes every check, never reject the block for it
            const float r0 = mv[0] - m0[0], r1 = mv[1] - m0[1], r2 = mv[2] - m0[2];
            bool ok = mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2] <= kObjMove * kObjMove &&
                      r0 * r0 + r1 * r1 + r2 * r2 <= rigid * rigid;
            for (int r = 0; r < 3 && ok; r++) {
                const float* p = x + r * 4;
                const float* q = y + r * 4;
                const float dot = p[0] * q[0] + p[1] * q[1] + p[2] * q[2];
                ok = dot > 0.0f;
            }
            if (!ok) mats[n--] = mats[--nm];
            continue;
        }
        if (mv[0] * mv[0] + mv[1] * mv[1] + mv[2] * mv[2] > kObjMove * kObjMove) {
            obj_st_.move++;
            tr.outcome = kOutMove, tr.fail_n = n;
            return;
        }
        const float r0 = mv[0] - m0[0], r1 = mv[1] - m0[1], r2 = mv[2] - m0[2];
        if (r0 * r0 + r1 * r1 + r2 * r2 > rigid * rigid) {
            obj_st_.rigid++;
            tr.outcome = kOutRigid, tr.fail_n = n;
            return;
        }
        for (int r = 0; r < 3; r++) {  // each row turned by less than 90 degrees
            const float* p = x + r * 4;
            const float* q = y + r * 4;
            if (!(p[0] * q[0] + p[1] * q[1] + p[2] * q[2] > 0.0f)) {
                obj_st_.rotation++;
                tr.outcome = kOutRotation, tr.fail_n = n;
                return;
            }
        }
    }
    for (u32 n = 0; n < nm; n++) {
        const float* x = a + mats[n];
        float* y = b + mats[n];
        for (int k = 0; k < 12; k++) y[k] = x[k] + (y[k] - x[k]) * 0.5f;
    }
    obj_st_.mats += nm;
    obj_st_.blended++;
    if (nm >= 8) obj_st_.skin_blended++;
    tr.outcome = kOutBlended;
}

}  // namespace gpu::vk::detail
