// runtime/p3d.cpp
#include "runtime/p3d.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace r4 {

namespace {

void SetErr(std::string* error, const char* msg) {
  if (error != nullptr) *error = msg;
}

struct Reader {
  const unsigned char* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  bool ok = true;

  bool Need(size_t n) {
    if (!ok || n > size - pos) {
      ok = false;
      return false;
    }
    return true;
  }
  uint8_t U8() {
    if (!Need(1)) return 0;
    return data[pos++];
  }
  uint16_t U16() {
    if (!Need(2)) return 0;
    const uint16_t v = static_cast<uint16_t>(data[pos]) |
                       static_cast<uint16_t>(data[pos + 1]) << 8;
    pos += 2;
    return v;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    const uint32_t v = static_cast<uint32_t>(data[pos]) |
                       (static_cast<uint32_t>(data[pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[pos + 2]) << 16) |
                       (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return v;
  }
  int32_t I32() { return static_cast<int32_t>(U32()); }
  std::string Str() {
    const uint16_t n = U16();
    if (!Need(n)) return {};
    std::string s(reinterpret_cast<const char*>(data + pos), n);
    pos += n;
    return s;
  }
  bool Bytes(unsigned char* out, size_t n) {
    if (!Need(n)) return false;
    std::memcpy(out, data + pos, n);
    pos += n;
    return true;
  }
};

struct Writer {
  std::vector<unsigned char> data;
  bool ok = true;

  void U8(uint8_t v) { data.push_back(v); }
  void U16(uint16_t v) {
    data.push_back(static_cast<unsigned char>(v & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
  }
  void U32(uint32_t v) {
    data.push_back(static_cast<unsigned char>(v & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    data.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
  }
  void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
  void Str(const std::string& s) {
    if (s.size() > 4096) { ok = false; return; }
    U16(static_cast<uint16_t>(s.size()));
    data.insert(data.end(), s.begin(), s.end());
  }
  void Bytes(const unsigned char* p, size_t n) {
    data.insert(data.end(), p, p + n);
  }
};

bool ReadCount(Reader& r, uint32_t* out, uint32_t max, std::string* error,
               const char* what) {
  *out = r.U32();
  if (!r.ok || *out > max) {
    if (error != nullptr) {
      *error = std::string("p3d: bad count for ") + what;
    }
    return false;
  }
  return true;
}

bool ReadImage(Reader& r, Image* img, std::string* error) {
  img->id = r.Str();
  img->width = r.I32();
  img->height = r.I32();
  if (!r.ok || img->width <= 0 || img->height <= 0 || img->width > 4096 ||
      img->height > 4096) {
    SetErr(error, "p3d: bad image size");
    return false;
  }
  const size_t bytes = static_cast<size_t>(img->width) *
                       static_cast<size_t>(img->height) * 4u;
  img->rgba.resize(bytes);
  return r.Bytes(img->rgba.data(), bytes);
}

bool ReadAction(Reader& r, Action* a, std::string* error) {
  a->name = r.Str();
  const uint16_t n = r.U16();
  if (!r.ok || n > 64) {
    SetErr(error, "p3d: bad action frame count");
    return false;
  }
  a->frames.resize(n);
  for (uint16_t i = 0; i < n; ++i) {
    AnimFrame& f = a->frames[i];
    f.image = r.I32();
    f.hold = static_cast<int>(r.U16());
    f.origin_x = r.I32();
    f.origin_y = r.I32();
    f.col_x = r.I32();
    f.col_y = r.I32();
    f.col_w = r.I32();
    f.col_h = r.I32();
    f.attack.x = r.I32();
    f.attack.y = r.I32();
    f.attack.w = r.I32();
    f.attack.h = r.I32();
    f.hurt.x = r.I32();
    f.hurt.y = r.I32();
    f.hurt.w = r.I32();
    f.hurt.h = r.I32();
    const uint8_t attach_count = r.U8();
    for (uint8_t k = 0; k < attach_count && r.ok; ++k) {
      AttachPoint ap;
      ap.name = r.Str();
      ap.x = r.I32();
      ap.y = r.I32();
      f.attach.push_back(std::move(ap));
    }
    if (!r.ok) {
      SetErr(error, "p3d: bad action frame");
      return false;
    }
  }
  a->fps = r.I32();
  a->loop = (r.U8() != 0);
  a->offset_x = r.I32();
  a->offset_y = r.I32();
  a->flip_h = r.U8() != 0;
  a->flip_v = r.U8() != 0;
  if (!r.ok || a->fps <= 0) {
    SetErr(error, "p3d: bad action fps");
    return false;
  }
  return true;
}

bool ReadObject(Reader& r, Object* o, std::string* error) {
  o->id = r.Str();
  o->name = r.Str();
  o->type = r.Str();
  o->team = r.Str();
  o->image = r.I32();
  o->frame_w = r.I32();
  o->frame_h = r.I32();
  o->col_x = r.I32();
  o->col_y = r.I32();
  o->col_w = r.I32();
  o->col_h = r.I32();
  o->hurt_x = r.I32();
  o->hurt_y = r.I32();
  o->hurt_w = r.I32();
  o->hurt_h = r.I32();
  o->attack_x = r.I32();
  o->attack_y = r.I32();
  o->attack_w = r.I32();
  o->attack_h = r.I32();
  o->speed = r.I32();
  o->gravity = r.I32();
  const uint16_t ac = r.U16();
  if (!r.ok || ac > 32) {
    SetErr(error, "p3d: bad object action count");
    return false;
  }
  o->actions.resize(ac);
  for (uint16_t i = 0; i < ac; ++i) {
    if (!ReadAction(r, &o->actions[i], error)) return false;
  }
  o->default_action = r.I32();
  const uint16_t rn = r.U16();
  if (!r.ok || rn > 64) {
    SetErr(error, "p3d: bad rule count");
    return false;
  }
  o->rules.resize(rn);
  for (uint16_t i = 0; i < rn; ++i) {
    Rule& rl = o->rules[i];
    rl.name = r.Str();
    rl.enabled = r.U8() != 0;
    const uint8_t cn = r.U8();
    for (uint8_t k = 0; k < cn && r.ok; ++k) {
      RuleCond rc;
      rc.type = r.U8();
      rc.key = r.Str();
      rc.name = r.Str();
      rc.op = r.I32();
      rc.value = r.I32();
      rl.when.push_back(std::move(rc));
    }
    const uint8_t en = r.U8();
    for (uint8_t k = 0; k < en && r.ok; ++k) {
      RuleEffect re;
      re.type = r.U8();
      re.x = r.I32();
      re.y = r.I32();
      re.text = r.Str();
      rl.effects.push_back(std::move(re));
    }
  }
  o->limit_screen = r.U8();
  o->pushable = r.U8();
  if (!r.ok || o->frame_w <= 0 || o->frame_h <= 0 || o->col_w <= 0 ||
      o->col_h <= 0) {
    SetErr(error, "p3d: bad object frame/collision");
    return false;
  }
  return true;
}

bool ReadTileset(Reader& r, Tileset* t, std::string* error) {
  t->id = r.Str();
  t->image = r.I32();
  t->tile_w = r.I32();
  t->tile_h = r.I32();
  t->columns = r.I32();
  if (!r.ok || t->tile_w <= 0 || t->tile_h <= 0 || t->columns <= 0) {
    SetErr(error, "p3d: bad tileset");
    return false;
  }
  return r.ok;
}

bool ReadScene(Reader& r, Scene* s, std::string* error) {
  s->id = r.Str();
  s->bgm = r.Str();
  // v41: 图集页表（槽位 → P3d::tilesets 下标）
  const uint16_t pn = r.U16();
  if (!r.ok || pn > 64) {
    SetErr(error, "p3d: bad scene tileset count");
    return false;
  }
  s->tilesets.resize(pn);
  for (uint16_t i = 0; i < pn; ++i) s->tilesets[i] = r.I32();
  s->camera_mode = r.Str();
  s->camera_offset_x = r.I32();
  s->camera_offset_y = r.I32();
  s->camera_deadzone_w = r.I32();
  s->camera_deadzone_h = r.I32();
  const uint16_t vn = r.U16();
  if (!r.ok || vn > 256) {
    SetErr(error, "p3d: bad scene variable count");
    return false;
  }
  for (uint16_t i = 0; i < vn; ++i) {
    const std::string key = r.Str();
    s->variables[key] = r.I32();
  }
  const uint16_t tn = r.U16();
  if (!r.ok || tn > 512) {
    SetErr(error, "p3d: bad trigger count");
    return false;
  }
  for (uint16_t i = 0; i < tn; ++i) {
    Trigger t;
    t.x = r.I32();
    t.y = r.I32();
    t.w = r.I32();
    t.h = r.I32();
    t.once = r.U8() != 0;
    t.target_object = r.I32();
    t.require_variable = r.Str();
    t.require_value = r.I32();
    t.name = r.Str();
    t.color = r.U32();
    const uint16_t mn = r.U16();
    if (!r.ok || mn > 32) {
      SetErr(error, "p3d: bad trigger mod count");
      return false;
    }
    for (uint16_t m = 0; m < mn; ++m) {
      TriggerMod mod;
      mod.stat = r.U8();
      mod.op = r.U8();
      mod.value = static_cast<float>(r.I32()) / 1000.0f;
      t.mods.push_back(mod);
    }
    t.interaction.enabled = r.U8() != 0;
    if (t.interaction.enabled) {
      t.interaction.prompt = r.Str();
      t.interaction.ui_id = r.Str();
      t.interaction.anchor = r.Str();
      t.interaction.key = r.Str();
      t.interaction.once = r.U8() != 0;
      t.interaction.require_variable = r.Str();
      t.interaction.require_value = r.I32();
      const uint16_t ian = r.U16();
      for (uint16_t ia = 0; ia < ian && r.ok; ++ia) {
        TriggerAction a;
        a.type = r.U8();
        a.target = r.Str();
        a.value = r.I32();
        t.interaction.actions.push_back(std::move(a));
      }
    }
    t.inside_interval = r.I32();
    if (t.inside_interval < 1) t.inside_interval = 1;
    const uint16_t rn = r.U16();
    if (!r.ok || rn > 64) {
      SetErr(error, "p3d: bad zone rule count");
      return false;
    }
    for (uint16_t ri = 0; ri < rn; ++ri) {
      TriggerRule rule;
      rule.enabled = r.U8() != 0;
      rule.mode = r.U8();
      rule.require_variable = r.Str();
      rule.require_value = r.I32();
      const uint16_t an2 = r.U16();
      if (!r.ok || an2 > 16) {
        SetErr(error, "p3d: bad zone rule action count");
        return false;
      }
      for (uint16_t ai = 0; ai < an2; ++ai) {
        TriggerAction action;
        action.type = r.U8();
        action.target = r.Str();
        action.value = r.I32();
        rule.actions.push_back(std::move(action));
      }
      t.rules.push_back(std::move(rule));
    }
    s->triggers.push_back(std::move(t));
  }
  const uint16_t lc = r.U16();
  if (!r.ok || lc == 0 || lc > 8) {
    SetErr(error, "p3d: bad scene layer count");
    return false;
  }
  s->layers.resize(lc);
  for (uint16_t li = 0; li < lc; ++li) {
    SceneLayer& layer = s->layers[li];
    layer.visible = (r.U8() != 0);
    layer.scroll_x = r.I32();
    layer.scroll_y = r.I32();
    layer.foreground = (r.U8() != 0);
    uint32_t n = 0;
    if (!ReadCount(r, &n, 65536, error, "scene chunks")) return false;
    layer.chunks.resize(n);
    for (uint32_t ci = 0; ci < n; ++ci) {
      SceneChunk& c = layer.chunks[ci];
      c.cx = r.I32();
      c.cy = r.I32();
      uint32_t cell_count = 0;
      if (!ReadCount(r, &cell_count, 4096, error, "chunk cells")) return false;
      if (cell_count != static_cast<uint32_t>(kP3dChunkCells * kP3dChunkCells)) {
        SetErr(error, "p3d: chunk cell count must be 1024");
        return false;
      }
      c.cells.resize(cell_count);
      for (uint32_t k = 0; k < cell_count; ++k) c.cells[k] = r.I32();
    }
  }
  const uint16_t mc = r.U16();
  if (!r.ok || mc > 1024) {
    SetErr(error, "p3d: bad collision chunk count");
    return false;
  }
  s->collision.resize(mc);
  for (uint16_t ci = 0; ci < mc; ++ci) {
    SceneCollisionChunk& c = s->collision[ci];
    c.cx = r.I32();
    c.cy = r.I32();
    uint32_t n = 0;
    if (!ReadCount(r, &n, 4096, error, "collision masks")) return false;
    if (n != static_cast<uint32_t>(kP3dChunkCells * kP3dChunkCells)) {
      SetErr(error, "p3d: collision mask count must be 1024");
      return false;
    }
    c.masks.resize(n);
    for (uint32_t k = 0; k < n; ++k) c.masks[k] = r.U8();
  }
  const uint16_t oc = r.U16();
  if (!r.ok || oc > 4096) {
    SetErr(error, "p3d: bad scene object count");
    return false;
  }
  s->objects.resize(oc);
  for (uint16_t i = 0; i < oc; ++i) {
    s->objects[i].object = r.I32();
    s->objects[i].x = r.I32();
    s->objects[i].y = r.I32();
  }
  s->gravity = r.I32();
  s->controlled = r.I32();
  s->hud_ui = r.I32();
  if (!r.ok || s->layers.empty()) {
    SetErr(error, "p3d: bad scene");
    return false;
  }
  return true;
}

bool ReadTransition(Reader& r, Transition* t) {
  t->from = r.I32();
  t->to = r.I32();
  t->type = r.U8();
  t->value_int = r.I32();
  t->value_string = r.Str();
  t->set_variable = r.Str();
  t->set_value = r.I32();
  return r.ok;
}

bool ReadUiElement(Reader& r, UiElementData* e) {
  e->id = r.Str();
  e->kind = r.U8();
  e->anchor = r.U8();
  e->x = r.I32();
  e->y = r.I32();
  e->w = r.I32();
  e->h = r.I32();
  e->slice_l = r.I32();
  e->slice_r = r.I32();
  e->slice_t = r.I32();
  e->slice_b = r.I32();
  e->slice_tile = r.U8() != 0;
  e->image = r.I32();
  e->src_x = r.I32();
  e->src_y = r.I32();
  e->src_w = r.I32();
  e->src_h = r.I32();
  e->frames = r.I32();
  e->frame = r.I32();
  e->text = r.Str();
  e->font_size = r.I32();
  e->color = r.U32();
  e->align = r.U8();
  e->binding.name = r.Str();
  e->binding.map = r.U8();
  e->binding.in_min = r.I32();
  e->binding.in_max = r.I32();
  e->binding.out_min = r.I32();
  e->binding.out_max = r.I32();
  e->binding.format = r.Str();
  e->visible_var = r.Str();
  e->visible_value = r.I32();
  e->padding = r.I32();
  e->action = r.Str();
  e->action_target = r.Str();
  e->action_value = r.I32();
  e->only_if_save = r.U8() != 0;
  const uint16_t item_n = r.U16();
  if (!r.ok || item_n > 1024) return false;
  e->items.resize(item_n);
  for (uint16_t i = 0; i < item_n; ++i) e->items[i] = r.Str();
  const uint16_t cn = r.U16();
  if (!r.ok || cn > 4096) return false;
  e->children.resize(cn);
  for (uint16_t i = 0; i < cn; ++i) {
    if (!ReadUiElement(r, &e->children[i])) return false;
  }
  return r.ok;
}

bool ReadUiScreen(Reader& r, UiScreenData* s) {
  s->id = r.Str();
  s->name = r.Str();
  s->modal = r.U8() != 0;
  s->layout = r.U8();
  s->dismiss = r.U8();
  s->dismiss_frames = r.I32();
  s->fade_frames = r.I32();
  s->fade_out_frames = r.I32();
  const uint16_t n = r.U16();
  if (!r.ok || n > 4096) return false;
  s->elements.resize(n);
  for (uint16_t i = 0; i < n; ++i) {
    if (!ReadUiElement(r, &s->elements[i])) return false;
  }
  return r.ok;
}

void WriteUiElement(Writer& w, const UiElementData& e) {
  w.Str(e.id);
  w.U8(static_cast<uint8_t>(e.kind));
  w.U8(static_cast<uint8_t>(e.anchor));
  w.I32(e.x);
  w.I32(e.y);
  w.I32(e.w);
  w.I32(e.h);
  w.I32(e.slice_l);
  w.I32(e.slice_r);
  w.I32(e.slice_t);
  w.I32(e.slice_b);
  w.U8(e.slice_tile ? 1 : 0);
  w.I32(e.image);
  w.I32(e.src_x);
  w.I32(e.src_y);
  w.I32(e.src_w);
  w.I32(e.src_h);
  w.I32(e.frames);
  w.I32(e.frame);
  w.Str(e.text);
  w.I32(e.font_size);
  w.U32(e.color);
  w.U8(static_cast<uint8_t>(e.align));
  w.Str(e.binding.name);
  w.U8(static_cast<uint8_t>(e.binding.map));
  w.I32(e.binding.in_min);
  w.I32(e.binding.in_max);
  w.I32(e.binding.out_min);
  w.I32(e.binding.out_max);
  w.Str(e.binding.format);
  w.Str(e.visible_var);
  w.I32(e.visible_value);
  w.I32(e.padding);
  w.Str(e.action);
  w.Str(e.action_target);
  w.I32(e.action_value);
  w.U8(e.only_if_save ? 1 : 0);
  w.U16(static_cast<uint16_t>(std::min<size_t>(1024, e.items.size())));
  for (const std::string& item : e.items) w.Str(item);
  w.U16(static_cast<uint16_t>(std::min<size_t>(4096, e.children.size())));
  for (const UiElementData& c : e.children) WriteUiElement(w, c);
}

void WriteUiScreen(Writer& w, const UiScreenData& s) {
  w.Str(s.id);
  w.Str(s.name);
  w.U8(s.modal ? 1 : 0);
  w.U8(static_cast<uint8_t>(s.layout));
  w.U8(static_cast<uint8_t>(s.dismiss));
  w.I32(s.dismiss_frames);
  w.I32(s.fade_frames);
  w.I32(s.fade_out_frames);
  w.U16(static_cast<uint16_t>(std::min<size_t>(4096, s.elements.size())));
  for (const UiElementData& e : s.elements) WriteUiElement(w, e);
}

}  // namespace

bool P3dLoad(const unsigned char* data, size_t size, P3d* out,
             std::string* error) {
  if (out == nullptr || data == nullptr) {
    SetErr(error, "p3d: null input");
    return false;
  }
  Reader r{data, size, 0, true};
  if (r.U32() != kP3dMagic) {
    SetErr(error, "p3d: bad magic");
    return false;
  }
  const uint32_t version = r.U32();
  if (version != kP3dVersion) {
    SetErr(error, "p3d: unsupported version");
    return false;
  }
  P3d p;
  p.initial_scene = r.I32();
  p.view_w = r.I32();
  p.view_h = r.I32();
  if (p.view_w <= 0 || p.view_h <= 0) {
    p.view_w = 480;
    p.view_h = 272;
  }
  uint32_t count = 0;
  if (!ReadCount(r, &count, 64, error, "images")) return false;
  p.images.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!ReadImage(r, &p.images[i], error)) return false;
  }

  if (!ReadCount(r, &count, 1024, error, "objects")) return false;
  p.objects.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!ReadObject(r, &p.objects[i], error)) return false;
  }

  if (!ReadCount(r, &count, 512, error, "tilesets")) return false;
  p.tilesets.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!ReadTileset(r, &p.tilesets[i], error)) return false;
  }

