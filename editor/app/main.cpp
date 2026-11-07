// editor/app/main.cpp
//
// R1b：单窗口编辑器壳 + 资源工具 + 设置页（最小闭环）。
//
// 布局见 docs/gui-minimal.md：
//   菜单 / 六工具 / 左列表 / 中编辑区 / 右属性 / 底部状态
// 只做一个主窗口；不引入多窗口、停靠、浮窗。
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <d3d11.h>
#include <mmsystem.h>

#include <algorithm>

#include "pgmmv/stats.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <fstream>

#include "base/log.h"
#include "base/version.h"
#include "pgmmv/export.h"
#include "pgmmv/project.h"
#include "runtime/audio.h"
#include "theme.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace {

constexpr UINT kDefaultWidth = 1280;
constexpr UINT kDefaultHeight = 800;
constexpr const char* kProduct = "PocketLoom";

struct D3DState {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain* swap = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  UINT width = 0;
  UINT height = 0;
};

D3DState g_d3d;
HWND g_hwnd = nullptr;
bool g_quit = false;
bool g_request_exit = false;
int g_frames_left = -1;
bool g_smoke = false;
bool g_font_cjk = false;


// ---- R0 的 UI 点击自检（保留） --------------------------------------------

// ---- R1b 项目状态 ----------------------------------------------------------
bool g_has_project = false;
pgmmv::Project g_project;
bool g_dirty = false;
std::string g_selected_asset;
std::string g_status = "就绪";
std::string g_error_message;
std::string g_validation_text;
std::string g_dependency_text;
bool g_show_error = false;

char g_project_name_buf[128] = {};
char g_initial_scene_buf[128] = {};
char g_asset_name_buf[128] = {};
char g_object_filter[64] = {};

enum class PendingAction { kNone, kNew, kOpen, kExit };
PendingAction g_pending = PendingAction::kNone;
bool g_show_confirm = false;
bool g_show_rename_object = false;
char g_rename_object_buf[128] = {};

// 预览纹理
struct PreviewTexture {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  int width = 0;
  int height = 0;
  std::string asset_id;
  bool failed = false;
};
PreviewTexture g_preview;
std::map<std::string, PreviewTexture> g_tileset_previews;

// ★ 缩略图必须用线性采样 ★
//   全局采样器已经是 NEAREST（像素图放大要清晰），但**缩小**时点采样等于
//   "每隔 N 像素抽一个" —— 509x340 的截图缩到 18x18 会变成一堆随机色块
//   （看起来就像"素材变红了"）。规则：放大/1:1 用 NEAREST，缩小用 LINEAR。
ID3D11SamplerState* g_samp_point = nullptr;
ID3D11SamplerState* g_samp_linear = nullptr;

void EnsureSamplers() {
  if (g_samp_point != nullptr || g_d3d.device == nullptr) return;
  D3D11_SAMPLER_DESC d;
  std::memset(&d, 0, sizeof(d));
  d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  d.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
  d.MaxLOD = D3D11_FLOAT32_MAX;
  d.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  g_d3d.device->CreateSamplerState(&d, &g_samp_point);
  d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  g_d3d.device->CreateSamplerState(&d, &g_samp_linear);
}

void BindPointSampler(const ImDrawList*, const ImDrawCmd*) {
  if (g_d3d.ctx != nullptr && g_samp_point != nullptr) g_d3d.ctx->PSSetSamplers(0, 1, &g_samp_point);
}
void BindLinearSampler(const ImDrawList*, const ImDrawCmd*) {
  if (g_d3d.ctx != nullptr && g_samp_linear != nullptr) g_d3d.ctx->PSSetSamplers(0, 1, &g_samp_linear);
}

// 画一张缩略图/预览：source_w 与 dst_w 决定用哪种采样
void ImageAutoFilter(ImTextureID tex, ImVec2 size, float src_w) {
  EnsureSamplers();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool shrink = src_w > 0.0f && size.x < src_w - 0.5f;
  if (shrink) dl->AddCallback(BindLinearSampler, nullptr);
  ImGui::Image(tex, size);
  if (shrink) dl->AddCallback(BindPointSampler, nullptr);
}
PreviewTexture* AssetPreview(const pgmmv::Asset& a);

// R1b/R2 自动化（文件对话框走测试路径覆盖；窗口/预览/保存仍是真实路径）
std::filesystem::path g_cli_open;
std::filesystem::path g_cli_r1_create;
std::filesystem::path g_cli_r1_png;
std::string g_r1_report_path;


bool TextMatchesFilter(const std::string& text, const char* filter) {
  if (filter == nullptr || filter[0] == '\0') return true;
  std::string hay = text;
  std::string needle = filter;
  for (char& ch : hay) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  for (char& ch : needle) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return hay.find(needle) != std::string::npos;
}

// ---- R2 对象工具状态 -------------------------------------------------------
std::string g_selected_object;
std::string g_selected_action = "idle";
char g_action_frames_buf[256] = {};
std::string g_action_frames_key;
bool g_action_frames_invalid = false;
char g_action_name_buf[128] = {};
std::string g_action_name_key;
int g_anim_frame = 0;
int g_anim_direction = 0;
// 动画页选中的是「动画资源」，不是对象
std::string g_selected_animation;
bool g_anim_playing = false;
double g_anim_accum = 0.0;

std::filesystem::path g_cli_r2_create;
std::filesystem::path g_cli_r2_png;
std::string g_r2_report_path;

// ---- R3 图块/场景/过渡状态 ------------------------------------------------
std::string g_selected_tileset;
std::string g_selected_scene;
std::string g_selected_object_for_place;
std::string g_replace_object;
bool g_asset_index_dirty = true;
std::map<std::string, std::vector<std::string>> g_asset_object_refs;
std::map<std::string, std::vector<std::string>> g_asset_tileset_refs;
std::map<std::string, std::vector<std::string>> g_asset_scene_refs;
int g_selected_placed_object = -1;
int g_selected_trigger = -1;
std::string g_selected_ui_screen;
int g_selected_ui_element = -1;
bool g_ui_dragging = false;
bool g_ui_resizing = false;
int g_ui_resize_w0 = 0;
int g_ui_resize_h0 = 0;
ImVec2 g_ui_resize_mouse0;
std::set<int> g_placed_multi;
std::vector<pgmmv::SceneObject> g_scene_clipboard;
int g_selected_tile = 0;
int g_paint_tile = 0;
int g_paint_slot = 0;  // 画笔的图集槽位（scene.tilesets 下标）
int g_brush_x = 0;
int g_brush_y = 0;
int g_brush_start_x = 0;
int g_brush_start_y = 0;
int g_brush_w = 1;
int g_brush_h = 1;
bool g_brush_dragging = false;
int g_scene_layer = 0;  // 0 tile, 1 object

// ★ 场景页当前选中的节点 —— 右栏属性栏按它分发 ★
enum class SceneSelKind { kScene = 0, kLayer, kTrigger, kObject, kCamera };
SceneSelKind g_scene_sel_kind = SceneSelKind::kScene;
bool g_scene_place_mode = false;
bool g_scene_place_trigger = false;  // 放置触发器：左键点关卡建一个 64x64
bool g_scene_collision_mode = false;
bool g_scene_trigger_mode = false;   // 触发层（矩形，右键拖新建）
int g_box_draw_target = 1;  // 0=碰撞 1=受击 2=攻击（画布拖拽 + 判定状态共用）
int g_active_tile_layer = 0;
int g_tile_sel_start = -1;
int g_tile_sel_end = -1;
float g_scene_zoom = 1.0f;
bool g_scene_fit_requested = false;
bool g_scene_origin_view_requested = true;
ImVec2 g_scene_pan = ImVec2(0.0f, 0.0f);
bool g_scene_focus_pending = false;
float g_scene_focus_x = 0.0f;
float g_scene_focus_y = 0.0f;
bool g_camera_frame_follow_spawn = false;
bool g_show_chunk_grid = true;
int g_collision_mask = 15;  // bit0 上, bit1 右, bit2 下, bit3 左, bit4 单向
bool g_scene_stroke_active = false;
struct SceneClip {
  bool valid = false;
  bool collision = false;
  int layer = -1;
  int w = 0;
  int h = 0;
  std::vector<int> cells;
  std::vector<unsigned char> masks;
};
SceneClip g_scene_clip;
int g_drag_placed_object = -1;
bool g_drag_started = false;

std::vector<pgmmv::Project> g_undo_stack;
std::vector<pgmmv::Project> g_redo_stack;
char g_tileset_name_buf[128] = {};
char g_scene_name_buf[128] = {};
int g_selected_transition = -1;

std::filesystem::path g_cli_r3_create;
std::filesystem::path g_cli_r3_png;
std::string g_r3_report_path;

// ---- R4 导出 / 试玩 -------------------------------------------------------
std::filesystem::path g_cli_r4_create;
std::filesystem::path g_cli_r4_tiles;
std::filesystem::path g_cli_r4_player;
std::filesystem::path g_cli_r4_p3d;
std::string g_r4_report_path;
HANDLE g_player_process = nullptr;

// ---- 基础路径 --------------------------------------------------------------

std::string PathText(const std::filesystem::path& p) { return p.u8string(); }

void SetStatus(const std::string& text) { g_status = text; }

void SetError(const std::string& text) {
  g_error_message = text;
  g_show_error = true;
  g_status = "错误：" + text;
}

void MarkDirty() {
  g_dirty = true;
  g_asset_index_dirty = true;
}

void PushUndo() {
  if (!g_has_project) return;
  if (g_undo_stack.size() >= 64) g_undo_stack.erase(g_undo_stack.begin());
  g_undo_stack.push_back(g_project);
  g_redo_stack.clear();
}

// ★ 数值输入 + 撤销 ★
//   先在局部副本上编辑，「开始编辑」的那一帧压撤销 —— 快照里还是编辑前的值。
//   直接写成 PushUndo() 在 if 里会晚一帧：点 +/− 的那一下已经把新值写进去了。
bool UndoIntField(const char* label, int* target, int min_value = -1000000000) {
  int v = *target;
  const bool changed = ImGui::InputInt(label, &v);
  if (ImGui::IsItemActivated()) PushUndo();
  if (!changed) return false;
  if (v < min_value) v = min_value;
  if (v == *target) return false;
  *target = v;
  MarkDirty();
  return true;
}

// ---- 统一按钮/排布 ---------------------------------------------------------
constexpr float kRowButtonH = 24.0f;

bool RowButton(const char* label) {
  return ImGui::Button(label, ImVec2(-1.0f, kRowButtonH));
}

bool InlineButton(const char* label, float width = 88.0f) {
  return ImGui::Button(label, ImVec2(width, 22.0f));
}

bool DialogButton(const char* label) {
  return ImGui::Button(label, ImVec2(110.0f, 28.0f));
}

void PushDangerColors() {
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.48f, 0.16f, 0.16f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.64f, 0.22f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.76f, 0.28f, 0.28f, 1.0f));
}

bool DangerRowButton(const char* label) {
  PushDangerColors();
  const bool clicked = ImGui::Button(label, ImVec2(-1.0f, kRowButtonH));
  ImGui::PopStyleColor(3);
  return clicked;
}

bool DangerSmallButton(const char* label) {
  PushDangerColors();
  const bool clicked = ImGui::SmallButton(label);
  ImGui::PopStyleColor(3);
  return clicked;
}

bool DangerInlineButton(const char* label, float width = 88.0f) {
  PushDangerColors();
  const bool clicked = ImGui::Button(label, ImVec2(width, 22.0f));
  ImGui::PopStyleColor(3);
  return clicked;
}

// 列表行末尾的“x”：统一贴右，不跟字段宽度漂移。
bool RightSmallButton(const char* label) {
  ImGui::SameLine();
  const float x = ImGui::GetWindowContentRegionMax().x - 26.0f;
  if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
  return ImGui::SmallButton(label);
}

// 文本输入 + 撤销：同样在激活那一帧压一次，打字过程中不重复压。
void PushUndoOnActivate() {
  if (ImGui::IsItemActivated()) PushUndo();
}

void SyncProjectBuffers() {
  g_asset_index_dirty = true;
  std::snprintf(g_project_name_buf, sizeof(g_project_name_buf), "%s",
                g_has_project ? g_project.name.c_str() : "");
  std::snprintf(g_initial_scene_buf, sizeof(g_initial_scene_buf), "%s",
                g_has_project ? g_project.settings.initial_scene.c_str() : "");
}

void SyncAssetBuffer() {
  g_asset_name_buf[0] = '\0';
  if (!g_has_project || g_selected_asset.empty()) {
    return;
  }
  const pgmmv::Asset* a = g_project.FindAsset(g_selected_asset);
  if (a != nullptr) {
    std::snprintf(g_asset_name_buf, sizeof(g_asset_name_buf), "%s", a->name.c_str());
  }
}

void PreviewRelease();

// ---- R2 对象/动作辅助 ------------------------------------------------------
pgmmv::ObjectDoc* CurrentObject() {
  if (!g_has_project || g_selected_object.empty()) return nullptr;
  return g_project.FindObject(g_selected_object);
}


// 动作在动画资源里；这几个helper让旧代码继续按"对象的动作"来写
std::vector<pgmmv::ActionDef>* ActionsOf(pgmmv::ObjectDoc* o) {
  // 新对象可以暂时没有动画资源；这里返回空表，避免调用方解引用 null。
  static std::vector<pgmmv::ActionDef> no_actions;
  no_actions.clear();
  if (o == nullptr) return &no_actions;
  std::vector<pgmmv::ActionDef>* acts =
      pgmmv::ObjectActionsMutable(g_project, *o);
  return acts != nullptr ? acts : &no_actions;
}


pgmmv::ActionDef* FindActionIn(pgmmv::ObjectDoc* o, const std::string& name) {
  if (o == nullptr) return nullptr;
  return pgmmv::FindObjectActionMutable(g_project, *o, name);
}

std::string FramesToText(const pgmmv::ActionDef& a) {
  std::string s;
  for (size_t i = 0; i < a.frames.size(); ++i) {
    if (i > 0) s += ",";
    s += std::to_string(a.frames[i].image);
  }
  return s;
}

void SyncActionFramesBuffer() {
  pgmmv::ObjectDoc* obj = CurrentObject();
  if (obj == nullptr) {
    g_action_frames_buf[0] = '\0';
    g_action_frames_key.clear();
    g_action_frames_invalid = false;
    return;
  }
  const pgmmv::ActionDef* a = FindActionIn(obj, g_selected_action);
  if (a == nullptr) {
    if (!(*ActionsOf(obj)).empty()) {
      g_selected_action = (*ActionsOf(obj)).front().name;
      a = FindActionIn(obj, g_selected_action);
    }
  }
  const std::string key = obj->id + "/" + g_selected_action;
  if (key != g_action_frames_key) {
    g_action_frames_key = key;
    g_action_frames_invalid = false;
    g_anim_frame = 0;
    g_anim_accum = 0.0;
  }
  if (a != nullptr) {
    std::snprintf(g_action_frames_buf, sizeof(g_action_frames_buf), "%s",
                  FramesToText(*a).c_str());
  } else {
    g_action_frames_buf[0] = '\0';
  }
}

void SyncActionNameBuffer() {
  pgmmv::ObjectDoc* obj = CurrentObject();
  if (obj == nullptr) {
    g_action_name_buf[0] = '\0';
    g_action_name_key.clear();
    return;
  }
  const std::string key = obj->id + "/" + g_selected_action;
  if (key != g_action_name_key) {
    g_action_name_key = key;
    const pgmmv::ActionDef* a = FindActionIn(obj, g_selected_action);
    std::snprintf(g_action_name_buf, sizeof(g_action_name_buf), "%s",
                  a != nullptr ? a->name.c_str() : "");
  }
}

void SyncObjectBuffers() {
  SyncActionFramesBuffer();
  SyncActionNameBuffer();
}

void SelectAction(const std::string& name) {
  { std::FILE* lf = std::fopen("editor-ui.log", "a");
    if (lf) { std::fprintf(lf, "select enter\n"); std::fclose(lf); } }
  g_selected_action = name;
    g_anim_frame = 0;
  g_anim_playing = false;
  g_anim_accum = 0.0;
  SyncActionFramesBuffer();
  { std::FILE* lf = std::fopen("editor-ui.log", "a");
    if (lf) { std::fprintf(lf, "select frames synced\n"); std::fclose(lf); } }
  SyncActionNameBuffer();
  { std::FILE* lf = std::fopen("editor-ui.log", "a");
    if (lf) { std::fprintf(lf, "select done\n"); std::fclose(lf); } }
}

int EditorActionHold(const pgmmv::DirectionDef& a, int index) {
  if (index >= 0 && index < static_cast<int>(a.frames.size()) &&
      a.frames[index].hold > 0) {
    return a.frames[index].hold;
  }
  return pgmmv::ActionDefaultHold(a);
}

void EditorEnsureActionHolds(pgmmv::DirectionDef* a) {
  if (a == nullptr) return;
  pgmmv::EnsureActionHolds(a);
}

void EditorClampAnimFrame(const pgmmv::DirectionDef* a) {
  if (a == nullptr || a->frames.empty()) {
    g_anim_frame = 0;
    return;
  }
  g_anim_frame = std::max(0, std::min(g_anim_frame,
                                      static_cast<int>(a->frames.size()) - 1));
}

pgmmv::AnimationDoc* EnsureAnimation();

// 动作属于动画资源：增/删/改名都改资源，同时同步对象的默认动作引用
void EditorAddAction() {
  pgmmv::AnimationDoc* ad = EnsureAnimation();
  if (ad == nullptr) return;
  std::string base = "action";
  std::string name = base;
  int suffix = 2;
  for (const pgmmv::ActionDef& a : ad->actions) {
    if (a.name == name) {
      name = base + "_" + std::to_string(suffix++);
    }
  }
  PushUndo();
  pgmmv::ActionDef a;
  a.name = name;
  a.fps = 8;
  a.loop = true;
  ad->actions.push_back(a);
  SelectAction(name);
  MarkDirty();
}

void EditorDeleteSelectedAction() {
  pgmmv::AnimationDoc* ad = EnsureAnimation();
  if (ad == nullptr || ad->actions.size() <= 1) return;
  const std::string name = g_selected_action;
  PushUndo();
  ad->actions.erase(
      std::remove_if(ad->actions.begin(), ad->actions.end(),
                     [&](const pgmmv::ActionDef& a) { return a.name == name; }),
      ad->actions.end());
  for (pgmmv::ObjectDoc& o : g_project.objects) {
    if (o.animation != ad->id) continue;
    if (o.default_action == name && !ad->actions.empty()) {
      o.default_action = ad->actions.front().name;
    }
  }
  if (!ad->actions.empty()) SelectAction(ad->actions.front().name);
  MarkDirty();
}

void EditorRenameAction(const std::string& new_name) {
  pgmmv::AnimationDoc* ad = EnsureAnimation();
  if (ad == nullptr || new_name.empty() || new_name == g_selected_action) return;
  for (const pgmmv::ActionDef& a : ad->actions) {
    if (a.name == new_name) return;
  }
  pgmmv::ActionDef* a = nullptr;
  for (pgmmv::ActionDef& x : ad->actions) {
    if (x.name == g_selected_action) a = &x;
  }
  if (a == nullptr) return;
  const std::string old_name = a->name;
  a->name = new_name;
  for (pgmmv::ObjectDoc& o : g_project.objects) {
    if (o.animation != ad->id) continue;
    if (o.default_action == old_name) o.default_action = new_name;
  }
  g_selected_action = new_name;
  g_action_name_key.clear();
  MarkDirty();
}

void SelectObject(const std::string& id) {
  g_selected_object = id;
  g_selected_object_for_place = id;  // 场景放置跟随当前对象
    pgmmv::ObjectDoc* obj = CurrentObject();
  if (obj != nullptr) {
    if (!obj->animation.empty()) g_selected_animation = obj->animation;
    if (FindActionIn(obj, g_selected_action) == nullptr) {
      g_selected_action = (*ActionsOf(obj)).empty() ? "idle" : (*ActionsOf(obj)).front().name;
    }
  }
  g_anim_frame = 0;
  g_anim_playing = false;
  g_anim_accum = 0.0;
  PreviewRelease();
  SyncObjectBuffers();
}


// ★ 改帧尺寸时跟着走 ★
//   原点/碰撞盒如果还是"脚下中心"的推导值，就按新尺寸重算；
//   一旦你手调过（不再等于推导值），就不再自动动它。
// 新建对象时先选的图集（空 = 不给图，进对象后再选）
std::string g_new_object_image;

void CreateObject() {
  if (!g_has_project) return;
  PushUndo();
  int n = 1;
  std::string id;
  do {
    id = "object_" + std::to_string(n);
    ++n;
  } while (g_project.FindObject(id) != nullptr);

  const std::string name = "Object " + std::to_string(n - 1);
  pgmmv::ObjectDoc o = pgmmv::ObjectCreateDefault(id, name);
  // ★ 用"新建时选的图集"，不再默认抓工程里第一张图（很容易抓到场景图集）★
  if (!g_new_object_image.empty()) o.image = g_new_object_image;
  // 只建对象，不自动建动画。动画由用户在对象页「动画资源」里选择。
  o.animation.clear();
  g_project.objects.push_back(std::move(o));
  g_dirty = true;
  SelectObject(id);
  g_selected_object_for_place = id;  // 场景页“+ 对象实例”默认放刚建的对象
    SetStatus("已新建对象：" + name + "，请在对象页选择动画资源（未保存）");
}




// ---- R3 图块/场景/过渡辅助 ------------------------------------------------
pgmmv::TilesetDoc* CurrentTileset() {
  if (!g_has_project || g_selected_tileset.empty()) return nullptr;
  return g_project.FindTileset(g_selected_tileset);
}
const pgmmv::TilesetDoc* CurrentTilesetC() {
  if (!g_has_project || g_selected_tileset.empty()) return nullptr;
  return g_project.FindTileset(g_selected_tileset);
}
pgmmv::SceneDoc* CurrentScene() {
  if (!g_has_project || g_selected_scene.empty()) return nullptr;
  return g_project.FindScene(g_selected_scene);
}


const pgmmv::SceneDoc* CurrentSceneC() {
  if (!g_has_project || g_selected_scene.empty()) return nullptr;
  return g_project.FindScene(g_selected_scene);
}

// 出生点 = 场景里玩家对象实例的位置（场景不再单独存起点）
int SceneSpawnX(pgmmv::SceneDoc* s);
int SceneSpawnY(pgmmv::SceneDoc* s);

// ---- v41 图集页表：槽位辅助 ----------------------------------------------
// 老工程/新场景的 tilesets 可能是空表：用第一层的旧字段补一条，保证槽位 0 有东西。
void EnsureSceneTilesets(pgmmv::SceneDoc* s) {
  if (s == nullptr || !s->tilesets.empty()) return;
  if (!g_project.tilesets.empty()) {
    s->tilesets.push_back(g_project.tilesets.front().id);
  }
}
int ClampPaintSlot(const pgmmv::SceneDoc& s) {
  if (s.tilesets.empty()) {
    g_paint_slot = 0;
    return 0;
  }
  if (g_paint_slot < 0) g_paint_slot = 0;
  if (g_paint_slot >= static_cast<int>(s.tilesets.size())) {
    g_paint_slot = static_cast<int>(s.tilesets.size()) - 1;
  }
  return g_paint_slot;
}
const pgmmv::TilesetDoc* TilesetAtSlotC(const pgmmv::SceneDoc& s, int slot) {
  if (slot < 0 || slot >= static_cast<int>(s.tilesets.size())) return nullptr;
  return g_project.FindTileset(s.tilesets[slot]);
}
// 删槽位：只允许删没被格子用到的；后面的槽位整体前移一格。

// 删除实例后清掉失效的受控绑定。

void SyncTilesetBuffers() {
  g_tileset_name_buf[0] = '\0';
  if (const pgmmv::TilesetDoc* t = CurrentTilesetC()) {
    std::snprintf(g_tileset_name_buf, sizeof(g_tileset_name_buf), "%s", t->name.c_str());
  }
}
void SyncSceneBuffers() {
  g_scene_name_buf[0] = '\0';
  if (const pgmmv::SceneDoc* s = CurrentSceneC()) {
    std::snprintf(g_scene_name_buf, sizeof(g_scene_name_buf), "%s", s->name.c_str());
  }
}

void SelectTileset(const std::string& id) {
  g_selected_tileset = id;
  g_selected_tile = 0;
  g_tile_sel_start = -1;
  g_tile_sel_end = -1;
  SyncTilesetBuffers();
}
void SelectScene(const std::string& id) {
  g_selected_scene = id;
  g_paint_slot = 0;
  g_scene_layer = 2;  // 默认主层，不能默认画到 HUD(BG0)
  g_active_tile_layer = 0;
  g_paint_tile = 0;
  g_selected_placed_object = -1;
  g_scene_origin_view_requested = true;
  g_scene_stroke_active = false;
  g_drag_placed_object = -1;
  g_drag_started = false;
  g_selected_object_for_place.clear();
  if (!g_project.objects.empty()) g_selected_object_for_place = g_project.objects.front().id;
  SyncSceneBuffers();
}

bool ObjectUsedInScenes(const std::string& id, std::string* where) {
  for (const pgmmv::SceneDoc& s : g_project.scenes) {
    for (const pgmmv::SceneObject& o : s.objects) {
      if (o.object == id) {
        if (where != nullptr) *where = s.id;
        return true;
      }
    }
  }
  return false;
}

void DeleteObject(const std::string& id) {
  std::string where;
  if (ObjectUsedInScenes(id, &where)) {
    SetError("对象仍被场景使用：" + where);
    return;
  }
  PushUndo();
  for (size_t i = 0; i < g_project.objects.size(); ++i) {
    if (g_project.objects[i].id == id) {
      const std::string anim_id = g_project.objects[i].animation;
      g_project.objects.erase(g_project.objects.begin() + i);
      // 顺手清掉没人用的动画资源（被别的对象引用就留着）
      if (!anim_id.empty()) {
        bool still_used = false;
        for (const pgmmv::ObjectDoc& o : g_project.objects) {
          if (o.animation == anim_id) still_used = true;
        }
        if (!still_used) {
          for (size_t k = 0; k < g_project.animations.size(); ++k) {
            if (g_project.animations[k].id == anim_id) {
              g_project.animations.erase(g_project.animations.begin() + static_cast<long>(k));
              break;
            }
          }
        }
      }
      MarkDirty();
      if (g_selected_object_for_place == id) {
        g_selected_object_for_place.clear();
        if (!g_project.objects.empty()) g_selected_object_for_place = g_project.objects.front().id;
      }
      if (g_selected_object == id) {
        g_selected_object.clear();
        if (!g_project.objects.empty()) {
          SelectObject(g_project.objects.front().id);
        } else {
          SyncObjectBuffers();
        }
      }
      SetStatus("已删除对象：" + id + "（未保存）");
      return;
    }
  }
}

bool TilesetUsedInScenes(const std::string& id, std::string* where) {
  for (const pgmmv::SceneDoc& s : g_project.scenes) {
    for (const std::string& t : s.tilesets) {
      if (t == id) {
        if (where != nullptr) *where = s.id;
        return true;
      }
    }
  }
  return false;
}

void DeleteTileset(const std::string& id) {
  std::string where;
  if (TilesetUsedInScenes(id, &where)) {
    SetError("图块组仍被场景使用：" + where);
    return;
  }
  PushUndo();
  for (size_t i = 0; i < g_project.tilesets.size(); ++i) {
    if (g_project.tilesets[i].id == id) {
      g_project.tilesets.erase(g_project.tilesets.begin() + i);
      MarkDirty();
      if (g_selected_tileset == id) {
        g_selected_tileset.clear();
        if (!g_project.tilesets.empty()) {
          SelectTileset(g_project.tilesets.front().id);
        } else {
          SyncTilesetBuffers();
        }
      }
      SetStatus("已删除图块组：" + id + "（未保存）");
      return;
    }
  }
}







bool TilesetImageSize(const pgmmv::TilesetDoc& ts, int* w, int* h) {
  const pgmmv::Asset* img = g_project.FindAssetByPath(ts.image);
  if (img == nullptr || img->width <= 0 || img->height <= 0) return false;
  *w = img->width;
  *h = img->height;
  return true;
}

// ★ 只重算列数，不改 tile 尺寸 ★
//   以前换图时会把尺寸强制成 8×8：16×16 的图块组一换图就被打回 8，
//   页面上的「宽/高」看着能改，实际换张图就没了。
int RecomputeTilesetColumns(pgmmv::TilesetDoc* ts) {
  if (ts == nullptr) return 0;
  int iw = 0;
  int ih = 0;
  if (!TilesetImageSize(*ts, &iw, &ih) || iw < 8 || ih < 8) return ts->columns;
  const int tw = std::max(1, ts->tile_w);
  ts->columns = std::max(1, iw / tw);
  return ts->columns;
}
bool NormalizeTilesetDims(pgmmv::TilesetDoc* ts) {
  if (ts == nullptr) return false;
  if (ts->tile_w < 1) ts->tile_w = 8;
  if (ts->tile_h < 1) ts->tile_h = 8;
  const int old_c = ts->columns;
  ts->offset_x = 0;
  ts->offset_y = 0;
  RecomputeTilesetColumns(ts);
  return old_c != ts->columns;
}
// ---- 预览 ------------------------------------------------------------------
void PreviewRelease() {
  if (g_preview.srv != nullptr) {
    g_preview.srv->Release();
    g_preview.srv = nullptr;
  }
  if (g_preview.tex != nullptr) {
    g_preview.tex->Release();
    g_preview.tex = nullptr;
  }
  g_preview.width = 0;
  g_preview.height = 0;
  g_preview.asset_id.clear();
  g_preview.failed = false;
}

bool CreatePreviewTexture(const std::vector<unsigned char>& rgba, int w, int h,
                         PreviewTexture* out) {
  if (g_d3d.device == nullptr || w <= 0 || h <= 0 ||
      rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4u) {
    return false;
  }

  // ★ 生成 mip 链（CPU 盒式下采样）★
  //   没有 mip 时，509x340 的图缩到 18x18 只能"抽一个像素"，
  //   缩略图会变成一块随机色（看起来就像素材变红）。有了 mip，缩小走平均。
  struct Level {
    const unsigned char* data;
    UINT pitch;
  };
  std::vector<Level> levels;
  levels.push_back(Level{rgba.data(), static_cast<UINT>(w * 4)});
  std::vector<std::vector<unsigned char>> owned;
  int cw = w, ch = h;
  while (cw > 1 || ch > 1) {
    const int nw = std::max(1, cw / 2);
    const int nh = std::max(1, ch / 2);
    std::vector<unsigned char> dst(static_cast<size_t>(nw) * nh * 4u);
    const unsigned char* src = levels.back().data;
    const UINT spitch = levels.back().pitch;
    for (int y = 0; y < nh; ++y) {
      for (int x = 0; x < nw; ++x) {
        const int x0 = std::min(cw - 1, x * 2);
        const int y0 = std::min(ch - 1, y * 2);
        const int x1 = std::min(cw - 1, x * 2 + 1);
        const int y1 = std::min(ch - 1, y * 2 + 1);
        // ★ 按 alpha 加权平均（预乘）★
        //   这张素材的"透明"像素 RGB 是 (155,93,96)（调色板颜色），不是黑。
        //   直接四通道各自平均，透明像素的暗红会把颜色染红 —— 缩略图就成了"偏红"。
        int sum_a = 0;
        int sum_rgb[3] = {0, 0, 0};
        const int off[4] = {y0 * static_cast<int>(spitch) + x0 * 4,
                            y0 * static_cast<int>(spitch) + x1 * 4,
                            y1 * static_cast<int>(spitch) + x0 * 4,
                            y1 * static_cast<int>(spitch) + x1 * 4};
        for (int k = 0; k < 4; ++k) {
          const unsigned char a = src[off[k] + 3];
          sum_a += a;
          for (int c = 0; c < 3; ++c) sum_rgb[c] += src[off[k] + c] * a;
        }
        unsigned char* o = dst.data() + (static_cast<size_t>(y) * nw + x) * 4;
        for (int c = 0; c < 3; ++c) {
          o[c] = sum_a > 0 ? static_cast<unsigned char>(sum_rgb[c] / sum_a) : 0;
        }
        o[3] = static_cast<unsigned char>(sum_a / 4);
      }
    }
    owned.push_back(std::move(dst));
    levels.push_back(Level{owned.back().data(), static_cast<UINT>(nw * 4)});
    cw = nw;
    ch = nh;
  }

  D3D11_TEXTURE2D_DESC td;
  std::memset(&td, 0, sizeof(td));
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = static_cast<UINT>(levels.size());
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  std::vector<D3D11_SUBRESOURCE_DATA> init(levels.size());
  for (size_t i = 0; i < levels.size(); ++i) {
    std::memset(&init[i], 0, sizeof(init[i]));
    init[i].pSysMem = levels[i].data;
    init[i].SysMemPitch = levels[i].pitch;
  }

  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g_d3d.device->CreateTexture2D(&td, init.data(), &tex)) || tex == nullptr) {
    return false;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC sd;
  std::memset(&sd, 0, sizeof(sd));
  sd.Format = td.Format;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = static_cast<UINT>(-1);

  ID3D11ShaderResourceView* srv = nullptr;
  if (FAILED(g_d3d.device->CreateShaderResourceView(tex, &sd, &srv)) || srv == nullptr) {
    tex->Release();
    return false;
  }
  if (out != nullptr) {
    out->tex = tex;
    out->srv = srv;
    out->width = w;
    out->height = h;
    out->failed = false;
  }
  return true;
}

void ReleaseTilesetPreviews() {
  for (auto& entry : g_tileset_previews) {
    PreviewTexture& p = entry.second;
    if (p.srv != nullptr) p.srv->Release();
    if (p.tex != nullptr) p.tex->Release();
  }
  g_tileset_previews.clear();
}


void LoadPreview(const pgmmv::Asset& a) {
  PreviewRelease();
  g_preview.asset_id = a.id;
  std::vector<unsigned char> rgba;
  int w = 0;
  int h = 0;
  std::string err;
  if (!pgmmv::LoadPngRgba(pgmmv::ProjectAssetPath(g_project, a), &rgba, &w, &h, &err)) {
    g_preview.failed = true;
    SetError("预览失败：" + err);
    return;
  }
  if (!CreatePreviewTexture(rgba, w, h, &g_preview)) {
    g_preview.failed = true;
    SetError("预览纹理创建失败（D3D11）");
  }
}

// ---- 项目动作 --------------------------------------------------------------
void OnProjectLoaded() {
  g_has_project = true;
  g_dirty = false;
  g_selected_asset.clear();
  if (!g_project.assets.empty()) {
    g_selected_asset = g_project.assets.front().id;
  }
  g_selected_object.clear();
    g_anim_frame = 0;
  g_anim_playing = false;
  g_anim_accum = 0.0;
  if (!g_project.objects.empty()) {
    g_selected_object = g_project.objects.front().id;

    const std::vector<pgmmv::ActionDef>* acts =
        pgmmv::ObjectActions(g_project, g_project.objects.front());
    g_selected_action = (acts == nullptr || acts->empty()) ? "idle"
                                                          : acts->front().name;
    g_selected_animation = g_project.objects.front().animation;
  }
  g_selected_tileset = g_project.tilesets.empty() ? "" : g_project.tilesets.front().id;
  g_paint_slot = 0;
  g_selected_scene = g_project.scenes.empty() ? "" : g_project.scenes.front().id;
  g_selected_transition = g_project.transitions.empty() ? -1 : 0;
  g_selected_tile = 0;
  g_paint_tile = 0;
  g_scene_layer = 2;  // 默认主层，不能默认画到 HUD(BG0)
  g_active_tile_layer = 0;
  g_selected_placed_object = -1;
  g_scene_origin_view_requested = true;
  g_scene_stroke_active = false;
  g_drag_placed_object = -1;
  g_drag_started = false;
  g_selected_object_for_place.clear();
  if (!g_project.objects.empty()) g_selected_object_for_place = g_project.objects.front().id;
  PreviewRelease();
  ReleaseTilesetPreviews();
  SyncProjectBuffers();
  SyncAssetBuffer();
  SyncObjectBuffers();
  SyncTilesetBuffers();
  SyncSceneBuffers();
  g_undo_stack.clear();
  g_redo_stack.clear();
  if (!g_project.settings.initial_scene.empty() &&
      g_project.FindScene(g_project.settings.initial_scene) == nullptr) {
    SetStatus("已打开：" + PathText(g_project.root_dir) +
              "；注意：初始场景不存在，请重新选择");
  } else {
    SetStatus("已打开：" + PathText(g_project.root_dir));
  }
}

