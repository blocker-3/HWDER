// Public option and calendar contracts. Each CTest process starts with its own TZ.
#include "host/ascii.h"
#include "host/calendar.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <initializer_list>

static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv) {
    check(argc == 3, "timezone and expected standard offset arguments");
#ifdef _WIN32
    check(_putenv_s("TZ", argv[1]) == 0, "set isolated timezone");
    _tzset();
#else
    check(setenv("TZ", argv[1], 1) == 0, "set isolated timezone");
    tzset();
#endif
    for (unsigned mask = 0; mask < 64; ++mask) {
        char option[] = "camera";
        for (unsigned i = 0; i < 6; ++i) if (mask & (1u << i)) option[i] -= 'a' - 'A';
        check(host::ascii_iequals(option, "camera"), "all 64 camera case combinations");
    }
    const char* rejected[] = {nullptr, "", "cam", "camer", "cameras", " camera", "camera ", "camera\n", "game", "camer\xE1"};
    for (const char* value : rejected)
        check(!host::ascii_iequals(value, "camera"), "reject absent, partial and unrelated option tokens");
    struct Fixture { std::time_t seconds; int year, month, day, hour, minute, second, weekday, yearday; };
    const Fixture fixtures[] = {
        {0, 1970, 1, 1, 0, 0, 0, 4, 0},
        {951827696, 2000, 2, 29, 12, 34, 56, 2, 59},
        {1704067200, 2024, 1, 1, 0, 0, 0, 1, 0},
        {2147483648LL, 2038, 1, 19, 3, 14, 8, 2, 18},
        {4107542400LL, 2100, 3, 1, 0, 0, 0, 1, 59},
    };
    for (const auto& f : fixtures) {
        std::tm calendar{};
        check(host::calendar_breakdown(f.seconds, false, calendar), "UTC breakdown succeeds");
        check(calendar.tm_year + 1900 == f.year && calendar.tm_mon + 1 == f.month && calendar.tm_mday == f.day &&
              calendar.tm_hour == f.hour && calendar.tm_min == f.minute && calendar.tm_sec == f.second &&
              calendar.tm_wday == f.weekday && calendar.tm_yday == f.yearday && calendar.tm_isdst == 0,
              "UTC fields equal independent epoch/leap/year-2038 fixtures");
        check(host::calendar_as_utc(calendar) == f.seconds, "calendar interpreted as UTC under every host TZ");
    }
    check(host::standard_utc_offset() == std::strtol(argv[2], nullptr, 10), "standard timezone offset sign");
    std::tm local{};
    check(host::calendar_breakdown(1704110400, true, local), "local breakdown succeeds"); // 2024-01-01 12:00 UTC
    check(host::calendar_as_utc(local) - 1704110400 == host::standard_utc_offset(), "local calendar and standard offset agree");
    std::tm invalid{};
    check(!host::calendar_breakdown(std::numeric_limits<std::time_t>::max(), false, invalid), "reject unrepresentable calendar");
    for (int year : {-1901, -1900, 65535 - 1900, 65536 - 1900, std::numeric_limits<int>::max()}) {
        std::tm boundary{};
        boundary.tm_year = year;
        check(host::calendar_year_fits_guest(boundary) == (year == -1900 || year == 65535 - 1900), "guest calendar year boundaries");
    }
#ifndef _WIN32
    std::tm large{};
    check(host::calendar_breakdown(2005949145600LL, false, large) && !host::calendar_year_fits_guest(large), "Linux year 65536 cannot wrap guest u16");
#endif
    std::puts("option and calendar contracts passed (64 case variants, 5 independent UTC fixtures, local offset, overflow)");
}
