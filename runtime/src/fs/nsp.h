// In-process NSP loading: the game's ExeFS modules and RomFS are read straight out of the user's
// own .nsp dump (decrypted on the fly with their prod.keys), so a distribution needs no extraction
// step. Directory mode (extracted exefs/ + romfs/) keeps working when no NSP is present or when
// HWDER_EXEFS / HWDER_ROMFS are set.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hwfs::nsp {

// Hyrule Warriors: Definitive Edition's application (program) ID, as in kernel.h and the README.
constexpr uint64_t kTitleId = 0x0100AE00096EA000ull;

// Paths picked on the startup screen (empty = search). Used after HWDER_NSP / HWDER_KEYS and before
// the automatic search; a set path that does not exist counts as missing, with no fallback.
void set_paths(const std::string& nsp, const std::string& keys);

// Looks for an NSP (HWDER_NSP, the picked path, else *.nsp beside the exe) and keys (HWDER_KEYS, the
// picked path, else prod.keys beside the exe, ~/.switch, yuzu/Ryujinx key folders). Returns true when
// the game will be served from the NSP. On failure the reason is logged and `error` describes it for
// the user. Can be called again after a failure (e.g. with new paths).
bool init(const std::string& exe_dir, std::string& error);
// Drops a successful init so the next init() checks again. Only before the game starts.
void reset();
bool active();

// Result of the last init(), for the startup screen.
enum class Status { Ok, NoGame, NoKeys, Unreadable };
struct Info {
    Status status = Status::NoGame;
    std::string error;            // plain-English reason when status != Ok
    std::string warning;          // set with Ok when the title ID is not HWDE's: playable, but likely the wrong file
    std::string nsp_path;         // the file looked at ("" = none found by the search)
    bool nsp_found = false;       // the file exists and opened
    std::string keys_path;        // the prod.keys used, or the explicit path that is missing
    bool keys_found = false;
    size_t key_count = 0, title_key_count = 0;
    uint64_t title_id = 0;        // program NCA title ID (0 = not read)
    std::string title_source;     // where title_id was read, e.g. "program NCA <name>.nca header"
    uint32_t version = 0;         // from the CNMT
    bool has_version = false;
    bool nsp_explicit = false, keys_explicit = false;  // set by an env var or the picked path
};
const Info& info();
const std::string& path();

// ExeFS file (main, rtld, sdk, subsdk0, main.npdm ...) fully decrypted. False if absent.
bool read_exefs(const char* name, std::vector<uint8_t>& out);
std::vector<std::string> exefs_files();

// Raw (level 3) RomFS image of the program NCA, as the SDK's IStorage sees it.
uint64_t romfs_size();
bool romfs_read(uint64_t off, uint8_t* dst, uint64_t size);

}  // namespace hwfs::nsp