void AfterHistoryRestore() {
  if (!g_has_project) return;
  if (!g_selected_object.empty() && g_project.FindObject(g_selected_object) == nullptr) {
    g_selected_object.clear();
  }
  if (g_selected_object.empty() && !g_project.objects.empty()) {
    g_selected_object = g_project.objects.front().id;
  }
  if (!g_selected_tileset.empty() && g_project.FindTileset(g_selected_tileset) == nullptr) {
    g_selected_tileset.clear();
  }
  if (g_selected_tileset.empty() && !g_project.tilesets.empty()) {
    g_selected_tileset = g_project.tilesets.front().id;
  }
  if (!g_selected_scene.empty() && g_project.FindScene(g_selected_scene) == nullptr) {
    g_selected_scene.clear();
  }
  if (g_selected_scene.empty() && !g_project.scenes.empty()) {
    g_selected_scene = g_project.scenes.front().id;
  }
  if (!g_selected_asset.empty() && g_project.FindAsset(g_selected_asset) == nullptr) {
    g_selected_asset.clear();
  }
  if (g_selected_transition >= static_cast<int>(g_project.transitions.size())) {
    g_selected_transition = g_project.transitions.empty() ? -1 : 0;
  }
  if (g_selected_action.empty()) g_selected_action = "idle";
  g_selected_placed_object = -1;
  g_drag_placed_object = -1;
  g_scene_stroke_active = false;
  g_selected_object_for_place.clear();
  if (!g_project.objects.empty()) g_selected_object_for_place = g_project.objects.front().id;
  g_action_frames_key.clear();
  PreviewRelease();
  ReleaseTilesetPreviews();
  SyncProjectBuffers();
  SyncAssetBuffer();
  SyncObjectBuffers();
  SyncTilesetBuffers();
  SyncSceneBuffers();
  g_dirty = true;
}

void DoUndo() {
  if (g_undo_stack.empty()) {
    SetStatus("没有可撤销的操作");
    return;
  }
  g_redo_stack.push_back(g_project);
  g_project = std::move(g_undo_stack.back());
  g_undo_stack.pop_back();
  AfterHistoryRestore();
  SetStatus("已撤销（未保存）");
}

void DoRedo() {
  if (g_redo_stack.empty()) {
    SetStatus("没有可重做的操作");
    return;
  }
  g_undo_stack.push_back(g_project);
  g_project = std::move(g_redo_stack.back());
  g_redo_stack.pop_back();
  AfterHistoryRestore();
  SetStatus("已重做（未保存）");
}

bool NewProjectAt(const std::filesystem::path& dir, std::string* error) {
  std::string name = dir.filename().u8string();
  if (name.empty()) {
    name = "Untitled";
  }
  pgmmv::Project p;
  std::string err;
  if (!pgmmv::ProjectCreate(dir, name, &p, &err)) {
    if (error != nullptr) *error = err;
    return false;
  }
  g_project = std::move(p);
  OnProjectLoaded();
  SetStatus("已新建：" + PathText(g_project.root_dir) + "（未保存）");
  g_dirty = true;
  return true;
}

bool OpenProjectAt(const std::filesystem::path& project_json, std::string* error) {
  pgmmv::Project p;
  std::string err;
  if (!pgmmv::ProjectLoad(project_json, &p, &err)) {
    if (error != nullptr) *error = err;
    return false;
  }
  g_project = std::move(p);
  OnProjectLoaded();
  return true;
}

bool SaveCurrent(std::string* error) {
  if (!g_has_project) {
    if (error != nullptr) *error = "没有打开项目";
    return false;
  }
  // R3 之后：初始场景必须指向一个存在的场景；R1/R2 项目留空。
  if (!g_project.settings.initial_scene.empty() &&
      g_project.FindScene(g_project.settings.initial_scene) == nullptr) {
    if (error != nullptr) {
      *error = "初始场景不存在：" + g_project.settings.initial_scene;
    }
    return false;
  }
  std::string err;
  if (!pgmmv::ProjectSave(&g_project, &err)) {
    if (error != nullptr) *error = err;
    return false;
  }
  g_dirty = false;
  SetStatus("已保存：" + PathText(pgmmv::ProjectFilePath(g_project)));
  return true;
}

bool ImportPngAt(const std::filesystem::path& png, std::string* error) {
  if (!g_has_project) {
    if (error != nullptr) *error = "没有打开项目";
    return false;
  }
  std::string id;
  std::string err;
  if (!pgmmv::ProjectImportPng(&g_project, png, &id, &err)) {
    if (error != nullptr) *error = err;
    return false;
  }
  // 记住源文件绝对路径，"更新"时从它重读
  if (pgmmv::Asset* imported = g_project.FindAsset(id)) {
    std::error_code ec_abs;
    const auto abs_src = std::filesystem::absolute(png, ec_abs);
    imported->source = (ec_abs ? png : abs_src).u8string();
  }
  g_selected_asset = id;
  g_dirty = true;
  ReleaseTilesetPreviews();
  PreviewRelease();
  SyncAssetBuffer();
  SetStatus("已导入：" + id + "（未保存）");
  return true;
}

// ---- 原生对话框 ------------------------------------------------------------
// ★ 文件夹选择：用现代 IFileOpenDialog + FOS_PICKFOLDERS ★
//   旧写法 SHBrowseForFolder + BIF_NEWDIALOGSTYLE 依赖 shell 的 COM 对象；
//   线程上没有 CoInitialize 时它会弹出一个空白/无响应的对话框 ——
//   「新建项目卡住」就是这么来的。main() 里已经补了 CoInitializeEx。
bool PickFolder(std::filesystem::path* out) {
  IFileOpenDialog* dlg = nullptr;
  const HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dlg));
  if (FAILED(hr) || dlg == nullptr) {
    return false;
  }
  DWORD opts = 0;
  if (SUCCEEDED(dlg->GetOptions(&opts))) {
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                    FOS_NOCHANGEDIR);
  }
  dlg->SetTitle(L"选择新项目文件夹（可在对话框里新建文件夹）");
  if (FAILED(dlg->Show(g_hwnd))) {
    dlg->Release();  // 用户取消
    return false;
  }
  IShellItem* item = nullptr;
  if (FAILED(dlg->GetResult(&item)) || item == nullptr) {
    dlg->Release();
    return false;
  }
  PWSTR path = nullptr;
  bool ok = SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr;
  if (ok) {
    *out = std::filesystem::path(path);
    CoTaskMemFree(path);
  }
  item->Release();
  dlg->Release();
  return ok;
}

bool PickOpenProject(std::filesystem::path* out) {
  wchar_t file[1024] = {};
  OPENFILENAMEW ofn;
  std::memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFilter = L"项目 (project.json)\0project.json\0所有文件 (*.*)\0*.*\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = 1024;
  ofn.lpstrTitle = L"打开项目";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn) != TRUE) {
    return false;
  }
  *out = std::filesystem::path(file);
  return true;
}

bool PickPngMany(std::vector<std::filesystem::path>* out) {
  std::vector<wchar_t> buffer(8192, L'\0');
  OPENFILENAMEW ofn;
  std::memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFilter = L"PNG 图像 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0";
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = static_cast<DWORD>(buffer.size());
  ofn.lpstrTitle = L"导入图片（可多选）";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_EXPLORER | OFN_ALLOWMULTISELECT;
  if (GetOpenFileNameW(&ofn) != TRUE) return false;
  const wchar_t* cursor = buffer.data();
  const std::wstring first(cursor);
  cursor += first.size() + 1;
  std::vector<std::wstring> names;
  while (*cursor != L'\0') {
    const std::wstring name(cursor);
    names.push_back(name);
    cursor += name.size() + 1;
  }
  out->clear();
  if (names.empty()) {
    out->emplace_back(first);
  } else {
    const std::filesystem::path dir(first);
    for (const std::wstring& name : names) out->push_back(dir / std::filesystem::path(name));
  }
  return true;
}


bool PickObjectJsonOpen(std::filesystem::path* out) {
  wchar_t file[1024] = {};
  OPENFILENAMEW ofn;
  std::memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFilter = L"对象模板 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = 1024;
  ofn.lpstrTitle = L"导入对象模板";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (GetOpenFileNameW(&ofn) != TRUE) return false;
  *out = std::filesystem::path(file);
  return true;
}

bool PickObjectJsonOpenMany(std::vector<std::filesystem::path>* out) {
  std::vector<wchar_t> buffer(8192, L'\0');
  OPENFILENAMEW ofn;
  std::memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFilter = L"对象模板 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0";
  ofn.lpstrFile = buffer.data();
  ofn.nMaxFile = static_cast<DWORD>(buffer.size());
  ofn.lpstrTitle = L"批量导入对象模板";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_EXPLORER | OFN_ALLOWMULTISELECT;
  if (GetOpenFileNameW(&ofn) != TRUE) return false;
  const wchar_t* cursor = buffer.data();
  const std::wstring first(cursor);
  cursor += first.size() + 1;
  std::vector<std::wstring> names;
  while (*cursor != L'\0') {
    const std::wstring name(cursor);
    names.push_back(name);
    cursor += name.size() + 1;
  }
  out->clear();
  if (names.empty()) {
    out->emplace_back(first);
  } else {
    const std::filesystem::path dir(first);
    for (const std::wstring& name : names) {
      out->push_back(dir / std::filesystem::path(name));
    }
  }
  return true;
}

bool PickObjectJsonSave(std::filesystem::path* out, const std::string& default_name) {
  wchar_t file[1024] = {};
  std::wstring name(default_name.begin(), default_name.end());
  name += L".json";
  std::wcsncpy(file, name.c_str(), 1023);
  OPENFILENAMEW ofn;
  std::memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g_hwnd;
  ofn.lpstrFilter = L"对象模板 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = 1024;
  ofn.lpstrTitle = L"导出对象模板";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  ofn.lpstrDefExt = L"json";
  if (GetSaveFileNameW(&ofn) != TRUE) return false;
  *out = std::filesystem::path(file);
  return true;
}

// ---- 动作入口 --------------------------------------------------------------
void DoNewProjectDialog() {
  std::filesystem::path dir;
  if (!PickFolder(&dir)) {
    return;
  }
  std::string err;
  if (!NewProjectAt(dir, &err)) {
    SetError("新建项目失败：" + err);
  }
}

void DoOpenProjectDialog() {
  std::filesystem::path file;
  if (!PickOpenProject(&file)) {
    return;
  }
  std::string err;
  if (!OpenProjectAt(file, &err)) {
    SetError("打开项目失败：" + err);
  }
}

void DoSave() {
  std::string err;
  if (!SaveCurrent(&err)) {
    SetError("保存失败：" + err);
  }
}

bool ExportCurrentP3d(std::filesystem::path* out_path, std::string* error) {
  if (!g_has_project) {
    if (error != nullptr) *error = "先新建或打开项目";
    return false;
  }
  if (!SaveCurrent(error)) return false;
  std::filesystem::path out = g_project.root_dir / "build" / "game.p3d";
  std::error_code ec;
  std::filesystem::create_directories(out.parent_path(), ec);
  std::string eerr;
  if (!pgmmv::ExportP3d(g_project, out, &eerr)) {
    if (error != nullptr) *error = eerr;
    return false;
  }
  if (out_path != nullptr) *out_path = out;
  return true;
}

void DoExportP3d() {
  std::filesystem::path out;
  std::string err;
  if (!ExportCurrentP3d(&out, &err)) {
    SetError("导出失败：" + err);
    return;
  }
  g_cli_r4_p3d = out;
  SetStatus("已导出：" + PathText(out));
}

bool PlayerStillRunning() {
  if (g_player_process == nullptr) return false;
  DWORD code = 0;
  if (GetExitCodeProcess(g_player_process, &code) && code == STILL_ACTIVE) {
    return true;
  }
  CloseHandle(g_player_process);
  g_player_process = nullptr;
  return false;
}

void DoPlay() {
  if (PlayerStillRunning()) {
    SetStatus("播放器已在运行，先关掉旧窗口");
    return;
  }
  std::filesystem::path p3d;
  std::string err;
  if (!ExportCurrentP3d(&p3d, &err)) {
    SetError("试玩失败：" + err);
    return;
  }
  wchar_t exe_buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
  if (n == 0) {
    SetError("试玩失败：找不到编辑器路径");
    return;
  }
  const std::filesystem::path player =
      std::filesystem::path(exe_buf).parent_path() / "PocketLoom-Play.exe";
  if (!std::filesystem::exists(player)) {
    SetError("试玩失败：缺少 " + PathText(player));
    return;
  }
  std::wstring cmd = L"\"" + player.wstring() + L"\" --p3d \"" +
                     p3d.wstring() + L"\"";
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  std::wstring cwd = g_project.root_dir.wstring();
  if (!CreateProcessW(player.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
    SetError("试玩失败：启动 Player 失败");
    return;
  }
  CloseHandle(pi.hThread);
  g_player_process = pi.hProcess;
  SetStatus("试玩：" + PathText(p3d));
}

void UiLog(const char* msg) {
  std::FILE* f = std::fopen("editor-ui.log", "a");
  if (f != nullptr) {
    std::fprintf(f, "%s\n", msg);
    std::fclose(f);
  }
}

void UiLogF(const char* fmt, ...) {
  std::FILE* f = std::fopen("editor-ui.log", "a");
  if (f == nullptr) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fprintf(f, "\n");
  std::fclose(f);
}

// 记录控件矩形，供模拟鼠标定位；同一 id 只在矩形变化时写一次。
void LogItemRect(const char* id) {
  const ImVec2 a = ImGui::GetItemRectMin();
  const ImVec2 b = ImGui::GetItemRectMax();
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%.0f %.0f %.0f %.0f", a.x, a.y, b.x, b.y);
  static std::map<std::string, std::string> last;
  auto it = last.find(id);
  if (it != last.end() && it->second == buf) return;
  last[id] = buf;
  UiLogF("rect %s %s", id, buf);
}


void DoImportPngsBulk() {
  if (!g_has_project) {
    SetError("先新建或打开项目");
    return;
  }
  std::vector<std::filesystem::path> files;
  if (!PickPngMany(&files) || files.empty()) return;
  PushUndo();
  int imported = 0;
  std::string last_error;
  for (const std::filesystem::path& file : files) {
    if (ImportPngAt(file, &last_error)) {
      ++imported;
    }
  }
  if (imported == 0) {
    SetError("批量导入失败：" + last_error);
    return;
  }
  SetStatus("已批量导入 " + std::to_string(imported) + " 张 PNG（未保存）");
}

void ExecutePending() {
  const PendingAction p = g_pending;
  g_pending = PendingAction::kNone;
  switch (p) {
    case PendingAction::kNew:
      DoNewProjectDialog();
      break;
    case PendingAction::kOpen:
      DoOpenProjectDialog();
      break;
    case PendingAction::kExit:
      g_quit = true;
      break;
    case PendingAction::kNone:
      break;
  }
}

void RequestAction(PendingAction p) {
  if (p == PendingAction::kNone) {
    return;
  }
  g_pending = p;
  if (g_dirty) {
    g_show_confirm = true;
    return;
  }
  ExecutePending();
}

// ---- D3D11 ----------------------------------------------------------------
bool CreateDeviceD3D(HWND hwnd, UINT w, UINT h) {
  DXGI_SWAP_CHAIN_DESC sd;
  std::memset(&sd, 0, sizeof(sd));
  sd.BufferCount = 2;
  sd.BufferDesc.Width = w;
  sd.BufferDesc.Height = h;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  sd.Flags = 0;

  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd,
      &g_d3d.swap, &g_d3d.device, &got, &g_d3d.ctx);
  if (FAILED(hr)) {
    hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, want, 2, D3D11_SDK_VERSION, &sd,
        &g_d3d.swap, &g_d3d.device, &got, &g_d3d.ctx);
  }
  if (FAILED(hr) || g_d3d.device == nullptr || g_d3d.swap == nullptr) {
    std::fprintf(stderr, "[editor] D3D11CreateDeviceAndSwapChain failed hr=0x%08lX\n",
                 static_cast<unsigned long>(hr));
    return false;
  }
  g_d3d.width = w;
  g_d3d.height = h;

  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_d3d.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr) {
    std::fprintf(stderr, "[editor] swapchain GetBuffer failed\n");
    return false;
  }
  const HRESULT hr2 = g_d3d.device->CreateRenderTargetView(back, nullptr, &g_d3d.rtv);
  back->Release();
  return SUCCEEDED(hr2);
}

void CleanupRenderTarget() {
  if (g_d3d.rtv != nullptr) {
    g_d3d.rtv->Release();
    g_d3d.rtv = nullptr;
  }
}

bool CreateRenderTarget() {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_d3d.swap->GetBuffer(0, IID_PPV_ARGS(&back))) || back == nullptr) {
    return false;
  }
  const HRESULT hr = g_d3d.device->CreateRenderTargetView(back, nullptr, &g_d3d.rtv);
  back->Release();
  return SUCCEEDED(hr);
}

void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_d3d.swap != nullptr) { g_d3d.swap->Release(); g_d3d.swap = nullptr; }
  if (g_d3d.ctx != nullptr) { g_d3d.ctx->Release(); g_d3d.ctx = nullptr; }
  if (g_d3d.device != nullptr) { g_d3d.device->Release(); g_d3d.device = nullptr; }
}

void ResizeSwapChain(UINT w, UINT h) {
  if (g_d3d.device == nullptr || w == 0 || h == 0) {
    return;
  }
  CleanupRenderTarget();
  g_d3d.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
  g_d3d.width = w;
  g_d3d.height = h;
  CreateRenderTarget();
}

// ---- Win32 ----------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui::GetCurrentContext() != nullptr) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) {
      return 1;
    }
  }
  switch (msg) {
    case WM_SIZE:
      if (g_d3d.device != nullptr && wp != SIZE_MINIMIZED) {
        ResizeSwapChain(static_cast<UINT>(LOWORD(lp)), static_cast<UINT>(HIWORD(lp)));
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_KEYMENU) {
        return 0;
      }
      break;
    case WM_CLOSE:
      g_request_exit = true;
      return 0;  // 不直接销毁窗口：主循环会走“未保存”确认
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- 字体 ------------------------------------------------------------------
bool LoadUiFont() {
  ImGuiIO& io = ImGui::GetIO();
  const char* candidates[] = {
      "C:\\Windows\\Fonts\\msyh.ttc",
      "C:\\Windows\\Fonts\\simhei.ttf",
      "C:\\Windows\\Fonts\\simsun.ttc",
  };
  for (const char* path : candidates) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) continue;
    std::fclose(f);
    ImFontConfig cfg;
    cfg.FontNo = 0;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    if (io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg,
                                     io.Fonts->GetGlyphRangesChineseSimplifiedCommon()) !=
        nullptr) {
      std::printf("[editor] UI font: %s\n", path);
      g_font_cjk = true;
      return true;
    }
  }
  io.Fonts->AddFontDefault();
  std::fprintf(stderr, "[editor] no CJK font found; using ImGui default font\n");
  return false;
}

// ---- UI --------------------------------------------------------------------
const char* kTools[] = {"场景", "图块", "动画", "对象", "资源", "界面"};
const char* ToolName(int i) {
  return (i >= 0 && i < static_cast<int>(sizeof(kTools) / sizeof(kTools[0])))
             ? kTools[i]
             : "?";
}
int g_current_tool = 0;

bool g_show_project_settings = false;

