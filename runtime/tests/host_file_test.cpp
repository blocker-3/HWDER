// Public native file contracts. Synthetic bytes only; no game, keys or saves.
#include "host/file.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
unsigned checks = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(std::string(message) + " (host error " + std::to_string(host::file_error()) + ")");
}
struct Handle {
    host::FileHandle value = host::invalid_file;
    explicit Handle(host::FileHandle file) : value(file) {}
    ~Handle() { host::file_close(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
fs::path executable_directory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    check(count > 0 && count < buffer.size(), "locate test executable");
    return fs::path(std::wstring(buffer.data(), count)).parent_path();
#else
    std::vector<char> buffer(4096);
    for (;;) {
        const auto count = readlink("/proc/self/exe", buffer.data(), buffer.size());
        check(count >= 0, "locate test executable");
        if (size_t(count) < buffer.size()) return fs::path(std::string(buffer.data(), size_t(count))).parent_path();
        buffer.resize(buffer.size() * 2);
    }
#endif
}
struct Directory {
    fs::path path;
    explicit Directory(const fs::path& base) {
        fs::create_directories(base);
        const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
        path = base / ("host-file-" + std::to_string(serial));
        check(fs::create_directory(path), "create fresh owned fixture directory");
    }
    ~Directory() { std::error_code error;fs::remove_all(path, error); }
};
#ifndef _WIN32
struct PrivateUmask {
    mode_t previous = umask(0077);
    ~PrivateUmask() { umask(previous); }
};
#endif
void make_sparse(host::FileHandle file) {
#ifdef _WIN32
    DWORD returned = 0;
    check(DeviceIoControl(reinterpret_cast<HANDLE>(file), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0,
          "enable sparse file before large-offset write");
#else
    (void)file;  // POSIX writes beyond EOF create holes without a separate flag.
#endif
}
void sparse_allocation(host::FileHandle file, const fs::path& path) {
#ifdef _WIN32
    (void)path;
    FILE_STANDARD_INFO info{};
    check(GetFileInformationByHandleEx(reinterpret_cast<HANDLE>(file), FileStandardInfo, &info, sizeof(info)) != 0 &&
          info.AllocationSize.QuadPart < 16ll * 1024 * 1024, "large logical offset remains sparse");
#else
    (void)file;
    struct stat info{};
    check(stat(path.c_str(), &info) == 0 && std::uint64_t(info.st_blocks) * 512 < 16ull * 1024 * 1024,
          "large logical offset remains sparse");
#endif
}
std::vector<unsigned char> read_all(const fs::path& path) {
    Handle file(host::file_open(path, host::file_read));
    check(file.value != host::invalid_file, "open fixture through its current name");
    std::uint64_t size = 0;
    check(host::file_size(file.value, size) && size < 1024, "small fixture length");
    std::vector<unsigned char> bytes(size_t(size), 0);
    check(host::file_read_at(file.value, bytes.data(), bytes.size(), 0) == std::int64_t(bytes.size()), "read complete small fixture");
    return bytes;
}
}  // namespace

int main() {
    try {
#ifndef _WIN32
        PrivateUmask private_files;
#endif
        // CMake places this executable in the host build directory. Keep every
        // fixture in its test-tmp subtree, independently of the caller's cwd.
        Directory owned(executable_directory() / "test-tmp");
        const auto directory = owned.path / fs::path(u8"Unicode space \u03bb \u65e5\u672c");
        check(fs::create_directory(directory), "create Unicode and space path");
        const auto original = directory / "source data.bin", destination = directory / "existing data.bin", moved = directory / "renamed data.bin";
        const std::vector<unsigned char> payload{0x00, 0x11, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd};
        const std::vector<unsigned char> sentinel{0x9a, 0x7b, 0x5c, 0x3d};
        {
            Handle file(host::file_open(original, host::file_read | host::file_write | host::file_create_new));
            check(file.value != host::invalid_file, "exclusive create at Unicode path");
            std::uint64_t size = 99;
            check(host::file_size(file.value, size) && size == 0, "new file is empty");
            std::array<unsigned char, 32> bytes{};
            check(host::file_read_at(file.value, bytes.data(), bytes.size(), 0) == 0, "empty file returns clean EOF");
            check(host::file_write_at(file.value, payload.data(), payload.size(), 0) == std::int64_t(payload.size()), "write initial binary payload");
            check(host::file_flush(file.value), "flush initial payload");
            check(host::file_read_at(file.value, bytes.data(), payload.size(), 0) == std::int64_t(payload.size()) &&
                  !std::memcmp(bytes.data(), payload.data(), payload.size()), "positional read observes written bytes");
            bytes.fill(0);
            check(host::file_read_at(file.value, bytes.data(), 5, 2) == 5 && !std::memcmp(bytes.data(), payload.data() + 2, 5),
                  "positional read is independent of earlier file operations");
            {
                Handle collision(host::file_open(original, host::file_write | host::file_create_new));
                check(collision.value == host::invalid_file, "exclusive create refuses existing file");
            }
            check(read_all(original) == payload, "failed exclusive create preserves existing bytes");
#ifndef _WIN32
            struct stat permissions{};
            check(stat(original.c_str(), &permissions) == 0 && (permissions.st_mode & 0777) == 0600, "new file is private 0600");
#endif
            make_sparse(file.value);
            constexpr std::uint64_t large_offset = (1ull << 32) + 123;
            check(host::file_write_at(file.value, sentinel.data(), sentinel.size(), large_offset) == std::int64_t(sentinel.size()),
                  "write beyond 4 GiB without offset truncation");
            check(host::file_size(file.value, size) && size == large_offset + sentinel.size(), "large offset sets full 64-bit size");
            check(host::file_flush(file.value), "flush sparse large-offset write");
            bytes.fill(0);
            check(host::file_read_at(file.value, bytes.data(), sentinel.size(), large_offset) == std::int64_t(sentinel.size()) &&
                  !std::memcmp(bytes.data(), sentinel.data(), sentinel.size()), "read beyond 4 GiB preserves offset and payload");
            bytes.fill(0xff);
            check(host::file_read_at(file.value, bytes.data(), bytes.size(), large_offset - bytes.size()) == std::int64_t(bytes.size()) &&
                  bytes == std::array<unsigned char, 32>{}, "sparse hole reads as zeros");
            sparse_allocation(file.value, original);
            check(host::file_read_at(file.value, bytes.data(), 1, large_offset + sentinel.size()) == 0, "end of large file returns clean EOF");
            check(host::file_read_at(file.value, bytes.data(), 1, std::numeric_limits<std::uint64_t>::max()) < 0,
                  "unrepresentable read offset fails without wrapping");
            check(host::file_write_at(file.value, sentinel.data(), sentinel.size(), std::numeric_limits<std::uint64_t>::max()) < 0,
                  "unrepresentable write offset fails without wrapping");
            check(host::file_size(file.value, size) && size == large_offset + sentinel.size(), "invalid offset leaves existing file size unchanged");
            check(!host::file_resize(file.value, std::numeric_limits<std::uint64_t>::max()), "unrepresentable resize fails");
            check(host::file_resize(file.value, 3), "truncate sparse file");
            check(host::file_size(file.value, size) && size == 3, "truncation updates size");
            check(read_all(original) == std::vector<unsigned char>(payload.begin(), payload.begin() + 3), "truncation preserves prefix only");
            check(host::file_resize(file.value, payload.size()), "extend truncated file");
            auto extended = read_all(original);
            auto expected = payload;
            std::fill(expected.begin() + 3, expected.end(), 0);
            check(extended == expected, "extension zeroes discarded tail");
            check(host::file_write_at(file.value, payload.data(), payload.size(), 0) == std::int64_t(payload.size()) && host::file_flush(file.value),
                  "restore small payload after sparse tests");
            Handle existing(host::file_open(destination, host::file_read | host::file_write | host::file_create_new));
            check(existing.value != host::invalid_file &&
                  host::file_write_at(existing.value, sentinel.data(), sentinel.size(), 0) == std::int64_t(sentinel.size()) &&
                  host::file_flush(existing.value), "create distinct rename destination");
            check(!host::file_rename_new(original, destination), "no-replace rename rejects collision");
            check(read_all(original) == payload && read_all(destination) == sentinel, "rename collision preserves both files and contents");
            check(host::file_rename_new(original, moved), "no-replace rename succeeds at unused name");
            check(!fs::exists(original) && read_all(moved) == payload && read_all(destination) == sentinel,
                  "successful rename moves only intended file");
            bytes.fill(0);
            check(host::file_read_at(file.value, bytes.data(), payload.size(), 0) == std::int64_t(payload.size()) &&
                  !std::memcmp(bytes.data(), payload.data(), payload.size()), "open handle remains usable across rename");
            host::FileTimes times;
            check(host::file_times(moved, times) && times.modified > 0, "native timestamp query succeeds at new name");
            Handle missing(host::file_open(directory / "missing" / "file.bin", host::file_read));
            check(missing.value == host::invalid_file && host::file_error() != 0, "missing path reports an error");
        }
        std::error_code cleanup;
        fs::remove_all(owned.path, cleanup);
        check(!cleanup && !fs::exists(owned.path), "remove only owned synthetic fixtures");
        std::printf("host file contract: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "host file contract FAIL: %s\n", error.what());
        return 1;
    }
}
