// PocketLoom 3DS stage 1 backend: clear + rect only, no textures yet.
#include "render/draw_list.h"

#include <3ds.h>
#include <citro2d.h>

namespace eng {
namespace {

C3D_RenderTarget* g_top = nullptr;
C3D_RenderTarget* g_bottom = nullptr;
bool g_ready = false;

bool CitroInit() {
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
  C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
  C2D_Prepare();
  g_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
  g_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
  if (g_top == nullptr || g_bottom == nullptr) {
    C2D_Fini();
    C3D_Fini();
    return false;
  }
  C2D_SetTintMode(C2D_TintMult);
  g_ready = true;
  return true;
}

void CitroShutdown() {
  g_ready = false;
  g_top = nullptr;
  g_bottom = nullptr;
  C2D_Fini();
  C3D_Fini();
}

void CitroSubmit(const DrawList& dl) {
  if (!g_ready) return;
  u32 clear = 0xFF000000u;
  for (int i = 0; i < dl.count(); ++i) {
    if (dl.cmd(i).kind == DrawCmdKind::kClear) {
      clear = dl.cmd(i).color;
      break;
    }
  }
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  C2D_TargetClear(g_top, clear);
  C2D_SceneBegin(g_top);
  for (int i = 0; i < dl.count(); ++i) {
    const DrawCmd& c = dl.cmd(i);
    if (c.kind != DrawCmdKind::kRect) continue;
    C2D_DrawRectangle(c.rect.position.x, c.rect.position.y, 0.0f,
                      c.rect.size.x, c.rect.size.y,
                      c.color, c.color, c.color, c.color);
  }
  C2D_TargetClear(g_bottom, C2D_Color32(8, 8, 16, 255));
  C2D_SceneBegin(g_bottom);
  C3D_FrameEnd(0);
}

void CitroReleaseTexture(TextureId) {}

}  // namespace

const IRenderBackend kRenderBackend = {
    CitroInit,
    CitroShutdown,
    CitroSubmit,
    CitroReleaseTexture,
    "3ds-citro2d",
};

}  // namespace eng
