#pragma once
#include <string>

// Shared tooltip/log formatting for all usage providers. Today's times omit
// the weekday; countdowns use compact days/hours/minutes, matching Claude.
std::wstring FormatUsageLocalTime(long long unixSeconds);
std::wstring FormatUsageDuration(long long seconds);
