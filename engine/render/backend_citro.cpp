// PocketLoom 3DS backend: DrawList -> citro2d.
#include "render/draw_list.h"
#include "render/swizzle.h"
#include "base/log.h"

#include <3ds.h>
#include <citro2d.h>

namespace eng {
namespace {

C3D_RenderTarget* g_top = nullptr;
C3D_RenderTarget* g_bottom = nullptr;
bool g_ready = false;
bool g_warned_badtex = false;

struct GpuTex {
  TextureId id = 0;
  bool flip_v = false;
  bool ok = false;
  C3D_Tex tex{};
};
GpuTex g_gpu[kMaxTextures] = {};
int g_gpu_count = 0;

C3D_Tex* GpuTextureFor(TextureId id, bool flip_v) {
  for (int i = 0; i < g_gpu_count; ++i) {
    if (g_gpu[i].id == id && g_gpu[i].flip_v == flip_v) {
      return g_gpu[i].ok ? &g_gpu[i].tex : nullptr;
    }
  }
  if (g_gpu_count >= kMaxTextures) return nullptr;
  const Texture* t = TextureGet(id);
  if (t == nullptr || !t->valid()) return nullptr;
  if ((t->width & 7) != 0 || (t->height & 7) != 0) {
    log_printf(LogLevel::kError, "3ds: texture %u size %dx%d not multiple of 8",
               static_cast<unsigned>(id), t->width, t->height);
    return nullptr;
  }
  GpuTex& slot = g_gpu[g_gpu_count++];
  slot.id = id;
  slot.flip_v = flip_v;
  slot.ok = false;
  if (!C3D_TexInit(&slot.tex, static_cast<u16>(t->width),
                   static_cast<u16>(t->height), GPU_RGBA8)) {
    log_printf(LogLevel::kError, "3ds: C3D_TexInit failed %dx%d", t->width, t->height);
    return nullptr;
  }
  C3D_TexSetFilter(&slot.tex, GPU_NEAREST, GPU_NEAREST);
  C3D_TexSetWrap(&slot.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
  u32 gpu_size = 0;
  void* gpu_mem = C3D_Tex2DGetImagePtr(&slot.tex, 0, &gpu_size);
  const u32 need = static_cast<u32>(t->width) * static_cast<u32>(t->height) * 4u;
  if (gpu_mem == nullptr || gpu_size < need ||
      !SwizzleRgba8To3ds(t->rgba8, t->width, t->height,
                         static_cast<u8*>(gpu_mem), flip_v)) {
    C3D_TexDelete(&slot.tex);
    return nullptr;
  }
  C3D_TexFlush(&slot.tex);
  slot.ok = true;
  return &slot.tex;
}

void ReleaseTextures() {
  for (int i = 0; i < g_gpu_count; ++i) {
    if (g_gpu[i].ok) C3D_TexDelete(&g_gpu[i].tex);
  }
  g_gpu_count = 0;
}

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
  if (g_ready) ReleaseTextures();
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
    if (dl.cmd(i).kind == DrawCmdKind::kClear) { clear = dl.cmd(i).color; break; }
  }
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  C2D_TargetClear(g_top, clear);
  C2D_SceneBegin(g_top);
  for (int i = 0; i < dl.count(); ++i) {
    const DrawCmd& c = dl.cmd(i);
    if (c.kind == DrawCmdKind::kRect) {
      C2D_DrawRectangle(c.rect.position.x, c.rect.position.y, 0.0f,
                        c.rect.size.x, c.rect.size.y,
                        c.color, c.color, c.color, c.color);
      continue;
    }
    if (c.kind != DrawCmdKind::kImage) continue;
    const Texture* t = TextureGet(c.texture);
    const bool flip_h = (c.flags & kDrawFlipH) != 0;
    const bool flip_v = (c.flags & kDrawFlipV) != 0;
    C3D_Tex* gpu = t != nullptr ? GpuTextureFor(c.texture, flip_v) : nullptr;
    if (t == nullptr || gpu == nullptr) {
      if (!g_warned_badtex) {
        g_warned_badtex = true;
        log_printf(LogLevel::kError, "3ds: image texture invalid");
      }
      continue;
    }
    const int sw = c.src.whole() ? t->width : static_cast<int>(c.src.w);
    const int sh = c.src.whole() ? t->height : static_cast<int>(c.src.h);
    const int sx = c.src.whole() ? 0 : static_cast<int>(c.src.x);
    const int sy = c.src.whole() ? 0 : static_cast<int>(c.src.y);
    if (sw <= 0 || sh <= 0) continue;
    const f32 inv_w = 1.0f / static_cast<f32>(t->width);
    const f32 inv_h = 1.0f / static_cast<f32>(t->height);
    f32 u_left = static_cast<f32>(sx) * inv_w;
    f32 u_right = static_cast<f32>(sx + sw) * inv_w;
    if (flip_h) { const f32 tmp = u_left; u_left = u_right; u_right = tmp; }
    Tex3DS_SubTexture sub;
    sub.width = static_cast<u16>(sw);
    sub.height = static_cast<u16>(sh);
    sub.left = u_left;
    sub.top = 1.0f - static_cast<f32>(sy) * inv_h;
    sub.right = u_right;
    sub.bottom = 1.0f - static_cast<f32>(sy + sh) * inv_h;
    C2D_Image img;
    img.tex = gpu;
    img.subtex = &sub;
    C2D_ImageTint tint;
    const C2D_ImageTint* tint_ptr = nullptr;
    if (c.color != 0xFFFFFFFFu) {
      const bool white = (c.color & 0x00FFFFFFu) == 0x00FFFFFFu;
      C2D_PlainImageTint(&tint, c.color, white ? 0.0f : 1.0f);
      tint_ptr = &tint;
    }
    C2D_DrawImageAt(img, c.rect.position.x, c.rect.position.y, 0.0f, tint_ptr,
                    c.rect.size.x / static_cast<f32>(sw),
                    c.rect.size.y / static_cast<f32>(sh));
  }
  C2D_TargetClear(g_bottom, C2D_Color32(8, 8, 16, 255));
  C2D_SceneBegin(g_bottom);
  C3D_FrameEnd(0);
}

void CitroReleaseTexture(TextureId id) {
  for (int i = 0; i < g_gpu_count; ++i) {
    if (g_gpu[i].id != id) continue;
    if (g_gpu[i].ok) C3D_TexDelete(&g_gpu[i].tex);
    g_gpu[i] = g_gpu[g_gpu_count - 1];
    --g_gpu_count;
    --i;
  }
}

}  // namespace

const IRenderBackend kRenderBackend = {
    CitroInit, CitroShutdown, CitroSubmit, CitroReleaseTexture, "3ds-citro2d",
};

}  // namespace eng
