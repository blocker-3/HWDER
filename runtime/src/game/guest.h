// Typed access to guest memory from the runtime. The guest is identity mapped (hwder/cpu.h): an address is a
// host pointer. These helpers replace GUESTP(u8, a) + memcpy with reads that name the type.
#pragma once
#include <string.h>

#include "hwder/cpu.h"

namespace game {

template <typename T>
inline T* at(u64 addr) {
    return reinterpret_cast<T*>(static_cast<uintptr_t>(addr));
}

template <typename T>
inline T read(u64 addr) {
    T v;
    memcpy(&v, at<const void>(addr), sizeof(T));
    return v;
}

template <typename T>
inline void write(u64 addr, const T& v) {
    memcpy(at<void>(addr), &v, sizeof(T));
}

// Follow a pointer stored in guest memory.
inline u64 deref(u64 addr) { return read<u64>(addr); }

// Virtual call target: slot `byte_offset` of the object's vtable.
inline u64 vfunc(u64 object, u32 byte_offset) { return read<u64>(read<u64>(object) + byte_offset); }

}  // namespace game
