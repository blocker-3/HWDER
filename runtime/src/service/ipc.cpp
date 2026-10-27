// HIPC/CMIF message handling. Layout reference: switchbrew "IPC_Marshalling".
#include "host/timing.h"

#include "ipc.h"
#include "hipc.h"

#include <cstring>
#include "interp/interp.h"
#include "debug_flags.h"

extern "C" void hw_log_backtrace(const char* tag);

namespace ipc {

using namespace kern;

// ------------------------------------------------------------------ sessions
// Server-side domain state. Clones of a domain session share it (same object ids).
struct Domain {
    bool active = false;
    std::map<u32, ServicePtr> objects;  // object id -> service
    u32 next_object = 1;
    u32 add(ServicePtr s) {
        u32 id = next_object++;
        objects[id] = std::move(s);
        return id;
    }
};

struct Session : Object {
    const char* type_name() const override { return "Session"; }
    ServicePtr service;
    std::shared_ptr<Domain> dom = std::make_shared<Domain>();
};

Handle new_session(ServicePtr s) {
    auto sess = std::make_shared<Session>();
    sess->service = std::move(s);
    return g_handles.add(sess);
}
static Handle clone_session(const Session& from) {
    auto sess = std::make_shared<Session>();
    sess->service = from.service;
    sess->dom = from.dom;
    return g_handles.add(sess);
}

// ------------------------------------------------------------------ services
static std::map<std::string, std::function<ServicePtr()>>& registry() {
    static std::map<std::string, std::function<ServicePtr()>> r;
    return r;
}
void register_service(const std::string& name, std::function<ServicePtr()> factory) { registry()[name] = std::move(factory); }
ServicePtr open_service(const std::string& name) {
    auto it = registry().find(name);
    if (it != registry().end()) return it->second();
    hw_log("ipc: service '%s' not implemented - using stub", name.c_str());
    return std::make_shared<StubService>(name);
}

void Service::dispatch(Request& rq, Response& rs) {
    auto it = commands_.find(rq.command);
    if (it == commands_.end()) {
        hw_log("ipc: %s command %u not implemented (stubbed)", name_.c_str(), rq.command);
        return;
    }
    it->second(rq, rs);
}

void StubService::dispatch(Request& rq, Response& rs) {
    hw_log("ipc: [stub %s] command %u (args %u bytes, A%zu B%zu X%zu C%zu)", name_.c_str(), rq.command, rq.args_size,
           rq.a.size(), rq.b.size(), rq.x.size(), rq.c.size());
}

// ------------------------------------------------------------------ entry points
Result connect_to_named_port(const char* name, Handle* out) {
    std::string n(name, strnlen(name, 12));
    if (n != "sm:") hw_log("ipc: ConnectToNamedPort(%s)", n.c_str());
    *out = new_session(open_service(n));
    return ResultSuccess;
}

static void dump_msg(const char* tag, const u8* msg) {
    char line[0x100 * 3 + 1];
    int n = 0;
    for (int i = 0; i < 0x80; i++) n += snprintf(line + n, sizeof(line) - n, "%02x%s", msg[i], (i & 3) == 3 ? " " : "");
    hw_log("ipc %s: %s", tag, line);
}

Result send_sync_request(Handle h, u8* msg) {
    static const char* dump_filter = debug::str(debug::IpcDump);
    auto sess = g_handles.get_as<Session>(h);
    if (!sess) {
        hw_log("ipc: SendSyncRequest on invalid handle 0x%x", h);
        return ResultInvalidHandle;
    }
    Parsed p;
    parse(msg, p, sess->dom->active);
    Request& rq = p.rq;
    Response rs;
    std::vector<u32> domain_out;
    std::vector<Handle> extra_moves;

    if (rq.type == 2) {  // Close
        g_handles.close(h);
        return ResultSuccess;
    }
    if (rq.type == 5 || rq.type == 7) {  // Control
        static const bool trace = debug::flag(debug::IpcTrace);
        if (trace) hw_log("ipc: control #%u on handle 0x%x (%s, domain=%d)", rq.command, h, sess->service->name().c_str(), sess->dom->active);
        switch (rq.command) {
        case 0:  // ConvertCurrentObjectToDomain
            sess->dom->active = true;
            rs.push<u32>(sess->dom->add(sess->service));
            break;
        case 1: {  // CopyFromCurrentDomain(object id) -> move handle
            u32 id = rq.pop<u32>();
            auto it = sess->dom->objects.find(id);
            if (it == sess->dom->objects.end()) rs.result = ResultInvalidHandle;
            else extra_moves.push_back(new_session(it->second));
            break;
        }
        case 2: case 4:  // CloneCurrentObject(Ex): the clone shares the domain
            extra_moves.push_back(clone_session(*sess));
            break;
        case 3:  // QueryPointerBufferSize
            rs.push<u16>(0x8000);
            break;
        default:
            hw_log("ipc: control command %u", rq.command);
        }
        write_response(msg, rs, false, {}, extra_moves);
        return ResultSuccess;
    }
    ServicePtr target = sess->service;
    if (sess->dom->active) {
        auto it = sess->dom->objects.find(p.domain_object);
        if (it == sess->dom->objects.end()) {
            hw_log("ipc: domain object %u not found", p.domain_object);
            rs.result = ResultInvalidHandle;
            write_response(msg, rs, true, {}, {});
            return ResultSuccess;
        }
        if (p.domain_close) {
            sess->dom->objects.erase(it);
            write_response(msg, rs, true, {}, {});
            return ResultSuccess;
        }
        target = it->second;
        for (u32 id : rq.in_objects) {
            auto ob = sess->dom->objects.find(id);
            rq.in_services.push_back(ob == sess->dom->objects.end() ? nullptr : ob->second);
        }
    } else {
        for (Handle mh : rq.move_handles) {
            auto in = g_handles.get_as<Session>(mh);
            rq.in_services.push_back(in ? in->service : nullptr);
        }
    }
    bool dump = dump_filter && target->name().find(dump_filter) != std::string::npos;
    if (dump) dump_msg("req ", msg);
    {
        // A service call that takes long blocks the guest thread that issued it: report it.
        host::Counter d0, d1, df;
        host::counter(&d0);
        hw_gi_note_ipc(target->name().c_str(), rq.command);
        target->dispatch(rq, rs);
        host::counter(&d1);
        host::counter_frequency(&df);
        double ms = (d1.QuadPart - d0.QuadPart) * 1000.0 / df.QuadPart;
        if (ms > 10.0) hw_log("ipc: slow: %s command %u took %.0f ms", target->name().c_str(), rq.command, ms);
    }
    static const bool trace = debug::flag(debug::IpcTrace);
    if (trace) {
        char hex[3 * 16 + 1] = {};
        for (size_t i = 0; i < rs.data.size() && i < 16; i++) snprintf(hex + i * 3, 4, " %02x", rs.data[i]);
        hw_log("ipc: %s #%u -> 0x%x (%zu bytes, %zu objs, %zu handles)%s", target->name().c_str(), rq.command, rs.result,
               rs.data.size(), rs.out_objects.size(), rs.copy_handles.size() + rs.move_handles.size(), hex);
    }
    static const char* bt_filter = debug::str(debug::IpcBt);  // e.g. "IFile#4"
    if (bt_filter) {
        std::string key = target->name() + "#" + std::to_string(rq.command);
        if (key == bt_filter) hw_log_backtrace(key.c_str());
    }
    for (auto& obj : rs.out_objects) {
        if (sess->dom->active) domain_out.push_back(sess->dom->add(obj));
        else extra_moves.push_back(new_session(obj));
    }
    write_response(msg, rs, sess->dom->active, domain_out, extra_moves);
    if (dump) dump_msg("resp", msg);
    return ResultSuccess;
}

}  // namespace ipc
