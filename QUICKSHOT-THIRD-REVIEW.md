# QuickShot 第三阶段：GDI 资源安全审查 + 启动恢复机制

- 仓库：`https://github.com/peican996/quickshot`
- 基线：`56406a4`（第二轮）
- 修改文件：`shot.c`
- 环境：Windows，MinGW-w64 / UCRT，`-O2 -s -Wall -mwindows`，本轮全部结果为本机实跑

---

## 1. 问题 1：绘图函数缺少 GDI 创建/选入失败检查

### 1.1 确认存在

审查指出的 `DrawPenStroke` 确实没有检查 `CreatePen` 与 `SelectObject`；`DrawEllipseAnnot`、`DrawRectAnnot`、
`DrawArrowAnnot`、`DrawNumberAnnot`、`DrawTextAnnot`、`DrawPalette` 同样存在：

- 对象创建失败（`CreatePen`/`CreateSolidBrush`/`CreateFontW` 返回 NULL）时仍然调用 `SelectObject`；
- `SelectObject` 返回 NULL 或 `HGDI_ERROR` 时不检查，继续在“上一位”对象状态下绘制（画笔/画刷/字体全错）；
- 恢复原对象时不检查，DC 状态可能停留在临时对象上，随后 `DeleteObject` 删除仍被选中的对象（删除静默失败、GDI 计数只增不减）。

### 1.2 修改方案

