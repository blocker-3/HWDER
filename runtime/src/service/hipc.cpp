// HIPC message framing: parsing an incoming request and writing the reply. Moved text-for-text from ipc.cpp.
#include <cstring>

#include "hipc.h"

namespace ipc {

Buffer Request::in_buffer(size_t i) const {
    if (i < a.size() && a[i].addr) return a[i];
    if (i < x.size()) return x[i];
    return {};
}
Buffer Request::out_buffer(size_t i) const {
    if (i < b.size() && b[i].addr) return b[i];
    if (i < c.size()) return c[i];
    return {};
}
std::string Request::in_string(size_t i) const {
    Buffer bf = in_buffer(i);
    if (!bf.addr) return {};
    const char* s = (const char*)bf.addr;
    return std::string(s, strnlen(s, (size_t)bf.size));
}

void parse(const u8* msg, Parsed& p, bool is_domain) {
    const u32* w = (const u32*)msg;
    u32 w0 = w[0], w1 = w[1];
    Request& rq = p.rq;
    rq.type = w0 & 0xFFFF;
    u32 nx = (w0 >> 16) & 0xF, na = (w0 >> 20) & 0xF, nb = (w0 >> 24) & 0xF, nw = (w0 >> 28) & 0xF;
    u32 raw_words = w1 & 0x3FF;
    u32 c_flags = (w1 >> 10) & 0xF;
    bool has_handles = (w1 >> 31) & 1;
    const u32* cur = w + 2;
    if (has_handles) {
        u32 hd = *cur++;
        rq.send_pid = hd & 1;
        u32 ncopy = (hd >> 1) & 0xF, nmove = (hd >> 5) & 0xF;
        if (rq.send_pid) {
            rq.pid = *(const u64*)cur;
            cur += 2;
        }
        for (u32 i = 0; i < ncopy; i++) rq.copy_handles.push_back(*cur++);
        for (u32 i = 0; i < nmove; i++) rq.move_handles.push_back(*cur++);
    }
    for (u32 i = 0; i < nx; i++) {
        u32 d0 = cur[0], d1 = cur[1];
        cur += 2;
        u64 addr = d1 | ((u64)((d0 >> 12) & 0xF) << 32) | ((u64)((d0 >> 6) & 0x7) << 36);
        rq.x.push_back({addr, d0 >> 16});
        rq.x_index.push_back((d0 & 0x3F) | ((d0 >> 3) & 0xE00));
    }
    auto abw = [&](std::vector<Buffer>& v, u32 n) {
        for (u32 i = 0; i < n; i++) {
            u32 s0 = cur[0], a0 = cur[1], d2 = cur[2];
            cur += 3;
            u64 size = s0 | ((u64)((d2 >> 24) & 0xF) << 32);
            u64 addr = a0 | ((u64)((d2 >> 28) & 0xF) << 32) | ((u64)((d2 >> 2) & 0x7) << 36);
            v.push_back({addr, size});
        }
    };
    abw(rq.a, na);
    abw(rq.b, nb);
    abw(rq.w, nw);
    const u8* raw = (const u8*)cur;
    const u8* raw_end = raw + raw_words * 4;
    // CMIF payload is 16-byte aligned relative to the message.
    size_t off = (size_t)(raw - msg);
    const u8* body = msg + ((off + 15) & ~(size_t)15);
    if (is_domain && (rq.type == 4 || rq.type == 6)) {
        u8 dcmd = body[0];
        u8 nobj = body[1];
        u16 len = *(const u16*)(body + 2);
        p.domain_cmd = dcmd;
        p.domain_object = *(const u32*)(body + 4);
        p.domain_close = dcmd == 2;
        body += 16;
        const u32* objs = (const u32*)(body + len);
        for (u32 i = 0; i < nobj; i++) rq.in_objects.push_back(objs[i]);
    }
    if (*(const u32*)body == kSfci || rq.type == 4 || rq.type == 5 || rq.type == 6 || rq.type == 7) {
        rq.command = *(const u32*)(body + 8);
        rq.args = body + 16;
        rq.args_size = (u32)(raw_end > rq.args ? raw_end - rq.args : 0);
    }
    // C descriptors follow the raw data.
    const u32* cd = (const u32*)raw_end;
    u32 nc = c_flags == 0 || c_flags == 1 ? 0 : (c_flags == 2 ? 1 : c_flags - 2);
    for (u32 i = 0; i < nc; i++) {
        u64 addr = cd[0] | ((u64)(cd[1] & 0xFFFF) << 32);
        rq.c.push_back({addr, cd[1] >> 16});
        cd += 2;
    }
}

void write_response(u8* msg, const Response& rs, bool domain, const std::vector<u32>& domain_objs,
                    const std::vector<Handle>& extra_moves) {
    std::vector<Handle> moves = rs.move_handles;
    moves.insert(moves.end(), extra_moves.begin(), extra_moves.end());
    bool has_handles = !rs.copy_handles.empty() || !moves.empty();
    u32* w = (u32*)msg;
    u32* cur = w + 2;
    if (has_handles) {
        *cur++ = ((u32)rs.copy_handles.size() << 1) | ((u32)moves.size() << 5);
        for (Handle h : rs.copy_handles) *cur++ = h;
        for (Handle h : moves) *cur++ = h;
    }
    size_t off = (size_t)((u8*)cur - msg);
    u8* body = msg + ((off + 15) & ~(size_t)15);
    u8* p = body;
    if (domain) {
        memset(p, 0, 16);
        *(u32*)p = (u32)domain_objs.size();
        p += 16;
    }
    u32* hdr = (u32*)p;
    hdr[0] = kSfco;
    hdr[1] = 0;
    hdr[2] = rs.result;
    hdr[3] = 0;
    p += 16;
    memcpy(p, rs.data.data(), rs.data.size());
    p += (rs.data.size() + 3) & ~(size_t)3;
    for (u32 id : domain_objs) {
        *(u32*)p = id;
        p += 4;
    }
    u32 raw_words = (u32)(((p - (u8*)cur) + 3) / 4) + 4;  // include alignment padding slack
    w[0] = 0;
    w[1] = (raw_words & 0x3FF) | (has_handles ? 0x80000000u : 0);
}

}  // namespace ipc
