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

---

## 9. 最终收尾（`v1.1.2`）：延迟释放队列动态扩容与所有权闭环

基线：`d67d277`。本轮只改延迟释放队列与相关测试，不动其他功能。

### 9.1 队列新设计

[`shot.c#L287-L409`](file:///C:/code/quickshot/shot.c#L287-L409)

```c
static HGDIOBJ *g_gdiDefer;        /* 动态数组，只存句柄 */
static int      g_gdiDeferredCount;
static int      g_gdiDeferredCap;  /* 按需翻倍：8 → 16 → 32 → 64 … */
static HGDIOBJ  g_gdiDeferEmergency[8];   /* 扩容失败时的兜底槽 */
```

| 设计点 | 做法 |
|---|---|
| 容量 | 无固定上限，`count == cap` 时按 2 倍扩容（首次 8） |
| 扩容失败 | 落入 8 个固定紧急槽，`g_gdiDeferAllocFailures++`，日志记录 |
| 紧急槽也满 | `g_gdiDeferLostHandles++` 并写 ERROR 日志，**不静默** |
| 去重 | `GdiDeferContains()` 同时检查主队列与紧急槽，同一句柄只登记一次 |
| 存储字段 | **只存句柄，不再保存 HDC**：队列从不销毁 DC，能否删除只由 `DeleteObject` 结果决定，避免对 DC 生命周期产生错误假设 |
| 移除条件 | 只有 `DeleteObject` 成功（或对象已不存在）才移除 |
| 进程退出 | `GdiDeferReleaseAll()` 再试一次并释放数组；仍有残留则记日志，不强行处理 |

**所有权规则**

1. `GdiDeferRetain()` 返回 `TRUE` → 句柄所有权转移给队列，调用方（`GdiRelease`）不得再释放；
2. 返回 `FALSE`（紧急槽也满）→ 调用方仍无权释放，句柄已计入丢失计数并写 ERROR 日志；
3. 队列只在真正删除成功后放弃句柄；
4. `GdiRelease` 的返回值仍为 `restored && deleted`，登记成功不代表“已释放”，只代表“已托管”。

### 9.2 `GdiRelease` 各失败路径

| 情况 | 句柄归属 / 处理 |
|---|---|
| 恢复成功、删除成功 | 已释放，返回 TRUE |
| 恢复成功、删除失败 | 登记队列（或紧急槽），返回 FALSE |
| 恢复失败、stock 顶出成功 | 顶出后删除；删除成败同上，返回值仍为 FALSE |
| 恢复失败、stock 顶出失败 | 直接尝试删除；失败则登记队列 |
| 恢复失败、`stockFallback = -1` | 直接尝试删除；失败则登记队列（位图类走 `DeleteDC` 路径） |
| 登记成功 | 队列持有；之后由 `GdiFlushDeferred()` 重试 |
| 登记失败（紧急槽满） | `g_gdiDeferLostHandles++` + ERROR 日志，绝不假装成功 |
| 重试仍未成功 | 保留在队列，下次 flush 继续 |
| 重试成功 | 移出队列，`g_gdiDeferredDone++` |

### 9.3 新增测试与实测输出

| 用例 | 覆盖点 |
|---|---|
| `defer: ...`（原有） | 恢复失败 + stock 无替代 + 删除失败 → 登记 → DC 销毁后释放 |
| `TestGdiDeferredBulk` | **40 条**延迟任务，验证超出原 16 上限后自动扩容、DC 销毁后全部回收 |
| `TestGdiDeferredDuplicate` | 同一句柄登记两次只保留一条、只释放一次 |
| `TestGdiDeferredAllocFailure` | 注入扩容失败 → 落紧急槽 → 仍能释放 |
| `TestGdiLifecycle` | 正常恢复/删除、恢复失败识别、stock 顶出、1000 次字体/画笔循环 |
| `TestGdiRestoreFailureDrawing` | 恢复失败后继续绘制，像素与参照图完全一致 |
| `TestRestoreOldBmp` | 缓冲分配/释放、原对象失效时的清理路径 |

```text
defer: selected=1 released=0 deferredCount=1 (base 0) objType=1
[PASS] defer: release reports failure
[PASS] defer: un-deletable object is retained
[PASS] defer: handle still valid before DC teardown
[PASS] defer: queue drained after DC destroyed
[PASS] defer: deferred object finally released
[PASS] defer: no gdi growth after deferred release
defer bulk: made=40 queued=40 (base 0, cap 64)
[PASS] defer bulk: 40 objects created and deferred
[PASS] defer bulk: queue grew past the old 16 limit
[PASS] defer bulk: backing array expanded
defer bulk: after destroy queued=0 freed=40
[PASS] defer bulk: queue drained after DC destroyed
[PASS] defer bulk: all 40 objects released
[PASS] defer bulk: no gdi growth
defer dup: first=1 second=1 queued=1 (base 0)
[PASS] defer dup: duplicate registration is accepted silently
[PASS] defer dup: only one entry kept
[PASS] defer dup: released exactly once
[PASS] defer dup: free counted once
[PASS] defer dup: no gdi growth
defer alloc: filled=64 retained=1 emergency=1 allocFails=1
[PASS] defer alloc: allocation failure falls back to emergency slot
[PASS] defer alloc: emergency entry released
[PASS] defer alloc: main queue returns to baseline
[PASS] defer alloc: all retained objects freed
[PASS] defer alloc: no gdi growth
draw fail: gdi 39 -> 40 -> 40 (100 injected draws)
draw fail: pixels compared=2304 diff=0
=== gdi total: 35 -> 31 ===
=== gdi failures: select=4 restore=107 release=2 deferred=0(emg 0) freed=107 allocFail=1 lost=0 ===
[PASS] no pending deferred gdi objects at exit
[PASS] no gdi handle lost during whole run
```

`select=4` / `restore=107` / `release=2` / `allocFail=1` 全部来自自检刻意构造；`lost=0` 表示整个运行期间没有任何句柄因队列满而丢失。

### 9.4 顺带修正的一处测试缺陷

马赛克缓存性能对比原先用 `GetTickCount64`（15.6ms 粒度）跑 500 次，两条路径常常落在同一刻度（实测 `16 ms vs 15 ms`）导致随机误判。改为 `QueryPerformanceCounter` 微秒计时后结果稳定：

```text
mosaic: annots 500x cached 3068 us, 500x uncached 20992 us
```

### 9.5 回归结果

| 命令 | 退出码 | 结果 |
|---|---|---|
| `--stress-test` | 0 | **107 PASS / 0 FAIL**（连续两次一致） |
| `--capture-test` | 0 | 2560×1440、`png: ok`、热键可用 |
| `--toolbar-test` | 0 | `out-of-bounds: 0`、`cursor mismatches: 0` |
| `--demo` | 0 | SHA256 `A8D9086C…28F2`，与此前各轮逐字节一致 |

其他回归点：马赛克预览/导出仍为逐像素 0 差异（196,144 px）；快捷键启动回退 4 项用例全部通过；正常渲染阶段 GDI 失败计数为 0；`-Wall` 无新增告警；整体 GDI 35 → 31，无持续增长。

### 9.6 尚未解决的风险

| # | 风险 | 说明 |
|---|---|---|
| 1 | 紧急槽仅 8 个 | 动态扩容失败且同时有 >8 个待托管对象时，仍会丢失并计数；正常路径不会进入 |
| 2 | 无法模拟真实 OOM | 扩容失败用注入开关验证；真实内存耗尽下的 `realloc` 行为未实测 |
| 3 | 注入开关位于产品代码 | 常量恒为 0，仅自检写入 |
| 4 | 延迟队列不在崩溃路径上生效 | 进程被强杀时队列随进程消失，属预期 |
| 5 | `shot.c` 单文件约 5000 行（含自检） | 按约定不再扩大本轮范围，模块化留到后续 |

---

## 10. 收尾轮（`v1.1.3`）：延迟释放队列静态预留与所有权闭环

基线：`bd01b8e`。只改延迟释放队列、退出清理与对应测试。

### 10.1 原问题的根因

上一版队列用 `realloc` 动态扩容，扩容失败后落到 8 个固定紧急槽；紧急槽满时
`GdiDeferRetain()` 返回 `FALSE`，而 `GdiRelease()` 只是记一行日志就把句柄丢掉了。

根因不是“槽太少”，而是：

1. **接管能力依赖动态分配**——内存分配失败时没有任何不依赖堆的兜底；
2. **返回值没有约束力**——返回 `FALSE` 后调用方既不持有也不释放，句柄就此失联；
3. 固定槽数量与“同时可能删不掉的对象数”之间没有任何论证关系。

### 10.2 新机制

[`shot.c#L296-L407`](file:///C:/code/quickshot/shot.c#L296-L407)

| 项目 | 设计 |
|---|---|
| 存储 | `static HGDIOBJ g_gdiDefer[64]`，**全程不使用 malloc/realloc** |
| 容量依据 | 对象只有“仍被某个 DC 引用”时才删不掉；每个 DC 每类对象最多持有一个，本程序同时存活的 DC（截屏 3 + 马赛克缓存 32 + 临时 DC）远小于 64 |
| 满时策略 | 先 `GdiDeferEvict()`：对每条重试删除，删掉的成功条目立刻让出槽位 |
| 驱逐后仍满 | `GdiDeferRejected++` + ERROR 日志 + 返回 `FALSE`，**所有权退回调用方** |
| 去重 | `GdiDeferContains()`，同一句柄只登记一次 |
| 移除条件 | 只有删除成功（或对象已不存在）才出队 |
| 边界 | 写入前检查 `count < GDI_DEFER_SLOTS`，不做任何乘法/翻倍/尺寸计算 |
| 退出 | 业务窗口与 DC 销毁后 `GdiDeferReleaseAll()`：再 flush 一次、逐条记录残留、清空队列 |

**所有权契约（写在代码注释里）**

1. `GdiDeferRetain()` 返回 `TRUE` → 所有权归队列，调用方不得再释放；
2. 返回 `FALSE` → **所有权仍归调用方**。`GdiRelease()` 会立刻重试删除：
   - 成功 → `deleted = TRUE`，如实返回；
   - 仍失败 → `g_gdiDeferLostHandles++` + ERROR 日志，**绝不假装成功**；
3. 队列从不销毁任何 DC。

### 10.3 一处重要的真实行为修正

实测发现：**Windows 允许对“已选入 DC 的画笔/画刷/字体”调用 `DeleteObject` 并返回 `TRUE`**
（对象被标记删除，DC 解引用后才真正释放）。所以“仍被引用 ⇒ 删除失败”这个前提在真实 GDI 上并不成立，
队列实际上是比原先设想更深一层、更难触发的安全网。

因此容量/所有权两类测试改用统一注入入口 `GdiTryDelete()`（`flush`、`evict`、`GdiRelease` 共用），
在**不破坏真实 DC** 的前提下构造“删不掉”的状态。这条行为差异也已写进代码注释。

### 10.4 新增/调整的测试

| 测试 | 覆盖点 | 结果 |
|---|---|---|
| `TestGdiDeferredRelease` | 单对象：删不掉 → 登记 → DC 销毁后释放 | PASS |
| `TestGdiDeferredBulk` | 40 条同时待释放，全部回收 | PASS |
| `TestGdiDeferredDuplicate` | 重复登记只留一条、只释放一次 | PASS |
| `TestGdiDeferredCapacity` | 真实构造 **64 个**被 64 个 DC 锁住的对象：填满、第 65 个被拒绝且所有权退回、DC 销毁后 64 个全部回收、之后可继续登记 | PASS |
| `TestGdiDeferredOwnership` | flush 不会强行删除仍被引用的对象；DC 销毁后才释放 | PASS |
| `TestGdiLifecycle` / `TestTextAnnotSelectFailure` / `TestGdiRestoreFailureDrawing` / `TestRestoreOldBmp` | 原有 GDI 恢复失败相关用例 | PASS |

关键实测输出：

```text
defer: selected=1 released=0 deferredCount=1 (base 0) objType=1
defer bulk: made=40 queued=40 (base 0, slots 64)
defer bulk: after destroy queued=0 freed=40
defer dup: first=1 second=1 queued=1 (base 0)
defer cap: made=64 queued=64 rejected=0
[PASS] defer cap: overflow is rejected instead of silently dropped
[PASS] defer cap: rejection is counted
[PASS] defer cap: queue stays within bounds
[PASS] defer cap: caller still owns and frees it
defer cap: after dc destroy queued=0 freed=64
[PASS] defer cap: registration works again after drain
defer own: selected=1 retained=1 queued=1 type=1
[PASS] defer own: flush does not force-delete referenced object
[PASS] defer own: released after dc destroyed
=== gdi failures: select=4 restore=107 release=2 | deferred=0 freed=108 evicted=0 rejected=1 lost=0 (slots 64) ===
```

### 10.5 退出流程

`WinMain` 的退出顺序（[shot.c#L5205-L5213](file:///C:/code/quickshot/shot.c#L5205-L5213)）：

1. `HotkeyUnregisterActive()`
2. `OverlayReleaseAll()`：销毁业务窗口、EDIT 控件、马赛克缓存与全部截屏 DC
3. 删除 4 个全局字体并将其句柄置空
4. `GdiDeferReleaseAll()`：最后一次延迟释放 → 逐条记录仍无法释放的对象（句柄 + 类型）→ 清空队列
5. `GdiplusShutdown()`

顺序保证：**先销毁持有引用的 DC，再做最终释放**，不会提前删掉仍被引用的对象。

### 10.6 回归结果

| 命令 | 退出码 | 结果 |
|---|---|---|
| `--stress-test` | 0 | **118 PASS / 0 FAIL** |
| `--capture-test` | 0 | 2560×1440、`png: ok`、热键可用 |
| `--toolbar-test` | 0 | `out-of-bounds: 0`、`cursor mismatches: 0` |
| `--demo` | 0 | SHA256 `A8D9086C…28F2`，与此前各轮逐字节一致 |

GDI 数量：`32 -> 25`；截图 1000 次 `32 → 32`；文字 1000 次 `38 → 38`；字体循环 `34 → 34`；
画笔两轮 `34 → 35 → 35`；延迟队列结束时 `deferred=0 lost=0`。`-Wall` 无新增告警。

### 10.7 仍然存在的资源丢失路径

| # | 路径 | 说明 |
|---|---|---|
| 1 | 64 条同时“删不掉” | 会拒绝登记并把所有权退回调用方；`GdiRelease` 重试删除后仍失败才计入 `lost`。正常操作下不可达（需要 64 个 DC 各锁住一个对象） |
| 2 | 进程被强杀 | 队列与进程同归于尽，属预期，无法在进程内补救 |
| 3 | `DeleteObject` 对已选入对象返回 TRUE 的延迟释放 | 由 GDI 在 DC 解引用后完成，本程序无法感知；`GetObjectType` 在回收前仍返回类型，测试已改用 GDI 总量判定 |



