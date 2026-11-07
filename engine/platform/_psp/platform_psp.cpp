// engine/platform/_psp/platform_psp.cpp
//
// PSP 平台层（参考实现 / 第一版骨架）
//
// 负责：输入、计时、显示模式、日志。
// 不负责：GU 初始化/绘制 —— 那些在 engine/render/backend_psp.cpp。
//
// 这版先保证接口与 PC 平台一致，方便对照：
//   platform/windows/platform_windows.cpp
//
// 尚未做：
//   - 文件读取/存档路径抽象（现在 psp_main 自己用 sceIo）
//   - 音频（runtime/audio_psp.cpp 目前静音）
//   - HOME/退出键处理（PSP 没有可直接读的 HOME 键）
#include "platform/platform.h"

#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <pspiofilemgr.h>

#include <cstdio>
#include <cstring>

#include "base/log.h"

namespace eng {
namespace {

struct PspState {
  bool running = false;
  bool quit = false;
  InputState input;
  u32 prev_buttons = 0;
  u64 tick_start = 0;
};

PspState g;

void PlatLogSink(void* user, LogLevel level, const char* text) {
  (void)user;
  const char* tag = "INF";
  switch (level) {
    case LogLevel::kDebug: tag = "DBG"; break;
    case LogLevel::kInfo:  tag = "INF"; break;
    case LogLevel::kWarn:  tag = "WRN"; break;
    case LogLevel::kError: tag = "ERR"; break;
  }
  std::fprintf(stderr, "[%s] %s\n", tag, (text != nullptr) ? text : "");
  std::fflush(stderr);
}

void SetKey(int key, bool down) {
  g.input.down[key] = down;
}

void ScanInput() {
  SceCtrlData pad;
  std::memset(&pad, 0, sizeof(pad));
  if (sceCtrlPeekBufferPositive(&pad, 1) < 0) {
    return;
  }

  for (int i = 0; i < kKeyCount; ++i) {
    g.input.pressed[i] = false;
  }

  // PSP 的 ○/× 和 PC 的 A/B 语义不同；这里按“右手确认”习惯：
  //   × = A（确认），○ = B（取消），□ = X，△ = Y。
  const u32 b = pad.Buttons;
  if (b != g.prev_buttons) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "buttons %u\n", static_cast<unsigned>(b));
    const SceUID fd = sceIoOpen("ms0:/PL_INPUT.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) { sceIoWrite(fd, buf, static_cast<unsigned>(std::strlen(buf))); sceIoClose(fd); }
  }
  SetKey(static_cast<int>(Key::kA), (b & PSP_CTRL_CROSS) != 0);
  SetKey(static_cast<int>(Key::kB), (b & PSP_CTRL_CIRCLE) != 0);
  SetKey(static_cast<int>(Key::kX), (b & PSP_CTRL_SQUARE) != 0);
  SetKey(static_cast<int>(Key::kY), (b & PSP_CTRL_TRIANGLE) != 0);
  SetKey(static_cast<int>(Key::kL), (b & PSP_CTRL_LTRIGGER) != 0);
  SetKey(static_cast<int>(Key::kR), (b & PSP_CTRL_RTRIGGER) != 0);
  SetKey(static_cast<int>(Key::kStart), (b & PSP_CTRL_START) != 0);
  SetKey(static_cast<int>(Key::kSelect), (b & PSP_CTRL_SELECT) != 0);
  SetKey(static_cast<int>(Key::kUp), (b & PSP_CTRL_UP) != 0);
  SetKey(static_cast<int>(Key::kDown), (b & PSP_CTRL_DOWN) != 0);
  SetKey(static_cast<int>(Key::kLeft), (b & PSP_CTRL_LEFT) != 0);
  SetKey(static_cast<int>(Key::kRight), (b & PSP_CTRL_RIGHT) != 0);