  if (!ReadCount(r, &count, 2048, error, "scenes")) return false;
  p.scenes.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!ReadScene(r, &p.scenes[i], error)) return false;
  }

  if (!ReadCount(r, &count, 4096, error, "transitions")) return false;
  p.transitions.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (!ReadTransition(r, &p.transitions[i])) {
      SetErr(error, "p3d: transition read failed");
      return false;
    }
  }
  if (version >= 29) {
    const uint32_t ac = r.U32();
    if (!r.ok || ac > 1024) {
      SetErr(error, "p3d: bad audio count");
      return false;
    }
    p.audio.resize(ac);
    for (uint32_t i = 0; i < ac; ++i) {
      AudioClip& a = p.audio[i];
      a.id = r.Str();
      const uint32_t n = r.U32();
      if (!r.ok || n > (1u << 26)) {
        SetErr(error, "p3d: bad audio size");
        return false;
      }
      a.data.resize(n);
      if (!r.Bytes(a.data.data(), n)) {
        SetErr(error, "p3d: truncated audio");
        return false;
      }
    }
  }
  if (!r.ok) {
    SetErr(error, "p3d: truncated file");
    return false;
  }
  if (p.scenes.empty()) {
    SetErr(error, "p3d: no scenes");
    return false;
  }
  if (p.initial_scene < 0 || p.initial_scene >= static_cast<int>(p.scenes.size())) {
    p.initial_scene = 0;
  }
  // 全局变量（末尾；老文件没有这段，r.ok 仍为真，按剩余字节兜底）
  {
    const uint16_t gn = r.U16();
    for (uint16_t i = 0; i < gn && r.ok; ++i) {
      std::string gname = r.Str();
      const int gval = r.I32();
      p.global_vars.emplace_back(std::move(gname), gval);
    }
  }
  if (version >= 75) {
    p.persistent_ui = r.I32();
    p.title_ui = r.I32();
    const uint16_t sn = r.U16();
    if (!r.ok || sn > 256) {
      SetErr(error, "p3d: bad ui screen count");
      return false;
    }
    p.ui_screens.resize(sn);
    for (uint16_t i = 0; i < sn; ++i) {
      if (!ReadUiScreen(r, &p.ui_screens[i])) {
        SetErr(error, "p3d: bad ui screen");
        return false;
      }
    }
  }
  *out = std::move(p);
  return true;
}

