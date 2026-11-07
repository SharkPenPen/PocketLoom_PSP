// runtime/psp_main.cpp
//
// PSP 播放器（第一版骨架）
//
// 目标：用当前 PocketLoom runtime 读取同一份 p3d。
// 目前直接读 ms0 上的 game.p3d，不走 romfs/ISO 抽象。
//
// 构建后生成 game_psp.elf，再打成 EBOOT.PBP。
// 真机/PPSSPP 路径：
//   ms0:/PSP/GAME/PocketLoom/EBOOT.PBP
//   ms0:/PSP/GAME/PocketLoom/game.p3d
#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "engine/platform/platform.h"
#include "engine/render/draw_list.h"
#include "engine/render/texture.h"
#include "runtime/audio.h"
#include "runtime/game.h"
#include "runtime/p3d.h"

PSP_MODULE_INFO("PocketLoom", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);

namespace {

constexpr const char* kDefaultP3d = "ms0:/PSP/GAME/PocketLoom/game.p3d";
constexpr const char* kSaveDir = "ms0:/PSP/SAVEDATA/PocketLoom";
constexpr const char* kSavePath = "ms0:/PSP/SAVEDATA/PocketLoom/save0.dat";
constexpr const char* kReportPath = "ms0:/PSP/SAVEDATA/PocketLoom/report.txt";
constexpr const char* kTracePath = "ms0:/PL_ERR.txt";

void Trace(const char* tag, const char* text) {
  const SceUID fd = sceIoOpen(kTracePath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) return;
  sceIoWrite(fd, tag, static_cast<unsigned int>(std::strlen(tag)));
  if (text != nullptr) sceIoWrite(fd, text, static_cast<unsigned int>(std::strlen(text)));
  sceIoWrite(fd, "\n", 1);
  sceIoClose(fd);
}

bool ReadFile(const char* path, std::vector<unsigned char>* out) {
  const SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
  if (fd < 0) return false;

  const SceOff size = sceIoLseek(fd, 0, PSP_SEEK_END);
  if (size <= 0 || size > (16 * 1024 * 1024)) {
    sceIoClose(fd);
    return false;
  }
  sceIoLseek(fd, 0, PSP_SEEK_SET);

  out->resize(static_cast<size_t>(size));
  int read_total = 0;
  while (read_total < static_cast<int>(size)) {
    const int n = sceIoRead(fd, out->data() + read_total,
                            static_cast<unsigned int>(size - read_total));
    if (n <= 0) break;
    read_total += n;
  }
  sceIoClose(fd);
  out->resize(static_cast<size_t>(read_total));
  return read_total == static_cast<int>(size);
}

void WriteReport(const r4::Game& game, int frames, const char* p3d_path) {
  std::FILE* f = std::fopen(kReportPath, "wb");
  if (f == nullptr) return;
  std::fprintf(f, "ok=1\n");
  std::fprintf(f, "frames=%d\n", frames);
  std::fprintf(f, "scene=%s\n", game.scene_id().c_str());
  std::fprintf(f, "transitions=%d\n", game.transition_count());
  std::fprintf(f, "audio=%s\n", r4::AudioBackend());
  std::fprintf(f, "x=%.1f\n", static_cast<double>(game.player().x));
  std::fprintf(f, "y=%.1f\n", static_cast<double>(game.player().y));
  std::fprintf(f, "cam_x=%.1f\n", static_cast<double>(game.camera_x()));
  std::fprintf(f, "cam_y=%.1f\n", static_cast<double>(game.camera_y()));
  std::fprintf(f, "finished=%d\n", game.finished() ? 1 : 0);
  std::fprintf(f, "p3d=%s\n", p3d_path == nullptr ? "" : p3d_path);
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  std::string p3d_path = kDefaultP3d;
  int frames_limit = 0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--p3d") == 0 && i + 1 < argc) {
      p3d_path = argv[++i];
    } else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
      frames_limit = std::atoi(argv[++i]);
    }
  }

  std::vector<unsigned char> bytes;
  if (!ReadFile(p3d_path.c_str(), &bytes)) {
    // 兼容直接放 EBOOT 旁边的情况
    if (!ReadFile("game.p3d", &bytes)) {
      std::fprintf(stderr, "psp: cannot read %s\n", p3d_path.c_str());
      return 2;
    }
    p3d_path = "game.p3d";
  }

  r4::P3d p3d;
  std::string err;
  bool loaded = false;
  try {
    loaded = r4::P3dLoad(bytes.data(), bytes.size(), &p3d, &err);
  } catch (const std::exception& e) {
    Trace("P3dLoad exception: ", e.what());
    return 3;
  } catch (...) {
    Trace("P3dLoad exception: unknown", nullptr);
    return 3;
  }
  if (!loaded) {
    Trace("P3dLoad error: ", err.c_str());
    std::fprintf(stderr, "psp: %s\n", err.c_str());
    return 3;
  }

  // p3d 是左下原点、y 向上；引擎内部是屏幕系，这里翻一次。
  r4::MirrorSceneYUp(p3d, p3d.view_h);

  eng::PlatformConfig cfg;
  cfg.title = "PocketLoom";
  cfg.width = 480;
  cfg.height = 272;
  if (!eng::Platform().init(cfg)) {
    std::fprintf(stderr, "psp: platform init failed\n");
    return 4;
  }
  if (!eng::RenderBackend().init()) {
    std::fprintf(stderr, "psp: render init failed\n");
    eng::Platform().shutdown();
    return 5;
  }

  sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);
  sceIoMkdir(kSaveDir, 0777);

  r4::AudioInit();  // 当前 audio_psp 静音

  eng::TextureId textures[32] = {};
  const int texture_count =
      static_cast<int>(p3d.images.size()) < 32 ? static_cast<int>(p3d.images.size()) : 32;
  for (int i = 0; i < texture_count; ++i) {
    const r4::Image& img = p3d.images[i];
    textures[i] = eng::TextureCreate(img.rgba.data(), img.width, img.height);
  }

  r4::Game game;
  game.SetSavePath(kSavePath);
  if (!game.Init(p3d, &err)) {
    std::fprintf(stderr, "psp: %s\n", err.c_str());
    for (int i = 0; i < texture_count; ++i) {
      if (textures[i] != 0) eng::TextureDestroy(textures[i]);
    }
    r4::AudioShutdown();
    eng::RenderBackend().shutdown();
    eng::Platform().shutdown();
    return 6;
  }

  // ★ 必须放静态区，不能放栈上 ★
  //   DrawList 有 kMaxCmds=6144 个 DrawCmd 槽，sizeof(DrawCmd)=60，
  //   实测 sizeof(DrawList)=369512（360.9KB）；而本线程栈只有 256KB
  //   （见上面的 PSP_MAIN_THREAD_STACK_SIZE_KB(256)）。
  //   放栈上会在进入循环前就压爆栈：纹理表/Game 被踩坏，
  //   PPSSPP 里表现为整屏黑 + 一批 0x00~0xA0 的低地址非法访问。
  static eng::DrawList dl;
  dl.ResetScreenSizes();
  dl.SetScreenSize(480, 272);

  int frames = 0;
  while (!eng::Platform().should_quit()) {
    eng::Platform().begin_frame();
    eng::Platform().pump_events();
    if (eng::Platform().should_quit()) break;

    const eng::InputState input = eng::Platform().input();
    game.Update(1.0f / 60.0f, input);

    dl.Reset();
    game.Render(dl, textures, texture_count);
    eng::RenderBackend().submit(dl);
    eng::Platform().end_frame();

    ++frames;
    if (frames_limit > 0 && frames >= frames_limit) break;
  }

  WriteReport(game, frames, p3d_path.c_str());

  for (int i = 0; i < texture_count; ++i) {
    if (textures[i] != 0) eng::TextureDestroy(textures[i]);
  }
  r4::AudioShutdown();
  eng::RenderBackend().shutdown();
  eng::Platform().shutdown();
  return 0;
}
