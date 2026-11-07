// engine/render/draw_list.cpp
#include "render/draw_list.h"

#if defined(__psp__)
#include <pspiofilemgr.h>
static void DropTrace(const char* tag, int a, int b, int c, int d) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s %d %d %d %d\n", tag, a, b, c, d);
  const SceUID fd = sceIoOpen("ms0:/PL_DROP.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) return;
  sceIoWrite(fd, buf, static_cast<unsigned int>(std::strlen(buf)));
  sceIoClose(fd);
}
#else
#define DropTrace(tag, a, b, c, d) ((void)0)
#endif

#include <cstdio>

#include "base/log.h"
#include "base/math.h"

namespace eng {

namespace {

// ★ 定点整数打印：value × 256，四舍五入 ★
//   为什么不用 %f 见 draw_list.h 的说明 —— 那会让"两端逐行一致"
//   变成"比两边的 libc"，是个假判据。
int Fixed256(f32 v) {
  const f32 s = v * 256.0f;
  return static_cast<int>(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

// 几何取整（像素对齐）—— 在录命令时就落地。
// 软光栅按 floor 落格；小数坐标会让采样第一列算出负数，出现 1px 黑缝。
// 所以录命令时就把矩形对齐到整数像素。
//
// 位置和尺寸各自四舍五入：相邻图块仍然首尾相接（x=4.7 与 x=12.7 的两块
// 变成 5 和 13），不会因为取整裂开或重叠。
f32 SnapPx(f32 v) { return Floor(v + 0.5f); }

Rect2 SnapRect(const Rect2& r) {
  return Rect2{Vec2{SnapPx(r.position.x), SnapPx(r.position.y)},
               Vec2{SnapPx(r.size.x), SnapPx(r.size.y)}};
}

}  // namespace

const char* DrawCmdKindName(DrawCmdKind k) {
  switch (k) {
    case DrawCmdKind::kClear: return "Clear";
    case DrawCmdKind::kRect:  return "Rect";
    case DrawCmdKind::kImage: return "Image";
  }
  return "?";
}

// ===========================================================================
void DrawList::Reset() {
  cmds_.clear();
  overflowed_ = false;
  dropped_ = 0;
  xform_depth_ = 0;
  clip_depth_ = 0;
  warned_rotation_ = false;
  warned_overflow_ = false;
  warned_nosize_ = false;
  warned_badtex_ = false;
  warned_badregion_ = false;
}

void DrawList::ResetScreenSizes() {
  w_ = 0;
  h_ = 0;
}

void DrawList::SetScreenSize(int w, int h) {
  w_ = w;
  h_ = h;
}

Rect2 DrawList::FullRect() const {
  return Rect2{Vec2{0.0f, 0.0f},
               Vec2{static_cast<f32>(w_), static_cast<f32>(h_)}};
}

const char* DrawList::WhyEmpty() const {
  if (w_ == 0 && h_ == 0) {
    return "屏幕尺寸没设（SetScreenSize 没调用）";
  }
  return nullptr;
}

// ===========================================================================
// 变换栈
// ===========================================================================
void DrawList::PushTransform(const Mat3& m) {
  if (xform_depth_ >= kMaxTransformDepth) {
    log_printf(LogLevel::kWarn, "drawlist: transform 栈溢出（上限 %d），这一层被丢掉",
               kMaxTransformDepth);
    return;
  }
  // ★ 新变换 = 当前变换 ∘ 新变换 ★（先应用新的，再应用已有的）
  //   这样 PushTransform(T) 的直觉是"在当前坐标系里再平移/旋转 T"。
  xform_stack_[xform_depth_] = CurrentTransform() * m;
  ++xform_depth_;
}

void DrawList::PopTransform() {
  if (xform_depth_ == 0) {
    log_printf(LogLevel::kWarn, "drawlist: transform 栈已空还 Pop");
    return;
  }
  --xform_depth_;
}

Mat3 DrawList::CurrentTransform() const {
  if (xform_depth_ == 0) {
    // ★ 按值返回单位矩阵，不用静态对象 ★（D-03a 禁全局构造，
    //   而 Mat3::Identity() 不是 constexpr，做不了常量初始化的静态量）
    return Mat3::Identity();
  }
  return xform_stack_[xform_depth_ - 1];
}

// ===========================================================================
// 裁剪栈
// ===========================================================================
void DrawList::PushClip(const Rect2& r) {
  if (clip_depth_ >= kMaxClipDepth) {
    log_printf(LogLevel::kWarn, "drawlist: clip 栈溢出（上限 %d），这一层被丢掉",
               kMaxClipDepth);
    return;
  }
  // ★ 与当前裁剪求交 ★ —— 裁剪只会越裁越小，这与所有 GUI 框架的直觉一致
  clip_stack_[clip_depth_] = r.Intersection(CurrentClip());
  ++clip_depth_;
}

void DrawList::PopClip() {
  if (clip_depth_ == 0) {
    log_printf(LogLevel::kWarn, "drawlist: clip 栈已空还 Pop");
    return;
  }
  --clip_depth_;
}

Rect2 DrawList::CurrentClip() const {
  if (clip_depth_ == 0) {
    return FullRect();
  }
  return clip_stack_[clip_depth_ - 1];
}

// ===========================================================================
// 录制
// ===========================================================================
Rect2 DrawList::Bake(const Rect2& r) const {
  const Mat3 m = CurrentTransform();

  // ★ M1 的矩形后端只画轴对齐矩形 ★，所以这里取 4 个角点的 AABB。
  //   如果变换里含旋转/切变，AABB 会变大 —— ★ 那是会看出来的错误 ★，
  //   所以明确告警，而不是静默地画个变大的框。
  //   （旋转矩形要等 M2 的多边形/可旋转精灵路径。）
  if (m.m[0][1] != 0.0f || m.m[1][0] != 0.0f) {
    if (!warned_rotation_) {
      warned_rotation_ = true;
      log_printf(LogLevel::kWarn,
                 "drawlist: 变换含旋转/切变，矩形按 AABB 烘焙（会变大）—— M2 才支持旋转矩形");
    }
  }

  const Vec2 p0 = TransformPoint(m, Vec2{r.position.x, r.position.y});
  const Vec2 p1 = TransformPoint(m, Vec2{r.position.x + r.size.x, r.position.y});
  const Vec2 p2 = TransformPoint(m, Vec2{r.position.x, r.position.y + r.size.y});
  const Vec2 p3 = TransformPoint(m, Vec2{r.position.x + r.size.x, r.position.y + r.size.y});

  const f32 minx = Min(Min(p0.x, p1.x), Min(p2.x, p3.x));
  const f32 miny = Min(Min(p0.y, p1.y), Min(p2.y, p3.y));
  const f32 maxx = Max(Max(p0.x, p1.x), Max(p2.x, p3.x));
  const f32 maxy = Max(Max(p0.y, p1.y), Max(p2.y, p3.y));
  return Rect2{Vec2{minx, miny}, Vec2{maxx - minx, maxy - miny}};
}

void DrawList::Push(DrawCmd c) {
  if (!cmds_.push_back(c)) {
    overflowed_ = true;
    ++dropped_;
    if (!warned_overflow_) {
      warned_overflow_ = true;
      log_printf(LogLevel::kError, "drawlist: 命令满了（上限 %d），后续命令被丢掉", kMaxCmds);
    }
  }
}

void DrawList::Clear(u32 rgba) {
  DrawCmd c;
  c.kind = DrawCmdKind::kClear;
  // ★ Clear 就是"画一个铺满当前屏的矩形" ★
  //   这样两端后端的实现完全统一，而且 Clear 出现在流的中间也语义正确
  c.rect = FullRect();
  c.clip = c.rect;  // Clear 不受裁剪栈影响 —— 它清的是整个目标
  c.color = rgba;
  Push(c);
}

void DrawList::DrawRect(const Rect2& r, u32 rgba) {
  if (w_ == 0 && h_ == 0) {
    if (!warned_nosize_) {
      warned_nosize_ = true;
      log_printf(LogLevel::kError, "drawlist: SetScreenSize 没调用，无法确定裁剪范围");
    }
    return;
  }
  DrawCmd c;
  c.kind = DrawCmdKind::kRect;
  c.rect = SnapRect(Bake(r));
  c.clip = CurrentClip();
  c.color = rgba;
  Push(c);
}

void DrawList::DrawImage(const Rect2& dst, TextureId tex, u32 tint, TexRegion src,
                         u8 flags) {
  if (w_ == 0 && h_ == 0) {
    if (!warned_nosize_) {
      warned_nosize_ = true;
      log_printf(LogLevel::kError, "drawlist: SetScreenSize 没调用，无法确定裁剪范围");
    }
    return;
  }
  // ★ 纹理不存在就明确拒绝并记日志 ★ —— 记一条注定画不出来的命令没有意义，
  //   而且会让命令流 diff 出现"两端都画不出来"的假一致。
  const Texture* t = TextureGet(tex);
  if (t == nullptr || !t->valid()) {
    DropTrace("badtex", static_cast<int>(tex), t != nullptr, t ? t->width : 0, t ? t->height : 0);
    if (!warned_badtex_) {
      warned_badtex_ = true;
      log_printf(LogLevel::kError, "drawlist: 纹理 %u 无效，Image 命令被丢掉",
                 static_cast<unsigned>(tex));
    }
    return;
  }

  // ★ 源区域校验：越界就明确拒绝，不静默钳位 ★
  //   钳位看起来"友好"，但会把"我用错了格子"变成"画出来是另一块" ——
  //   那正是最难查的那类错。宁可什么都不画 + 一条日志。
  if (!src.whole()) {
    if (src.x < 0 || src.y < 0 || src.w <= 0 || src.h <= 0 ||
        src.x + src.w > t->width || src.y + src.h > t->height) {
      DropTrace("badregion", static_cast<int>(tex), src.x, src.y, src.w, src.h);
      if (!warned_badregion_) {
        warned_badregion_ = true;
        log_printf(LogLevel::kError,
                   "drawlist: 源区域 %d,%d,%d,%d 超出纹理 %u 的 %dx%d，Image 命令被丢掉",
                   src.x, src.y, src.w, src.h, static_cast<unsigned>(tex), t->width,
                   t->height);
      }
      return;
    }
  }

  DrawCmd c;
  c.kind = DrawCmdKind::kImage;
  c.flags = flags;
  c.rect = SnapRect(Bake(dst));
  c.clip = CurrentClip();
  c.color = tint;
  c.texture = tex;
  c.src = src.whole() ? TexRegion::Make(0, 0, t->width, t->height) : src;
  Push(c);
}

// ===========================================================================
// 命令流文本导出（T1.6）—— ★ 逐行回调，不在栈上开大缓冲 ★
// ===========================================================================
void DrawListDump(const DrawList& dl, DrawDumpSink sink, void* user) {
  if (sink == nullptr) {
    return;
  }
  // 用固定小缓冲，避免大栈帧。
  char line[256];

  std::snprintf(line, sizeof(line), "cmds=%d overflow=%d dropped=%d\n", dl.count(),
                dl.overflowed() ? 1 : 0, dl.dropped());
  sink(user, line);

  for (int i = 0; i < dl.count(); ++i) {
    const DrawCmd& c = dl.cmd(i);
    if (c.kind == DrawCmdKind::kImage) {
      // ★ Image 多两个字段：源区域与翻转 ★
      //   只在 Image 行上加，Clear/Rect 的行格式不变 ——
      //   这样已有的格式断言不用动，diff 里也只有真正相关的行会变。
      char flip[3];
      flip[0] = (c.flags & kDrawFlipH) ? 'H' : '-';
      flip[1] = (c.flags & kDrawFlipV) ? 'V' : '-';
      flip[2] = '\0';
      std::snprintf(line, sizeof(line),
                    "[%d] %s rect=%d,%d,%d,%d clip=%d,%d,%d,%d color=%08X tex=%u "
                    "src=%d,%d,%d,%d flip=%s\n",
                    i, DrawCmdKindName(c.kind),
                    Fixed256(c.rect.position.x), Fixed256(c.rect.position.y),
                    Fixed256(c.rect.size.x), Fixed256(c.rect.size.y),
                    Fixed256(c.clip.position.x), Fixed256(c.clip.position.y),
                    Fixed256(c.clip.size.x), Fixed256(c.clip.size.y),
                    static_cast<unsigned>(c.color), static_cast<unsigned>(c.texture),
                    static_cast<int>(c.src.x), static_cast<int>(c.src.y),
                    static_cast<int>(c.src.w), static_cast<int>(c.src.h), flip);
    } else {
      std::snprintf(line, sizeof(line),
                    "[%d] %s rect=%d,%d,%d,%d clip=%d,%d,%d,%d color=%08X tex=%u\n", i,
                    DrawCmdKindName(c.kind),
                    Fixed256(c.rect.position.x), Fixed256(c.rect.position.y),
                    Fixed256(c.rect.size.x), Fixed256(c.rect.size.y),
                    Fixed256(c.clip.position.x), Fixed256(c.clip.position.y),
                    Fixed256(c.clip.size.x), Fixed256(c.clip.size.y),
                    static_cast<unsigned>(c.color), static_cast<unsigned>(c.texture));
    }
    sink(user, line);
  }
}

}  // namespace eng
