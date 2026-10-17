// Build identity, stamped by tools/version.cmake on every build.
#include "hwder/runtime.h"
#include "hwder_version.h"

extern "C" const char* hw_version() { return HWDER_VERSION; }
extern "C" const char* hw_commit() { return HWDER_COMMIT; }
extern "C" const char* hw_build_date() { return HWDER_BUILD_DATE; }
