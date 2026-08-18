#pragma once

#include <string>

struct VSCodeTitleParts {
    std::wstring repo;   // ${activeRepositoryName}, empty if not present/placeholder
    std::wstring branch; // ${activeRepositoryBranchName}, empty if not present/placeholder
    std::wstring folder; // ${folderName}, empty if not present
};

// VS Code's window.title is configured as
// "${activeRepositoryName} - ${activeRepositoryBranchName} - ${folderName}"
// -- when the window is inside a git repo, this yields 3 " - "-separated
// segments (repo, branch, folder). Outside a git repo, VS Code substitutes
// "-" for the empty repo/branch placeholders and then collapses/trims the
// resulting whitespace, so the title actually observed is "- - folder" (2
// segments after splitting) or, if there's no folder open either, just
// "- -" (1 segment).
//
// Also handles the default, un-customized VS Code title ("file - folder"
// or just "folder"), for windows where window.title hasn't been
// customized this way -- the leading "file" segment ends up in `folder`
// alongside the actual folder name in that case (there's no way to tell
// them apart from the title alone).
VSCodeTitleParts ParseVSCodeTitle(const std::wstring& title);

// Builds the border label text from parsed title parts: "repo - branch"
// when a repo is present (folder ignored), otherwise just the folder name.
std::wstring BuildFolderLabel(const VSCodeTitleParts& parts);

// Convenience: ParseVSCodeTitle + BuildFolderLabel.
std::wstring ExtractFolderName(const std::wstring& title);