void DrawProjectSettingsModal() {
  if (!g_show_project_settings) return;
  ImGui::OpenPopup("项目设置##modal");
  g_show_project_settings = false;
  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal("项目设置##modal", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("项目：%s", g_project.name.c_str());
    ImGui::Separator();
    const char* cur_init = g_project.settings.initial_scene.empty()
                               ? "(第一个场景)"
                               : g_project.settings.initial_scene.c_str();
    ImGui::TextUnformatted("初始场景");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##initial_scene", cur_init)) {
      if (ImGui::Selectable("(第一个场景)", g_project.settings.initial_scene.empty())) {
        PushUndo();
        g_project.settings.initial_scene.clear();
        MarkDirty();
      }
      for (const pgmmv::SceneDoc& sc : g_project.scenes) {
        if (ImGui::Selectable(sc.name.c_str(),
                              g_project.settings.initial_scene == sc.id)) {
          PushUndo();
          g_project.settings.initial_scene = sc.id;
          MarkDirty();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::Separator();
    ImGui::TextDisabled("屏幕：%d×%d", pgmmv::kScreenWidth, pgmmv::kScreenHeight);
    ImGui::Separator();
    if (DialogButton("关闭")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void DrawConfirmModal() {
  if (!g_show_confirm) return;
  if (!ImGui::IsPopupOpen("未保存##confirm")) {
    ImGui::OpenPopup("未保存##confirm");
  }
  ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Appearing);
  bool keep_open = true;
  if (ImGui::BeginPopupModal("未保存##confirm", &keep_open,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    const char* what = g_pending == PendingAction::kExit ? "退出"
                       : g_pending == PendingAction::kNew ? "新建项目"
                                                          : "打开项目";
    ImGui::TextUnformatted("当前项目有未保存修改。");
    ImGui::TextDisabled("%s前要先保存吗？", what);
    ImGui::Separator();
    if (DialogButton("保存")) {
      std::string err;
      if (SaveCurrent(&err)) {
        g_show_confirm = false;
        ImGui::CloseCurrentPopup();
        ExecutePending();
      } else {
        SetError("保存失败：" + err);
      }
    }
    ImGui::SameLine();
    if (DialogButton("不保存")) {
      g_show_confirm = false;
      ImGui::CloseCurrentPopup();
      ExecutePending();
    }
    ImGui::SameLine();
    if (DialogButton("取消")) {
      g_show_confirm = false;
      g_pending = PendingAction::kNone;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (!keep_open) {
    g_show_confirm = false;
    g_pending = PendingAction::kNone;
  }
}

void DrawRenameObjectModal() {
  if (!g_show_rename_object) return;
  if (!ImGui::IsPopupOpen("重命名对象##rename")) {
    ImGui::OpenPopup("重命名对象##rename");
  }
  if (ImGui::BeginPopupModal("重命名对象##rename", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    pgmmv::ObjectDoc* obj = CurrentObject();
    if (obj == nullptr) {
      g_show_rename_object = false;
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      return;
    }
    ImGui::TextUnformatted("对象名称");
    ImGui::SetNextItemWidth(320.0f);
    const bool enter = ImGui::InputText("##rename_object", g_rename_object_buf,
                                        sizeof(g_rename_object_buf),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    if (enter || DialogButton("确定")) {
      obj->name = g_rename_object_buf;
      g_show_rename_object = false;
      MarkDirty();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (DialogButton("取消")) {
      g_show_rename_object = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void DrawMenuBar() {
  if (!ImGui::BeginMenuBar()) {
    return;
  }
  if (ImGui::BeginMenu("文件")) {
    if (ImGui::MenuItem("新建项目...")) RequestAction(PendingAction::kNew);
    if (ImGui::MenuItem("打开项目...")) RequestAction(PendingAction::kOpen);
    if (ImGui::MenuItem("保存", "Ctrl+S", false, g_has_project)) DoSave();
    if (ImGui::MenuItem("导出 game.p3d", nullptr, false, g_has_project)) DoExportP3d();
    ImGui::Separator();
    if (ImGui::MenuItem("退出")) RequestAction(PendingAction::kExit);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("编辑")) {
    if (ImGui::MenuItem("撤销", "Ctrl+Z", false, !g_undo_stack.empty())) DoUndo();
    if (ImGui::MenuItem("重做", "Ctrl+Y", false, !g_redo_stack.empty())) DoRedo();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("项目")) {
    if (ImGui::MenuItem("项目设置...", nullptr, false, g_has_project)) {
      g_show_project_settings = true;
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("运行")) {
    if (ImGui::MenuItem("试玩", "F5", false, g_has_project)) DoPlay();
    if (ImGui::MenuItem("导出 game.p3d", nullptr, false, g_has_project)) DoExportP3d();
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("帮助")) {
    ImGui::TextDisabled("%s  %s", kProduct, eng::version_string());
    ImGui::EndMenu();
  }

  const char* name = g_has_project ? g_project.name.c_str() : "未打开项目";
  char right[256];
  std::snprintf(right, sizeof(right), "%s%s", name, g_dirty ? " *" : "");
  const float tw = ImGui::CalcTextSize(right).x;
  ImGui::SameLine(ImGui::GetWindowWidth() - tw - 12.0f);
  ImGui::TextDisabled("%s", right);
  ImGui::EndMenuBar();
}

pgmmv::AnimationDoc* EnsureAnimation();
pgmmv::ActionDef* CurrentAnimAction();

// 预览只看动画资源自己（图集 + 帧尺寸），和对象无关
void DrawCheckerboard(ImDrawList* dl, ImVec2 p, ImVec2 size) {
  const float cell = 8.0f;
  const ImU32 c1 = IM_COL32(70, 70, 76, 255);
  const ImU32 c2 = IM_COL32(96, 96, 104, 255);
  const int nx = static_cast<int>((size.x + cell - 1) / cell);
  const int ny = static_cast<int>((size.y + cell - 1) / cell);
  for (int y = 0; y < ny; ++y) {
    for (int x = 0; x < nx; ++x) {
      const ImVec2 a(p.x + x * cell, p.y + y * cell);
      const ImVec2 b(std::min(p.x + (x + 1) * cell, p.x + size.x),
                     std::min(p.y + (y + 1) * cell, p.y + size.y));
      dl->AddRectFilled(a, b, ((x + y) & 1) ? c2 : c1);
    }
  }
}

void DrawObjectPreview(const std::string& image, int frame_w, int frame_h,
                       const pgmmv::DirectionDef& dir) {
  if (image.empty()) {
    ImGui::TextDisabled("这个动画还没有图像：在右栏「图像」里选");
    return;
  }
  const pgmmv::Asset* img = g_project.FindAssetByPath(image);
  if (img == nullptr) {
    ImGui::TextDisabled("图像资源不存在：%s", image.c_str());
    return;
  }
  if (g_preview.asset_id != img->id) {
    LoadPreview(*img);
  }
  if (g_preview.srv == nullptr || g_preview.width <= 0 || g_preview.height <= 0) {
    ImGui::TextDisabled("图像预览不可用");
    return;
  }
  const int fw = frame_w;
  const int fh = frame_h;
  if (fw <= 0 || fh <= 0) {
    ImGui::TextDisabled("帧尺寸无效");
    return;
  }
  if (dir.frames.empty()) {
    ImGui::TextDisabled("这个动作没有帧");
    return;
  }
  if (g_anim_frame < 0) g_anim_frame = 0;
  if (g_anim_frame >= static_cast<int>(dir.frames.size())) {
    g_anim_frame = static_cast<int>(dir.frames.size()) - 1;
  }
  const int cols = std::max(1, g_preview.width / fw);
  const int frame_no = dir.frames[g_anim_frame].image;
  const int sx = (frame_no % cols) * fw;
  const int sy = (frame_no / cols) * fh;
  if (sx + fw > g_preview.width || sy + fh > g_preview.height) {
    ImGui::TextDisabled("帧 %d 超出图像（帧尺寸/图像尺寸不匹配）", frame_no);
    return;
  }

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  float scale = std::min(avail.x / static_cast<float>(fw),
                         avail.y / static_cast<float>(fh));
  if (scale > 12.0f) scale = 12.0f;
  if (scale <= 0.0f) return;
  const ImVec2 size(static_cast<float>(fw) * scale, static_cast<float>(fh) * scale);
  ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f,
                             ImGui::GetCursorPosY() + (avail.y - size.y) * 0.5f));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  DrawCheckerboard(dl, p, size);
  ImVec2 uv0(static_cast<float>(sx) / static_cast<float>(g_preview.width),
             static_cast<float>(sy) / static_cast<float>(g_preview.height));
  ImVec2 uv1(static_cast<float>(sx + fw) / static_cast<float>(g_preview.width),
             static_cast<float>(sy + fh) / static_cast<float>(g_preview.height));
  if (dir.flip_h) { const float t = uv0.x; uv0.x = uv1.x; uv1.x = t; }
  if (dir.flip_v) { const float t = uv0.y; uv0.y = uv1.y; uv1.y = t; }
  const ImVec2 off(static_cast<float>(dir.offset_x) * scale,
                   static_cast<float>(dir.offset_y) * scale);
  const ImVec2 sp(p.x + off.x, p.y + off.y);
  dl->AddImage(reinterpret_cast<ImTextureID>(g_preview.srv), sp,
               ImVec2(sp.x + size.x, sp.y + size.y), uv0, uv1);
  int ox = dir.origin_x;
  int oy = dir.origin_y;
  if (!dir.frames.empty()) {
    pgmmv::ResolveFrameOrigin(dir, g_anim_frame, &ox, &oy);
  }
  const ImVec2 anchor(p.x + static_cast<float>(ox) * scale,
                      p.y + static_cast<float>(oy) * scale);
  auto draw_box = [&](const pgmmv::CollisionBox& b, ImU32 color) {
    if (b.w <= 0 || b.h <= 0) return;
    dl->AddRect(ImVec2(anchor.x + static_cast<float>(b.x) * scale,
                       anchor.y + static_cast<float>(b.y) * scale),
                ImVec2(anchor.x + static_cast<float>(b.x + b.w) * scale,
                       anchor.y + static_cast<float>(b.y + b.h) * scale),
                color, 0.0f, 0, 2.0f);
  };
  const int fi = dir.frames.empty()
                     ? 0
                     : std::max(0, std::min(g_anim_frame,
                                            static_cast<int>(dir.frames.size()) - 1));
  // 画的是"解析后"的值：这一帧没设就沿用前面最近的设置
  const pgmmv::CollisionBox* col_box =
      pgmmv::ResolveFrameBox(dir, fi, 0, nullptr);
  const pgmmv::CollisionBox* atk_box =
      pgmmv::ResolveFrameBox(dir, fi, 1, nullptr);
  const pgmmv::CollisionBox* hurt_box =
      pgmmv::ResolveFrameBox(dir, fi, 2, nullptr);
  if (col_box != nullptr) draw_box(*col_box, IM_COL32(80, 150, 240, 255));
  if (atk_box != nullptr) draw_box(*atk_box, IM_COL32(90, 200, 110, 255));
  if (hurt_box != nullptr) draw_box(*hurt_box, IM_COL32(226, 152, 62, 255));
  // 连接点：小菱形 + 名字
  if (fi < static_cast<int>(dir.frames.size())) {
    for (const pgmmv::AttachPoint& ap : dir.frames[fi].attach) {
      const ImVec2 q(anchor.x + static_cast<float>(ap.x) * scale,
                     anchor.y + static_cast<float>(ap.y) * scale);
      dl->AddQuadFilled(ImVec2(q.x, q.y - 5.0f), ImVec2(q.x + 5.0f, q.y),
                        ImVec2(q.x, q.y + 5.0f), ImVec2(q.x - 5.0f, q.y),
                        IM_COL32(250, 220, 90, 255));
      dl->AddText(ImVec2(q.x + 7.0f, q.y - 7.0f), IM_COL32(250, 230, 160, 255),
                  ap.name.c_str());
    }
  }
  dl->AddLine(ImVec2(anchor.x - 10.0f, anchor.y),
              ImVec2(anchor.x + 10.0f, anchor.y),
              IM_COL32(226, 96, 96, 255), 1.5f);
  dl->AddLine(ImVec2(anchor.x, anchor.y - 10.0f),
              ImVec2(anchor.x, anchor.y + 10.0f),
              IM_COL32(226, 96, 96, 255), 1.5f);
  ImGui::InvisibleButton("##preview_hit", size);

  // ★ 盒子直接拖 ★
  //   碰撞（蓝）/ 受击（橙）/ 攻击（绿）都能在预览上拖：拖内部=移动，拖四角=改尺寸。
  pgmmv::ActionDef* actm = CurrentAnimAction();
  pgmmv::DirectionDef* dirm =
      actm != nullptr ? pgmmv::ActionDirectionAt(actm, g_anim_direction) : nullptr;
  if (dirm != nullptr) {
    // 拖框落点：三种盒子都写在当前帧（第一次改时从解析值拷一份）
    auto target_box = [&](int which) -> pgmmv::CollisionBox* {
      if (dirm->frames.empty()) return nullptr;
      const int fi2 = std::max(0, std::min(g_anim_frame,
                                          static_cast<int>(dirm->frames.size()) - 1));
      pgmmv::FrameBox& fb = which == 0 ? dirm->frames[fi2].collision
                            : which == 1 ? dirm->frames[fi2].hurt
                                         : dirm->frames[fi2].attack;
      if (fb.state != pgmmv::BoxState::kBox) {
        const pgmmv::CollisionBox* r =
            pgmmv::ResolveFrameBox(*dirm, fi2, which, nullptr);
        if (r != nullptr) fb.box = *r;
        fb.state = pgmmv::BoxState::kBox;
      }
      return &fb.box;
    };
    static ImVec2 drag_from_px;
    static pgmmv::CollisionBox drag_start;

    const ImVec2 mo = ImGui::GetIO().MousePos;
    const float px = (mo.x - anchor.x) / scale;
    const float py = (mo.y - anchor.y) / scale;
    // ★ 按下 = 开始拉框绘制（1 像素精度）★
    static bool band_active = false;
    static ImVec2 band_start;
    if (!band_active && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
      band_active = true;
      band_start = ImVec2(px, py);
      PushUndo();
    }
    if (band_active) {
      const ImVec2 a(anchor.x + std::min(band_start.x, px) * scale,
                     anchor.y + std::min(band_start.y, py) * scale);
      const ImVec2 b(anchor.x + std::max(band_start.x, px) * scale,
                     anchor.y + std::max(band_start.y, py) * scale);
      dl->AddRect(a, b, IM_COL32(255, 240, 140, 255), 0.0f, 0, 1.5f);
      dl->AddRectFilled(a, b, IM_COL32(255, 240, 140, 40));
      if (!ImGui::IsMouseDown(0)) {
        band_active = false;
        pgmmv::CollisionBox* dst = target_box(g_box_draw_target);
        if (dst != nullptr) {
          const int x0 = static_cast<int>(std::lround(std::min(band_start.x, px)));
          const int y0 = static_cast<int>(std::lround(std::min(band_start.y, py)));
          const int x1 = static_cast<int>(std::lround(std::max(band_start.x, px)));
          const int y1 = static_cast<int>(std::lround(std::max(band_start.y, py)));
          dst->x = x0;
          dst->y = y0;
          dst->w = std::max(1, x1 - x0);
          dst->h = std::max(1, y1 - y0);
          MarkDirty();
        }
      }
    }
  }
}

void DrawObjectTimeline(pgmmv::ObjectDoc* obj, pgmmv::DirectionDef* dir) {
  // 动画页没有 ObjectDoc，obj 可以为空；只有“对象级碰撞”那一行依赖它。
  if (dir == nullptr) return;
  EditorEnsureActionHolds(dir);
  if (ImGui::Button("|<", ImVec2(48, 24))) { g_anim_frame = 0; g_anim_playing = false; }
  ImGui::SameLine();
  if (ImGui::Button("<", ImVec2(48, 24))) {
    g_anim_frame = std::max(0, g_anim_frame - 1);
    g_anim_playing = false;
  }
  ImGui::SameLine();
  if (ImGui::Button(g_anim_playing ? "暂停" : "播放", ImVec2(56, 24))) {
    g_anim_playing = !g_anim_playing;
    g_anim_accum = 0.0;
    if (g_anim_playing && dir->frames.empty()) g_anim_playing = false;
  }
  ImGui::SameLine();
  if (ImGui::Button(">", ImVec2(48, 24))) {
    g_anim_frame = std::min(static_cast<int>(dir->frames.size()) - 1,
                            g_anim_frame + 1);
    g_anim_playing = false;
  }
  ImGui::SameLine();
  if (ImGui::Button(">|", ImVec2(48, 24))) {
    g_anim_frame = std::max(0, static_cast<int>(dir->frames.size()) - 1);
    g_anim_playing = false;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("帧 %d / %d", dir->frames.empty() ? 0 : g_anim_frame + 1,
                      static_cast<int>(dir->frames.size()));

  EditorClampAnimFrame(dir);


  if (!dir->frames.empty()) {
    ImGui::SetNextItemWidth(110.0f);
    int frame_no = dir->frames[g_anim_frame].image;
    if (ImGui::InputInt("帧号", &frame_no)) {
      dir->frames[g_anim_frame].image = std::max(0, frame_no);
      MarkDirty();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int hold = EditorActionHold(*dir, g_anim_frame);
    if (ImGui::InputInt("本帧停留（tick）", &hold)) {
      dir->frames[g_anim_frame].hold = std::max(1, std::min(600, hold));
      MarkDirty();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.3f 秒", static_cast<double>(EditorActionHold(*dir, g_anim_frame)) / 60.0);
  }

  // ★ 时间线栅格：**列 = 时间格子（tick）**，一个画面可以跨多列 ★
  //   frames[] 是"画面"（关键帧），holds[] 是每个画面停留多少 tick。
  //   所以 2 张图各停 2 tick = 4 列；不是"每列换一张图"。
  ImGui::Separator();
  {
    const float head_w = 56.0f;
    const float cell_w = 18.0f;
    const float row_h = 22.0f;
    const int n = static_cast<int>(dir->frames.size());
    // 展开：tick -> 关键帧；key_start[i] = 第 i 个画面的起始 tick
    std::vector<int> tick_key;
    std::vector<int> key_start(static_cast<size_t>(std::max(n, 1)), 0);
    for (int i = 0; i < n; ++i) {
      key_start[static_cast<size_t>(i)] = static_cast<int>(tick_key.size());
      const int h = std::max(1, EditorActionHold(*dir, i));
      for (int t = 0; t < h; ++t) tick_key.push_back(i);
    }
    const int ticks = static_cast<int>(tick_key.size());
    ImGui::BeginChild("##seq_grid", ImVec2(0, row_h * 4 + 30), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    // 用一个显式的 InvisibleButton 覆盖整张表来接收点击：
    // 之前用 IsWindowHovered() 判断，面板被裁剪/滚动的时
  // 候会失灵（点上去没反应就是这个原因）。
    const ImVec2 grid_size(head_w + cell_w * static_cast<float>(std::max(ticks, 1)) + 10.0f,
                           row_h * 4 + 24.0f);
    ImGui::InvisibleButton("##grid_hit", grid_size);
    // tick 标尺（每 5 格一个数字）
    for (int t = 0; t < ticks; t += 5) {
      char num[16];
      std::snprintf(num, sizeof(num), "%d", t);
      dl->AddText(ImVec2(p0.x + head_w + t * cell_w + 1.0f, p0.y + 2.0f),
                  IM_COL32(150, 158, 172, 255), num);
    }
    static const char* kRows[4] = {"帧", "碰撞", "受击", "攻击"};
    // 每格的底色（当前画面覆盖的 tick 高亮）
    for (int r = 0; r < 4; ++r) {
      const float y = p0.y + 16.0f + static_cast<float>(r) * row_h;
      dl->AddText(ImVec2(p0.x + 4.0f, y + 4.0f), IM_COL32(180, 188, 200, 255), kRows[r]);
      for (int t = 0; t < ticks; ++t) {
        const float x = p0.x + head_w + static_cast<float>(t) * cell_w;
        const bool cur = (tick_key[t] == g_anim_frame);
        const ImU32 bg = cur ? IM_COL32(58, 88, 138, 255)
                             : (((t & 1) != 0) ? IM_COL32(44, 44, 44, 255)
                                               : IM_COL32(38, 38, 38, 255));
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cell_w - 2.0f, y + row_h - 3.0f), bg);
      }
    }
    // 每个画面画成一个块（宽度 = hold * cell_w）
    for (int i = 0; i < n; ++i) {
      const float x = p0.x + head_w + static_cast<float>(key_start[i]) * cell_w;
      const float w = static_cast<float>(std::max(1, EditorActionHold(*dir, i))) * cell_w - 2.0f;
      // 帧行：块 + 图集号
      const float y0 = p0.y + 16.0f;
      dl->AddRect(ImVec2(x, y0), ImVec2(x + w, y0 + row_h - 3.0f),
                  IM_COL32(120, 128, 140, 255));
      char num[16];
      std::snprintf(num, sizeof(num), "%d", dir->frames[i].image);
      dl->AddText(ImVec2(x + 3.0f, y0 + 3.0f), IM_COL32(222, 228, 238, 255), num);
      // 碰撞行：逐帧盒；没有逐帧设置时回退到对象默认碰撞
      const pgmmv::CollisionBox* cb =
          pgmmv::ResolveFrameBox(*dir, i, 0,
                                 obj != nullptr ? &obj->collision : nullptr);
      if (cb != nullptr && cb->w > 0 && cb->h > 0) {
        const float yc = p0.y + 16.0f + row_h;
        dl->AddRectFilled(ImVec2(x + 1.0f, yc + 4.0f),
                          ImVec2(x + w - 1.0f, yc + row_h - 7.0f),
                          IM_COL32(80, 150, 240, 255));
      }
      // 受击行
      const pgmmv::CollisionBox* hb =
          pgmmv::ResolveFrameBox(*dir, i, 2, nullptr);
      if (hb != nullptr && hb->w > 0 && hb->h > 0) {
        const float y1 = p0.y + 16.0f + row_h * 2.0f;
        dl->AddRectFilled(ImVec2(x + 1.0f, y1 + 4.0f), ImVec2(x + w - 1.0f, y1 + row_h - 7.0f),
                          IM_COL32(226, 152, 62, 255));
      }
      // 攻击行
      const pgmmv::CollisionBox* ab =
          pgmmv::ResolveFrameBox(*dir, i, 1, nullptr);
      if (ab != nullptr && ab->w > 0 && ab->h > 0) {
        const float y2 = p0.y + 16.0f + row_h * 3.0f;
        dl->AddRectFilled(ImVec2(x + 1.0f, y2 + 4.0f), ImVec2(x + w - 1.0f, y2 + row_h - 7.0f),
                          IM_COL32(90, 200, 110, 255));
      }
    }
    // 点某一格选中它所属的**画面**；按住横向拖动 = 调整画面顺序
    static int drag_from = -1;
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool in_cells = ImGui::IsItemHovered() && m.x > p0.x + head_w && ticks > 0;
    int hover_col = -1;
    if (in_cells) {
      const int t = static_cast<int>((m.x - p0.x - head_w) / cell_w);
      if (t >= 0 && t < ticks) hover_col = tick_key[static_cast<size_t>(t)];
    }
    if (in_cells && ImGui::IsMouseClicked(0) && hover_col >= 0) {
      g_anim_frame = hover_col;
      g_anim_playing = false;
      drag_from = hover_col;
      // ★ 点哪一行就画哪个盒子 ★（帧行只选帧，其余行顺带切换绘制目标）
      const int row = static_cast<int>((m.y - (p0.y + 16.0f)) / row_h);
      if (row >= 1 && row <= 3) g_box_draw_target = row - 1;
    }
    if (drag_from >= 0 && hover_col >= 0 && hover_col != drag_from &&
        ImGui::IsMouseDown(0)) {
      const int from = drag_from;
      const int to = hover_col;
      auto move1 = [](auto& v, int a, int b) {
        if (a < b) {
          std::rotate(v.begin() + a, v.begin() + a + 1, v.begin() + b + 1);
        } else {
          std::rotate(v.begin() + b, v.begin() + a, v.begin() + a + 1);
        }
      };
      move1(dir->frames, from, to);
      g_anim_frame = to;
      drag_from = to;
      MarkDirty();
    }
    if (!ImGui::IsMouseDown(0)) drag_from = -1;
    ImGui::EndChild();
  }
}

void DrawObjectAnimation() {
  pgmmv::AnimationDoc* adoc = EnsureAnimation();
  if (adoc == nullptr) {
    ImGui::TextDisabled("还没有动画：在左边「+ 动画」新建一个");
    return;
  }
  pgmmv::ActionDef* action = CurrentAnimAction();
  if (action == nullptr) {
    ImGui::TextDisabled("这个动画还没有动作");
    return;
  }
  const int dir_count = 1 + static_cast<int>(action->directions.size());
  if (g_anim_direction < 0) g_anim_direction = 0;
  if (g_anim_direction >= dir_count) g_anim_direction = dir_count - 1;
  pgmmv::DirectionDef* dir = pgmmv::ActionDirectionAt(action, g_anim_direction);
  EditorEnsureActionHolds(dir);
  EditorClampAnimFrame(dir);

  ImGui::BeginChild("##seq_main", ImVec2(0, 0), false);
  const float top_h = 184.0f;

  ImGui::BeginChild("##seq_actions_col", ImVec2(172, top_h), true);
  ImGui::TextDisabled("动作");
  for (size_t i = 0; i < adoc->actions.size(); ++i) {
    const pgmmv::ActionDef& act = adoc->actions[i];
    const bool sel = (g_selected_action == act.name);
    std::string label = act.name;
    if (ImGui::Selectable(label.c_str(), sel, 0, ImVec2(92.0f, 0.0f))) {
      SelectAction(act.name);
      g_anim_direction = 0;
      g_anim_frame = 0;
      action = CurrentAnimAction();
      if (action != nullptr) {
        dir = pgmmv::ActionDirectionAt(action, 0);
        if (dir != nullptr) {
          EditorEnsureActionHolds(dir);
          EditorClampAnimFrame(dir);
        }
      }
      UiLogF("anim select %s", act.name.c_str());
    }
    LogItemRect(("act_" + std::string(act.name)).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%df", static_cast<int>(act.frames.size()));
  }
  if (ImGui::Button("+ 动作", ImVec2(64, 22))) EditorAddAction();
  ImGui::SameLine();
  if (adoc->actions.size() > 1 && ImGui::Button("删除", ImVec2(52, 22))) {
    EditorDeleteSelectedAction();
    g_anim_direction = 0;
  }
  ImGui::EndChild();
  ImGui::SameLine();

  ImGui::BeginChild("##seq_dirs_col", ImVec2(150, top_h), true);
  ImGui::TextDisabled("变体 / 方向");
  for (int i = 0; i < dir_count; ++i) {
    const char* name = i == 0 ? action->base_variant.c_str()
                              : action->directions[i - 1].name.c_str();
    if (ImGui::Selectable(name, g_anim_direction == i)) {
      g_anim_direction = i;
      g_anim_frame = 0;
    }
  }
  if (ImGui::Button("+ 变体", ImVec2(64, 22))) {
    pgmmv::DirectionDef nd;
    const int idx = static_cast<int>(action->directions.size());
    nd.name = "变体 " + std::to_string(idx + 1);
    nd.frames = dir->frames;  // 整份帧数据一起复制
    nd.origin_x = dir->origin_x;
    nd.origin_y = dir->origin_y;
    nd.offset_x = dir->offset_x;
    nd.offset_y = dir->offset_y;
    nd.flip_h = dir->flip_h;
    nd.flip_v = dir->flip_v;
    nd.fps = dir->fps;
    nd.loop = dir->loop;
    action->directions.push_back(nd);
    g_anim_direction = dir_count;
    g_anim_frame = 0;
    MarkDirty();
  }
  ImGui::SameLine();
  if (g_anim_direction > 0 && ImGui::Button("删除", ImVec2(52, 22))) {
    action->directions.erase(action->directions.begin() + (g_anim_direction - 1));
    g_anim_direction = 0;
    g_anim_frame = 0;
    MarkDirty();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  ImGui::BeginChild("##seq_frames_col", ImVec2(0, top_h), true);
  ImGui::TextDisabled("帧（%d）", static_cast<int>(dir->frames.size()));
  // ★ 新建帧 = 复制当前帧 ★（一整份数据，含三个盒子和以后的连接点）
    if (InlineButton("+ 帧", 64)) {
    const int at = dir->frames.empty()
                       ? 0
                       : std::min(static_cast<int>(dir->frames.size()),
                                  g_anim_frame + 1);
    pgmmv::ActionFrame f;
    if (!dir->frames.empty()) {
      const int src = std::max(0, std::min(g_anim_frame,
                                  static_cast<int>(dir->frames.size()) - 1));
      f = dir->frames[static_cast<size_t>(src)];
    } else {
      f.hold = pgmmv::ActionDefaultHold(*dir);
    }
    dir->frames.insert(dir->frames.begin() + at, f);
    g_anim_frame = at;
    MarkDirty();
  }
  ImGui::SameLine();
  if (DangerInlineButton("删除帧", 64) && dir->frames.size() > 1) {
    dir->frames.erase(dir->frames.begin() + g_anim_frame);
    if (g_anim_frame >= static_cast<int>(dir->frames.size())) {
      g_anim_frame = static_cast<int>(dir->frames.size()) - 1;
    }
    MarkDirty();
  }
  ImGui::SameLine();
  ImGui::Checkbox("循环", &dir->loop);
  if (ImGui::IsItemDeactivatedAfterEdit()) MarkDirty();
  for (int i = 0; i < static_cast<int>(dir->frames.size()); ++i) {
    auto state_ch = [](pgmmv::BoxState st) {
      return st == pgmmv::BoxState::kNone ? "无"
             : st == pgmmv::BoxState::kBox ? "有" : "沿";
    };
    char label[128];
    std::snprintf(label, sizeof(label), "帧 %d  图集 %d  %dt  攻%s 受%s",
                  i, dir->frames[i].image, EditorActionHold(*dir, i),
                  state_ch(dir->frames[i].attack.state),
                  state_ch(dir->frames[i].hurt.state));
    if (ImGui::Selectable(label, g_anim_frame == i)) {
      g_anim_frame = i;
      g_anim_playing = false;
    }
  }
  ImGui::EndChild();

  if (g_anim_playing && !dir->frames.empty()) {
    g_anim_accum += static_cast<double>(ImGui::GetIO().DeltaTime) * 60.0;
    while (g_anim_accum >= static_cast<double>(EditorActionHold(*dir, g_anim_frame))) {
      g_anim_accum -= static_cast<double>(EditorActionHold(*dir, g_anim_frame));
      ++g_anim_frame;
      if (g_anim_frame >= static_cast<int>(dir->frames.size())) {
        if (dir->loop) {
          g_anim_frame = 0;
        } else {
          g_anim_frame = static_cast<int>(dir->frames.size()) - 1;
          g_anim_playing = false;
          break;
        }
      }
    }
  }

  // 预览让出更多空间给下面的时间线栅格（3 行 + 标尺 ≈ 100px）
  ImGui::BeginChild("##seq_preview", ImVec2(0, 280), false);
  DrawObjectPreview(adoc->image, adoc->frame_w, adoc->frame_h, *dir);
  ImGui::EndChild();
  DrawObjectTimeline(nullptr, dir);
  ImGui::EndChild();
}

bool ProjectHasAssetPath(const std::string& path) {
  for (const pgmmv::Asset& a : g_project.assets) {
    if (a.path == path) return true;
  }
  return false;
}

void ExportCurrentObjectTemplate() {
  pgmmv::ObjectDoc* obj = CurrentObject();
  if (obj == nullptr) return;
  std::filesystem::path path;
  if (!PickObjectJsonSave(&path, obj->id)) return;
  std::string error;
  if (!pgmmv::ObjectSave(path, obj, &error)) {
    SetStatus("导出对象模板失败：" + error);
    return;
  }
  SetStatus("已导出对象模板：" + PathText(path));
}

bool FilesEqual(const std::filesystem::path& a, const std::filesystem::path& b) {
  std::ifstream fa(a, std::ios::binary);
  std::ifstream fb(b, std::ios::binary);
  if (!fa || !fb) return false;
  std::vector<char> da((std::istreambuf_iterator<char>(fa)),
                       std::istreambuf_iterator<char>());
  std::vector<char> db((std::istreambuf_iterator<char>(fb)),
                       std::istreambuf_iterator<char>());
  return da == db;
}

bool ImportTemplateImageDependency(const std::filesystem::path& template_path,
                                   std::string* image_path, std::string* error) {
  if (image_path->empty()) return true;
  const std::filesystem::path rel(*image_path);
  const std::string wanted_name = rel.filename().u8string();
  for (const pgmmv::Asset& asset : g_project.assets) {
    if (std::filesystem::path(asset.path).filename().u8string() == wanted_name) {
      *image_path = asset.path;
      return true;
    }
  }
  const std::filesystem::path dir = template_path.parent_path();
  const std::filesystem::path candidates[] = {
      dir / rel,
      dir.parent_path() / rel,
      dir / rel.filename(),
  };
  std::filesystem::path source;
  for (const std::filesystem::path& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && !ec) {
      source = candidate;
      break;
    }
  }
  if (source.empty()) {
    if (error != nullptr) {
      *error = "当前项目没有资源 " + *image_path + "，模板目录附近也没有同名文件";
    }
    return false;
  }
  for (const pgmmv::Asset& asset : g_project.assets) {
    const std::filesystem::path existing = g_project.root_dir / asset.path;
    std::error_code ec;
    if (std::filesystem::exists(existing, ec) && !ec && FilesEqual(existing, source)) {
      *image_path = asset.path;
      return true;
    }
  }
  std::string id;
  std::string import_error;
  if (!pgmmv::ProjectImportPng(&g_project, source, &id, &import_error)) {
    if (error != nullptr) *error = import_error;
    return false;
  }
  const pgmmv::Asset* asset = g_project.FindAsset(id);
  if (asset == nullptr) {
    if (error != nullptr) *error = "导入图像后找不到资源记录";
    return false;
  }
  *image_path = asset->path;
  g_selected_asset = id;
  SyncAssetBuffer();
  return true;
}

void ExportCurrentObjectPackage() {
  pgmmv::ObjectDoc* obj = CurrentObject();
  if (obj == nullptr) return;
  std::filesystem::path dir;
  if (!PickFolder(&dir)) return;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    SetStatus("导出模板包失败：无法创建目录");
    return;
  }
  std::string error;
  if (!pgmmv::ObjectSave(dir / "object.json", obj, &error)) {
    SetStatus("导出模板包失败：" + error);
    return;
  }
  if (!obj->image.empty()) {
    const std::filesystem::path src = g_project.root_dir / obj->image;
    const std::filesystem::path dst = dir / obj->image;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec) {
      SetStatus("对象已导出，但复制图像失败：" + ec.message());
      return;
    }
  }
  SetStatus("已导出对象模板包：" + PathText(dir));
}

bool ImportObjectTemplatePath(const std::filesystem::path& path, bool push_undo,
                                 std::string* imported_id, std::string* error) {
  pgmmv::ObjectDoc obj;
  if (!pgmmv::ObjectLoad(path, &obj, error)) return false;
  if (push_undo) PushUndo();
  if (!obj.image.empty() && !ProjectHasAssetPath(obj.image)) {
    if (!ImportTemplateImageDependency(path, &obj.image, error)) return false;
  }
  std::string base = obj.id + "_import";
  std::string id = base;
  int n = 2;
  while (g_project.FindObject(id) != nullptr) id = base + "_" + std::to_string(n++);
  obj.id = id;
  obj.name += " Import";
  g_project.objects.push_back(std::move(obj));
  g_selected_object = id;
  g_selected_action = "idle";
  if (imported_id != nullptr) *imported_id = id;
  MarkDirty();
  return true;
}

void ImportObjectTemplate() {
  std::filesystem::path path;
  if (!PickObjectJsonOpen(&path)) return;
  std::string id;
  std::string error;
  if (!ImportObjectTemplatePath(path, true, &id, &error)) {
    SetStatus("导入失败：" + error);
    return;
  }
  SetStatus("已导入对象模板：" + id + "（未保存）");
}

void ImportObjectTemplatesBulk() {
  std::vector<std::filesystem::path> paths;
  if (!PickObjectJsonOpenMany(&paths) || paths.empty()) return;
  PushUndo();
  int imported = 0;
  std::string last_error;
  for (const std::filesystem::path& path : paths) {
    std::string id;
    if (ImportObjectTemplatePath(path, false, &id, &last_error)) {
      ++imported;
    }
  }
  if (imported == 0) {
    SetStatus("批量导入失败：" + last_error);
    return;
  }
  SetStatus("已批量导入 " + std::to_string(imported) + " 个对象模板（未保存）");
}


void DuplicateCurrentObject() {
  pgmmv::ObjectDoc* src = CurrentObject();
  if (src == nullptr) return;
  pgmmv::ObjectDoc copy = *src;
  PushUndo();
  std::string base = src->id + "_copy";
  std::string id = base;
  int n = 2;
  while (g_project.FindObject(id) != nullptr) id = base + "_" + std::to_string(n++);
  copy.id = id;
  copy.name = src->name + " Copy";
  g_project.objects.push_back(std::move(copy));
  g_selected_object = id;
  g_selected_action = "idle";
  MarkDirty();
}


void DrawFramePicker(pgmmv::AnimationDoc* adoc, pgmmv::DirectionDef* a) {
  if (!ImGui::CollapsingHeader("选择帧图像", ImGuiTreeNodeFlags_DefaultOpen)) return;
  // ★ 图集在这里也能换 ★（以前只有"基础数据"里能换，选帧的时候根本看不到）
  ImGui::TextUnformatted("图集");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0f);
  {
    const pgmmv::Asset* cur_img =
        adoc->image.empty() ? nullptr : g_project.FindAssetByPath(adoc->image);
    const char* cur_name = cur_img != nullptr ? cur_img->name.c_str() : "(无)";
    if (ImGui::BeginCombo("##pick_image", cur_name)) {
      for (const pgmmv::Asset& asset : g_project.assets) {
        if (asset.kind != pgmmv::AssetKind::kImage) continue;
        if (ImGui::Selectable(asset.name.c_str(), adoc->image == asset.path)) {
          if (adoc->image.empty()) {
            adoc->frame_w = asset.width;
            adoc->frame_h = asset.height;
          }
          adoc->image = asset.path;
          PreviewRelease();
          MarkDirty();
        }
      }
      ImGui::EndCombo();
    }
  }
  if (adoc->image.empty()) {
    ImGui::TextDisabled("这个动画还没有图像");
    return;
  }
  const pgmmv::Asset* img = g_project.FindAssetByPath(adoc->image);
  if (img == nullptr) {
    ImGui::TextDisabled("图像资源不存在");
    return;
  }
  if (g_preview.asset_id != img->id) LoadPreview(*img);
  if (g_preview.srv == nullptr || g_preview.width <= 0 || g_preview.height <= 0) {
    ImGui::TextDisabled("图像预览不可用");
    return;
  }
  // ★ 帧尺寸在这里也能改 ★
  //   素材的格子大小不固定（16×16 / 32×32 / 48×64…），以前只有"基础数据"里能改，
  //   选帧的时候看不到格子对不对，只能来回切页。
  ImGui::TextUnformatted("帧尺寸");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(96.0f);
  int fw_edit = adoc->frame_w;
  if (ImGui::InputInt("##pick_fw", &fw_edit, 1, 8)) {
    adoc->frame_w = std::max(1, fw_edit);
    MarkDirty();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("×");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(96.0f);
  int fh_edit = adoc->frame_h;
  if (ImGui::InputInt("##pick_fh", &fh_edit, 1, 8)) {
    adoc->frame_h = std::max(1, fh_edit);
    MarkDirty();
  }
  const int fw = std::max(1, adoc->frame_w);
  const int fh = std::max(1, adoc->frame_h);
  const int cols = std::max(1, g_preview.width / fw);
  const int rows = std::max(1, g_preview.height / fh);
  ImGui::TextDisabled("图片 %d×%d → %d×%d 格（共 %d 格）", g_preview.width, g_preview.height,
                      cols, rows, cols * rows);
  float sheet_scale = (ImGui::GetContentRegionAvail().x - 24.0f) /
                      static_cast<float>(g_preview.width);
  if (sheet_scale > 2.0f) sheet_scale = 2.0f;
  if (sheet_scale <= 0.0f) return;
  const ImVec2 sheet_size(static_cast<float>(g_preview.width) * sheet_scale,
                          static_cast<float>(g_preview.height) * sheet_scale);
  ImGui::BeginChild("##frame_sheet", ImVec2(0, std::min(sheet_size.y, 300.0f)), true);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##frame_sheet_hit", sheet_size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddImage(reinterpret_cast<ImTextureID>(g_preview.srv), p,
               ImVec2(p.x + sheet_size.x, p.y + sheet_size.y));
  const float cw = static_cast<float>(fw) * sheet_scale;
  const float ch = static_cast<float>(fh) * sheet_scale;
  for (int c = 0; c <= cols; ++c) {
    dl->AddLine(ImVec2(p.x + static_cast<float>(c) * cw, p.y),
                ImVec2(p.x + static_cast<float>(c) * cw, p.y + sheet_size.y),
                IM_COL32(120, 120, 120, 110));
  }
  for (int r = 0; r <= rows; ++r) {
    dl->AddLine(ImVec2(p.x, p.y + static_cast<float>(r) * ch),
                ImVec2(p.x + sheet_size.x, p.y + static_cast<float>(r) * ch),
                IM_COL32(120, 120, 120, 110));
  }
  int cur_frame = 0;
  int cur_index = 0;
  if (!a->frames.empty()) {
    cur_index = std::max(0, std::min(g_anim_frame,
                                     static_cast<int>(a->frames.size()) - 1));
    cur_frame = a->frames[cur_index].image;
  }
  const int cur_col = cur_frame % cols;
  const int cur_row = cur_frame / cols;
  dl->AddRect(ImVec2(p.x + static_cast<float>(cur_col) * cw,
                     p.y + static_cast<float>(cur_row) * ch),
              ImVec2(p.x + static_cast<float>(cur_col + 1) * cw,
                     p.y + static_cast<float>(cur_row + 1) * ch),
              IM_COL32(80, 150, 240, 255), 0.0f, 0, 3.0f);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int c = static_cast<int>((m.x - p.x) / cw);
    const int r = static_cast<int>((m.y - p.y) / ch);
    if (c >= 0 && c < cols && r >= 0 && r < rows) {
      dl->AddRect(ImVec2(p.x + static_cast<float>(c) * cw,
                         p.y + static_cast<float>(r) * ch),
                  ImVec2(p.x + static_cast<float>(c + 1) * cw,
                         p.y + static_cast<float>(r + 1) * ch),
                  IM_COL32(240, 200, 100, 255), 0.0f, 0, 2.0f);
      if (ImGui::IsMouseClicked(0) && !a->frames.empty()) {
        a->frames[cur_index].image = r * cols + c;
        MarkDirty();
      }
    }
  }
  ImGui::EndChild();
  ImGui::TextDisabled("点击格子 = 设置当前帧；共 %d 格", cols * rows);
}

void DrawActionSettings() {
  pgmmv::AnimationDoc* adoc = EnsureAnimation();
  if (adoc == nullptr) {
    ImGui::TextDisabled("还没有动画");
    return;
  }
  // 动画资源自己的属性（名字 / id / 图集 / 帧尺寸在下面"选择帧图像"里）
  if (ImGui::CollapsingHeader("动画资源", ImGuiTreeNodeFlags_DefaultOpen)) {
    static char name_buf[96] = "";
    static std::string name_for = "";
    if (name_for != adoc->id) {
      std::snprintf(name_buf, sizeof(name_buf), "%s", adoc->name.c_str());
      name_for = adoc->id;
    }
    ImGui::TextUnformatted("名称");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##anim_name", name_buf, sizeof(name_buf));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      PushUndo();
      adoc->name = name_buf;
      MarkDirty();
    }
    ImGui::TextDisabled("id：%s（文件名）", adoc->id.c_str());
    int users = 0;
    for (const pgmmv::ObjectDoc& o : g_project.objects) {
      if (o.animation == adoc->id) ++users;
    }
    ImGui::TextDisabled("被 %d 个对象引用", users);
  }
  ImGui::Separator();
  pgmmv::ActionDef* a = CurrentAnimAction();
  if (ImGui::CollapsingHeader("动作", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (a == nullptr) {
      ImGui::TextDisabled("没有选中动作");
    } else {
      pgmmv::DirectionDef* d = pgmmv::ActionDirectionAt(a, g_anim_direction);
      SyncActionNameBuffer();
      ImGui::TextUnformatted("动作名称");
      ImGui::SetNextItemWidth(-1.0f);
      ImGui::InputText("##action_name", g_action_name_buf,
                       sizeof(g_action_name_buf));
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        EditorRenameAction(g_action_name_buf);
        SyncActionNameBuffer();
      }
      if (g_anim_direction == 0) {
        static std::string base_variant_for;
        static char base_variant_buf[64] = {};
        if (base_variant_for != a->name) {
          base_variant_for = a->name;
          std::snprintf(base_variant_buf, sizeof(base_variant_buf), "%s",
                        a->base_variant.c_str());
        }
        ImGui::TextUnformatted("变体名称");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputText("##base_variant", base_variant_buf,
                             sizeof(base_variant_buf))) {
          a->base_variant = base_variant_buf;
          MarkDirty();
        }
      } else if (d != nullptr) {
        static std::string dir_for;
        static char dir_buf[64] = {};
        const std::string dk = a->name + "/" + std::to_string(g_anim_direction);
        if (dir_for != dk) {
          dir_for = dk;
          std::snprintf(dir_buf, sizeof(dir_buf), "%s", d->name.c_str());
        }
        ImGui::TextUnformatted("方向名称");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        if (ImGui::InputText("##dir_name", dir_buf, sizeof(dir_buf))) {
          d->name = dir_buf;
          MarkDirty();
        }
      }
      EditorEnsureActionHolds(d);
      int total_ticks = 0;
      for (const pgmmv::ActionFrame& f : d->frames) {
        total_ticks += f.hold > 0 ? f.hold : pgmmv::ActionDefaultHold(*d);
      }
      // ★ 这里的"帧数"= 时间格子数（tick 总数），也就是这个动作播放多久 ★
      //   不是画面数：2 张图各停 2 tick，就是 4 帧时长、2 个画面。
      //   加时长 = 延长最后一个画面；减时长 = 从末尾扣（扣到 0 就删画面）。
      ImGui::TextUnformatted("动作总时长（tick）");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90.0f);
      int total_target = total_ticks;
      if (ImGui::InputInt("##action_totalticks", &total_target, 1, 4)) {
        // ★ 改总时长只动【当前选中的画面】，不再从末尾扣 ★
        //   以前是"从后面一帧一帧扣"，改第一帧的时长会把后面的帧直接删掉。
        total_target = std::max(1, std::min(8192, total_target));
        int cur = 0;
        for (const pgmmv::ActionFrame& f : d->frames) {
          cur += f.hold > 0 ? f.hold : pgmmv::ActionDefaultHold(*d);
        }
        if (!d->frames.empty() && total_target != cur) {
          const int f = std::max(0, std::min(g_anim_frame,
                                             static_cast<int>(d->frames.size()) - 1));
          int& h = d->frames[static_cast<size_t>(f)].hold;
          h = std::max(1, (h > 0 ? h : pgmmv::ActionDefaultHold(*d)) +
                              (total_target - cur));
        }
        EditorEnsureActionHolds(d);
        EditorClampAnimFrame(d);
        MarkDirty();
        UiLogF("action total ticks -> %d (frame %d)", total_target, g_anim_frame);
      }
      ImGui::SameLine();
      ImGui::TextDisabled("画面 %d 个 · %.3f 秒", static_cast<int>(d->frames.size()),
                          static_cast<double>(total_ticks) / 60.0);
      ImGui::TextDisabled("（改 tick 只会加减「当前画面」的时长，不动其它画面）");
      ImGui::TextUnformatted("回退 fps（旧数据）");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("每帧 hold 优先；没有 hold 的旧动画才用这个 fps。");
      }
      ImGui::SetNextItemWidth(90.0f);
      int fps = d->fps;
      if (ImGui::InputInt("##action_fps", &fps)) {
        d->fps = std::max(1, std::min(60, fps));
        MarkDirty();
      }
      if (ImGui::Checkbox("循环", &d->loop)) MarkDirty();
      ImGui::Separator();
      ImGui::TextUnformatted("原点 X / Y（像素）");
      ImGui::SetNextItemWidth(88.0f);
      int ox = d->origin_x;
      if (ImGui::InputInt("##origin_x", &ox)) { d->origin_x = ox; MarkDirty(); }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(88.0f);
      int oy = d->origin_y;
      if (ImGui::InputInt("##origin_y", &oy)) { d->origin_y = oy; MarkDirty(); }
      if (ImGui::SmallButton("左上##origin_tl")) {
        d->origin_x = 0; d->origin_y = 0; MarkDirty();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("中心##origin_c")) {
        d->origin_x = adoc->frame_w / 2; d->origin_y = adoc->frame_h / 2; MarkDirty();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("脚下##origin_b")) {
        d->origin_x = adoc->frame_w / 2; d->origin_y = adoc->frame_h; MarkDirty();
      }
      ImGui::TextUnformatted("判定盒");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("画布拖拽和下方“判定状态”共用这个选择；三种都是逐帧盒，未设置时碰撞回退到对象默认盒。");
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120.0f);
      const char* kBoxNames[3] = {"碰撞", "受击", "攻击"};
      ImGui::Combo("##box_draw_target", &g_box_draw_target, kBoxNames, 3);
      ImGui::TextUnformatted("偏移 X / Y（像素）");
      ImGui::SetNextItemWidth(88.0f);
      int fx = d->offset_x;
      if (ImGui::InputInt("##offset_x", &fx)) { d->offset_x = fx; MarkDirty(); }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(88.0f);
      int fy = d->offset_y;
      if (ImGui::InputInt("##offset_y", &fy)) { d->offset_y = fy; MarkDirty(); }
      ImGui::TextUnformatted("翻转");
      if (ImGui::Checkbox("水平##flip_h", &d->flip_h)) MarkDirty();
      ImGui::SameLine();
      if (ImGui::Checkbox("垂直##flip_v", &d->flip_v)) MarkDirty();
    }
  }
  // 连接点（当前帧）：枪口 / 手 这类命名位置，坐标相对中心点
  if (a != nullptr) {
    pgmmv::DirectionDef* dd = pgmmv::ActionDirectionAt(a, g_anim_direction);
    if (dd != nullptr && !dd->frames.empty() &&
        ImGui::CollapsingHeader("连接点（当前帧）",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      const int fi = std::max(0, std::min(g_anim_frame,
                            static_cast<int>(dd->frames.size()) - 1));
      auto& pts = dd->frames[static_cast<size_t>(fi)].attach;
      for (size_t i = 0; i < pts.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", pts[i].name.c_str());
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputText("##pt_name", nb, sizeof(nb))) {
          pts[i].name = nb;
          MarkDirty();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        if (ImGui::InputInt("##pt_x", &pts[i].x)) MarkDirty();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        if (ImGui::InputInt("##pt_y", &pts[i].y)) MarkDirty();
        ImGui::SameLine();
        if (ImGui::Button("删", ImVec2(30, 0))) {
          PushUndo();
          pts.erase(pts.begin() + static_cast<long>(i));
          MarkDirty();
          ImGui::PopID();
          break;
        }
        ImGui::PopID();
      }
      if (ImGui::Button("+ 连接点", ImVec2(90, 22))) {
        PushUndo();
        pgmmv::AttachPoint ap;
        ap.name = "point";
        pts.push_back(ap);
        MarkDirty();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("坐标相对中心点");
    }
  }
  if (a != nullptr) {
    DrawFramePicker(adoc, pgmmv::ActionDirectionAt(a, g_anim_direction));
  }
}


void DrawObjectLeft() {
  ImGui::TextUnformatted("对象");
  if (!g_has_project) {
    ImGui::TextDisabled("先新建或打开项目");
    return;
  }
  if (RowButton("新建对象")) {
    CreateObject();
  }
  if (RowButton("导入对象模板")) {
    ImportObjectTemplate();
  }
  if (RowButton("批量导入对象模板")) {
    ImportObjectTemplatesBulk();
  }
  ImGui::Separator();
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##object_filter", "搜索对象", g_object_filter,
                           sizeof(g_object_filter));
  if (g_project.objects.empty()) {
    ImGui::TextDisabled("没有对象");
  }
  for (const pgmmv::ObjectDoc& o : g_project.objects) {
    if (!TextMatchesFilter(o.name, g_object_filter) &&
        !TextMatchesFilter(o.id, g_object_filter)) {
      continue;
    }
    const std::string label = o.name + "##" + o.id;
    if (ImGui::Selectable(label.c_str(), g_selected_object == o.id)) {
      SelectObject(o.id);
          }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
      SelectObject(o.id);
      std::snprintf(g_rename_object_buf, sizeof(g_rename_object_buf), "%s",
                    o.name.c_str());
      g_show_rename_object = true;
    }
  }
  if (!g_selected_object.empty()) {
    ImGui::Separator();
    if (RowButton("重命名选中对象")) {
      const pgmmv::ObjectDoc* obj = CurrentObject();
      std::snprintf(g_rename_object_buf, sizeof(g_rename_object_buf), "%s",
                    obj != nullptr ? obj->name.c_str() : "");
      g_show_rename_object = true;
    }
    if (RowButton("复制选中对象")) DuplicateCurrentObject();
    if (RowButton("导出对象模板")) ExportCurrentObjectTemplate();
    if (RowButton("导出模板包（含图像）")) ExportCurrentObjectPackage();
    if (DangerRowButton("删除选中对象")) {
      DeleteObject(g_selected_object);
    }
  }

}


// ===========================================================================
// PGMMV 风格外壳（重建）
// ===========================================================================

// 资源反查索引：旧实现丢失，先给空实现，资源页后续重写。

void ApplyPgStyle() {
  ImGui::StyleColorsDark();
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 8.0f;
  s.ChildRounding = 8.0f;
  s.FrameRounding = 6.0f;
  s.PopupRounding = 8.0f;
  s.ScrollbarRounding = 8.0f;
  s.GrabRounding = 6.0f;
  s.WindowBorderSize = 0.0f;
  s.ChildBorderSize = 1.0f;
  s.FrameBorderSize = 0.0f;
  s.PopupBorderSize = 1.0f;
  s.WindowPadding = ImVec2(12.0f, 10.0f);
  s.FramePadding = ImVec2(8.0f, 5.0f);
  s.CellPadding = ImVec2(6.0f, 4.0f);
  s.ItemSpacing = ImVec2(8.0f, 6.0f);
  s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
  s.ScrollbarSize = 12.0f;
  s.GrabMinSize = 10.0f;

  const ImVec4 window_bg(0.090f, 0.098f, 0.118f, 1.0f);
  const ImVec4 panel_bg(0.125f, 0.137f, 0.165f, 1.0f);
  const ImVec4 panel_head(0.160f, 0.176f, 0.212f, 1.0f);
  const ImVec4 border(0.204f, 0.224f, 0.267f, 1.0f);
  const ImVec4 frame_bg(0.090f, 0.098f, 0.118f, 1.0f);
  const ImVec4 hover(0.180f, 0.200f, 0.243f, 1.0f);
  const ImVec4 accent(0.298f, 0.553f, 1.000f, 1.0f);
  const ImVec4 text(0.847f, 0.871f, 0.914f, 1.0f);
  const ImVec4 muted(0.494f, 0.529f, 0.592f, 1.0f);
  const ImVec4 sel(0.169f, 0.310f, 0.525f, 1.0f);

  s.Colors[ImGuiCol_WindowBg] = window_bg;
  s.Colors[ImGuiCol_ChildBg] = panel_bg;
  s.Colors[ImGuiCol_PopupBg] = panel_bg;
  s.Colors[ImGuiCol_MenuBarBg] = window_bg;
  s.Colors[ImGuiCol_Border] = border;
  s.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  s.Colors[ImGuiCol_FrameBg] = frame_bg;
  s.Colors[ImGuiCol_FrameBgHovered] = hover;
  s.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.25f, 0.31f, 1.0f);
  s.Colors[ImGuiCol_TitleBg] = window_bg;
  s.Colors[ImGuiCol_TitleBgActive] = panel_head;
  s.Colors[ImGuiCol_TitleBgCollapsed] = window_bg;
  s.Colors[ImGuiCol_Header] = ImVec4(sel.x, sel.y, sel.z, 0.85f);
  s.Colors[ImGuiCol_HeaderHovered] = ImVec4(accent.x, accent.y, accent.z, 0.22f);
  s.Colors[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
  s.Colors[ImGuiCol_Button] = panel_head;
  s.Colors[ImGuiCol_ButtonHovered] = hover;
  s.Colors[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.28f, 0.36f, 1.0f);
  s.Colors[ImGuiCol_Tab] = window_bg;
  s.Colors[ImGuiCol_TabHovered] = ImVec4(accent.x, accent.y, accent.z, 0.25f);
  s.Colors[ImGuiCol_TabSelected] = panel_head;
  s.Colors[ImGuiCol_TabDimmed] = window_bg;
  s.Colors[ImGuiCol_TabDimmedSelected] = panel_head;
  s.Colors[ImGuiCol_Text] = text;
  s.Colors[ImGuiCol_TextDisabled] = muted;
  s.Colors[ImGuiCol_CheckMark] = accent;
  s.Colors[ImGuiCol_SliderGrab] = accent;
  s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.42f, 0.64f, 1.0f, 1.0f);
  s.Colors[ImGuiCol_Separator] = border;
  s.Colors[ImGuiCol_SeparatorHovered] = accent;
  s.Colors[ImGuiCol_SeparatorActive] = accent;
  s.Colors[ImGuiCol_ScrollbarBg] = window_bg;
  s.Colors[ImGuiCol_ScrollbarGrab] = panel_head;
  s.Colors[ImGuiCol_ScrollbarGrabHovered] = hover;
  s.Colors[ImGuiCol_ScrollbarGrabActive] = accent;
  s.Colors[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.20f);
  s.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
  s.Colors[ImGuiCol_ResizeGripActive] = accent;
  s.Colors[ImGuiCol_TableHeaderBg] = panel_head;
  s.Colors[ImGuiCol_TableBorderStrong] = border;
  s.Colors[ImGuiCol_TableBorderLight] = ImVec4(border.x, border.y, border.z, 0.45f);
  s.Colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
  s.Colors[ImGuiCol_TextLink] = accent;
  s.Colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.28f);
  s.Colors[ImGuiCol_NavHighlight] = accent;
  s.Colors[ImGuiCol_DragDropTarget] = accent;
}

bool TextTab(const char* label, bool selected, float w) {
  ImGui::PushStyleColor(ImGuiCol_Button,
                        selected ? ImVec4(0.160f, 0.176f, 0.212f, 1.0f)
                                 : ImVec4(0.090f, 0.098f, 0.118f, 1.0f));
  const bool clicked = ImGui::Button(label, ImVec2(w, 26.0f));
  ImGui::PopStyleColor();
  if (selected) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    dl->AddRectFilled(ImVec2(a.x, b.y - 2.0f), ImVec2(b.x, b.y),
                      IM_COL32(76, 141, 255, 255));
  }
  return clicked;
}

void DrawToolIcon(int i, ImVec2 p) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled(ImVec2(p.x + 12.0f, p.y + 12.0f), 12.0f, IM_COL32(76, 141, 255, 255));
  const ImU32 c = IM_COL32(255, 255, 255, 255);
  const ImVec2 o(p.x + 5.0f, p.y + 5.0f);
  const ImVec2 q(o.x + 14.0f, o.y + 14.0f);
  switch (i) {
    case 0:
      dl->AddRect(o, q, c, 0.0f, 0, 1.5f);
      dl->AddLine(ImVec2(o.x + 3, o.y + 4), ImVec2(o.x + 11, o.y + 4), c, 1.5f);
      break;
    case 1:
      dl->AddRect(o, q, c, 0.0f, 0, 1.5f);
      dl->AddLine(ImVec2(o.x + 7, o.y), ImVec2(o.x + 7, q.y), c, 1.0f);
      dl->AddLine(ImVec2(o.x, o.y + 7), ImVec2(q.x, o.y + 7), c, 1.0f);
      break;
    case 2:
      dl->AddRect(o, q, c, 0.0f, 0, 1.5f);
      dl->AddRectFilled(ImVec2(o.x + 3, o.y + 3), ImVec2(o.x + 5, o.y + 5), c);
      dl->AddRectFilled(ImVec2(o.x + 9, o.y + 3), ImVec2(o.x + 11, o.y + 5), c);
      break;
    case 3:
      dl->AddCircle(ImVec2(o.x + 7, o.y + 7), 6.5f, c, 12, 1.5f);
      break;
    case 4: {
      const ImVec2 m(o.x + 7, o.y + 7);
      dl->AddQuad(ImVec2(m.x, o.y), ImVec2(q.x, m.y), ImVec2(m.x, q.y),
                  ImVec2(o.x, m.y), c, 1.5f);
      break;
    }
    case 5:
      dl->AddLine(ImVec2(o.x, o.y + 7), ImVec2(o.x + 11, o.y + 7), c, 1.5f);
      dl->AddTriangleFilled(ImVec2(o.x + 14, o.y + 7), ImVec2(o.x + 9, o.y + 3),
                            ImVec2(o.x + 9, o.y + 11), c);
      break;
    case 6:
      dl->AddLine(ImVec2(o.x + 7, o.y + 1), ImVec2(o.x + 7, o.y + 13), c, 2.0f);
      dl->AddLine(ImVec2(o.x + 1, o.y + 7), ImVec2(o.x + 13, o.y + 7), c, 2.0f);
      break;
    default:
      dl->AddLine(ImVec2(o.x + 1, o.y + 2), ImVec2(q.x, o.y + 2), c, 1.5f);
      dl->AddLine(ImVec2(o.x + 1, o.y + 7), ImVec2(q.x, o.y + 7), c, 1.5f);
      dl->AddLine(ImVec2(o.x + 1, o.y + 12), ImVec2(q.x, o.y + 12), c, 1.5f);
      break;
  }
}

void DrawPgTabs() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float h = 44.0f;
  float x = start.x;
  for (int i = 0; i < static_cast<int>(sizeof(kTools) / sizeof(kTools[0])); ++i) {
    const float tw = ImGui::CalcTextSize(kTools[i]).x;
    const float w = 8.0f + 24.0f + 8.0f + tw + 14.0f;
    const ImVec2 p(x, start.y);
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton(kTools[i], ImVec2(w, h));
    const bool sel = (g_current_tool == i);
    const bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      sel ? IM_COL32(67, 67, 67, 255)
                          : (hov ? IM_COL32(58, 58, 58, 255) : IM_COL32(46, 46, 46, 255)));
    DrawToolIcon(i, ImVec2(p.x + 8.0f, p.y + 10.0f));
    dl->AddText(ImVec2(p.x + 40.0f, p.y + 13.0f), IM_COL32(230, 230, 230, 255), kTools[i]);
    if (ImGui::IsItemClicked()) g_current_tool = i;
    if (sel) {
      dl->AddRectFilled(ImVec2(p.x, p.y + h - 3.0f), ImVec2(p.x + w, p.y + h),
                        IM_COL32(47, 128, 237, 255));
    }
    x += w;
  }
  ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + h));
}
void DrawTilePageLeft() {
  ImGui::TextDisabled("图块列表");
  if (RowButton("新建图块组")) {
    int n = 1;
    std::string id;
    for (;;) {
      id = "tileset_" + std::to_string(n++);
      if (g_project.FindTileset(id) == nullptr) break;
    }
    PushUndo();
    g_project.tilesets.push_back(
        pgmmv::TilesetCreateDefault(id, id, "", 8, 8, 1));
    g_selected_tileset = id;
    g_paint_tile = 0;
    MarkDirty();
  }
  // ★ 走 DeleteTileset：先查引用，别再直接 erase ★
  //   直接删会把场景里的 tileset id 删成悬空引用（那些格子导出时全变空）。
  if (!g_selected_tileset.empty() && DangerRowButton("删除选中图块组")) {
    DeleteTileset(g_selected_tileset);
  }
  ImGui::Separator();
  for (const pgmmv::TilesetDoc& ts : g_project.tilesets) {
    if (ImGui::Selectable(ts.name.c_str(), g_selected_tileset == ts.id)) {
      g_selected_tileset = ts.id;
      g_paint_tile = 0;
    }
  }
  if (g_project.tilesets.empty()) ImGui::TextDisabled("没有图块组");
}

