#include <cstring>
// IFileSystem / IFile / IDirectory backed by a host directory (save data, SD card).
#include "fs/host_root.h"
#include "host/file.h"
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <memory>
#include <vector>
#include <cerrno>
#include <limits>

#include "fs_internal.h"

namespace hwfs {

namespace {

constexpr u32 kEntryDir = 0, kEntryFile = 1;
constexpr u32 kOpenRead = 1, kOpenWrite = 2, kOpenAppend = 4;
constexpr u32 kDirFilterDirs = 1, kDirFilterFiles = 2, kDirNoFileSize = 0x80000000u;

// Guest path -> normalized component list ("." and ".." resolved, clamped at the root).
bool normalize(const std::string& in, std::string* out) {
    std::string p = in;
    size_t colon = p.find(':');
    if (colon != std::string::npos && p.find('/') > colon) p = p.substr(colon + 1);  // strip "mount:"
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find_first_of("/\\", i);
        if (j == std::string::npos) j = p.size();
        std::string c = p.substr(i, j - i);
        if (c == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!c.empty() && c != ".") {
            parts.push_back(std::move(c));
        }
        i = j + 1;
    }
    out->clear();
    for (auto& c : parts) *out += "/" + c;
    if (out->empty()) *out = "/";
    return true;
}

bool is_dir(const HostPath& p) { std::error_code error;return std::filesystem::is_directory(p, error); }
bool path_exists(const HostPath& p) { std::error_code error;return std::filesystem::exists(p, error); }
void create_dirs(const HostPath& p) { std::error_code error;std::filesystem::create_directories(p, error); }

// Deletes everything below dir (not dir itself).
bool clean_dir(const HostPath& dir) {
#ifdef _WIN32
    WIN32_FIND_DATAW entry{};
    HANDLE handle = FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    if (handle == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L"..")) continue;
        const auto path = dir / entry.cFileName;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok &= clean_dir(path);ok &= RemoveDirectoryW(path.c_str()) != 0;
        } else {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            ok &= DeleteFileW(path.c_str()) != 0;
        }
    } while (FindNextFileW(handle, &entry));
    FindClose(handle);return ok;
#else
    std::error_code error;
    for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        // remove_all removes a symlink itself and never follows its target.
        std::filesystem::remove_all(it->path(), error);
        if (error) return false;
    }
    return !error;
#endif
}

// Shared per-filesystem state: the root, plus how the guest named it in logs.
struct FsRoot {
    HostPath root;  // native absolute path below the selected save/SD/RomFS root
    std::string label;
    bool read_only = false;
    HostPath host(const std::string& norm) const {
        if (norm == "/") return root;
        const auto candidate = (root / std::filesystem::u8path(norm.substr(1))).make_preferred();
#ifndef _WIN32
        // A host symlink must not let a normalized guest path escape its root.
        std::error_code error;
        const auto base = std::filesystem::weakly_canonical(root, error);if (error) return {};
        const auto target = std::filesystem::weakly_canonical(candidate, error);if (error) return {};
        const auto relative = target.lexically_relative(base);
        if (relative.empty() || *relative.begin() == "..") return {};
#endif
        return candidate;
    }
};

