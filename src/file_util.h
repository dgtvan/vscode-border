#pragma once

#include <windows.h>

#include <string>

// Directory containing the running exe (no trailing slash) -- where
// config.ini, the log file, and other app-local files live.
std::wstring GetExeDir();

// Reads the full contents of `path` into a string. Returns an empty string
// if the file doesn't exist or is empty -- callers can't distinguish
// "missing" from "empty" from the return value alone.
std::string ReadFileBytes(const std::wstring& path);
