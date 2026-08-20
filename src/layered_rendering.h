#pragma once

#include <windows.h>

#include <string>

COLORREF ContrastTextColor(COLORREF bg);

void BlendTextIntoPixels(HDC screenDC, UINT32* pixels, int surfaceWidth, int surfaceHeight,
                         int x, int y, int width, int height, const std::wstring& text,
                         HFONT font, COLORREF bgColor, COLORREF textColor, UINT format);