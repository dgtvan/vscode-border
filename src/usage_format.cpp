#include "usage_format.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>

namespace {

long long UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return (long long)((t.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

SYSTEMTIME UnixToLocalTime(long long unix) {
    ULARGE_INTEGER t;
    t.QuadPart = (ULONGLONG)unix * 10000000ULL + 116444736000000000ULL;
    FILETIME utc = {t.LowPart, t.HighPart};
    SYSTEMTIME st = {}, local = {};
    FileTimeToSystemTime(&utc, &st);
    SystemTimeToTzSpecificLocalTime(nullptr, &st, &local);
    return local;
}

} // namespace

// "21:20", with the weekday in front ("Sat 07:00") unless it's today.
std::wstring FormatUsageLocalTime(long long unix) {
    static const wchar_t* const kDays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    SYSTEMTIME t = UnixToLocalTime(unix), today = UnixToLocalTime(UnixNow());
    bool sameDay = t.wYear == today.wYear && t.wMonth == today.wMonth && t.wDay == today.wDay;
    wchar_t buf[32];
    if (sameDay) swprintf(buf, 32, L"%02u:%02u", t.wHour, t.wMinute);
    else swprintf(buf, 32, L"%ls %02u:%02u", kDays[t.wDayOfWeek % 7], t.wHour, t.wMinute);
    return buf;
}

// "4h 51m", "4d 14h", "12m".
std::wstring FormatUsageDuration(long long seconds) {
    long long minutes = std::max(0LL, seconds) / 60;
    wchar_t buf[32];
    if (minutes >= 24 * 60) swprintf(buf, 32, L"%lldd %lldh", minutes / (24 * 60), minutes / 60 % 24);
    else if (minutes >= 60) swprintf(buf, 32, L"%lldh %lldm", minutes / 60, minutes % 60);
    else swprintf(buf, 32, L"%lldm", minutes);
    return buf;
}
