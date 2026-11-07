---
name: pocketloom-workflow
description: PocketLoom 开发工作流。修改 engine/runtime/pgmmv/editor、改 p3d 或存档格式、做 demo、接入 3DS/PSP 后端时使用；先定变更轨道，再按构建、能力测试、demo 导出、打包的顺序验证，避免旧玩法回潮和格式不同步。
whenToUse: 任务涉及 D:\GitHud\GameStudio\GBAStudioMini，或提到 PocketLoom、p3d、3DSStudioMini、PSP 移植、编辑器、demo、打包时。
---

# PocketLoom 工作流

## 0. 项目事实

- 主工程：`D:\GitHud\GameStudio\GBAStudioMini`
- 3DS 参考工程：`D:\GitHud\GameStudio\3DSStudioMini-PGMMV`
- 编辑器、运行时、导出、demo、打包都在主工程。
- 3DS 参考工程是旧版本，只能参考平台层，不要整包合并。
- 当前数据版本：
  - p3d **v91**
  - 存档 **PLSV v2**
- 当前设计基线：
  - 运行时没有固定玩法：没有玩家属性、HP、分数、伤害公式、Shooter、FlowLink。
  - 所有状态只走变量表。
  - 对象没有属性表。
  - 8×8 只是编辑器最小绘制单位，不是地图底层。
  - 图块尺寸读 tileset 的 `tile_w/tile_h`。
  - 碰撞层是独立的 8px 网格。
  - 前景层会盖住对象，地砖默认放背景层。
  - UI 字体 12/14/16；12px 不能删。
- 编辑器只跑 PC，不把编辑器带进 3DS/PSP。

## 1. 开工前

1. 确认当前路径和 git 根。
2. 读最近提交和相关 docs，不要凭记忆改。
3. 给任务标轨道：

```text
[core]     runtime / p3d / game / engine base
[editor]   PC 编辑器
[data]     改 p3d、save、JSON 格式
[3ds]      只改 3DS 平台层/后端
[psp]      只改 PSP 平台层/后端
```

一次只做一条轨道。跨轨道时拆成多个可验证的小步。

## 2. 任务合同

改代码前先写清楚：

```text
目标：
涉及文件：
是否改数据格式：
p3d/save 是否升版本：
验证方式：
```

没有验证方式的任务不开始。

## 3. 实现规则

- 先 grep 引用，再删/改。
- 大删除前先提交或备份；删完立刻构建。
- 不把表现层概念写回基础层。
- 不在 runtime 里加平台头（`<3ds.h>`、PSP SDK、`windows.h`）。
- 平台差异只放：
  - `engine/platform/<platform>`
  - `engine/render/backend_<platform>`
- 旧工程不兼容是默认策略，不做无谓迁移。

## 4. 验证顺序

在主工程执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-editor.ps1
build\host\bin\capabilities.exe
```

导出三个 demo：

```powershell
build\host\bin\ploom.exe --open samples\bee\project.json --export-p3d samples\bee\bee.p3d
build\host\bin\ploom.exe --open samples\platformer\project.json --export-p3d samples\platformer\game.p3d
build\host\bin\ploom.exe --open samples\topdown\project.json --export-p3d samples\topdown\game.p3d
```

启动 smoke：

```powershell
build\host\bin\PocketLoom-Play.exe --p3d samples\bee\bee.p3d --frames 2
build\host\bin\PocketLoom-Play.exe --p3d samples\platformer\game.p3d --frames 2
build\host\bin\PocketLoom-Play.exe --p3d samples\topdown\game.p3d --frames 2
```

需要发布包时：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1
```

## 5. 平台闸门

| 改动 | PC | 3DS | PSP |
|---|---|---|---|
| [core] | build + capabilities + 3 demo | 编译；运行路径变了就实跑 3dsx | 编译；运行路径变了就实跑 EBOOT |
| [editor] | build + editor smoke | 不影响 | 不影响 |
| [data] | 重导 3 demo | 读同一 p3d | 读同一 p3d |
| [3ds] | 不回归即可 | 3dsx 实跑 | 只要求编译不坏 |
| [psp] | 不回归即可 | 只要求编译不坏 | EBOOT 实跑 |

