// HIPC message framing: parsing an incoming request and writing the reply. Pure functions of the message
// bytes (no kernel state), shared by ipc.cpp and the unit tests. Moved text-for-text from ipc.cpp.
#pragma once
#include "ipc.h"

namespace ipc {

constexpr u32 kSfci = 0x49434653;  // 'SFCI'
constexpr u32 kSfco = 0x4F434653;  // 'SFCO'

struct Parsed {
    Request rq;
    bool domain_close = false;
    u32 domain_object = 0;
    u32 domain_cmd = 0;
};

void parse(const u8* msg, Parsed& p, bool is_domain);
void write_response(u8* msg, const Response& rs, bool domain, const std::vector<u32>& domain_objs,
                    const std::vector<Handle>& extra_moves);

}  // namespace ipc
