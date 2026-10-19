#include "host/file.h"
#include <algorithm>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace host {
#ifdef _WIN32
namespace { HANDLE native(FileHandle file) { return reinterpret_cast<HANDLE>(file); } }
FileHandle file_open(const std::filesystem::path& path, unsigned mode) {
    DWORD access = (mode & file_read ? GENERIC_READ : 0) | (mode & file_write ? GENERIC_WRITE : 0);
    auto preferred = path;preferred.make_preferred();
    return reinterpret_cast<FileHandle>(CreateFileW(preferred.c_str(), access ? access : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        mode & file_create_new ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}
void file_close(FileHandle file) { if (file != invalid_file) CloseHandle(native(file)); }
std::int64_t file_read_at(FileHandle file, void* bytes, std::size_t size, std::uint64_t offset) {
    OVERLAPPED operation{};operation.Offset = DWORD(offset);operation.OffsetHigh = DWORD(offset >> 32);
    DWORD count = 0;
    if (!ReadFile(native(file), bytes, DWORD(std::min<std::size_t>(size, 0x40000000)), &count, &operation)) return -1;
    return count;
}
std::int64_t file_write_at(FileHandle file, const void* bytes, std::size_t size, std::uint64_t offset) {
    OVERLAPPED operation{};operation.Offset = DWORD(offset);operation.OffsetHigh = DWORD(offset >> 32);
    DWORD count = 0;
    if (!WriteFile(native(file), bytes, DWORD(std::min<std::size_t>(size, 0x40000000)), &count, &operation)) return -1;
    return count;
}
bool file_resize(FileHandle file, std::uint64_t size) {
    if (size > std::uint64_t(std::numeric_limits<std::int64_t>::max())) return false;
    FILE_END_OF_FILE_INFO info{};info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(native(file), FileEndOfFileInfo, &info, sizeof(info)) != 0;
}
bool file_size(FileHandle file, std::uint64_t& size) {
    LARGE_INTEGER result{};
    if (!GetFileSizeEx(native(file), &result) || result.QuadPart < 0) return false;
    size = std::uint64_t(result.QuadPart);return true;
}
bool file_flush(FileHandle file) { return FlushFileBuffers(native(file)) != 0; }
bool file_rename_new(const std::filesystem::path& from, const std::filesystem::path& to) {
    auto source = from, destination = to;source.make_preferred();destination.make_preferred();
    return MoveFileExW(source.c_str(), destination.c_str(), 0) != 0;
}
unsigned long file_error() { return GetLastError(); }
bool file_times(const std::filesystem::path& path, FileTimes& result) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    auto preferred = path;preferred.make_preferred();
    if (!GetFileAttributesExW(preferred.c_str(), GetFileExInfoStandard, &data)) return false;
    auto seconds = [](const FILETIME& time) {
        const std::uint64_t ticks = (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        return ticks < 116444736000000000ull ? 0ull : (ticks - 116444736000000000ull) / 10000000;
    };
    result = {seconds(data.ftCreationTime), seconds(data.ftLastAccessTime), seconds(data.ftLastWriteTime)};
    return true;
}
#else
static_assert(sizeof(off_t) >= 8, "NSP positional IO requires 64-bit offsets");
namespace {
bool valid_offset(std::uint64_t offset) {
    if (offset <= std::uint64_t(std::numeric_limits<off_t>::max())) return true;
    errno = EOVERFLOW;return false;
}
}
FileHandle file_open(const std::filesystem::path& path, unsigned mode) {
    int flags = mode & file_write ? (mode & file_read ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (mode & file_create_new) flags |= O_CREAT | O_EXCL;
    int result;
    do { result = open(path.c_str(), flags | O_CLOEXEC, 0600); } while (result < 0 && errno == EINTR);
    return result;
}
void file_close(FileHandle file) { if (file != invalid_file) close(int(file)); }
std::int64_t file_read_at(FileHandle file, void* bytes, std::size_t size, std::uint64_t offset) {
    if (!valid_offset(offset)) return -1;
    ssize_t result;
    do { result = pread(int(file), bytes, std::min<std::size_t>(size, 0x40000000), off_t(offset)); } while (result < 0 && errno == EINTR);
    return result;
}
std::int64_t file_write_at(FileHandle file, const void* bytes, std::size_t size, std::uint64_t offset) {
    if (!valid_offset(offset)) return -1;
    ssize_t result;
    do { result = pwrite(int(file), bytes, std::min<std::size_t>(size, 0x40000000), off_t(offset)); } while (result < 0 && errno == EINTR);
    return result;
}
bool file_resize(FileHandle file, std::uint64_t size) {
    if (!valid_offset(size)) return false;
    int result;
    do { result = ftruncate(int(file), off_t(size)); } while (result < 0 && errno == EINTR);
    return result == 0;
}
bool file_size(FileHandle file, std::uint64_t& size) {
    struct stat result{};
    if (fstat(int(file), &result) || result.st_size < 0) return false;
    size = std::uint64_t(result.st_size);return true;
}
bool file_flush(FileHandle file) {
    int result;
    do { result = fsync(int(file)); } while (result < 0 && errno == EINTR);
    return result == 0;
}
bool file_rename_new(const std::filesystem::path& from, const std::filesystem::path& to) {
    return syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0;
}
unsigned long file_error() { return static_cast<unsigned long>(errno); }
bool file_times(const std::filesystem::path& path, FileTimes& result) {
    struct stat data{};
    if (stat(path.c_str(), &data)) return false;
    auto seconds = [](time_t time) { return time < 0 ? 0ull : std::uint64_t(time); };
    // POSIX stat has no portable birth time; ctime is the metadata-change fallback.
    result = {seconds(data.st_ctime), seconds(data.st_atime), seconds(data.st_mtime)};
    return true;
}
#endif
}  // namespace host