没有工具链就明确写“未验证”，不要写“应该可以”。

## 6. 数据格式规则

改以下内容必须升 p3d 或 save 版本：

- p3d 二进制字段、枚举顺序、对象/场景结构；
- save 字段或编码；
- 变量作用域/持久化语义。

改完必须：

1. 升常量；
2. 重新导出三个 demo；
3. 用播放器实际读；
4. 更新 docs；
5. 在提交信息里写版本变化。

旧 p3d/旧存档不兼容是允许的，不做兼容层。

## 7. 3DS / PSP 接入顺序

不要直接把旧 Game 搬过去。

### 阶段 0：平台契约

先统一：

- 文件读取；
- 存档路径；
- 纹理上传；
- 输入映射；
- 音频；
- 时间/日志。

runtime 不直接依赖 `std::filesystem` 和平台 API。

### 阶段 1：平台 hello

- 平台层 + 渲染后端 + 工具链；
- 清屏/输入/退出；
- 不接 Game。

### 阶段 2：当前 runtime

- 读当前 p3d；
- 调 `Game::Init/Update/Render`；
- 只画背景。

### 阶段 3：demo

- 当前 demo 重新导出；
- 放进 romfs / ISO；
- 实跑 3DSX / EBOOT。

### 阶段 4：输入/音频/存档/UI

每个阶段都要有实跑证据。

## 8. 3DS 复用清单

可复用：

- `engine/platform/_3ds/`
- `engine/render/backend_citro.cpp`
- `runtime/audio_3ds.cpp`
- `cmake/toolchain-3ds.cmake`
- `cmake/3dsx.cmake`
- `cmake/cia/`
- 本机 devkitPro

不可直接复用：

- 旧 `runtime/game.cpp`（p3d v67 + 固定玩法）
- 旧 `runtime/p3d.*`
- 旧 `pgmmv/`、`editor/`、`demo/`
- 旧字体 `font*`

3DS 阶段目标：

- 3DS 显示 400×240 安全区；
- 当前 p3d v91；
- 同一份 demo 数据。

当前 3DS 已落地：`engine/platform/_3ds/platform_3ds.cpp`、`engine/render/backend_citro.cpp`、`runtime/3ds_main.cpp`、`runtime/audio_3ds.cpp`、`cmake/toolchain-3ds.cmake`、`cmake/3dsx.cmake`、`tools/3ds-hello/`。3DS 主工程已能读 romfs 的 p3d，跑 `Game::Init/Update/Render`，在 Azahar 显示地图/蜜蜂/玩家；纹理通道/8×8 块用 tex3ds 官方 foursome 表（A,B,G,R），不要再改回简化 Morton8；改前先跑 16×16 色块截图。

## 9. PSP 预留

- PSP 逻辑分辨率 480×272，和 PC 视口一致。
- PSP 内存只有 32MB，资产/图集要有预算。
- runtime 已移除 `<filesystem>`；PSP 用 `P3dLoad(bytes)` + 自己的 `sceIo` 读文件。
- 纹理 swizzle 只放 PSP 后端。
- 不复制 runtime，不写平台分支进 Game。
- 同一份 p3d 跑 PC/3DS/PSP。
- 当前已补 PSP 骨架：
  - `engine/platform/_psp/`
  - `engine/render/backend_psp.cpp`
  - `runtime/psp_main.cpp`
  - `runtime/audio_psp.cpp`
  - `cmake/toolchain-psp.cmake`
  - `tools/build-psp.ps1`
  - 状态与限制见 `docs/21-PSP后端.md`。