class IFile : public FsService {
public:
    IFile(host::FileHandle h, u32 mode, std::string path) : FsService("IFile"), h_(h), mode_(mode), path_(std::move(path)) {
        reg(0, [this](Request& rq, Response& rs) {  // Read(u32 option, s64 offset, s64 size, B buffer) -> s64
            rq.pop<u32>();
            s64 off = rq.pop<s64>();
            s64 size = rq.pop<s64>();
            ipc::Buffer b = rq.out_buffer();
            if (off < 0) return void(rs.result = ResultInvalidOffset);
            if (size < 0) return void(rs.result = ResultInvalidSize);
            if (!(mode_ & kOpenRead)) return void(rs.result = ResultReadNotPermitted);
            u64 want = std::min<u64>((u64)size, b.size), done = 0;
            u8* dst = (u8*)b.addr;
            while (done < want) {
                u64 o = (u64)off + done;
                const auto got = host::file_read_at(h_, dst + done, size_t(std::min<u64>(want - done, 0x40000000ull)), o);
                if (got <= 0) break;
                done += got;
            }
            if (trace_enabled())
                hw_log("fs: file read %s 0x%llx+0x%llx -> 0x%llx", path_.c_str(), (unsigned long long)off,
                       (unsigned long long)want, (unsigned long long)done);
            rs.push<u64>(done);
        });
        reg(1, [this](Request& rq, Response& rs) {  // Write(u32 option, s64 offset, s64 size, A buffer)
            u32 option = rq.pop<u32>();
            s64 off = rq.pop<s64>();
            s64 size = rq.pop<s64>();
            ipc::Buffer b = rq.in_buffer();
            if (off < 0) return void(rs.result = ResultInvalidOffset);
            if (size < 0) return void(rs.result = ResultInvalidSize);
            if (!(mode_ & kOpenWrite)) return void(rs.result = ResultWriteNotPermitted);
            u64 want = std::min<u64>((u64)size, b.size), done = 0;
            const u8* src = (const u8*)b.addr;
            while (done < want) {
                u64 o = (u64)off + done;
                const auto put = host::file_write_at(h_, src + done, size_t(std::min<u64>(want - done, 0x40000000ull)), o);
                if (put <= 0) {
                    hw_log("fs: write to %s failed (error %lu)", path_.c_str(), host::file_error());
                    return void(rs.result = ResultUsableSpaceNotEnough);
                }
                done += put;
            }
            if (option & 1) host::file_flush(h_);
            if (trace_enabled())
                hw_log("fs: file write %s 0x%llx+0x%llx", path_.c_str(), (unsigned long long)off, (unsigned long long)want);
        });
        reg(2, [this](Request&, Response&) {  // Flush
            if (mode_ & kOpenWrite) host::file_flush(h_);
        });
        reg(3, [this](Request& rq, Response& rs) {  // SetSize(s64)
            s64 size = rq.pop<s64>();
            if (size < 0) return void(rs.result = ResultInvalidSize);
            if (!(mode_ & kOpenWrite)) return void(rs.result = ResultWriteNotPermitted);
            if (!host::file_resize(h_, u64(size)))
                rs.result = ResultUsableSpaceNotEnough;
        });
        reg(4, [this](Request&, Response& rs) {  // GetSize -> s64
            u64 size = 0;host::file_size(h_, size);
            rs.push<s64>(s64(size));
        });
        reg(5, [](Request&, Response& rs) {  // OperateRange -> QueryRangeInfo (0x40)
            u8 info[0x40] = {};
            rs.data.insert(rs.data.end(), info, info + sizeof(info));
        });
    }
    ~IFile() override { host::file_close(h_); }

private:
    host::FileHandle h_;
    u32 mode_;
    std::string path_;
};

#pragma pack(push, 1)
struct DirectoryEntry {
    char name[0x301];
    u8 pad0[3];
    u8 type;
    u8 pad1[3];
    s64 size;
};
#pragma pack(pop)
static_assert(sizeof(DirectoryEntry) == 0x310);

class IDirectory : public FsService {
public:
    IDirectory(std::vector<DirectoryEntry> entries) : FsService("IDirectory"), entries_(std::move(entries)) {
        reg(0, [this](Request& rq, Response& rs) {  // Read(B buffer) -> s64 count
            ipc::Buffer b = rq.out_buffer();
            std::lock_guard<std::mutex> l(m_);
            u64 n = std::min<u64>(b.size / sizeof(DirectoryEntry), entries_.size() - pos_);
            if (n) memcpy((void*)b.addr, &entries_[pos_], (size_t)n * sizeof(DirectoryEntry));
            pos_ += (size_t)n;
            rs.push<s64>((s64)n);
        });
        reg(1, [this](Request&, Response& rs) { rs.push<s64>((s64)entries_.size()); });  // GetEntryCount
    }

private:
    std::mutex m_;
    std::vector<DirectoryEntry> entries_;
    size_t pos_ = 0;
};

