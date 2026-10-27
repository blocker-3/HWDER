// HWDER runtime services shared by the core, kernel and services.
#pragma once
#include <stdarg.h>
#include "hwder/cpu.h"
#include "hwder/dispatch.h"

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

void hw_log(const char* fmt, ...);
void hw_fatal(const char* why);
void hw_sleep_ns(s64 ns);
__attribute__((noreturn)) void hw_exit_process(int code);

// Build identity (runtime/src/version.cpp): git describe, short commit, build date.
const char* hw_version(void);
const char* hw_commit(void);
const char* hw_build_date(void);

#ifdef __cplusplus
}
void hw_config_load(const std::string& dir);
const std::string& hw_romfs_path();
#endif

#define GUESTP(T, a) ((T*)(uintptr_t)(a))
