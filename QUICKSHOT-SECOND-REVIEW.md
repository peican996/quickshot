# QuickShot 第二轮代码审查与边界问题修复报告

- 仓库：`https://github.com/peican996/quickshot`
- 基线：`95aab96`（第一轮修复提交）+ 工作区未提交改动（勾/叉对换、本轮四项修复）
- 修改文件：`shot.c`（唯一代码文件）
- 验证环境：Windows，MinGW-w64 / UCRT，本机实际编译并执行（本轮所有结果均为真实运行输出）
- 本轮不做重构，只修边界问题与测试强度

---

## 1. 四项问题逐条确认与修复

### 1.1 马赛克缓存失效遗漏（WM_LBUTTONUP / ST_DRAW）

**确认存在**

[shot.c#L2476-L2484](file:///C:/code/quickshot/shot.c#L2476-L2484) 原来只有：

```c
if (!keep) g_annCount--;
```

拖动过程中该条马赛克已经参与过 `OverlayDraw`，`g_mosaicCache[g_annCount-1]` 可能已经写入
`{rc, sel, epoch}`。丢弃时既不 bump 版本、也不清缓存，违反了「标注集合变化 → 缓存必须失效」的不变量。

**问题原因分析（如实说明影响范围）**

在当前渲染管线里，这个陈旧条目要真正显示出错误像素，需要同时满足：同一下标、同一 `rc`、同一 `sel`、同一 epoch，
而 `rc`/`sel` 都在缓存键里，所以「同坐标再画一个马赛克」实际上会得到与当前底图一致的结果（本轮测试也验证了这一点）。
但这属于**靠巧合正确**：一旦后续新增「按区域局部失效」「底图局部刷新」等改动，陈旧条目就会立刻变成可见脏数据。
因此按审查建议修复不变量本身，而不是依赖当前实现的巧合。

**修改**

[shot.c#L2476-L2484](file:///C:/code/quickshot/shot.c#L2476-L2484)：

```c
if (!keep) {
    g_annCount--;
    /* 这条标注在拖动过程中可能已经生成过马赛克缓存，
     * 丢弃后必须让它失效：否则同一下标复用时可能命中旧图。 */
    MosaicEpochBump();
    QSLog("overlay: annot discarded (too small), epoch=%u", g_mosaicEpoch);
}
```

**其余标注变更路径复核**（确认无遗漏）

| 路径 | 位置 | 是否失效 |
|---|---|---|
| 撤销 `UndoOne()` | [shot.c#L2179](file:///C:/code/quickshot/shot.c#L2179) | 已 `MosaicEpochBump()` |
| 文字提交/改写/清空删除 | [shot.c#L1797](file:///C:/code/quickshot/shot.c#L1797) | 已 bump |
| 重新框选（`g_annCount = 0`） | 两处，均 bump | 已 bump |
| 松手丢弃标注 | 本轮新增 | 已 bump |
| 换底图 / 遮罩关闭 | `FreeCapture()` → `MosaicCacheFreeAll()` | 整体清空 |
| 新增标注（追加） | 只追加下标，不影响已有条目的源像素 | 无需 bump |

**回归测试**：`TestMosaicDiscard()`

- 走真实消息链路 `WM_LBUTTONDOWN → WM_MOUSEMOVE → WM_LBUTTONUP` 画一条 2px 宽的马赛克（拖动时确实生成缓存），松手被丢弃；
- 断言 `g_annCount == 0`，且 `g_mosaicEpoch` 发生变化；
- 在同一下标再画一个正常马赛克，与「强制无缓存重算」的导出图**逐像素**比较。

实测输出：

```text
discard: annots=0, epoch 839 -> 840
[PASS] narrow mosaic discarded on mouse up
[PASS] discard invalidates mosaic cache
[PASS] mosaic recreated at same index
discard regression matches current base: compared 196144 px, diff 0 (0.0000%)
[PASS] discard regression matches current base
```

---

### 1.2 OvDimBuffers 错误处理不完整

**确认存在**：`GetObjectW` 失败、`bm.bmBits` 为空、尺寸不符、行宽不足时，原实现仍返回 `TRUE`；
像素量按 `vw*vh` 计算，没有显式溢出保护；`RunDemoShot` 还忽略了返回值。

**修改**（[shot.c#L406-L455](file:///C:/code/quickshot/shot.c#L406-L455)）

1. 进入时校验 `hdcDim / hdcCap / pixCap` 非空、`vw/vh > 0`；
2. `vw * vh * 4` 做 `size_t` 溢出检查；
3. `BitBlt` 失败 → `FALSE`；
4. `GetObjectW` 失败 → `FALSE`（带日志）；
5. `bmBits` 为空或 `bmBitsPixel != 32` → `FALSE`；
6. `bmWidth/bmHeight` 与 `vw/vh` 不一致 → `FALSE`；
7. `bmWidthBytes < vw*4` → `FALSE`（否则按行遍历会越界）；
8. 变暗循环改为**按 DIB 实际 `bmWidthBytes` 逐行遍历**，不再假设行宽恰好等于 `vw*4`；
9. 只有上述全部通过并完成逐像素变暗才返回 `TRUE`。

**调用方复核**

| 调用方 | 处理 |
|---|---|
| `CaptureVirtualScreen()` | 失败即 `FreeCapture()` 并返回 `FALSE`（已有） |
| `RunDemoShot()` | 本轮补上：失败时 `FreeCapture() + OverlayGdiFree()` 并返回 1 |
| `WarmUpAll()` / `TestMosaic()` / `TestMosaicDiscard()` | 失败即判定该用例失败并释放缓冲 |

---

### 1.3 马赛克缓存内存预算统计口径不一致

**确认存在**：`MosaicEnsure()` 对 `g_mosaicScratch` 也执行 `g_mosaicCachedPixels += w*h`，
`MosaicCacheFree()` 也从同一变量扣除，与「scratch 不参与正式缓存预算」的注释矛盾；
且 scratch 的占用会挤占正式缓存预算，导致可缓存区域被误判为超预算。

**修改**

1. 新增判定函数 [MosaicBudgeted()](file:///C:/code/quickshot/shot.c#L619-L622)：只有 `g_mosaicCache[]` 中的槽位计入预算，`g_mosaicScratch` 不计；
2. `MosaicEnsure()` 只在 `MosaicBudgeted(c)` 为真时累加；
3. `MosaicCacheFree()` 只在 `MosaicBudgeted(c)` 为真时扣除；
4. 缓存可命中的判定仍只使用「正式槽位自身占用」`slotPixels`，因此：
   - 单条正式缓存 ≤ 8M 像素（`MOSAIC_CACHE_MAX_PIXELS`）；
   - 全部正式缓存 ≤ 16M 像素（`MOSAIC_CACHE_BUDGET`）；
   - scratch 只受单次区域大小限制，不影响正式缓存判定；
5. 注释同步改为「scratch 是单块临时画布，不参与正式缓存预算」。

**预算边界测试**：`TestMosaicBudget()`

| 场景 | 断言 | 实测 |
|---|---|---|
| 初始状态 | `g_mosaicCachedPixels == 0` | PASS |
| 首次 100×100 缓存 | 计费 10000、`capW==100` | PASS |
| 扩到 300×200 | 释放旧槽后计费 60000、`capW==300` | PASS |
| 缩到 150×100 | 复用 300×200 画布，计费仍 60000 | PASS |
| 14 条 1200×1000（1.2M/条） | 第 1–13 条入缓存，第 14 条超预算退回 scratch | `13 of 14 cached, pixels=15600000, scratch.capW=1200` |
| 计费与槽位一致 | `cachedPixels == 13 × 1200 × 1000` | PASS |
| 总量 | 任何时刻 `≤ 16M` | PASS |
| `MosaicCacheFreeAll()` | 计费归零、scratch 释放 | PASS |
| 单条 2900×2900（8.41M > 8M） | 不缓存、走 scratch、不计费 | `slot.capW=0 scratch.capW=2900 pixels=0` |
| 最终清理 | 计费归零 | PASS |

结论：预算口径现在与注释一致，scratch 不再污染统计，所有缓存释放后统计严格归零。

---

### 1.4 预览 / 导出一致性测试改为逐像素

**修改**：新增 [ComparePreviewVsExport()](file:///C:/code/quickshot/shot.c#L3322-L3341)，`TestMosaic()` 与
`TestMosaicDiscard()` 都改用它：

- 预览图：`RenderOverlayToDib()` 生成的 top-down DIB（客户端坐标）；
- 导出图：`BuildFinalBitmap()` 生成的 bottom-up DIB（选区坐标，逐行 `h-1-y` 转换）；
- 比较范围：**选区内缩 6px 的全部像素**，一次不漏；
- 输出：比较像素总数、差异像素数、差异比例；任何差异即判定失败。

**为什么内缩 6px（如实说明）**

选区内侧 1px 是选框边框，四角/四边是 7×7 手柄，选区上方还有「宽 × 高」尺寸标签——这些是遮罩界面的装饰元素，
本来就不会进入导出图。内缩后比对区域不含任何 UI 装饰，但覆盖了**全部马赛克区域**（三条马赛克都落在该区域内）。
这不是抽样：区域内每个像素都参与比较。

实测输出（两条用例各自 196,144 像素，0 差异）：

```text
mosaic preview == export (all pixels): compared 196144 px, diff 0 (0.0000%)
[PASS] mosaic preview == export (all pixels)
discard regression matches current base: compared 196144 px, diff 0 (0.0000%)
[PASS] discard regression matches current base
```

---

## 2. 修改清单

| 函数 / 位置 | 改动 |
|---|---|
| [`OvDimBuffers()`](file:///C:/code/quickshot/shot.c#L406-L455) | 完整校验 DIB 指针/位深/尺寸/行宽，按 `bmWidthBytes` 逐行变暗，失败返回 FALSE |
| [`MosaicBudgeted()`](file:///C:/code/quickshot/shot.c#L619-L622) | 新增，区分正式缓存槽位与 scratch |
| [`MosaicCacheFree()`](file:///C:/code/quickshot/shot.c#L624-L643) | 只对正式槽位归还预算 |
| [`MosaicEnsure()`](file:///C:/code/quickshot/shot.c#L660-L687) | 只对正式槽位累加预算 |
| [`DrawMosaicAnnot()`](file:///C:/code/quickshot/shot.c#L723-L758) | 注释明确预算口径（逻辑不变） |
| `OverlayProc()` WM_LBUTTONUP / ST_DRAW | 丢弃标注时 `MosaicEpochBump()` |
| [`RunDemoShot()`](file:///C:/code/quickshot/shot.c) | 检查 `OvDimBuffers()` 返回值并清理 |
| [`ComparePreviewVsExport()`](file:///C:/code/quickshot/shot.c#L3318-L3341) | 新增整区逐像素比较 |
| [`TestMosaic()`](file:///C:/code/quickshot/shot.c#L3343) | 改选区、马赛克位置，改为整区逐像素比较 |
| [`TestMosaicDiscard()`](file:///C:/code/quickshot/shot.c#L3508) | 新增：真实鼠标链路丢弃回归 |
| [`TestMosaicBudget()`](file:///C:/code/quickshot/shot.c#L3574) | 新增：预算边界六类场景 |
| [`TestSurfaceCreate/Destroy()`](file:///C:/code/quickshot/shot.c#L3471-L3505) | 新增：测试用 DIB 画布封装 |
| `TestHotkey()` / `HotkeyBlockerThread()` | 改用 Ctrl+Alt+F9/F10/F11/F12，避免与正在运行的真实实例抢 Ctrl+Alt+A |
| `ClipboardBlockerThread()` | 增加 20×20ms 重试，避免外部剪贴板管理器导致用例被跳过 |

---

## 3. 回归测试与真实输出

构建：

```bat
windres app.rc -O coff -o app.res
gcc -O2 -s -Wall -mwindows shot.c app.res -o snapshot.exe ^
    -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -lcomdlg32 -lm
```

编译通过，`-Wall` 无告警。

### 3.1 `--stress-test`（完整输出）

```text
=== QuickShot stability self-test ===
capture: 1000x 54656 ms, gdi 20 -> 20, fails=0
[PASS] capture 1000 times
[PASS] capture loop gdi stable
  gdi after capture: 20
text: 1000x commit/cancel, gdi 26 -> 26, fails=0
[PASS] text create/commit/cancel 1000x
[PASS] text loop gdi stable
[PASS] teardown with editor open
  gdi after text: 20
[PASS] mosaic 300 redraws render
[PASS] mosaic redraw gdi stable
mosaic: annots 500x cached 0 ms, 500x uncached 32 ms
[PASS] mosaic cache is not slower than recompute
mosaic preview == export (all pixels): compared 196144 px, diff 0 (0.0000%)
[PASS] mosaic preview == export (all pixels)
[PASS] mosaic undo restores base pixels
  gdi after mosaic: 20
discard: annots=0, epoch 839 -> 840
[PASS] narrow mosaic discarded on mouse up
[PASS] discard invalidates mosaic cache
[PASS] mosaic recreated at same index
discard regression matches current base: compared 196144 px, diff 0 (0.0000%)
[PASS] discard regression matches current base
[PASS] budget: starts at zero
[PASS] budget: first cache billed 100x100
[PASS] budget: grow bills new size
[PASS] budget: shrink reuses canvas without extra billing
budget: 13 of 14 large mosaics cached, pixels=15600000, scratch.capW=1200
[PASS] budget: 13 large regions fit, 14th rejected
[PASS] budget: accounting matches cached slots
[PASS] budget: over-budget region uses scratch
[PASS] budget: total never exceeds limit
[PASS] budget: free all returns to zero
[PASS] budget: scratch released
budget: oversize -> slot.capW=0 scratch.capW=2900 pixels=0
[PASS] budget: single region over 8M not cached
[PASS] budget: oversize single region uses scratch
[PASS] budget: scratch is not billed
[PASS] budget: zero after cleanup
[PASS] clipboard copy 64x48
[PASS] clipboard rejects w=0
[PASS] clipboard rejects w<0
[PASS] clipboard rejects huge size
[PASS] clipboard rejects NULL bits
[PASS] clipboard busy: copy fails safely
files: made=20 dup=0
[PASS] 20 shots in one second get unique names
[PASS] no duplicate file names
[PASS] existing files are not overwritten
[PASS] missing dir rejected
[PASS] occupied hotkey rejected
[PASS] old hotkey kept after failed switch
[PASS] switch to free hotkey works
[PASS] released combo can be applied
[PASS] hotkey unregistered
=== gdi total: 20 -> 18 ===
[PASS] no gdi leak across whole run
=== OK (0 failures) ===
```

退出码 0，44 项检查全部通过。

### 3.2 `--capture-test` / `--toolbar-test` / `--demo`

```text
capture-test exit=0
capture: 2560 x 1440 at (0,0)
annots: 7
crop: 400 x 300
png: ok -> C:\code\quickshot\selftest.png
hotkey Ctrl+Alt+A: occupied        ← 本机有正在运行的真实 QuickShot 实例占用，属环境状态

toolbar-test exit=0
out-of-bounds: 0
cursor mismatches: 0
png: ok -> C:\code\quickshot\toolbar.png

demo exit=0
demo: ok
```

### 3.3 与 `95aab96` 基线做像素级回归对比

用 `git show 95aab96:shot.c` 单独编译基线版本，与本轮版本在同一张底图上执行 `--demo`，逐像素比较：

```text
image: 1400x843, diff pixels: 288
diff bbox: (1338,134)-(1382,147)
```

差异仅出现在工具条右上角 45×14 的区域，正是上一轮按要求对换的 ✗ / ✓ 两个按钮（各 30×14）。
也就是说：本轮 `OvDimBuffers` 与马赛克相关的改动**没有造成任何其它像素变化**，渲染结果与基线一致。

---

## 4. 马赛克缓存内存统计结果

| 项目 | 数值 |
|---|---|
| 单条上限 | 8M 像素（`MOSAIC_CACHE_MAX_PIXELS`） |
| 总量上限 | 16M 像素（`MOSAIC_CACHE_BUDGET`，约 64MB） |
| 14 条 1200×1000 实测 | 13 条入缓存，合计 15,600,000 像素；第 14 条退回 scratch |
| 单条 2900×2900 实测 | 8.41M 像素 > 8M，不缓存；`slot.capW = 0`，`scratch.capW = 2900`，计费 0 |
| scratch 是否计入预算 | 不计入（等式校验通过） |
| `MosaicCacheFreeAll()` 后 | `g_mosaicCachedPixels == 0` |
| 整个流程结束 | GDI 20 → 18，无增长 |

---

## 5. 预览与导出完整像素比较结果

| 用例 | 比较区域 | 像素总数 | 差异 | 比例 | 结果 |
|---|---|---|---|---|---|
| `TestMosaic`（3 条马赛克） | 选区 610×340 内缩 6px | 196,144 | 0 | 0.0000% | PASS |
| `TestMosaicDiscard`（丢弃后重建） | 同上 | 196,144 | 0 | 0.0000% | PASS |

比较覆盖了三条马赛克的全部像素，不再使用采样点。除比对区域外的 6px 边缘是选框边框/手柄/尺寸标签，
属于界面装饰，不属于导出内容（已在代码注释中说明）。

---

## 6. 是否引入新的回归

| 检查项 | 结果 |
|---|---|
| 编译告警 | 无（`-Wall`） |
| `--stress-test` | 0 failures，退出码 0 |
| `--capture-test` | 通过（退出码 0） |
| `--toolbar-test` | 布局越界 0、光标判定错 0 |
| `--demo` | 与基线仅差 ✗/✓ 按钮位置（288 px） |
| GDI 增长 | 1000 次截图 20→20；1000 次文字编辑 26→26；整体 20→18 |
| 预览/导出 | 逐像素一致（两个用例各 196,144 px） |
| 性能 | 缓存命中 500 次 0 ms，强制重算 32 ms（数量级差距保持） |

---

## 7. 测试环境说明与未能覆盖项

- 本轮所有输出均在本机 Windows（2560×1440 桌面会话）实际编译、实际运行得到，非引用上一轮结果。
- 未覆盖（需人工）：真实键鼠全流程交互、中文输入法上屏、多显示器混合 DPI、托盘图标在 Explorer 重启后的恢复。
- `--stress-test` 依赖交互式桌面（抓屏、剪贴板、全局热键）；CI 无桌面时请只跑 `--capture-test` / `--toolbar-test`。
- 本机常驻一个真实 QuickShot 实例时会占用 `Ctrl+Alt+A`，因此自检改用功能键组合，避免把环境占用误判成缺陷
  （`--capture-test` 中打印的 `hotkey Ctrl+Alt+A: occupied` 即属此情况，不影响退出码）。

---

## 8. 遗留问题（与第一轮一致，本轮未扩大范围）

1. 托盘提示文字仍写死 `Ctrl+Alt+A`，切换热键后不更新（低风险，涉及 UI 文案）；
2. 启动时若保存的热键被占用，程序无热键可用，只能手动换一个（原行为）；
3. 马赛克失效粒度为「整条作废」，文字改写会让所有马赛克重算一次（点击级开销，受预算约束）；
4. 剪贴板拒绝 >2GB 的 DIB（远超实际截图尺寸）；
5. `--stress-test` 依赖交互式桌面，需在 CI 中区分。
