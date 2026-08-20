#include "layered_rendering.h"

COLORREF ContrastTextColor(COLORREF bg) {
    double luminance = 0.299 * GetRValue(bg) + 0.587 * GetGValue(bg) + 0.114 * GetBValue(bg);
    return luminance > 150.0 ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

void BlendTextIntoPixels(HDC screenDC, UINT32* pixels, int surfaceWidth, int surfaceHeight,
                         int x, int y, int width, int height, const std::wstring& text,
                         HFONT font, COLORREF bgColor, COLORREF textColor, UINT format) {
    if (width <= 0 || height <= 0 || text.empty()) return;

    HDC maskDC = CreateCompatibleDC(screenDC);
    BITMAPINFO maskBmi = {};
    maskBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    maskBmi.bmiHeader.biWidth = width;
    maskBmi.bmiHeader.biHeight = -height;
    maskBmi.bmiHeader.biPlanes = 1;
    maskBmi.bmiHeader.biBitCount = 32;
    maskBmi.bmiHeader.biCompression = BI_RGB;
    void* maskBits = nullptr;
    HBITMAP maskBmp = CreateDIBSection(maskDC, &maskBmi, DIB_RGB_COLORS, &maskBits, nullptr, 0);
    if (!maskBmp) {
        DeleteDC(maskDC);
        return;
    }

    HBITMAP oldMaskBmp = (HBITMAP)SelectObject(maskDC, maskBmp);
    HFONT oldMaskFont = (HFONT)SelectObject(maskDC, font);
    SetBkMode(maskDC, TRANSPARENT);
    SetTextColor(maskDC, RGB(255, 255, 255));
    RECT textRect = {0, 0, width, height};
    DrawTextW(maskDC, text.c_str(), (int)text.size(), &textRect, format | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(maskDC, oldMaskFont);

    BYTE bgR = GetRValue(bgColor), bgG = GetGValue(bgColor), bgB = GetBValue(bgColor);
    BYTE txR = GetRValue(textColor), txG = GetGValue(textColor), txB = GetBValue(textColor);
    UINT32* maskPixels = (UINT32*)maskBits;
    for (int row = 0; row < height; row++) {
        int outY = y + row;
        if (outY < 0 || outY >= surfaceHeight) continue;
        for (int col = 0; col < width; col++) {
            int outX = x + col;
            if (outX < 0 || outX >= surfaceWidth) continue;
            UINT32 mp = maskPixels[row * width + col];
            BYTE covR = (BYTE)((mp >> 16) & 0xFF);
            BYTE covG = (BYTE)((mp >> 8) & 0xFF);
            BYTE covB = (BYTE)(mp & 0xFF);
            if (covR == 0 && covG == 0 && covB == 0) continue;
            BYTE outR = (BYTE)(bgR + ((int)(txR - bgR) * covR) / 255);
            BYTE outG = (BYTE)(bgG + ((int)(txG - bgG) * covG) / 255);
            BYTE outB = (BYTE)(bgB + ((int)(txB - bgB) * covB) / 255);
            pixels[outY * surfaceWidth + outX] =
                (UINT32(255) << 24) | (UINT32(outR) << 16) | (UINT32(outG) << 8) | UINT32(outB);
        }
    }

    SelectObject(maskDC, oldMaskBmp);
    DeleteObject(maskBmp);
    DeleteDC(maskDC);
}