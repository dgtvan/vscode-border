#include "window_title.h"
#include "logger.h"

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
            // Not normally reachable -- the no-repo case collapses to 2
            // segments (see below), not 3 with literal "-" placeholders.
            // Seeing this shape suggests window.title isn't the template
            // this parser expects, e.g. a workspace-level override.
            LogWarn(L"window_title: unexpected 3-segment placeholder shape, title=[%ls]", title.c_str());
            parts.folder = segments[2];
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
        // More than 3 " - "-separated segments doesn't match this app's
        // template or VS Code's un-customized default at all -- most
        // likely a workspace-level window.title override (see README.md)
        // is taking priority over the global setting this app manages, so
        // the label below will just show the raw title instead of
        // repo/branch.
        LogWarn(L"window_title: title doesn't match any recognized shape (%zu segments), title=[%ls] -- "
                L"possibly a workspace-level window.title override",
                segments.size(), title.c_str());
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