- PSP 骨架已在 pspdev CI 编过，并在 PPSSPP 实跑；当前卡在“世界/精灵纹理不可见”，不能当成已收尾。
- Windows 原生没有官方 pspdev；WSL2 用 `tools/setup-psp-wsl.sh`，构建用 `tools/build-psp-wsl.sh`。
- 不想用 WSL：走 `.github/workflows/psp.yml`（GitHub Actions）或 Linux 虚拟机；原生 MinPSP 太老，不用。

## 10. 收尾定义

完成必须同时满足：

- 构建通过；
- capabilities 通过；
- 三个 demo 能导出、能启动；
- 目标平台有实跑证据；
- 数据版本已同步；
- `tools/package.ps1` 通过（需要发布时）；
- `git status` 干净；
- 提交信息说明改了什么、版本是否变化。

## 11. 大改保护

- 删除超过 100 行：先列引用，先提交，分两次删。
- 改数据格式：先改运行时读取，再改导出，再改 demo。
- 改图层/相机：用 16px 和 8px 各验一次。
- 改 UI 字体：生成 480×272、400×240 预览，检查底部裁切。
- 改规则：同时测 var_add/var_set 和一次区域触发。
- 改平台层：先 hello，再接 Game。

## 12. 常用路径

```text
构建：tools/build-editor.ps1
测试：build/host/bin/capabilities.exe
播放器：build/host/bin/PocketLoom-Play.exe
编辑器：build/host/bin/PocketLoom-Editor.exe
打包：tools/package.ps1
demo：samples/bee、samples/platformer、samples/topdown
3DS 参考：D:\GitHud\GameStudio\3DSStudioMini-PGMMV
```

## 13. 当前接入状态（2026-11，交接必读）

### 3DS
- 本机 devkitPro：`D:\GitHud\GameStudio\3DSStudioMini-PGMMV\toolchain\devkitpro`。
- CMake/Ninja：`D:\GitHud\GameStudio\GBAStudioMini\toolchain\mingw64\bin`。
- Azahar：`3DSStudioMini-PGMMV\toolchain\azahar\azahar.exe`；截图工具 `3DSStudioMini-PGMMV\tools\shot-window.ps1`。
- CI：`.github/workflows/3ds.yml`，镜像 `devkitpro/devkitarm:latest`，产物 `pocketloom-3ds-hello`（含 `game_3ds.3dsx`）。
- 3DS 主目标：`cmake -S . -B build/3ds-main -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/toolchain-3ds.cmake`，再编 `game_3ds`。
- 3DS 读 `romfs:/game.p3d`；romfs 由 CMake 从 `samples/bee/bee.p3d` 生成。**不要用 `P3dLoadFile` 读 romfs**，用 `fopen/fread` + `P3dLoad(bytes)`。
- `runtime/3ds_main.cpp` 已跑 `Game::Init/Update/Render`；后端 `backend_citro.cpp` 已有纹理上传/绘制，Azahar 已显示地图、蜜蜂、玩家。
- 纹理转换已改成 tex3ds 官方 swizzle：8×8 foursome 表 + 成对交换，通道 A,B,G,R；16×16 色块已验证四色正确。不要再手写简化 Morton8，也不要直接用 `C3D_TexUpload`（会条纹）。
- `runtime/audio_3ds.cpp` 已接 ndsp；没有 `sdmc:/3ds/dspfirm.cdc` 时会静音。
- 输入：平台层映射已写；模拟器自动注入按键受 Windows 前台锁影响，**真实按键验收仍未完成**。
- 本机 Windows 编 devkitARM 会遇到临时文件/对象写权限；CI 是可靠验证路径，别把本机失败当代码失败。

