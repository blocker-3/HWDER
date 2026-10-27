// Exercise production settings loading in a disposable CTest working directory.
#include "gpu/settings.h"
#include "hid/host_input.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hid {
void default_remap(int* out, int, bool) {
    for (int i = 0; i < settings::kRemapSlots; ++i) out[i] = -1;
}
}

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() {
#ifdef _WIN32
    check(_putenv_s("HWDER_SETTINGS_TEST_ABSENT", "") == 0, "clear absent switch");
    check(_putenv_s("HWDER_SETTINGS_TEST_EXTERNAL", "external") == 0, "set external switch");
    check(_putenv_s("HWDER_SYNC_PIPELINES", "0") == 0, "set external pipeline preference");
#else
    check(unsetenv("HWDER_SETTINGS_TEST_ABSENT") == 0, "clear absent switch");
    check(setenv("HWDER_SETTINGS_TEST_EXTERNAL", "external", 1) == 0, "set external switch");
    check(setenv("HWDER_SETTINGS_TEST_EMPTY", "", 1) == 0, "set explicitly empty switch");
    check(setenv("HWDER_SYNC_PIPELINES", "0", 1) == 0, "set external pipeline preference");
#endif
    FILE* f = std::fopen("hwder_settings.ini", "w");
    check(f != nullptr, "open isolated settings");
    std::fputs("version=3\nasync_shaders=0\ndebug_env=HWDER_SETTINGS_TEST_ABSENT,HWDER_SETTINGS_TEST_EXTERNAL,HWDER_SETTINGS_TEST_EMPTY\n", f);
    check(std::fclose(f) == 0, "write isolated settings");
    settings::load();
    auto equals = [](const char* name, const char* expected) {
        const char* value = std::getenv(name);
        return value && std::strcmp(value, expected) == 0;
    };
    check(equals("HWDER_SETTINGS_TEST_ABSENT", "1"), "absent configured switch becomes enabled");
    check(equals("HWDER_SETTINGS_TEST_EXTERNAL", "external"), "external switch value wins");
    check(equals("HWDER_SYNC_PIPELINES", "0"), "external pipeline preference wins");
#ifndef _WIN32
    check(equals("HWDER_SETTINGS_TEST_EMPTY", ""), "explicitly empty external value wins");
#endif
    check(std::remove("hwder_settings.ini") == 0, "remove test settings");
    std::puts("settings environment precedence passed");
}