class IFileSystem : public FsService {
public:
    explicit IFileSystem(std::shared_ptr<FsRoot> fs) : FsService("IFileSystem"), fs_(std::move(fs)) {
        // All path-taking commands: path in the first X (type 0x19) buffer.
        reg(0, [this](Request& rq, Response& rs) {  // CreateFile(u32 option, s64 size, path)
            rq.pop<u32>();
            s64 size = rq.pop<s64>();
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (ro(rs)) return;
            if (path_exists(h)) return fail(rs, ResultPathAlreadyExists, "CreateFile", p);
            if (!parent_is_dir(h)) return fail(rs, ResultPathNotFound, "CreateFile", p);
            const auto f = host::file_open(h, host::file_write | host::file_create_new);
            if (f == host::invalid_file) return fail(rs, ResultPathNotFound, "CreateFile", p);
            if (size > 0) {
                if (!host::file_resize(f, u64(size)))
                    rs.result = ResultUsableSpaceNotEnough;
            }
            host::file_close(f);
            log_op("CreateFile", p, rs.result, size);
        });
        reg(1, [this](Request& rq, Response& rs) {  // DeleteFile(path)
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (ro(rs)) return;
            if (!path_exists(h) || is_dir(h))
                return fail(rs, ResultPathNotFound, "DeleteFile", p);
            std::error_code error;
            if (!std::filesystem::remove(h, error)) rs.result = ResultTargetLocked;
            log_op("DeleteFile", p, rs.result);
        });
        reg(2, [this](Request& rq, Response& rs) {  // CreateDirectory(path)
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (ro(rs)) return;
            if (path_exists(h)) return fail(rs, ResultPathAlreadyExists, "CreateDirectory", p);
            if (!parent_is_dir(h)) return fail(rs, ResultPathNotFound, "CreateDirectory", p);
            std::error_code error;
            if (!std::filesystem::create_directory(h, error)) rs.result = ResultPathNotFound;
            log_op("CreateDirectory", p, rs.result);
        });
        reg(3, [this](Request& rq, Response& rs) {  // DeleteDirectory(path)
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (ro(rs)) return;
            if (!is_dir(h)) return fail(rs, ResultPathNotFound, "DeleteDirectory", p);
            std::error_code error;
            if (!std::filesystem::remove(h, error))
                rs.result = error == std::errc::directory_not_empty ? ResultDirectoryNotEmpty : ResultTargetLocked;
            log_op("DeleteDirectory", p, rs.result);
        });
        reg(4, [this](Request& rq, Response& rs) {  // DeleteDirectoryRecursively(path)
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (ro(rs)) return;
            if (!is_dir(h)) return fail(rs, ResultPathNotFound, "DeleteDirectoryRecursively", p);
            if (p == "/") rs.result = clean_dir(h) ? 0 : ResultTargetLocked;
            else {
                std::error_code error;
                if (!clean_dir(h) || !std::filesystem::remove(h, error)) rs.result = ResultTargetLocked;
            }
            log_op("DeleteDirectoryRecursively", p, rs.result);
        });
        reg(5, [this](Request& rq, Response& rs) { rename(rq, rs, false); });  // RenameFile(old, new)
        reg(6, [this](Request& rq, Response& rs) { rename(rq, rs, true); });   // RenameDirectory(old, new)
        reg(7, [this](Request& rq, Response& rs) {  // GetEntryType(path) -> u32
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (!path_exists(h)) {
                if (trace_enabled()) hw_log("fs: [%s] GetEntryType %s -> not found", fs_->label.c_str(), p.c_str());
                return void(rs.result = ResultPathNotFound);
            }
            rs.push<u32>(is_dir(h) ? kEntryDir : kEntryFile);
        });
        reg(8, [this](Request& rq, Response& rs) {  // OpenFile(u32 mode, path) -> IFile
            u32 mode = rq.pop<u32>();
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (fs_->read_only && (mode & (kOpenWrite | kOpenAppend))) return void(rs.result = ResultPermissionDenied);
            unsigned access = 0;
            if (mode & kOpenRead) access |= host::file_read;
            if (mode & (kOpenWrite | kOpenAppend)) access |= host::file_write;
            if (is_dir(h)) return fail(rs, ResultPathNotFound, "OpenFile", p);
            const auto f = host::file_open(h, access);
            if (f == host::invalid_file) {
                const auto error = host::file_error();
#ifdef _WIN32
                const bool locked = error == 32;  // ERROR_SHARING_VIOLATION
#else
                const bool locked = error == EBUSY || error == ETXTBSY;
#endif
                return fail(rs, locked ? ResultTargetLocked : ResultPathNotFound, "OpenFile", p);
            }
            log_op("OpenFile", p, 0, mode);
            rs.push_object(std::make_shared<IFile>(f, mode, fs_->label + ":" + p));
        });
        reg(9, [this](Request& rq, Response& rs) {  // OpenDirectory(u32 filter, path) -> IDirectory
            u32 filter = rq.pop<u32>();
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (!is_dir(h)) return fail(rs, ResultPathNotFound, "OpenDirectory", p);
            std::vector<DirectoryEntry> entries;
            std::error_code error;
            for (std::filesystem::directory_iterator it(h, error), end; !error && it != end; it.increment(error)) {
                const bool dir = it->is_directory(error);
                if (error) break;
                if (dir && !(filter & kDirFilterDirs)) continue;
                if (!dir && !(filter & kDirFilterFiles)) continue;
                DirectoryEntry e{};
                const auto name = path_utf8(it->path().filename());
                memcpy(e.name, name.data(), std::min<size_t>(name.size(), 0x300));
                e.type = dir ? kEntryDir : kEntryFile;
                if (!dir && !(filter & kDirNoFileSize)) {
                    e.size = s64(it->file_size(error));
                    if (error) break;
                }
                entries.push_back(e);
            }
            log_op("OpenDirectory", p, 0, entries.size());
            rs.push_object(std::make_shared<IDirectory>(std::move(entries)));
        });
        reg(10, [](Request&, Response&) {});  // Commit: writes go straight to the host files
        reg(11, [this](Request&, Response& rs) { rs.push<s64>(free_space()); });       // GetFreeSpaceSize
        reg(12, [this](Request&, Response& rs) { rs.push<s64>(total_space()); });      // GetTotalSpaceSize
        reg(13, [this](Request& rq, Response& rs) {  // CleanDirectoryRecursively(path)
            std::string p;
            HostPath h = path(rq, 0, &p);
            if (ro(rs)) return;
            if (!is_dir(h)) return fail(rs, ResultPathNotFound, "CleanDirectoryRecursively", p);
            if (!clean_dir(h)) rs.result = ResultTargetLocked;
            log_op("CleanDirectoryRecursively", p, rs.result);
        });
        reg(14, [this](Request& rq, Response& rs) {  // GetFileTimeStampRaw(path) -> 0x20 bytes
            std::string p;
            HostPath h = path(rq, 0, &p);
            host::FileTimes times;
            if (!host::file_times(h, times)) return void(rs.result = ResultPathNotFound);
            rs.push<u64>(times.created);
            rs.push<u64>(times.accessed);
            rs.push<u64>(times.modified);
            rs.push<u64>(1);  // is_valid
        });
        reg(15, [](Request&, Response&) {});  // QueryEntry: nothing to report
    }

private:
    HostPath path(Request& rq, size_t i, std::string* norm) {
        normalize(rq.in_string(i), norm);
        return fs_->host(*norm);
    }
    bool ro(Response& rs) {
        if (!fs_->read_only) return false;
        rs.result = ResultPermissionDenied;
        return true;
    }
    static bool parent_is_dir(const HostPath& h) {
        return !h.empty() && is_dir(h.parent_path());
    }
    void fail(Response& rs, Result r, const char* op, const std::string& p) {
        rs.result = r;
        log_op(op, p, r);
    }
    void log_op(const char* op, const std::string& p, Result r, u64 arg = 0) {
        hw_log("fs: [%s] %s %s (0x%llx) -> 0x%x", fs_->label.c_str(), op, p.c_str(), (unsigned long long)arg, r);
    }
    void rename(Request& rq, Response& rs, bool dir) {
        const char* op = dir ? "RenameDirectory" : "RenameFile";
        std::string a, b;
        HostPath ha = path(rq, 0, &a), hb = path(rq, 1, &b);
        if (ro(rs)) return;
        if (!path_exists(ha) || is_dir(ha) != dir)
            return fail(rs, ResultPathNotFound, op, a);
        if (path_exists(hb)) return fail(rs, ResultPathAlreadyExists, op, b);
        if (!parent_is_dir(hb)) return fail(rs, ResultPathNotFound, op, b);
        if (!host::file_rename_new(ha, hb)) rs.result = ResultTargetLocked;
        log_op(op, a + " -> " + b, rs.result);
    }
    s64 free_space() {
        std::error_code error;
        const auto space = std::filesystem::space(fs_->root, error);
        if (!error) return s64(std::min<u64>(space.available, 0x100000000ull));  // report at most 4 GiB
        return 0x100000000ll;
    }
    s64 total_space() { return 0x100000000ll; }

    std::shared_ptr<FsRoot> fs_;
};

}  // namespace

ServicePtr make_host_filesystem(const HostPath& root, bool read_only, const std::string& label) {
    auto fs = std::make_shared<FsRoot>();
    fs->root = host_root(root);  // never lexically_normal() a Windows long path (host_root.h)
    fs->read_only = read_only;
    fs->label = label;
    if (!read_only) create_dirs(fs->root);
    return std::make_shared<IFileSystem>(fs);
}

}  // namespace hwfs
