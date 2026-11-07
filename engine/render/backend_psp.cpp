// engine/render/backend_psp.cpp
//
// PSP GU 后端（第一版骨架）。
//
// 设计目标：
//   - 只实现 DrawList 的 3 种命令：clear / rect / image；
//   - 纹理从 engine/render/texture.h 的 RGBA8 懒上传；
//   - 因为 PSP GU 需要 2 的幂纹理，这里在 CPU 侧补边到 POT；
//   - 裁剪用 GU scissor，翻转只改 UV，不复制纹理。
//
// 已知限制：
//   1. 纹理尺寸超过 1024 会被拒绝；
//   2. 颜色格式按 GU 的 ABGR 顺序解释，真机需要一次颜色验证；
//   3. 未做 3D、未做视频、未做双缓冲以外的呈现策略；
//   4. 性能未测，先保证语义和 DrawList 对齐。
#include "platform/platform.h"
#include "render/draw_list.h"
#include "render/texture.h"

#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>

#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include "base/log.h"

namespace eng {
namespace {

constexpr int kScreenW = 480;
constexpr int kScreenH = 272;
constexpr int kBufferW = 512;
constexpr int kMaxTexture = kMaxTextures;

// 256 KB 命令列表；PSP 的 GU 命令缓冲必须 16 字节对齐。
alignas(16) u32 g_list[0x40000 / 4];

struct PspTexture {
  const u8* source = nullptr;
  u32* data = nullptr;
  int width = 0;
  int height = 0;
  int pot_w = 0;
  int pot_h = 0;
  bool used = false;
};

PspTexture g_textures[kMaxTexture];
bool g_ready = false;

void GpuTrace(const char* tag, int value) {
  char buf[96];
  int n = 0;
  while (tag[n] != '\0' && n < 60) { buf[n] = tag[n]; ++n; }
  if (value >= 0) {
    buf[n++] = ' ';
    char d[12];
    int dn = 0;
    if (value == 0) d[dn++] = '0';
    while (value > 0) { d[dn++] = static_cast<char>('0' + value % 10); value /= 10; }
    while (dn > 0) buf[n++] = d[--dn];
  }
  buf[n++] = '\n';
  const SceUID fd = sceIoOpen("ms0:/PL_GPU.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) return;
  sceIoWrite(fd, buf, static_cast<unsigned int>(n));
  sceIoClose(fd);
}

int NextPow2(int v) {
  int p = 1;
  while (p < v) p <<= 1;
  return p;
}

void* AllocAligned16(size_t size) {
#if defined(__psp__) || defined(_PSP) || defined(__PSP__)
  return memalign(16, size);
#elif defined(_WIN32)
  // 仅用于 host 侧语法检查；PSP 实际走 memalign。
  return _aligned_malloc(size, 16);
#else
  void* p = nullptr;
  return posix_memalign(&p, 16, size) == 0 ? p : nullptr;
#endif
}

void ScissorFromRect(const Rect2& r) {
  int x = static_cast<int>(r.position.x);
  int y = static_cast<int>(r.position.y);
  int w = static_cast<int>(r.size.x);
  int h = static_cast<int>(r.size.y);
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > kScreenW) w = kScreenW - x;
  if (y + h > kScreenH) h = kScreenH - y;
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  sceGuScissor(x, y, w, h);
}

u32 ToGuColor(u32 rgba) {
  // engine 的颜色是 R 在最低字节（0xAABBGGRR）；
  // GU 的顶点颜色同样是字节序 R,G,B,A，因此直接传。
  return rgba;
}

void ResetStates() {
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuDisable(GU_CULL_FACE);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuScissor(0, 0, kScreenW, kScreenH);
}

bool EnsureTexture(TextureId id, const Texture** out, PspTexture** out_tex) {
  if (id == kInvalidTexture || id > static_cast<TextureId>(kMaxTexture)) return false;
  const Texture* tex = TextureGet(id);
  if (tex == nullptr || !tex->valid()) return false;

  PspTexture& slot = g_textures[id - 1];

  // 同一个纹理 id 可能被重新创建；source/宽高变化时重新上传。
  if (slot.used && slot.source == tex->rgba8 && slot.width == tex->width &&
      slot.height == tex->height) {
    *out = tex;
    *out_tex = &slot;
    return true;
  }

  const int pw = NextPow2(tex->width);
  const int ph = NextPow2(tex->height);
  if (pw <= 0 || ph <= 0 || pw > 1024 || ph > 1024) {
    log_printf(LogLevel::kError, "psp: texture %u too large (%dx%d)", id, tex->width,
               tex->height);
    return false;
  }

  if (slot.data == nullptr || slot.pot_w != pw || slot.pot_h != ph) {
    std::free(slot.data);
    slot.data = static_cast<u32*>(AllocAligned16(static_cast<size_t>(pw) * ph * 4));
    if (slot.data == nullptr) {
      log_printf(LogLevel::kError, "psp: texture %u alloc failed", id);
      slot.used = false;
      return false;
    }
    std::memset(slot.data, 0, static_cast<size_t>(pw) * ph * 4);
  }

  for (int y = 0; y < tex->height; ++y) {
    std::memcpy(slot.data + static_cast<size_t>(y) * pw,
                tex->rgba8 + static_cast<size_t>(y) * tex->width * 4,
                static_cast<size_t>(tex->width) * 4);
  }
  sceKernelDcacheWritebackAll();

  slot.source = tex->rgba8;
  slot.width = tex->width;
  slot.height = tex->height;
  slot.pot_w = pw;
  slot.pot_h = ph;
  slot.used = true;

  *out = tex;
  *out_tex = &slot;
  return true;
}

struct VtxTex {
  float u, v;
  u32 color;
  float x, y, z;
};

struct VtxColor {
  u32 color;
  float x, y, z;
};

void DrawImage(const DrawCmd& cmd) {
  const Texture* tex = nullptr;
  PspTexture* pt = nullptr;
  if (!EnsureTexture(cmd.texture, &tex, &pt)) return;

  const int sw = cmd.src.whole() ? pt->width : static_cast<int>(cmd.src.w);
  const int sh = cmd.src.whole() ? pt->height : static_cast<int>(cmd.src.h);
  const int sx = cmd.src.whole() ? 0 : static_cast<int>(cmd.src.x);
  const int sy = cmd.src.whole() ? 0 : static_cast<int>(cmd.src.y);
  if (sw <= 0 || sh <= 0) return;

  float u0 = static_cast<float>(sx) / static_cast<float>(pt->pot_w);
  float v0 = static_cast<float>(sy) / static_cast<float>(pt->pot_h);
  float u1 = static_cast<float>(sx + sw) / static_cast<float>(pt->pot_w);
  float v1 = static_cast<float>(sy + sh) / static_cast<float>(pt->pot_h);

  if ((cmd.flags & kDrawFlipH) != 0) {
    const float t = u0;
    u0 = u1;
    u1 = t;
  }
  if ((cmd.flags & kDrawFlipV) != 0) {
    const float t = v0;
    v0 = v1;
    v1 = t;
  }

  const float x0 = cmd.rect.position.x;
  const float y0 = cmd.rect.position.y;
  const float x1 = x0 + cmd.rect.size.x;
  const float y1 = y0 + cmd.rect.size.y;
  const u32 color = ToGuColor(cmd.color);

  const VtxTex v[4] = {
      {u0, v0, color, x0, y0, 0.0f},
      {u1, v0, color, x1, y0, 0.0f},
      {u0, v1, color, x0, y1, 0.0f},
      {u1, v1, color, x1, y1, 0.0f},
  };

  ScissorFromRect(cmd.clip);

  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_8888, 0, 0, 0);
  sceGuTexImage(0, pt->pot_w, pt->pot_h, pt->pot_w, pt->data);
  sceGuTexSync();
  sceGuColor(color);
  sceGuDrawArray(GU_TRIANGLE_STRIP,
                 GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                 4, nullptr, v);
}

void DrawRect(const DrawCmd& cmd) {
  const float x0 = cmd.rect.position.x;
  const float y0 = cmd.rect.position.y;
  const float x1 = x0 + cmd.rect.size.x;
  const float y1 = y0 + cmd.rect.size.y;
  const u32 color = ToGuColor(cmd.color);

  const VtxColor v[4] = {
      {color, x0, y0, 0.0f},
      {color, x1, y0, 0.0f},
      {color, x0, y1, 0.0f},
      {color, x1, y1, 0.0f},
  };

  ScissorFromRect(cmd.clip);

  sceGuDisable(GU_TEXTURE_2D);
  sceGuColor(color);
  sceGuDrawArray(GU_TRIANGLE_STRIP,
                 GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, nullptr, v);
}

bool PspInit() {
  sceGuInit();

  sceGuStart(GU_DIRECT, g_list);
  sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void*>(0), kBufferW);
  sceGuDispBuffer(kScreenW, kScreenH, reinterpret_cast<void*>(0x88000), kBufferW);
  sceGuDepthBuffer(reinterpret_cast<void*>(0x110000), kBufferW);
  sceGuOffset(2048 - (kScreenW / 2), 2048 - (kScreenH / 2));
  sceGuViewport(2048, 2048, kScreenW, kScreenH);
  sceGuScissor(0, 0, kScreenW, kScreenH);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuFinish();
  sceGuSync(0, 0);
  sceGuDisplay(GU_TRUE);