void DrawTilePageCenter() {
  pgmmv::TilesetDoc* ts = CurrentTileset();
  if (!g_has_project || ts == nullptr) {
    ImGui::TextDisabled("选择或新建一个图块组");
    return;
  }
  ImGui::TextUnformatted("图块素材");
  ImGui::SetNextItemWidth(260.0f);
  const char* cur = ts->image.empty() ? "(未设置)" : ts->image.c_str();
  if (ImGui::BeginCombo("##tile_image", cur)) {
    for (const pgmmv::Asset& a : g_project.assets) {
      if (a.kind != pgmmv::AssetKind::kImage) continue;
      if (ImGui::Selectable(a.path.c_str(), ts->image == a.path)) {
        ts->image = a.path;
        // ★ 换图后必须重算列数 ★
        //   TilesetCreateDefault 建空图块组时 columns=1，只改 image 不重算的话，
        //   后面所有"行宽"的推算都会错位（画出来的图块和选中的不是同一块）。
        NormalizeTilesetDims(ts);
        MarkDirty();
      }
    }
    ImGui::EndCombo();
  }
  const pgmmv::Asset* asset = g_project.FindAssetByPath(ts->image);
  if (asset == nullptr) {
    ImGui::TextDisabled("先在资源页导入图像，再在这里选择");
    return;
  }
  PreviewTexture* pv = AssetPreview(*asset);
  if (pv == nullptr || pv->srv == nullptr) {
    ImGui::TextDisabled("预览不可用");
    return;
  }
  static float zoom = 1.0f;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120.0f);
  ImGui::SliderFloat("##tile_zoom", &zoom, 0.5f, 6.0f, "%.2fx");
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  float scale = std::min(avail.x / static_cast<float>(pv->width),
                         avail.y / static_cast<float>(pv->height));
  scale *= zoom;
  if (scale <= 0.0f) return;
  const ImVec2 size(static_cast<float>(pv->width) * scale,
                    static_cast<float>(pv->height) * scale);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##tile_sheet", size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddImage(reinterpret_cast<ImTextureID>(pv->srv), p0,
               ImVec2(p0.x + size.x, p0.y + size.y));
  const int tw = std::max(1, ts->tile_w);
  const int th = std::max(1, ts->tile_h);
  const int cols = std::max(1, pv->width / tw);
  const int rows = std::max(1, pv->height / th);
  for (int c = 0; c <= cols; ++c) {
    dl->AddLine(ImVec2(p0.x + c * tw * scale, p0.y),
                ImVec2(p0.x + c * tw * scale, p0.y + rows * th * scale),
                IM_COL32(0, 0, 0, 90));
  }
  for (int r = 0; r <= rows; ++r) {
    dl->AddLine(ImVec2(p0.x, p0.y + r * th * scale),
                ImVec2(p0.x + cols * tw * scale, p0.y + r * th * scale),
                IM_COL32(0, 0, 0, 90));
  }
  const int cur_col = g_paint_tile % cols;
  const int cur_row = g_paint_tile / cols;
  dl->AddRect(ImVec2(p0.x + cur_col * tw * scale, p0.y + cur_row * th * scale),
              ImVec2(p0.x + (cur_col + 1) * tw * scale, p0.y + (cur_row + 1) * th * scale),
              IM_COL32(47, 128, 237, 255), 0.0f, 0, 2.5f);
  if (ImGui::IsItemHovered()) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int c = static_cast<int>((m.x - p0.x) / (tw * scale));
    const int r = static_cast<int>((m.y - p0.y) / (th * scale));
    if (c >= 0 && c < cols && r >= 0 && r < rows) {
      dl->AddRect(ImVec2(p0.x + c * tw * scale, p0.y + r * th * scale),
                  ImVec2(p0.x + (c + 1) * tw * scale, p0.y + (r + 1) * th * scale),
                  IM_COL32(255, 196, 80, 255), 0.0f, 0, 2.0f);
      if (ImGui::IsMouseClicked(0)) {
        g_brush_dragging = true;
        g_brush_start_x = c;
        g_brush_start_y = r;
        g_brush_x = c;
        g_brush_y = r;
        g_brush_w = 1;
        g_brush_h = 1;
        g_paint_tile = r * cols + c;
      } else if (g_brush_dragging && ImGui::IsMouseDown(0)) {
        g_brush_x = std::min(c, g_brush_start_x);
        g_brush_y = std::min(r, g_brush_start_y);
        g_brush_w = std::abs(c - g_brush_start_x) + 1;
        g_brush_h = std::abs(r - g_brush_start_y) + 1;
        g_paint_tile = g_brush_y * cols + g_brush_x;
      }
      if (ImGui::IsMouseReleased(0)) g_brush_dragging = false;
    }
  }
  ImGui::TextDisabled("点击格子选中图块；网格 %d × %d", cols, rows);
}

int g_tile_tab = 0;

void DrawTilePageRight() {
  pgmmv::TilesetDoc* ts = CurrentTileset();
  if (!g_has_project || ts == nullptr) {
    ImGui::TextDisabled("未选择图块组");
    return;
  }
  ImGui::TextDisabled("基础");
  ImGui::Text("ID：%s", ts->id.c_str());
  ImGui::Text("名称：%s", ts->name.c_str());
  ImGui::TextUnformatted("宽");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  if (UndoIntField("##tw", &ts->tile_w, 1)) RecomputeTilesetColumns(ts);
  ImGui::SameLine(0.0f, 24.0f);
  ImGui::TextUnformatted("高");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  if (UndoIntField("##th", &ts->tile_h, 1)) RecomputeTilesetColumns(ts);
  if (ts->tile_w < 1) ts->tile_w = 1;
  if (ts->tile_h < 1) ts->tile_h = 1;
  if (g_tile_tab == 0) return;
  ImGui::Separator();
  // ★ 图块本身没有任何属性 ★
  //   碰撞在独立的碰撞层（每格一个 mask），玩法交给触发器和以后的脚本；
  //   以前这里的"墙"复选框运行时根本不读（tile.wall 只在 p3d 里占位），已移除。
  ImGui::TextDisabled("选中的图块 #%d（图块本身没有属性）", g_paint_tile);
  ImGui::TextDisabled("碰撞请用画布上方的「碰撞层」；玩法用触发器。");
}


// ===========================================================================
// 场景页 v1
// ===========================================================================
void DrawSceneTilesetPalette() {
  pgmmv::SceneDoc* s = CurrentScene();
  if (s == nullptr) return;
  EnsureSceneTilesets(s);
  const int slot = ClampPaintSlot(*s);
  const pgmmv::TilesetDoc* ts = TilesetAtSlotC(*s, slot);

  // ★ 图集切换：画笔属于某个槽位，画布按格子自己的槽位取图 ★
  char cur_label[160];
  if (ts != nullptr) {
    std::snprintf(cur_label, sizeof(cur_label), "%d: %s", slot, ts->name.c_str());
  } else {
    std::snprintf(cur_label, sizeof(cur_label), "(本场景没有图块组)");
  }
  ImGui::SetNextItemWidth(-96.0f);
  if (ImGui::BeginCombo("##paint_slot", cur_label)) {
    for (int i = 0; i < static_cast<int>(s->tilesets.size()); ++i) {
      const pgmmv::TilesetDoc* t = TilesetAtSlotC(*s, i);
      char label[192];
      std::snprintf(label, sizeof(label), "%d: %s", i,
                    t != nullptr ? t->name.c_str() : s->tilesets[i].c_str());
      if (ImGui::Selectable(label, i == slot)) g_paint_slot = i;
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("+图集", ImVec2(88.0f, 0.0f))) ImGui::OpenPopup("##add_scene_tileset");
  if (ImGui::BeginPopup("##add_scene_tileset")) {
    bool any = false;
    for (const pgmmv::TilesetDoc& cand : g_project.tilesets) {
      if (std::find(s->tilesets.begin(), s->tilesets.end(), cand.id) !=
          s->tilesets.end()) {
        continue;
      }
      any = true;
      if (ImGui::Selectable(cand.name.c_str())) {
        PushUndo();
        s->tilesets.push_back(cand.id);
        g_paint_slot = static_cast<int>(s->tilesets.size()) - 1;
        MarkDirty();
      }
    }
    if (!any) ImGui::TextDisabled("（没有别的图块组）");
    ImGui::EndPopup();
  }
  if (ts == nullptr) {
    ImGui::TextDisabled("选一个图块组再画");
    return;
  }
  const pgmmv::Asset* asset = g_project.FindAssetByPath(ts->image);
  if (asset == nullptr) {
    ImGui::TextDisabled("图块素材缺失");
    return;
  }
  PreviewTexture* pv = AssetPreview(*asset);
  if (pv == nullptr || pv->srv == nullptr) return;
  // 素材可能很大：这里可以滚轮缩放、中键拖动，外面套一层可滚动的子窗口
  static float pal_zoom = 1.0f;
  static ImVec2 pal_pan(0.0f, 0.0f);
  ImGui::BeginChild("##pal_scroll", ImVec2(0.0f, 220.0f), true,
                    ImGuiWindowFlags_HorizontalScrollbar);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float avail_w = ImGui::GetContentRegionAvail().x;
  float base = avail_w / static_cast<float>(pv->width);
  if (base > 2.5f) base = 2.5f;
  float scale = base * pal_zoom;
  const ImVec2 size(static_cast<float>(pv->width) * scale,
                    static_cast<float>(pv->height) * scale);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + pal_pan.x, origin.y + pal_pan.y));
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##scene_sheet", size);
  if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float wx = (m.x - p0.x) / scale;
    const float wy = (m.y - p0.y) / scale;
    pal_zoom = std::max(0.5f, std::min(8.0f, pal_zoom + ImGui::GetIO().MouseWheel * 0.2f));
    scale = base * pal_zoom;
    pal_pan.x = m.x - origin.x - wx * scale;
    pal_pan.y = m.y - origin.y - wy * scale;
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(2)) {
    pal_pan.x += ImGui::GetIO().MouseDelta.x;
    pal_pan.y += ImGui::GetIO().MouseDelta.y;
  }
  if (pal_pan.x > 0.0f) pal_pan.x = 0.0f;
  if (pal_pan.y > 0.0f) pal_pan.y = 0.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddImage(reinterpret_cast<ImTextureID>(pv->srv), p0,
               ImVec2(p0.x + size.x, p0.y + size.y));
  const int tw = std::max(1, ts->tile_w);
  const int th = std::max(1, ts->tile_h);
  const int cols = std::max(1, pv->width / tw);
  const int rows = std::max(1, pv->height / th);
  // 切到另一张图集后笔刷可能落在图外：夹回来，免得落笔写进一个越界索引
  if (g_brush_x < 0) g_brush_x = 0;
  if (g_brush_y < 0) g_brush_y = 0;
  if (g_brush_x >= cols) g_brush_x = cols - 1;
  if (g_brush_y >= rows) g_brush_y = rows - 1;
  if (g_brush_w < 1) g_brush_w = 1;
  if (g_brush_h < 1) g_brush_h = 1;
  if (g_brush_x + g_brush_w > cols) g_brush_w = cols - g_brush_x;
  if (g_brush_y + g_brush_h > rows) g_brush_h = rows - g_brush_y;
  for (int c = 0; c <= cols; ++c) {
    dl->AddLine(ImVec2(p0.x + c * tw * scale, p0.y),
                ImVec2(p0.x + c * tw * scale, p0.y + rows * th * scale),
                IM_COL32(0, 0, 0, 90));
  }
  for (int r = 0; r <= rows; ++r) {
    dl->AddLine(ImVec2(p0.x, p0.y + r * th * scale),
                ImVec2(p0.x + cols * tw * scale, p0.y + r * th * scale),
                IM_COL32(0, 0, 0, 90));
  }
  dl->AddRect(ImVec2(p0.x + g_brush_x * tw * scale, p0.y + g_brush_y * th * scale),
              ImVec2(p0.x + (g_brush_x + g_brush_w) * tw * scale,
                     p0.y + (g_brush_y + g_brush_h) * th * scale),
              IM_COL32(47, 128, 237, 255), 0.0f, 0, 2.5f);
  if (ImGui::IsItemHovered()) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int c = static_cast<int>((m.x - p0.x) / (tw * scale));
    const int r = static_cast<int>((m.y - p0.y) / (th * scale));
    if (c >= 0 && c < cols && r >= 0 && r < rows) {
      if (ImGui::IsMouseClicked(0)) {
        g_brush_dragging = true;
        g_brush_start_x = c;
        g_brush_start_y = r;
        g_brush_x = c;
        g_brush_y = r;
        g_brush_w = 1;
        g_brush_h = 1;
        g_paint_tile = r * cols + c;
      } else if (g_brush_dragging && ImGui::IsMouseDown(0)) {
        g_brush_x = std::min(c, g_brush_start_x);
        g_brush_y = std::min(r, g_brush_start_y);
        g_brush_w = std::abs(c - g_brush_start_x) + 1;
        g_brush_h = std::abs(r - g_brush_start_y) + 1;
        g_paint_tile = g_brush_y * cols + g_brush_x;
      }
      if (ImGui::IsMouseReleased(0)) g_brush_dragging = false;
    }
  }
  ImGui::EndChild();
  ImGui::TextDisabled("笔刷 %d × %d   （滚轮缩放 / 中键拖动）", g_brush_w, g_brush_h);
}

void DrawScenePageLeft() {
  DrawSceneTilesetPalette();
  ImGui::Separator();

  // ★ 场景树 ★
  //   场景 → 图层 / 触发器 / 对象。触发器和对象本来就存在 scene.json 里，
  //   树只是把它们摊开；选中谁，右栏就显示谁的属性。
  if (RowButton("+ 新建")) {
    ImGui::OpenPopup("##scene_tree_new");
  }
  if (ImGui::BeginPopup("##scene_tree_new")) {
    pgmmv::SceneDoc* cur = CurrentScene();
    if (ImGui::Selectable("场景")) {
      int n = 1;
      std::string id;
      for (;;) {
        id = "scene_" + std::to_string(n++);
        if (g_project.FindScene(id) == nullptr) break;
      }
      const std::string ts =
          g_project.tilesets.empty() ? "" : g_project.tilesets.front().id;
      g_project.scenes.push_back(pgmmv::SceneCreateDefault(id, id, ts));
      g_selected_scene = id;
      g_scene_sel_kind = SceneSelKind::kScene;
      MarkDirty();
      UiLogF("scene created count=%d", static_cast<int>(g_project.scenes.size()));
    }
    if (ImGui::Selectable("触发器", false, cur == nullptr ? ImGuiSelectableFlags_Disabled : 0)) {
      if (cur != nullptr) {
        PushUndo();
        pgmmv::TriggerDef t;
        t.x = SceneSpawnX(cur);
        t.y = SceneSpawnY(cur);
        t.w = 64;
        t.h = 64;
        t.name = "触发器 " + std::to_string(cur->triggers.size() + 1);
        const float hue =
            std::fmod(0.6180339887f * static_cast<float>(cur->triggers.size()), 1.0f);
        const ImVec4 c = ImColor::HSV(hue, 0.55f, 0.95f).Value;
        t.color = (static_cast<unsigned>(c.x * 255.0f + 0.5f) << 24) |
                  (static_cast<unsigned>(c.y * 255.0f + 0.5f) << 16) |
                  (static_cast<unsigned>(c.z * 255.0f + 0.5f) << 8) | 0xFFu;
        cur->triggers.push_back(t);
        g_selected_trigger = static_cast<int>(cur->triggers.size()) - 1;
        g_scene_sel_kind = SceneSelKind::kTrigger;
        MarkDirty();
      }
    }
    if (ImGui::Selectable("对象（实例）", false, cur == nullptr ? ImGuiSelectableFlags_Disabled : 0)) {
      if (cur != nullptr) {
        std::string obj_id = g_selected_object_for_place;
        if (obj_id.empty() && !g_project.objects.empty()) {
          obj_id = g_project.objects.front().id;
        }
        if (obj_id.empty()) {
          SetError("先建一个对象（对象页）再往场景里放");
        } else {
          PushUndo();
          cur->AddSceneObject(obj_id, SceneSpawnX(cur), SceneSpawnY(cur));
          g_selected_placed_object = static_cast<int>(cur->objects.size()) - 1;
          g_scene_sel_kind = SceneSelKind::kObject;
          MarkDirty();
        }
      }
    }
    ImGui::EndPopup();
  }

  // 快捷添加：直接挂到当前场景下面（放在起点，之后可在画布上拖/改坐标）
  {
    pgmmv::SceneDoc* cur = CurrentScene();
    ImGui::BeginDisabled(cur == nullptr);
    if (RowButton("+ 触发器")) {
      if (cur != nullptr) {
        PushUndo();
        pgmmv::TriggerDef t;
        t.x = SceneSpawnX(cur) - 32;
        t.y = SceneSpawnY(cur) - 32;
        t.w = 64;
        t.h = 64;
        t.name = "触发器 " + std::to_string(cur->triggers.size() + 1);
        const float hue =
            std::fmod(0.6180339887f * static_cast<float>(cur->triggers.size()), 1.0f);
        const ImVec4 c = ImColor::HSV(hue, 0.55f, 0.95f).Value;
        t.color = (static_cast<unsigned>(c.x * 255.0f + 0.5f) << 24) |
                  (static_cast<unsigned>(c.y * 255.0f + 0.5f) << 16) |
                  (static_cast<unsigned>(c.z * 255.0f + 0.5f) << 8) | 0xFFu;
        cur->triggers.push_back(t);
        g_selected_trigger = static_cast<int>(cur->triggers.size()) - 1;
        g_scene_sel_kind = SceneSelKind::kTrigger;
        g_scene_trigger_mode = true;
        MarkDirty();
      }
    }
    if (RowButton("+ 对象实例")) {
      if (cur != nullptr) {
        std::string obj_id = g_selected_object_for_place;
        if (obj_id.empty() && !g_project.objects.empty()) {
          obj_id = g_project.objects.front().id;
        }
        if (obj_id.empty()) {
          SetError("先建一个对象（对象页）再往场景里放");
        } else {
          PushUndo();
          cur->AddSceneObject(obj_id, SceneSpawnX(cur), SceneSpawnY(cur));
          g_selected_placed_object = static_cast<int>(cur->objects.size()) - 1;
          g_scene_sel_kind = SceneSelKind::kObject;
          MarkDirty();
        }
      }
    }
    ImGui::EndDisabled();
  }

  if (!g_selected_scene.empty() && DangerRowButton("删除选中的场景内容")) {
    pgmmv::SceneDoc* cur = CurrentScene();
    if (g_scene_sel_kind == SceneSelKind::kTrigger && cur != nullptr &&
        g_selected_trigger >= 0 &&
        g_selected_trigger < static_cast<int>(cur->triggers.size())) {
      PushUndo();
      cur->triggers.erase(cur->triggers.begin() + g_selected_trigger);
      g_selected_trigger = -1;
      g_scene_sel_kind = SceneSelKind::kScene;
      MarkDirty();
    } else if (g_scene_sel_kind == SceneSelKind::kObject && cur != nullptr &&
               g_selected_placed_object >= 0 &&
               g_selected_placed_object < static_cast<int>(cur->objects.size())) {
      PushUndo();
      cur->objects.erase(cur->objects.begin() + g_selected_placed_object);
      g_selected_placed_object = -1;
      g_scene_sel_kind = SceneSelKind::kScene;
      MarkDirty();
    } else if (g_scene_sel_kind == SceneSelKind::kLayer) {
      SetError("图层固定三层，删不了");
    } else {
      for (size_t i = 0; i < g_project.scenes.size(); ++i) {
        if (g_project.scenes[i].id == g_selected_scene) {
          PushUndo();
          g_project.scenes.erase(g_project.scenes.begin() + i);
          break;
        }
      }
      g_selected_scene = g_project.scenes.empty() ? "" : g_project.scenes.front().id;
      g_scene_sel_kind = SceneSelKind::kScene;
      MarkDirty();
    }
  }

  ImGui::Separator();
  for (size_t si = 0; si < g_project.scenes.size(); ++si) {
    pgmmv::SceneDoc& sc = g_project.scenes[si];
    const bool cur = (sc.id == g_selected_scene);
    ImGui::PushID(static_cast<int>(si));
    if (cur) {
      const bool open = ImGui::TreeNodeEx(
          sc.name.c_str(),
          ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
      if (ImGui::IsItemClicked()) g_scene_sel_kind = SceneSelKind::kScene;
      if (open) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "图层 (%d)",
                      static_cast<int>(sc.layers.size()));
        if (ImGui::TreeNodeEx(buf, ImGuiTreeNodeFlags_SpanAvailWidth)) {
          for (int li = 0; li < static_cast<int>(sc.layers.size()) && li < 4; ++li) {
            ImGui::PushID(100 + li);
            if (ImGui::Selectable(
                    sc.layers[static_cast<size_t>(li)].name.c_str(),
                    g_scene_sel_kind == SceneSelKind::kLayer &&
                        g_scene_layer == li)) {
              g_scene_layer = li;
              g_scene_sel_kind = SceneSelKind::kLayer;
            }
            ImGui::PopID();
          }
          ImGui::TreePop();
        }
        if (ImGui::Selectable("相机", g_scene_sel_kind == SceneSelKind::kCamera)) {
          g_scene_sel_kind = SceneSelKind::kCamera;
        }
        std::snprintf(buf, sizeof(buf), "触发器 (%d)",
                      static_cast<int>(sc.triggers.size()));
        if (ImGui::TreeNodeEx(buf, ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen)) {
          for (size_t ti = 0; ti < sc.triggers.size(); ++ti) {
            ImGui::PushID(200 + static_cast<int>(ti));
            const std::string nm = sc.triggers[ti].name.empty()
                                       ? ("触发器 " + std::to_string(ti + 1))
                                       : sc.triggers[ti].name;
            if (ImGui::Selectable(
                    nm.c_str(),
                    g_scene_sel_kind == SceneSelKind::kTrigger &&
                        g_selected_trigger == static_cast<int>(ti))) {
              g_selected_trigger = static_cast<int>(ti);
              g_scene_sel_kind = SceneSelKind::kTrigger;
              g_scene_trigger_mode = true;
            }
            ImGui::PopID();
          }
          ImGui::TreePop();
        }
        std::snprintf(buf, sizeof(buf), "实例 (%d)",
                      static_cast<int>(sc.objects.size()));
        if (ImGui::TreeNodeEx(buf, ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen)) {
          for (size_t oi = 0; oi < sc.objects.size(); ++oi) {
            ImGui::PushID(300 + static_cast<int>(oi));
            const pgmmv::SceneObject& inst = sc.objects[oi];
            const pgmmv::ObjectDoc* od = g_project.FindObject(inst.object);
            const std::string nm = od != nullptr ? od->name : inst.id;
            char ibuf[192];
            std::snprintf(ibuf, sizeof(ibuf), "%s  (%d,%d)", nm.c_str(), inst.x, inst.y);
            if (ImGui::Selectable(
                    ibuf,
                    g_scene_sel_kind == SceneSelKind::kObject &&
                        g_selected_placed_object == static_cast<int>(oi))) {
              g_selected_placed_object = static_cast<int>(oi);
              g_scene_sel_kind = SceneSelKind::kObject;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
              g_scene_focus_pending = true;
              g_scene_focus_x = static_cast<float>(inst.x);
              g_scene_focus_y = static_cast<float>(inst.y) +
                                (od != nullptr ? static_cast<float>(od->frame.h) * 0.5f
                                               : 16.0f);
            }
            if (ImGui::IsItemHovered()) {
              ImGui::SetTooltip("实例 %s", inst.id.c_str());
            }
            if (ImGui::BeginPopupContextItem("##inst_ctx")) {
              if (ImGui::MenuItem("聚焦")) {
                g_scene_focus_pending = true;
                g_scene_focus_x = static_cast<float>(inst.x);
                g_scene_focus_y = static_cast<float>(inst.y) +
                                  (od != nullptr ? static_cast<float>(od->frame.h) * 0.5f
                                                 : 16.0f);
                g_selected_placed_object = static_cast<int>(oi);
                g_scene_sel_kind = SceneSelKind::kObject;
              }
              if (ImGui::MenuItem("复制")) {
                PushUndo();
                sc.AddSceneObject(inst.object, inst.x + 16, inst.y + 16);
                g_selected_placed_object = static_cast<int>(sc.objects.size()) - 1;
                g_scene_sel_kind = SceneSelKind::kObject;
                MarkDirty();
                ImGui::EndPopup();
                ImGui::PopID();
                continue;
              }
              if (ImGui::MenuItem("删除")) {
                PushUndo();
                sc.objects.erase(sc.objects.begin() + static_cast<long>(oi));
                if (g_selected_placed_object == static_cast<int>(oi)) {
                  g_selected_placed_object = -1;
                  g_scene_sel_kind = SceneSelKind::kScene;
                } else if (g_selected_placed_object > static_cast<int>(oi)) {
                  --g_selected_placed_object;
                }
                MarkDirty();
                ImGui::EndPopup();
                ImGui::PopID();
                break;
              }
              ImGui::EndPopup();
            }
            ImGui::PopID();
          }
          ImGui::TreePop();
        }
        ImGui::TreePop();
      }
    } else {
      if (ImGui::Selectable(sc.name.c_str(), false)) {
        g_selected_scene = sc.id;
        g_scene_sel_kind = SceneSelKind::kScene;
        g_selected_trigger = -1;
        g_selected_placed_object = -1;
      }
    }
    ImGui::PopID();
  }
  if (g_project.scenes.empty()) ImGui::TextDisabled("没有场景");
}

// 出生点 = 场景里玩家对象实例的位置（场景不再单独存起点）
const pgmmv::SceneObject* SceneSpawnObject(pgmmv::SceneDoc* s) {
  if (s == nullptr) return nullptr;
  for (const std::string& id : s->controlled) {
    if (const pgmmv::SceneObject* o = s->FindSceneObject(id)) return o;
  }
  for (const pgmmv::SceneObject& o : s->objects) {
    for (const pgmmv::ObjectDoc& od : g_project.objects) {
      if (od.id == o.object && od.type == "player") return &o;
    }
  }
  if (!s->objects.empty()) return &s->objects.front();
  return nullptr;
}

int SceneSpawnX(pgmmv::SceneDoc* s) {
  const pgmmv::SceneObject* o = SceneSpawnObject(s);
  return o != nullptr ? o->x : 64;
}

int SceneSpawnY(pgmmv::SceneDoc* s) {
  const pgmmv::SceneObject* o = SceneSpawnObject(s);
  return o != nullptr ? o->y : 80;
}

