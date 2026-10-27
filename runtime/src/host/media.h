#pragma once
#include <atomic>

namespace host {
// SDL window queries belong on the UI thread; audio reads only this snapshot.
inline std::atomic<bool> media_focus{true};
inline void set_media_focus(bool focused) { media_focus.store(focused, std::memory_order_relaxed); }
inline bool media_has_focus() { return media_focus.load(std::memory_order_relaxed); }
}  // namespace host