  g_ready = true;
  return true;
}

void PspShutdown() {
  for (auto& t : g_textures) {
    std::free(t.data);
    t = PspTexture{};
  }
  if (g_ready) {
    sceGuDisplay(GU_FALSE);
    sceGuTerm();
    g_ready = false;
  }
}

void PspSubmit(const DrawList& dl) {
  const char* why = dl.WhyEmpty();
  if (why != nullptr) {
    log_printf(LogLevel::kError, "psp: %s", why);
    return;
  }
  if (!g_ready) return;
  GpuTrace("submit_enter", dl.count());

  sceGuStart(GU_DIRECT, g_list);

  sceGumMatrixMode(GU_PROJECTION);
  sceGumLoadIdentity();
  sceGumOrtho(0.0f, static_cast<float>(dl.ScreenWidth()),
              static_cast<float>(dl.ScreenHeight()), 0.0f, -1.0f, 1.0f);
  sceGumMatrixMode(GU_VIEW);
  sceGumLoadIdentity();
  sceGumMatrixMode(GU_MODEL);
  sceGumLoadIdentity();

  ResetStates();

  for (int i = 0; i < dl.count(); ++i) {
    GpuTrace("cmd", i);
    const DrawCmd& cmd = dl.cmd(i);
    switch (cmd.kind) {
      case DrawCmdKind::kClear:
        GpuTrace("clear", -1);
        sceGuScissor(0, 0, kScreenW, kScreenH);
        sceGuClearColor(ToGuColor(cmd.color));
        sceGuClear(GU_COLOR_BUFFER_BIT);
        break;
      case DrawCmdKind::kRect:
        GpuTrace("rect", -1);
        DrawRect(cmd);
        break;
      case DrawCmdKind::kImage:
        GpuTrace("image", -1);
        DrawImage(cmd);
        break;
    }
    GpuTrace("cmd_done", i);
  }

  GpuTrace("finish", -1);
  sceGuFinish();
  GpuTrace("sync", -1);
  sceGuSync(0, 0);
  GpuTrace("vblank", -1);
  sceDisplayWaitVblankStart();
  GpuTrace("swap", -1);
  sceGuSwapBuffers();
  GpuTrace("submit_done", -1);
}

void PspReleaseTexture(TextureId id) {
  if (id == kInvalidTexture || id > static_cast<TextureId>(kMaxTexture)) return;
  PspTexture& t = g_textures[id - 1];
  std::free(t.data);
  t = PspTexture{};
}

}  // namespace

const IRenderBackend kRenderBackend = {
    PspInit,
    PspShutdown,
    PspSubmit,
    PspReleaseTexture,
    "psp-gu",
};

}  // namespace eng
