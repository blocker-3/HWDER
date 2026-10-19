#pragma once
#include "pipeline_catalog.h"

namespace gpu::catalog {
// Rebuilt from the currently selected NSP/extracted RomFS. Owns metadata only.
class GameSources {
public:
    bool index();
    bool locate(const u8* bytes, size_t available, u32 stage, Locator& out) const;
    bool read(const Locator&, std::vector<u8>&) const;
    size_t count() const { return shaders_.size(); }
private:
    struct Resource { std::string name; u64 offset, size; };
    std::vector<Resource> files_;
    std::vector<Locator> shaders_;
    bool nsp_ = false;
    std::string root_;
    bool resource(const Resource&, std::vector<u8>&) const;
};
} // namespace gpu::catalog