bool P3dLoadFile(const std::string& path, P3d* out,
                 std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    SetErr(error, "p3d: cannot open file");
    return false;
  }
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  if (n <= 0 || n > 128ll * 1024ll * 1024ll) {
    SetErr(error, "p3d: bad file size");
    return false;
  }
  f.seekg(0, std::ios::beg);
  std::vector<unsigned char> bytes(static_cast<size_t>(n));
  f.read(reinterpret_cast<char*>(bytes.data()), n);
  if (!f) {
    SetErr(error, "p3d: read failed");
    return false;
  }
  return P3dLoad(bytes.data(), bytes.size(), out, error);
}

bool P3dSave(const std::string& path, const P3d& p,
             std::string* error) {
  Writer w;
  w.U32(kP3dMagic);
  w.U32(kP3dVersion);
  w.I32(p.initial_scene);
  w.I32(p.view_w);
  w.I32(p.view_h);
  w.U32(static_cast<uint32_t>(p.images.size()));
  for (const Image& img : p.images) {
    w.Str(img.id);
    w.I32(img.width);
    w.I32(img.height);
    if (!w.ok || img.rgba.size() != static_cast<size_t>(img.width) *
                                       static_cast<size_t>(img.height) * 4u) {
      SetErr(error, "p3d: image pixel size mismatch");
      return false;
    }
    w.Bytes(img.rgba.data(), img.rgba.size());
  }

  w.U32(static_cast<uint32_t>(p.objects.size()));
  for (const Object& o : p.objects) {
    w.Str(o.id);
    w.Str(o.name);
    w.Str(o.type);
    w.Str(o.team);
    w.I32(o.image);
    w.I32(o.frame_w);
    w.I32(o.frame_h);
    w.I32(o.col_x);
    w.I32(o.col_y);
    w.I32(o.col_w);
    w.I32(o.col_h);
    w.I32(o.hurt_x);
    w.I32(o.hurt_y);
    w.I32(o.hurt_w);
    w.I32(o.hurt_h);
    w.I32(o.attack_x);
    w.I32(o.attack_y);
    w.I32(o.attack_w);
    w.I32(o.attack_h);
    w.I32(o.speed);
    w.I32(o.gravity);
    w.U16(static_cast<uint16_t>(o.actions.size()));
    for (const Action& a : o.actions) {
      w.Str(a.name);
      w.U16(static_cast<uint16_t>(a.frames.size()));
      for (const AnimFrame& f : a.frames) {
        w.I32(f.image);
        w.U16(static_cast<uint16_t>(std::max(1, f.hold)));
        w.I32(f.origin_x);
        w.I32(f.origin_y);
        w.I32(f.col_x);
        w.I32(f.col_y);
        w.I32(f.col_w);
        w.I32(f.col_h);
        w.I32(f.attack.x);
        w.I32(f.attack.y);
        w.I32(f.attack.w);
        w.I32(f.attack.h);
        w.I32(f.hurt.x);
        w.I32(f.hurt.y);
        w.I32(f.hurt.w);
        w.I32(f.hurt.h);
        w.U8(static_cast<uint8_t>(std::min<size_t>(255, f.attach.size())));
        for (const AttachPoint& ap : f.attach) {
          w.Str(ap.name);
          w.I32(ap.x);
          w.I32(ap.y);
        }
      }
      w.I32(a.fps);
      w.U8(a.loop ? 1 : 0);
      w.I32(a.offset_x);
      w.I32(a.offset_y);
      w.U8(a.flip_h ? 1 : 0);
      w.U8(a.flip_v ? 1 : 0);
    }
    w.I32(o.default_action);
    w.U16(static_cast<uint16_t>(o.rules.size()));
    for (const Rule& rl : o.rules) {
      w.Str(rl.name);
      w.U8(rl.enabled ? 1 : 0);
      w.U8(static_cast<uint8_t>(rl.when.size()));
      for (const RuleCond& rc : rl.when) {
        w.U8(rc.type);
        w.Str(rc.key);
        w.Str(rc.name);
        w.I32(rc.op);
        w.I32(rc.value);
      }
      w.U8(static_cast<uint8_t>(rl.effects.size()));
      for (const RuleEffect& re : rl.effects) {
        w.U8(re.type);
        w.I32(re.x);
        w.I32(re.y);
        w.Str(re.text);
      }
    }
    w.U8(o.limit_screen);
    w.U8(o.pushable);
  }

  w.U32(static_cast<uint32_t>(p.tilesets.size()));
  for (const Tileset& t : p.tilesets) {
    w.Str(t.id);
    w.I32(t.image);
    w.I32(t.tile_w);
    w.I32(t.tile_h);
    w.I32(t.columns);
  }

  w.U32(static_cast<uint32_t>(p.scenes.size()));
  for (const Scene& s : p.scenes) {
    w.Str(s.id);
    w.Str(s.bgm);
    w.U16(static_cast<uint16_t>(s.tilesets.size()));
    for (int ti : s.tilesets) w.I32(ti);
    w.Str(s.camera_mode);
    w.I32(s.camera_offset_x);
    w.I32(s.camera_offset_y);
    w.I32(s.camera_deadzone_w);
    w.I32(s.camera_deadzone_h);
    w.U16(static_cast<uint16_t>(s.variables.size()));
    for (const auto& entry : s.variables) {
      w.Str(entry.first);
      w.I32(entry.second);
    }
    w.U16(static_cast<uint16_t>(s.triggers.size()));
    for (const Trigger& t : s.triggers) {
      w.I32(t.x);
      w.I32(t.y);
      w.I32(t.w);
      w.I32(t.h);
      w.U8(t.once ? 1 : 0);
      w.I32(t.target_object);
      w.Str(t.require_variable);
      w.I32(t.require_value);
    w.Str(t.name);
      w.U32(t.color);
      w.U16(static_cast<uint16_t>(t.mods.size()));
      for (const TriggerMod& m : t.mods) {
        w.U8(static_cast<uint8_t>(m.stat));
        w.U8(static_cast<uint8_t>(m.op));
        w.I32(static_cast<int32_t>(m.value * 1000.0f + (m.value >= 0 ? 0.5f : -0.5f)));
      }
      w.U8(t.interaction.enabled ? 1 : 0);
      if (t.interaction.enabled) {
        w.Str(t.interaction.prompt);
        w.Str(t.interaction.ui_id);
        w.Str(t.interaction.anchor);
        w.Str(t.interaction.key);
        w.U8(t.interaction.once ? 1 : 0);
        w.Str(t.interaction.require_variable);
        w.I32(t.interaction.require_value);
        w.U16(static_cast<uint16_t>(t.interaction.actions.size()));
        for (const TriggerAction& a : t.interaction.actions) {
          w.U8(static_cast<uint8_t>(a.type));
          w.Str(a.target);
          w.I32(a.value);
        }
      }
      w.I32(t.inside_interval);
      w.U16(static_cast<uint16_t>(std::min<size_t>(64, t.rules.size())));
      for (const TriggerRule& rule : t.rules) {
        w.U8(rule.enabled ? 1 : 0);
        w.U8(static_cast<uint8_t>(rule.mode));
        w.Str(rule.require_variable);
        w.I32(rule.require_value);
        w.U16(static_cast<uint16_t>(std::min<size_t>(16, rule.actions.size())));
        for (const TriggerAction& a : rule.actions) {
          w.U8(static_cast<uint8_t>(a.type));
          w.Str(a.target);
          w.I32(a.value);
        }
      }
    }
    w.U16(static_cast<uint16_t>(s.layers.size()));
    for (const SceneLayer& layer : s.layers) {
      w.U8(layer.visible ? 1 : 0);
      w.I32(layer.scroll_x);
      w.I32(layer.scroll_y);
      w.U8(layer.foreground ? 1 : 0);
      w.U32(static_cast<uint32_t>(layer.chunks.size()));
      for (const SceneChunk& c : layer.chunks) {
        w.I32(c.cx);
        w.I32(c.cy);
        w.U32(static_cast<uint32_t>(kP3dChunkCells * kP3dChunkCells));
        for (int i = 0; i < kP3dChunkCells * kP3dChunkCells; ++i) {
          w.I32(i < static_cast<int>(c.cells.size()) ? c.cells[i] : -1);
        }
      }
    }
    w.U16(static_cast<uint16_t>(s.collision.size()));
    for (const SceneCollisionChunk& c : s.collision) {
      w.I32(c.cx);
      w.I32(c.cy);
      w.U32(static_cast<uint32_t>(kP3dChunkCells * kP3dChunkCells));
      for (int i = 0; i < kP3dChunkCells * kP3dChunkCells; ++i) {
        w.U8(i < static_cast<int>(c.masks.size()) ? c.masks[i] : 0);
      }
    }
    w.U16(static_cast<uint16_t>(s.objects.size()));
    for (const SceneObject& so : s.objects) {
      w.I32(so.object);
      w.I32(so.x);
      w.I32(so.y);
    }
    w.I32(s.gravity);
    w.I32(s.controlled);
    w.I32(s.hud_ui);
  }

  w.U32(static_cast<uint32_t>(p.transitions.size()));
  for (const Transition& t : p.transitions) {
    w.I32(t.from);
    w.I32(t.to);
    w.U8(static_cast<uint8_t>(t.type));
    w.I32(t.value_int);
    w.Str(t.value_string);
    w.Str(t.set_variable);
    w.I32(t.set_value);
  }

  w.U32(static_cast<uint32_t>(p.audio.size()));
  for (const AudioClip& a : p.audio) {
    w.Str(a.id);
    w.U32(static_cast<uint32_t>(a.data.size()));
    w.Bytes(a.data.data(), a.data.size());
  }

  if (!w.ok) {
    SetErr(error, "p3d: write overflow/too long string");
    return false;
  }
  // ★ 全局变量：写在文件最末尾 ★（插在中间曾经让后续段全部错位）
  w.U16(static_cast<uint16_t>(p.global_vars.size()));
  for (const auto& kv : p.global_vars) {
    w.Str(kv.first);
    w.I32(kv.second);
  }
  w.I32(p.persistent_ui);
  w.I32(p.title_ui);
  w.U16(static_cast<uint16_t>(std::min<size_t>(256, p.ui_screens.size())));
  for (const UiScreenData& s : p.ui_screens) WriteUiScreen(w, s);
  if (!w.ok) {
    SetErr(error, "p3d: write overflow/too long string");
    return false;
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    SetErr(error, "p3d: cannot create output");
    return false;
  }
  f.write(reinterpret_cast<const char*>(w.data.data()),
          static_cast<std::streamsize>(w.data.size()));
  if (!f) {
    SetErr(error, "p3d: write failed");
    return false;
  }
  return true;
}

