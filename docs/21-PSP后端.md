# 21 - PSP 后端

> 状态（2026-11-08）：PSP 已在 PPSSPP 1.20.4 实跑，地图、图块、蜜蜂、玩家、HUD 都正常显示。
> 构建走 GitHub Actions（`PSP build`），本机没有 pspdev。
> 当前剩余：输入/音频验收，存档/UI 细项，调试清理。

## 1. 文件

| 文件 | 作用 |
|---|---|
| `engine/platform/_psp/platform_psp.cpp` | 输入、计时、显示模式、日志 |
| `engine/render/backend_psp.cpp` | PSP GU 后端：clear / rect / image |
| `runtime/psp_main.cpp` | PSP 播放器入口，读 p3d，跑 Game |
| `runtime/audio_psp.cpp` | 音频占位（当前静音） |
| `cmake/toolchain-psp.cmake` | pspdev 工具链 |
| `cmake/psp-package.cmake` | ELF -> EBOOT.PBP |
| `tools/build-psp.ps1` | 一键构建（需要 pspdev） |

## 2. 构建与运行

- CI：`.github/workflows/psp.yml`，artifact `pocketloom-psp`，含 `game_psp.elf`、`EBOOT.PBP`、`game.p3d`。
- PPSSPP memstick（新版）：
  `C:\Users\11747\Documents\PPSSPP\PSP\GAME\PocketLoom`
- 启动参数必须带：
  `--escape-exit --fullscreen <EBOOT.PBP>`
- 判定：窗口标题 `- PocketLoom`；正常退出/截图能看见地图、HUD、精灵。

## 3. 渲染约定（重要）

- 纹理：RGBA8 源，POT 至少 64；`GU_PSM_8888`。
- 纹理坐标 **是 texel 坐标，不是 0..1**：
  `u0=sx, v0=sy, u1=sx+sw, v1=sy+sh`。
  这是 2026-11-08 世界/精灵不显示的根因；不要再除以 POT。
- 绘制用 `GU_SPRITES` + `GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D`。
- 当前纹理数据按线性直拷，`sceGuTexMode(..., 0)`；不要启用 swizzle（除非重新验证）。
- 顶点颜色走 `sceGuColor`，不要给 sprite 顶点加 `GU_COLOR_8888`。
- `DrawList` 必须放静态区（`sizeof` 约 360KB，PSP 主线程栈 256KB）。
- PSP 纹理接收 `rgba8` 源数据；大图先估 32MB 内存，单张超过 1024 拒绝。

## 4. 路径

```text
ms0:/PSP/GAME/PocketLoom/EBOOT.PBP
ms0:/PSP/GAME/PocketLoom/game.p3d
ms0:/PSP/SAVEDATA/PocketLoom/save0.dat
ms0:/PSP/SAVEDATA/PocketLoom/report.txt
```

也支持当前目录的 `game.p3d`，方便 PPSSPP。

## 5. 已解决

- EBOOT 可启动。
- 黑屏：DrawList 放栈上导致 256KB 栈溢出，已改 static。
- P3dLoad / Game::Init / Update 可跑。
- 世界、图块、对象、HUD 显示。
- 纹理坐标错误已修（texel，不是 normalized）。
- 调试 trace 已关闭；不再写 `PL_*.txt`。

## 6. 已知限制

1. **输入**：平台层映射已写，但 PPSSPP 自动注入未完整验收。
2. **音频**：`audio_psp.cpp` 仍是静音占位。
3. **存档/UI**：路径已接，完整存档/UI 流程未逐项验收。
4. **颜色顺序**：当前 `ToGuColor` 直接传；若真机红蓝反，只改这一处。
5. **内存**：未做 32MB 预算；大图集先算。
6. **退出**：PSP 没有可直接读的 HOME 键；目前靠 PPSSPP 停止/退出。

## 7. 下一步

1. 输入：在 PPSSPP 里实际按方向/A/Start，确认移动与交互。
2. 音频：接 `sceAudio`，先做 PCM 测试音。
3. 存档：实际写/读一次 `save0.dat`。
4. UI：确认 HUD/面板在 480×272 下不裁切。
5. 清理剩余调试代码，再跑三 demo 回归。

## 8. 踩坑记录

- 不要用归一化 UV 画 PSP 纹理，会只采到一个 texel（表现为黑屏/纯色）。
- 不要逐图同步写日志，会拖慢到只看到前几百条；用汇总计数。
- 不要直接把 `C3D_TexUpload` 的思路套 PSP；PSP 纹理是线性数据 + texel UV。
- 修改 `DrawImage` 后，先跑一张 16×16 棋盘纹理，能看出 UV/采样问题，纯色图看不出来。
