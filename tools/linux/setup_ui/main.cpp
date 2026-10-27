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

// ---- Files: the system file dialog (xdg-desktop-portal: KDE's or GNOME's own picker), typing as the fallback -----

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
    std::deque<std::vector<std::string>> queue;  // commands waiting for the running one
    // The dialog answers on another thread: its result waits here for the UI thread.
    std::mutex dialog_mutex;
    bool dialog_open = false;
    int dialog_target = -1;          // set with dialog_pick when a file was chosen
    std::string dialog_pick;
    int dialog_failed_target = -1;   // the system dialog could not open: type the path instead
    std::string dialog_error;
    bool typing[3] = {false, false, false};
    char typed[3][1024] = {};

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
        if (dialog_target >= 0) {
            files[dialog_target].path = dialog_pick;
            typing[dialog_target] = false;
            dialog_target = -1;
            check_files();
        }
        if (dialog_failed_target >= 0) {
            int i = dialog_failed_target;
            typing[i] = true;
            snprintf(typed[i], sizeof typed[i], "%s", files[i].path.c_str());
            push_log("The system file dialog is not available (" + dialog_error + "); type the path instead.");
            dialog_failed_target = -1;
        }
    }
};

App* g_app = nullptr;
ImVec2 g_probe_min, g_probe_max;  // the Close button, for HWDER_SETUP_SELFTEST
bool g_probe_hovered = false;

struct DialogRequest {
    App* app;
    int target;
};

void SDLCALL dialog_done(void* user, const char* const* list, int) {
    std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(user));
    App* app = request->app;
    std::lock_guard lock(app->dialog_mutex);
    app->dialog_open = false;
    if (!list) {  // no portal and no zenity
        app->dialog_failed_target = request->target;
        app->dialog_error = SDL_GetError();
    } else if (list[0]) {
        app->dialog_pick = list[0];
        app->dialog_target = request->target;
    }  // else: cancelled
}

const char* kLabels[3] = {"Game (.nsp)", "Keys (prod.keys)", "Update 1.0.1 (optional)"};

// Starts where the file already is, else in ~/Documents/HWDER-files (where the Ally's files live) when it exists,
// else next to another chosen file, else the home folder.
std::string dialog_folder(const App& app, int target) {
    std::vector<std::string> candidates;
    if (!app.files[target].path.empty()) candidates.push_back(parent_of(app.files[target].path));
    candidates.push_back(home() + "/Documents/HWDER-files");
    for (auto& f : app.files)
        if (!f.path.empty()) candidates.push_back(parent_of(f.path));
    for (auto& c : candidates)
        if (is_dir(c)) return c;
    return home();
}

