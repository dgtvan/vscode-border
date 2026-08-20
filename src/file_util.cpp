#include "file_util.h"
#include "logger.h"

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}

std::string ReadFileBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();

    DWORD size = GetFileSize(h, nullptr);
    std::string buf;
    if (size != INVALID_FILE_SIZE && size > 0) {
        buf.resize(size);
        DWORD bytesRead = 0;
        ReadFile(h, &buf[0], size, &bytesRead, nullptr);
        buf.resize(bytesRead);
    }
    CloseHandle(h);
    return buf;
}

bool WriteFileBytes(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"file_util: failed to open %ls for write, lastError=%lu", path.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != content.size()) {
        Log(L"file_util: failed to write %ls, lastError=%lu", path.c_str(), GetLastError());
        return false;
    }
    return true;
}
