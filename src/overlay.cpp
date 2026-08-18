#include "overlay.h"

#include <algorithm>

const wchar_t* kOverlayClassName = L"VSCodeBorderOverlayWndClass";

HWND CreateOverlay(HINSTANCE hInstance) {
    return CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInstance, nullptr);
}

// Picks black or white text so the label stays readable against any
// palette color.
static COLORREF ContrastTextColor(COLORREF bg) {
    double luminance = 0.299 * GetRValue(bg) + 0.587 * GetGValue(bg) + 0.114 * GetBValue(bg);
    return luminance > 150.0 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

// Only the border bands (and label rect) are touched (not the whole
// width*height area) so resizing a large window stays cheap.
void PaintOverlay(HWND overlay, int width, int height, COLORREF color, int thickness, int opacity,
                   const std::wstring& label, bool showLabel, int labelHeight, int labelFontSize) {
    if (width <= 0 || height <= 0) return;
    int t = std::min(thickness, std::min(width, height) / 2);
    if (t < 1) t = 1;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);
    // No ZeroMemory here: CreateDIBSection always hands back a freshly
    // committed buffer, and Windows guarantees freshly committed pages are
    // zero-filled (never hands user-mode code another process's leftover
    // memory) -- an explicit clear would just be a second full-buffer pass
    // for no benefit, on what's already the most size-sensitive call here.

    BYTE a = (BYTE)opacity;
    BYTE r = (BYTE)((GetRValue(color) * a) / 255);
    BYTE g = (BYTE)((GetGValue(color) * a) / 255);
    BYTE b = (BYTE)((GetBValue(color) * a) / 255);
    UINT32 px = (UINT32(a) << 24) | (UINT32(r) << 16) | (UINT32(g) << 8) | UINT32(b);

    // Border bands.
    UINT32* pixels = (UINT32*)bits;
    for (int y = 0; y < t; y++)
        for (int x = 0; x < width; x++) pixels[y * width + x] = px;
    for (int y = height - t; y < height; y++)
        for (int x = 0; x < width; x++) pixels[y * width + x] = px;
    for (int y = t; y < height - t; y++) {
        for (int x = 0; x < t; x++) pixels[y * width + x] = px;
        for (int x = width - t; x < width; x++) pixels[y * width + x] = px;
    }

    // Label chip: sits just inside the border at the top-left corner,
    // overlapping the target window's own top-left corner content, drawn
    // fully opaque (independent of the border's `opacity`) so the folder
    // name stays legible even when the border itself is faint. Known
    // trade-off: this can occasionally blink -- see docs/ARCHITECTURE.md's
    // "Known issue: label can occasionally blink".
    if (showLabel && !label.empty() && labelHeight > 0) {
        int lh = std::min(labelHeight, height - 2 * t);
        if (lh > 4) {
            HFONT font = CreateFontW(-labelFontSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT oldFont = (HFONT)SelectObject(memDC, font);

            SIZE textSz = {0, 0};
            GetTextExtentPoint32W(memDC, label.c_str(), (int)label.size(), &textSz);
            SelectObject(memDC, oldFont);
            const int paddingX = 10;
            int lw = std::min(width - 2 * t, (int)textSz.cx + paddingX * 2);
            if (lw > 4) {
                int lx = t, ly = t;
                COLORREF textColor = ContrastTextColor(color);
                BYTE bgR = GetRValue(color), bgG = GetGValue(color), bgB = GetBValue(color);
                BYTE txR = GetRValue(textColor), txG = GetGValue(textColor), txB = GetBValue(textColor);
                UINT32 lpx = (UINT32(255) << 24) | (UINT32(bgR) << 16) | (UINT32(bgG) << 8) | UINT32(bgB);
                for (int y = ly; y < ly + lh; y++)
                    for (int x = lx; x < lx + lw; x++) pixels[y * width + x] = lpx;

                // GDI's ClearType/antialiased text rendering writes partial
                // coverage into the alpha byte of a 32bpp DIB, which would
                // make partially-covered glyph-edge pixels blend with
                // whatever is *behind* the overlay instead of the label
                // background -- looking transparent/washed-out on some
                // pixels while fully-covered ones stay solid. Render the
                // glyphs into an isolated black-background mask instead and
                // use its per-channel brightness as blend coverage, so the
                // label rect written above always stays fully opaque.
                HDC maskDC = CreateCompatibleDC(screenDC);
                BITMAPINFO maskBmi = {};
                maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                maskBmi.bmiHeader.biWidth = lw;
                maskBmi.bmiHeader.biHeight = -lh;
                maskBmi.bmiHeader.biPlanes = 1;
                maskBmi.bmiHeader.biBitCount = 32;
                maskBmi.bmiHeader.biCompression = BI_RGB;
                void* maskBits = nullptr;
                HBITMAP maskBmp = CreateDIBSection(maskDC, &maskBmi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
                if (maskBmp) {
                    HBITMAP oldMaskBmp = (HBITMAP)SelectObject(maskDC, maskBmp);
                    // Freshly committed pages are zero-filled, giving a black background for free.
                    HFONT oldMaskFont = (HFONT)SelectObject(maskDC, font);
                    SetBkMode(maskDC, TRANSPARENT);
                    SetTextColor(maskDC, RGB(255, 255, 255));
                    RECT textRect = {paddingX, 0, lw - paddingX, lh};
                    DrawTextW(maskDC, label.c_str(), (int)label.size(), &textRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(maskDC, oldMaskFont);

                    UINT32* maskPixels = (UINT32*)maskBits;
                    for (int y = 0; y < lh; y++) {
                        for (int x = 0; x < lw; x++) {
                            UINT32 mp = maskPixels[y * lw + x];
                            BYTE covR = (BYTE)((mp >> 16) & 0xFF);
                            BYTE covG = (BYTE)((mp >> 8) & 0xFF);
                            BYTE covB = (BYTE)(mp & 0xFF);
                            if (covR == 0 && covG == 0 && covB == 0) continue; // no coverage here
                            BYTE outR = (BYTE)(bgR + ((int)(txR - bgR) * covR) / 255);
                            BYTE outG = (BYTE)(bgG + ((int)(txG - bgG) * covG) / 255);
                            BYTE outB = (BYTE)(bgB + ((int)(txB - bgB) * covB) / 255);
                            pixels[(ly + y) * width + (lx + x)] =
                                (UINT32(255) << 24) | (UINT32(outR) << 16) | (UINT32(outG) << 8) | UINT32(outB);
                        }
                    }

                    SelectObject(maskDC, oldMaskBmp);
                    DeleteObject(maskBmp);
                }
                DeleteDC(maskDC);
            }

            DeleteObject(font);
        }
    }

    POINT ptSrc = {0, 0};
    SIZE sz = {width, height};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(overlay, screenDC, nullptr, &sz, memDC, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}
