#include "render/swizzle.h"
#include "base/log.h"

#include <cstring>

namespace eng {
namespace {
const unsigned char kFoursome[][4] = {
    {2, 8, 16, 4},   {3, 9, 17, 5},   {6, 10, 24, 20},
    {7, 11, 25, 21}, {14, 26, 28, 22}, {15, 27, 29, 23},
    {34, 40, 48, 36}, {35, 41, 49, 37}, {38, 42, 56, 52},
    {39, 43, 57, 53}, {46, 58, 60, 54}, {47, 59, 61, 55},
};
void SwapPixel(u8* p, int a, int b) {
  u8 t[4];
  std::memcpy(t, p + a * 4, 4);
  std::memcpy(p + a * 4, p + b * 4, 4);
  std::memcpy(p + b * 4, t, 4);
}
}
bool SwizzleRgba8To3ds(const u8* src_rgba, int width, int height, u8* dst,
                       bool flip_v) {
  if (src_rgba == nullptr || dst == nullptr || width <= 0 || height <= 0) return false;
  if ((width & 7) != 0 || (height & 7) != 0) {
    log_printf(LogLevel::kError, "swizzle: size must be multiple of 8 (%dx%d)", width, height);
    return false;
  }
  const int tiles_x = width >> 3;
  u8 tile[64 * 4];
  for (int ty = 0; ty < height; ty += 8) {
    for (int tx = 0; tx < width; tx += 8) {
      for (int y = 0; y < 8; ++y) {
        const int sy = flip_v ? (height - 1 - (ty + y)) : (ty + y);
        for (int x = 0; x < 8; ++x) {
          const u8* s = src_rgba + (static_cast<usize>(sy) * static_cast<usize>(width) +
                                    static_cast<usize>(tx + x)) * 4u;
          u8* d = tile + (y * 8 + x) * 4;
          d[0] = s[3];
          d[1] = s[2];
          d[2] = s[1];
          d[3] = s[0];
        }
      }
      for (const auto& e : kFoursome) {
        const u8 t[4] = {tile[e[0] * 4], tile[e[0] * 4 + 1], tile[e[0] * 4 + 2], tile[e[0] * 4 + 3]};
        std::memcpy(tile + e[0] * 4, tile + e[1] * 4, 4);
        std::memcpy(tile + e[1] * 4, tile + e[2] * 4, 4);
        std::memcpy(tile + e[2] * 4, tile + e[3] * 4, 4);
        std::memcpy(tile + e[3] * 4, t, 4);
      }
      SwapPixel(tile, 12, 18);
      SwapPixel(tile, 13, 19);
      SwapPixel(tile, 44, 50);
      SwapPixel(tile, 45, 51);
      const int tile_index = (ty >> 3) * tiles_x + (tx >> 3);
      std::memcpy(dst + tile_index * 64 * 4, tile, sizeof(tile));
    }
  }
  return true;
}
}