void DrawScenePageCenter() {
  pgmmv::SceneDoc* s = CurrentScene();
  if (!g_has_project || s == nullptr) {
    ImGui::TextDisabled("选择或新建一个场景");
    return;
  }
  if (g_scene_layer < 0) g_scene_layer = 0;
  if (g_scene_layer > 3) g_scene_layer = 3;
  pgmmv::SceneLayer& layer = s->layers[g_scene_layer];
  EnsureSceneTilesets(s);
  ClampPaintSlot(*s);

  // 网格尺寸：沿用图层自己的图块组（旧字段）；没有就退回场景第一个图集。
  const pgmmv::TilesetDoc* grid_ts = nullptr;
  if (!s->tilesets.empty()) {
    grid_ts = g_project.FindTileset(s->tilesets.front());
  }
  const int tw = grid_ts != nullptr ? std::max(1, grid_ts->tile_w) : 16;
  const int th = grid_ts != nullptr ? std::max(1, grid_ts->tile_h) : 16;

  // ★ 每个槽位一张纹理：格子记录自己的槽位，画布按槽位取图 ★
  struct SlotView {
    PreviewTexture* pv;
    int tw;
    int th;
    int cols;
  };
  std::vector<SlotView> views(s->tilesets.size(), SlotView{nullptr, tw, th, 1});
  for (size_t i = 0; i < s->tilesets.size(); ++i) {
    const pgmmv::TilesetDoc* t = g_project.FindTileset(s->tilesets[i]);
    if (t == nullptr) continue;
    const pgmmv::Asset* a = g_project.FindAssetByPath(t->image);
    PreviewTexture* p = a != nullptr ? AssetPreview(*a) : nullptr;
    views[i].pv = p;
    views[i].tw = std::max(1, t->tile_w);
    views[i].th = std::max(1, t->tile_h);
    views[i].cols = p != nullptr ? std::max(1, p->width / views[i].tw) : 1;
  }
  // 画笔图集（调色板选中的槽位）：落笔用的列数要按它自己算。
  const pgmmv::TilesetDoc* paint_ts = TilesetAtSlotC(*s, g_paint_slot);
  PreviewTexture* paint_pv = nullptr;
  if (paint_ts != nullptr) {
    const pgmmv::Asset* pa = g_project.FindAssetByPath(paint_ts->image);
    paint_pv = pa != nullptr ? AssetPreview(*pa) : nullptr;
  }
  const int paint_cols =
      paint_pv != nullptr ? std::max(1, paint_pv->width / std::max(1, paint_ts->tile_w)) : 1;

  float& zoom = g_scene_zoom;
  ImVec2& pan = g_scene_pan;
  ImGui::SetNextItemWidth(120.0f);
  ImGui::SliderFloat("##scene_zoom", &zoom, 0.1f, 5.0f, "%.2fx");
  ImGui::SameLine();
  if (InlineButton("适配", 64)) g_scene_fit_requested = true;
  ImGui::SameLine();
  ImGui::TextDisabled("左键画 / 右键擦 / 图块 %d:%d", g_paint_slot, g_paint_tile);
  ImGui::SameLine();
  ImGui::Checkbox("块线", &g_show_chunk_grid);
  // ★ 图层切换放在画布上方（照 PGMMV：图层标签在画布顶上，不在右栏）★
  {
    static const char* kNames[4] = {"HUD", "前景", "主层", "背景"};
    const int layer_n = std::min(4, static_cast<int>(s->layers.size()));
    for (int i = 0; i < layer_n; ++i) {
      const char* nm = s->layers[i].name.empty() ? kNames[i] : s->layers[i].name.c_str();
      char label[64];
      std::snprintf(label, sizeof(label), "%s%s%s%s", s->layers[i].locked ? "[锁] " : "",
                    s->layers[i].enabled ? "" : "[关] ", nm,
                    (g_scene_layer == i) ? " *" : "");
      if (i == 0) ImGui::TextUnformatted("图层");
      ImGui::SameLine();
      // ★ 用点击前的状态决定是否 Pop ★
      //   直接写 if (g_scene_layer == i) Pop 会有两个问题：点了之后 g_scene_layer 变了，
      //   push 和 pop 就对不上 → ImGui 样式栈失衡（点一下就报错）。
      const bool active = (g_scene_layer == i);
      if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.45f, 0.72f, 1.0f));
      if (ImGui::Button(label, ImVec2(86.0f, 24.0f))) {
        g_scene_layer = i;
        g_scene_sel_kind = SceneSelKind::kLayer;
        UiLogF("layer click %d", i);
      }
      if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine(0.0f, 18.0f);
    if (ImGui::Checkbox("显示", &layer.visible)) MarkDirty();
    ImGui::SameLine();
    if (ImGui::Checkbox("锁定", &layer.locked)) MarkDirty();
    ImGui::SameLine(0.0f, 18.0f);
    if (ImGui::Checkbox("碰撞层", &g_scene_collision_mode)) {
      UiLogF("collision mode %d", g_scene_collision_mode ? 1 : 0);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("触发层", &g_scene_trigger_mode)) {
      if (g_scene_trigger_mode) g_scene_collision_mode = false;
    }
  }
  // ★ 碰撞笔：逐边选择要写的墙（bit0 上 / bit1 右 / bit2 下 / bit3 左）★
  //   以前只会写 15（四边全墙），没法做单向平台/半格墙。
  if (g_scene_collision_mode) {
    ImGui::TextUnformatted("碰撞笔");
    static const char* kEdges[4] = {"上", "右", "下", "左"};
    for (int i = 0; i < 4; ++i) {
      ImGui::SameLine();
      bool on = ((g_collision_mask >> i) & 1) != 0;
      if (ImGui::Checkbox(kEdges[i], &on)) {
        if (on) g_collision_mask |= (1 << i);
        else g_collision_mask &= ~(1 << i);
      }
    }
    ImGui::SameLine(0.0f, 16.0f);
    if (InlineButton("全墙", 64)) g_collision_mask = 15;
    ImGui::SameLine();
    if (InlineButton("清空笔", 64)) g_collision_mask = 0;
    ImGui::SameLine();
    ImGui::TextDisabled("左键写笔头 · 右键擦");
  }
  const float cell = static_cast<float>(tw) * zoom;
  // ★ 无限画布 = 视口本身就是画布 ★
  //   不再维护一块"固定大小的矩形"（那样迟早会看到边，而且边外面画不到）。
  //   世界坐标直接由 pan/zoom 映射到屏幕；画到哪，数据层就在哪生成 chunk。
  const ImVec2 vp = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 vsz(std::max(160.0f, avail.x), std::max(120.0f, avail.y));
  // 适配内容：场景切换/工程打开/点“适配”时执行一次
  if (g_scene_origin_view_requested) {
    g_scene_origin_view_requested = false;
    // 默认让相机框左下角落在原点附近，而不是自动把内容居中。
    pan = ImVec2(40.0f, vsz.y - 40.0f);
  }
  if (g_scene_fit_requested) {
    g_scene_fit_requested = false;
    int minc = 0, minr = 0, maxc = 0, maxr = 0;
    const bool any = s->ContentBounds(&minc, &minr, &maxc, &maxr);
    float min_x = any ? static_cast<float>(minc * pgmmv::kSceneBase) : 0.0f;
    float min_y = any ? static_cast<float>(minr * pgmmv::kSceneBase) : 0.0f;
    float max_x = any ? static_cast<float>((maxc + 1) * pgmmv::kSceneBase)
                      : static_cast<float>(pgmmv::kScreenWidth);
    float max_y = any ? static_cast<float>((maxr + 1) * pgmmv::kSceneBase)
                      : static_cast<float>(pgmmv::kScreenHeight);
    for (const pgmmv::SceneObject& o : s->objects) {
      const pgmmv::ObjectDoc* od = g_project.FindObject(o.object);
      const float fw = od != nullptr ? static_cast<float>(std::max(1, od->frame.w)) : 32.0f;
      const float fh = od != nullptr ? static_cast<float>(std::max(1, od->frame.h)) : 32.0f;
      min_x = std::min(min_x, static_cast<float>(o.x));
      min_y = std::min(min_y, static_cast<float>(o.y));
      max_x = std::max(max_x, static_cast<float>(o.x) + fw);
      max_y = std::max(max_y, static_cast<float>(o.y) + fh);
    }
    for (const pgmmv::TriggerDef& t : s->triggers) {
      min_x = std::min(min_x, static_cast<float>(t.x));
      min_y = std::min(min_y, static_cast<float>(t.y));
      max_x = std::max(max_x, static_cast<float>(t.x + t.w));
      max_y = std::max(max_y, static_cast<float>(t.y + t.h));
    }
    const float w = std::max(8.0f, max_x - min_x);
    const float h = std::max(8.0f, max_y - min_y);
    const float margin = 24.0f;
    zoom = std::clamp(std::min((vsz.x - margin * 2.0f) / w,
                               (vsz.y - margin * 2.0f) / h),
                      0.1f, 5.0f);
    const float cx = (min_x + max_x) * 0.5f;
    const float cy = (min_y + max_y) * 0.5f;
    pan = ImVec2(vsz.x * 0.5f - cx * zoom, vsz.y * 0.5f + cy * zoom);
  }
  if (g_scene_focus_pending) {
    g_scene_focus_pending = false;
    g_scene_fit_requested = false;
    pan = ImVec2(vsz.x * 0.5f - g_scene_focus_x * zoom,
                 vsz.y * 0.5f + g_scene_focus_y * zoom);
  }
  const float panx = pan.x;
  const float pany = pan.y;
  // 屏幕 ← 世界（y 向上）：点用 world_left/world_top(_,0)，矩形用 world_top(y,h)
  auto world_left = [&](float x) { return vp.x + panx + x * zoom; };
  auto world_top = [&](float y, float h) { return vp.y + pany - (y + h) * zoom; };
  auto wx_at = [&](float mx) { return (mx - vp.x - panx) / zoom; };
  auto wy_at = [&](float my) { return (vp.y + pany - my) / zoom; };
  auto col_at = [&](float mx) { return static_cast<int>(std::floor(wx_at(mx) / static_cast<float>(tw))); };
  auto row_at = [&](float my) { return static_cast<int>(std::floor(wy_at(my) / static_cast<float>(th))); };
  auto cell_top = [&](int r) { return world_top(static_cast<float>(r * th), static_cast<float>(th)); };
  // 可见格范围（多画一圈，滚动时不露边）
  const int cL = col_at(vp.x) - 1;
  const int cR = col_at(vp.x + vsz.x) + 1;
  const int rT = row_at(vp.y) + 1;
  const int rB = row_at(vp.y + vsz.y) - 1;
  ImGui::SetCursorScreenPos(vp);
  ImGui::InvisibleButton("##scene_canvas", vsz);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(vp, ImVec2(vp.x + vsz.x, vp.y + vsz.y), IM_COL32(58, 58, 58, 255));
  // ★ 按可见 chunk 裁剪：先定位 chunk，再遍历 chunk 内格子 ★
  //   旧实现每个可见格都调一次 TileAt（低倍率下 16 万次/帧）；现在只有几百次 chunk 查找。
  const int kChunk = pgmmv::kSceneChunkCells;
  auto floor_div = [](int a, int b) {
    int q = a / b;
    if ((a % b) != 0 && ((a < 0) != (b < 0))) --q;
    return q;
  };
  const int cc0 = floor_div(cL, kChunk);
  const int cc1 = floor_div(cR, kChunk);
  const int cr0 = floor_div(rB, kChunk);
  const int cr1 = floor_div(rT, kChunk);
  for (int cy = cr0; cy <= cr1; ++cy) {
    for (int cx = cc0; cx <= cc1; ++cx) {
      const pgmmv::SceneChunk* chunk = s->FindChunk(g_scene_layer, cx, cy);
      if (chunk == nullptr || chunk->cells.size() < static_cast<size_t>(kChunk * kChunk)) {
        continue;
      }
      for (int ly = 0; ly < kChunk; ++ly) {
        const int r = cy * kChunk + ly;
        if (r < rB || r > rT) continue;
        for (int lx = 0; lx < kChunk; ++lx) {
          const int c = cx * kChunk + lx;
          if (c < cL || c > cR) continue;
          const int tile = chunk->cells[static_cast<size_t>(ly) * kChunk + lx];
          if (tile < 0) continue;
          const int cell_slot = pgmmv::CellSlot(tile);
          if (cell_slot < 0 || cell_slot >= static_cast<int>(views.size())) continue;
          const SlotView& v = views[static_cast<size_t>(cell_slot)];
          if (v.pv == nullptr || v.pv->srv == nullptr) continue;
          const int idx = pgmmv::CellIndex(tile);
          const int sx = (idx % v.cols) * v.tw;
          const int sy = (idx / v.cols) * v.th;
          const ImVec2 a(world_left(static_cast<float>(c * tw)), cell_top(r));
          const ImVec2 b(a.x + cell, a.y + cell);
          dl->AddImage(reinterpret_cast<ImTextureID>(v.pv->srv), a, b,
                       ImVec2(sx / static_cast<float>(v.pv->width), sy / static_cast<float>(v.pv->height)),
                       ImVec2((sx + v.tw) / static_cast<float>(v.pv->width),
                              (sy + v.th) / static_cast<float>(v.pv->height)));
        }
      }
    }
  }
  for (int c = cL; c <= cR; ++c) {
    const float gx = world_left(static_cast<float>(c * tw));
    dl->AddLine(ImVec2(gx, vp.y), ImVec2(gx, vp.y + vsz.y), IM_COL32(0, 0, 0, 60));
  }
  for (int r = rB; r <= rT; ++r) {
    // ★ 线在"行的底边"（world y = r*th）★
    //   用 cell_top(r) 是行的顶边，整组横线会差一格 —— 相机框左下角就压不到网格交点上了。
    const float gy = world_top(static_cast<float>(r * th), 0.0f);
    dl->AddLine(ImVec2(vp.x, gy), ImVec2(vp.x + vsz.x, gy), IM_COL32(0, 0, 0, 60));
  }
  // ★ 16×16 chunk 边界（128px）★ —— 是存储块边界，不是世界边界（世界无边界）
  if (g_show_chunk_grid) {
    for (int c = cL; c <= cR; ++c) {
      if ((c % pgmmv::kSceneChunkCells) != 0) continue;
      const float x = world_left(static_cast<float>(c * tw));
      dl->AddLine(ImVec2(x, vp.y), ImVec2(x, vp.y + vsz.y), IM_COL32(120, 200, 255, 90), 1.0f);
    }
    for (int r = rB; r <= rT; ++r) {
      if ((r % pgmmv::kSceneChunkCells) != 0) continue;
      const float y = world_top(static_cast<float>(r * th), 0.0f);
      dl->AddLine(ImVec2(vp.x, y), ImVec2(vp.x + vsz.x, y), IM_COL32(120, 200, 255, 90), 1.0f);
    }
  }
  // ---- 坐标轴：世界 y 向上，x 向右；让作者能直接判断方向 ----
  {
    const ImU32 col_x = IM_COL32(235, 110, 100, 190);
    const ImU32 col_y = IM_COL32(105, 215, 135, 190);
    const float ax = world_left(0.0f);
    const float ay = world_top(0.0f, 0.0f);
    if (ay >= vp.y && ay <= vp.y + vsz.y) {
      dl->AddLine(ImVec2(vp.x, ay), ImVec2(vp.x + vsz.x, ay), col_x, 1.5f);
      dl->AddText(ImVec2(vp.x + vsz.x - 34.0f, ay - 18.0f), col_x, "X+");
    }
    if (ax >= vp.x && ax <= vp.x + vsz.x) {
      dl->AddLine(ImVec2(ax, vp.y), ImVec2(ax, vp.y + vsz.y), col_y, 1.5f);
      dl->AddText(ImVec2(ax + 6.0f, vp.y + 8.0f), col_y, "Y+");
    }
    if (ax >= vp.x && ax <= vp.x + vsz.x && ay >= vp.y && ay <= vp.y + vsz.y) {
      dl->AddCircleFilled(ImVec2(ax, ay), 4.0f, IM_COL32(255, 255, 255, 220));
      dl->AddText(ImVec2(ax + 7.0f, ay + 5.0f), IM_COL32(220, 220, 220, 220), "(0,0)");
    }
    // 固定方向罗盘
    const ImVec2 g(vp.x + 30.0f, vp.y + 54.0f);
    dl->AddLine(g, ImVec2(g.x + 28.0f, g.y), col_x, 2.5f);
    dl->AddTriangleFilled(ImVec2(g.x + 28, g.y), ImVec2(g.x + 20, g.y - 5),
                          ImVec2(g.x + 20, g.y + 5), col_x);
    dl->AddText(ImVec2(g.x + 34.0f, g.y - 8.0f), col_x, "X");
    dl->AddLine(g, ImVec2(g.x, g.y - 28.0f), col_y, 2.5f);
    dl->AddTriangleFilled(ImVec2(g.x, g.y - 28), ImVec2(g.x - 5, g.y - 20),
                          ImVec2(g.x + 5, g.y - 20), col_y);
    dl->AddText(ImVec2(g.x + 7.0f, g.y - 44.0f), col_y, "Y");
  }
  // 鼠标坐标读数
  {
    const ImVec2 m = ImGui::GetIO().MousePos;
    if (m.x >= vp.x && m.x <= vp.x + vsz.x && m.y >= vp.y && m.y <= vp.y + vsz.y) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "X %d   Y %d", static_cast<int>(std::floor(wx_at(m.x))),
                    static_cast<int>(std::floor(wy_at(m.y))));
      const ImVec2 tp(vp.x + 14.0f, vp.y + vsz.y - 30.0f);
      dl->AddRectFilled(ImVec2(tp.x - 6, tp.y - 4), ImVec2(tp.x + 150, tp.y + 20),
                        IM_COL32(20, 20, 24, 205), 3.0f);
      dl->AddText(tp, IM_COL32(225, 230, 240, 255), buf);
    }
  }
  auto object_box = [&](const pgmmv::SceneObject& o, ImVec2* a, ImVec2* b) {
    const pgmmv::ObjectDoc* od = g_project.FindObject(o.object);
    const float fw = od != nullptr ? static_cast<float>(std::max(1, od->frame.w)) : 20.0f;
    const float fh = od != nullptr ? static_cast<float>(std::max(1, od->frame.h)) : 20.0f;
    int ox = static_cast<int>(fw * 0.5f);
    int oy = static_cast<int>(fh);
    if (od != nullptr) {
      const pgmmv::ActionDef* act =
          pgmmv::FindObjectAction(g_project, *od, od->default_action);
      if (act == nullptr) {
        const std::vector<pgmmv::ActionDef>* acts =
            pgmmv::ObjectActions(g_project, *od);
        if (acts != nullptr && !acts->empty()) act = &acts->front();
      }
      if (act != nullptr && !act->frames.empty()) {
        pgmmv::ResolveFrameOrigin(*act, 0, &ox, &oy);
      }
    }
    // 场景 y 向上；实例坐标是“脚底原点”，不是帧左上。
    *a = ImVec2(world_left(static_cast<float>(o.x - ox)),
                world_top(static_cast<float>(o.y + oy - static_cast<int>(fh)), fh));
    *b = ImVec2(a->x + fw * zoom, a->y + fh * zoom);
  };
  for (size_t i = 0; i < s->objects.size(); ++i) {
    const pgmmv::SceneObject& o = s->objects[i];
    ImVec2 a, b;
    object_box(o, &a, &b);
    const bool sel = (g_selected_placed_object == static_cast<int>(i));
    dl->AddRect(a, b,
                sel ? IM_COL32(47, 128, 237, 255) : IM_COL32(90, 200, 120, 255), 0.0f, 0,
                sel ? 3.0f : 1.0f);
  }
  if (!g_scene_place_mode && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    g_selected_placed_object = -1;
    for (size_t i = 0; i < s->objects.size(); ++i) {
      const pgmmv::SceneObject& o = s->objects[i];
      ImVec2 a, b;
      object_box(o, &a, &b);
      if (m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) {
        g_selected_placed_object = static_cast<int>(i);
        break;
      }
    }
    // 画布点选 → 右栏跟着切（点空地 = 回到场景属性）
    if (g_selected_placed_object >= 0) {
      g_scene_sel_kind = SceneSelKind::kObject;
    } else if (!g_scene_trigger_mode) {
      g_scene_sel_kind = SceneSelKind::kScene;
    }
  }
  // ★ 拖动对象：累积浮点偏移，避免整数截断导致慢速拖动丢步 ★
  {
    static ImVec2 drag_accum(0.0f, 0.0f);
    static bool drag_undo = false;
    const bool pressed_here =
        ImGui::IsItemHovered() && ImGui::IsMouseClicked(0);
    if (pressed_here) {
      drag_accum = ImVec2(0.0f, 0.0f);
      drag_undo = false;
    }
    if (g_selected_placed_object >= 0 &&
        g_selected_placed_object < static_cast<int>(s->objects.size()) &&
        ImGui::IsItemHovered() && ImGui::IsMouseDragging(0)) {
      if (!drag_undo) {
        PushUndo();
        drag_undo = true;
      }
      drag_accum.x += ImGui::GetIO().MouseDelta.x / zoom;
      drag_accum.y -= ImGui::GetIO().MouseDelta.y / zoom;  // y 向上
      const int dx = static_cast<int>(drag_accum.x);
      const int dy = static_cast<int>(drag_accum.y);
      if (dx != 0 || dy != 0) {
        pgmmv::SceneObject& o = s->objects[g_selected_placed_object];
        o.x += dx;
        o.y += dy;
        drag_accum.x -= static_cast<float>(dx);
        drag_accum.y -= static_cast<float>(dy);
        MarkDirty();
      }
    }
  }

  // ★ 触发层：矩形触发器，右键拖新建，左键点选 ★
  if (g_scene_trigger_mode) {
    ImDrawList* tdl = dl;
    for (size_t ti = 0; ti < s->triggers.size(); ++ti) {
      const pgmmv::TriggerDef& t = s->triggers[ti];
      const float tx0 = world_left(static_cast<float>(t.x));
      const float ty0 = world_top(static_cast<float>(t.y + t.h), 0);
      const float tx1 = world_left(static_cast<float>(t.x + t.w));
      const float ty1 = world_top(static_cast<float>(t.y), 0);
      const ImU32 rr = (t.color >> 24) & 0xFFu;
      const ImU32 gg = (t.color >> 16) & 0xFFu;
      const ImU32 bb = (t.color >> 8) & 0xFFu;
      tdl->AddRectFilled(ImVec2(tx0, ty0), ImVec2(tx1, ty1), IM_COL32(rr, gg, bb, 44));
      tdl->AddRect(ImVec2(tx0, ty0), ImVec2(tx1, ty1), IM_COL32(rr, gg, bb, 255), 0.0f, 0,
                   (static_cast<int>(ti) == g_selected_trigger) ? 2.5f : 1.5f);
      if (!t.name.empty()) {
        tdl->AddText(ImVec2(tx0 + 3.0f, ty0 + 2.0f), IM_COL32(rr, gg, bb, 255), t.name.c_str());
      }
    }
    static bool trig_drag = false;
    static ImVec2 trig_start;
    const ImVec2 tm = ImGui::GetIO().MousePos;
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
      trig_drag = true;
      trig_start = ImVec2(std::floor(wx_at(tm.x) / 8.0f) * 8.0f,
                          std::floor(wy_at(tm.y) / 8.0f) * 8.0f);
      PushUndo();
    }
    if (trig_drag) {
      const float cx = std::floor(wx_at(tm.x) / 8.0f) * 8.0f;
      const float cy = std::floor(wy_at(tm.y) / 8.0f) * 8.0f;
      const ImVec2 a(world_left(std::min(trig_start.x, cx)), world_top(std::max(trig_start.y, cy), 0));
      const ImVec2 b(world_left(std::max(trig_start.x, cx)), world_top(std::min(trig_start.y, cy), 0));
      dl->AddRect(a, b, IM_COL32(255, 240, 140, 255), 0.0f, 0, 1.5f);
      if (!ImGui::IsMouseDown(1)) {
        trig_drag = false;
        pgmmv::TriggerDef t;
        t.x = static_cast<int>(std::min(trig_start.x, cx));
        t.y = static_cast<int>(std::min(trig_start.y, cy));
        t.w = static_cast<int>(std::fabs(cx - trig_start.x));
        t.h = static_cast<int>(std::fabs(cy - trig_start.y));
        if (t.w >= 8 && t.h >= 8) {
          t.name = "触发器 " + std::to_string(s->triggers.size() + 1);
          // 每个触发器颜色不同：按黄金角散开
          const float hue = std::fmod(0.6180339887f * static_cast<float>(s->triggers.size()), 1.0f);
          const ImVec4 c = ImColor::HSV(hue, 0.55f, 0.95f).Value;
          t.color = (static_cast<unsigned>(c.x * 255.0f + 0.5f) << 24) |
                    (static_cast<unsigned>(c.y * 255.0f + 0.5f) << 16) |
                    (static_cast<unsigned>(c.z * 255.0f + 0.5f) << 8) | 0xFFu;
          s->triggers.push_back(t);
          g_selected_trigger = static_cast<int>(s->triggers.size()) - 1;
          g_scene_sel_kind = SceneSelKind::kTrigger;
          MarkDirty();
        }
      }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
      g_selected_trigger = -1;
      const float wx = wx_at(tm.x);
      const float wy = wy_at(tm.y);
      for (int ti = static_cast<int>(s->triggers.size()) - 1; ti >= 0; --ti) {
        const pgmmv::TriggerDef& t = s->triggers[static_cast<size_t>(ti)];
        if (wx >= t.x && wx <= t.x + t.w && wy >= t.y && wy <= t.y + t.h) {
          g_selected_trigger = ti;
          break;
        }
      }
      g_scene_sel_kind = g_selected_trigger >= 0 ? SceneSelKind::kTrigger
                                                 : SceneSelKind::kScene;
    }
    // ★ 拖动触发器：左键按住已选中的触发器移动（右键仍是"新建"）★
    {
      static ImVec2 taccum(0.0f, 0.0f);
      static bool tundo = false;
      if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        taccum = ImVec2(0.0f, 0.0f);
        tundo = false;
      }
      if (g_selected_trigger >= 0 &&
          g_selected_trigger < static_cast<int>(s->triggers.size()) &&
          ImGui::IsItemHovered() && ImGui::IsMouseDragging(0)) {
        if (!tundo) {
          PushUndo();
          tundo = true;
        }
        taccum.x += ImGui::GetIO().MouseDelta.x / zoom;
        taccum.y -= ImGui::GetIO().MouseDelta.y / zoom;
        const int dx = static_cast<int>(taccum.x);
        const int dy = static_cast<int>(taccum.y);
        if (dx != 0 || dy != 0) {
          pgmmv::TriggerDef& t = s->triggers[static_cast<size_t>(g_selected_trigger)];
          t.x += dx;
          t.y += dy;
          taccum.x -= static_cast<float>(dx);
          taccum.y -= static_cast<float>(dy);
          MarkDirty();
        }
      }
    }
  }
  if (g_scene_collision_mode) {
    for (int r = rB; r <= rT; ++r) {
      for (int c = cL; c <= cR; ++c) {
        const int m = s->collision.MaskAt(c, r);
        if (m != 0) {
          const ImVec2 a(world_left(static_cast<float>(c * tw)), cell_top(r));
          const ImVec2 b(a.x + cell, a.y + cell);
          dl->AddRectFilled(a, b, IM_COL32(220, 60, 60, 40));  // 淡底：这一格有碰撞
          const ImU32 red = IM_COL32(235, 72, 72, 255);
          if (m & 1) dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, a.y), red, 2.0f);  // 上
          if (m & 2) dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, b.y), red, 2.0f);  // 右
          if (m & 4) dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), red, 2.0f);  // 下
          if (m & 8) dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, b.y), red, 2.0f);  // 左
        }
      }
    }
  }
  if (g_scene_collision_mode && ImGui::IsItemHovered() &&
      (ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1))) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int c = col_at(m.x);
    const int r = row_at(m.y);
    if (c >= cL && c <= cR && r >= rB && r <= rT) {
      // 一笔一次撤销：按下的那一帧存快照，拖动过程中不再存
      if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) PushUndo();
      const int mask = ImGui::IsMouseDown(1) ? 0 : (g_collision_mask & 15);
      const int wc = c, wr = r;
      UiLogF("collision paint c=%d r=%d mask=%d", wc, wr, mask);
      if (s->collision.MaskAt(wc, wr) != mask) {
        s->collision.SetMaskAt(wc, wr, mask);
        MarkDirty();
      }
    }
  }
  if (!g_scene_collision_mode && g_scene_place_trigger && ImGui::IsItemHovered() &&
      ImGui::IsMouseClicked(0)) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int px = static_cast<int>(wx_at(m.x));
    const int py = static_cast<int>(wy_at(m.y));
    PushUndo();
    pgmmv::TriggerDef t;
    t.x = px - 32;
    t.y = py - 32;
    t.w = 64;
    t.h = 64;
    t.name = "触发器 " + std::to_string(s->triggers.size() + 1);
    const float hue =
        std::fmod(0.6180339887f * static_cast<float>(s->triggers.size()), 1.0f);
    const ImVec4 c = ImColor::HSV(hue, 0.55f, 0.95f).Value;
    t.color = (static_cast<unsigned>(c.x * 255.0f + 0.5f) << 24) |
              (static_cast<unsigned>(c.y * 255.0f + 0.5f) << 16) |
              (static_cast<unsigned>(c.z * 255.0f + 0.5f) << 8) | 0xFFu;
    s->triggers.push_back(t);
    g_selected_trigger = static_cast<int>(s->triggers.size()) - 1;
    g_scene_sel_kind = SceneSelKind::kTrigger;
    g_scene_trigger_mode = true;
    MarkDirty();
    UiLogF("placed trigger %d %d count=%d", t.x, t.y,
           static_cast<int>(s->triggers.size()));
  }
  if (!g_scene_collision_mode && g_scene_place_mode && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int px = static_cast<int>(wx_at(m.x));
    const int py = static_cast<int>(wy_at(m.y));
    if (!g_selected_object_for_place.empty()) {
      PushUndo();
      s->AddSceneObject(g_selected_object_for_place, px, py);
      g_selected_placed_object = static_cast<int>(s->objects.size()) - 1;
      g_scene_sel_kind = SceneSelKind::kObject;
      MarkDirty();
      UiLogF("placed object %d %d count=%d", px, py,
             static_cast<int>(s->objects.size()));
    }
  }
  {
    const float tw_px = static_cast<float>(pgmmv::kScreenWidth);
    const float th_px = static_cast<float>(pgmmv::kScreenHeight);
    const bool cam_follow = g_camera_frame_follow_spawn && s->camera_mode == "follow";
    const float frame_x = cam_follow ? static_cast<float>(SceneSpawnX(s)) - tw_px * 0.5f : 0.0f;
    const float frame_y = cam_follow ? static_cast<float>(SceneSpawnY(s)) - th_px * 0.5f : 0.0f;
    dl->AddRect(ImVec2(world_left(frame_x), world_top(frame_y + th_px, 0.0f)),
                ImVec2(world_left(frame_x + tw_px), world_top(frame_y, 0.0f)),
                IM_COL32(80, 160, 255, 200), 0.0f, 0, 2.0f);
    // ★ 相机死区可视化：画在视口中央（相机不动时角色在这个框里不推镜头）★
    if (s->camera.deadzone_w > 0 || s->camera.deadzone_h > 0) {
      const float dw = static_cast<float>(std::max(0, s->camera.deadzone_w));
      const float dh = static_cast<float>(std::max(0, s->camera.deadzone_h));
      const float cx = frame_x + tw_px * 0.5f;
      const float cy = frame_y + th_px * 0.5f;
      const ImVec2 a(world_left(cx - dw * 0.5f), world_top(cy + dh * 0.5f, 0.0f));
      const ImVec2 b(world_left(cx + dw * 0.5f), world_top(cy - dh * 0.5f, 0.0f));
      dl->AddRect(a, b, IM_COL32(255, 150, 60, 220), 0.0f, 0, 1.5f);
      char dz[64];
      std::snprintf(dz, sizeof(dz), "死区 %dx%d", s->camera.deadzone_w,
                    s->camera.deadzone_h);
      dl->AddText(ImVec2(a.x + 4.0f, a.y + 3.0f), IM_COL32(255, 180, 110, 230), dz);
    }
    // ★ 原点 (0,0) = 相機框左下角：绿色十字 + "0,0" ★
    //   以前这里画的是「起点」，但起点坐标是 (start_x, start_y)，不是原点 ——
    //   两个概念混在一起，看着就像原点跑偏了。
    {
      const ImVec2 o(world_left(0.0f), world_top(0.0f, 0.0f));
      const ImU32 c = IM_COL32(90, 220, 120, 255);
      dl->AddLine(ImVec2(o.x - 7, o.y), ImVec2(o.x + 7, o.y), c, 2.0f);
      dl->AddLine(ImVec2(o.x, o.y - 7), ImVec2(o.x, o.y + 7), c, 2.0f);
      dl->AddText(ImVec2(o.x + 9, o.y + 3), IM_COL32(150, 230, 170, 220), "0,0");
    }
    // 起点（start_x / start_y）：橙色圆点 + START，和原点区分开
    {
      const ImVec2 sp(world_left(static_cast<float>(SceneSpawnX(s))),
                      world_top(static_cast<float>(SceneSpawnY(s)), 0.0f));
      dl->AddCircleFilled(sp, 4.0f, IM_COL32(255, 170, 60, 255));
      dl->AddCircle(sp, 6.0f, IM_COL32(255, 200, 120, 220), 0, 1.5f);
      dl->AddText(ImVec2(sp.x + 9, sp.y - 18), IM_COL32(255, 200, 120, 230), "START");
    }
  }
  for (size_t ti = 0; ti < s->triggers.size(); ++ti) {
    pgmmv::TriggerDef& tr = s->triggers[ti];
    const ImVec2 ta(world_left(static_cast<float>(tr.x)),
                    world_top(static_cast<float>(tr.y + tr.h), 0.0f));
    const ImVec2 tb(world_left(static_cast<float>(tr.x + tr.w)),
                    world_top(static_cast<float>(tr.y), 0.0f));
    const bool selected = (g_selected_trigger == static_cast<int>(ti));
    dl->AddRectFilled(ta, tb, selected ? IM_COL32(255, 220, 90, 90) : IM_COL32(230, 200, 60, 55));
    dl->AddRect(ta, tb, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 200, 60, 220), 0.0f, 0, selected ? 2.0f : 1.5f);
    const ImVec2 m = ImGui::GetIO().MousePos;
    const bool inside = m.x >= ta.x && m.x <= tb.x && m.y >= ta.y && m.y <= tb.y;
    static bool trig_move_undo = false;
    if (inside && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
      g_selected_trigger = static_cast<int>(ti);
      g_selected_placed_object = -1;
      g_scene_sel_kind = SceneSelKind::kTrigger;
      trig_move_undo = false;
    }
    if (selected && inside && ImGui::IsItemHovered() && ImGui::IsMouseDragging(0)) {
      if (!trig_move_undo) {
        PushUndo();
        trig_move_undo = true;
      }
      tr.x += static_cast<int>(ImGui::GetIO().MouseDelta.x / zoom);
      tr.y -= static_cast<int>(ImGui::GetIO().MouseDelta.y / zoom);  // y 向上：往下拖 = y 变小
      MarkDirty();
    }
  }
  // 笔刷 ghost：悬停时显示落笔范围
  if (!g_scene_collision_mode && !g_scene_trigger_mode && !g_scene_place_mode && !layer.locked &&
      ImGui::IsItemHovered()) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int c = col_at(m.x);
    const int r = row_at(m.y);
    const int bw = std::max(1, g_brush_w);
    const int bh = std::max(1, g_brush_h);
    const ImVec2 a(world_left(static_cast<float>(c * tw)),
                   world_top(static_cast<float>(r * th), static_cast<float>(bh * th)));
    const ImVec2 b(world_left(static_cast<float>((c + bw) * tw)),
                   world_top(static_cast<float>(r * th), 0.0f));
    dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 28));
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 150), 0.0f, 0, 1.0f);
  }
  if (!g_scene_collision_mode && !g_scene_trigger_mode && !g_scene_place_mode && !layer.locked &&
      ImGui::IsItemHovered() && (ImGui::IsMouseDown(0) || ImGui::IsMouseDown(1))) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const int c = col_at(m.x);
    const int r = row_at(m.y);
    if (c >= cL && c <= cR && r >= rB && r <= rT) {
      // 一笔一次撤销（拖动过程中不重复存）
      if (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1)) PushUndo();
      const int wc0 = c, wr0 = r;
      if (ImGui::IsMouseDown(1)) {
        if (s->TileAt(g_scene_layer, wc0, wr0) != -1) {
          s->SetTileAt(g_scene_layer, wc0, wr0, -1);
          MarkDirty();
        }
      } else {
        // ★ 行宽必须和「选择器」和「画布」用同一个来源：图片宽 / 图块宽 ★
        //   以前这里查 TilesetDoc.columns，一旦它和图片实际列数不一致
        //   （换过图、改过 tile 尺寸、或者层上的 tileset id 对不上导致回落到 1），
        //   写进去的索引就会指到别的图块 —— 画出"和选中的不一样"的块。
        //   v41：索引按「画笔槽位」的图集算，落笔写 PackCell(槽位, 索引)。
        for (int dy = 0; dy < g_brush_h; ++dy) {
          for (int dx = 0; dx < g_brush_w; ++dx) {
            const int cc = c + dx;
            const int rr = r + dy;
            // ★ y 向上：从选择区的底部往上贴 ★ 否则框选出来的块贴上去是上下镜像的
            const int bsy = g_brush_y + (g_brush_h - 1 - dy);
            const int idx = bsy * paint_cols + (g_brush_x + dx);
            const int packed = pgmmv::PackCell(g_paint_slot, idx);
            if (s->TileAt(g_scene_layer, cc, rr) != packed) {
              s->SetTileAt(g_scene_layer, cc, rr, packed);
              MarkDirty();
            }
          }
        }
      }
    }
  }
  // ★ 缩放/平移放到最后处理 ★
  //   它们会改 zoom/pan，而本帧的 tile/网格是在函数开头用旧的 zoom、旧的 pan 画的；
  //   如果在这里之前改，后面画的「碰撞线」「原点/起点标记」就会用新 zoom + 旧 pan ——
  //   每滚一格它们就和图层错开一帧，看起来就是闪烁。
  if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    const float wx = wx_at(m.x);
    const float wy = wy_at(m.y);
    const float nz = std::max(0.4f, std::min(5.0f, zoom + ImGui::GetIO().MouseWheel * 0.1f));
    if (nz != zoom) {
      zoom = nz;
      pan.x = m.x - vp.x - wx * zoom;
      pan.y = m.y - vp.y + wy * zoom;
    }
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(2)) {
    pan.x += ImGui::GetIO().MouseDelta.x;
    pan.y += ImGui::GetIO().MouseDelta.y;
  }
}

