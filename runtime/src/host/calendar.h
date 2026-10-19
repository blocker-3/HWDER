// Reentrant calendar conversions; UTC interpretation must not depend on host TZ.
#pragma once
#include <ctime>

namespace host {
inline bool calendar_year_fits_guest(const std::tm& value) {
    // Guest CalendarTime stores the full year in u16; avoid overflowing tm_year + 1900.
    return value.tm_year >= -1900 && value.tm_year <= 65535 - 1900;
}
inline bool calendar_breakdown(std::time_t seconds, bool local, std::tm& out) {
    out = {};
#ifdef _WIN32
    return (local ? localtime_s(&out, &seconds) : gmtime_s(&out, &seconds)) == 0;
#else
    return (local ? localtime_r(&seconds, &out) : gmtime_r(&seconds, &out)) != nullptr;
#endif
}
inline std::time_t calendar_as_utc(std::tm value) {
#ifdef _WIN32
    return _mkgmtime(&value);
#else
    return timegm(&value);
#endif
}
// Retain the service's standard-time bias (not the current daylight-saving bias).
inline long standard_utc_offset() {
#ifdef _WIN32
    long west = 0;
    _get_timezone(&west);
    return -west;
#else
    tzset();
    return -::timezone;
#endif
}
}  // namespace host
