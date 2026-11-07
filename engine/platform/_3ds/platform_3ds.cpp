// PocketLoom 3DS platform: input, clock, lifecycle. Graphics init lives in backend_citro.
#include "platform/platform.h"

#include <3ds.h>
#include <cstdio>
#include <cstring>

namespace eng {
namespace {

struct State3ds {
  bool running = false;
  bool quit = false;
  InputState input;
  u64 tick_start = 0;
};

State3ds g;

void ScanInput() {
  hidScanInput();
  const u32 held = hidKeysHeld();
  const u32 down = hidKeysDown();
  std::memset(&g.input, 0, sizeof(g.input));

  const struct { u32 mask; Key key; } map[] = {
      {KEY_A, Key::kA},       {KEY_B, Key::kB},       {KEY_X, Key::kX},
      {KEY_Y, Key::kY},       {KEY_L, Key::kL},       {KEY_R, Key::kR},
      {KEY_START, Key::kStart}, {KEY_SELECT, Key::kSelect},
      {KEY_DUP, Key::kUp},    {KEY_DDOWN, Key::kDown},
      {KEY_DLEFT, Key::kLeft}, {KEY_DRIGHT, Key::kRight},
  };
  for (const auto& m : map) {
    const int i = static_cast<int>(m.key);
    g.input.down[i] = (held & m.mask) != 0;
    g.input.pressed[i] = (down & m.mask) != 0;
  }
  if ((down & KEY_START) != 0) g.quit = true;
}

bool PlatInit(const PlatformConfig& cfg) {
  (void)cfg;
  gfxInitDefault();
  g.tick_start = svcGetSystemTick();
  g.running = true;
  g.quit = false;
  return true;
}

void PlatShutdown() {
  if (g.running) gfxExit();
  g.running = false;
}

void PlatBeginFrame() { ScanInput(); }
void PlatEndFrame() {}
void PlatPumpEvents() {}
bool PlatShouldQuit() { return g.quit; }
const InputState& PlatInput() { return g.input; }

u64 PlatTickUs() {
  const u64 now = svcGetSystemTick();
  return now >= g.tick_start ? (now - g.tick_start) / 268u : 0;
}

void PlatLogWrite(LogLevel level, const char* text) {
  const char* tag = level == LogLevel::kError ? "ERR"
                   : level == LogLevel::kWarn ? "WRN" : "INF";
  std::printf("[%s] %s\n", tag, text != nullptr ? text : "");
}

SoftFrameBuffer PlatSoftFramebuffer() { return {}; }
DisplayImage PlatReadDisplay() { return {}; }

}  // namespace

const IPlatform kPlatform = {
    PlatInit,          PlatShutdown,     PlatBeginFrame, PlatEndFrame,
    PlatPumpEvents,    PlatShouldQuit,   PlatInput,      PlatTickUs,
    PlatLogWrite,      PlatSoftFramebuffer, PlatReadDisplay,
};

}  // namespace eng