  const u32 pressed = b & ~g.prev_buttons;
  if ((pressed & PSP_CTRL_CROSS) != 0) g.input.pressed[static_cast<int>(Key::kA)] = true;
  if ((pressed & PSP_CTRL_CIRCLE) != 0) g.input.pressed[static_cast<int>(Key::kB)] = true;
  if ((pressed & PSP_CTRL_SQUARE) != 0) g.input.pressed[static_cast<int>(Key::kX)] = true;
  if ((pressed & PSP_CTRL_TRIANGLE) != 0) g.input.pressed[static_cast<int>(Key::kY)] = true;
  if ((pressed & PSP_CTRL_LTRIGGER) != 0) g.input.pressed[static_cast<int>(Key::kL)] = true;
  if ((pressed & PSP_CTRL_RTRIGGER) != 0) g.input.pressed[static_cast<int>(Key::kR)] = true;
  if ((pressed & PSP_CTRL_START) != 0) g.input.pressed[static_cast<int>(Key::kStart)] = true;
  if ((pressed & PSP_CTRL_SELECT) != 0) g.input.pressed[static_cast<int>(Key::kSelect)] = true;
  if ((pressed & PSP_CTRL_UP) != 0) g.input.pressed[static_cast<int>(Key::kUp)] = true;
  if ((pressed & PSP_CTRL_DOWN) != 0) g.input.pressed[static_cast<int>(Key::kDown)] = true;
  if ((pressed & PSP_CTRL_LEFT) != 0) g.input.pressed[static_cast<int>(Key::kLeft)] = true;
  if ((pressed & PSP_CTRL_RIGHT) != 0) g.input.pressed[static_cast<int>(Key::kRight)] = true;

  g.prev_buttons = b;
}

bool PlatInit(const PlatformConfig& cfg) {
  (void)cfg;
  log_set_sink(&PlatLogSink, nullptr);

  if (sceCtrlSetSamplingCycle(0) < 0) return false;
  if (sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG) < 0) return false;
  if (sceDisplaySetMode(0, 480, 272) < 0) return false;

  g.tick_start = sceKernelGetSystemTimeWide();
  g.running = true;
  g.quit = false;
  return true;
}

void PlatShutdown() {
  g.running = false;
}

void PlatBeginFrame() {
  ScanInput();
}

void PlatEndFrame() {
  // 呈现由 backend_psp 的 submit 完成（GU finish/sync/swap）。
}

void PlatPumpEvents() {
  // PSP 没有窗口消息队列；输入在 begin_frame 扫描。
}

bool PlatShouldQuit() { return g.quit; }

const InputState& PlatInput() { return g.input; }

u64 PlatTickUs() {
  const u64 now = sceKernelGetSystemTimeWide();
  return now >= g.tick_start ? now - g.tick_start : 0;
}

void PlatLogWrite(LogLevel lv, const char* text) {
  const char* tag = "INF";
  switch (lv) {
    case LogLevel::kDebug: tag = "DBG"; break;
    case LogLevel::kInfo:  tag = "INF"; break;
    case LogLevel::kWarn:  tag = "WRN"; break;
    case LogLevel::kError: tag = "ERR"; break;
  }
  std::fprintf(stdout, "[%s] %s\n", tag, (text != nullptr) ? text : "");
  std::fflush(stdout);
}

SoftFrameBuffer PlatSoftFramebuffer() {
  // PSP 走 GU 后端，不提供 CPU 帧缓冲。
  SoftFrameBuffer fb;
  fb.pixels = nullptr;
  fb.width = 0;
  fb.height = 0;
  fb.pitch = 0;
  return fb;
}

DisplayImage PlatReadDisplay() {
  DisplayImage di;
  di.pixels = nullptr;
  di.width = 0;
  di.height = 0;
  di.pitch = 0;
  return di;
}

}  // namespace

const IPlatform kPlatform = {
    PlatInit,
    PlatShutdown,
    PlatBeginFrame,
    PlatEndFrame,
    PlatPumpEvents,
    PlatShouldQuit,
    PlatInput,
    PlatTickUs,
    PlatLogWrite,
    PlatSoftFramebuffer,
    PlatReadDisplay,
};

}  // namespace eng
