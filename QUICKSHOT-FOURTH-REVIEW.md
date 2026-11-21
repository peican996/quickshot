# QuickShot 第四阶段：GDI 资源恢复闭环修复

- 仓库：`https://github.com/peican996/quickshot`
- 基线：`fdc1eb8`（`shot.c` blob `d831229923f4e3ef2818de6bd134aa4d834d31ba`）
- 修改文件：`shot.c`（唯一代码文件）
- 环境：Windows，MinGW-w64 / UCRT，`-O2 -s -Wall -mwindows`，无新增告警
- 本轮不新增功能、不做重构，只把 GDI 临时对象的生命周期补成闭环并补测试

---

## 1. 修改文件与函数清单

| 位置 | 修改 |
|---|---|
| [`GdiSelect()`](file:///C:/code/quickshot/shot.c#L278-L305) | 新增 `old` 输出指针判空；入口先 `*old = NULL`；增加自检用故障注入开关；失败日志带 hdc/obj |
| [`GdiRestore()`](file:///C:/code/quickshot/shot.c#L309-L321) | 改为返回 `BOOL`，校验 hdc/old，检查 `SelectObject` 返回值，失败写日志 |
| [`GdiRelease()`](file:///C:/code/quickshot/shot.c#L330-L350) | 恢复失败时用同类 stock 对象顶出临时对象，再 `DeleteObject` 并检查结果；失败日志带句柄值；恢复失败一律返回 `FALSE` |
| [`RestoreOldBmp()`](file:///C:/code/quickshot/shot.c#L442-L446) | 改用 `GdiRestore`，失败后仍清空记录，由 `DeleteDC` 解除引用 |
| `OvAllocBuffers` / `MosaicEnsure` / `MosaicCacheFree` | 位图选入/还原改走封装，失败路径统一释放 |
| `DrawPenStroke` / `DrawEllipseAnnot` / `DrawRectAnnot` / `DrawArrowAnnot` / `DrawNumberAnnot` / `DrawTextAnnot` | 临时画笔/画刷/字体全部走 `GdiSelect` + `GdiRelease` |
| `ToastProc` / `ToastShowAt` / `TextAnnotRect` | 字体选入失败时安全降级（默认字体 / 零尺寸），字体必删 |
| `CreatePinWindow` / `PinProc WM_DESTROY` / `RenderOverlayToDib` / `BuildFinalBitmap` / `RunDemoShot` / `RunToolbarTest` / 自检微基准 / `TestSurfaceCreate/Destroy` | 选入与恢复全部收口，失败路径补齐 |
| 测试 | 新增 `TestTextAnnotSelectFailure`、`TestRestoreOldBmp`；`TestGdiLifecycle` 扩充恢复失败分支；热键回退补充注册表校验与「下次启动优先原组合」 |

---

## 2. 每个 GDI 资源的创建、选入、恢复、释放路径

| 资源 | 创建 | 选入 | 恢复 | 释放 | 失败处理 |
|---|---|---|---|---|---|
| 截屏 DIB ×3 | `OvAllocBuffers` / `CreateDIBSection` | `GdiSelect`（记 `g_ov.oldBmp*`） | `RestoreOldBmp` → `GdiRestore` | `FreeCapture`：还原→`DeleteDC`→`DeleteObject` | 任一步失败 → `done:` → `FreeCapture` 全量清理 |
| 马赛克缓存 DIB | `MosaicEnsure` | `GdiSelect`（记 `c->oldBmp`） | `GdiRestore` | `MosaicCacheFree`：还原→`DeleteDC`→`DeleteObject` | 选入失败 → `MosaicCacheFree` 并返回 FALSE |
| 成品位图 | `BuildFinalBitmap` | `GdiSelect` | `GdiRestore` | 调用方 `DeleteObject` | BitBlt 失败 → 还原→`DeleteDC`→`DeleteObject` 并返回 NULL |
| 钉图位图 | `BuildFinalBitmap` 结果移交 `PinData` | `GdiSelect` | `WM_DESTROY` 中 `GdiRestore` | 还原→`DeleteDC`→`DeleteObject` | 建窗失败时按同序清理 |
| 描边画笔 | `CreatePen` | `GdiSelect` | `GdiRelease` | `DeleteObject`（含结果检查） | 创建/选入失败 → 删除并跳过绘制 |
| 画刷 | `CreateSolidBrush` | `GdiSelect` | `GdiRelease`（stock 兜底 `WHITE_BRUSH`） | `DeleteObject` | 同上 |
| 文字/序号字体 | `CreateFontW` | `GdiSelect` | `GdiRelease`（stock 兜底 `DEFAULT_GUI_FONT`） | `DeleteObject` | 创建失败直接返回；选入失败删字体后返回 |
| 输入框字体 | `CreateFontW` | `WM_SETFONT`（控件持有） | — | 控件销毁后 `DeleteObject` | 建控件失败 → 立即删字体 |
| `NULL_BRUSH` / `NULL_PEN` | stock | `GdiSelect` | `GdiRestore`（无临时对象可删） | 不释放（系统对象） | 选入失败则跳过该元素 |
| 菜单 / HRGN | `CreatePopupMenu` / `CreateRoundRectRgn` | — | — | `DestroyMenu` / `DeleteObject`（`SetWindowRgn` 失败时） | 创建失败安全返回 |

文字属性（`SetBkMode` / `SetTextColor`）在 `DrawNumberAnnot`、`DrawTextAnnot`、`OverlayDraw` 三处保存返回值并恢复，避免污染后续绘制。

---

## 3. 直接 `SelectObject()` 调用说明

全文件检索 `SelectObject(` 仅剩 3 处：

| 位置 | 用途 | 为何不用封装 |
|---|---|---|
| [`shot.c#L297`](file:///C:/code/quickshot/shot.c#L297) | `GdiSelect` 内部选入 | 封装本体，需要直接调用并校验返回值 |
| [`shot.c#L315`](file:///C:/code/quickshot/shot.c#L315) | `GdiRestore` 内部还原 | 封装本体，需要判断返回值并写日志 |
| [`shot.c#L336`](file:///C:/code/quickshot/shot.c#L336) | `GdiRelease` 内 stock 顶出 | 恢复失败后的补救，需要拿到被顶出的临时对象句柄 |

其余初始化选入、绘制切换、销毁前恢复三类路径全部通过封装，无遗漏。

恢复失败后的清理顺序（`GdiRelease`）：

1. 恢复原对象失败 → 说明 DC 里仍是临时对象；
2. 若该类型有 stock 替代（画笔/画刷/字体），先 `SelectObject(stock)` 把临时对象顶出；
3. 再 `DeleteObject(临时对象)`；仍失败则日志记录句柄值，不假设已释放；
4. 位图类没有 stock 替代，调用方统一采用「`GdiRestore` → `DeleteDC` → `DeleteObject`」，靠 `DeleteDC` 解除引用。

---

## 4. 恢复失败模拟测试

为覆盖「选入失败」分支，新增**只影响返回值、不触碰 DC** 的注入开关：

```c
static int g_gdiFailInject = 0;   /* 自检用，正常运行时恒为 0 */
```

`GdiSelect` 在校验参数之后、真正调用 `SelectObject` 之前检查该计数，命中时直接返回 `FALSE`。这样模拟失败时 DC 状态完全不变，不会破坏真实绘制。

覆盖到的用例与实测结果：

| 用例 | 构造方式 | 结果 |
|---|---|---|
| `gdi: restore with NULL dc / NULL object rejected` | 直接传非法参数 | PASS |
| `gdi: restore failure detected` | 用**已 `DeleteObject` 的画笔句柄**作为「原对象」 | PASS |
| `gdi: dc usable after failed restore` | 恢复失败后立刻再选入/恢复一次 | PASS |
| `gdi: release reports restore failure instead of success` | 恢复失败 + stock 顶出成功，检查返回值必须为 FALSE | PASS |
| `gdi: temp object not leaked / really deleted` | 同上，比较 GDI 计数并用 `GetObjectW` 验证句柄已失效 | PASS |
| `text measure: failed select yields zero-size rect` | 注入一次失败，调用 `TextAnnotRect` | `rect 0x0`，PASS |
| `text measure: font not leaked on select failure` | 同上，比较 GDI 计数 | `20 -> 20`，PASS |
| `text annot: select failure does not leak font` | 注入失败后调用 `DrawTextAnnot` | PASS |
| `restore: invalid old handle is cleared` | `RestoreOldBmp` 传入失效原对象 | PASS |
| `restore: bitmap deletable after forced restore failure` | 还原失败后 `DeleteDC` → `DeleteObject(bmp)` | PASS |

实现过程中该组测试真实抓到一个缺陷：最初 `GdiRelease` 在 stock 顶出成功后把 `restored` 置为 TRUE，导致「恢复失败却上报成功」。已改为分离 `restored` 与「是否成功顶出」，修复后通过。

---

## 5. 完整测试日志与退出码

### 5.1 构建

```bat
windres app.rc -O coff -o app.res
gcc -O2 -s -Wall -mwindows shot.c app.res -o snapshot.exe ^
    -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -lcomdlg32 -lm
```

退出码 0，`-Wall` 无告警。

### 5.2 `--stress-test`（80 项 PASS / 0 FAIL，退出码 0）

完整输出（`Get-Content stress.log`）：

```text
=== QuickShot stability self-test ===
  gdi after warmup: 20
capture: 1000x 41250 ms, gdi 20 -> 20, fails=0
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
mosaic: annots 500x cached 0 ms, 500x uncached 47 ms
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
startup hotkey: blocked=1 result=2 mods=0x3 vk=0x41
[PASS] startup falls back when saved hotkey is occupied
[PASS] fallback differs from saved combo
[PASS] fallback comes from preset list
[PASS] fallback keeps the saved config in registry
[PASS] startup hotkey released
[PASS] next start prefers the saved combo once it is free
[PASS] startup uses saved hotkey when free
startup all occupied: blocked=4/4 result=0 hotkeyOk=0
[PASS] startup survives when every preset is occupied
[PASS] failed startup leaves registry untouched
[PASS] menu switch works after a failed startup
[PASS] menu switch persists the new hotkey
[PASS] gdi: NULL dc rejected
[PASS] gdi: NULL object rejected
[PASS] gdi: restore with NULL dc rejected
[PASS] gdi: restore with NULL object rejected
[PASS] gdi: select pen
[PASS] gdi: restore pen
[PASS] gdi: delete after successful restore
[PASS] gdi: restore failure detected
[PASS] gdi: dc usable after failed restore
[PASS] gdi: release reports restore failure instead of success
[PASS] gdi: temp object not leaked after failed restore
[PASS] gdi: temp object really deleted
gdi font loop: 1000x, gdi 22 -> 22, fails=0
[PASS] gdi: font create/select/restore/delete 1000x
[PASS] gdi: font loop does not grow gdi count
gdi pen loop: 1000x, gdi 22 -> 23, fails=0
[PASS] gdi: pen create/select/restore/delete 1000x
[PASS] gdi: pen loop does not grow gdi count
gdi pen loop: second 1000x, gdi 23 -> 23
[PASS] gdi: pen loop second batch has no failures
[PASS] gdi: pen loop is steady after first batch
text measure: injected select failure, rect 0x0, gdi 20 -> 20
[PASS] text measure: failed select yields zero-size rect
[PASS] text measure: font not leaked on select failure
[PASS] text annot: select failure does not leak font
restore old bmp: gdi 20 -> 26 -> 20
[PASS] restore: capture buffers add 6 gdi objects
[PASS] restore: FreeCapture releases them
[PASS] restore: invalid old handle is cleared
[PASS] restore: bitmap deletable after forced restore failure
=== gdi total: 20 -> 18 ===
[PASS] no gdi leak across whole run
=== OK (0 failures) ===
```

### 5.3 其它验收命令

| 命令 | 退出码 | 关键输出 |
|---|---|---|
| `--capture-test` | 0 | `capture: 2560 x 1440`、`png: ok`、`hotkey Ctrl+Alt+A: ok` |
| `--toolbar-test` | 0 | `out-of-bounds: 0`、`cursor mismatches: 0`、`png: ok` |
| `--demo docs\overlay.png demo4.png` | 0 | `demo: ok` |
| `--stress-test` | 0 | 80 PASS / 0 FAIL |

### 5.4 渲染回归

`--demo` 输出 SHA256 与前几轮完全一致：

```text
A8D9086C38DA2914B751F7F56CD2258C28AA09D78185781573758C201D6F28F2
```

说明本轮封装与清理顺序调整没有改变任何绘制像素。

---

## 6. 快捷键回归

实现未改动，语义保持不变：

- 保存的组合空闲 → 直接生效（`startup uses saved hotkey when free`）
- 保存的组合被占用 → 按预设顺序回退（`startup falls back...`，实测回退到 `Ctrl+Alt+S`）
- 临时回退不写注册表（`fallback keeps the saved config in registry`）
- 全部预设被占用 → 返回无可用热键状态，程序继续运行（`blocked=4/4 result=0`）
- 托盘菜单仍可改键并持久化（`menu switch works / persists`）
- 原组合释放后，下次启动优先回到原组合（`next start prefers the saved combo once it is free`）

---

## 7. 剩余风险

| # | 风险 | 说明 |
|---|---|---|
| 1 | 真实 GDI 句柄耗尽未覆盖 | 用注入开关模拟「选入失败」、用失效句柄模拟「恢复失败」，但没有在真实资源耗尽条件下验证 `CreatePen/CreateFontW` 返回 NULL 的路径 |
| 2 | 注入开关是产品代码分支 | `g_gdiFailInject` 正常恒为 0，仅自检写入；如担心误用可在发布构建中用宏裁掉 |
| 3 | `DeleteObject` 因对象仍被引用而失败 | 只有日志，无法在合法 API 范围内进一步补救；依赖调用方保持「还原→删 DC→删对象」的顺序 |
| 4 | 位图类没有 stock 兜底 | 恢复失败时只能依赖 `DeleteDC` 解除引用，已通过测试验证可行 |
| 5 | 自检会临时改写热键注册表 | 测试前后备份还原；若测试期间用户手动改键会被还原值覆盖 |
| 6 | `shot.c` 约 4600 行（含自检） | 按既定计划，模块化拆分放到后续阶段 |

---

## 8. 收尾轮（提交 `v1.1.1`）：异常释放闭环与恢复结果检查

本轮只处理两项 P2，不改变任何正常功能表现。

### 8.1 `GdiRelease` 异常释放路径

[`shot.c#L386-L433`](file:///C:/code/quickshot/shot.c#L386-L433) 的新顺序：

1. `GdiFlushDeferred()` 顺手清理之前排队、现在已能释放的对象；
2. `GdiRestore()` 恢复原对象；
3. 恢复失败且有 stock 替代 → 顶出临时对象，**并检查顶出是否成功**；
4. `DeleteObject()` 并检查结果；
5. **删除失败时不再丢句柄**：把「对象 + 所属 DC」登记进延迟队列；
6. 返回 `restored && deleted`，异常情况一律 `FALSE`。

延迟队列（[`shot.c#L289-L317`](file:///C:/code/quickshot/shot.c#L289-L317)）：

```c
#define GDI_DEFER_MAX 16
typedef struct { HGDIOBJ obj; HDC hdc; } GdiDeferred;
```

- 所有权：登记后句柄由延迟队列持有，调用方不再负责；
- 生命周期：`GdiFlushDeferred()` 只对「DC 已释放、对象已可删」的条目重试 `DeleteObject`，**从不主动销毁任何 DC**（符合“不允许为释放临时对象而销毁外部共享 DC”）；
- 调用时机：`GdiRelease()` 入口、`FreeCapture()`、`MosaicCacheFree()`、`BuildFinalBitmap()`、钉图销毁/创建失败、`RunDemoShot()`、`TestSurfaceDestroy()` 等每个 `DeleteDC` 之后；
- 队列满（16）时明确日志，不会静默丢弃。

### 8.2 所有 `GdiRelease` 调用点及失败处理

| 调用点 | stock 兜底 | 失败时的行为 |
|---|---|---|
| `DrawPenStroke` "pen stroke" | NULL_PEN | 日志 + 计数 + 必要时延迟释放；绘制已完成，不影响后续 |
| `DrawEllipseAnnot` "ellipse pen" | NULL_PEN | 同上 |
| `DrawRectAnnot` "rect pen" | NULL_PEN | 同上 |
| `DrawArrowAnnot` "arrow brush" / "arrow pen" | WHITE_BRUSH / NULL_PEN | 同上；画刷未选入时直接 `DeleteObject` |
| `DrawNumberAnnot` "number circle brush" / "number font" | WHITE_BRUSH / DEFAULT_GUI_FONT | 同上 |
| `DrawTextAnnot` "text font" | DEFAULT_GUI_FONT | 同上 |
| `TextAnnotRect` "measure font" | DEFAULT_GUI_FONT | 失败时仍返回测量结果（可能为 0 尺寸），字体必删 |
| 自检 "release fallback" / "loop font" / "loop pen" / "loop pen 2" / "defer test" | 视用例 | 直接断言返回值与队列状态 |

绘图函数无法“撤销”已经画出的像素，因此失败不会中断绘制流程，但会同时满足三点：写日志（含上下文与句柄）、累加 `g_gdiReleaseFailures`、必要时进入延迟队列——不会被完全忽略。自检在**正常渲染阶段**断言三个计数器必须为 0（`no gdi failures during warmup rendering`）。

### 8.3 所有 `GdiRestore` 调用点及失败处理

| 调用点 | 恢复对象类型 | 失败影响 | 处理 |
|---|---|---|---|
| `RestoreOldBmp` "capture bitmap" | 位图 | 位图仍被 DC 引用 | 清空记录，由 `DeleteDC` 解除引用后删除 |
| `DrawEllipseAnnot` "ellipse brush" | stock NULL_BRUSH | 上一个画刷未恢复 | 日志 + 计数；后续画笔仍会正常释放 |
| `DrawRectAnnot` "rect brush" | stock NULL_BRUSH | 同上 | 同上 |
| `DrawArrowAnnot` "arrow null pen" | stock NULL_PEN | 同上 | 同上 |
| `DrawNumberAnnot` "number circle null pen" | stock NULL_PEN | 同上 | 同上 |
| `MosaicCacheFree` "mosaic cache bitmap" | 位图 | 位图仍被引用 | `DeleteDC` 后再删 |
| `BuildFinalBitmap` "final bitmap" | 位图 | 同上 | 同上 |
| `ToastProc` / `ToastShowAt` | 全局字体 | 提示字体未恢复 | 只影响本次绘制 DC，日志 + 计数 |
| `DrawToolbar` "toolbar separator pen" | 全局画笔 | 分隔线 DC 状态 | 只在 `GdiSelect` 成功时恢复，失败记日志 |
| `OverlayDraw` "selection outer/inner frame"、"selection frame brush"、"size label font" | 全局画笔/画刷/字体 | 选区装饰绘制 | **本轮修复**：尺寸标签先判断 `GdiSelect` 结果，未选入就不调用恢复（原先会无条件恢复空句柄） |
| `CreatePinWindow` / `PinProc` | 位图 | 位图仍被引用 | `DeleteDC` 后删除 |
| `RunDemoShot` / `RunToolbarTest` / `RenderOverlayToDib` / 自检微基准 / `TestSurfaceDestroy` | 位图 | 同上 | 同上 |

其余 stock 对象恢复失败只会让 DC 停留在 stock 状态（不会泄漏句柄），因此按“日志 + 计数”升级，不阻断绘制。

### 8.4 测试框架的两个修正（保证计数可信）

1. **补齐全局 GDI 对象**：自检直接创建遮罩窗口，没有走 `WM_CREATE`/捕获入口的 `OverlayGdiInit`，导致工具条画笔、选框画笔、标签字体都是空句柄，每次 `OverlayDraw` 产生 4～5 次“空对象选入”。新增 `TestOverlayEnsureGdi()` 在预热和文字用例后补齐，失败计数从 3000+ 降到 4（全部来自刻意构造的非法入参）。
2. **失败计数纳入断言**：`g_gdiSelectFailures` / `g_gdiRestoreFailures` / `g_gdiReleaseFailures` 在正常渲染阶段必须为 0。

### 8.5 故障注入测试结果

三个注入开关都只改返回值、不触碰 DC：

| 注入 | 用途 | 实测 |
|---|---|---|
| `g_gdiFailInject` | 选入失败 | `text measure: injected select failure, rect 0x0, gdi 35 -> 35` |
| `g_gdiRestoreFailInject` | 恢复失败 | `draw fail: gdi 39 -> 40 -> 40 (100 injected draws)`，像素比对 `2304 diff=0` |
| `g_gdiDeleteFailInject` | 删除失败（模拟仍被引用） | `defer: selected=1 released=0 deferredCount=1 (base 0) objType=1` |

延迟释放闭环实测：

```text
defer: selected=1 released=0 deferredCount=1 (base 0) objType=1
[PASS] defer: release reports failure
[PASS] defer: un-deletable object is retained
[PASS] defer: handle still valid before DC teardown
[PASS] defer: queue drained after DC destroyed
[PASS] defer: deferred object finally released
[PASS] defer: no gdi growth after deferred release
```

说明：`GetObjectType` 对“已删除但句柄尚未回收”的对象仍会返回类型，不能作为唯一判据，因此以「队列计数 + GDI 总量」作为释放证据。

### 8.6 回归结果

```text
gdi after warmup: 35
capture: 1000x 46375 ms, gdi 35 -> 35
text: 1000x commit/cancel, gdi 41 -> 41
gdi font loop: 1000x, gdi 37 -> 37
gdi pen loop: 1000x, gdi 37 -> 38（首次画线的一次性缓存）
gdi pen loop: second 1000x, gdi 38 -> 38
=== gdi total: 35 -> 31 ===
=== gdi failures: select=4 restore=107 release=2 deferred=0 freed=1 ===
=== OK (0 failures) ===
```

| 命令 | 退出码 | 结果 |
|---|---|---|
| `--stress-test` | 0 | **90 PASS / 0 FAIL** |
| `--capture-test` | 0 | 2560×1440、`png: ok`、热键可用 |
| `--toolbar-test` | 0 | `out-of-bounds: 0`、`cursor mismatches: 0` |
| `--demo` | 0 | SHA256 `A8D9086C…28F2`，与前几轮完全一致 |

计数器含义：`select=4`（2 次刻意非法入参 + 2 次文字选入注入）、`restore=107`（1 次绘制注入 + 100 次循环注入 + 6 次刻意失败）、`release=2`（两个刻意失败用例），均来自自检构造，正常路径为 0。

### 8.7 收尾轮新增风险

| # | 风险 | 说明 |
|---|---|---|
| 1 | 延迟队列容量 16 | 溢出时明确日志但无法继续保留；正常路径不会进入该分支 |
| 2 | `GetObjectType` 不能证明句柄已回收 | 改用 GDI 总量与队列计数作为判据 |
| 3 | 注入开关存在于产品代码 | 常量恒为 0，仅自检写入；如需可编译期裁掉 |

