#include "text_util.h"

#include <windows.h>

#include <cctype>
#include <cstdlib>

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::string EscapeTextLine(const std::wstring& w) {
    std::string in = WideToUtf8(w);
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::wstring UnescapeTextLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') { out += '\n'; i++; continue; }
            if (n == 'r') { out += '\r'; i++; continue; }
            if (n == '\\') { out += '\\'; i++; continue; }
        }
        out += s[i];
    }
    return Utf8ToWide(out);
}

std::wstring FileUriToPath(const std::string& uri) {
    const std::string prefix = "file:///";
    if (uri.compare(0, prefix.size(), prefix) != 0) return L"";
    std::string decoded;
    decoded.reserve(uri.size() - prefix.size());
    for (size_t i = prefix.size(); i < uri.size(); i++) {
        if (uri[i] == '%' && i + 2 < uri.size() && isxdigit((unsigned char)uri[i + 1]) &&
            isxdigit((unsigned char)uri[i + 2])) {
            decoded.push_back((char)strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
            continue;
        }
        decoded.push_back(uri[i] == '/' ? '\\' : uri[i]);
    }
    return Utf8ToWide(decoded);
}
