// The kernel handle table. Moved text-for-text from kernel.cpp so the unit tests can link it alone (they stub
// g_kernel_lock and current_thread()).
#include "kernel.h"

namespace kern {

Handle HandleTable::add(ObjectPtr obj) {
    KGuard g(g_kernel_lock);
    Handle h = (next_++ << 15) | 0x10;  // HOS-like handle values (non-zero, low bits set)
    table_[h] = std::move(obj);
    return h;
}
ObjectPtr HandleTable::get(Handle h) const {
    KGuard g(g_kernel_lock);
    if (h == kCurrentThread) {
        Thread* t = current_thread();
        // The current thread's object is kept alive by its own handle entry.
        for (auto& [k, v] : table_)
            if (v.get() == t) return v;
        return nullptr;
    }
    auto it = table_.find(h);
    return it == table_.end() ? nullptr : it->second;
}
bool HandleTable::close(Handle h) {
    KGuard g(g_kernel_lock);
    return table_.erase(h) != 0;
}

}  // namespace kern
