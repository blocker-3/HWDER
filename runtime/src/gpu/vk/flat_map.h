// A small open-addressing hash map for the renderer's per-draw caches keyed by 64-bit hashes (vertex/index slice
// and index-range caches). std::unordered_map spent ~50-80 ns per lookup there (a modulo and a node pointer chase),
// several lookups per draw. Linear probing in one array, Fibonacci hashing. erase_if() rebuilds the table; clear()
// empties it.
// An empty slot has key 0 (a key of 0 is stored as kZeroKey), so a probe reads only the slot itself: the geometry
// cache holds up to 200k entries, and a separate occupancy array cost a second cache miss per probe.
// Pointers to values stay valid until the next insert of a new key (which may grow the table).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gpu::vk {

template <class V>
class FlatMap {
public:
    struct Slot {
        std::uint64_t first = 0;
        V second{};
    };
    Slot* find(std::uint64_t key) {
        if (!size_) return nullptr;
        key = stored(key);
        for (std::size_t i = index(key);; i = (i + 1) & mask_) {
            if (slots_[i].first == key) return &slots_[i];
            if (!slots_[i].first) return nullptr;
        }
    }
    Slot* end() const { return nullptr; }
    V& operator[](std::uint64_t key) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        key = stored(key);
        std::size_t i = index(key);
        for (; slots_[i].first; i = (i + 1) & mask_)
            if (slots_[i].first == key) return slots_[i].second;
        slots_[i].first = key;
        slots_[i].second = V{};
        size_++;
        return slots_[i].second;
    }
    std::size_t size() const { return size_; }
    // Removes every entry for which pred(key, value) is true (rebuilding the table; pointers become invalid).
    template <class Pred>
    void erase_if(Pred pred) {
        std::vector<Slot> old_slots;
        old_slots.swap(slots_);
        mask_ = size_ = 0;
        shift_ = 64;
        for (Slot& s : old_slots)
            if (s.first && !pred(s.first, s.second)) insert_stored(s.first, s.second);
    }
    void clear() {
        for (Slot& s : slots_) s.first = 0;
        size_ = 0;
    }

private:
    static constexpr std::uint64_t kZeroKey = 0x9E3779B97F4A7C15ull;
    static std::uint64_t stored(std::uint64_t key) { return key ? key : kZeroKey; }
    std::size_t index(std::uint64_t key) const { return (std::size_t)((key * 0x9E3779B97F4A7C15ull) >> shift_); }
    void insert_stored(std::uint64_t key, const V& v) {  // key already in stored form
        if ((size_ + 1) * 2 > slots_.size()) grow();
        std::size_t i = index(key);
        while (slots_[i].first) i = (i + 1) & mask_;
        slots_[i].first = key;
        slots_[i].second = v;
        size_++;
    }
    void grow() {
        std::vector<Slot> old_slots;
        old_slots.swap(slots_);
        const std::size_t cap = std::max<std::size_t>(1024, old_slots.size() * 2);
        slots_.resize(cap);
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) shift_--;
        size_ = 0;
        for (const Slot& s : old_slots)
            if (s.first) insert_stored(s.first, s.second);
    }
    std::vector<Slot> slots_;
    std::size_t mask_ = 0, size_ = 0;
    unsigned shift_ = 64;
};

}  // namespace gpu::vk
