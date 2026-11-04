#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Icon size and trailing gap on window labels and hub items.
const int kProfileIconSize = 14, kProfileIconGap = 4;

// Shared Claude/Codex icon rendering: config-relative image files, base64
// images, or a letter badge. Pixels are premultiplied ARGB.
std::wstring ResolveProfileIconSource(const std::wstring& icon);
std::wstring DescribeProfileIconSource(const std::wstring& source);
std::vector<UINT32> RenderProfileIcon(const std::wstring& name, const std::wstring& source, int size);
