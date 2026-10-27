// HWDER Setup window for Linux (SDL3 + Dear ImGui). Code-free: it only drives tools/linux_setup.py, the same
// backend as the terminal setup, through its --json lines, so both always do the same steps. Usable with a mouse,
// keyboard, touch screen or controller (Desktop Mode and Steam's Game Mode).
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "font_data.h"  // Roboto Medium (Apache-2.0), generated from third_party/imgui/misc/fonts at build time

namespace {

// ---- Minimal JSON (the backend's own output only) ---------------------------------------------------------------

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;
    const Json& operator[](const char* key) const {
        static const Json none;
        for (auto& [k, v] : o)
            if (k == key) return v;
        return none;
    }
    bool truthy() const { return kind == Bool ? b : kind == Number ? n != 0 : kind == String ? !s.empty() : false; }
    std::string str() const { return kind == String ? s : std::string(); }
};

struct Parser {
    const char* p;
    const char* end;
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool parse(Json& out) {
        ws();
        if (p >= end) return false;
        if (*p == '{') {
            out.kind = Json::Object;
            ++p;
            ws();
            if (p < end && *p == '}') { ++p; return true; }
            while (p < end) {
                Json key;
                if (!parse(key) || key.kind != Json::String) return false;
                ws();
                if (p >= end || *p++ != ':') return false;
                Json value;
                if (!parse(value)) return false;
                out.o.emplace_back(key.s, std::move(value));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (*p == '[') {
            out.kind = Json::Array;
            ++p;
            ws();
            if (p < end && *p == ']') { ++p; return true; }
            while (p < end) {
                Json value;
                if (!parse(value)) return false;
                out.a.push_back(std::move(value));
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
            return false;
        }
        if (*p == '"') {
            out.kind = Json::String;
            ++p;
            while (p < end && *p != '"') {
                char c = *p++;
                if (c != '\\') { out.s += c; continue; }
                if (p >= end) return false;
                c = *p++;
                switch (c) {
                case 'n': out.s += '\n'; break;
                case 't': out.s += '\t'; break;
                case 'r': out.s += '\r'; break;
                case 'b': out.s += '\b'; break;
                case 'f': out.s += '\f'; break;
                case 'u': {
                    if (end - p < 4) return false;
                    unsigned cp = (unsigned)strtoul(std::string(p, 4).c_str(), nullptr, 16);
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        unsigned lo = (unsigned)strtoul(std::string(p + 2, 4).c_str(), nullptr, 16);
                        p += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp < 0x80) out.s += char(cp);
                    else if (cp < 0x800) { out.s += char(0xC0 | (cp >> 6)); out.s += char(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) {
                        out.s += char(0xE0 | (cp >> 12)); out.s += char(0x80 | ((cp >> 6) & 0x3F));
                        out.s += char(0x80 | (cp & 0x3F));
                    } else {
                        out.s += char(0xF0 | (cp >> 18)); out.s += char(0x80 | ((cp >> 12) & 0x3F));
                        out.s += char(0x80 | ((cp >> 6) & 0x3F)); out.s += char(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out.s += c;
                }
            }
            if (p >= end) return false;
            ++p;
            return true;
        }
        if (!strncmp(p, "true", 4)) { out.kind = Json::Bool; out.b = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { out.kind = Json::Bool; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char* stop = nullptr;
        out.n = strtod(p, &stop);
        if (stop == p) return false;
        out.kind = Json::Number;
        p = stop;
        return true;
    }
};

bool parse_json(const std::string& text, Json& out) {
    Parser parser{text.data(), text.data() + text.size()};
    return parser.parse(out);
}

// ---- The backend process ----------------------------------------------------------------------------------------

std::string g_here;  // folder of the release (hwder-setup, tools/, setup/)

class Task {
public:
    ~Task() { stop(); if (reader_.joinable()) reader_.join(); }
    bool start(const std::vector<std::string>& args) {
        if (running()) return false;
        if (reader_.joinable()) reader_.join();
        int fds[2];
        if (pipe(fds) != 0) return false;
        std::string script = g_here + "/tools/linux_setup.py";
        std::vector<std::string> argv{"python3", "-u", script, "--json"};
        argv.insert(argv.end(), args.begin(), args.end());
        pid_t pid = fork();
        if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
        if (pid == 0) {
            setsid();  // its own session: Stop reaches the whole build, and a Steam restart does not end it
            dup2(fds[1], 1);
            int null = open("/dev/null", O_RDWR);
            dup2(null, 0);
            dup2(fds[1], 2);
            close(fds[0]);
            std::vector<char*> raw;
            for (auto& a : argv) raw.push_back(a.data());
            raw.push_back(nullptr);
            execvp("python3", raw.data());
            _exit(127);
        }
        close(fds[1]);
        pid_ = pid;
        done_ = false;
        reader_ = std::thread([this, fd = fds[0]] {
            std::string buffer;
            char chunk[4096];
            for (;;) {
                ssize_t n = read(fd, chunk, sizeof chunk);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                buffer.append(chunk, size_t(n));
                size_t nl;
                while ((nl = buffer.find('\n')) != std::string::npos) {
                    std::string line = buffer.substr(0, nl);
                    buffer.erase(0, nl + 1);
                    std::lock_guard lock(mutex_);
                    lines_.push_back(std::move(line));
                }
            }
            close(fd);
            int status = 0;
            waitpid(pid_, &status, 0);
            std::lock_guard lock(mutex_);
            exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
            done_ = true;
        });
        return true;
    }
    bool running() const { return pid_ > 0 && !done_; }
    void stop() {
        if (running()) kill(-pid_, SIGINT);
    }
    // Lines produced since the last call; finished is set once, with the exit code, when the process ended.
    std::vector<std::string> take(bool& finished, int& code) {
        std::lock_guard lock(mutex_);
        std::vector<std::string> out(lines_.begin(), lines_.end());
        lines_.clear();
        finished = done_ && pid_ > 0 && !reported_;
        if (finished) { reported_ = true; code = exit_code_; }
        return out;
    }
    void reset_report() { reported_ = false; }

private:
    pid_t pid_ = -1;
    std::atomic<bool> done_{true};
    bool reported_ = false;
    int exit_code_ = 0;
    std::thread reader_;
    std::mutex mutex_;
    std::deque<std::string> lines_;
};

// ---- File browser (works with a controller and inside Game Mode, where system dialogs may not show) -------------

struct Browser {
    bool open = false;
    int target = 0;  // 0 game, 1 keys, 2 update
    std::string dir;
    std::vector<std::string> exts;
    std::vector<std::pair<std::string, bool>> entries;  // name, is_dir
    std::string error;
    bool focus_first = false;

    void load(const std::string& path) {
        DIR* d = opendir(path.c_str());
        if (!d) { error = "Cannot open " + path; return; }
        error.clear();
        dir = path;
        entries.clear();
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == ".." || name[0] == '.') continue;
            std::string full = dir + (dir == "/" ? "" : "/") + name;
            struct stat st{};
            if (stat(full.c_str(), &st) != 0) continue;
            bool is_dir = S_ISDIR(st.st_mode);
            if (!is_dir) {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                bool match = exts.empty();
                for (auto& x : exts)
                    if (lower.size() >= x.size() && lower.compare(lower.size() - x.size(), x.size(), x) == 0) match = true;
                if (!match) continue;
            }
            entries.emplace_back(name, is_dir);
        }
        closedir(d);
        std::sort(entries.begin(), entries.end(), [](auto& x, auto& y) {
            if (x.second != y.second) return x.second;
            return strcasecmp(x.first.c_str(), y.first.c_str()) < 0;
        });
        focus_first = true;
    }
};

std::string home() {
    const char* h = getenv("HOME");
    return h && *h ? h : "/";
}

std::string parent_of(const std::string& path) {
    if (path == "/") return path;
    auto slash = path.find_last_of('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

bool is_dir(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

struct FileState {
    std::string path;
    bool ok = false;
    std::string message = "not chosen yet";
};

struct App {
    Task task;
    std::string task_kind;  // "", "status", "check", "prepare", "steam", "desktop", "play"
    Json status;
    bool have_status = false;
    FileState files[3];
    std::string stage = "";
    float progress = -1;
    std::string progress_text;
    std::deque<std::string> log;
    std::string result_message;
    bool result_ok = true;
    bool steam_confirm = false;
    bool quit = false;
    Browser browser;
    std::deque<std::vector<std::string>> queue;  // commands waiting for the running one
    std::string system_dialog_pick;  // set from SDL's dialog callback
    int system_dialog_target = -1;
    std::mutex dialog_mutex;

    void run(const std::string& kind, std::vector<std::string> args) {
        if (task.running()) { queue.push_back(args); queue.back().insert(queue.back().begin(), kind); return; }
        task_kind = kind;
        args.insert(args.begin(), kind);
        task.reset_report();
        if (kind != "status" && kind != "check") { result_message.clear(); progress = -1; stage.clear(); }
        if (!task.start(args)) {
            result_ok = false;
            result_message = "Could not start python3 (" + g_here + "/tools/linux_setup.py).";
            task_kind.clear();
        }
    }
    void refresh() { run("status", {}); }
    void check_files() {
        run("check", {"--nsp", files[0].path, "--keys", files[1].path, "--update", files[2].path});
    }

    void handle(const std::string& line) {
        Json j;
        if (!parse_json(line, j) || j.kind != Json::Object) {
            if (!line.empty()) push_log(line);
            return;
        }
        std::string type = j["type"].str();
        if (type == "status") {
            bool first = !have_status;
            status = j;
            have_status = true;
            const char* keys[3] = {"nsp", "keys", "update"};
            for (int i = 0; i < 3; ++i) {
                const Json& f = j["files"][keys[i]];
                if (first) {  // later refreshes keep what the player chose since
                    files[i].path = f["path"].str();
                    files[i].ok = f["ok"].truthy();
                    files[i].message = f["message"].str();
                }
            }
        } else if (type == "stage") {
            stage = j["text"].str();
            push_log("== " + stage);
        } else if (type == "progress") {
            progress = float(j["fraction"].n);
            progress_text = j["text"].str();
        } else if (type == "log") {
            push_log(j["text"].str());
        } else if (type == "result") {
            if (task_kind == "check") {
                const char* keys[3] = {"nsp", "keys", "update"};
                for (int i = 0; i < 3; ++i) {
                    const Json& f = j["data"]["files"][keys[i]];
                    files[i].ok = f["ok"].truthy();
                    files[i].message = f["message"].str();
                }
                return;
            }
            result_ok = j["ok"].truthy();
            result_message = j["message"].str();
            push_log(result_message);
        }
    }
    void push_log(const std::string& text) {
        log.push_back(text);
        while (log.size() > 300) log.pop_front();
    }

    void poll() {
        bool finished = false;
        int code = 0;
        for (auto& line : task.take(finished, code)) handle(line);
        if (finished) {
            std::string kind = task_kind;
            task_kind.clear();
            if (kind != "status" && kind != "check") {
                progress = -1;
                refresh();
            }
            if (!queue.empty()) {
                auto next = queue.front();
                queue.pop_front();
                std::string k = next.front();
                next.erase(next.begin());
                run(k, next);
            }
        }
        std::lock_guard lock(dialog_mutex);
        if (system_dialog_target >= 0) {
            files[system_dialog_target].path = system_dialog_pick;
            system_dialog_target = -1;
            check_files();
        }
    }
};

App* g_app = nullptr;

void SDLCALL dialog_done(void* user, const char* const* list, int) {
    auto* app = static_cast<App*>(user);
    if (!list || !list[0]) return;
    std::lock_guard lock(app->dialog_mutex);
    app->system_dialog_pick = list[0];
    app->system_dialog_target = app->browser.target;
}

const char* kLabels[3] = {"Game (.nsp)", "Keys (prod.keys)", "Update 1.0.1 (optional)"};

void open_browser(App& app, int target) {
    Browser& b = app.browser;
    b.target = target;
    b.exts = target == 0 ? std::vector<std::string>{".nsp"} : target == 1 ? std::vector<std::string>{".keys"}
                                                                          : std::vector<std::string>{".nsp", ".nsz"};
    std::string start = app.files[target].path.empty() ? "" : parent_of(app.files[target].path);
    if (start.empty() || !is_dir(start)) {
        for (int i = 0; i < 3 && start.empty(); ++i)
            if (!app.files[i].path.empty() && is_dir(parent_of(app.files[i].path))) start = parent_of(app.files[i].path);
    }
    if (start.empty() || !is_dir(start)) start = home();
    b.load(start);
    b.open = true;
}

void draw_browser(App& app, SDL_Window* window) {
    Browser& b = app.browser;
    if (b.open) {
        ImGui::OpenPopup("Choose a file");
        b.open = false;
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.05f, vp->WorkPos.y + vp->WorkSize.y * 0.05f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.9f, vp->WorkSize.y * 0.9f));
    if (!ImGui::BeginPopupModal("Choose a file", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;
    ImGui::TextWrapped("%s: pick the file, or open a folder. B / Esc closes.", kLabels[b.target]);
    ImGui::TextDisabled("%s", b.dir.c_str());
    // Places: home folders, SD cards and USB drives (/run/media/<user>/...), /mnt.
    std::vector<std::pair<std::string, std::string>> places{{"Home", home()}};
    for (const char* sub : {"Downloads", "Documents", "Desktop", "Games"})
        if (is_dir(home() + "/" + sub)) places.emplace_back(sub, home() + "/" + sub);
    const char* user = getenv("USER");
    std::string media = std::string("/run/media/") + (user ? user : "");
    if (DIR* d = opendir(media.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') places.emplace_back(e->d_name, media + "/" + e->d_name);
        closedir(d);
    }
    if (is_dir("/mnt")) places.emplace_back("/mnt", "/mnt");
    places.emplace_back("/", "/");
    if (ImGui::Button("Up")) b.load(parent_of(b.dir));
    for (auto& [name, path] : places) {
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(name.c_str()).x + 40) ImGui::NewLine();
        if (ImGui::Button(name.c_str())) b.load(path);
    }
    if (!b.error.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", b.error.c_str());
    float footer = ImGui::GetFrameHeightWithSpacing() * 1.4f;
    ImGui::BeginChild("entries", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    std::string chosen_dir, chosen_file;
    if (b.entries.empty()) ImGui::TextDisabled("No folders or matching files here.");
    for (size_t i = 0; i < b.entries.size(); ++i) {
        auto& [name, dir] = b.entries[i];
        std::string label = (dir ? "[folder]  " : "") + name;
        if (b.focus_first && i == 0) { ImGui::SetKeyboardFocusHere(); b.focus_first = false; }
        if (ImGui::Selectable(label.c_str(), false))
            (dir ? chosen_dir : chosen_file) = b.dir + (b.dir == "/" ? "" : "/") + name;
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Use the system file dialog")) {
        static SDL_DialogFileFilter filters[3][1] = {{{"Game NSP", "nsp"}}, {{"prod.keys", "keys"}},
                                                     {{"Update NSP/NSZ", "nsp;nsz"}}};
        SDL_ShowOpenFileDialog(dialog_done, &app, window, filters[b.target], 1, b.dir.c_str(), false);
        ImGui::CloseCurrentPopup();
    }
    if (!chosen_dir.empty()) b.load(chosen_dir);
    if (!chosen_file.empty()) {
        app.files[b.target].path = chosen_file;
        app.check_files();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void section(const char* title) {
    ImGui::Spacing();
    ImGui::SeparatorText(title);
}

void draw(App& app, SDL_Window* window) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("HWDER Setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    const Json& st = app.status;
    bool busy = app.task.running() && app.task_kind != "status" && app.task_kind != "check";
    ImGui::Text("HWDER Setup");
    ImGui::SameLine();
    ImGui::TextDisabled("%s  -  Hyrule Warriors: Definitive Edition for Linux", st["version"].str().c_str());
    ImGui::TextWrapped("Builds the game on this PC from your own game dump and keys. Your files are read where they "
                       "are and never copied. Saves: %s/save", st["data_dir"].str().c_str());

    section("1  Your files");
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        FileState& f = app.files[i];
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Choose...")) open_browser(app, i);
        if (i == 2 && !f.path.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Remove")) { f.path.clear(); app.check_files(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("%s:", kLabels[i]);
        ImGui::SameLine();
        if (f.path.empty())
            ImGui::TextColored(i == 2 ? ImVec4(0.6f, 0.6f, 0.6f, 1) : ImVec4(1, 0.75f, 0.3f, 1), "%s",
                               i == 2 ? "none (base game)" : "not chosen yet");
        else {
            ImGui::TextColored(f.ok ? ImVec4(0.45f, 0.9f, 0.45f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s %s",
                               f.ok ? "OK" : "Problem:", f.message.c_str());
            ImGui::TextDisabled("    %s", f.path.c_str());
        }
        ImGui::PopID();
    }

    section("2  Prepare the game");
    bool files_ok = app.files[0].ok && app.files[1].ok && (app.files[2].path.empty() || app.files[2].ok);
    bool ready = st["game_ready"].truthy();
    if (busy) {
        ImGui::TextWrapped("%s", app.stage.c_str());
        if (app.progress >= 0)
            ImGui::ProgressBar(app.progress, ImVec2(-1, 0), app.progress_text.c_str());
        if (app.task_kind == "prepare" && ImGui::Button("Stop")) app.task.stop();
    } else {
        if (ready && st["rebuild_needed"].truthy())
            ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "This release is newer than your build (%s): prepare again.",
                               st["built_version"].str().c_str());
        else if (ready)
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "The game is ready%s.",
                               st["update_in_build"].truthy() ? " (with update 1.0.1)" : "");
        else
            ImGui::TextWrapped("Takes about 15 minutes on a ROG Ally. The first time, the build tools are downloaded "
                               "too (about 200 MB, each file checked).");
        ImGui::BeginDisabled(!files_ok || st["game_running"].truthy());
        if (ImGui::Button(ready ? "Prepare again" : "Prepare the game")) {
            std::vector<std::string> args{"--nsp", app.files[0].path, "--keys", app.files[1].path};
            if (!app.files[2].path.empty()) { args.push_back("--update"); args.push_back(app.files[2].path); }
            app.log.clear();
            app.run("prepare", args);
        }
        ImGui::EndDisabled();
        if (!files_ok) { ImGui::SameLine(); ImGui::TextDisabled("Choose the game and keys first."); }
        if (st["game_running"].truthy()) { ImGui::SameLine(); ImGui::TextDisabled("Close HWDER first."); }
    }
    if (!app.result_message.empty() && !busy)
        ImGui::TextColored(app.result_ok ? ImVec4(0.45f, 0.9f, 0.45f, 1) : ImVec4(1, 0.45f, 0.4f, 1), "%s",
                           app.result_message.c_str());

    section("3  Play");
    ImGui::BeginDisabled(!ready || busy);
    if (ImGui::Button("Play")) app.run("play", {});
    ImGui::SameLine();
    const Json& steam = st["steam"];
    if (ImGui::Button(steam["added"].truthy() ? "Update Steam entry" : "Add to Steam")) {
        if (steam["running"].truthy()) app.steam_confirm = true;
        else app.run("steam", {"--yes"});
    }
    ImGui::SameLine();
    if (ImGui::Button(st["launchers"]["desktop"].truthy() ? "Desktop icon: done" : "Desktop icon"))
        app.run("desktop", {"--desktop-icon"});
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close")) app.quit = true;
    if (ready) {
        std::string where = "In your application menu";
        if (steam["added"].truthy()) where += " and in Steam (also in Game Mode)";
        else if (!steam["installed"].truthy()) where += " (Steam was not found)";
        ImGui::TextDisabled("%s. Run HWDER Setup again to update, change files or rebuild.", where.c_str());
        ImGui::TextDisabled("Saves: %s/save    Mods: %s", st["data_dir"].str().c_str(), st["mods_dir"].str().c_str());
    }

    if (app.steam_confirm) { ImGui::OpenPopup("Add to Steam"); app.steam_confirm = false; }
    if (ImGui::BeginPopupModal("Add to Steam", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(vp->WorkSize.x * 0.7f);
        ImGui::TextWrapped("Steam is running. It keeps its game list in memory and rewrites it when it exits, so it "
                           "has to close before HWDER can be added.");
        ImGui::TextWrapped(steam["game_mode"].truthy()
            ? "In Game Mode Steam restarts by itself; HWDER is in your library when it is back. Setup keeps "
              "running; if the screen goes back to Steam, you are done."
            : "Setup closes Steam, adds HWDER and starts Steam again.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Close Steam and add")) { app.run("steam", {"--yes"}); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    draw_browser(app, window);

    section("Details");
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& line : app.log) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

std::string executable_dir() {
    char buffer[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n <= 0) return ".";
    buffer[n] = 0;
    std::string exe = buffer;
    return parent_of(parent_of(exe));  // <release>/setup/hwder-setup-ui
}

}  // namespace

int main(int, char**) {
    g_here = executable_dir();
    unsetenv("LD_PRELOAD");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "HWDER Setup: %s\nRun it in a terminal instead: %s/hwder-setup --cli\n", SDL_GetError(),
                g_here.c_str());
        return 3;
    }
    const char* desktop = getenv("XDG_CURRENT_DESKTOP");
    bool game_mode = getenv("GAMESCOPE_WAYLAND_DISPLAY") || (desktop && !strcasecmp(desktop, "gamescope"));
    SDL_Window* window = SDL_CreateWindow("HWDER Setup", 1280, 800,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                          (game_mode ? SDL_WINDOW_FULLSCREEN : 0));
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        fprintf(stderr, "HWDER Setup: %s\nRun it in a terminal instead: %s/hwder-setup --cli\n", SDL_GetError(),
                g_here.c_str());
        return 3;
    }
    SDL_SetRenderVSync(renderer, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void*)kFontData, int(sizeof kFontData), 22.0f, &font_config);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FrameRounding = 6;
    style.FramePadding = ImVec2(14, 8);
    style.ItemSpacing = ImVec2(12, 10);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    App app;
    g_app = &app;
    app.refresh();
    while (!app.quit) {
        SDL_Event event;
        bool waited = SDL_WaitEventTimeout(&event, 50);
        while (waited || SDL_PollEvent(&event)) {
            waited = false;
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) app.quit = true;
        }
        app.poll();
        // Text grows with the window: readable on a 7" handheld and on a TV.
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        style.FontScaleMain = std::clamp(std::min(w / 1280.0f, h / 800.0f), 0.8f, 2.0f);
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw(app, window);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 22, 26, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    if (app.task.running() && app.task_kind == "prepare") app.task.stop();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
