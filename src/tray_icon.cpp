#include "tray_icon.h"

#include <vector>

// Composites a yellow "!" glyph onto the bottom-right corner of baseIcon,
// returning a new icon -- used for the tray icon's warning badge (see
// UpdateTrayIconWarningState in vscode_border.cpp). baseIcon's own
// transparency is recovered by rendering it once over solid black and once
// over solid white and comparing the two: pixels where both renders match
// are opaque (composited color = either render), pixels that differ are
// transparent. This works regardless of whether the source icon carries a
// real alpha channel or (as assets/app.ico turned out to, confirmed by
// inspection) a classic 1-bit AND mask with an unused/zero alpha byte in its
// color bitmap -- DrawIconEx always composites correctly against a solid
// backdrop either way, so measuring the visible result sidesteps needing to
// know which format is behind it.
HICON CreateWarningBadgedIcon(HICON baseIcon, int size) {
    HDC screenDC = GetDC(nullptr);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    auto renderOverBg = [&](COLORREF bg, HBITMAP& outBmp, void*& outBits) {
        HDC dc = CreateCompatibleDC(screenDC);
        outBmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &outBits, nullptr, 0);
        HBITMAP old = (HBITMAP)SelectObject(dc, outBmp);
        RECT rc = {0, 0, size, size};
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(dc, &rc, brush);
        DeleteObject(brush);
        DrawIconEx(dc, 0, 0, baseIcon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        DeleteDC(dc);
    };

    HBITMAP blackBmp = nullptr, whiteBmp = nullptr;
    void* blackBits = nullptr;
    void* whiteBits = nullptr;
    renderOverBg(RGB(0, 0, 0), blackBmp, blackBits);
    renderOverBg(RGB(255, 255, 255), whiteBmp, whiteBits);
    if (!blackBmp || !whiteBmp) {
        if (blackBmp) DeleteObject(blackBmp);
        if (whiteBmp) DeleteObject(whiteBmp);
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    HDC memDC = CreateCompatibleDC(screenDC);
    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!colorBmp) {
        DeleteObject(blackBmp);
        DeleteObject(whiteBmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, colorBmp);

    UINT32* blackPx = (UINT32*)blackBits;
    UINT32* whitePx = (UINT32*)whiteBits;
    UINT32* pixels = (UINT32*)bits;
    for (int i = 0; i < size * size; i++) {
        UINT32 bp = blackPx[i] & 0x00FFFFFF;
        UINT32 wp = whitePx[i] & 0x00FFFFFF;
        pixels[i] = (bp == wp) ? (0xFF000000u | bp) : 0u;
    }
    DeleteObject(blackBmp);
    DeleteObject(whiteBmp);

    // Yellow "!" glyph in the bottom-right corner, blended straight onto
    // whatever's already there -- no badge backdrop shape. GDI's
    // antialiased text rendering writes partial coverage into the alpha
    // byte of a 32bpp DIB (same issue as overlay.cpp's label text), so
    // render it into an isolated black-background mask DC first and use
    // its brightness as blend coverage against each destination pixel's
    // *existing* color instead of a fixed background.
    int r = size * 7 / 16;
    int cx = size - r - 1 + r / 3; // nudged right of center-in-corner
    int cy = size - r - 1;
    COLORREF glyphColor = RGB(0xFF, 0xC8, 0x00); // yellow
    BYTE gR = GetRValue(glyphColor), gG = GetGValue(glyphColor), gB = GetBValue(glyphColor);
    int boxSize = r * 2;
    HDC maskDC = CreateCompatibleDC(screenDC);
    BITMAPINFO maskBmi = {};
    maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    maskBmi.bmiHeader.biWidth = boxSize;
    maskBmi.bmiHeader.biHeight = -boxSize;
    maskBmi.bmiHeader.biPlanes = 1;
    maskBmi.bmiHeader.biBitCount = 32;
    maskBmi.bmiHeader.biCompression = BI_RGB;
    void* maskBits = nullptr;
    HBITMAP maskBmp = CreateDIBSection(maskDC, &maskBmi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    if (maskBmp) {
        HBITMAP oldMaskBmp = (HBITMAP)SelectObject(maskDC, maskBmp);
        HFONT font = CreateFontW(-(int)(r * 2.1), 0, 0, 0, FW_BLACK, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(maskDC, font);
        SetBkMode(maskDC, TRANSPARENT);
        SetTextColor(maskDC, RGB(255, 255, 255));
        RECT textRect = {0, 0, boxSize, boxSize};
        DrawTextW(maskDC, L"!", 1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(maskDC, oldFont);
        DeleteObject(font);

        UINT32* maskPixels = (UINT32*)maskBits;
        int originX = cx - r, originY = cy - r;
        for (int y = 0; y < boxSize; y++) {
            for (int x = 0; x < boxSize; x++) {
                BYTE cov = (BYTE)(maskPixels[y * boxSize + x] & 0xFF); // R=G=B for white-on-black text
                if (cov == 0) continue;
                int px = originX + x, py = originY + y;
                if (px < 0 || py < 0 || px >= size || py >= size) continue;
                UINT32 existing = pixels[py * size + px];
                BYTE exA = (BYTE)((existing >> 24) & 0xFF);
                BYTE exR = (BYTE)((existing >> 16) & 0xFF);
                BYTE exG = (BYTE)((existing >> 8) & 0xFF);
                BYTE exB = (BYTE)(existing & 0xFF);
                BYTE outR = (BYTE)(exR + ((int)gR - exR) * cov / 255);
                BYTE outG = (BYTE)(exG + ((int)gG - exG) * cov / 255);
                BYTE outB = (BYTE)(exB + ((int)gB - exB) * cov / 255);
                BYTE outA = (BYTE)(exA + (255 - exA) * cov / 255); // opaque even over transparent bg
                pixels[py * size + px] = (UINT32(outA) << 24) | (UINT32(outR) << 16) | (UINT32(outG) << 8) |
                                          UINT32(outB);
            }
        }
        SelectObject(maskDC, oldMaskBmp);
        DeleteObject(maskBmp);
    }
    DeleteDC(maskDC);

    // All-zero AND mask -- the color bitmap's own alpha channel (which we
    // just built by hand above) governs transparency for this 32bpp icon.
    int monoStride = ((size + 15) / 16) * 2;
    std::vector<BYTE> monoBits(monoStride * size, 0);
    HBITMAP andMaskBmp = CreateBitmap(size, size, 1, 1, monoBits.data());

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = andMaskBmp;
    ii.hbmColor = colorBmp;
    HICON result = CreateIconIndirect(&ii);

    DeleteObject(andMaskBmp);
    SelectObject(memDC, oldBmp);
    DeleteObject(colorBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return result;
}
