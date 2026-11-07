#include "render/swizzle.h"
#include "base/log.h"
namespace eng {
namespace {
constexpr int Morton8(int x, int y) {
  return ((x & 1) << 0) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
         ((x & 4) << 2) | ((y & 4) << 3);
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
  for (int y = 0; y < height; ++y) {
    const int src_y = flip_v ? (height - 1 - y) : y;
    for (int x = 0; x < width; ++x) {
      const int tile = (y >> 3) * tiles_x + (x >> 3);
      const int slot = tile * 64 + Morton8(x & 7, y & 7);
      const u8* s = src_rgba + (static_cast<usize>(src_y) * static_cast<usize>(width) +
                                static_cast<usize>(x)) * 4u;
      u8* d = dst + static_cast<usize>(slot) * 4u;
      d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    }
  }
  return true;
}
}
