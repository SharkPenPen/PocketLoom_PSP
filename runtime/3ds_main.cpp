// PocketLoom 3DS stage 2: read p3d and run Game update/render (clear/rect backend).
#include <3ds.h>

#include <cstdio>
#include <string>
#include <vector>

#include "engine/platform/platform.h"
#include "engine/render/draw_list.h"
#include "engine/render/texture.h"
#include "runtime/audio.h"
#include "runtime/game.h"
#include "runtime/p3d.h"

int main() {
  romfsInit();
  eng::PlatformConfig cfg;
  cfg.width = 400;
  cfg.height = 240;
  cfg.title = "PocketLoom";
  if (!eng::Platform().init(cfg)) {
    romfsExit();
    return 1;
  }
  if (!eng::RenderBackend().init()) {
    eng::Platform().shutdown();
    romfsExit();
    return 2;
  }
  r4::AudioInit();

  std::FILE* f = std::fopen("romfs:/game.p3d", "rb");
  if (f == nullptr) {
    eng::RenderBackend().shutdown();
    eng::Platform().shutdown();
    romfsExit();
    return 3;
  }
  std::fseek(f, 0, SEEK_END);
  const long file_size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (file_size <= 0) { std::fclose(f); eng::RenderBackend().shutdown(); eng::Platform().shutdown(); romfsExit(); return 3; }
  std::vector<unsigned char> p3d_bytes(static_cast<size_t>(file_size));
  const size_t got = std::fread(p3d_bytes.data(), 1, p3d_bytes.size(), f);
  std::fclose(f);
  if (got != p3d_bytes.size()) { eng::RenderBackend().shutdown(); eng::Platform().shutdown(); romfsExit(); return 3; }

  r4::P3d p3d;
  std::string err;
  if (!r4::P3dLoad(p3d_bytes.data(), p3d_bytes.size(), &p3d, &err)) {
    eng::RenderBackend().shutdown();
    eng::Platform().shutdown();
    romfsExit();
    return 3;
  }
  r4::MirrorSceneYUp(p3d, 240);

  static std::vector<std::vector<unsigned char>> texture_data;
  eng::TextureId textures[32] = {};
  const int texture_count = static_cast<int>(p3d.images.size()) < 32
                                ? static_cast<int>(p3d.images.size())
                                : 32;
  texture_data.resize(static_cast<size_t>(texture_count));
  for (int i = 0; i < texture_count; ++i) {
    texture_data[static_cast<size_t>(i)] = p3d.images[static_cast<size_t>(i)].rgba;
    textures[i] = eng::TextureCreate(texture_data[static_cast<size_t>(i)].data(),
                                     p3d.images[static_cast<size_t>(i)].width,
                                     p3d.images[static_cast<size_t>(i)].height);
  }

  r4::Game game;
  game.SetSavePath("sdmc:/3ds/PocketLoom/save0.dat");
  if (!game.Init(p3d, &err)) {
    eng::RenderBackend().shutdown();
    eng::Platform().shutdown();
    romfsExit();
    return 4;
  }

  static eng::DrawList dl;
  dl.ResetScreenSizes();
  dl.SetScreenSize(400, 240);

  while (aptMainLoop() && !eng::Platform().should_quit()) {
    eng::Platform().begin_frame();
    eng::Platform().pump_events();
    if (eng::Platform().should_quit()) break;

    const eng::InputState input = eng::Platform().input();
    game.Update(1.0f / 60.0f, input);

    dl.Reset();
    game.Render(dl, textures, texture_count);
    eng::RenderBackend().submit(dl);
    eng::Platform().end_frame();
  }

  r4::AudioShutdown();
  eng::RenderBackend().shutdown();
  eng::Platform().shutdown();
  romfsExit();
  return 0;
}