新增两个统一入口（[shot.c#L517-L534](file:///C:/code/quickshot/shot.c#L517-L534)）：

```c
static BOOL GdiSelect(HDC hdc, HGDIOBJ obj, HGDIOBJ *old)   /* 建/选失败一律 FALSE */
static void GdiRestore(HDC hdc, HGDIOBJ old)                /* 空句柄/错误句柄安全跳过 */
```

规则固定为：**只有 `GdiSelect` 成功才绘制，失败直接放弃本次绘制并删除自己刚创建的对象；绘制完再 `GdiRestore`**。

| 函数 | 修改点 |
|---|---|
| `DrawPenStroke` | 笔建不出/选入失败 → 放弃 |
| `DrawEllipseAnnot` | 画笔选入失败 → 放弃；`NULL_BRUSH` 选入失败 → 不画但恢复画笔 |
| `DrawRectAnnot` | 同上 |
| `DrawArrowAnnot` | 杆、三角分别判断：三角画笔/画刷失败时杆仍然正确绘制 |
| `DrawNumberAnnot` | 圆底画笔+`NULL_PEN` 分别判断；字体创建失败只保留圆底；`SetBkMode/SetTextColor` 保存并恢复 |
| `DrawTextAnnot` | 字体创建/选入失败 → 放弃；文字颜色与背景模式保存并恢复 |
| `DrawPalette` | 每个色块/边框的画刷创建失败 → 跳过该元素，不影响其它色块 |
| `DrawSizeRowBg` / `DrawPinBg` | 边框画刷创建失败 → 只画底色 |
| `DrawToolbar` | 分隔线画笔选入失败 → 跳过线条，工具条其余部分照常绘制 |
| `OverlayDraw` | 选框画笔、尺寸标签字体全部改为 `GdiSelect`；文字属性用后恢复 |
| `ToastProc` / `PinProc` | 背景/边框画刷创建失败时跳过填充，不再对 NULL 刷 `FillRect` |
| `ShowTrayMenu` | 两个 `CreatePopupMenu` 失败时安全返回，不泄漏已建菜单 |

**关键原则**：不在 `SelectObject` 失败后继续画，也不在错误状态下执行 `DeleteObject`——这两点正是审查指出的隐患。

### 1.3 验证

- `--demo` 输出与第三阶段前**逐字节相同**（SHA256 `A8D9086C38DA2914B751F7F56CD2258C28AA09D78185781573758C201D6F28F2`），
  说明这次纯防御性改造没有改变任何绘制结果；
- 新增 `TestGdiGuards()` 验证入参防御（真实 GDI 耗尽无法在测试中可靠构造，见 §4）：

```text
[PASS] gdi guards: NULL dc rejected
[PASS] gdi guards: NULL object rejected
[PASS] gdi guards: valid object selected
```

---

## 2. 问题 2：启动时快捷键被占用缺少恢复策略

### 2.1 原行为

`LoadSettings` → `HotkeyTryApply(保存的组合)`，失败就只弹一个 MessageBox，之后整个进程没有任何有效热键，
只能靠用户自己发现并手动进托盘菜单改——期间按快捷键无反应。

### 2.2 新策略

新增 [`HotkeyRegisterOnStartup()`](file:///C:/code/quickshot/shot.c#L2748-L2769)：

1. 先尝试上次保存的组合；
2. 失败则按预设顺序（`Ctrl+Alt+A` → `Ctrl+Alt+S` → `Ctrl+Shift+A` → `Alt+Shift+A`）依次尝试；
3. 只有注册成功才更新内存中的 `g_mods/g_vk/g_hotkeyId`（复用 `HotkeyTryApply` 的事务逻辑）；
4. **不写注册表**：回退只影响本次运行，下次启动仍优先尝试用户原来选的那个，
   避免“一次临时占用”变成“永久改配置”；
5. 全部失败时保持注册表与内存一致，弹窗提示进托盘菜单手动设置，托盘菜单始终可用；
6. 启动提示气泡显示**实际生效**的组合；发生回退时提示“原快捷键被占用，已临时改用 …”，
   消除“配置和实际不一致”的困惑。

同时补上托盘提示的同步（[`TrayUpdateTip()`](file:///C:/code/quickshot/shot.c#L2830-L2841)）：启动注册后、
以及每次在菜单里成功切换热键后刷新 `szTip`，不再固定显示 `Ctrl+Alt+A`。

### 2.3 验证

新增 [`TestHotkeyStartupFallback()`](file:///C:/code/quickshot/shot.c#L3956-L3994)：用独立线程占住 `Ctrl+Shift+A`，
把“保存的配置”设成该组合，再调用启动注册函数。实测（`mods=0x3 vk=0x53` 即本次回退到 `Ctrl+Alt+S`，
因为本机有正在运行的真实 QuickShot 占用了 `Ctrl+Alt+A`）：

```text
startup hotkey: blocked=1 result=2 mods=0x3 vk=0x53
[PASS] startup falls back when saved hotkey is occupied
[PASS] fallback differs from saved combo
[PASS] fallback comes from preset list
[PASS] startup hotkey released
[PASS] startup uses saved hotkey when free
```

`result=2` 即 `HK_START_FALLBACK`；最后一项验证保存的组合空闲时走 `HK_START_SAVED`，不会做无谓回退。

---

## 3. 修改清单

| 位置 | 改动 |
|---|---|
| `GdiSelect()` / `GdiRestore()` | 新增统一选入/恢复入口 |
| `DrawPenStroke`、`DrawEllipseAnnot`、`DrawRectAnnot`、`DrawArrowAnnot`、`DrawNumberAnnot`、`DrawTextAnnot` | 创建/选入失败检查，文字属性保存恢复 |
| `DrawPalette`、`DrawSizeRowBg`、`DrawPinBg`、`DrawToolbar` | 画刷/画笔失败时跳过对应元素 |
| `OverlayDraw` | 选框画笔、标签字体、文字属性走统一检查 |
| `ToastProc`、`PinProc`、`ShowTrayMenu` | 画刷/菜单创建失败时的安全分支 |
| `HotkeyRegisterOnStartup()` | 新增启动恢复（保存→预设回退，不回写配置） |
| `HotkeyLabel()` / `TrayUpdateTip()` | 新增，提示与生效组合同步 |
| WinMain 启动段 | 使用新注册函数，提示气泡显示实际组合与回退说明 |
| `TestHotkeyStartupFallback()`、`TestGdiGuards()` | 新增测试 |
| `StartHotkeyBlocker()` / `StopHotkeyBlocker()` | 抽出占位线程封装，供两个热键用例复用 |

---

## 4. 测试结果

### 4.1 `--stress-test`（本机实跑，退出码 0，54 项检查全部 PASS）

```text
=== QuickShot stability self-test ===
capture: 1000x 38281 ms, gdi 20 -> 20, fails=0
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
mosaic: annots 500x cached 0 ms, 500x uncached 16 ms
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
startup hotkey: blocked=1 result=2 mods=0x3 vk=0x53
[PASS] startup falls back when saved hotkey is occupied
[PASS] fallback differs from saved combo
[PASS] fallback comes from preset list
[PASS] startup hotkey released
[PASS] startup uses saved hotkey when free
[PASS] gdi guards: NULL dc rejected
[PASS] gdi guards: NULL object rejected
[PASS] gdi guards: valid object selected
=== gdi total: 20 -> 18 ===
[PASS] no gdi leak across whole run
=== OK (0 failures) ===
```

### 4.2 其它自检

| 命令 | 结果 |
|---|---|
| `--capture-test` | 退出码 0，`png: ok`（`hotkey Ctrl+Alt+A: occupied` 是本机已有实例占用，属环境状态） |
| `--toolbar-test` | 退出码 0，`out-of-bounds: 0`、`cursor mismatches: 0` |
| `--demo` | 退出码 0，SHA256 与第三阶段前完全一致 |

---

## 5. 回归结论

| 检查项 | 结果 |
|---|---|
| 编译 | `-Wall` 无告警 |
| 渲染结果 | `--demo` 输出逐字节相同（`A8D9086C…`） |
| GDI 计数 | 截图 1000 次 20→20；文字 1000 次 26→26；整体 20→18 |
| 马赛克 | 预览/导出逐像素 0 差异（各 196,144 px） |
| 热键 | 冲突拒绝、旧热键保留、空闲切换、启动回退、启动直用，全部通过 |
| 新增资源 | 占位线程/事件句柄、菜单句柄在每条分支都有释放 |

---

## 6. 测试可信度说明

- 本报告所有输出都是在本机 Windows（2560×1440 桌面会话）用本轮源码重新编译、实际运行得到的，
  不是引用上一轮结果；`--stress-test` 的压缩日志与完整日志保存在构建目录（不入库）。
- 仍无法自动化覆盖的部分：
  - **真实 GDI 耗尽**：无法在测试进程里可靠地把 GDI 句柄用尽而不影响系统，
    因此只验证了统一的入参防御（NULL DC / NULL 对象）与“成功路径不变”；
  - 中文输入法上屏、多显示器混合 DPI、Explorer 重启后的托盘恢复：需要人工回归；
  - 启动回退依赖“占位线程能抢到目标组合”，若本机该组合已被第三方占用，用例会标记 SKIP 而不误判失败。

---

## 7. 遗留问题

| # | 问题 | 风险 | 说明 |
|---|---|---|---|
| 1 | 回退后的快捷键不写注册表 | 低 | 有意设计：保留用户偏好，下次启动仍优先尝试原组合；本次运行通过托盘提示与启动气泡明确告知 |
| 2 | 托盘提示只在启动和手动切换后刷新 | 低 | 已覆盖所有会改变热键的入口；外部抢注不会改变本进程状态 |
| 3 | GDI 耗尽只能在真实资源紧张时才会触发防御分支 | 低 | 代码路径已按“失败即放弃绘制并正确恢复”实现，但缺少可复现的自动化手段 |
| 4 | `shot.c` 仍是单文件（约 3800 行，含自检） | 中 | 按审查建议暂不拆分，先积累稳定版本；模块化留到下一阶段 |
| 5 | 剪贴板拒绝 >2GB DIB（沿用） | 极低 | 远超实际截图尺寸 |