void DrawScenePageRight() {
  pgmmv::SceneDoc* s = CurrentScene();
  if (!g_has_project || s == nullptr) {
    ImGui::TextDisabled("未选择场景");
    return;
  }
  if (g_scene_layer < 0) g_scene_layer = 0;
  if (g_scene_layer > 3) g_scene_layer = 3;
  pgmmv::SceneLayer& layer = s->layers[g_scene_layer];
  EnsureSceneTilesets(s);

  // ★ 右栏 = 属性栏：只显示「当前选中节点」的属性 ★
  //   选中在树里点、也能在画布上点；失效的选中回落到场景。
  if (g_scene_sel_kind == SceneSelKind::kTrigger &&
      (g_selected_trigger < 0 ||
       g_selected_trigger >= static_cast<int>(s->triggers.size()))) {
    g_scene_sel_kind = SceneSelKind::kScene;
  }
  if (g_scene_sel_kind == SceneSelKind::kObject &&
      (g_selected_placed_object < 0 ||
       g_selected_placed_object >= static_cast<int>(s->objects.size()))) {
    g_scene_sel_kind = SceneSelKind::kScene;
  }
  const bool show_scene = (g_scene_sel_kind == SceneSelKind::kScene);
  const bool show_layer = (g_scene_sel_kind == SceneSelKind::kLayer);
  const bool show_trigger = (g_scene_sel_kind == SceneSelKind::kTrigger);
  const bool show_object = (g_scene_sel_kind == SceneSelKind::kObject);
  const bool show_camera = (g_scene_sel_kind == SceneSelKind::kCamera);

  char sel_title[192];
  if (show_trigger) {
    const pgmmv::TriggerDef& t = s->triggers[static_cast<size_t>(g_selected_trigger)];
    std::snprintf(sel_title, sizeof(sel_title), "触发器 · %s",
                  t.name.empty() ? ("#" + std::to_string(g_selected_trigger)).c_str()
                                 : t.name.c_str());
  } else if (show_object) {
    const pgmmv::SceneObject& o = s->objects[static_cast<size_t>(g_selected_placed_object)];
    const pgmmv::ObjectDoc* od = g_project.FindObject(o.object);
    std::snprintf(sel_title, sizeof(sel_title), "对象 · %s",
                  od != nullptr ? od->name.c_str() : o.id.c_str());
  } else if (show_camera) {
    std::snprintf(sel_title, sizeof(sel_title), "相机");
  } else if (show_layer) {
    std::snprintf(sel_title, sizeof(sel_title), "图层 · %s",
                  layer.name.empty() ? "(未命名)" : layer.name.c_str());
  } else {
    std::snprintf(sel_title, sizeof(sel_title), "场景 · %s", s->name.c_str());
  }
  ImGui::SeparatorText(sel_title);

  if (show_scene) {
  ImGui::Text("ID：%s", s->id.c_str());
  // ★ 谁是玩家（受控对象）★ 没有它，试玩时不会创建可控角色
  {
    const std::string cur_id =
        s->controlled.empty() ? std::string() : s->controlled.front();
    // 没指定时运行时自动取第一个 player 对象，这里把那个名字显示出来
    std::string auto_name;
    for (const pgmmv::ObjectDoc& od : g_project.objects) {
      if (od.type == "player") {
        auto_name = od.name;
        break;
      }
    }
    const std::string cur_label =
        !cur_id.empty() ? cur_id
                        : (auto_name.empty() ? std::string("(无)")
                                             : ("(自动) " + auto_name));
    ImGui::TextUnformatted("玩家对象");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##controlled", cur_label.c_str())) {
      if (ImGui::Selectable("(自动：第一个 player)", cur_id.empty())) {
        PushUndo();
        s->controlled.clear();
        MarkDirty();
      }
      for (const pgmmv::ObjectDoc& o : g_project.objects) {
        if (ImGui::Selectable(o.name.c_str(), cur_id == o.id)) {
          PushUndo();
          s->controlled = {o.id};
          MarkDirty();
        }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::TextDisabled("出生点 = 场景里玩家对象实例的位置");
  ImGui::TextUnformatted("地图尺寸（格）");
  ImGui::SetNextItemWidth(90.0f);
  if (UndoIntField("##map_w", &s->map_w, 1)) {
    s->map_w = std::max(1, s->map_w);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("×");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  if (UndoIntField("##map_h", &s->map_h, 1)) {
    s->map_h = std::max(1, s->map_h);
  }
  ImGui::TextUnformatted("背景音乐");
  ImGui::SetNextItemWidth(-1.0f);
  {
    const char* cur = s->bgm.empty() ? "(无)" : s->bgm.c_str();
    if (ImGui::BeginCombo("##scene_bgm", cur)) {
      if (ImGui::Selectable("(无)", s->bgm.empty())) {
        PushUndo();
        s->bgm.clear();
        MarkDirty();
        UiLog("bgm select none");
      }
      for (const pgmmv::Asset& a : g_project.assets) {
        if (a.kind != pgmmv::AssetKind::kAudio) continue;
        if (ImGui::Selectable(a.name.c_str(), s->bgm == a.id)) {
          PushUndo();
          s->bgm = a.id;
          MarkDirty();
          UiLogF("bgm select %s", a.id.c_str());
        }
      }
      ImGui::EndCombo();
    }
  }
  // 图块组是后台自动维护的（画笔用到哪个就进槽位表），不在这里显示
  }  // show_scene
  // ★ 图层属性：只放跟"这一层怎么画"有关的东西 ★
  //   显示/锁定/碰撞层/触发层 在画布上方；这里管网格尺寸、前景、视差。
  if (show_layer) {
  ImGui::Text("当前：%s", layer.name.empty() ? "(未命名)" : layer.name.c_str());
  {
    bool en = layer.enabled;
    if (ImGui::Checkbox("启用该层", &en)) {
      PushUndo();
      layer.enabled = en;
      MarkDirty();
    }
  }
  ImGui::TextUnformatted("优先级（0 最前，3 最后）");
  ImGui::SetNextItemWidth(90.0f);
  if (UndoIntField("##prio", &layer.priority, 1)) {
    layer.priority = std::max(0, std::min(3, layer.priority));
  }
  // 视差：1.00 = 跟世界一起动；0.10 = 慢速；0 = 钉在屏幕上
  ImGui::TextUnformatted("视差速度（相对摄像机）");
  {
    float mvx = static_cast<float>(layer.parallax_x) / 100.0f;
    float mvy = static_cast<float>(layer.parallax_y) / 100.0f;
    ImGui::TextUnformatted("X 视差");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(112.0f);
    if (ImGui::SliderFloat("##mvx", &mvx, 0.0f, 2.0f, "%.2f")) {
      layer.parallax_x = static_cast<int>(mvx * 100.0f + 0.5f);
      MarkDirty();
    }
    if (ImGui::IsItemActivated()) PushUndo();
    ImGui::SameLine();
    ImGui::TextUnformatted("Y 视差");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(112.0f);
    if (ImGui::SliderFloat("##mvy", &mvy, 0.0f, 2.0f, "%.2f")) {
      layer.parallax_y = static_cast<int>(mvy * 100.0f + 0.5f);
      MarkDirty();
    }
    if (ImGui::IsItemActivated()) PushUndo();
    ImGui::TextDisabled("1.00 跟随世界 / 0.10 慢速视差 / 0 固定屏幕");
  }
  }  // show_layer
  // ★ 放置工具：这些不是场景数据，是编辑器当前的选择 ★
  if (show_scene) {
  ImGui::SeparatorText("放置");
  ImGui::TextUnformatted("对象");
  ImGui::SetNextItemWidth(-1.0f);
  const char* cur_obj = g_selected_object_for_place.empty()
                            ? "(未选择)" : g_selected_object_for_place.c_str();
  if (ImGui::BeginCombo("##place_obj", cur_obj)) {
    for (const pgmmv::ObjectDoc& o : g_project.objects) {
      if (ImGui::Selectable(o.name.c_str(), g_selected_object_for_place == o.id)) {
        g_selected_object_for_place = o.id;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Checkbox("放置模式（对象）", &g_scene_place_mode);
  ImGui::Checkbox("放置触发器（左键点关卡）", &g_scene_place_trigger);
  static bool place_cb_log = false;
  if (!place_cb_log) {
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    UiLogF("place cb rect %.0f %.0f %.0f %.0f", a.x, a.y, b.x, b.y);
    place_cb_log = true;
  }
  }  // show_scene
  // ★ 相机 ★
  if (show_camera) {
    ImGui::Separator();
    ImGui::Checkbox("相机框跟起点", &g_camera_frame_follow_spawn);
    ImGui::SameLine();
    ImGui::TextDisabled("关闭后回到 (0,0)");
    ImGui::TextDisabled("相机（跟随时）");
    const char* kModes[] = {"follow", "fixed"};
    const char* kModeLabels[] = {"跟随（死区）", "固定"};
    int mode = 0;
    for (int i = 0; i < 2; ++i) {
      if (s->camera_mode == kModes[i]) mode = i;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##cam_mode", &mode, kModeLabels, 2)) {
      PushUndo();
      s->camera_mode = kModes[mode];
      MarkDirty();
    }
    ImGui::TextDisabled("死区：角色在框内屏幕不动，出框才推屏幕");
    ImGui::TextUnformatted("死区 宽 / 高");
    ImGui::SetNextItemWidth(90.0f);
    UndoIntField("##dz_w", &s->camera.deadzone_w, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    UndoIntField("##dz_h", &s->camera.deadzone_h, 1);
    ImGui::SameLine();
    if (ImGui::Button("铺满屏幕", ImVec2(80, 22))) {
      PushUndo();
      s->camera.deadzone_w = pgmmv::kScreenWidth;
      s->camera.deadzone_h = pgmmv::kScreenHeight;
      MarkDirty();
    }
    ImGui::TextUnformatted("偏移 X / Y");
    ImGui::SetNextItemWidth(90.0f);
    UndoIntField("##cam_ox", &s->camera.offset_x, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    UndoIntField("##cam_oy", &s->camera.offset_y, 1);
    ImGui::TextDisabled("0,0 = 角色居中；死区外推到边才跟随");
  }

  // ★ 对象（场景实例）属性 ★
  if (show_object) {
  if (g_selected_placed_object >= 0 &&
      g_selected_placed_object < static_cast<int>(s->objects.size())) {
    ImGui::Separator();
    pgmmv::SceneObject& o = s->objects[g_selected_placed_object];
    ImGui::TextUnformatted("引用对象");
    ImGui::SetNextItemWidth(-1.0f);
    const pgmmv::ObjectDoc* ref = g_project.FindObject(o.object);
    if (ImGui::BeginCombo("##sel_obj", ref != nullptr ? ref->name.c_str()
                                                       : o.object.c_str())) {
      for (const pgmmv::ObjectDoc& od : g_project.objects) {
        if (ImGui::Selectable(od.name.c_str(), o.object == od.id)) {
          o.object = od.id;
          MarkDirty();
        }
      }
      ImGui::EndCombo();
    }
    ImGui::TextUnformatted("位置 X / Y");
    ImGui::SetNextItemWidth(90.0f);
    UndoIntField("##ox", &o.x);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    UndoIntField("##oy", &o.y);
    if (RowButton("聚焦实例")) {
      g_scene_focus_pending = true;
      g_scene_focus_x = static_cast<float>(o.x);
      g_scene_focus_y = static_cast<float>(o.y) +
                        (ref != nullptr ? static_cast<float>(ref->frame.h) * 0.5f : 16.0f);
    }
    if (RowButton("复制对象")) {
      const std::string obj_id = o.object;
      const int ox = o.x;
      const int oy = o.y;
      PushUndo();
      s->AddSceneObject(obj_id, ox + 16, oy + 16);
      g_selected_placed_object = static_cast<int>(s->objects.size()) - 1;
      MarkDirty();
    }
    if (DangerRowButton("删除对象")) {
      PushUndo();
      s->objects.erase(s->objects.begin() + g_selected_placed_object);
      g_selected_placed_object = -1;
      MarkDirty();
    }
  }
  }  // show_object
  // ★ 触发器属性（右栏只显示选中的那一个）★
  if (show_trigger && g_selected_trigger >= 0 &&
      g_selected_trigger < static_cast<int>(s->triggers.size())) {
    pgmmv::TriggerDef& trg = s->triggers[static_cast<size_t>(g_selected_trigger)];
    ImGui::TextDisabled("触发器 #%d", g_selected_trigger);
    // 矩形（原来在列表里，现在归属性栏）
    ImGui::TextUnformatted("矩形 X / Y");
    ImGui::SetNextItemWidth(80.0f);
    UndoIntField("##trx", &trg.x);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    UndoIntField("##try", &trg.y);
    ImGui::TextUnformatted("矩形 宽 / 高");
    ImGui::SetNextItemWidth(80.0f);
    UndoIntField("##trw", &trg.w, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    UndoIntField("##trh", &trg.h, 1);
    // 进入检测对象：空 = 旧行为（玩家），否则检测指定对象的实例
    {
      const pgmmv::ObjectDoc* target = trg.target_object.empty()
                                           ? nullptr
                                           : g_project.FindObject(trg.target_object);
      const char* cur = target != nullptr ? target->name.c_str() : "(玩家 / 旧行为)";
      ImGui::TextUnformatted("进入检测对象");
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::BeginCombo("##trig_object", cur)) {
        if (ImGui::Selectable("(玩家 / 旧行为)", trg.target_object.empty())) {
          trg.target_object.clear();
          MarkDirty();
        }
        for (const pgmmv::ObjectDoc& o : g_project.objects) {
          if (ImGui::Selectable(o.name.c_str(), trg.target_object == o.id)) {
            trg.target_object = o.id;
            MarkDirty();
          }
        }
        ImGui::EndCombo();
      }
      ImGui::TextDisabled("选中哪个对象，就检测它的实例进入本触发器");
    }
    // 进入 / 离开 的效果列表（第一条进入动作留给上面的场景下拉）
    auto draw_actions = [&](const char* label, std::vector<pgmmv::TriggerAction>* list,
                            size_t first) {
      ImGui::Separator();
      ImGui::TextUnformatted(label);
      int del = -1;
      for (size_t i = first; i < list->size(); ++i) {
        pgmmv::TriggerAction& a = (*list)[i];
        ImGui::PushID(static_cast<int>(i) + 5000);
        const char* types[] = {"change_scene", "play_sfx", "set_variable",
                               "add_variable", "open_ui", "close_ui", "save", "load"};
        const char* labels[] = {"切场景", "音效", "设变量",
                                "加变量", "打开界面", "关闭界面", "存档", "读档"};
        int ti = 0;
        for (int k = 0; k < 8; ++k) {
          if (a.type == types[k]) ti = k;
        }
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::Combo("##t", &ti, labels, 8)) {
          a.type = types[ti];
          MarkDirty();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        if (a.type == "open_ui") {
          const char* cur = a.target.empty() ? "(选界面)" : a.target.c_str();
          if (ImGui::BeginCombo("##tg", cur)) {
            for (const pgmmv::UiScreenDoc& us : g_project.ui_screens) {
              const std::string label = us.name.empty() ? us.id : us.name;
              if (ImGui::Selectable(label.c_str(), a.target == us.id)) {
                a.target = us.id;
                MarkDirty();
              }
            }
            ImGui::EndCombo();
          }
        } else if (a.type == "close_ui") {
          ImGui::TextDisabled("(无需目标)");
        } else if (a.type == "save" || a.type == "load") {
          ImGui::TextDisabled("(save0.dat)");
        } else {
          char tb[64];
          std::snprintf(tb, sizeof(tb), "%s", a.target.c_str());
          if (ImGui::InputTextWithHint("##tg", "对象/属性/音效", tb, sizeof(tb))) {
            a.target = tb;
            MarkDirty();
          }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        if (ImGui::InputInt("##v", &a.value)) MarkDirty();
        ImGui::SameLine();
        if (RightSmallButton("x")) del = static_cast<int>(i);
        ImGui::PopID();
      }
      if (del >= 0) {
        PushUndo();
        list->erase(list->begin() + del);
        MarkDirty();
      }
      if (InlineButton("+ 动作", 64)) {
        PushUndo();
        pgmmv::TriggerAction a;
        a.type = "set_variable";
        list->push_back(a);
        MarkDirty();
      }
    };
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("##inside_n", &trg.inside_interval, 1, 10)) {
      trg.inside_interval = std::max(1, trg.inside_interval);
      MarkDirty();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("停留规则帧间隔");
    ImGui::SeparatorText("区域规则");
    int del_rule = -1;
    for (int ri = 0; ri < static_cast<int>(trg.rules.size()); ++ri) {
      pgmmv::TriggerRuleDef& rule = trg.rules[static_cast<size_t>(ri)];
      ImGui::PushID(7000 + ri);
      if (ImGui::Checkbox("启用", &rule.enabled)) MarkDirty();
      const char* modes[] = {"进入", "离开", "在里面"};
      int mode = std::max(0, std::min(2, rule.mode));
      ImGui::SetNextItemWidth(90.0f);
      if (ImGui::Combo("##rmode", &mode, modes, 3)) {
        rule.mode = mode;
        MarkDirty();
      }
      ImGui::SameLine();
      char var_buf[64];
      std::snprintf(var_buf, sizeof(var_buf), "%s", rule.require_variable.c_str());
      ImGui::SetNextItemWidth(120.0f);
      if (ImGui::InputTextWithHint("##rvar", "变量(空=总是)", var_buf,
                                   sizeof(var_buf))) {
        rule.require_variable = var_buf;
        MarkDirty();
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(60.0f);
      if (ImGui::InputInt("##rval", &rule.require_value)) MarkDirty();
      draw_actions("规则动作", &rule.actions, 0);
      if (DangerSmallButton("删除规则")) del_rule = ri;
      ImGui::PopID();
    }
    if (del_rule >= 0) {
      PushUndo();
      trg.rules.erase(trg.rules.begin() + del_rule);
      MarkDirty();
    }
    if (RowButton("+ 规则")) {
      PushUndo();
      pgmmv::TriggerRuleDef rule;
      trg.rules.push_back(rule);
      MarkDirty();
    }

    char namebuf[64];
    std::snprintf(namebuf, sizeof(namebuf), "%s", trg.name.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##trig_name", namebuf, sizeof(namebuf))) {
      trg.name = namebuf;
      MarkDirty();
    }
    PushUndoOnActivate();
    float rgba[4] = {((trg.color >> 24) & 0xFF) / 255.0f, ((trg.color >> 16) & 0xFF) / 255.0f,
                     ((trg.color >> 8) & 0xFF) / 255.0f, 1.0f};
    ImGui::SetNextItemWidth(-1.0f);
    // 点色块会当场改色：先判断激活、压完撤销再写回
    const bool color_edited = ImGui::ColorEdit3("##trig_color", rgba);
    const bool color_activated = ImGui::IsItemActivated();
    if (color_activated) PushUndo();
    if (color_edited) {
      trg.color = (static_cast<unsigned>(rgba[0] * 255.0f + 0.5f) << 24) |
                  (static_cast<unsigned>(rgba[1] * 255.0f + 0.5f) << 16) |
                  (static_cast<unsigned>(rgba[2] * 255.0f + 0.5f) << 8) | 0xFFu;
      MarkDirty();
    }
    ImGui::TextUnformatted("修饰器（改基础参数）");
    int remove_mod = -1;
    for (int mi = 0; mi < static_cast<int>(trg.mods.size()); ++mi) {
      pgmmv::StatMod& m = trg.mods[static_cast<size_t>(mi)];
      ImGui::PushID(mi);
      int stat = static_cast<int>(m.id);
      const char* stat_names[] = {"speed", "gravity", "max_fall"};
      ImGui::SetNextItemWidth(140.0f);
      if (ImGui::Combo("##stat", &stat, stat_names, 3)) {
        PushUndo();
        m.id = static_cast<pgmmv::StatId>(stat);
        MarkDirty();
      }
      ImGui::SameLine();
      int op = static_cast<int>(m.op);
      const char* op_names[] = {"+", "x", "="};
      ImGui::SetNextItemWidth(52.0f);
      if (ImGui::Combo("##op", &op, op_names, 3)) {
        PushUndo();
        m.op = static_cast<pgmmv::StatOp>(op);
        MarkDirty();
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80.0f);
      if (ImGui::DragFloat("##val", &m.value, 0.05f, -9999.0f, 9999.0f, "%.2f")) MarkDirty();
      if (ImGui::IsItemActivated()) PushUndo();
      ImGui::SameLine();
      if (RightSmallButton("x")) remove_mod = mi;
      ImGui::PopID();
    }
    if (remove_mod >= 0) {
      PushUndo();
      trg.mods.erase(trg.mods.begin() + remove_mod);
      MarkDirty();
    }
    ImGui::Separator();
    {
      bool has_it = trg.interaction.enabled;
      if (ImGui::Checkbox("交互点（进区域按键触发）", &has_it)) {
        PushUndo();
        trg.interaction.enabled = has_it;
        MarkDirty();
      }
      if (trg.interaction.enabled) {
        char pbuf[128];
        std::snprintf(pbuf, sizeof(pbuf), "%s", trg.interaction.prompt.c_str());
        if (trg.interaction.ui_id.empty()) {
          ImGui::SetNextItemWidth(-1.0f);
          if (ImGui::InputTextWithHint("##it_prompt", "内嵌提示文字", pbuf, sizeof(pbuf))) {
            trg.interaction.prompt = pbuf;
            MarkDirty();
          }
        }
        int anchor = 0;
        if (trg.interaction.anchor == "zone_center") anchor = 1;
        else if (trg.interaction.anchor == "object") anchor = 2;
        const char* anchors[] = {"区域上方", "区域中心", "跟随对象"};
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("锚点", &anchor, anchors, 3)) {
          PushUndo();
          trg.interaction.anchor = anchor == 1 ? "zone_center" : (anchor == 2 ? "object" : "zone_top");
          MarkDirty();
        }
        int key = 1;
        if (trg.interaction.key == "a") key = 0;
        else if (trg.interaction.key == "start") key = 2;
        const char* keys[] = {"A", "B", "Start"};
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::Combo("按键", &key, keys, 3)) {
          PushUndo();
          trg.interaction.key = key == 0 ? "a" : (key == 2 ? "start" : "b");
          MarkDirty();
        }
        ImGui::SameLine();
        {
          bool once = trg.interaction.once;
          if (ImGui::Checkbox("只触发一次", &once)) {
            PushUndo();
            trg.interaction.once = once;
            MarkDirty();
          }
        }
        // 前置条件：变量 == 值（不满足就不显示提示、也不触发）
        ImGui::TextUnformatted("前置变量");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        {
          const char* cur = trg.interaction.require_variable.empty()
                                ? "(无)"
                                : trg.interaction.require_variable.c_str();
          if (ImGui::BeginCombo("##it_req", cur)) {
            if (ImGui::Selectable("(无)", trg.interaction.require_variable.empty())) {
              trg.interaction.require_variable.clear();
              MarkDirty();
            }
            for (const auto& kv : g_project.variables) {
              if (ImGui::Selectable(kv.first.c_str(),
                                    trg.interaction.require_variable == kv.first)) {
                trg.interaction.require_variable = kv.first;
                MarkDirty();
              }
            }
            for (const auto& kv : s->variables) {
              if (ImGui::Selectable(kv.first.c_str(),
                                    trg.interaction.require_variable == kv.first)) {
                trg.interaction.require_variable = kv.first;
                MarkDirty();
              }
            }
            ImGui::EndCombo();
          }
        }
        if (!trg.interaction.require_variable.empty()) {
          ImGui::SameLine();
          ImGui::SetNextItemWidth(70.0f);
          if (ImGui::InputInt("= 值##it_reqv", &trg.interaction.require_value)) MarkDirty();
        }
        ImGui::TextDisabled("动作");
        int remove_act = -1;
        for (int ai = 0; ai < static_cast<int>(trg.interaction.actions.size()); ++ai) {
          pgmmv::TriggerAction& a = trg.interaction.actions[static_cast<size_t>(ai)];
          ImGui::PushID(1000 + ai);
          const char* kTypes[] = {"change_scene", "dialogue", "set_variable", "play_sfx"};
          int t = 1;
          if (a.type == "change_scene") t = 0;
          else if (a.type == "set_variable") t = 2;
          else if (a.type == "play_sfx") t = 3;
          ImGui::SetNextItemWidth(120.0f);
          if (ImGui::Combo("##atype", &t, kTypes, 4)) {
            a.type = kTypes[t];
            MarkDirty();
          }
          ImGui::SameLine();
          char abuf[128];
          std::snprintf(abuf, sizeof(abuf), "%s", a.target.c_str());
          ImGui::SetNextItemWidth(150.0f);
          if (ImGui::InputText("##atarget", abuf, sizeof(abuf))) {
            a.target = abuf;
            MarkDirty();
          }
          if (a.type == "set_variable" || a.type == "add_variable") {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.0f);
            if (ImGui::InputInt("##aval", &a.value)) MarkDirty();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0f);
            const char* cva = a.target.empty() ? "(选择变量)" : a.target.c_str();
            if (ImGui::BeginCombo("##avarc", cva)) {
              for (const auto& kv : g_project.variables) {
                if (ImGui::Selectable(kv.first.c_str(), a.target == kv.first)) {
                  a.target = kv.first;
                  MarkDirty();
                }
              }
              for (const auto& kv : s->variables) {
                if (ImGui::Selectable(kv.first.c_str(), a.target == kv.first)) {
                  a.target = kv.first;
                  MarkDirty();
                }
              }
              ImGui::EndCombo();
            }
          }
          ImGui::SameLine();
          if (RightSmallButton("x")) remove_act = ai;
          ImGui::PopID();
        }
        if (remove_act >= 0) {
          PushUndo();
          trg.interaction.actions.erase(trg.interaction.actions.begin() + remove_act);
          MarkDirty();
        }
        if (InlineButton("+ 动作", 72)) {
          PushUndo();
          pgmmv::TriggerAction a;
          a.type = "dialogue";
          trg.interaction.actions.push_back(a);
          MarkDirty();
        }
      }
    }
    if (InlineButton("+ 修饰器", 80)) {
      PushUndo();
      pgmmv::StatMod m;
      m.id = pgmmv::StatId::kSpeed;
      m.op = pgmmv::StatOp::kMul;
      m.value = 0.5f;
      trg.mods.push_back(m);
      MarkDirty();
    }
    ImGui::SameLine();
    if (DangerInlineButton("删除触发器", 120)) {
      PushUndo();
      s->triggers.erase(s->triggers.begin() + g_selected_trigger);
      g_selected_trigger = -1;
      g_scene_sel_kind = SceneSelKind::kScene;
      MarkDirty();
    }
  }
  // 项目级设置（初始场景/屏幕尺寸）在 菜单 → 项目 → 项目设置
  if (show_scene) {
  // ★ 变量（场景级）★
  ImGui::Separator();
  if (ImGui::CollapsingHeader("变量", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled("全局（跨场景，存档保留）");
    {
      std::string remove_g;
      for (auto& kv : g_project.variables) {
        ImGui::PushID(("g_" + kv.first).c_str());
        ImGui::Text("%s", kv.first.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("##gv", &kv.second, 0, 0)) MarkDirty();
        ImGui::SameLine();
        if (RightSmallButton("x")) remove_g = kv.first;
        ImGui::PopID();
      }
      if (!remove_g.empty()) {
        g_project.variables.erase(remove_g);
        MarkDirty();
      }
      static char new_gvar[64] = "";
      ImGui::SetNextItemWidth(140.0f);
      ImGui::InputTextWithHint("##newgvar", "新全局变量", new_gvar, sizeof(new_gvar));
      ImGui::SameLine();
      if (InlineButton("+ 全局", 72) && new_gvar[0] != 0) {
        g_project.variables[new_gvar] = 0;
        new_gvar[0] = 0;
        MarkDirty();
      }
    }
    ImGui::TextDisabled("场景（进入场景时重置）");
    if (InlineButton("从触发器/UI 中收集", 160)) {
      for (const pgmmv::TriggerDef& t : s->triggers) {
        if (!t.interaction.require_variable.empty() &&
            s->variables.find(t.interaction.require_variable) == s->variables.end()) {
          s->variables[t.interaction.require_variable] = t.interaction.require_value;
        }
        for (const pgmmv::TriggerAction& a : t.interaction.actions) {
          if (a.type == "set_variable" && !a.target.empty() &&
              s->variables.find(a.target) == s->variables.end()) {
            s->variables[a.target] = a.value;
          }
        }
      }
      MarkDirty();
    }
    std::string remove_var;
    for (auto& kv : s->variables) {
      ImGui::PushID(kv.first.c_str());
      ImGui::TextUnformatted(kv.first.c_str());
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80.0f);
      if (ImGui::InputInt("##v", &kv.second, 0, 0)) MarkDirty();
      ImGui::SameLine();
      if (RightSmallButton("x")) remove_var = kv.first;
      ImGui::PopID();
    }
    if (!remove_var.empty()) {
      s->variables.erase(remove_var);
      MarkDirty();
    }
    static char new_var[64] = "";
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputTextWithHint("##newvar", "新变量名", new_var, sizeof(new_var));
    ImGui::SameLine();
    if (InlineButton("+ 变量", 72) && new_var[0] != 0) {
      s->variables[new_var] = 0;
      new_var[0] = 0;
      MarkDirty();
    }
  }
  ImGui::TextDisabled("UI 元素在左栏「UI 层」里编辑");
  }  // show_scene
}



// ------------------------- 动画页精修 -------------------------
// ★ 动画列表 = 动画资源列表，和对象没有关系 ★
pgmmv::AnimationDoc* CurrentAnimation() {
  if (!g_has_project || g_selected_animation.empty()) return nullptr;
  for (pgmmv::AnimationDoc& ad : g_project.animations) {
    if (ad.id == g_selected_animation) return &ad;
  }
  return nullptr;
}

pgmmv::AnimationDoc* EnsureAnimation() {
  if (CurrentAnimation() != nullptr) return CurrentAnimation();
  if (!g_has_project) return nullptr;
  if (!g_project.animations.empty()) {
    g_selected_animation = g_project.animations.front().id;
    return &g_project.animations.front();
  }
  return nullptr;
}

void CreateAnimation() {
  if (!g_has_project) return;
  PushUndo();
  pgmmv::AnimationDoc ad = pgmmv::AnimationCreateDefault("anim", "anim");
  std::string base = "anim";
  int n = 1;
  while (pgmmv::FindAnimation(g_project, base) != nullptr) {
    base = "anim_" + std::to_string(++n);
  }
  ad.id = base;
  ad.name = base;
  ad.image = g_new_object_image;
  pgmmv::ActionDef idle;
  idle.name = "idle";
  ad.actions.push_back(idle);
  g_project.animations.push_back(std::move(ad));
  g_selected_animation = base;
  g_selected_action = "idle";
  g_anim_frame = 0;
  g_anim_direction = 0;
  MarkDirty();
}

void DeleteAnimation() {
  pgmmv::AnimationDoc* ad = CurrentAnimation();
  if (ad == nullptr) return;
  int users = 0;
  for (const pgmmv::ObjectDoc& o : g_project.objects) {
    if (o.animation == ad->id) ++users;
  }
  if (users > 0) {
    SetStatus("这个动画还有 " + std::to_string(users) + " 个对象在用，先改它们的引用");
    return;
  }
  PushUndo();
  for (size_t i = 0; i < g_project.animations.size(); ++i) {
    if (g_project.animations[i].id == ad->id) {
      g_project.animations.erase(g_project.animations.begin() + static_cast<long>(i));
      break;
    }
  }
  g_selected_animation.clear();
  g_selected_action.clear();
  MarkDirty();
}

void DrawAnimListLeft() {
  ImGui::TextDisabled("动画列表");
  ImGui::Separator();
  if (!g_has_project) {
    ImGui::TextDisabled("未打开项目");
    return;
  }
  if (RowButton("+ 动画")) CreateAnimation();
  ImGui::BeginDisabled(CurrentAnimation() == nullptr);
  if (DangerRowButton("删除动画")) DeleteAnimation();
  ImGui::EndDisabled();
  ImGui::Separator();
  if (g_project.animations.empty()) {
    ImGui::TextDisabled("还没有动画，点「+ 动画」新建一个");
    return;
  }
  for (pgmmv::AnimationDoc& ad : g_project.animations) {
    ImGui::PushID(ad.id.c_str());
    const bool open = ImGui::TreeNodeEx(
        ad.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
      g_selected_animation = ad.id;
      if (!ad.actions.empty() && g_selected_action != ad.actions.front().name) {
        g_selected_action = ad.actions.front().name;
        g_anim_frame = 0;
        g_anim_direction = 0;
      }
    }
    if (open) {
      for (pgmmv::ActionDef& a : ad.actions) {
        const bool sel =
            (g_selected_animation == ad.id && g_selected_action == a.name);
        if (ImGui::Selectable(a.name.c_str(), sel)) {
          g_selected_animation = ad.id;
          g_selected_action = a.name;
          g_anim_frame = 0;
          g_anim_direction = 0;
        }
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
}

pgmmv::ActionDef* CurrentAnimAction() {
  pgmmv::AnimationDoc* ad = EnsureAnimation();
  if (ad == nullptr) return nullptr;
  for (pgmmv::ActionDef& a : ad->actions) {
    if (a.name == g_selected_action) return &a;
  }
  if (!ad->actions.empty()) {
    g_selected_action = ad->actions.front().name;
    return &ad->actions.front();
  }
  return nullptr;
}

void DrawDetectionSettings() {
  pgmmv::ActionDef* action = CurrentAnimAction();
  pgmmv::DirectionDef* dir = action == nullptr ? nullptr
                                               : pgmmv::ActionDirectionAt(action, g_anim_direction);
  if (dir == nullptr) {
    ImGui::TextDisabled("未选择动作");
    return;
  }
  if (dir->frames.empty()) {
    ImGui::TextDisabled("当前方向没有帧");
    return;
  }
  // 不再单独维护一套「壁/当/攻」标签：判定盒统一由右栏的「判定盒」选择。
  const int track = g_box_draw_target;
  ImGui::Separator();
  g_anim_frame = std::max(0, std::min(g_anim_frame,
                                      static_cast<int>(dir->frames.size()) - 1));
  bool changed = false;
  const int fi = std::max(0, std::min(g_anim_frame,
                                     static_cast<int>(dir->frames.size()) - 1));
  pgmmv::FrameBox& fb = track == 0 ? dir->frames[static_cast<size_t>(fi)].collision
                        : track == 1 ? dir->frames[static_cast<size_t>(fi)].hurt
                                     : dir->frames[static_cast<size_t>(fi)].attack;
  if (track == 0) {
    ImGui::TextUnformatted("碰撞判定（当前帧）");
  } else if (track == 1) {
    ImGui::TextUnformatted("受击判定（当前帧）");
  } else {
    ImGui::TextUnformatted("攻击判定（当前帧）");
  }
  ImGui::PushID(track);
  // ★ 三态：沿用上一帧 / 明确没有 / 有（改数值时自动变"有"）★
  const pgmmv::CollisionBox* resolved =
      pgmmv::ResolveFrameBox(*dir, fi, track, nullptr);
  const char* kStates[3] = {"沿用前帧", "本帧无", "本帧有"};
  for (int s = 0; s < 3; ++s) {
    if (s > 0) ImGui::SameLine();
    const bool sel = (static_cast<int>(fb.state) == s);
    if (ImGui::RadioButton(kStates[s], sel)) {
      if (s == 2 && fb.state != pgmmv::BoxState::kBox) {
        if (resolved != nullptr) fb.box = *resolved;
      }
      fb.state = static_cast<pgmmv::BoxState>(s);
      changed = true;
    }
  }
  if (fb.state == pgmmv::BoxState::kInherit && resolved != nullptr) {
    ImGui::TextDisabled("当前值来自前面的帧：%d,%d %dx%d", resolved->x, resolved->y,
                        resolved->w, resolved->h);
  } else if (fb.state == pgmmv::BoxState::kNone) {
    ImGui::TextDisabled("这一帧起没有判定（直到下一帧再设）");
  }
  if (fb.state == pgmmv::BoxState::kBox) {
  pgmmv::CollisionBox* box = &fb.box;
  ImGui::SetNextItemWidth(58.0f);
  if (ImGui::InputInt("X##box_x", &box->x)) changed = true;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(58.0f);
  if (ImGui::InputInt("Y##box_y", &box->y)) changed = true;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(58.0f);
  if (ImGui::InputInt("宽##box_w", &box->w)) changed = true;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(58.0f);
  if (ImGui::InputInt("高##box_h", &box->h)) changed = true;
  }
  ImGui::PopID();
  if (changed) MarkDirty();
}
void DrawPlaceholder(const char* title);

const char* kResTabs[] = {"图像", "音频", "变量"};
int g_res_tab = 0;
char g_asset_search[64] = {};

PreviewTexture* AssetPreview(const pgmmv::Asset& a) {
  auto it = g_tileset_previews.find(a.id);
  if (it != g_tileset_previews.end()) return &it->second;
  PreviewTexture p;
  p.asset_id = a.id;
  std::vector<unsigned char> rgba;
  int w = 0;
  int h = 0;
  std::string err;
  if (!pgmmv::LoadPngRgba(pgmmv::ProjectAssetPath(g_project, a), &rgba, &w, &h,
                          &err)) {
    p.failed = true;
    g_tileset_previews[a.id] = p;
    return &g_tileset_previews[a.id];
  }
  if (!CreatePreviewTexture(rgba, w, h, &p)) p.failed = true;
  auto res = g_tileset_previews.emplace(a.id, std::move(p));
  return &res.first->second;
}

void CollectAssetRefs(const pgmmv::Asset& a, std::vector<std::string>* out) {
  if (out == nullptr) return;
  for (const pgmmv::ObjectDoc& o : g_project.objects) {
    if (o.image == a.path) out->push_back("对象：" + o.name);
  }
  for (const pgmmv::TilesetDoc& ts : g_project.tilesets) {
    if (ts.image == a.path) out->push_back("图块组：" + ts.name);
  }
}

void DrawResourceTabs() {
  for (int i = 0; i < static_cast<int>(sizeof(kResTabs) / sizeof(kResTabs[0])); ++i) {
    if (i > 0) ImGui::SameLine();
    if (TextTab(kResTabs[i], g_res_tab == i, 76.0f)) g_res_tab = i;
  }
}


// 导入一个音频文件（不弹对话框），成功返回 true
bool ImportAudioFile(const std::filesystem::path& src) {
  if (!g_has_project) return false;
  std::string base = src.stem().u8string();
  for (char& c : base) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
          (u >= '0' && u <= '9') || u == '_' || u == '-')) {
      c = '_';
    }
  }
  if (base.empty()) base = "audio";
  std::string id = base;
  for (int n = 2; g_project.FindAsset(id) != nullptr; ++n) {
    id = base + "_" + std::to_string(n);
  }
  std::string ext = src.extension().u8string();
  std::error_code ec;
  std::filesystem::create_directories(g_project.root_dir / "assets", ec);
  const std::filesystem::path dst =
      g_project.root_dir / "assets" / std::filesystem::u8path(id + ext);
  std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    SetError("复制失败：" + ec.message());
    return false;
  }
  pgmmv::Asset a;
  a.id = id;
  a.name = base;
  a.type = "audio";
  a.kind = pgmmv::AssetKind::kAudio;
  a.path = "assets/" + id + ext;
  a.source = src.u8string();  // 记住源文件，"更新"时从它重读
  g_project.assets.push_back(a);
  MarkDirty();
  return true;
}

void DoImportAudiosBulk() {
  if (!g_has_project) { SetError("先新建或打开项目"); return; }
  std::vector<std::filesystem::path> files;
  {
    wchar_t buf[8192] = {};
    OPENFILENAMEW ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"音频 (*.wav;*.ogg)\0*.wav;*.ogg\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 8192;
    ofn.lpstrTitle = L"导入音频";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (GetOpenFileNameW(&ofn) != TRUE) return;
    const wchar_t* cur = buf;
    const std::wstring first(cur);
    cur += first.size() + 1;
    std::vector<std::wstring> names;
    while (*cur != L'\0') {
      const std::wstring nm(cur);
      names.push_back(nm);
      cur += nm.size() + 1;
    }
    if (names.empty()) {
      files.emplace_back(first);
    } else {
      const std::filesystem::path dir(first);
      for (const std::wstring& nm : names) files.push_back(dir / std::filesystem::path(nm));
    }
  }
  int imported = 0;
  PushUndo();
  for (const std::filesystem::path& src : files) {
    if (!ImportAudioFile(src)) continue;
    ++imported;
  }
  if (imported > 0) SetStatus("已导入 " + std::to_string(imported) + " 个音频（未保存）");
  else SetError("没有导入任何音频");
}

void DrawAudioLeft() {
  ImGui::TextDisabled("声音列表");
  if (RowButton("导入音频")) DoImportAudiosBulk();
  ImGui::Separator();
  int count = 0;
  for (const pgmmv::Asset& a : g_project.assets) {
    if (a.kind != pgmmv::AssetKind::kAudio) continue;
    if (ImGui::Selectable(a.name.c_str(), g_selected_asset == a.id)) {
      g_selected_asset = a.id;
      SyncAssetBuffer();
    }
    ++count;
  }
  if (count == 0) ImGui::TextDisabled("没有音频资源");
}

void DrawResourceLeft2() {
  if (g_res_tab == 1) {
    DrawAudioLeft();
    return;
  }
  if (g_res_tab != 0) {
    DrawPlaceholder(kResTabs[g_res_tab]);
    return;
  }
  ImGui::TextDisabled("图像列表");
  if (RowButton("导入图片")) DoImportPngsBulk();
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##asset_search", "输入搜索文本", g_asset_search,
                           sizeof(g_asset_search));
  if (!g_has_project) {
    ImGui::TextDisabled("未打开项目");
    return;
  }
  if (g_project.assets.empty()) {
    ImGui::TextDisabled("没有图像资源");
  }
  std::map<std::string, std::vector<const pgmmv::Asset*>> groups;
  for (const pgmmv::Asset& a : g_project.assets) {
    if (a.kind != pgmmv::AssetKind::kImage && a.kind != pgmmv::AssetKind::kUnknown) {
      continue;
    }
    if (!TextMatchesFilter(a.name, g_asset_search) &&
        !TextMatchesFilter(a.id, g_asset_search)) {
      continue;
    }
    const size_t slash = a.path.find_last_of('/');
    const std::string folder = slash == std::string::npos ? "(根)" : a.path.substr(0, slash);
    groups[folder].push_back(&a);
  }
  for (const auto& g : groups) {
    if (ImGui::TreeNodeEx(g.first.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      for (const pgmmv::Asset* ap : g.second) {
        const pgmmv::Asset& a = *ap;
        PreviewTexture* p = AssetPreview(a);
        if (p != nullptr && p->srv != nullptr) {
          ImageAutoFilter(reinterpret_cast<ImTextureID>(p->srv), ImVec2(18.0f, 18.0f),
                          static_cast<float>(p->width));
          ImGui::SameLine();
        }
        if (ImGui::Selectable(a.name.c_str(), g_selected_asset == a.id)) {
          g_selected_asset = a.id;
          SyncAssetBuffer();
        }
      }
      ImGui::TreePop();
    }
  }
}

void DrawResourceCenter2() {
  if (g_res_tab == 1) {
    const pgmmv::Asset* aa = g_project.FindAsset(g_selected_asset);
    if (aa == nullptr || aa->kind != pgmmv::AssetKind::kAudio) {
      ImGui::TextDisabled("未选择音频");
      return;
    }
    ImGui::Text("名称：%s", aa->name.c_str());
    ImGui::Text("路径：%s", aa->path.c_str());
    std::error_code ec;
    const auto sz = std::filesystem::file_size(pgmmv::ProjectAssetPath(g_project, *aa), ec);
    ImGui::Text("大小：%llu 字节", static_cast<unsigned long long>(ec ? 0 : sz));
    // 更新：挑一个新文件覆盖这个音频资源（保留 id / 名称）
    // 更新：从记录的源文件重读（源素材可能随时被外部工具改）
    if (ImGui::Button("更新", ImVec2(90.0f, 26.0f))) {
      if (aa->source.empty()) {
        SetError("这个音频没有记录源文件（旧工程），请重新导入一次");
      } else {
        std::error_code ec2;
        const auto dst = pgmmv::ProjectAssetPath(g_project, *aa);
        std::filesystem::copy_file(std::filesystem::path(aa->source), dst,
                                   std::filesystem::copy_options::overwrite_existing, ec2);
        if (ec2) {
          SetError("更新失败：" + ec2.message());
        } else {
          MarkDirty();
          SetStatus("已从源文件更新：" + aa->name);
        }
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("源：%s", aa->source.empty() ? "(未记录)" : aa->source.c_str());
    return;
  }
  if (g_res_tab != 0) {
    DrawPlaceholder(kResTabs[g_res_tab]);
    return;
  }
  pgmmv::Asset* asset = g_project.FindAsset(g_selected_asset);
  if (!g_has_project || asset == nullptr) {
    ImGui::TextDisabled("没有选中的图像（从左侧「导入图片」进入）");
    return;
  }
  if (ImGui::Button("更新", ImVec2(90.0f, 26.0f))) {
    if (asset->source.empty()) {
      SetError("这个资源没有记录源文件（旧工程），请重新导入一次");
    } else {
      std::error_code ec_copy;
      const auto dst = pgmmv::ProjectAssetPath(g_project, *asset);
      std::filesystem::copy_file(std::filesystem::path(asset->source), dst,
                                 std::filesystem::copy_options::overwrite_existing, ec_copy);
      if (ec_copy) {
        SetError("更新失败：" + ec_copy.message());
      } else {
        // 源图尺寸可能变了：更新资源尺寸，并清掉按 asset id 缓存的缩略图。
        std::vector<unsigned char> probe;
        int new_w = 0;
        int new_h = 0;
        std::string probe_err;
        const auto dst_path = pgmmv::ProjectAssetPath(g_project, *asset);
        if (pgmmv::LoadPngRgba(dst_path, &probe, &new_w, &new_h, &probe_err)) {
          asset->width = new_w;
          asset->height = new_h;
        }
        ReleaseTilesetPreviews();
        PreviewRelease();
        LoadPreview(*asset);
        MarkDirty();
        SetStatus("已从源文件更新：" + asset->name);
      }
    }
  }
  {
    std::vector<std::string> refs;
    CollectAssetRefs(*asset, &refs);
    std::string line = "引用：";
    if (refs.empty()) {
      line += "（无）";
    } else {
      for (size_t i = 0; i < refs.size(); ++i) {
        if (i > 0) line += "  /  ";
        line += refs[i];
      }
    }
    ImGui::TextDisabled("%s", line.c_str());
  }
  ImGui::Separator();
  if (g_preview.asset_id != asset->id) LoadPreview(*asset);
  if (g_preview.srv == nullptr) {
    ImGui::TextDisabled("预览不可用");
    return;
  }
  static float zoom = 1.0f;
  ImGui::SetNextItemWidth(140.0f);
  ImGui::SliderFloat("##zoom", &zoom, 0.25f, 4.0f, "%.2fx");
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  float scale = std::min(avail.x / static_cast<float>(g_preview.width),
                         avail.y / static_cast<float>(g_preview.height));
  scale *= zoom;
  if (scale <= 0.0f) return;
  const ImVec2 size(static_cast<float>(g_preview.width) * scale,
                    static_cast<float>(g_preview.height) * scale);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  DrawCheckerboard(ImGui::GetWindowDrawList(), pos, size);
  ImGui::Image(reinterpret_cast<ImTextureID>(g_preview.srv), size);
}

void PlayAudioAsset(const pgmmv::Asset& a) {
  std::ifstream f(pgmmv::ProjectAssetPath(g_project, a), std::ios::binary);
  if (!f) {
    UiLog("play 0");
    return;
  }
  std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
  r4::AudioPlayClip(data.data(), data.size(), false);
  UiLogF("play %d", data.empty() ? 0 : 1);
}

void StopAudio() { r4::AudioStop(); }

void DrawResourceRight2() {
  if (g_res_tab == 2) {
    ImGui::TextDisabled("变量（待定）");
    return;
  }
  if (g_res_tab == 1) {
    pgmmv::Asset* aa = g_project.FindAsset(g_selected_asset);
    if (aa == nullptr || aa->kind != pgmmv::AssetKind::kAudio) {
      ImGui::TextDisabled("未选择音频");
      return;
    }
    ImGui::TextDisabled("属性");
    ImGui::Text("ID：%s", aa->id.c_str());
    ImGui::TextWrapped("路径：%s", aa->path.c_str());
    if (ImGui::Button("试听", ImVec2(80.0f, 24.0f))) {
      PlayAudioAsset(*aa);
    }
    LogItemRect("audio_play");
    ImGui::SameLine();
    if (ImGui::Button("停止", ImVec2(80.0f, 24.0f))) StopAudio();
    ImGui::TextUnformatted("名称");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##audio_name", g_asset_name_buf, sizeof(g_asset_name_buf))) {
      aa->name = g_asset_name_buf;
      MarkDirty();
    }
    std::vector<std::string> arefs;
    CollectAssetRefs(*aa, &arefs);
    ImGui::Separator();
    if (!arefs.empty()) {
      ImGui::TextDisabled("被引用，先解除引用再删除");
    } else if (DangerRowButton("删除资源")) {
      for (size_t ai = 0; ai < g_project.assets.size(); ++ai) {
        if (g_project.assets[ai].id == aa->id) {
          g_project.assets.erase(g_project.assets.begin() + ai);
          break;
        }
      }
      g_selected_asset.clear();
      MarkDirty();
      return;
    }
    return;
  }
  pgmmv::Asset* asset = g_project.FindAsset(g_selected_asset);
  if (!g_has_project || asset == nullptr) {
    ImGui::TextDisabled("未选择图像");
    return;
  }
  ImGui::TextDisabled("属性");
  ImGui::Text("ID：%s", asset->id.c_str());
  ImGui::Text("尺寸：%d × %d", asset->width, asset->height);
  ImGui::TextWrapped("路径：%s", asset->path.c_str());
  ImGui::TextUnformatted("名称");
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::InputText("##asset_name", g_asset_name_buf, sizeof(g_asset_name_buf))) {
    asset->name = g_asset_name_buf;
    MarkDirty();
  }
  ImGui::Separator();
  ImGui::TextDisabled("被引用");
  std::vector<std::string> refs;
  CollectAssetRefs(*asset, &refs);
  if (refs.empty()) {
    ImGui::TextDisabled("（无）");
  } else {
    for (const std::string& r : refs) ImGui::BulletText("%s", r.c_str());
  }
  ImGui::Separator();
  std::vector<std::string> refs2;
  CollectAssetRefs(*asset, &refs2);
  if (!refs2.empty()) {
    ImGui::TextDisabled("被引用，先解除引用再删除");
  } else if (DangerRowButton("删除资源")) {
    for (size_t ai = 0; ai < g_project.assets.size(); ++ai) {
      if (g_project.assets[ai].id == asset->id) {
        g_project.assets.erase(g_project.assets.begin() + ai);
        break;
      }
    }
    g_selected_asset.clear();
    PreviewRelease();
    MarkDirty();
    return;
  }
}

void DrawPlaceholder(const char* title) {
  ImGui::TextDisabled("%s（重建中）", title);
  ImGui::Separator();
  ImGui::TextWrapped("这一页的旧实现随 main.cpp 一起丢失，按 PGMMV 截图重写。");
}



// ------------------------- 规则编辑器（对象页中间） -------------------------
namespace ruledui {

enum class PKind { kNone, kKey, kVarName, kAnim, kOp, kNum, kFlip, kSfx };
const char* kOpLabels[] = {"<", "<=", "=", ">=", ">"};

struct CondUI { const char* label; pgmmv::RuleCond type; PKind a; PKind b; };
struct ActUI  { const char* label; pgmmv::RuleAct type; PKind a; };

const CondUI kConds[] = {
    {"常驻", pgmmv::RuleCond::kAlways, PKind::kNone, PKind::kNone},
    {"按住", pgmmv::RuleCond::kKeyDown, PKind::kKey, PKind::kNone},
    {"刚按下", pgmmv::RuleCond::kKeyPressed, PKind::kKey, PKind::kNone},
    {"松开", pgmmv::RuleCond::kKeyUp, PKind::kKey, PKind::kNone},
    {"在地面", pgmmv::RuleCond::kOnGround, PKind::kNone, PKind::kNone},
    {"在空中", pgmmv::RuleCond::kOnGroundNot, PKind::kNone, PKind::kNone},
    {"动画播完", pgmmv::RuleCond::kAnimEnd, PKind::kNone, PKind::kNone},
    {"每 N 帧", pgmmv::RuleCond::kTimer, PKind::kNum, PKind::kNone},
    {"长按", pgmmv::RuleCond::kKeyHeld, PKind::kKey, PKind::kNum},
    {"变量比较", pgmmv::RuleCond::kVarCmp, PKind::kVarName, PKind::kOp},
    {"离玩家 <", pgmmv::RuleCond::kPlayerDistLt, PKind::kNum, PKind::kNone},
    {"可推动", pgmmv::RuleCond::kPushable, PKind::kNone, PKind::kNone},
};

const ActUI kActs[] = {
    {"设速度", pgmmv::RuleAct::kMove, PKind::kNone},
    {"每帧位移", pgmmv::RuleAct::kMovePx, PKind::kNone},
    {"施加重力", pgmmv::RuleAct::kGravity, PKind::kNone},
    {"播动画", pgmmv::RuleAct::kAnim, PKind::kAnim},
    {"朝向", pgmmv::RuleAct::kFlip, PKind::kFlip},
    {"音效", pgmmv::RuleAct::kSfx, PKind::kSfx},
    {"速度叠加", pgmmv::RuleAct::kMoveAdd, PKind::kNone},
    {"按方向键移动", pgmmv::RuleAct::kMoveInput, PKind::kNone},
    {"生成", pgmmv::RuleAct::kSpawn, PKind::kNone},
    {"销毁", pgmmv::RuleAct::kDestroy, PKind::kNone},
    {"变量加减", pgmmv::RuleAct::kVarAdd, PKind::kNone},
    {"变量赋值", pgmmv::RuleAct::kVarSet, PKind::kNone},
};

const char* kKeys[] = {"left", "right", "up", "down", "jump", "attack"};
const char* kKeyLabels[] = {"左", "右", "上", "下", "跳", "攻击"};

int KeyIndex(const std::string& k) {
  for (int i = 0; i < 6; ++i) {
    if (k == kKeys[i]) return i;
  }
  return 0;
}

void KeyCombo(const char* id, std::string* out) {
  int idx = KeyIndex(*out);
  ImGui::SetNextItemWidth(80.0f);
  if (ImGui::Combo(id, &idx, kKeyLabels, 6)) *out = kKeys[idx];
}

void BindObjectAnimation(pgmmv::ObjectDoc* obj, const pgmmv::AnimationDoc& ad) {
  obj->animation = ad.id;
  obj->image = ad.image;
  obj->frame.w = ad.frame_w;
  obj->frame.h = ad.frame_h;
  MarkDirty();
}

void AnimCombo(const char* id, pgmmv::ObjectDoc* obj, std::string* out) {
  if (obj == nullptr) return;
  const pgmmv::AnimationDoc* ad = pgmmv::FindAnimation(g_project, obj->animation);
  if (ad == nullptr) {
    // 对象还没绑动画组时，在“播动画”动作里直接选组；选中后帧尺寸/图像立刻按动画资源更新。
    ImGui::TextDisabled("动画组");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    int gidx = 0;
    std::vector<const char*> groups;
    groups.push_back("(选动画组)");
    for (const pgmmv::AnimationDoc& g : g_project.animations) {
      groups.push_back(g.name.c_str());
    }
    if (ImGui::Combo("##an_group", &gidx, groups.data(),
                     static_cast<int>(groups.size())) &&
        gidx > 0) {
      BindObjectAnimation(obj, g_project.animations[static_cast<size_t>(gidx - 1)]);
      ad = pgmmv::FindAnimation(g_project, obj->animation);
    }
    ImGui::SameLine();
  }
  std::vector<std::string> names;
  std::vector<std::string> values;
  names.push_back("(选动画)");
  values.push_back("");
  int idx = 0;
  if (ad != nullptr) {
    for (const pgmmv::ActionDef& a : ad->actions) {
      names.push_back(a.name + " / " + a.base_variant);
      values.push_back(a.name);
      if (a.name == *out) idx = static_cast<int>(values.size()) - 1;
      for (const pgmmv::DirectionDef& d : a.directions) {
        names.push_back(a.name + " / " + d.name);
        values.push_back(a.name + "@" + d.name);
        if (*out == values.back()) idx = static_cast<int>(values.size()) - 1;
      }
    }
  }
  std::vector<const char*> labels;
  for (const std::string& n : names) labels.push_back(n.c_str());
  ImGui::SetNextItemWidth(130.0f);
  if (ImGui::Combo(id, &idx, labels.data(), static_cast<int>(labels.size()))) {
    *out = values[static_cast<size_t>(idx)];
  }
}

void DrawCond(pgmmv::ObjectDoc* obj, pgmmv::Cond* c, int uid) {
  (void)obj;
  ImGui::PushID(uid);
  std::vector<const char*> labels;
  int idx = 0;
  for (size_t i = 0; i < sizeof(kConds) / sizeof(kConds[0]); ++i) {
    labels.push_back(kConds[i].label);
    if (kConds[i].type == c->type) idx = static_cast<int>(i);
  }
  ImGui::SetNextItemWidth(96.0f);
  if (ImGui::Combo("##when", &idx, labels.data(), static_cast<int>(labels.size()))) {
    c->type = kConds[idx].type;
  }
  ImGui::SameLine();
  switch (kConds[idx].a) {
    case PKind::kKey: KeyCombo("##k", &c->key); break;
    case PKind::kVarName: {
      char vb[64];
      std::snprintf(vb, sizeof(vb), "%s", c->name.c_str());
      ImGui::SetNextItemWidth(120.0f);
      if (ImGui::InputTextWithHint("##v", "变量名", vb, sizeof(vb))) {
        c->name = vb;
        MarkDirty();
      }
      break;
    }
    case PKind::kNum: {
      ImGui::SetNextItemWidth(70.0f);
      if (ImGui::InputInt("##n", &c->value)) MarkDirty();
      break;
    }
    default: break;
  }
  if (kConds[idx].b == PKind::kNum) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::InputInt("##n2", &c->value)) MarkDirty();
  } else if (kConds[idx].b == PKind::kOp) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::Combo("##op", &c->op, kOpLabels, 5)) MarkDirty();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    if (ImGui::InputInt("##ov", &c->value)) MarkDirty();
  }
  ImGui::PopID();
}

void DrawAct(pgmmv::ObjectDoc* obj, pgmmv::Effect* e, int uid) {
  ImGui::PushID(uid);
  std::vector<const char*> labels;
  int idx = 0;
  for (size_t i = 0; i < sizeof(kActs) / sizeof(kActs[0]); ++i) {
    labels.push_back(kActs[i].label);
    if (kActs[i].type == e->type) idx = static_cast<int>(i);
  }
  ImGui::SetNextItemWidth(96.0f);
  if (ImGui::Combo("##act", &idx, labels.data(), static_cast<int>(labels.size()))) {
    e->type = kActs[idx].type;
  }
  ImGui::SameLine();
  switch (kActs[idx].type) {
    case pgmmv::RuleAct::kMove:
    case pgmmv::RuleAct::kMovePx:
    case pgmmv::RuleAct::kMoveAdd: {
      int dir = 0;
      int val = 0;
      if (e->y == 0 && e->x >= 0) { dir = 0; val = e->x; }
      else if (e->y == 0 && e->x < 0) { dir = 1; val = -e->x; }
      else if (e->x == 0 && e->y >= 0) { dir = 2; val = e->y; }
      else if (e->x == 0 && e->y < 0) { dir = 3; val = -e->y; }
      else { dir = 4; val = std::max(std::abs(e->x), std::abs(e->y)); }
      const char* dirs[] = {"+X", "-X", "+Y", "-Y", "X/Y"};
      ImGui::SetNextItemWidth(78.0f);
      if (ImGui::Combo("##move_dir", &dir, dirs, 5)) {
        if (dir == 4) {
          if (e->x == 0 && e->y == 0) e->x = val;
        } else if (dir == 0) { e->x = std::abs(val); e->y = 0; }
        else if (dir == 1) { e->x = -std::abs(val); e->y = 0; }
        else if (dir == 2) { e->x = 0; e->y = std::abs(val); }
        else { e->x = 0; e->y = -std::abs(val); }
        MarkDirty();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("速度");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(80.0f);
      if (ImGui::InputInt("##move_val", &val)) {
        val = std::max(0, val);
        if (dir == 0) { e->x = val; e->y = 0; }
        else if (dir == 1) { e->x = -val; e->y = 0; }
        else if (dir == 2) { e->x = 0; e->y = val; }
        else if (dir == 3) { e->x = 0; e->y = -val; }
        MarkDirty();
      }
      if (dir == 4) {
        ImGui::SameLine();
        ImGui::TextDisabled("X");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        if (ImGui::InputInt("##move_x", &e->x)) MarkDirty();
        ImGui::SameLine();
        ImGui::TextDisabled("Y");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64.0f);
        if (ImGui::InputInt("##move_y", &e->y)) MarkDirty();
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("+X/-X 水平，+Y/-Y 垂直；X/Y 用于斜向自定义。");
      }
      break;
    }
    case pgmmv::RuleAct::kMoveInput: {
      ImGui::TextDisabled("X");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(64.0f);
      if (ImGui::InputInt("##x", &e->x)) MarkDirty();
      ImGui::SameLine();
      ImGui::TextDisabled("Y");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(64.0f);
      if (ImGui::InputInt("##y", &e->y)) MarkDirty();
      break;
    }
    case pgmmv::RuleAct::kAnim: AnimCombo("##an", obj, &e->text); break;
    case pgmmv::RuleAct::kFlip: {
      int fi = e->text == "left" ? 0 : 1;
      const char* fl[] = {"左", "右"};
      ImGui::SetNextItemWidth(70.0f);
      if (ImGui::Combo("##fl", &fi, fl, 2)) e->text = fi == 0 ? "left" : "right";
      break;
    }
    case pgmmv::RuleAct::kSfx: {
      std::vector<const char*> al;
      al.push_back("(无声)");
      int ai = 0;
      int k = 0;
      for (const pgmmv::Asset& a : g_project.assets) {
        if (a.kind != pgmmv::AssetKind::kAudio) continue;
        al.push_back(a.name.c_str());
        ++k;
        if (a.id == e->text) ai = k;
      }
      ImGui::SetNextItemWidth(120.0f);
      if (ImGui::Combo("##sfx", &ai, al.data(), static_cast<int>(al.size()))) {
        e->text = ai == 0 ? std::string() : std::string(al[static_cast<size_t>(ai)]);
      }
      break;
    }
    case pgmmv::RuleAct::kSpawn: {
      std::vector<const char*> ol;
      ol.push_back("(选对象)");
      int oi = 0;
      int idx2 = 0;
      for (const pgmmv::ObjectDoc& od : g_project.objects) {
        ol.push_back(od.id.c_str());
        ++idx2;
        if (od.id == e->text) oi = idx2;
      }
      ImGui::SetNextItemWidth(110.0f);
      if (ImGui::Combo("##sp", &oi, ol.data(), static_cast<int>(ol.size()))) {
        e->text = oi == 0 ? std::string() : ol[static_cast<size_t>(oi)];
      }
      ImGui::SameLine();
      ImGui::TextDisabled("X");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(52.0f);
      if (ImGui::InputInt("##sx", &e->x)) MarkDirty();
      ImGui::SameLine();
      ImGui::TextDisabled("Y");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(52.0f);
      if (ImGui::InputInt("##sy", &e->y)) MarkDirty();
      break;
    }
    case pgmmv::RuleAct::kVarAdd:
    case pgmmv::RuleAct::kVarSet: {
      char vb[64];
      std::snprintf(vb, sizeof(vb), "%s", e->text.c_str());
      ImGui::SetNextItemWidth(120.0f);
      if (ImGui::InputTextWithHint("##var", "变量名", vb, sizeof(vb))) {
        e->text = vb;
        MarkDirty();
      }
      ImGui::SameLine();
      ImGui::TextDisabled("值");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.0f);
      if (ImGui::InputInt("##varv", &e->y)) MarkDirty();
      break;
    }
    default: break;
  }
  ImGui::PopID();
}

}  // namespace ruledui

