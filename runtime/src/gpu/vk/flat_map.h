// A small open-addressing hash map for the renderer's per-draw caches keyed by 64-bit hashes (vertex/index slice
// and index-range caches). std::unordered_map spent ~50-80 ns per lookup there (a modulo and a node pointer chase),
// several lookups per draw. Linear probing in one array, Fibonacci hashing. erase_if() rebuilds the table; clear()
// empties it.
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
        for (std::size_t i = index(key);; i = (i + 1) & mask_) {
            if (!used_[i]) return nullptr;
            if (slots_[i].first == key) return &slots_[i];
        }
    }
    Slot* end() const { return nullptr; }
    V& operator[](std::uint64_t key) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        std::size_t i = index(key);
        for (; used_[i]; i = (i + 1) & mask_)
            if (slots_[i].first == key) return slots_[i].second;
        used_[i] = 1;
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
        std::vector<std::uint8_t> old_used;
        old_slots.swap(slots_);
        old_used.swap(used_);
        mask_ = size_ = 0;
        shift_ = 64;
        for (std::size_t i = 0; i < old_slots.size(); i++)
            if (old_used[i] && !pred(old_slots[i].first, old_slots[i].second)) (*this)[old_slots[i].first] = old_slots[i].second;
    }
    void clear() {
        std::fill(used_.begin(), used_.end(), 0);
        size_ = 0;
    }

private:
    std::size_t index(std::uint64_t key) const { return (std::size_t)((key * 0x9E3779B97F4A7C15ull) >> shift_); }
    void grow() {
        std::vector<Slot> old_slots;
        std::vector<std::uint8_t> old_used;
        old_slots.swap(slots_);
        old_used.swap(used_);
        const std::size_t cap = std::max<std::size_t>(1024, old_slots.size() * 2);
        slots_.resize(cap);
        used_.assign(cap, 0);
        mask_ = cap - 1;
        shift_ = 64;
        for (std::size_t c = cap; c > 1; c >>= 1) shift_--;
        size_ = 0;
        for (std::size_t i = 0; i < old_slots.size(); i++)
            if (old_used[i]) (*this)[old_slots[i].first] = old_slots[i].second;
    }
    std::vector<Slot> slots_;
    std::vector<std::uint8_t> used_;
    std::size_t mask_ = 0, size_ = 0;
    unsigned shift_ = 64;
};

}  // namespace gpu::vk