// 场景数据是「左下原点、y 向上」，引擎内部是屏幕系 y 向下。
//   这里是**唯一**一次坐标转换，别在别处再翻。
//
//   点：y' = H - y      矩形：y' = H - (y + h)
//   瓦片行：r' = H/8 - 1 - r      碰撞 mask：上/下两位互换（bit0 <-> bit2）
//
//   行号翻转不是 32 的倍数（240/32 = 7.5），chunk 会跨两个，所以重新分块。
//   图块行按场景 tileset 的 tile_h；碰撞层是独立的 8px 网格。
void MirrorSceneYUp(P3d& p3d, int view_h) {
  if (view_h <= 0) return;
  auto floor_div = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };

  for (Scene& s : p3d.scenes) {
    int tile_h = 8;
    if (!s.tilesets.empty()) {
      const int ti = s.tilesets.front();
      if (ti >= 0 && ti < static_cast<int>(p3d.tilesets.size()) &&
          p3d.tilesets[static_cast<size_t>(ti)].tile_h > 0) {
        tile_h = p3d.tilesets[static_cast<size_t>(ti)].tile_h;
      }
    }
    const int rows_total = std::max(1, view_h / tile_h);
    const int collision_rows = std::max(1, view_h / 8);
    if (s.camera_offset_y != 0) s.camera_offset_y = -s.camera_offset_y;
    for (SceneObject& o : s.objects) o.y = view_h - o.y;
    for (Trigger& t : s.triggers) t.y = view_h - (t.y + t.h);

    for (SceneLayer& layer : s.layers) {
      std::map<std::pair<int, int>, int> slot;
      std::vector<SceneChunk> out;
      for (const SceneChunk& c : layer.chunks) {
        for (int ly = 0; ly < kP3dChunkCells; ++ly) {
          for (int lx = 0; lx < kP3dChunkCells; ++lx) {
            const size_t i = static_cast<size_t>(ly * kP3dChunkCells + lx);
            if (i >= c.cells.size() || c.cells[i] < 0) continue;
            const int col = c.cx * kP3dChunkCells + lx;
            const int row = c.cy * kP3dChunkCells + ly;
            const int nrow = rows_total - 1 - row;
            const int ncx = floor_div(col, kP3dChunkCells);
            const int ncy = floor_div(nrow, kP3dChunkCells);
            const int nlx = col - ncx * kP3dChunkCells;
            const int nly = nrow - ncy * kP3dChunkCells;
            const std::pair<int, int> key(ncx, ncy);
            auto it = slot.find(key);
            if (it == slot.end()) {
              SceneChunk nc;
              nc.cx = ncx;
              nc.cy = ncy;
              nc.cells.assign(kP3dChunkCells * kP3dChunkCells, -1);
              it = slot.emplace(key, static_cast<int>(out.size())).first;
              out.push_back(std::move(nc));
            }
            out[static_cast<size_t>(it->second)].cells[nly * kP3dChunkCells + nlx] = c.cells[i];
          }
        }
      }
      layer.chunks = std::move(out);
    }

    std::map<std::pair<int, int>, int> slot;
    std::vector<SceneCollisionChunk> out;
    for (const SceneCollisionChunk& c : s.collision) {
      for (int ly = 0; ly < kP3dChunkCells; ++ly) {
        for (int lx = 0; lx < kP3dChunkCells; ++lx) {
          const size_t i = static_cast<size_t>(ly * kP3dChunkCells + lx);
          if (i >= c.masks.size() || c.masks[i] == 0) continue;
          const unsigned char m = c.masks[i];
          const unsigned char nm = static_cast<unsigned char>(
              ((m & 1u) << 2) | (m & 2u) | ((m & 4u) >> 2) | (m & 8u));
          const int col = c.cx * kP3dChunkCells + lx;
          const int row = c.cy * kP3dChunkCells + ly;
          const int nrow = collision_rows - 1 - row;
          const int ncx = floor_div(col, kP3dChunkCells);
          const int ncy = floor_div(nrow, kP3dChunkCells);
          const int nlx = col - ncx * kP3dChunkCells;
          const int nly = nrow - ncy * kP3dChunkCells;
          const std::pair<int, int> key(ncx, ncy);
          auto it = slot.find(key);
          if (it == slot.end()) {
            SceneCollisionChunk nc;
            nc.cx = ncx;
            nc.cy = ncy;
            nc.masks.assign(kP3dChunkCells * kP3dChunkCells, 0);
            it = slot.emplace(key, static_cast<int>(out.size())).first;
            out.push_back(std::move(nc));
          }
          out[static_cast<size_t>(it->second)].masks[nly * kP3dChunkCells + nlx] = nm;
        }
      }
    }
    s.collision = std::move(out);
  }
}

}  // namespace r4
