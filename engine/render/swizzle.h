#pragma once
#include "base/types.h"
namespace eng {
bool SwizzleRgba8To3ds(const u8* src_rgba, int width, int height, u8* dst,
                       bool flip_v = false);
}