void choose_file(App& app, SDL_Window* window, int target) {
    static const SDL_DialogFileFilter filters[3][2] = {
        {{"Game (.nsp)", "nsp"}, {"All files", "*"}},
        {{"Keys (.keys)", "keys"}, {"All files", "*"}},
        {{"Update (.nsp, .nsz)", "nsp;nsz"}, {"All files", "*"}}};
    {
        std::lock_guard lock(app.dialog_mutex);
        if (app.dialog_open) return;
        app.dialog_open = true;
    }
    std::string folder = dialog_folder(app, target) + "/";
    SDL_ShowOpenFileDialog(dialog_done, new DialogRequest{&app, target}, window, filters[target], 2, folder.c_str(),
                           false);
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
        if (ImGui::Button("Choose...")) choose_file(app, window, i);
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
        if (app.typing[i]) {  // the system dialog could not open
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.75f);
            bool enter = ImGui::InputTextWithHint("##path", "Type the full path of the file", app.typed[i],
                                                  sizeof app.typed[i], ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::Button("Use") || enter) {
                f.path = app.typed[i];
                app.check_files();
            }
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
        if (!ready) ImGui::SetItemDefaultFocus();  // the controller starts on the next step
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
    if (ready) ImGui::SetItemDefaultFocus();
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
    g_probe_min = ImGui::GetItemRectMin();
    g_probe_max = ImGui::GetItemRectMax();
    g_probe_hovered = ImGui::IsItemHovered();
    if (ready) {
        std::string where = "In your application menu";
        if (steam["added"].truthy()) where += " and in Steam (also in Game Mode)";
        else if (!steam["installed"].truthy()) where += " (Steam was not found)";
        ImGui::TextDisabled("%s. Run HWDER Setup again to update, change files or rebuild.", where.c_str());
        ImGui::TextDisabled("Saves: %s/save", st["data_dir"].str().c_str());
        ImGui::TextDisabled("Mods:  %s", st["mods_dir"].str().c_str());
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
    // No SDL_WINDOW_HIGH_PIXEL_DENSITY: with it, KDE's fractional scaling (150%) put the drawing and the pointer in
    // different places (Max had to point below a button). Without it the window is one coordinate space and the
    // compositor scales it; HWDER_SETUP_HIDPI=1 turns it back on for tests.
    SDL_Window* window = SDL_CreateWindow("HWDER Setup", 1280, 800,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN |
                                          (getenv("HWDER_SETUP_HIDPI") ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0) |
                                          (game_mode ? SDL_WINDOW_FULLSCREEN : 0));
    // Where the compositor does not scale us (X11/XWayland with a DPI setting), grow by the display scale; never
    // larger than the screen.
    float display_scale = window && !getenv("HWDER_SETUP_HIDPI") ? std::clamp(SDL_GetWindowDisplayScale(window), 1.0f, 3.0f) : 1.0f;
    SDL_Rect usable{};
    if (window && !game_mode && SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(window), &usable)) {
        int w = std::min(int(1280 * display_scale), int(usable.w * 0.95f));
        int h = std::min(int(800 * display_scale), int(usable.h * 0.92f));
        SDL_SetWindowSize(window, w, h);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (window) SDL_ShowWindow(window);
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
    style.ScaleAllSizes(display_scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    App app;
    g_app = &app;
    app.refresh();
    // Test hooks: HWDER_SETUP_SCREENSHOT=<file.bmp> saves the window once the status is shown, then quits.
    // HWDER_SETUP_SELFTEST=<prefix> also moves a synthetic pointer to the Close button's centre (window coordinates,
    // as the compositor reports a real pointer) and writes <prefix>.txt: sizes, scales and whether ImGui saw the hover.
    const char* screenshot = getenv("HWDER_SETUP_SCREENSHOT");
    const char* selftest = getenv("HWDER_SETUP_SELFTEST");
    std::string selftest_bmp = selftest ? std::string(selftest) + ".bmp" : "";
    if (selftest) screenshot = selftest_bmp.c_str();
    int settled_frames = 0;
    const int probe_at = getenv("HWDER_SETUP_SELFTEST_DIALOG") ? 150 : 10;  // the dialog answers asynchronously
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
        float fit = std::min(w / (1280.0f * display_scale), h / (800.0f * display_scale));
        style.FontScaleMain = std::clamp(fit, 0.75f, 2.0f) * display_scale;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw(app, window);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 22, 26, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (selftest && getenv("HWDER_SETUP_SELFTEST_DIALOG") && app.have_status && settled_frames == 2)
            choose_file(app, window, 0);  // without a session bus: must fall back to typing the path
        if (selftest && app.have_status && settled_frames == probe_at) {
            SDL_Event motion{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID = SDL_GetWindowID(window);
            motion.motion.x = (g_probe_min.x + g_probe_max.x) / 2;
            motion.motion.y = (g_probe_min.y + g_probe_max.y) / 2;
            SDL_PushEvent(&motion);
        }
        if (selftest && app.have_status && settled_frames == probe_at + 9) {
            int ww, wh, pw, ph, ow, oh;
            float sx, sy;
            SDL_GetWindowSize(window, &ww, &wh);
            SDL_GetWindowSizeInPixels(window, &pw, &ph);
            SDL_GetCurrentRenderOutputSize(renderer, &ow, &oh);
            SDL_GetRenderScale(renderer, &sx, &sy);
            if (FILE* f = fopen((std::string(selftest) + ".txt").c_str(), "w")) {
                fprintf(f, "driver %s\nwindow %dx%d points, %dx%d pixels, render output %dx%d, render scale %.3f\n"
                        "display scale %.3f, pixel density %.3f\nimgui display %.0fx%.0f, framebuffer scale %.3f\n"
                        "close button %.0f,%.0f-%.0f,%.0f, pointer %.0f,%.0f, hovered %d\n",
                        SDL_GetCurrentVideoDriver(), ww, wh, pw, ph, ow, oh, sx, SDL_GetWindowDisplayScale(window),
                        SDL_GetWindowPixelDensity(window), io.DisplaySize.x, io.DisplaySize.y,
                        io.DisplayFramebufferScale.x, g_probe_min.x, g_probe_min.y, g_probe_max.x, g_probe_max.y,
                        io.MousePos.x, io.MousePos.y, g_probe_hovered ? 1 : 0);
                fprintf(f, "dialog open %d, typing fallback %d %d %d, ui scale %.2f\n", app.dialog_open ? 1 : 0,
                        app.typing[0], app.typing[1], app.typing[2], display_scale);
                fclose(f);
            }
        }
        if (screenshot && app.have_status && !app.task.running() && ++settled_frames == probe_at + 10) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
                SDL_SaveBMP(shot, screenshot);
                SDL_DestroySurface(shot);
            }
            app.quit = true;
        }
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
