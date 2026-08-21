#pragma once

#include <string>

std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);

// Escapes backslash/newline/carriage-return so `w` can be embedded as one
// line of a line-based text file (label_aliases.ini, project_list_order.ini,
// ...) without corrupting the format. Does not escape "=" or other
// delimiters -- callers that use "=" as a field separator split on the
// first occurrence only (see label_alias.cpp), matching config.cpp's own
// key=value parsing. Operates on UTF-8 bytes, so it's safe for non-ASCII
// text (folder/repo/branch names).
std::string EscapeTextLine(const std::wstring& w);
std::wstring UnescapeTextLine(const std::string& s);
