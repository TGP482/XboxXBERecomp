// The window's icon from the title's own image: the xbe's $$XTIMAGE section, the picture the dashboard showed for
// the game. It is read from the player's xbe at start, so no game art ships with the executable.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "xbe/xbe.h"

namespace app {

namespace {

// RGB565 to 8 bit channels
void Color565(uint16_t c, uint8_t out[3]) {
    out[0] = uint8_t((c >> 11) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[2] = uint8_t((c & 31) * 255 / 31);
}

// one DXT colour block into a 4x4 RGBA patch (alpha untouched); four colours always, as DXT2-5 use them
void ColorBlock(const uint8_t* b, bool dxt1, uint8_t px[16][4]) {
    uint16_t c0, c1;
    std::memcpy(&c0, b, 2);
    std::memcpy(&c1, b + 2, 2);
    uint8_t c[4][4] = {};
    Color565(c0, c[0]);
    Color565(c1, c[1]);
    for (int k = 0; k < 3; k++) {
        if (!dxt1 || c0 > c1) {
            c[2][k] = uint8_t((2 * c[0][k] + c[1][k]) / 3);
            c[3][k] = uint8_t((c[0][k] + 2 * c[1][k]) / 3);
        } else {
            c[2][k] = uint8_t((c[0][k] + c[1][k]) / 2);
        }
    }
    c[0][3] = c[1][3] = c[2][3] = 255;
    c[3][3] = (!dxt1 || c0 > c1) ? 255 : 0;
    uint32_t bits;
    std::memcpy(&bits, b + 4, 4);
    for (int i = 0; i < 16; i++) {
        const uint8_t* s = c[(bits >> (2 * i)) & 3];
        std::memcpy(px[i], s, dxt1 ? 4 : 3);
    }
}

// the xbe texture formats a title image comes in, into w x h RGBA rows; false for any other
bool Decode(uint32_t format, const uint8_t* d, size_t size, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
    rgba.assign(size_t(w) * h * 4, 0);
    if (format == 0x0C || format == 0x0E || format == 0x0F) {  // DXT1, DXT2/3, DXT4/5
        const size_t block = format == 0x0C ? 8 : 16;
        if (size < size_t(w / 4) * (h / 4) * block) return false;
        for (uint32_t by = 0; by < h / 4; by++)
            for (uint32_t bx = 0; bx < w / 4; bx++) {
                const uint8_t* b = d + (size_t(by) * (w / 4) + bx) * block;
                uint8_t px[16][4] = {};
                ColorBlock(b + block - 8, format == 0x0C, px);
                if (format == 0x0E) {
                    for (int i = 0; i < 16; i++) px[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                } else if (format == 0x0F) {
                    uint8_t a[8] = {b[0], b[1]};
                    for (int i = 2; i < 8; i++)
                        a[i] = a[0] > a[1] ? uint8_t(((8 - i) * a[0] + (i - 1) * a[1]) / 7)
                               : i < 6     ? uint8_t(((6 - i) * a[0] + (i - 1) * a[1]) / 5)
                                           : (i == 6 ? 0 : 255);
                    uint64_t bits = 0;
                    std::memcpy(&bits, b + 2, 6);
                    for (int i = 0; i < 16; i++) px[i][3] = a[(bits >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; i++)
                    std::memcpy(&rgba[((size_t(by) * 4 + i / 4) * w + bx * 4 + i % 4) * 4], px[i], 4);
            }
        return true;
    }
    if (format == 0x06 || format == 0x12) {  // A8R8G8B8, swizzled or linear
        if (size < size_t(w) * h * 4) return false;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                size_t i = size_t(y) * w + x;
                if (format == 0x06) {  // Morton order: x and y bits interleaved, x lowest, while both last
                    i = 0;
                    for (uint32_t bit = 0, out = 0; (1u << bit) < std::max(w, h); bit++) {
                        if ((1u << bit) < w) i |= size_t((x >> bit) & 1) << out++;
                        if ((1u << bit) < h) i |= size_t((y >> bit) & 1) << out++;
                    }
                }
                const uint8_t* s = d + i * 4;  // B G R A
                uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
                o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = s[3];
            }
        return true;
    }
    return false;
}

// an n x n icon, averaged down from the w x h picture
HICON MakeIcon(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, int n) {
    BITMAPV5HEADER bh{};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = n;
    bh.bV5Height = -n;  // top down
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000;
    bh.bV5GreenMask = 0x0000FF00;
    bh.bV5BlueMask = 0x000000FF;
    bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    auto* out = static_cast<uint8_t*>(bits);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const uint32_t x0 = x * w / n, x1 = std::max(x0 + 1, (x + 1) * w / n);
            const uint32_t y0 = y * h / n, y1 = std::max(y0 + 1, (y + 1) * h / n);
            uint32_t sum[4] = {}, count = 0;
            for (uint32_t sy = y0; sy < y1; sy++)
                for (uint32_t sx = x0; sx < x1; sx++, count++)
                    for (int k = 0; k < 4; k++) sum[k] += rgba[(size_t(sy) * w + sx) * 4 + k];
            uint8_t* o = out + (size_t(y) * n + x) * 4;  // B G R A
            o[0] = uint8_t(sum[2] / count), o[1] = uint8_t(sum[1] / count), o[2] = uint8_t(sum[0] / count);
            o[3] = uint8_t(sum[3] / count);
        }
    std::vector<uint8_t> zeros(size_t((n + 15) / 16 * 2) * n);  // the alpha channel is the mask
    HBITMAP mask = CreateBitmap(n, n, 1, 1, zeros.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

}  // namespace

void SetTitleIcon(void* hwnd, const std::vector<uint8_t>& xbe_file) {
    xbe::Image img;
    std::string error;
    if (!img.Parse(xbe_file, error)) return;
    auto it = std::find_if(img.sections.begin(), img.sections.end(), [](const xbe::Section& s) { return s.name == "$$XTIMAGE"; });
    if (it == img.sections.end() || it->rsize < 0x20 || size_t(it->raw) + it->rsize > img.file.size()) return;
    // an XPR0 bundle: magic, total size, header size, then the texture's D3D resource (common, data, lock, format)
    const uint8_t* x = img.file.data() + it->raw;
    uint32_t total, header, format;
    std::memcpy(&total, x + 4, 4);
    std::memcpy(&header, x + 8, 4);
    std::memcpy(&format, x + 12 + 12, 4);
    if (std::memcmp(x, "XPR0", 4) || total > it->rsize || header >= total) return;
    const uint32_t w = 1u << ((format >> 20) & 15), h = 1u << ((format >> 24) & 15);
    std::vector<uint8_t> rgba;
    if (w < 4 || h < 4 || w > 1024 || h > 1024 || !Decode((format >> 8) & 0xFF, x + header, total - header, w, h, rgba)) {
        LOGW("title image: format %08X not decoded, the window keeps the default icon", format);
        return;
    }
    // owned by the window for the life of the process
    if (HICON big = MakeIcon(rgba, w, h, GetSystemMetrics(SM_CXICON)))
        SendMessageW(static_cast<HWND>(hwnd), WM_SETICON, ICON_BIG, LPARAM(big));
    if (HICON small = MakeIcon(rgba, w, h, GetSystemMetrics(SM_CXSMICON)))
        SendMessageW(static_cast<HWND>(hwnd), WM_SETICON, ICON_SMALL, LPARAM(small));
}

}  // namespace app
