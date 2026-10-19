#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace host {
using FileHandle = std::intptr_t;
constexpr FileHandle invalid_file = -1;
enum FileMode : unsigned { file_read = 1, file_write = 2, file_create_new = 4 };
FileHandle file_open(const std::filesystem::path& path, unsigned mode);
void file_close(FileHandle file);
std::int64_t file_read_at(FileHandle file, void* bytes, std::size_t size, std::uint64_t offset);
std::int64_t file_write_at(FileHandle file, const void* bytes, std::size_t size, std::uint64_t offset);
bool file_resize(FileHandle file, std::uint64_t size);
bool file_size(FileHandle file, std::uint64_t& size);
bool file_flush(FileHandle file);
// Rename without replacing an existing destination, including concurrent creates.
bool file_rename_new(const std::filesystem::path& from, const std::filesystem::path& to);
unsigned long file_error();
struct FileTimes { std::uint64_t created = 0, accessed = 0, modified = 0; };
bool file_times(const std::filesystem::path& path, FileTimes& result);
}  // namespace host