pgmmv::Cond MakeCond(pgmmv::RuleCond t, const char* key = "") {
  pgmmv::Cond c;
  c.type = t;
  c.key = key;
  return c;
}

pgmmv::Effect MakeAct(pgmmv::RuleAct t, int x = 0, int y = 0, const char* text = "") {
  pgmmv::Effect e;
  e.type = t;
  e.x = x;
  e.y = y;
  e.text = text;
  return e;
}

// 模板：玩家（走 / 停 / 跳 / 落）
void ApplyPlayerTemplate(pgmmv::ObjectDoc* obj) {
  using C = pgmmv::RuleCond;
  using A = pgmmv::RuleAct;
  pgmmv::Rule gravity;
  gravity.name = "重力";
  gravity.when.push_back(MakeCond(C::kAlways));
  gravity.effects.push_back(MakeAct(A::kGravity));

  auto walk = [](const char* key, int vx, const char* flip) {
    pgmmv::Rule r;
    r.name = vx > 0 ? "右走" : "左走";
    r.when.push_back(MakeCond(C::kKeyDown, key));
    r.effects.push_back(MakeAct(A::kMove, vx, 0));
    r.effects.push_back(MakeAct(A::kAnim, 0, 0, "run"));
    r.effects.push_back(MakeAct(A::kFlip, 0, 0, flip));
    return r;
  };
  auto stop = [](const char* key) {
    pgmmv::Rule r;
    r.name = "停";
    r.when.push_back(MakeCond(C::kKeyUp, key));
    r.effects.push_back(MakeAct(A::kAnim, 0, 0, "idle"));
    return r;
  };
  pgmmv::Rule jump;
  jump.name = "跳";
  jump.when.push_back(MakeCond(C::kKeyPressed, "jump"));
  jump.when.push_back(MakeCond(C::kOnGround));
  jump.effects.push_back(MakeAct(A::kMove, 0, -420));
  jump.effects.push_back(MakeAct(A::kAnim, 0, 0, "jump"));
  jump.effects.push_back(MakeAct(A::kSfx, 0, 0, "jump"));

  pgmmv::Rule fall;
  fall.name = "下落";
  fall.when.push_back(MakeCond(C::kOnGroundNot));
  fall.effects.push_back(MakeAct(A::kAnim, 0, 0, "fall"));

  obj->rules.clear();
  obj->rules.push_back(gravity);
  auto run = [](const char* key, int vx) {
    pgmmv::Rule r;
    r.name = vx > 0 ? "右跑" : "左跑";
    pgmmv::Cond held;
    held.type = C::kKeyHeld;
    held.key = key;
    held.value = 30;  // 按住 30 帧（0.5 秒）后加速
    r.when.push_back(held);
    r.effects.push_back(MakeAct(A::kMove, vx, 0));
    r.effects.push_back(MakeAct(A::kAnim, 0, 0, "run"));
    return r;
  };
  obj->rules.push_back(walk("right", 110, "right"));
  obj->rules.push_back(walk("left", -110, "left"));
  obj->rules.push_back(run("right", 176));
  obj->rules.push_back(run("left", -176));
  obj->rules.push_back(stop("left"));
  obj->rules.push_back(stop("right"));
  obj->rules.push_back(jump);
  obj->rules.push_back(fall);
}

// 模板：巡逻敌人（一直走 + 定时换向，靠 set 属性当反向标记暂时不做）
void ApplyPatrolTemplate(pgmmv::ObjectDoc* obj) {
  using C = pgmmv::RuleCond;
  using A = pgmmv::RuleAct;
  pgmmv::Rule walk;
  walk.name = "巡逻";
  walk.when.push_back(MakeCond(C::kAlways));
  walk.effects.push_back(MakeAct(A::kMove, 30, 0));
  walk.effects.push_back(MakeAct(A::kAnim, 0, 0, "idle"));

  pgmmv::Rule turn;
  turn.name = "换向";
  pgmmv::Cond t;
  t.type = C::kTimer;
  t.value = 120;
  turn.when.push_back(t);
  turn.effects.push_back(MakeAct(A::kFlip, 0, 0, "left"));

  obj->rules.clear();
  obj->rules.push_back(walk);
  obj->rules.push_back(turn);
}

void DrawObjectRules(pgmmv::ObjectDoc* obj) {
  // ---- 规则表：一行一条，条件/动作内联编辑 ----
  if (ImGui::CollapsingHeader("规则", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::TextDisabled("按键驱动：当 [条件] 就 [动作]；点每个标签直接改。");
    int del_rule = -1;
    for (size_t ri = 0; ri < obj->rules.size(); ++ri) {
      pgmmv::Rule& r = obj->rules[ri];
      ImGui::PushID(static_cast<int>(ri));
      if (ImGui::Checkbox("##on", &r.enabled)) MarkDirty();
      ImGui::SameLine();
      ImGui::TextDisabled("%02zu", ri + 1);
      ImGui::SameLine();
      char rname[64];
      std::snprintf(rname, sizeof(rname), "%s", r.name.c_str());
      ImGui::SetNextItemWidth(102.0f);
      if (ImGui::InputText("##rule_name", rname, sizeof(rname))) {
        r.name = rname;
        MarkDirty();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("+ 条件")) {
        PushUndo();
        pgmmv::Cond c;
        c.type = pgmmv::RuleCond::kKeyDown;
        c.key = "right";
        r.when.push_back(c);
        MarkDirty();
      }
      ImGui::SameLine();
      if (InlineButton("+ 动作", 64)) {
        PushUndo();
        pgmmv::Effect e;
        e.type = pgmmv::RuleAct::kMove;
        r.effects.push_back(e);
        MarkDirty();
      }
      ImGui::SameLine();
      if (DangerSmallButton("删除")) del_rule = static_cast<int>(ri);

      // 条件行
      ImGui::TextColored(ImVec4(0.62f, 0.78f, 1.0f, 1.0f), "当");
      ImGui::SameLine();
      if (r.when.empty()) {
        ImGui::TextDisabled("(无条件 = 常驻)");
      }
      int del_cond = -1;
      for (size_t ci = 0; ci < r.when.size(); ++ci) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.14f, 0.22f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.18f, 0.30f, 0.46f, 1.0f));
        ruledui::DrawCond(obj, &r.when[ci],
                          static_cast<int>(ri) * 100 + 1 + static_cast<int>(ci));
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (RightSmallButton("x##c")) del_cond = static_cast<int>(ci);
        if (ci + 1 < r.when.size()) {
          ImGui::SameLine();
          ImGui::TextDisabled("且");
          ImGui::SameLine();
        }
      }
      if (del_cond >= 0) {
        PushUndo();
        r.when.erase(r.when.begin() + del_cond);
        MarkDirty();
      }

      // 动作行
      ImGui::TextColored(ImVec4(0.62f, 0.90f, 0.68f, 1.0f), "就");
      ImGui::SameLine();
      if (r.effects.empty()) {
        ImGui::TextDisabled("(无动作)");
      }
      int del_act = -1;
      for (size_t ai = 0; ai < r.effects.size(); ++ai) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.16f, 0.28f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.22f, 0.38f, 0.26f, 1.0f));
        ruledui::DrawAct(obj, &r.effects[ai],
                         static_cast<int>(ri) * 100 + 10 + static_cast<int>(ai));
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (RightSmallButton("x##a")) del_act = static_cast<int>(ai);
        if (ai + 1 < r.effects.size()) {
          ImGui::SameLine();
          ImGui::TextDisabled("·");
          ImGui::SameLine();
        }
      }
      if (del_act >= 0) {
        PushUndo();
        r.effects.erase(r.effects.begin() + del_act);
        MarkDirty();
      }
      ImGui::Separator();
      ImGui::PopID();
    }
    if (del_rule >= 0) {
      PushUndo();
      obj->rules.erase(obj->rules.begin() + del_rule);
      MarkDirty();
    }
    if (ImGui::Button("模板：玩家", ImVec2(110, 22))) {
      PushUndo();
      ApplyPlayerTemplate(obj);
      MarkDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("模板：巡逻敌人", ImVec2(130, 22))) {
      PushUndo();
      ApplyPatrolTemplate(obj);
      MarkDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("+ 规则", ImVec2(80, 22))) {
      PushUndo();
      pgmmv::Rule r;
      r.name = "rule";
      pgmmv::Cond c;
      c.type = pgmmv::RuleCond::kKeyDown;
      c.key = "right";
      r.when.push_back(c);
      pgmmv::Effect e;
      e.type = pgmmv::RuleAct::kMove;
      r.effects.push_back(e);
      obj->rules.push_back(r);
      MarkDirty();
    }
  }
}

