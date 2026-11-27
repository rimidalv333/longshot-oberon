// (longshot, issue 1295; Longshot PR #1367, ported 2026-11-26 for the base PS5) OoT's static shadow map of an area made smaller.
// Which size: vk_texture.cpp shadow_map_pct (RX_VK_SHADOW_MAP_SIZE).
//
// The game makes one 12288 x 12288 D16 depth image (NVN 0x32) when an area loads, draws it there (its draws' viewport and
// scissor the whole image) and samples it as a depth comparison through normalized coordinates: 288 MB of VRAM, on every
// GPU. Made at a share of its side (75 %: 9216, 162 MB; 50 %: 6144, 72 MB) with the draws' viewport, scissor and clears
// scaled by the same share, the same coordinates land on the same place in it. Its shadows' edges are coarser by as much.
// The sun's cascades (1600 to 3072, host/oot_shadow.c) and the characters' own maps (NVN 0x34, 768 and less) are not this
// image.
#pragma once
#include <cstdint>

namespace static_shadow_map {

constexpr int kNvnD16 = 0x32;               // (NVN_FORMAT_DEPTH16)
constexpr uint32_t kMinSide = 8192;         // (the map is 12288; the cascades are at most 3072)

// The texture is the static shadow map's kind: 2D, D16, one level and layer, not multisampled, square, kMinSide or more
inline bool candidate(bool is_2d, int nvn_format, uint32_t levels, uint32_t layers, int samples, uint32_t width,
                      uint32_t height) {
    return is_2d && nvn_format == kNvnD16 && levels == 1 && layers == 1 && samples <= 1 && width == height &&
           width >= kMinSide;
}

// A side of the game's image at that share (12288: 9216 at 75, 6144 at 50)
inline uint32_t side(uint32_t game, uint32_t pct) {
    return (uint32_t)(((uint64_t)game * pct + 99) / 100);
}

struct Rect {
    int32_t x, y;
    uint32_t w, h;
};

// The game's rectangle (scissor, clear) in the image at pct % of its side: its edges rounded outward, so every texel the
// game's rectangle touches is in it
inline Rect scale_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct) {
    if (!pct || pct == 100) return {x, y, (uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0)};
    auto lo = [&](int64_t v) {   // (floor of v * pct / 100)
        const int64_t n = v * pct;
        return n >= 0 ? n / 100 : -((-n + 99) / 100);
    };
    auto hi = [&](int64_t v) {   // (ceil)
        const int64_t n = v * pct;
        return n >= 0 ? (n + 99) / 100 : -(-n / 100);
    };
    const int64_t x0 = lo(x), y0 = lo(y), x1 = hi((int64_t)x + (w > 0 ? w : 0)), y1 = hi((int64_t)y + (h > 0 ? h : 0));
    return {(int32_t)x0, (int32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
}

// The game's viewport (x, y, width, height in texels of the full image) in the image at pct % of its side
inline void scale_viewport(float v[4], uint32_t pct) {
    if (!pct || pct == 100) return;
    const float k = (float)pct / 100.0f;
    for (int i = 0; i < 4; i++) v[i] *= k;
}

} // namespace static_shadow_map
