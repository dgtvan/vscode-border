#include "window_title.h"

#include <vector>

static std::vector<std::wstring> SplitDashSeparated(const std::wstring& s) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(L" - ", start);
        if (pos == std::wstring::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 3;
    }
    return parts;
}

VSCodeTitleParts ParseVSCodeTitle(const std::wstring& title) {
    std::wstring t = title;
    static const wchar_t* kSuffixes[] = {
        L" - Visual Studio Code - Insiders",
        L" - Visual Studio Code",
    };
    for (const wchar_t* suf : kSuffixes) {
        size_t len = wcslen(suf);
        if (t.size() >= len && t.compare(t.size() - len, len, suf) == 0) {
            t = t.substr(0, t.size() - len);
            break;
        }
    }

    VSCodeTitleParts parts;
    std::vector<std::wstring> segments = SplitDashSeparated(t);
    if (segments.size() == 3) {
        if (segments[0] == L"-" && segments[1] == L"-") {
            parts.folder = segments[2]; // placeholder guard, not normally reachable
        } else {
            parts.repo = segments[0];
            parts.branch = segments[1];
        }
    } else if (segments.size() == 2) {
        // Either the default un-customized "file - folder" title, or this
        // repo/branch template with repo+branch collapsed into a single
        // "-" placeholder ("- - folder" -> ["-", "folder"]) -- either way
        // the folder/project name is the second segment.
        parts.folder = segments[1];
    } else if (segments.size() == 1) {
        if (segments[0] != L"- -") parts.folder = segments[0]; // "- -" = no repo, no folder open
    } else {
        parts.folder = t;
    }
    return parts;
}

std::wstring BuildFolderLabel(const VSCodeTitleParts& parts) {
    if (!parts.repo.empty()) return parts.repo + L" - " + parts.branch;
    return parts.folder;
}

std::wstring ExtractFolderName(const std::wstring& title) {
    return BuildFolderLabel(ParseVSCodeTitle(title));
}
