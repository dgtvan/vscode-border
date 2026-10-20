#pragma once

#include <windows.h>

#include <string>

COLORREF ContrastTextColor(COLORREF bg);

void BlendTextIntoPixels(HDC screenDC, UINT32* pixels, int surfaceWidth, int surfaceHeight,
                         int x, int y, int width, int height, const std::wstring& text,
                         HFONT font, COLORREF bgColor, COLORREF textColor, UINT format);

// Composites a `size`-square premultiplied-ARGB icon onto an opaque 32bpp
// surface with its top-left at (left, top), clipped to `clip` and to the
// surface.
void BlendPremultipliedIcon(const UINT32* icon, int size, UINT32* pixels, int surfaceWidth, int surfaceHeight,
                            int left, int top, const RECT& clip);