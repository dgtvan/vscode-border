#include "profile_icon.h"

#include "file_util.h"
#include "logger.h"
#include <gdiplus.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace {

// ---- Icons ----

// A config.ini path, made absolute (relative to config.ini's folder).
std::wstring ConfigRelativePath(const std::wstring& p) {
    if (p.empty()) return p;
    bool absolute = p.size() > 1 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\'));
    return absolute ? p : GetExeDir() + L"\\" + p;
}

// Whether an icon value is the image itself rather than a path to one: a
// data URI ("data:image/png;base64,..."), or bare base64 -- which never has
// a '.', '\\' or ':', while a path to an image file always has one of them.
bool IsBase64Icon(const std::wstring& icon) {
    if (_wcsnicmp(icon.c_str(), L"data:", 5) == 0) return true;
    return icon.find_first_of(L".\\:") == std::wstring::npos;
}

// Decodes base64 (a data URI's payload, or bare), skipping whitespace; false
// on anything else outside the base64 alphabet.
bool DecodeBase64(const std::wstring& text, std::vector<BYTE>& out) {
    size_t start = 0;
    if (_wcsnicmp(text.c_str(), L"data:", 5) == 0) {
        start = text.find(L',');
        if (start == std::wstring::npos) return false;
        start++;
    }
    out.clear();
    UINT32 acc = 0;
    int bits = 0;
    for (size_t i = start; i < text.size(); i++) {
        wchar_t c = text[i];
        int v;
        if (c >= L'A' && c <= L'Z') v = c - L'A';
        else if (c >= L'a' && c <= L'z') v = c - L'a' + 26;
        else if (c >= L'0' && c <= L'9') v = c - L'0' + 52;
        else if (c == L'+' || c == L'-') v = 62;
        else if (c == L'/' || c == L'_') v = 63;
        else if (c == L'=') break;
        else if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        else return false;
        acc = (acc << 6) | (UINT32)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((BYTE)(acc >> bits));
        }
    }
    return !out.empty();
}

// Loads a resolved icon source as a GDI+ bitmap, or null.
Gdiplus::Bitmap* LoadIconImage(const std::wstring& source) {
    if (!IsBase64Icon(source)) return new Gdiplus::Bitmap(source.c_str());
    std::vector<BYTE> bytes;
    if (!DecodeBase64(source, bytes)) return nullptr;
    IStream* stream = SHCreateMemStream(bytes.data(), (UINT)bytes.size());
    if (!stream) return nullptr;
    // The bitmap reads from the stream for as long as it lives, and holds
    // its own reference to it.
    Gdiplus::Bitmap* image = new Gdiplus::Bitmap(stream);
    stream->Release();
    return image;
}

} // namespace

std::wstring ResolveProfileIconSource(const std::wstring& icon) {
    return icon.empty() || IsBase64Icon(icon) ? icon : ConfigRelativePath(icon);
}

// For log lines: a base64 icon by its size, not its contents.
std::wstring DescribeProfileIconSource(const std::wstring& source) {
    if (source.empty()) return L"the letter badge";
    if (IsBase64Icon(source)) return L"a base64 image (" + std::to_wstring(source.size()) + L" chars)";
    return source;
}

// A profile's icon rendered to premultiplied ARGB (S^2 pixels) from
// `iconSource` (see ConfiguredIconSource), or the badge with `name`'s first
// letter if that's empty or can't be read.
std::vector<UINT32> RenderProfileIcon(const std::wstring& name, const std::wstring& iconSource, int S) {
    std::vector<UINT32> out;
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) {
        LogWarn(L"profile_icon: GDI+ didn't start -- no profile icon");
        return out;
    }
    {
        Gdiplus::Bitmap target(S, S, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(&target);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        bool drawn = false;
        if (!iconSource.empty()) {
            Gdiplus::Bitmap* image = LoadIconImage(iconSource);
            if (image && image->GetLastStatus() == Gdiplus::Ok && image->GetWidth() > 0 && image->GetHeight() > 0) {
                // Without tile-flip wrapping, bicubic downscaling blends the
                // image's edge with transparent black, leaving a dark rim.
                Gdiplus::ImageAttributes attrs;
                attrs.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
                g.DrawImage(image, Gdiplus::Rect(0, 0, S, S), 0, 0, (INT)image->GetWidth(), (INT)image->GetHeight(),
                            Gdiplus::UnitPixel, &attrs);
                drawn = true;
            } else {
                LogWarn(L"profile_icon: can't read the icon for [%ls] from %ls -- using the letter badge",
                        name.c_str(), DescribeProfileIconSource(iconSource).c_str());
            }
            delete image;
        }
        if (!drawn) {
            Gdiplus::SolidBrush circle(Gdiplus::Color(255, 0x3B, 0x42, 0x52));
            g.FillEllipse(&circle, 0.0f, 0.0f, (Gdiplus::REAL)S, (Gdiplus::REAL)S);
            // A folder name like ".claude-work" starts with its first letter.
            size_t first = name.find_first_not_of(L".");
            wchar_t letter[2] = {first == std::wstring::npos ? L'?' : (wchar_t)towupper(name[first]), 0};
            Gdiplus::Font font(L"Segoe UI", S * 0.62f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat fmt;
            fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
            g.DrawString(letter, 1, &font, Gdiplus::RectF(0, 0.5f, (Gdiplus::REAL)S, (Gdiplus::REAL)S), &fmt, &white);
        }

        Gdiplus::Rect rect(0, 0, S, S);
        Gdiplus::BitmapData data;
        if (target.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
            out.resize(S * S);
            for (int y = 0; y < S; y++) {
                const UINT32* row = (const UINT32*)((const BYTE*)data.Scan0 + y * data.Stride);
                std::copy(row, row + S, out.begin() + y * S);
            }
            target.UnlockBits(&data);
        }
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return out;
}