### PSP
- PPSSPP：`C:\Users\11747\Documents\PPSSPP\PSP\GAME\PocketLoom`（新 PPSSPP 用 Documents 下的 memstick，不是旧 `ppsspp_win\memstick`）。
- 启动参数必须带：`--escape-exit --fullscreen <EBOOT.PBP>`。
- 当前已通：启动、P3dLoad、Game::Init、Update、Render、HUD 文本/分数条、命令提交。
- 当前未通：世界/精灵纹理不可见。已排除：坐标（debug 矩形在玩家位置可见）、纹理句柄（psp_main 侧 id 有效）、上传缓冲中心像素（非 0）、整图 UV、P3d move/swap。
- 最接近的线索：DrawList 里对象 Image 命令存在但 backend 只记录到字体纹理 `tex_ok 1`；优先查 DrawList/TextureGet 跨库或对象 Image 是否真正进 backend，不要再从坐标/解析器重头查。
- 调试痕迹（PL_ERR/PL_TRACE/PL_GAME/PL_GPU/PL_DROP/PL_INPUT、GameTrace/GpuTrace）大量留在源码；发布前必须清。
- PSP 解析必须保留的 workaround：`Action::frames` 用 `push_back(AnimFrame{})`；nontrivial 容器用 `reserve/emplace`；`DrawList` 放 static；主线程栈 256KB。
- 参考文档：`docs/21-PSP后端.md`。

### 编辑器
- `文件 > 导出 game.p3d` 已加；导出到 `<project>/build/game.p3d`，无保存对话框。
- 编辑器和 `ploom.exe` 导出同一份 p3d；bee 导出哈希与 `samples/bee/bee.p3d` 一致。
- 鼠标自动化：Windows 前台锁会让 `mouse_event` 只激活窗口；必须先 `AttachThreadInput(GetCurrentThreadId, foregroundTid, true) + SetForegroundWindow + BringWindowToTop + SetFocus`，再点。
- 已验证鼠标：打开工程、切资源/动画/对象页、加对象实例、加触发器、加 UI 元素、F5 试玩、导出。
- 未完全验收：地图图块绘制（bee 的 tileset 只有 1 格，点击不改 JSON）、UI 预览视觉细节、触发器编辑后再导出实跑。
- 面板 CLI：`PocketLoom-Editor.exe --open <project.json> --tool 0..5`，0 场景 1 图块 2 动画 3 对象 4 资源 5 界面。

## 14. 跨对话交接

1. 先 `git status`、`git log --oneline -10`，读本文件和相关 docs。
2. 先判断轨道；不要同时改 core/editor/3ds/psp。
3. 改前确认当前数据版本仍是 p3d v91 / PLSV v2。
4. 大改前提交；调试 trace 不要当正式代码。
5. 交接时明确写：
   - 已验证到哪一步；
   - 目标平台是否有实跑截图；
   - 已知失败/未验证项；
   - 下一个最小验证动作。
6. 不要重新踩这些坑：
   - PSP 黑屏先从启动/栈查起（已解决，别再怀疑 EBOOT）；
   - 3DS 不要用 `P3dLoadFile` 读 romfs；
   - 编辑器鼠标不进画布先修前台锁，不要先改编辑器逻辑；
   - 改纹理通道顺序必须截图对比。

## 15. 验收矩阵（当前）

| 项 | PC | 3DS/Azahar | PSP/PPSSPP |
|---|---|---|---|
| 构建 | 通过 | CI 通过 | CI 通过 |
| 启动 | 通过 | 通过 | 通过 |
| HUD | 通过 | 通过 | 通过 |
| 地图/对象 | 通过 | 通过 | 贴图不可见 |
| 输入 | 键盘可用 | 代码已接，自动注入未验 | 自动注入未验 |
| 音频 | 通过 | ndsp 已接，未听验 | 静音占位 |
| 编辑器导出 | 通过 | 读同一 p3d | 读同一 p3d |

## 16. 下一步优先级

1. PSP：查对象 Image 命令为何没进 backend（DrawList/TextureGet/跨库），保留现有 trace 直到定位。
2. 3DS：手动按键 + 听音频；校色；补一个双击/长按测试。
3. 编辑器：用多格 tileset 验证图块绘制；触发器加完导出实跑；UI 元素导出后 3DS/PC 对照。
4. 清理 PSP/3DS 调试 trace，更新 docs，跑三 demo + capabilities + package。

