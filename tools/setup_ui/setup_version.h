// HWDER Setup.exe's own version, shown in its Windows version information (bootstrap.rc).
// It is separate from the HWDER release version on purpose: Setup links without timestamps, so
// an unchanged Setup is the same file, byte for byte, in every release, and keeps the antivirus
// reputation that exact file has earned. Bump the version whenever bootstrap.cpp,
// steam_shortcut.h, bootstrap.rc, bootstrap.manifest or hwder.ico change, then update the digest
// with: python tools/release_kit.py --setup-digest (release_kit refuses a stale digest).
#pragma once
#define HWDER_SETUP_VERSION_MAJOR 1
#define HWDER_SETUP_VERSION_MINOR 0
#define HWDER_SETUP_VERSION_PATCH 0
#define HWDER_SETUP_VERSION "1.0.0"
#define HWDER_SETUP_SOURCES_SHA256 "441a91617245860eee847488eaa3edf32ec9cd34cbac45d3e07231a28cfa7081"