void DrawObjectPageCenter() {
  pgmmv::ObjectDoc* obj = CurrentObject();
  if (obj == nullptr) {
    ImGui::TextDisabled("左侧新建或选择一个对象");
    return;
  }
  ImGui::BeginChild("##obj_center", ImVec2(0, 0), false);
  DrawObjectRules(obj);
  ImGui::EndChild();
}

void DrawObjectPageRight() {
  ImGui::TextDisabled("对象不再有属性表；状态请用全局变量和规则表达");
}

// ------------------------- 界面页 -------------------------
pgmmv::UiScreenDoc* CurrentUiScreen() {
  if (!g_has_project) return nullptr;
  for (pgmmv::UiScreenDoc& s : g_project.ui_screens) {
    if (s.id == g_selected_ui_screen) return &s;
  }
  if (!g_project.ui_screens.empty()) {
    g_selected_ui_screen = g_project.ui_screens.front().id;
    return &g_project.ui_screens.front();
  }
  return nullptr;
}

pgmmv::UiElementDoc* CurrentUiElement() {
  pgmmv::UiScreenDoc* s = CurrentUiScreen();
  if (s == nullptr) return nullptr;
  if (g_selected_ui_element < 0 ||
      g_selected_ui_element >= static_cast<int>(s->elements.size())) {
    return nullptr;
  }
  return &s->elements[static_cast<size_t>(g_selected_ui_element)];
}

std::string UniqueUiId(const char* prefix) {
  for (int i = 1; i < 10000; ++i) {
    const std::string id = std::string(prefix) + std::to_string(i);
    bool used = false;
    for (const pgmmv::UiScreenDoc& s : g_project.ui_screens) {
      if (s.id == id) { used = true; break; }
    }
    if (!used) return id;
  }
  return std::string(prefix) + "x";
}

std::string UniqueUiElementId(const pgmmv::UiScreenDoc& s, const char* prefix) {
  for (int i = 1; i < 10000; ++i) {
    const std::string id = std::string(prefix) + std::to_string(i);
    bool used = false;
    for (const pgmmv::UiElementDoc& e : s.elements) {
      if (e.id == id) { used = true; break; }
    }
    if (!used) return id;
  }
  return std::string(prefix) + "x";
}

const char* kUiKinds[] = {"panel", "text", "image", "value_image", "value_bar", "button", "list"};
const char* kUiAnchors[] = {"top_left",  "top_center",  "top_right",
                            "mid_left",  "center",      "mid_right",
                            "bottom_left", "bottom_center", "bottom_right"};

ImVec2 UiElementSize(const pgmmv::UiElementDoc& e) {
  float w = e.w > 0 ? static_cast<float>(e.w) : 32.0f;
  float h = e.h > 0 ? static_cast<float>(e.h) : 16.0f;
  if (e.kind == "text") {
    const ImVec2 ts = ImGui::CalcTextSize(e.text.c_str());
    if (e.w <= 0) w = ts.x;
    if (e.h <= 0) h = ts.y;
  }
  if (e.kind == "list") {
    if (e.w <= 0) w = 160.0f;
    if (e.h <= 0) h = 80.0f;
  }
  return ImVec2(w, h);
}

ImVec2 UiElementPos(const pgmmv::UiElementDoc& e, ImVec2 lp, ImVec2 ls,
                    ImVec2 sz) {
  int col = 0;
  int row = 1;
  if (e.anchor == "top_left" || e.anchor == "top_center" ||
      e.anchor == "top_right") {
    row = 0;
  } else if (e.anchor == "bottom_left" || e.anchor == "bottom_center" ||
             e.anchor == "bottom_right") {
    row = 2;
  }
  if (e.anchor == "top_right" || e.anchor == "mid_right" ||
      e.anchor == "bottom_right") {
    col = 2;
  } else if (e.anchor == "top_center" || e.anchor == "center" ||
             e.anchor == "bottom_center") {
    col = 1;
  }
  return ImVec2(lp.x + (ls.x - sz.x) * col / 2 + e.x,
                lp.y + (ls.y - sz.y) * row / 2 + e.y);
}

void DrawUiPageLeft() {
  if (!g_has_project) {
    ImGui::TextDisabled("先打开项目");
    return;
  }
  ImGui::TextUnformatted("界面列表");
  if (RowButton("+ 新界面")) {
    PushUndo();
    pgmmv::UiScreenDoc s;
    s.id = UniqueUiId("ui_");
    s.name = "界面";
    g_project.ui_screens.push_back(s);
    g_selected_ui_screen = s.id;
    g_selected_ui_element = -1;
    MarkDirty();
  }
  ImGui::Separator();
  for (const pgmmv::UiScreenDoc& s : g_project.ui_screens) {
    const std::string label = s.name.empty() ? s.id : s.name + " (" + s.id + ")";
    if (ImGui::Selectable(label.c_str(), s.id == g_selected_ui_screen)) {
      g_selected_ui_screen = s.id;
      g_selected_ui_element = -1;
    }
  }
  pgmmv::UiScreenDoc* s = CurrentUiScreen();
  if (s == nullptr) return;
  ImGui::Separator();
  ImGui::TextUnformatted("元素");
  if (RowButton("+ 元素")) {
    PushUndo();
    pgmmv::UiElementDoc e;
    e.id = UniqueUiElementId(*s, "el_");
    e.kind = "text";
    e.text = "Text";
    s->elements.push_back(e);
    g_selected_ui_element = static_cast<int>(s->elements.size()) - 1;
    MarkDirty();
  }
  if (DangerRowButton("删除元素") && g_selected_ui_element >= 0 &&
      g_selected_ui_element < static_cast<int>(s->elements.size())) {
    PushUndo();
    s->elements.erase(s->elements.begin() + g_selected_ui_element);
    g_selected_ui_element = -1;
    MarkDirty();
  }
  for (int i = 0; i < static_cast<int>(s->elements.size()); ++i) {
    const pgmmv::UiElementDoc& e = s->elements[static_cast<size_t>(i)];
    const std::string label = e.kind + "  " + e.id;
    if (ImGui::Selectable(label.c_str(), g_selected_ui_element == i)) {
      g_selected_ui_element = i;
    }
  }
  ImGui::Separator();
  if (DangerRowButton("删除界面")) {
    PushUndo();
    for (size_t i = 0; i < g_project.ui_screens.size(); ++i) {
      if (g_project.ui_screens[i].id == g_selected_ui_screen) {
        g_project.ui_screens.erase(g_project.ui_screens.begin() + i);
        break;
      }
    }
    g_selected_ui_screen.clear();
    g_selected_ui_element = -1;
    MarkDirty();
  }
}

ImU32 UiColor(const pgmmv::UiElementDoc& e) {
  return IM_COL32((e.color >> 24) & 0xFF, (e.color >> 16) & 0xFF,
                  (e.color >> 8) & 0xFF, e.color & 0xFF);
}

// 编辑器预览：按 9-slice 平铺（不拉伸边/中间），和运行时一致。
void DrawUiPreviewImage(ImDrawList* dl, const pgmmv::UiElementDoc& e, ImVec2 p,
                        ImVec2 sz) {
  if (e.image.empty()) return;
  const pgmmv::Asset* a = g_project.FindAsset(e.image);
  if (a == nullptr) return;
  PreviewTexture* pv = AssetPreview(*a);
  if (pv == nullptr || pv->srv == nullptr || pv->width <= 0 || pv->height <= 0) {
    return;
  }
  const ImTextureID tex = reinterpret_cast<ImTextureID>(pv->srv);
  const int sw = e.src_w > 0 ? e.src_w : pv->width;
  const int sh = e.src_h > 0 ? e.src_h : pv->height;
  if (sw <= 0 || sh <= 0) return;
  const int sx = e.src_x;
  const int sy = e.src_y;
  const ImU32 col = UiColor(e);
  auto add = [&](ImVec2 dp, ImVec2 ds, int ux, int uy, int uw, int uh) {
    if (ds.x <= 0.0f || ds.y <= 0.0f) return;
    dl->AddImage(tex, dp, ImVec2(dp.x + ds.x, dp.y + ds.y),
                 ImVec2(static_cast<float>(sx + ux) / pv->width,
                        static_cast<float>(sy + uy) / pv->height),
                 ImVec2(static_cast<float>(sx + ux + uw) / pv->width,
                        static_cast<float>(sy + uy + uh) / pv->height),
                 col);
  };
  const int l = e.slice_l;
  const int r = e.slice_r;
  const int t = e.slice_t;
  const int b = e.slice_b;
  if (l <= 0 || t <= 0 || l + r >= sw || t + b >= sh) {
    add(p, sz, 0, 0, sw, sh);
    return;
  }
  const float kx = sz.x / static_cast<float>(sw);
  const float ky = sz.y / static_cast<float>(sh);
  const float dl_ = l * kx;
  const float dr = r * kx;
  const float dt = t * ky;
  const float db = b * ky;
  add(p, ImVec2(dl_, dt), 0, 0, l, t);
  add(ImVec2(p.x + sz.x - dr, p.y), ImVec2(dr, dt), sw - r, 0, r, t);
  add(ImVec2(p.x, p.y + sz.y - db), ImVec2(dl_, db), 0, sh - b, l, b);
  add(ImVec2(p.x + sz.x - dr, p.y + sz.y - db), ImVec2(dr, db), sw - r, sh - b,
      r, b);
  for (float x = dl_; x < sz.x - dr;) {
    const float tw = std::min(dl_, sz.x - dr - x);
    add(ImVec2(p.x + x, p.y), ImVec2(tw, dt), l, 0, l, t);
    add(ImVec2(p.x + x, p.y + sz.y - db), ImVec2(tw, db), l, sh - b, l, b);
    x += tw;
  }
  for (float y = dt; y < sz.y - db;) {
    const float th = std::min(dt, sz.y - db - y);
    add(ImVec2(p.x, p.y + y), ImVec2(dl_, th), 0, t, l, t);
    add(ImVec2(p.x + sz.x - dr, p.y + y), ImVec2(dr, th), sw - r, t, r, t);
    y += th;
  }
  if (e.slice_tile) {
    const float cw = (sw - l - r) * kx;
    const float ch = (sh - t - b) * ky;
    for (float y = 0; y < sz.y - dt - db; y += ch) {
      const float th = std::min(ch, sz.y - dt - db - y);
      for (float x = 0; x < sz.x - dl_ - dr; x += cw) {
        const float tw = std::min(cw, sz.x - dl_ - dr - x);
        add(ImVec2(p.x + dl_ + x, p.y + dt + y), ImVec2(tw, th), l, t, sw - l - r,
            sh - t - b);
      }
    }
  } else {
    dl->AddRectFilled(ImVec2(p.x + dl_, p.y + dt),
                      ImVec2(p.x + sz.x - dr, p.y + sz.y - db), col);
  }
}

void DrawUiPageCenter() {
  pgmmv::UiScreenDoc* s = CurrentUiScreen();
  if (s == nullptr) {
    ImGui::TextDisabled("左侧选一个界面");
    return;
  }
  ImGui::Text("预览 480×272  ·  %s", s->name.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("（点元素选中）");
  ImVec2 avail = ImGui::GetContentRegionAvail();
  if (avail.x < 64.0f || avail.y < 64.0f) return;
  float scale = std::min(avail.x / 480.0f, avail.y / 272.0f);
  scale = std::min(scale, 3.0f);
  if (scale < 0.5f) scale = 0.5f;
  const ImVec2 canvas(480.0f * scale, 272.0f * scale);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + canvas.x, origin.y + canvas.y),
                    IM_COL32(0, 0, 0, 255));
  dl->AddRect(origin, ImVec2(origin.x + canvas.x, origin.y + canvas.y),
              IM_COL32(90, 90, 90, 255));
  ImVec2 lp = origin;
  ImVec2 ls = canvas;
  if (s->layout == "safe") {
    ls = ImVec2(400.0f * scale, 240.0f * scale);
    lp = ImVec2(origin.x + (canvas.x - ls.x) / 2,
                origin.y + (canvas.y - ls.y) / 2);
    dl->AddRect(lp, ImVec2(lp.x + ls.x, lp.y + ls.y),
                IM_COL32(80, 150, 240, 255));
  }
  const bool clicked = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0);
  for (int i = 0; i < static_cast<int>(s->elements.size()); ++i) {
    const pgmmv::UiElementDoc& e = s->elements[static_cast<size_t>(i)];
    ImVec2 sz = UiElementSize(e);
    sz = ImVec2(sz.x * scale, sz.y * scale);
    const ImVec2 p = UiElementPos(e, lp, ls, sz);
    const ImU32 col = UiColor(e);
    if (!e.image.empty() && (e.kind == "panel" || e.kind == "image" ||
                             e.kind == "value_image")) {
      DrawUiPreviewImage(dl, e, p, sz);
    } else {
      dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y),
                        (col & 0x00FFFFFFu) | (50u << 24));
    }
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), col);
    if (e.kind == "text") {
      dl->AddText(ImGui::GetFont(), static_cast<float>(e.font_size),
                  ImVec2(p.x, p.y), col, e.text.c_str());
    } else if (e.kind == "list") {
      const int row_h = e.font_size + 4;
      for (int r = 0; r < static_cast<int>(e.items.size()); ++r) {
        const float ty = p.y + 2.0f + static_cast<float>(r * row_h) * scale;
        if (ty + static_cast<float>(row_h) * scale > p.y + sz.y) break;
        dl->AddText(ImGui::GetFont(), static_cast<float>(e.font_size) * scale,
                    ImVec2(p.x + 2.0f, ty), col,
                    e.items[static_cast<size_t>(r)].c_str());
      }
    }
    if (g_selected_ui_element == i) {
      const ImVec2 h(p.x + sz.x - 5.0f, p.y + sz.y - 5.0f);
      dl->AddRectFilled(h, ImVec2(h.x + 10.0f, h.y + 10.0f),
                        IM_COL32(255, 210, 90, 255));
      if (clicked && ImGui::IsMouseHoveringRect(h, ImVec2(h.x + 10.0f, h.y + 10.0f))) {
        g_ui_resizing = true;
        g_ui_dragging = false;
        g_ui_resize_w0 = static_cast<int>(UiElementSize(e).x);
        g_ui_resize_h0 = static_cast<int>(UiElementSize(e).y);
        g_ui_resize_mouse0 = ImGui::GetIO().MousePos;
      }
    }
    if (!g_ui_resizing && clicked &&
        ImGui::IsMouseHoveringRect(p, ImVec2(p.x + sz.x, p.y + sz.y))) {
      g_selected_ui_element = i;
      g_ui_dragging = true;
    }
  }
  if (g_ui_resizing) {
    if (ImGui::IsMouseDown(0)) {
      pgmmv::UiElementDoc* e = CurrentUiElement();
      if (e != nullptr) {
        const ImVec2 delta = ImGui::GetIO().MousePos;
        const int nw = g_ui_resize_w0 + static_cast<int>(
                           std::round((delta.x - g_ui_resize_mouse0.x) / scale));
        const int nh = g_ui_resize_h0 + static_cast<int>(
                           std::round((delta.y - g_ui_resize_mouse0.y) / scale));
        e->w = std::max(1, nw);
        e->h = std::max(1, nh);
        MarkDirty();
      }
    } else {
      g_ui_resizing = false;
    }
  } else if (g_ui_dragging) {
    if (ImGui::IsMouseDown(0)) {
      const ImVec2 delta = ImGui::GetIO().MouseDelta;
      pgmmv::UiElementDoc* e = CurrentUiElement();
      if (e != nullptr && (delta.x != 0.0f || delta.y != 0.0f)) {
        e->x += static_cast<int>(std::round(delta.x / scale));
        e->y += static_cast<int>(std::round(delta.y / scale));
        MarkDirty();
      }
    } else {
      g_ui_dragging = false;
    }
  }
  ImGui::Dummy(canvas);
}

void DrawUiPageRight() {
  pgmmv::UiScreenDoc* s = CurrentUiScreen();
  if (s == nullptr) {
    ImGui::TextDisabled("左侧选一个界面");
    return;
  }
  pgmmv::UiElementDoc* e = CurrentUiElement();
  if (e == nullptr) {
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s", s->name.c_str());
    if (ImGui::InputText("界面名", name_buf, sizeof(name_buf))) {
      s->name = name_buf;
      MarkDirty();
    }
    if (ImGui::Checkbox("模态（暂停 + 锁输入）", &s->modal)) MarkDirty();
    int layout = (s->layout == "safe") ? 1 : 0;
    const char* layouts[] = {"screen", "safe"};
    if (ImGui::Combo("布局区", &layout, layouts, 2)) {
      s->layout = layouts[layout];
      MarkDirty();
    }
    const char* dismiss_labels[] = {"确认关闭", "取消关闭", "定时关闭", "不自动关"};
    int dm = std::max(0, std::min(3, s->dismiss));
    if (ImGui::Combo("关闭方式", &dm, dismiss_labels, 4)) {
      s->dismiss = dm;
      MarkDirty();
    }
    if (s->dismiss == 2) {
      if (UndoIntField("多少帧后关", &s->dismiss_frames, 1)) MarkDirty();
    }
    if (UndoIntField("淡入帧数", &s->fade_frames, 0)) MarkDirty();
    if (UndoIntField("淡出帧数", &s->fade_out_frames, 0)) MarkDirty();
    ImGui::Separator();
    ImGui::TextUnformatted("挂载");
    bool is_title = (g_project.title_ui == s->id);
    if (ImGui::Checkbox("标题界面", &is_title)) {
      g_project.title_ui = is_title ? s->id : "";
      MarkDirty();
    }
    bool persistent = (g_project.persistent_ui == s->id);
    if (ImGui::Checkbox("工程常驻", &persistent)) {
      g_project.persistent_ui = persistent ? s->id : "";
      MarkDirty();
    }
    for (pgmmv::SceneDoc& sc : g_project.scenes) {
      bool is_hud = (sc.hud_ui == s->id);
      const std::string label = "场景 HUD: " + (sc.name.empty() ? sc.id : sc.name);
      if (ImGui::Checkbox(label.c_str(), &is_hud)) {
        sc.hud_ui = is_hud ? s->id : "";
        MarkDirty();
      }
    }
    ImGui::TextDisabled("选中一个元素后编辑属性");
    return;
  }
  ImGui::Text("元素：%s", e->id.c_str());
  char id_buf[64];
  std::snprintf(id_buf, sizeof(id_buf), "%s", e->id.c_str());
  if (ImGui::InputText("id", id_buf, sizeof(id_buf))) {
    e->id = id_buf;
    MarkDirty();
  }
  int kind = 0;
  for (int i = 0; i < 7; ++i) if (e->kind == kUiKinds[i]) kind = i;
  if (ImGui::Combo("类型", &kind, kUiKinds, 7)) {
    e->kind = kUiKinds[kind];
    MarkDirty();
  }
  if (e->kind == "image" || e->kind == "panel" || e->kind == "value_image") {
    const char* cur = e->image.empty() ? "(无)" : e->image.c_str();
    if (ImGui::BeginCombo("图片", cur)) {
      if (ImGui::Selectable("(无)", e->image.empty())) {
        e->image.clear();
        MarkDirty();
      }
      for (const pgmmv::Asset& a : g_project.assets) {
        if (a.kind != pgmmv::AssetKind::kImage) continue;
        const bool sel = (e->image == a.id);
        const std::string label = a.name.empty() ? a.id : a.name;
        if (ImGui::Selectable(label.c_str(), sel)) {
          e->image = a.id;
          e->src_x = 0;
          e->src_y = 0;
          e->src_w = a.width;
          e->src_h = a.height;
          MarkDirty();
        }
      }
      ImGui::EndCombo();
    }
  }
  int anchor = 0;
  for (int i = 0; i < 9; ++i) if (e->anchor == kUiAnchors[i]) anchor = i;
  if (ImGui::Combo("锚点", &anchor, kUiAnchors, 9)) {
    e->anchor = kUiAnchors[anchor];
    MarkDirty();
  }
  if (UndoIntField("X", &e->x, -10000)) MarkDirty();
  if (UndoIntField("Y", &e->y, -10000)) MarkDirty();
  if (UndoIntField("宽（0 = 内容）", &e->w, 0)) MarkDirty();
  if (UndoIntField("高（0 = 内容）", &e->h, 0)) MarkDirty();
  if (e->kind == "text" || e->kind == "panel" || e->kind == "button") {
    char text_buf[512];
    std::snprintf(text_buf, sizeof(text_buf), "%s", e->text.c_str());
    if (ImGui::InputTextMultiline("文字", text_buf, sizeof(text_buf),
                                  ImVec2(-1, 60))) {
      e->text = text_buf;
      MarkDirty();
    }
    const char* kFontSizes[] = {"12", "14", "16"};
    int fs = e->font_size >= 16 ? 2 : (e->font_size >= 14 ? 1 : 0);
    if (ImGui::Combo("字号", &fs, kFontSizes, 3)) {
      e->font_size = 12 + fs * 2;
      MarkDirty();
    }
    const char* aligns[] = {"left", "center", "right"};
    int align = std::max(0, std::min(2, e->align));
    if (ImGui::Combo("对齐", &align, aligns, 3)) {
      e->align = align;
      MarkDirty();
    }
  }
  float col[4] = {((e->color >> 24) & 0xFF) / 255.0f,
                  ((e->color >> 16) & 0xFF) / 255.0f,
                  ((e->color >> 8) & 0xFF) / 255.0f,
                  (e->color & 0xFF) / 255.0f};
  if (ImGui::ColorEdit4("颜色", col)) {
    e->color = (static_cast<unsigned>(col[0] * 255.0f + 0.5f) << 24) |
               (static_cast<unsigned>(col[1] * 255.0f + 0.5f) << 16) |
               (static_cast<unsigned>(col[2] * 255.0f + 0.5f) << 8) |
               static_cast<unsigned>(col[3] * 255.0f + 0.5f);
    MarkDirty();
  }
  if (e->kind == "button" || e->kind == "list") {
    if (ImGui::Checkbox("有存档才显示", &e->only_if_save)) MarkDirty();
  }
  if (e->kind == "button") {
    const char* actions[] = {"close", "open_ui", "set_var", "scene", "new_game",
                             "load"};
    const char* action_labels[] = {"关闭界面", "打开界面", "设变量", "切场景",
                                   "新游戏", "读档"};
    int ai = 0;
    for (int i = 0; i < 6; ++i) if (e->action == actions[i]) ai = i;
    if (ImGui::Combo("按钮动作", &ai, action_labels, 6)) {
      e->action = actions[ai];
      MarkDirty();
    }
    if (e->action == "open_ui") {
      const char* cur = e->action_target.empty() ? "(选界面)" : e->action_target.c_str();
      if (ImGui::BeginCombo("目标界面", cur)) {
        for (const pgmmv::UiScreenDoc& us : g_project.ui_screens) {
          const std::string label = us.name.empty() ? us.id : us.name;
          if (ImGui::Selectable(label.c_str(), e->action_target == us.id)) {
            e->action_target = us.id;
            MarkDirty();
          }
        }
        ImGui::EndCombo();
      }
    } else if (e->action == "close" || e->action == "new_game" ||
               e->action == "load") {
      ImGui::TextDisabled(e->action == "load" ? "(读取 save0.dat)"
                                                : "(无需目标)");
    } else {
      char target_buf[96];
      std::snprintf(target_buf, sizeof(target_buf), "%s", e->action_target.c_str());
      if (ImGui::InputText(e->action == "scene" ? "目标场景" : "变量名", target_buf,
                           sizeof(target_buf))) {
        e->action_target = target_buf;
        MarkDirty();
      }
      if (e->action == "set_var") {
        if (UndoIntField("变量值", &e->action_value, -10000)) MarkDirty();
      }
    }
  }
  if (e->kind == "list") {
    std::string joined;
    for (const std::string& item : e->items) {
      if (!joined.empty()) joined += '\n';
      joined += item;
    }
    char items_buf[1024];
    std::snprintf(items_buf, sizeof(items_buf), "%s", joined.c_str());
    if (ImGui::InputTextMultiline("列表项（每行一个）", items_buf,
                                  sizeof(items_buf), ImVec2(-1, 100))) {
      e->items.clear();
      std::string cur;
      for (const char* p = items_buf; *p != '\0'; ++p) {
        if (*p == '\n') {
          if (!cur.empty()) e->items.push_back(cur);
          cur.clear();
        } else if (*p != '\r') {
          cur += *p;
        }
      }
      if (!cur.empty()) e->items.push_back(cur);
      MarkDirty();
    }
    const char* actions[] = {"close", "open_ui", "set_var", "scene"};
    const char* labels[] = {"关闭界面", "打开界面", "设变量", "切场景"};
    int ai = 0;
    for (int i = 0; i < 4; ++i) if (e->action == actions[i]) ai = i;
    if (ImGui::Combo("列表动作", &ai, labels, 4)) {
      e->action = actions[ai];
      MarkDirty();
    }
    if (e->action == "set_var") {
      char target_buf[96];
      std::snprintf(target_buf, sizeof(target_buf), "%s", e->action_target.c_str());
      if (ImGui::InputText("变量名", target_buf, sizeof(target_buf))) {
        e->action_target = target_buf;
        MarkDirty();
      }
    } else if (e->action == "open_ui" || e->action == "scene") {
      ImGui::TextDisabled("用选中行的文字作为目标 id");
    } else {
      ImGui::TextDisabled("(确认后关闭当前界面)");
    }
  }
  if (e->kind == "value_bar" || e->kind == "value_image") {
    char var_buf[64];
    std::snprintf(var_buf, sizeof(var_buf), "%s", e->value_var.c_str());
    if (ImGui::InputText("取值变量", var_buf, sizeof(var_buf))) {
      e->value_var = var_buf;
      MarkDirty();
    }
    if (UndoIntField("上限", &e->out_max, 1)) MarkDirty();
    if (UndoIntField("帧数", &e->frames, 1)) MarkDirty();
    char fmt_buf[64];
    std::snprintf(fmt_buf, sizeof(fmt_buf), "%s", e->format.c_str());
    if (ImGui::InputText("格式", fmt_buf, sizeof(fmt_buf))) {
      e->format = fmt_buf;
      MarkDirty();
    }
  }
  if (ImGui::CollapsingHeader("九宫格 / 容器")) {
    if (UndoIntField("左", &e->slice_l, 0)) MarkDirty();
    if (UndoIntField("右", &e->slice_r, 0)) MarkDirty();
    if (UndoIntField("上", &e->slice_t, 0)) MarkDirty();
    if (UndoIntField("下", &e->slice_b, 0)) MarkDirty();
    if (ImGui::Checkbox("中间平铺", &e->slice_tile)) MarkDirty();
    if (UndoIntField("padding", &e->padding, 0)) MarkDirty();
  }
  if (ImGui::CollapsingHeader("显示条件")) {
    char vis_buf[64];
    std::snprintf(vis_buf, sizeof(vis_buf), "%s", e->visible_var.c_str());
    if (ImGui::InputText("变量", vis_buf, sizeof(vis_buf))) {
      e->visible_var = vis_buf;
      MarkDirty();
    }
    if (UndoIntField("等于", &e->visible_value, -10000)) MarkDirty();
  }
}

void DrawShell() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("##shell", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_MenuBar |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar();
  DrawMenuBar();
  DrawProjectSettingsModal();
  DrawConfirmModal();
  DrawRenameObjectModal();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
  DrawPgTabs();
  // ★ 一键试玩：导出当前工程并启动播放器 ★
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
  if (ImGui::Button("试玩 (F5)", ImVec2(96, 30)) && g_has_project) DoPlay();
  ImGui::PopStyleVar();

  const float status_h = 32.0f;
  float subtab_h = 0.0f;
  if (g_current_tool == 4) {
    DrawResourceTabs();
    subtab_h = 32.0f;
  }
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float content_h = avail.y - status_h - subtab_h;
  const float left_w = 258.0f;
  float right_w = 380.0f;
  if (g_current_tool == 3) right_w = 430.0f;
  if (g_current_tool == 0 || g_current_tool == 1) right_w = 360.0f;
  const float center_w = avail.x - left_w - right_w - 12.0f;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
  ImGui::BeginChild("##left", ImVec2(left_w, content_h), ImGuiChildFlags_Borders);
  switch (g_current_tool) {
    case 0: DrawScenePageLeft(); break;
    case 1: DrawTilePageLeft(); break;
    case 2: DrawAnimListLeft(); break;
    case 3: DrawObjectLeft(); break;
    case 4: DrawResourceLeft2(); break;
    case 5: DrawUiPageLeft(); break;
    default: DrawPlaceholder(ToolName(g_current_tool)); break;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##center", ImVec2(center_w, content_h), ImGuiChildFlags_Borders);
  switch (g_current_tool) {
    case 0: DrawScenePageCenter(); break;
    case 1: DrawTilePageCenter(); break;
    case 2: DrawObjectAnimation(); break;
    case 3: DrawObjectPageCenter(); break;
    case 4: DrawResourceCenter2(); break;
    case 5: DrawUiPageCenter(); break;
    default: DrawPlaceholder(ToolName(g_current_tool)); break;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##right", ImVec2(right_w, content_h), ImGuiChildFlags_Borders);
  switch (g_current_tool) {
    case 0: DrawScenePageRight(); break;
    case 1: DrawTilePageRight(); break;
    case 2: {
      // ★ 动画页的右栏 = 属性栏：只放动画自己的东西 ★
      //   对象属性（图像/帧宽高/碰撞盒/音效/类型…）在「对象」页，这里不再重复一遍。
      DrawActionSettings();
      ImGui::Separator();
      DrawDetectionSettings();
      break;
    }
    case 3: DrawObjectPageRight(); break;
    case 4: DrawResourceRight2(); break;
    case 5: DrawUiPageRight(); break;
    default: DrawPlaceholder(ToolName(g_current_tool)); break;
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  ImGui::Separator();
  ImGui::TextDisabled("%s  |  %s",
                      g_has_project ? g_project.name.c_str() : "未打开项目",
                      ToolName(g_current_tool));
  ImGui::SameLine(ImGui::GetWindowWidth() - 260.0f);
  ImGui::TextDisabled("%s", g_has_project ? ProjectFilePath(g_project).u8string().c_str() : "");
  ImGui::End();
}

}  // namespace

int main(int argc, char** argv) {
  SetProcessDPIAware();
  // shell 对话框（选文件夹/选文件）要 COM；没有它 SHBrowseForFolder 会空白卡死。
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::string open_path;
  std::string new_project_dir;
  bool pick_folder_selftest = false;
  bool list_scenes = false;
  std::string import_png;
  std::string export_p3d;
  std::string scene_id;
  std::string theme_path;
  std::string ini_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--open" && i + 1 < argc) {
      open_path = argv[++i];
    } else if (a == "--new" && i + 1 < argc) {
      new_project_dir = argv[++i];
    } else if (a == "--pick-folder-selftest") {
      pick_folder_selftest = true;
    } else if (a == "--list-scenes") {
      list_scenes = true;
    } else if (a == "--smoke") {
      g_smoke = true;
    } else if (a == "--frames" && i + 1 < argc) {
      g_frames_left = std::atoi(argv[++i]);
    } else if (a == "--tool" && i + 1 < argc) {
      g_current_tool = std::max(0, std::min(5, std::atoi(argv[++i])));
    } else if (a == "--import-png" && i + 1 < argc) {
      import_png = argv[++i];
    } else if (a == "--export-p3d" && i + 1 < argc) {
      export_p3d = argv[++i];
    } else if (a == "--scene" && i + 1 < argc) {
      scene_id = argv[++i];
    } else if (a == "--theme" && i + 1 < argc) {
      theme_path = argv[++i];
    } else if (a == "--ini" && i + 1 < argc) {
      ini_path = argv[++i];
    }
  }
  if (g_smoke && g_frames_left < 0) g_frames_left = 5;

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"PocketLoomEditor";
  RegisterClassExW(&wc);

  HWND hwnd = CreateWindowW(wc.lpszClassName, L"PocketLoom Editor",
                            WS_OVERLAPPEDWINDOW, 30, 30, 1920, 1017, nullptr,
                            nullptr, wc.hInstance, nullptr);
  if (hwnd == nullptr) return 1;
  g_hwnd = hwnd;
  if (!CreateDeviceD3D(hwnd, 1920, 1017)) {
    CleanupDeviceD3D();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }
  ShowWindow(hwnd, SW_SHOWMAXIMIZED);
  UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  static std::string ini_storage;
  if (!ini_path.empty()) {
    ini_storage = ini_path;
    io.IniFilename = ini_storage.c_str();
  } else {
    io.IniFilename = nullptr;
  }
  ApplyPgStyle();
  LoadUiFont();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_d3d.device, g_d3d.ctx);
  r4::AudioInit();

  if (!new_project_dir.empty()) {
    // 无对话框建项目（给自动化和「选择文件夹」出问题时用）
    std::string err;
    if (!NewProjectAt(std::filesystem::path(new_project_dir), &err)) {
      std::fprintf(stderr, "[editor] new failed: %s\n", err.c_str());
    }
  } else if (!open_path.empty()) {
    std::string err;
    if (ProjectLoad(std::filesystem::path(open_path), &g_project, &err)) {
      OnProjectLoaded();
    } else {
      std::fprintf(stderr, "[editor] open failed: %s\n", err.c_str());
    }
  }
  if (list_scenes) {
    for (const pgmmv::SceneDoc& sc : g_project.scenes) {
      std::printf("SCENE:%s\n", sc.id.c_str());
    }
    return 0;
  }
  if (!scene_id.empty()) {
    if (g_project.FindScene(scene_id) == nullptr) {
      std::fprintf(stderr, "[editor] scene not found: %s\n", scene_id.c_str());
      return 6;
    }
    SelectScene(scene_id);
  }
  if (pick_folder_selftest) {
    // 自检：确认文件夹对话框能弹出来、能返回（不会被卡住）
    std::filesystem::path picked;
    const bool ok = PickFolder(&picked);
    std::printf("[editor] pick-folder ok=%d path=%s\n", ok ? 1 : 0, PathText(picked).c_str());
    std::fflush(stdout);
    return 0;
  }
  if (!export_p3d.empty()) {
    std::string eerr;
    if (!pgmmv::ExportP3d(g_project, std::filesystem::path(export_p3d), &eerr)) {
      std::fprintf(stderr, "[editor] export failed: %s\n", eerr.c_str());
      return 3;
    }
    std::printf("[editor] exported %s\n", export_p3d.c_str());
    return 0;
  }

  if (!import_png.empty()) {
    std::string ierr;
    if (ImportPngAt(std::filesystem::path(import_png), &ierr)) {
      std::printf("[editor] imported png, assets=%d\n",
                  static_cast<int>(g_project.assets.size()));
      if (!g_project.assets.empty()) g_selected_asset = g_project.assets.back().id;
    } else {
      std::fprintf(stderr, "[editor] import failed: %s\n", ierr.c_str());
      return 2;
    }
  }

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (g_request_exit) {
      g_request_exit = false;
      RequestAction(PendingAction::kExit);
    }
    if (done || g_quit) break;
    if (g_frames_left == 0) break;

    ExecutePending();
    if (g_quit) break;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl) {
      if (ImGui::IsKeyPressed(ImGuiKey_S, false)) DoSave();
      if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) DoPlay();
      if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) DoUndo();
      if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) DoRedo();
    }
    DrawShell();
    ImGui::Render();

    const float clear[4] = {0.180f, 0.180f, 0.180f, 1.0f};
    g_d3d.ctx->OMSetRenderTargets(1, &g_d3d.rtv, nullptr);
    g_d3d.ctx->ClearRenderTargetView(g_d3d.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_d3d.swap->Present(1, 0);

    if (g_frames_left > 0) --g_frames_left;
  }

  r4::AudioShutdown();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CleanupDeviceD3D();
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}
