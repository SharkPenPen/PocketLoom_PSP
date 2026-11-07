// PocketLoom 3DS stage 1: platform + clear/rect + input, no Game yet.
#include "platform/platform.h"
#include "render/draw_list.h"

#include <3ds.h>

int main() {
  eng::PlatformConfig cfg;
  cfg.width = 400;
  cfg.height = 240;
  cfg.title = "PocketLoom";
  if (!eng::Platform().init(cfg)) return 1;
  if (!eng::RenderBackend().init()) {
    eng::Platform().shutdown();
    return 2;
  }

  static eng::DrawList dl;
  dl.ResetScreenSizes();
  dl.SetScreenSize(400, 240);

  while (aptMainLoop() && !eng::Platform().should_quit()) {
    eng::Platform().begin_frame();
    eng::Platform().pump_events();
    if (eng::Platform().should_quit()) break;

    dl.Reset();
    dl.Clear(0xFF101018u);
    dl.DrawRect(eng::Rect2{eng::Vec2{40.0f, 40.0f}, eng::Vec2{120.0f, 80.0f}},
                0xFF0000FFu);
    eng::RenderBackend().submit(dl);
    eng::Platform().end_frame();
  }

  eng::RenderBackend().shutdown();
  eng::Platform().shutdown();
  return 0;
}
