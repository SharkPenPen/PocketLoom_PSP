// PocketLoom 3DS 阶段 1：只验证平台初始化、清屏、输入、退出。
#include <3ds.h>
#include <citro2d.h>

int main(void) {
  gfxInitDefault();
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
  C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
  C2D_Prepare();

  C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
  C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

  while (aptMainLoop()) {
    hidScanInput();
    if ((hidKeysDown() & KEY_START) != 0) break;

    C3D_FrameBegin(0);
    C2D_TargetClear(top, C2D_Color32(16, 16, 32, 255));
    C2D_SceneBegin(top);
    C2D_DrawRectSolid(40.0f, 40.0f, 0.0f, 120.0f, 80.0f,
                      C2D_Color32(255, 32, 32, 255));

    C2D_TargetClear(bottom, C2D_Color32(8, 8, 16, 255));
    C2D_SceneBegin(bottom);
    C2D_DrawRectSolid(20.0f, 20.0f, 0.0f, 80.0f, 40.0f,
                      C2D_Color32(32, 160, 255, 255));
    C3D_FrameEnd(0);
  }

  C2D_Fini();
  C3D_Fini();
  gfxExit();
  return 0;
}
