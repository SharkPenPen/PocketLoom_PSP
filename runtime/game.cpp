// runtime/game.cpp
#include "runtime/game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#if defined(__psp__)
#include <pspiofilemgr.h>
static void GameTrace(const char* tag) {
  const SceUID fd = sceIoOpen("ms0:/PL_GAME.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) return;
  int n = 0;
  while (tag[n] != '\0' && n < 80) ++n;
  sceIoWrite(fd, tag, static_cast<unsigned int>(n));
  sceIoWrite(fd, "\n", 1);
  sceIoClose(fd);
}
static void GameTraceInt(const char* tag, int value) {
  char buf[96];
  int n = 0;
  while (tag[n] != '\0' && n < 60) { buf[n] = tag[n]; ++n; }
  buf[n++] = ' ';
  char d[16];
  int dn = 0;
  if (value < 0) { buf[n++] = '-'; value = -value; }
  if (value == 0) d[dn++] = '0';
  while (value > 0) { d[dn++] = static_cast<char>('0' + value % 10); value /= 10; }
  while (dn > 0) buf[n++] = d[--dn];
  buf[n++] = '\n';
  const SceUID fd = sceIoOpen("ms0:/PL_GAME.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
  if (fd < 0) return;
  sceIoWrite(fd, buf, static_cast<unsigned int>(n));
  sceIoClose(fd);
}
#else
#define GameTrace(tag) ((void)0)
#endif

#include "engine/base/rect.h"
#include "engine/render/texture.h"
#include "runtime/ui.h"
#include "runtime/audio.h"

namespace r4 {
namespace {
void PutU16(std::string* b, unsigned v) {
  b->push_back(static_cast<char>(v & 0xFF));
  b->push_back(static_cast<char>((v >> 8) & 0xFF));
}

void PutI32(std::string* b, int v) {
  const unsigned u = static_cast<unsigned>(v);
  for (int i = 0; i < 4; ++i) {
    b->push_back(static_cast<char>((u >> (i * 8)) & 0xFF));
  }
}

void PutStr(std::string* b, const std::string& s) {
  const unsigned n = s.size() > 65535 ? 65535 : static_cast<unsigned>(s.size());
  PutU16(b, n);
  b->append(s.data(), n);
}

bool GetU16(const std::string& b, size_t* pos, unsigned* out) {
  if (*pos + 2 > b.size()) return false;
  *out = static_cast<unsigned char>(b[*pos]) |
         (static_cast<unsigned>(static_cast<unsigned char>(b[*pos + 1])) << 8);
  *pos += 2;
  return true;
}

bool GetI32(const std::string& b, size_t* pos, int* out) {
  if (*pos + 4 > b.size()) return false;
  unsigned u = 0;
  for (int i = 0; i < 4; ++i) {
    u |= static_cast<unsigned>(static_cast<unsigned char>(b[*pos + i])) << (i * 8);
  }
  *out = static_cast<int>(u);
  *pos += 4;
  return true;
}

bool GetStr(const std::string& b, size_t* pos, std::string* out) {
  unsigned n = 0;
  if (!GetU16(b, pos, &n)) return false;
  if (*pos + n > b.size()) return false;
  out->assign(b.data() + *pos, n);
  *pos += n;
  return true;
}

std::string ObjectTeam(const Object& o) {
  if (!o.team.empty()) return o.team;
  if (o.type == "player") return "player";
  if (o.type == "enemy") return "enemy";
  return "neutral";
}

// v43：碰撞盒是每帧的；这一帧没给（w/h=0）就退回对象上的默认盒。
AttackBox ColBoxOf(const Object& o, int action, int frame) {
  if (action >= 0 && action < static_cast<int>(o.actions.size())) {
    const Action& a = o.actions[static_cast<size_t>(action)];
    if (frame >= 0 && frame < static_cast<int>(a.frames.size())) {
      const AnimFrame& f = a.frames[static_cast<size_t>(frame)];
      if (f.col_w > 0 && f.col_h > 0) {
        return AttackBox{f.col_x, f.col_y, f.col_w, f.col_h};
      }
    }
  }
  return AttackBox{o.col_x, o.col_y, o.col_w, o.col_h};
}

}


namespace {


int ActionTotalTicks(const Action& a) {
  int total = 0;
  for (size_t i = 0; i < a.frames.size(); ++i) total += P3dActionHold(a, i);
  return total;
}

int ActionFrameAt(const Action& a, float time, bool* ended) {
  GameTrace("af_enter");
  if (ended != nullptr) *ended = false;
  const int n = static_cast<int>(a.frames.size());
  GameTraceInt("af_n", n);
  if (n <= 0) {
    if (ended != nullptr) *ended = true;
    return 0;
  }
  const int total = ActionTotalTicks(a);
  GameTraceInt("af_total", total);
  if (total <= 0) return 0;
  int tick = static_cast<int>(std::floor(std::max(0.0f, time) * 60.0f));
  GameTrace("af_tick");
  if (a.loop) {
    tick %= total;
  } else if (tick >= total) {
    if (ended != nullptr) *ended = true;
    return n - 1;
  }
  int idx = 0;
  GameTrace("af_loop");
  while (idx < n) {
    const int hold = P3dActionHold(a, idx);
    if (tick < hold) break;
    tick -= hold;
    ++idx;
  }
  if (idx >= n) idx = a.loop ? 0 : n - 1;
  return idx;
}

const Object* FindPlayerObject(const P3d& p, const Scene& s, int* out_index) {
  *out_index = -1;
  int idx = s.controlled;
  if (idx < 0 || idx >= static_cast<int>(s.objects.size())) {
    // 兜底：场景没标 controlled 时，找第一个 type=player 的实例
    for (size_t i = 0; i < s.objects.size(); ++i) {
      const int oi = s.objects[i].object;
      if (oi >= 0 && oi < static_cast<int>(p.objects.size()) &&
          p.objects[static_cast<size_t>(oi)].type == "player") {
        idx = static_cast<int>(i);
        break;
      }
    }
  }
  if (idx < 0 || idx >= static_cast<int>(s.objects.size())) return nullptr;
  const int object_index = s.objects[static_cast<size_t>(idx)].object;
  if (object_index < 0 || object_index >= static_cast<int>(p.objects.size())) {
    return nullptr;
  }
  *out_index = object_index;
  return &p.objects[object_index];
}

}  // namespace

bool Game::Init(const P3d& p3d, std::string* error) {
  GameTrace("init_enter");
  if (p3d.scenes.empty()) {
    if (error != nullptr) *error = "runtime: no scenes";
    return false;
  }
  p3d_ = &p3d;
  view_w_ = p3d.view_w > 0 ? p3d.view_w : 480;
  view_h_ = p3d.view_h > 0 ? p3d.view_h : 272;
  scene_index_ = p3d.initial_scene;
  if (scene_index_ < 0 || scene_index_ >= static_cast<int>(p3d.scenes.size())) {
    scene_index_ = 0;
  }
  player_object_ = -1;
  toast_.clear();
  toast_timer_ = 0.0f;
  SetScene(scene_index_);
  GameTrace("init_after_scene");
  if (p3d.title_ui >= 0 &&
      p3d.title_ui < static_cast<int>(p3d.ui_screens.size())) {
    GameTrace("title_enter");
    OpenUiById(p3d.ui_screens[static_cast<size_t>(p3d.title_ui)].id);
    GameTrace("title_ok");
  }
  GameTrace("init_return");
  return true;
}

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

void Game::PlaySceneBgm() {
  if (p3d_ == nullptr) return;
  const Scene& s = p3d_->scenes[scene_index_];
  if (s.bgm.empty()) {
    AudioStop();
    return;
  }
  for (const AudioClip& a : p3d_->audio) {
    if (a.id == s.bgm && !a.data.empty()) {
      AudioPlayClip(a.data.data(), a.data.size(), true);
      return;
    }
  }
  AudioStop();
}

void Game::PlayClipById(const std::string& id) {
  if (id.empty() || p3d_ == nullptr) return;
  for (const AudioClip& a : p3d_->audio) {
    if (a.id == id && !a.data.empty()) {
      AudioPlayClip(a.data.data(), a.data.size(), false);
      return;
    }
  }
}

void Game::SetScene(int index) {
  GameTrace("scene_enter");
  if (p3d_ == nullptr) return;
  if (index < 0 || index >= static_cast<int>(p3d_->scenes.size())) return;
  modal_stack_.clear();
  modal_focus_ = -1;
  modal_timer_ = 0;
  modal_closing_ = false;
  modal_close_fade_ = 0;
  modal_close_total_ = 0;
  has_pending_ui_ = false;
  pending_actions_.clear();
  scene_index_ = index;
  const Scene& s = p3d_->scenes[index];
  // 变量一律全局保存；场景里的 variables 只作为初始值，不覆盖已有变量。
  if (!globals_seeded_ && p3d_ != nullptr) {
    globals_seeded_ = true;
    for (const auto& kv : p3d_->global_vars) variables_.emplace(kv.first, kv.second);
  }
  for (const auto& entry : s.variables) variables_.emplace(entry.first, entry.second);
  // ★ 出生点 = 场景里那个"玩家对象实例"的位置 ★
  //   不再用场景的 start_x/start_y：那是和实例位置重复的一份数据，
  //   拖动实例却改不了出生点，两个值迟早对不上。
  int spawn_x = 64;
  int spawn_y = 192;
  auto instance_of = [&](int object_index, int* ox, int* oy) {
    for (const SceneObject& inst : s.objects) {
      if (inst.object != object_index) continue;
      *ox = inst.x;
      *oy = inst.y;
      return true;
    }
    return false;
  };
  const bool controlled_is_player =
      s.controlled >= 0 && s.controlled < static_cast<int>(p3d_->objects.size()) &&
      p3d_->objects[static_cast<size_t>(s.controlled)].type == "player";
  if (!controlled_is_player || !instance_of(s.controlled, &spawn_x, &spawn_y)) {
    bool found = false;
    for (const SceneObject& inst : s.objects) {
      if (inst.object < 0 || inst.object >= static_cast<int>(p3d_->objects.size())) continue;
      if (p3d_->objects[static_cast<size_t>(inst.object)].type != "player") continue;
      spawn_x = inst.x;
      spawn_y = inst.y;
      found = true;
      break;
    }
    if (!found && !s.objects.empty()) {
      spawn_x = s.objects.front().x;
      spawn_y = s.objects.front().y;
    }
  }
  respawn_x_ = static_cast<float>(spawn_x);
  respawn_y_ = static_cast<float>(spawn_y);
  has_respawn_ = false;
  trigger_done_.assign(s.triggers.size(), 0);
  player_.x = static_cast<float>(spawn_x);
  player_.y = static_cast<float>(spawn_y);
  player_.vx = 0.0f;
  player_.vy = 0.0f;
  player_.on_ground = false;
  player_.anim_time = 0.0f;
  player_.frame = 0;
  player_.anim_end = false;
  player_.action = 0;
  player_.facing = 1;
  finished_ = false;

  int player_index = -1;
  const Object* po = FindPlayerObject(*p3d_, s, &player_index);
  if (po != nullptr) {
    player_object_ = player_index;
    int action = po->default_action;
    if (action < 0 || action >= static_cast<int>(po->actions.size())) action = 0;
    player_.action = action;
  }
  camera_mode_ = (s.camera_mode == "fixed") ? CameraMode::kFixed
                                                 : CameraMode::kFollow;
  GameTrace("scene_before_runtime");
  InitSceneRuntime();
  GameTrace("scene_after_runtime");
  UpdateCamera(0.016f);
  GameTrace("scene_after_camera");
  PlaySceneBgm();
  GameTrace("scene_after_bgm");
}

void Game::InitSceneRuntime() {
  InitEnemies();
}

void Game::InitEnemies() {
  enemies_.clear();
  checkpoints_.clear();
  projectiles_.clear();
  if (p3d_ == nullptr) return;
  const Scene& s = p3d_->scenes[scene_index_];
  for (int i = 0; i < static_cast<int>(s.objects.size()); ++i) {
    const SceneObject& so = s.objects[i];
    if (so.object < 0 || so.object >= static_cast<int>(p3d_->objects.size())) continue;
    if (so.object == player_object_) continue;
    const Object& o = p3d_->objects[so.object];
    if (o.type == "checkpoint") {
      CheckpointState cp;
      cp.object = so.object;
      cp.x = static_cast<float>(so.x);
      cp.y = static_cast<float>(so.y);
      checkpoints_.push_back(cp);
      continue;
    }
    if (o.type != "enemy") continue;
    EnemyState e;
    e.object = so.object;
    e.scene_object = i;
    e.x = static_cast<float>(so.x);
    e.y = static_cast<float>(so.y);
    e.home_x = e.x;
    e.dir = -1;
    e.facing = -1;
    e.action = o.default_action;
    if (e.action < 0 || e.action >= static_cast<int>(o.actions.size())) e.action = 0;
    for (int a = 0; a < static_cast<int>(o.actions.size()); ++a) {
      if (o.actions[a].name == "death") {
        e.death_action = a;
        break;
      }
    }
    enemies_.push_back(e);
  }
}

void Game::DrawToast(eng::DrawList& dl, const eng::TextureId* textures,
                     int texture_count) {
  // ★ 调试叠加（按 Start 切换）：玩家坐标 / 当前交互 / 触发区矩形 ★
  if (debug_on_ && p3d_ != nullptr && !p3d_->scenes.empty()) {
    const Scene& sc = p3d_->scenes[scene_index_];
    char buf[128];
    std::snprintf(buf, sizeof(buf), "X %.0f Y %.0f ACT %d", player_.x, player_.y,
                  active_interaction_);
    UiDrawTextLine(dl, 4, 4, buf, 0xFF00FF00u, 0, 0);
    for (size_t i = 0; i < sc.triggers.size(); ++i) {
      const Trigger& t = sc.triggers[i];
      if (!t.interaction.enabled && t.mods.empty()) continue;
      const float x0 = static_cast<float>(t.x) - camera_x_;
      const float y0 = static_cast<float>(t.y) - camera_y_;
      dl.DrawRect(eng::Rect2{eng::Vec2{x0, y0},
                             eng::Vec2{static_cast<float>(t.w), static_cast<float>(t.h)}},
                  0xFF00FFFFu);
      std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(i));
      UiDrawTextLine(dl, static_cast<int>(x0 + 2), static_cast<int>(y0 + 2), buf,
                 0xFF00FFFFu, 0, 0);
    }
  }
  DrawUiScreens(dl, textures, texture_count);
  if (toast_timer_ <= 0.0f || toast_.empty()) return;
  UiDrawTextLine(dl, 0, 12, toast_, 0xFFFFFFFFu, 1, view_w_);
}

int Game::UiValue(const char* name) const {
  if (name == nullptr || name[0] == '\0') return 0;
  auto it = variables_.find(name);
  return it == variables_.end() ? 0 : it->second;
}

bool Game::SaveBytes(std::string* out) const {
  if (out == nullptr || p3d_ == nullptr || p3d_->scenes.empty()) return false;
  out->clear();
  out->append("PLSV", 4);
  PutU16(out, 2);  // version：所有变量都全局保存
  PutStr(out, p3d_->scenes[static_cast<size_t>(scene_index_)].id);
  PutI32(out, static_cast<int>(std::lround(player_.x)));
  PutI32(out, static_cast<int>(std::lround(player_.y)));
  PutU16(out, static_cast<unsigned>(variables_.size()));
  for (const auto& kv : variables_) {
    PutStr(out, kv.first);
    PutI32(out, kv.second);
  }
  return true;
}

bool Game::SaveExists() const {
  if (save_path_.empty()) return false;
  std::ifstream f(save_path_, std::ios::binary);
  return f.good();
}

void Game::StartNewGame() {
  if (p3d_ == nullptr) return;
  variables_.clear();
  globals_seeded_ = false;
  CloseUi();
  SetScene(p3d_->initial_scene);
}

bool Game::LoadBytes(const std::string& data) {
  if (p3d_ == nullptr || data.size() < 6) return false;
  if (data.compare(0, 4, "PLSV") != 0) return false;
  size_t pos = 4;
  unsigned version = 0;
  if (!GetU16(data, &pos, &version) || version != 2) return false;
  std::string scene_id;
  int px = 0;
  int py = 0;
  if (!GetStr(data, &pos, &scene_id) || !GetI32(data, &pos, &px) ||
      !GetI32(data, &pos, &py)) {
    return false;
  }
  unsigned count = 0;
  if (!GetU16(data, &pos, &count) || count > 4096) return false;
  std::vector<std::pair<std::string, int>> globals;
  for (unsigned i = 0; i < count; ++i) {
    std::string key;
    int value = 0;
    if (!GetStr(data, &pos, &key) || !GetI32(data, &pos, &value)) return false;
    globals.emplace_back(std::move(key), value);
  }
  int scene_index = -1;
  for (int i = 0; i < static_cast<int>(p3d_->scenes.size()); ++i) {
    if (p3d_->scenes[static_cast<size_t>(i)].id == scene_id) {
      scene_index = i;
      break;
    }
  }
  if (scene_index < 0) return false;
  SetScene(scene_index);
  variables_.clear();
  for (const auto& kv : globals) variables_[kv.first] = kv.second;
  globals_seeded_ = true;
  player_.x = static_cast<float>(px);
  player_.y = static_cast<float>(py);
  toast_.clear();
  toast_timer_ = 0.0f;
  UpdateCamera(0.0f);
  return true;
}

bool Game::SaveToFile(const std::string& path) const {
  std::string data;
  if (!SaveBytes(&data)) return false;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  return f.good();
}

bool Game::LoadFromFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  return LoadBytes(data);
}


void Game::DrawUiScreens(eng::DrawList& dl, const eng::TextureId* textures,
                         int texture_count) const {
  if (p3d_ == nullptr || p3d_->scenes.empty()) return;
  UiDrawContext ctx;
  ctx.save_exists = SaveExists();
  ctx.dl = &dl;
  ctx.view_w = view_w_;
  ctx.view_h = view_h_;
  ctx.value = [](void* user, const char* name) {
    return static_cast<const Game*>(user)->UiValue(name);
  };
  ctx.user = const_cast<Game*>(this);
  const Scene& sc = p3d_->scenes[scene_index_];
  if (sc.hud_ui >= 0 && sc.hud_ui < static_cast<int>(p3d_->ui_screens.size())) {
    const UiScreen screen =
        UiScreenFromData(p3d_->ui_screens[static_cast<size_t>(sc.hud_ui)],
                         textures, texture_count);
    UiDrawScreen(screen, ctx);
  }
  if (p3d_->persistent_ui >= 0 &&
      p3d_->persistent_ui < static_cast<int>(p3d_->ui_screens.size())) {
    const UiScreen screen =
        UiScreenFromData(p3d_->ui_screens[static_cast<size_t>(p3d_->persistent_ui)],
                         textures, texture_count);
    UiDrawScreen(screen, ctx);
  }
  if (!modal_stack_.empty()) {
    dl.DrawRect(eng::Rect2{eng::Vec2{0.0f, 0.0f},
                           eng::Vec2{static_cast<float>(view_w_),
                                     static_cast<float>(view_h_)}},
                0x80000000u);
    for (size_t mi = 0; mi < modal_stack_.size(); ++mi) {
      const int idx = modal_stack_[mi];
      if (idx < 0 || idx >= static_cast<int>(p3d_->ui_screens.size())) continue;
      const UiScreen screen =
          UiScreenFromData(p3d_->ui_screens[static_cast<size_t>(idx)], textures,
                           texture_count);
      const bool top = (mi + 1 == modal_stack_.size());
      ctx.focus = top ? modal_focus_ : -1;
      ctx.list_selected = top ? modal_list_index_ : 0;
      UiDrawScreen(screen, ctx);
    }
    int fade_alpha = 0;
    if (modal_closing_ && modal_close_total_ > 0) {
      fade_alpha = modal_close_fade_ * 255 / modal_close_total_;
    } else if (modal_fade_ > 0 && modal_fade_total_ > 0) {
      fade_alpha = modal_fade_ * 255 / modal_fade_total_;
    }
    if (fade_alpha > 0) {
      fade_alpha = std::max(0, std::min(255, fade_alpha));
      dl.DrawRect(eng::Rect2{eng::Vec2{0.0f, 0.0f},
                             eng::Vec2{static_cast<float>(view_w_),
                                       static_cast<float>(view_h_)}},
                  static_cast<eng::u32>(fade_alpha) << 24);
    }
  }
}

bool Game::UiModalActive() const {
  if (p3d_ == nullptr || modal_stack_.empty()) return false;
  const int idx = modal_stack_.back();
  return idx >= 0 && idx < static_cast<int>(p3d_->ui_screens.size()) &&
         p3d_->ui_screens[static_cast<size_t>(idx)].modal;
}

void Game::OpenUiById(const std::string& id) {
  if (p3d_ == nullptr || id.empty()) return;
  for (int i = 0; i < static_cast<int>(p3d_->ui_screens.size()); ++i) {
    if (p3d_->ui_screens[static_cast<size_t>(i)].id == id) {
      if (!modal_stack_.empty() && modal_stack_.back() == i) return;
      modal_stack_.push_back(i);
      modal_focus_ = -1;
      const UiScreenData& screen = p3d_->ui_screens[static_cast<size_t>(i)];
      modal_list_index_ = 0;
      for (int k = 0; k < static_cast<int>(screen.elements.size()); ++k) {
        const UiElementData& el = screen.elements[static_cast<size_t>(k)];
        if (el.only_if_save && !SaveExists()) continue;
        if (el.kind == 7 || el.kind == 6) {
          modal_focus_ = k;
          break;
        }
      }
      modal_timer_ = screen.dismiss_frames;
      modal_fade_total_ = screen.fade_frames;
      modal_fade_ = modal_fade_total_;
      modal_closing_ = false;
      modal_close_fade_ = 0;
      modal_close_total_ = 0;
      return;
    }
  }
}

void Game::RequestCloseUi() {
  if (p3d_ == nullptr || modal_stack_.empty() || modal_closing_) return;
  const int idx = modal_stack_.back();
  if (idx < 0 || idx >= static_cast<int>(p3d_->ui_screens.size())) {
    CloseUi();
    return;
  }
  const int total = p3d_->ui_screens[static_cast<size_t>(idx)].fade_out_frames;
  if (total <= 0) {
    CloseUi();
    return;
  }
  modal_closing_ = true;
  modal_close_fade_ = 0;
  modal_close_total_ = total;
}

void Game::CloseUi() {
  if (!modal_stack_.empty()) modal_stack_.pop_back();
  modal_focus_ = -1;
  modal_list_index_ = 0;
  modal_timer_ = 0;
  modal_fade_ = 0;
  modal_fade_total_ = 0;
  modal_closing_ = false;
  modal_close_fade_ = 0;
  modal_close_total_ = 0;
  if (modal_stack_.empty() && has_pending_ui_) {
    has_pending_ui_ = false;
    const Trigger trigger = pending_trigger_;
    std::vector<TriggerAction> actions = std::move(pending_actions_);
    pending_actions_.clear();
    RunTriggerList(trigger, actions);
  }
}

void Game::MoveModalFocus(int dir, bool allow_list) {
  if (p3d_ == nullptr || modal_stack_.empty()) return;
  const int idx = modal_stack_.back();
  if (idx < 0 || idx >= static_cast<int>(p3d_->ui_screens.size())) return;
  const UiScreenData& screen = p3d_->ui_screens[static_cast<size_t>(idx)];
  if (allow_list && modal_focus_ >= 0 &&
      modal_focus_ < static_cast<int>(screen.elements.size())) {
    const UiElementData& e = screen.elements[static_cast<size_t>(modal_focus_)];
    if (e.kind == 6 && !e.items.empty()) {
      modal_list_index_ = std::max(
          0, std::min(static_cast<int>(e.items.size()) - 1,
                      modal_list_index_ + dir));
      return;
    }
  }
  std::vector<int> focusables;
  for (int i = 0; i < static_cast<int>(screen.elements.size()); ++i) {
    const UiElementData& el = screen.elements[static_cast<size_t>(i)];
    if (el.only_if_save && !SaveExists()) continue;
    if (el.kind == 7 || el.kind == 6) focusables.push_back(i);
  }
  if (focusables.empty()) return;
  int pos = 0;
  for (int i = 0; i < static_cast<int>(focusables.size()); ++i) {
    if (focusables[static_cast<size_t>(i)] == modal_focus_) pos = i;
  }
  pos = (pos + dir + static_cast<int>(focusables.size())) %
        static_cast<int>(focusables.size());
  modal_focus_ = focusables[static_cast<size_t>(pos)];
  modal_list_index_ = 0;
}

void Game::ActivateModalFocus() {
  if (p3d_ == nullptr || modal_stack_.empty()) return;
  const int idx = modal_stack_.back();
  if (idx < 0 || idx >= static_cast<int>(p3d_->ui_screens.size())) return;
  const UiScreenData& screen = p3d_->ui_screens[static_cast<size_t>(idx)];
  if (modal_focus_ < 0 ||
      modal_focus_ >= static_cast<int>(screen.elements.size())) {
    return;
  }
  const UiElementData& e = screen.elements[static_cast<size_t>(modal_focus_)];
  if (e.kind == 6) {
    if (e.items.empty()) return;
    const int sel = std::max(
        0, std::min(static_cast<int>(e.items.size()) - 1, modal_list_index_));
    const std::string& item = e.items[static_cast<size_t>(sel)];
    if (e.action.empty() || e.action == "close") {
      RequestCloseUi();
      return;
    }
    if (e.action == "scene") {
      for (int i = 0; i < static_cast<int>(p3d_->scenes.size()); ++i) {
        if (p3d_->scenes[static_cast<size_t>(i)].id == item) {
          SetScene(i);
          return;
        }
      }
      return;
    }
    if (e.action == "open_ui") {
      OpenUiById(item);
      return;
    }
    if (e.action == "set_var") {
      variables_[e.action_target] = sel;
      return;
    }
    return;
  }
  if (e.kind != 7) return;
  if (e.action == "new_game") {
    StartNewGame();
    return;
  }
  if (e.action == "load") {
    if (LoadFromFile(save_path_)) CloseUi();
    return;
  }
  if (e.action.empty() || e.action == "close") {
    RequestCloseUi();
    return;
  }
  if (e.action == "open_ui") {
    OpenUiById(e.action_target);
    return;
  }
  if (e.action == "set_var") {
    variables_[e.action_target] = e.action_value;
    return;
  }
  if (e.action == "scene") {
    for (int i = 0; i < static_cast<int>(p3d_->scenes.size()); ++i) {
      if (p3d_->scenes[static_cast<size_t>(i)].id == e.action_target) {
        SetScene(i);
        return;
      }
    }
  }
}

void Game::UpdateEnemies(float dt) {
  if (p3d_ == nullptr || enemies_.empty()) return;
  for (EnemyState& e : enemies_) {
    if (e.dead) continue;
    if (e.object < 0 || e.object >= static_cast<int>(p3d_->objects.size())) continue;
    const Object& eo = p3d_->objects[e.object];
    if (e.dying) {
      if (e.death_action < 0) {
        e.dead = true;
        continue;
      }
      e.anim_time += dt;
      int action = e.death_action >= 0 ? e.death_action : e.action;
      if (action >= 0 && action < static_cast<int>(eo.actions.size())) {
        const Action& a = eo.actions[action];
        if (!a.frames.empty()) {
          bool ended = false;
          e.frame = ActionFrameAt(a, e.anim_time, &ended);
          if (ended) e.dead = true;
        } else {
          e.dead = true;
        }
      } else {
        e.dead = true;
      }
      continue;
    }

    e.tick++;
    if (!eo.rules.empty()) {
      RunEnemyRules(eo, e, dt);
    } else {
      const float speed = eo.speed > 0 ? static_cast<float>(eo.speed) * 0.45f : 30.0f;
      const float range = 64.0f;
      e.x += static_cast<float>(e.dir) * speed * dt;
      if (e.x < e.home_x - range) e.dir = 1;
      if (e.x > e.home_x + range) e.dir = -1;
      e.facing = e.dir;
    }
    e.anim_time += dt;
    if (e.action >= 0 && e.action < static_cast<int>(eo.actions.size())) {
      const Action& a = eo.actions[e.action];
      if (!a.frames.empty()) {
        e.frame = ActionFrameAt(a, e.anim_time, nullptr);
      }
    }

  }
}

int Game::ChunkTile(const SceneLayer& layer, int col, int row) const {
  const int cx = col >= 0 ? col / kP3dChunkCells
                          : -((-col + kP3dChunkCells - 1) / kP3dChunkCells);
  const int cy = row >= 0 ? row / kP3dChunkCells
                          : -((-row + kP3dChunkCells - 1) / kP3dChunkCells);
  const int lx = col - cx * kP3dChunkCells;
  const int ly = row - cy * kP3dChunkCells;
  for (const SceneChunk& c : layer.chunks) {
    if (c.cx != cx || c.cy != cy) continue;
    const size_t i = static_cast<size_t>(ly) * kP3dChunkCells + lx;
    if (i >= c.cells.size()) return -1;
    return c.cells[i];
  }
  return -1;
}


int Game::TileWallsAt(int col, int row) const {
  if (p3d_ == nullptr) return 0;
  const Scene& s = p3d_->scenes[scene_index_];
  const int cx = col >= 0 ? col / kP3dChunkCells
                          : -((-col + kP3dChunkCells - 1) / kP3dChunkCells);
  const int cy = row >= 0 ? row / kP3dChunkCells
                          : -((-row + kP3dChunkCells - 1) / kP3dChunkCells);
  const int lx = col - cx * kP3dChunkCells;
  const int ly = row - cy * kP3dChunkCells;
  for (const SceneCollisionChunk& c : s.collision) {
    if (c.cx != cx || c.cy != cy) continue;
    const size_t i = static_cast<size_t>(ly) * kP3dChunkCells + lx;
    if (i >= c.masks.size()) return 0;
    return c.masks[i];
  }
  return 0;
}

bool Game::PlayerAttachPoint(const char* name, float* x, float* y) const {
  if (p3d_ == nullptr) return false;
  const Object& o = p3d_->objects[player_object_];
  if (player_.action < 0 || player_.action >= static_cast<int>(o.actions.size())) {
    return false;
  }
  const Action& a = o.actions[player_.action];
  if (player_.frame < 0 || player_.frame >= static_cast<int>(a.frames.size())) {
    return false;
  }
  for (const AttachPoint& ap : a.frames[player_.frame].attach) {
    if (ap.name == name) {
      *x = player_.x + static_cast<float>(ap.x);
      *y = player_.y + static_cast<float>(ap.y);
      return true;
    }
  }
  return false;
}

void Game::ResolveX(float dx, int obj_index) {
  if (dx == 0.0f || p3d_ == nullptr || obj_index < 0 ||
      obj_index >= static_cast<int>(p3d_->objects.size())) {
    return;
  }
  const Object& o = p3d_->objects[obj_index];
  const AttackBox cb = ColBoxOf(o, player_.action, player_.frame);
  const float old = player_.x;
  player_.x += dx;
  const float top = player_.y + static_cast<float>(cb.y);
  const float bottom = top + static_cast<float>(cb.h);
  const float left = player_.x + static_cast<float>(cb.x);
  const float right = left + static_cast<float>(cb.w);
  const float old_left = old + static_cast<float>(cb.x);
  const float old_right = old_left + static_cast<float>(cb.w);
  const int r0 = static_cast<int>(std::floor(top / 8.0f));
  const int r1 = static_cast<int>(std::floor((bottom - 0.001f) / 8.0f));
  float edge = 0.0f;
  bool hit = false;
  if (dx > 0.0f) {
    for (int r = r0; r <= r1; ++r) {
      const float ty0 = static_cast<float>(r * 8);
      const float ty1 = ty0 + 8.0f;
      if (top >= ty1 - 0.001f || bottom <= ty0 + 0.001f) continue;
      const int c0 = static_cast<int>(std::floor(old_right / 8.0f));
      const int c1 = static_cast<int>(std::floor((right - 0.001f) / 8.0f));
      for (int c = c0; c <= c1; ++c) {
        const float tx = static_cast<float>(c * 8);
        if (old_right <= tx + 0.001f && right > tx + 0.001f &&
            (TileWallsAt(c, r) & 8) != 0) {
          if (!hit || tx < edge) {
            edge = tx;
            hit = true;
          }
        }
      }
    }
    if (hit) player_.x = edge - static_cast<float>(cb.x + cb.w);
  } else {
    for (int r = r0; r <= r1; ++r) {
      const float ty0 = static_cast<float>(r * 8);
      const float ty1 = ty0 + 8.0f;
      if (top >= ty1 - 0.001f || bottom <= ty0 + 0.001f) continue;
      const int c0 = static_cast<int>(std::floor((left - 0.001f) / 8.0f));
      const int c1 = static_cast<int>(std::floor(old_left / 8.0f));
      for (int c = c0; c <= c1; ++c) {
        const float tx = static_cast<float>((c + 1) * 8);
        if (old_left >= tx - 0.001f && left < tx - 0.001f &&
            (TileWallsAt(c, r) & 2) != 0) {
          if (!hit || tx > edge) {
            edge = tx;
            hit = true;
          }
        }
      }
    }
    if (hit) player_.x = edge - static_cast<float>(cb.x);
  }
  if (hit) player_.vx = 0.0f;
}

void Game::ResolveY(float dy, int obj_index) {
  if (dy == 0.0f || p3d_ == nullptr || obj_index < 0 ||
      obj_index >= static_cast<int>(p3d_->objects.size())) {
    return;
  }
  const Object& o = p3d_->objects[obj_index];
  const AttackBox cb = ColBoxOf(o, player_.action, player_.frame);
  const float old = player_.y;
  player_.y += dy;
  const float left = player_.x + static_cast<float>(cb.x);
  const float right = left + static_cast<float>(cb.w);
  const float old_top = old + static_cast<float>(cb.y);
  const float old_bottom = old_top + static_cast<float>(cb.h);
  const float top = player_.y + static_cast<float>(cb.y);
  const float bottom = top + static_cast<float>(cb.h);
  const int c0 = static_cast<int>(std::floor(left / 8.0f));
  const int c1 = static_cast<int>(std::floor((right - 0.001f) / 8.0f));
  float edge = 0.0f;
  bool hit = false;
  if (dy > 0.0f) {
    for (int c = c0; c <= c1; ++c) {
      const float tx0 = static_cast<float>(c * 8);
      const float tx1 = tx0 + 8.0f;
      if (left >= tx1 - 0.001f || right <= tx0 + 0.001f) continue;
      const int r0 = static_cast<int>(std::floor(old_bottom / 8.0f));
      const int r1 = static_cast<int>(std::floor((bottom - 0.001f) / 8.0f));
      for (int r = r0; r <= r1; ++r) {
        const float ty = static_cast<float>(r * 8);
        if (old_bottom <= ty + 0.001f && bottom > ty + 0.001f &&
            (TileWallsAt(c, r) & 1) != 0) {
          if (!hit || ty < edge) {
            edge = ty;
            hit = true;
          }
        }
      }
    }
    if (hit) {
      player_.y = edge - static_cast<float>(cb.y + cb.h);
      player_.on_ground = true;
      player_.vy = 0.0f;
    }
  } else {
    for (int c = c0; c <= c1; ++c) {
      const float tx0 = static_cast<float>(c * 8);
      const float tx1 = tx0 + 8.0f;
      if (left >= tx1 - 0.001f || right <= tx0 + 0.001f) continue;
      const int r0 = static_cast<int>(std::floor((top - 0.001f) / 8.0f));
      const int r1 = static_cast<int>(std::floor(old_top / 8.0f));
      for (int r = r0; r <= r1; ++r) {
        const float ty = static_cast<float>((r + 1) * 8);
        if (old_top >= ty - 0.001f && top < ty - 0.001f &&
            (TileWallsAt(c, r) & 4) != 0) {
          if (!hit || ty > edge) {
            edge = ty;
            hit = true;
          }
        }
      }
    }
    if (hit) {
      player_.y = edge - static_cast<float>(cb.y);
      player_.vy = 0.0f;
    }
  }
}

void Game::UpdateAnimation(float dt) {
  GameTrace("anim_enter");
  if (p3d_ == nullptr || player_object_ < 0) return;
  const Object& o = p3d_->objects[player_object_];
  if (o.actions.empty()) return;
  if (player_.action < 0 || player_.action >= static_cast<int>(o.actions.size())) {
    int action = o.default_action;
    if (action < 0 || action >= static_cast<int>(o.actions.size())) action = 0;
    player_.action = action;
  }
  GameTraceInt("anim_action", player_.action);
  GameTraceInt("anim_actions", static_cast<int>(o.actions.size()));
  const Action& a = o.actions[player_.action];
  if (a.frames.empty()) {
    player_.frame = 0;
    player_.anim_end = true;
    return;
  }
  player_.anim_time += dt;
  GameTrace("anim_before_frame_at");
  bool ended = false;
  player_.frame = ActionFrameAt(a, player_.anim_time, &ended);
  GameTrace("anim_after_frame_at");
  player_.anim_end = ended;
}

bool Game::RunTriggerList(const Trigger& trigger,
                          const std::vector<TriggerAction>& list) {
  for (size_t ai = 0; ai < list.size(); ++ai) {
    const TriggerAction& action = list[ai];
    if (action.type == 1) {  // set_respawn
      respawn_x_ = static_cast<float>(trigger.x + trigger.w / 2);
      respawn_y_ = static_cast<float>(trigger.y + trigger.h / 2);
      has_respawn_ = true;
      continue;
    }
    if (action.type == 2) {  // set_variable
      variables_[action.target] = action.value;
      continue;
    }
    if (action.type == 13) {  // add_variable
      variables_[action.target] += action.value;
      continue;
    }
    if (action.type == 11) {  // save
      SaveToFile(save_path_);
      continue;
    }
    if (action.type == 12) {  // load
      if (LoadFromFile(save_path_)) return true;
      continue;
    }
    if (action.type == 9) {  // open_ui
      OpenUiById(action.target);
      if (UiModalActive() && !modal_stack_.empty()) {
        const int top = modal_stack_.back();
        if (top >= 0 && top < static_cast<int>(p3d_->ui_screens.size()) &&
            p3d_->ui_screens[static_cast<size_t>(top)].dismiss != 3) {
          pending_trigger_ = trigger;
          pending_actions_.assign(list.begin() + static_cast<std::ptrdiff_t>(ai) + 1,
                                  list.end());
          has_pending_ui_ = true;
          return true;
        }
      }
      continue;
    }
    if (action.type == 10) {  // close_ui
      RequestCloseUi();
      continue;
    }
    if (action.type == 0) {  // change_scene
      int target = -1;
      for (int i = 0; i < static_cast<int>(p3d_->scenes.size()); ++i) {
        if (p3d_->scenes[i].id == action.target) {
          target = i;
          break;
        }
      }
      if (target < 0) continue;
      ++transition_count_;
      SetScene(target);
      return true;
    }
    if (action.type == 6) {  // play_sfx
      PlayClipById(action.target);
      continue;
    }
    if (action.type == 8) {  // spawn
      SpawnById(action.target, player_.x, player_.y, player_.facing, false);
      continue;
    }
  }
  return false;
}

void Game::UpdateTriggers() {
  if (p3d_ == nullptr || player_object_ < 0) return;
  const Scene& s = p3d_->scenes[scene_index_];
  if (trigger_done_.size() != s.triggers.size()) {
    trigger_done_.assign(s.triggers.size(), 0);
  }
  if (trigger_inside_.size() != s.triggers.size()) {
    trigger_inside_.assign(s.triggers.size(), 0);
  }
  if (trigger_tick_.size() != s.triggers.size()) {
    trigger_tick_.assign(s.triggers.size(), 0);
  }
  const Object& po = p3d_->objects[player_object_];
  auto overlaps = [](float x0, float y0, float x1, float y1, const Trigger& t) {
    return !(x1 <= static_cast<float>(t.x) || x0 >= static_cast<float>(t.x + t.w) ||
             y1 <= static_cast<float>(t.y) || y0 >= static_cast<float>(t.y + t.h));
  };
  auto rule_ok = [&](const TriggerRule& rule) {
    if (!rule.enabled) return false;
    if (rule.require_variable.empty()) return true;
    auto it = variables_.find(rule.require_variable);
    const int value = it == variables_.end() ? 0 : it->second;
    return value == rule.require_value;
  };
  for (size_t i = 0; i < s.triggers.size(); ++i) {
    const Trigger& t = s.triggers[i];
    bool any_action = false;
    auto run_rules = [&](int mode) {
      for (const TriggerRule& rule : t.rules) {
        if (rule.mode != mode || !rule_ok(rule)) continue;
        any_action = true;
        if (RunTriggerList(t, rule.actions)) return true;
      }
      return false;
    };
    bool overlap = false;
    if (t.target_object >= 0) {
      for (const SceneObject& so : s.objects) {
        if (so.object != t.target_object) continue;
        if (t.target_object == player_object_) {
          const float x0 = player_.x + static_cast<float>(po.col_x);
          const float y0 = player_.y + static_cast<float>(po.col_y);
          overlap = overlaps(x0, y0, x0 + static_cast<float>(po.col_w),
                             y0 + static_cast<float>(po.col_h), t);
        } else {
          const Object& to = p3d_->objects[static_cast<size_t>(so.object)];
          const float x0 = static_cast<float>(so.x) + static_cast<float>(to.col_x);
          const float y0 = static_cast<float>(so.y) + static_cast<float>(to.col_y);
          overlap = overlaps(x0, y0, x0 + static_cast<float>(to.col_w),
                             y0 + static_cast<float>(to.col_h), t);
        }
        if (overlap) break;
      }
    } else {
      const float px0 = player_.x + static_cast<float>(po.col_x);
      const float px1 = px0 + static_cast<float>(po.col_w);
      const float py0 = player_.y + static_cast<float>(po.col_y);
      const float py1 = py0 + static_cast<float>(po.col_h);
      overlap = overlaps(px0, py0, px1, py1, t);
    }
    const bool was_inside = trigger_inside_[i] != 0;
    trigger_inside_[i] = overlap ? 1 : 0;
    if (overlap) {
      if (!was_inside) trigger_tick_[i] = 0;
      ++trigger_tick_[i];
      if (t.inside_interval > 0 &&
          (trigger_tick_[i] % static_cast<unsigned>(t.inside_interval)) == 0) {
        if (run_rules(2)) return;
      }
    } else {
      trigger_tick_[i] = 0;
    }
    // 离开：跑 exit rules / exit_actions（不判断 once）
    if (!overlap && was_inside) {
      if (run_rules(1)) return;
    }
    if (!overlap) continue;
    if (t.once && trigger_done_[i] != 0) continue;
    if (!t.require_variable.empty()) {
      auto it = variables_.find(t.require_variable);
      const int value = it == variables_.end() ? 0 : it->second;
      if (value != t.require_value) continue;
    }
    if (was_inside && t.once) continue;
    any_action = false;
    if (run_rules(0)) {
      trigger_done_[i] = 1;
      return;
    }
    // 没有任何规则满足时不要提前标记 done，否则变量条件满足后
    // 这个 once 触发永远不会再执行。
    if (!any_action) continue;
    trigger_done_[i] = 1;
  }
}

// 规则里的「生成」：按对象 id 生成一发，速度沿朝向（子弹速度取目标对象的 bullet_speed）
void Game::SpawnById(const std::string& id, float x, float y, int dir, bool from_player) {
  if (p3d_ == nullptr) return;
  for (size_t i = 0; i < p3d_->objects.size(); ++i) {
    const Object& target = p3d_->objects[i];
    if (target.id != id) continue;
    ProjectileState p;
    p.object = static_cast<int>(i);
    p.x = x;
    p.y = y;
    const float speed = target.speed > 0 ? static_cast<float>(target.speed) : 220.0f;
    p.vx = (dir < 0 ? -1.0f : 1.0f) * speed;
    p.vy = 0.0f;
    p.life = 3.0f;
    p.from_player = from_player;
    projectiles_.push_back(p);
    return;
  }
}

void Game::UpdateProjectiles(float dt) {
  if (p3d_ == nullptr) return;
  for (size_t i = 0; i < projectiles_.size();) {
    ProjectileState& p = projectiles_[i];
    p.x += p.vx * dt;
    p.y += p.vy * dt;
    p.life -= dt;
    if (p.hit_cd > 0.0f) p.hit_cd -= dt;
    bool remove = p.life <= 0.0f;
    if (!remove && p.destroy_with_parent && p.parent_scene_object >= 0) {
      bool parent_dead = true;
      for (const EnemyState& owner : enemies_) {
        if (owner.scene_object == p.parent_scene_object && !owner.dead && !owner.dying) {
          parent_dead = false;
          break;
        }
      }
      if (parent_dead) remove = true;
    }
    if (!remove && p.hit_cd <= 0.0f && p.object >= 0 &&
        p.object < static_cast<int>(p3d_->objects.size())) {
      const Object& target = p3d_->objects[p.object];
      if (p.from_player) {
        for (EnemyState& e : enemies_) {
          if (e.dying || e.dead) continue;
          const Object& eo = p3d_->objects[e.object];
          if (ObjectTeam(eo) == ObjectTeam(target)) continue;
          const AttackBox hb = ColBoxOf(eo, e.action, e.frame);
          const float ex0 = e.x + static_cast<float>(hb.x);
          const float ey0 = e.y + static_cast<float>(hb.y);
          const float ex1 = ex0 + static_cast<float>(hb.w);
          const float ey1 = ey0 + static_cast<float>(hb.h);
          if (p.x <= ex0 || p.x >= ex1 || p.y <= ey0 || p.y >= ey1) continue;
          e.dying = true;
          e.anim_time = 0.0f;
          e.frame = 0;
          if (e.death_action >= 0) e.action = e.death_action;
          if (p.destroy_on_hit) {
            remove = true;
          } else {
            p.hit_cd = 0.2f;
          }
          break;
        }
      }
    }
    if (remove) {
      projectiles_.erase(projectiles_.begin() + i);
    } else {
      ++i;
    }
  }
}

void Game::RenderProjectiles(eng::DrawList& dl, const eng::TextureId* textures,
                             int texture_count) const {
  if (p3d_ == nullptr) return;
  const float ox = camera_x_;
  const float oy = camera_y_;
  for (const ProjectileState& p : projectiles_) {
    if (p.object < 0 || p.object >= static_cast<int>(p3d_->objects.size())) continue;
    const Object& o = p3d_->objects[p.object];
    if (o.image < 0 || o.image >= texture_count || textures[o.image] == 0) continue;
    int frame_no = 0;
    int action = o.default_action;
    if (action >= 0 && action < static_cast<int>(o.actions.size())) {
      const Action& a = o.actions[action];
      if (!a.frames.empty()) frame_no = a.frames[0].image;
    }
    const int fw = std::max(1, o.frame_w);
    const int fh = std::max(1, o.frame_h);
    const int tex_w = o.image < static_cast<int>(p3d_->images.size())
                          ? p3d_->images[o.image].width
                          : fw;
    const int tc = (tex_w / fw > 0) ? frame_no % (tex_w / fw) : 0;
    const int tr = (tex_w / fw > 0) ? frame_no / (tex_w / fw) : 0;
    dl.DrawImage(eng::Rect2{
                     eng::Vec2{p.x - ox - fw * 0.5f,
                               p.y - oy - fh * 0.5f},
                     eng::Vec2{static_cast<float>(fw), static_cast<float>(fh)}},
                 textures[o.image], p.from_player ? 0xFFFFFFFFu : 0xFF80A0FFu,
                 eng::TexRegion::Make(tc * fw, tr * fh, fw, fh));
  }
}

void Game::UpdateCamera(float dt) {
  if (p3d_ == nullptr) return;
  const Scene& s = p3d_->scenes[scene_index_];
  // ★ 死区：角色在框里 → 相机不动；出框才推 ★
  //   以前的写法先把 desired 设成"居中跟随"，死区只在出框时改它，
  //   结果框内相机会偷偷跟过去 —— 死区等于没生效。
  float desired_x = camera_x_;
  float desired_y = camera_y_;
  if (camera_mode_ == CameraMode::kFollow) {
  if (s.camera_deadzone_w > 0) {
    const float screen_x = player_.x - camera_x_;
    const float left = view_w_ * 0.5f - static_cast<float>(s.camera_deadzone_w) * 0.5f;
    const float right = view_w_ * 0.5f + static_cast<float>(s.camera_deadzone_w) * 0.5f;
    if (screen_x < left) desired_x = player_.x - left;
    else if (screen_x > right) desired_x = player_.x - right;
  } else {
    desired_x = player_.x - view_w_ * 0.5f + static_cast<float>(s.camera_offset_x);
  }
  if (s.camera_deadzone_h > 0) {
    const float screen_y = player_.y - camera_y_;
    const float top = view_h_ * 0.5f - static_cast<float>(s.camera_deadzone_h) * 0.5f;
    const float bottom = view_h_ * 0.5f + static_cast<float>(s.camera_deadzone_h) * 0.5f;
    if (screen_y < top) desired_y = player_.y - top;
    else if (screen_y > bottom) desired_y = player_.y - bottom;
  } else {
    desired_y = player_.y - view_h_ * 0.5f + static_cast<float>(s.camera_offset_y);
  }
  }
  // ★ 世界矩形按所有 chunk 的 min/max 算，不能只看 max ★
  //   地图往左/上扩到负坐标时，只取 max 会把相机钳在 0，
  //   x<0 的内容永远进不了视野（无限地图的一半就废了）。
  // 世界范围按“有内容的格子”算，不能按 chunk 的 16 格边界算：
  // 地图 60x34 时最后一个 chunk 会补到 64x48，固定相机会被多出来的空格推偏。
  int min_c = 0, min_r = 0, max_c = 0, max_r = 0;
  bool any_chunk = false;
  for (const SceneLayer& l : s.layers) {
    for (const SceneChunk& c : l.chunks) {
      for (int ly = 0; ly < kP3dChunkCells; ++ly) {
        for (int lx = 0; lx < kP3dChunkCells; ++lx) {
          const size_t i = static_cast<size_t>(ly * kP3dChunkCells + lx);
          if (i >= c.cells.size() || c.cells[i] < 0) continue;
          const int col = c.cx * kP3dChunkCells + lx;
          const int row = c.cy * kP3dChunkCells + ly;
          if (!any_chunk) {
            min_c = col; max_c = col + 1; min_r = row; max_r = row + 1;
            any_chunk = true;
          } else {
            min_c = std::min(min_c, col); max_c = std::max(max_c, col + 1);
            min_r = std::min(min_r, row); max_r = std::max(max_r, row + 1);
          }
        }
      }
    }
  }
  if (!any_chunk) {
    for (const SceneLayer& l : s.layers) {
      for (const SceneChunk& c : l.chunks) {
        const int c0 = c.cx * kP3dChunkCells;
        const int r0 = c.cy * kP3dChunkCells;
        const int c1 = c0 + kP3dChunkCells;
        const int r1 = r0 + kP3dChunkCells;
        if (!any_chunk) {
          min_c = c0; max_c = c1; min_r = r0; max_r = r1; any_chunk = true;
        } else {
          min_c = std::min(min_c, c0); max_c = std::max(max_c, c1);
          min_r = std::min(min_r, r0); max_r = std::max(max_r, r1);
        }
      }
    }
  }
  float tile_w = 8.0f;
  float tile_h = 8.0f;
  if (!s.tilesets.empty()) {
    const int ti = s.tilesets.front();
    if (ti >= 0 && ti < static_cast<int>(p3d_->tilesets.size())) {
      const Tileset& ts = p3d_->tilesets[static_cast<size_t>(ti)];
      if (ts.tile_w > 0) tile_w = static_cast<float>(ts.tile_w);
      if (ts.tile_h > 0) tile_h = static_cast<float>(ts.tile_h);
    }
  }
  const float world_l = static_cast<float>(min_c) * tile_w;
  const float world_t = static_cast<float>(min_r) * tile_h;
  const float world_r = static_cast<float>(max_c) * tile_w;
  const float world_b = static_cast<float>(max_r) * tile_h;
  const float ww = world_r - world_l;
  const float wh = world_b - world_t;
  if (camera_mode_ == CameraMode::kFixed) {
    camera_x_ = world_l - (static_cast<float>(view_w_) - ww) * 0.5f;
    camera_y_ = world_t - (static_cast<float>(view_h_) - wh) * 0.5f;
    return;
  }
  if (camera_mode_ != CameraMode::kFollow) return;
  if (ww > static_cast<float>(view_w_)) {
    desired_x = std::clamp(desired_x, world_l, world_r - static_cast<float>(view_w_));
  } else {
    desired_x = world_l - (static_cast<float>(view_w_) - ww) * 0.5f;
  }
  if (wh > static_cast<float>(view_h_)) {
    desired_y = std::clamp(desired_y, world_t, world_b - static_cast<float>(view_h_));
  } else {
    desired_y = world_t - (static_cast<float>(view_h_) - wh) * 0.5f;
  }
  const float t = 1.0f - std::exp(-8.0f * dt);
  camera_x_ += (desired_x - camera_x_) * t;
  camera_y_ += (desired_y - camera_y_) * t;
}

namespace {
int KeyEnum(const char* name) {
  if (std::strcmp(name, "left") == 0) return static_cast<int>(eng::Key::kLeft);
  if (std::strcmp(name, "right") == 0) return static_cast<int>(eng::Key::kRight);
  if (std::strcmp(name, "up") == 0) return static_cast<int>(eng::Key::kUp);
  if (std::strcmp(name, "down") == 0) return static_cast<int>(eng::Key::kDown);
  if (std::strcmp(name, "jump") == 0) return static_cast<int>(eng::Key::kA);
  if (std::strcmp(name, "attack") == 0) return static_cast<int>(eng::Key::kB);
  return -1;
}
bool KeyUpOf(const char* name, const eng::InputState& prev,
             const eng::InputState& cur) {
  const int k = KeyEnum(name);
  if (k < 0) return false;
  return prev.down[k] && !cur.down[k];
}
bool KeyOf(const char* name, const eng::InputState& in) {
  if (std::strcmp(name, "left") == 0) return eng::KeyDown(in, eng::Key::kLeft);
  if (std::strcmp(name, "right") == 0) return eng::KeyDown(in, eng::Key::kRight);
  if (std::strcmp(name, "up") == 0) return eng::KeyDown(in, eng::Key::kUp);
  if (std::strcmp(name, "down") == 0) return eng::KeyDown(in, eng::Key::kDown);
  if (std::strcmp(name, "jump") == 0) return eng::KeyDown(in, eng::Key::kA);
  if (std::strcmp(name, "attack") == 0) return eng::KeyDown(in, eng::Key::kB);
  return false;
}
bool KeyPressOf(const char* name, const eng::InputState& in) {
  if (std::strcmp(name, "left") == 0) return eng::KeyPressed(in, eng::Key::kLeft);
  if (std::strcmp(name, "right") == 0) return eng::KeyPressed(in, eng::Key::kRight);
  if (std::strcmp(name, "up") == 0) return eng::KeyPressed(in, eng::Key::kUp);
  if (std::strcmp(name, "down") == 0) return eng::KeyPressed(in, eng::Key::kDown);
  if (std::strcmp(name, "jump") == 0) return eng::KeyPressed(in, eng::Key::kA);
  if (std::strcmp(name, "attack") == 0) return eng::KeyPressed(in, eng::Key::kB);
  return false;
}
}  // namespace

// 规则驱动：条件读输入/状态，效果写 vx/vy/动作/朝向。
void Game::RunPlayerRules(const Object& o, const eng::InputState& input, float dt) {
  (void)dt;
  int want_anim = -1;
  int want_flip = 0;  // -1 左, 1 右, 0 不动
  bool moved = false;
  for (const Rule& r : o.rules) {
    if (!r.enabled) continue;
    bool hit = true;
    for (const RuleCond& c : r.when) {
      const bool ok = [&]() {
        switch (c.type) {
          case 0: return true;
          case 1: return KeyOf(c.key.c_str(), input);
          case 2: return KeyPressOf(c.key.c_str(), input);
          case 3:  // key_up
            return KeyUpOf(c.key.c_str(), prev_input_, input);
          case 4: return player_.on_ground;
          case 5: return !player_.on_ground;
          case 6: {  // anim_end：最后一个画面且停留时间已过
            if (player_.action < 0 ||
                player_.action >= static_cast<int>(o.actions.size())) {
              return false;
            }
            const Action& a = o.actions[static_cast<size_t>(player_.action)];
            if (a.frames.empty()) return false;
            const size_t last = a.frames.size() - 1;
            if (player_.frame != static_cast<int>(last)) return false;
            return player_.anim_time >=
                   static_cast<float>(P3dActionHold(a, last)) / 60.0f;
          }
          case 7: {  // 变量比较（op: 0< 1<= 2= 3>= 4>）
            auto it = variables_.find(c.name);
            const int v = it == variables_.end() ? 0 : it->second;
            switch (c.op) {
              case 0: return v < c.value;
              case 2: return v == c.value;
              case 3: return v >= c.value;
              case 4: return v > c.value;
              default: return v <= c.value;
            }
          }
          case 8: return c.value > 0 && (tick_ % static_cast<uint32_t>(c.value)) == 0;
          case 9: return false;  // 玩家自己，没有“离玩家”的概念
          case 10: {  // key_held：按住超过 N 帧
            const int k = KeyEnum(c.key.c_str());
            return k >= 0 && key_hold_[k] >= c.value;
          }
          default: return false;
        }
      }();
      if (!ok) {
        hit = false;
        break;
      }
    }
    if (!hit) continue;
    for (const RuleEffect& e : r.effects) {
      switch (e.type) {
        case 0:  // move
          if (e.x != 0 || e.y != 0) {
            player_.vx = static_cast<float>(e.x);
            player_.vy = static_cast<float>(e.y);
            moved = true;
          }
          break;
        case 8:  // move_add：叠加到当前速度（惯性/击退）
          player_.vx += static_cast<float>(e.x);
          player_.vy += static_cast<float>(e.y);
          moved = true;
          break;
        case 9: {  // move_input：按方向键移动（8 向自动归一化）
          int ix = 0;
          int iy = 0;
          if (eng::KeyDown(input, eng::Key::kLeft)) --ix;
          if (eng::KeyDown(input, eng::Key::kRight)) ++ix;
          if (eng::KeyDown(input, eng::Key::kUp)) --iy;
          if (eng::KeyDown(input, eng::Key::kDown)) ++iy;
          const float sx = static_cast<float>(e.x != 0 ? e.x : o.speed);
          const float sy = static_cast<float>(e.y != 0 ? e.y : o.speed);
          float len = std::sqrt(static_cast<float>(ix * ix + iy * iy));
          if (len > 0.0f) {
            player_.vx = static_cast<float>(ix) / len * sx;
            player_.vy = static_cast<float>(iy) / len * sy;
            moved = true;
            if (ix != 0) player_.facing = ix > 0 ? 1 : -1;
          } else {
            player_.vx = 0.0f;
            player_.vy = 0.0f;
          }
          break;
        }
        case 10:  // var_add（全局变量）
          if (!e.text.empty()) variables_[e.text] += e.y;
          break;
        case 11:  // var_set（全局变量）
          if (!e.text.empty()) variables_[e.text] = e.y;
          break;
        case 1:  // move_px
          player_.x += static_cast<float>(e.x);
          player_.y += static_cast<float>(e.y);
          break;
        case 2:  // anim
          for (size_t ai = 0; ai < o.actions.size(); ++ai) {
            if (o.actions[ai].name == e.text) want_anim = static_cast<int>(ai);
          }
          break;
        case 3:  // flip
          if (e.text == "left") want_flip = -1;
          if (e.text == "right") want_flip = 1;
          break;
        case 5:  // sfx
          PlayClipById(e.text);
          break;
        case 6:  // spawn
          SpawnById(e.text, player_.x + static_cast<float>(e.x),
                    player_.y + static_cast<float>(e.y), player_.facing, true);
          break;
        default: break;
      }
    }
  }
  if (!moved) {
    player_.vx = 0.0f;
  }
  if (want_flip < 0) player_.facing = -1;
  if (want_flip > 0) player_.facing = 1;
  if (want_anim >= 0 && want_anim != player_.action) {
    player_.action = want_anim;
    player_.frame = 0;
    player_.anim_time = 0.0f;
  }
}

// 敌人规则：条件看自身状态，效果写 vx/vy/动作/朝向；位移直接作用在 e.x/e.y。
void Game::RunEnemyRules(const Object& o, EnemyState& e, float dt) {
  int want_anim = -1;
  int want_flip = 0;
  bool moved = false;
  for (const Rule& r : o.rules) {
    if (!r.enabled) continue;
    bool hit = true;
    for (const RuleCond& c : r.when) {
      const bool ok = [&]() {
        switch (c.type) {
          case 0: return true;
          case 4: return false;   // 敌人没有地面判定
          case 5: return true;
          case 7: {
            auto it = variables_.find(c.name);
            const int v = it == variables_.end() ? 0 : it->second;
            switch (c.op) {
              case 0: return v < c.value;
              case 2: return v == c.value;
              case 3: return v >= c.value;
              case 4: return v > c.value;
              default: return v <= c.value;
            }
          }
          case 8: return c.value > 0 && (e.tick % static_cast<uint32_t>(c.value)) == 0;
          case 9: {  // 离玩家 < N 像素
            const float dx = e.x - player_.x;
            const float dy = e.y - player_.y;
            return dx * dx + dy * dy <=
                   static_cast<float>(c.value) * static_cast<float>(c.value);
          }
          default: return false;
        }
      }();
      if (!ok) {
        hit = false;
        break;
      }
    }
    if (!hit) continue;
    for (const RuleEffect& ef : r.effects) {
      switch (ef.type) {
        case 0:  // move
          e.vx = static_cast<float>(ef.x);
          e.vy = static_cast<float>(ef.y);
          moved = true;
          break;
        case 8:  // move_add
          e.vx += static_cast<float>(ef.x);
          e.vy += static_cast<float>(ef.y);
          moved = true;
          break;
        case 1:  // move_px
          e.x += static_cast<float>(ef.x);
          e.y += static_cast<float>(ef.y);
          break;
        case 2:  // anim
          for (size_t ai = 0; ai < o.actions.size(); ++ai) {
            if (o.actions[ai].name == ef.text) want_anim = static_cast<int>(ai);
          }
          break;
        case 3:  // flip
          if (ef.text == "left") want_flip = -1;
          if (ef.text == "right") want_flip = 1;
          break;
        case 5:
          PlayClipById(ef.text);
          break;
        case 6:  // spawn
          SpawnById(ef.text, e.x + static_cast<float>(ef.x),
                    e.y + static_cast<float>(ef.y), e.facing, false);
          break;
        case 10:  // var_add
          if (!ef.text.empty()) variables_[ef.text] += ef.y;
          break;
        case 11:  // var_set
          if (!ef.text.empty()) variables_[ef.text] = ef.y;
          break;
        case 7:  // destroy
          e.dead = true;
          break;
        default: break;
      }
    }
  }
  if (moved) {
    e.x += e.vx * dt;
    e.y += e.vy * dt;
  }
  if (want_flip < 0) e.facing = -1;
  if (want_flip > 0) e.facing = 1;
  if (want_anim >= 0 && want_anim != e.action) {
    e.action = want_anim;
    e.frame = 0;
    e.anim_time = 0.0f;
  }
}

void Game::UpdateSimulation(float dt, const eng::InputState& input) {
  GameTrace("sim_enter");
  if (p3d_ == nullptr || player_object_ < 0) return;
  if (toast_timer_ > 0.0f) toast_timer_ -= dt;
  if (player_attack_active_ > 0.0f) player_attack_active_ -= dt;
  if (player_attack_cooldown_ > 0.0f) player_attack_cooldown_ -= dt;
  const Object& o = p3d_->objects[player_object_];

  // ★ 触发器 → 环境修饰器 ★
  //   对象所在位置命中哪些触发器，就把它们的修饰器收集起来，交给 ComposeStats 合成。
  env_mods_.clear();
  {
    const Scene& sc = p3d_->scenes[scene_index_];
    for (const Trigger& t : sc.triggers) {
      if (player_.x < static_cast<float>(t.x) || player_.x > static_cast<float>(t.x + t.w)) continue;
      if (player_.y < static_cast<float>(t.y) || player_.y > static_cast<float>(t.y + t.h)) continue;
      for (const TriggerMod& m : t.mods) {
        pgmmv::StatMod sm;
        sm.id = static_cast<pgmmv::StatId>(m.stat);
        sm.op = static_cast<pgmmv::StatOp>(m.op);
        sm.value = m.value;
        env_mods_.push_back(sm);
      }
    }
  }
  GameTrace("sim_after_env");
  // ★ 速度/跳跃/重力改成读"合成后的参数" ★
  //   base 来自对象字段，修饰器（水/毒/装备/…）以后往环境里加，这里不用改。
  pgmmv::StatBlock st = pgmmv::DefaultStats();
  st[pgmmv::StatId::kSpeed] = static_cast<float>(o.speed);
  st[pgmmv::StatId::kGravity] = static_cast<float>(o.gravity);
  st = pgmmv::ComposeStats(st, env_mods_);
  const float stat_gravity = st[pgmmv::StatId::kGravity];
  const float stat_maxfall = st[pgmmv::StatId::kMaxFall];
  player_.vy += stat_gravity * dt;  // 重力是常量，不需要写规则
  // 行为全部由规则驱动（没有规则 = 不动）
  RunPlayerRules(o, input, dt);
  GameTrace("sim_after_rules");
  if (player_.vy > stat_maxfall) player_.vy = stat_maxfall;
  player_.on_ground = false;
  ResolveX(player_.vx * dt, player_object_);
  ResolveY(player_.vy * dt, player_object_);
  GameTrace("sim_after_resolve");
  if (o.limit_screen) {  // 按当前帧的碰撞盒夹在 240x160 内
    const AttackBox cb = ColBoxOf(o, player_.action, player_.frame);
    const float left = -static_cast<float>(cb.x);
    const float right = view_w_ - static_cast<float>(cb.x + cb.w);
    const float top = -static_cast<float>(cb.y);
    const float bottom = view_h_ - static_cast<float>(cb.y + cb.h);
    player_.x = std::max(left, std::min(right, player_.x));
    player_.y = std::max(top, std::min(bottom, player_.y));
    if (player_.x <= left || player_.x >= right) player_.vx = 0.0f;
    if (player_.y >= bottom) player_.vy = 0.0f;
  }

  UpdateInteraction(input);
  GameTrace("sim_after_interaction");
  const Object& pc = p3d_->objects[player_object_];
  for (CheckpointState& cp : checkpoints_) {
    if (cp.active || cp.object < 0 || cp.object >= static_cast<int>(p3d_->objects.size())) continue;
    const Object& co = p3d_->objects[cp.object];
    const float cx0 = cp.x + static_cast<float>(co.col_x);
    const float cx1 = cx0 + static_cast<float>(co.col_w);
    const float cy0 = cp.y + static_cast<float>(co.col_y);
    const float cy1 = cy0 + static_cast<float>(co.col_h);
    const float px0 = player_.x + static_cast<float>(pc.col_x);
    const float px1 = px0 + static_cast<float>(pc.col_w);
    const float py0 = player_.y + static_cast<float>(pc.col_y);
    const float py1 = py0 + static_cast<float>(pc.col_h);
    if (px1 > cx0 && px0 < cx1 && py1 > cy0 && py0 < cy1) {
      cp.active = true;
      respawn_x_ = cp.x;
      respawn_y_ = cp.y;
      has_respawn_ = true;
      toast_ = "CHECK";
      toast_timer_ = 1.0f;
      AudioPlay(Sfx::kCoin);
    }
  }
  if (player_.y > 10000.0f) {
    SetScene(scene_index_);
    return;
  }
  GameTrace("sim_after_checkpoints");

  UpdateAnimation(dt);
  GameTrace("sim_after_anim");
  UpdateEnemies(dt);
  GameTrace("sim_after_enemies");
  UpdateProjectiles(dt);
  GameTrace("sim_after_projectiles");
  UpdateTriggers();
  GameTrace("sim_after_triggers");
  UpdateCamera(dt);
  GameTrace("sim_after_camera");
}

// 交互点：玩家在区域内且条件满足时，按键执行动作
void Game::UpdateInteraction(const eng::InputState& input) {
  if (eng::KeyPressed(input, eng::Key::kStart)) debug_on_ = !debug_on_;
  active_interaction_ = -1;
  if (p3d_ == nullptr) return;
  const Scene& sc = p3d_->scenes[scene_index_];
  for (size_t i = 0; i < sc.triggers.size(); ++i) {
    const Trigger& t = sc.triggers[i];
    if (!t.interaction.enabled) continue;
    if (i < interaction_done_.size() && interaction_done_[i] && t.interaction.once) continue;
    if (player_.x < static_cast<float>(t.x) || player_.x > static_cast<float>(t.x + t.w)) continue;
    if (player_.y < static_cast<float>(t.y) || player_.y > static_cast<float>(t.y + t.h)) continue;
    if (!t.interaction.require_variable.empty()) {
      auto it = variables_.find(t.interaction.require_variable);
      const int v = it == variables_.end() ? 0 : it->second;
      if (v != t.interaction.require_value) continue;
    }
    active_interaction_ = static_cast<int>(i);
    if (!t.interaction.prompt.empty()) {
      toast_ = t.interaction.prompt;
      toast_timer_ = std::max(toast_timer_, 0.1f);
    }
    break;
  }
  if (active_interaction_ < 0) return;
  const Trigger& t = sc.triggers[static_cast<size_t>(active_interaction_)];
  bool pressed = false;
  const std::string& k = t.interaction.key;
  if (k == "b") pressed = eng::KeyPressed(input, eng::Key::kB);
  else if (k == "a") pressed = eng::KeyPressed(input, eng::Key::kA);
  else if (k == "start") pressed = eng::KeyPressed(input, eng::Key::kStart);
  else pressed = eng::KeyPressed(input, eng::Key::kB);
  if (!pressed) return;
  if (interaction_done_.size() <= static_cast<size_t>(active_interaction_)) {
    interaction_done_.resize(sc.triggers.size(), 0);
  }
  interaction_done_[static_cast<size_t>(active_interaction_)] = 1;
  for (const TriggerAction& a : t.interaction.actions) {
    if (a.type == 0) {  // change_scene
      for (size_t si = 0; si < p3d_->scenes.size(); ++si) {
        if (p3d_->scenes[si].id == a.target) {
          SetScene(static_cast<int>(si));
          return;
        }
      }
    } else if (a.type == 2) {  // set_variable
      variables_[a.target] = a.value;
    } else if (a.type == 5) {  // dialogue → 用 toast 显示一句
      toast_ = a.target;
      toast_timer_ = 2.0f;
    } else if (a.type == 6) {  // play_sfx
      PlayClipById(a.target);
    }
  }
}

void Game::Update(float dt, const eng::InputState& input) {
  GameTrace("update_enter");
  if (p3d_ == nullptr || player_object_ < 0) return;
  for (int i = 0; i < eng::kKeyCount; ++i) {
    key_hold_[i] = input.down[i] ? key_hold_[i] + 1 : 0;
  }
  ++frame_count_;
  ++tick_;
  if (dt < 0.0f) dt = 0.0f;
  if (dt > 0.05f) dt = 0.05f;
  GameTrace("update_after_keys");
  if (UiModalActive()) {
    const int top = modal_stack_.back();
    if (top >= 0 && top < static_cast<int>(p3d_->ui_screens.size())) {
      const UiScreenData& screen = p3d_->ui_screens[static_cast<size_t>(top)];
      if (modal_closing_) {
        ++modal_close_fade_;
        if (modal_close_fade_ >= modal_close_total_) CloseUi();
        prev_input_ = input;
        return;
      }
      if (screen.dismiss == 2) {
        --modal_timer_;
        if (modal_timer_ <= 0) {
          RequestCloseUi();
          prev_input_ = input;
          return;
        }
      }
      if (modal_fade_ > 0) --modal_fade_;
      if (input.pressed[static_cast<int>(eng::Key::kUp)]) {
        MoveModalFocus(-1, true);
      }
      if (input.pressed[static_cast<int>(eng::Key::kDown)]) {
        MoveModalFocus(1, true);
      }
      if (input.pressed[static_cast<int>(eng::Key::kLeft)]) {
        MoveModalFocus(-1, false);
      }
      if (input.pressed[static_cast<int>(eng::Key::kRight)]) {
        MoveModalFocus(1, false);
      }
      if (input.pressed[static_cast<int>(eng::Key::kA)]) {
        if (modal_focus_ >= 0) {
          ActivateModalFocus();
        } else if (screen.dismiss == 0) {
          RequestCloseUi();
        }
      }
      if ((input.pressed[static_cast<int>(eng::Key::kB)] ||
           input.pressed[static_cast<int>(eng::Key::kStart)]) &&
          screen.dismiss != 3) {
        RequestCloseUi();
      }
    }
    prev_input_ = input;
    return;
  }
  GameTrace("update_before_sim");
  UpdateSimulation(dt, input);
  GameTrace("update_after_sim");
  prev_input_ = input;  // 下一帧算 key_up 用
}

void Game::RenderWorld(eng::DrawList& dl, const eng::TextureId* textures,
                            int texture_count) const {
  if (p3d_ == nullptr) return;
  const Scene& s = p3d_->scenes[scene_index_];
  // 用场景实际 tileset 的 tile 尺寸，不能再硬编码 8x8。
  float tw = 8.0f;
  float th = 8.0f;
  for (int ts_index : s.tilesets) {
    if (ts_index < 0 || ts_index >= static_cast<int>(p3d_->tilesets.size())) continue;
    const Tileset& ts = p3d_->tilesets[static_cast<size_t>(ts_index)];
    if (ts.tile_w > 0 && ts.tile_h > 0) {
      tw = static_cast<float>(ts.tile_w);
      th = static_cast<float>(ts.tile_h);
      break;
    }
  }
  dl.Clear(0xFF000000u);

  // ★ v41：一层里的格子可以来自多个图集页（槽位）★
  //   先按可见范围把格子收进列表（记住槽位），再逐槽位取纹理画。
  struct CellDraw {
    int slot;
    int index;
    float x;
    float y;
  };
  auto draw_layers = [&](bool foreground_pass) {
    for (const SceneLayer& layer : s.layers) {
      if (!layer.visible) continue;
      if (layer.foreground != foreground_pass) continue;
      if (s.tilesets.empty()) continue;
      const float cam_x =
          camera_x_ * static_cast<float>(layer.scroll_x) / 100.0f;
      const float cam_y =
          camera_y_ * static_cast<float>(layer.scroll_y) / 100.0f;
      const int view_w = view_w_;
      const int view_h = view_h_;
      const int c0 = static_cast<int>(std::floor(cam_x / tw)) - 1;
      const int c1 = static_cast<int>(std::ceil((cam_x + view_w) / tw)) + 1;
      const int r0 = static_cast<int>(std::floor(cam_y / th)) - 1;
      const int r1 = static_cast<int>(std::ceil((cam_y + view_h) / th)) + 1;
      std::vector<CellDraw> cells;
      cells.reserve(4096);
      for (const SceneChunk& chunk : layer.chunks) {
        const int base_c = chunk.cx * kP3dChunkCells;
        const int base_r = chunk.cy * kP3dChunkCells;
        if (base_c + kP3dChunkCells <= c0 || base_c > c1 ||
            base_r + kP3dChunkCells <= r0 || base_r > r1) {
          continue;
        }
        for (int ly = 0; ly < kP3dChunkCells; ++ly) {
          const int r = base_r + ly;
          if (r < r0 || r > r1) continue;
          for (int lx = 0; lx < kP3dChunkCells; ++lx) {
            const int c = base_c + lx;
            if (c < c0 || c > c1) continue;
            const size_t idx = static_cast<size_t>(ly) * kP3dChunkCells + lx;
            if (idx >= chunk.cells.size() || chunk.cells[idx] < 0) continue;
            const int v = chunk.cells[idx];
            const int slot = r4::P3dCellSlot(v);
            if (slot < 0 || slot >= static_cast<int>(s.tilesets.size())) continue;
            const float cy2 = static_cast<float>(r) * th - cam_y;
            cells.push_back(CellDraw{slot, r4::P3dCellIndex(v),
                                     static_cast<float>(c) * tw - cam_x, cy2});
          }
        }
      }
      for (size_t si = 0; si < s.tilesets.size(); ++si) {
        const int ts_index = s.tilesets[si];
        if (ts_index < 0 ||
            ts_index >= static_cast<int>(p3d_->tilesets.size())) {
          continue;
        }
        const Tileset& ts = p3d_->tilesets[ts_index];
        if (ts.image < 0 || ts.image >= texture_count) continue;
        const eng::TextureId tex = textures[ts.image];
        if (tex == 0) continue;
        const int cols = std::max(1, ts.columns);
        for (const CellDraw& cd : cells) {
          if (cd.slot != static_cast<int>(si)) continue;
          const eng::TexRegion src = eng::TexRegion::Make(
              (cd.index % cols) * ts.tile_w, (cd.index / cols) * ts.tile_h,
              ts.tile_w, ts.tile_h);
          dl.DrawImage(
              eng::Rect2{eng::Vec2{cd.x, cd.y}, eng::Vec2{tw, th}}, tex,
              0xFFFFFFFFu, src);
        }
      }
    }
  };
  draw_layers(false);

  auto draw_object = [&](int obj_index, float x, float y, int action_index,
                          int frame_index, bool flip_x) {
    if (obj_index < 0 || obj_index >= static_cast<int>(p3d_->objects.size())) return;
    const Object& o = p3d_->objects[obj_index];
    if (o.image < 0 || o.image >= texture_count) return;
    const eng::TextureId tex = textures[o.image];
    if (tex == 0) return;
    int frame_no = 0;
    const int fw = std::max(1, o.frame_w);
    const int fh = std::max(1, o.frame_h);
    int origin_x = fw / 2;
    int origin_y = fh / 2;
    const Action* act = nullptr;
    if (action_index >= 0 && action_index < static_cast<int>(o.actions.size())) {
      act = &o.actions[action_index];
      if (!act->frames.empty()) {
        int fi = frame_index;
        if (fi < 0) fi = 0;
        if (fi >= static_cast<int>(act->frames.size())) fi = static_cast<int>(act->frames.size()) - 1;
        frame_no = act->frames[fi].image;
        origin_x = act->frames[fi].origin_x;
        origin_y = act->frames[fi].origin_y;
      }
    }
    const int off_x = act != nullptr ? act->offset_x : 0;
    const int off_y = act != nullptr ? act->offset_y : 0;
    const int tex_w = (o.image >= 0 && o.image < static_cast<int>(p3d_->images.size()))
                          ? p3d_->images[o.image].width
                          : fw;
    const int tc = (tex_w / fw > 0) ? frame_no % (tex_w / fw) : 0;
    const int tr = (tex_w / fw > 0) ? frame_no / (tex_w / fw) : 0;
    const eng::TexRegion src = eng::TexRegion::Make(tc * fw, tr * fh, fw, fh);
    eng::u8 flags = 0;
    if (flip_x) flags = static_cast<eng::u8>(flags ^ eng::kDrawFlipH);
    if (act != nullptr && act->flip_h) {
      flags = static_cast<eng::u8>(flags ^ eng::kDrawFlipH);
    }
    if (act != nullptr && act->flip_v) flags = static_cast<eng::u8>(flags | eng::kDrawFlipV);
    dl.DrawImage(eng::Rect2{
                     eng::Vec2{x - camera_x_ - static_cast<float>(origin_x) +
                                   static_cast<float>(off_x),
                               y - camera_y_ - static_cast<float>(origin_y) +
                                   static_cast<float>(off_y)},
                     eng::Vec2{static_cast<float>(fw), static_cast<float>(fh)}},
                 tex, 0xFFFFFFFFu, src, flags);
  };

  for (int i = 0; i < static_cast<int>(s.objects.size()); ++i) {
    const SceneObject& so = s.objects[i];
    if (so.object == player_object_) continue;
    if (so.object < 0 || so.object >= static_cast<int>(p3d_->objects.size())) continue;
    bool enemy_seen = false;
    for (const EnemyState& e : enemies_) {
      if (e.scene_object != i) continue;
      enemy_seen = true;
      if (e.dead) break;
      draw_object(e.object, e.x, e.y, e.action, e.frame, e.facing < 0);
      break;
    }
    if (enemy_seen) continue;
    const Object& eo = p3d_->objects[so.object];
    int action = so.action >= 0 ? so.action : eo.default_action;
    int frame = 0;
    if (action >= 0 && action < static_cast<int>(eo.actions.size())) {
      const Action& a = eo.actions[action];
      if (!a.frames.empty()) {
        const float at = so.action >= 0
                             ? static_cast<float>(frame_count_ - so.anim_start) / 60.0f
                             : static_cast<float>(frame_count_) / 60.0f;
        frame = ActionFrameAt(a, at, nullptr);
      }
    }
    draw_object(so.object, static_cast<float>(so.x), static_cast<float>(so.y), action,
                frame, false);
  }
  RenderProjectiles(dl, textures, texture_count);
  if (player_object_ >= 0) {
    draw_object(player_object_, player_.x, player_.y, player_.action, player_.frame,
                player_.facing > 0);
  }

  draw_layers(true);


}

void Game::Render(eng::DrawList& dl, const eng::TextureId* textures,
                  int texture_count) const {
  // 注意：DrawToast 在 const 方法里不能调用，这里用 const_cast（只为画字，不改状态）
  Game* self = const_cast<Game*>(this);
  if (p3d_ == nullptr) return;
  RenderWorld(dl, textures, texture_count);
  self->DrawToast(dl, textures, texture_count);
}

}  // namespace r4
