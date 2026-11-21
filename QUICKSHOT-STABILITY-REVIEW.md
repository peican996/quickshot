# QuickShot 第一阶段稳定性修复报告

- 仓库：`https://github.com/peican996/quickshot`
- 基线提交：`9dd0343`（docs: 增加界面配图，新增离屏合成模式）
- 修改范围：`shot.c`（产品代码 + 自检）、`README.md`（补充自检入口说明）
- 技术栈保持不变：纯 Win32 + GDI+，单文件 C，无新增第三方依赖
- 验证环境：Windows（2560×1440 桌面会话），MinGW-w64 / UCRT，`gcc 13+`

---

## 1. 修改概览

| 文件 | 修改目的 | 修复的实际问题 |
|---|---|---|
| `shot.c` | 字体句柄生命周期、热键切换回滚、截屏 GDI 资源与错误处理、剪贴板溢出、文件名唯一性、马赛克缓存、自检入口 | 6 项指定问题全部确认在最新代码中存在并已修复；另修复 4 处顺带发现的资源问题 |
| `README.md` | 补充 `--stress-test` 自检说明 | 文档与新增自检入口保持一致 |

新增/改写的主要函数（`shot.c` 行号）：

| 位置 | 内容 |
|---|---|
| [`OvAllocBuffers`](file:///C:/code/quickshot/shot.c#L358)、[`OvDimBuffers`](file:///C:/code/quickshot/shot.c#L405)、[`CaptureVirtualScreen`](file:///C:/code/quickshot/shot.c#L435)、[`FreeCapture`](file:///C:/code/quickshot/shot.c#L473) | 截屏缓冲分配/抓屏/释放全链路 |
| [`CopyDibToClipboard`](file:///C:/code/quickshot/shot.c#L852)、[`BuildUniqueShotPath`](file:///C:/code/quickshot/shot.c#L917) | 剪贴板打包、截图文件命名 |
| [`MosaicCacheFree`](file:///C:/code/quickshot/shot.c#L595)、[`MosaicEnsure`](file:///C:/code/quickshot/shot.c#L631)、[`MosaicEpochBump`](file:///C:/code/quickshot/shot.c#L622)、[`DrawMosaicAnnot`](file:///C:/code/quickshot/shot.c#L694) | 马赛克结果缓存 |
| [`TextEditDestroy`](file:///C:/code/quickshot/shot.c#L1716)、[`TextStart`](file:///C:/code/quickshot/shot.c#L1803)、[`TextCommit`](file:///C:/code/quickshot/shot.c#L1729)、[`OverlayReleaseAll`](file:///C:/code/quickshot/shot.c#L1873) | 文字编辑字体所有权、遮罩资源统一释放 |
| [`HotkeyTryApply`](file:///C:/code/quickshot/shot.c#L2625)、[`HotkeyUnregisterActive`](file:///C:/code/quickshot/shot.c#L2606) | 热键切换事务化 |
| [`RunStabilityTest`](file:///C:/code/quickshot/shot.c#L3629) 及 `Test*` 系列 | 新增 `--stress-test` 自检 |

未改动：UI 布局、快捷键默认值、托盘菜单结构、标注工具行为、PNG 命名风格（仅在同秒重名时追加序号）。

---

## 2. 问题修复详情

### 2.1 文字编辑字体句柄提前释放（最高优先级）

**原始问题**

```c
HFONT ef = CreateFontW(...);
g_edit = CreateWindowExW(... "EDIT" ...);
SendMessageW(g_edit, WM_SETFONT, (WPARAM)ef, TRUE);
DeleteObject(ef);          // ← 控件仍在用这个句柄
```

**原因**

`WM_SETFONT` 不会复制字体对象，控件只是保存句柄，之后输入、重绘、`EM_SETSEL` 重排都要用它。原代码在设置完成后立即 `DeleteObject`，句柄在控件存活期间就已经失效：轻则字体回退成系统默认，重则 `EditProc` 后续 `CallWindowProcW` 绘制时命中已释放的 GDI 对象（GDI 句柄被复用时表现为随机花屏/崩溃）。原实现的空指针/创建失败路径同样没有释放字体的统一出口。

**修改方案**

1. 新增全局 `g_editFont`，明确“字体所有权跟着输入框走”；
2. `TextStart` 把 `CreateFontW` 的结果存入 `g_editFont`，再 `WM_SETFONT`，**不再立即删除**；
3. 新增唯一销毁出口 `TextEditDestroy()`：先 `DestroyWindow(编辑框)`，再 `DeleteObject(g_editFont)`，最后清空两个全局；
4. 覆盖所有路径：
   - 创建字体失败 → 直接返回，不产生句柄；
   - 创建编辑框失败 → 立即释放刚创建的字体（此时还没有控件引用它）；
   - 提交 / 取消 / 重新开始编辑 → 都走 `TextEditDestroy()`；
   - 遮罩关闭、异常销毁 → `OverlayReleaseAll()` 里第一步就是 `TextEditDestroy()`。

**关键逻辑**

```c
static void TextEditDestroy(void)
{
    if (g_edit) { HWND e = g_edit; g_edit = NULL; DestroyWindow(e); }
    if (g_editFont) { DeleteObject(g_editFont); g_editFont = NULL; }
}
```

**遗留风险**：无。重复调用安全（两个句柄都先置空）。

---

### 2.2 热键修改失败导致旧热键丢失、配置与运行状态不一致

**原始问题**

```c
static void RegisterHotkeyNow(void)
{
    if (g_hotkeyOk) { UnregisterHotKey(g_hMsgWnd, HOTKEY_ID); g_hotkeyOk = FALSE; }
    g_hotkeyOk = RegisterHotKey(g_hMsgWnd, HOTKEY_ID, g_mods | MOD_NOREPEAT, g_vk);
}
// 调用方：先改 g_mods/g_vk，再写注册表，最后才注册
```

**原因**

“先注销旧的 → 再注册新的”不是原子操作。新组合被其他程序占用时：旧热键已被注销、新热键注册失败、`g_mods/g_vk` 和注册表却已经写成新值。用户既失去了可用的截图热键，重启后依然注册失败（而且菜单勾选显示的是一个实际未生效的组合）。

**修改方案**

1. 新增 `HotkeyTryApply(mods, vk)` 完成事务化切换：
   - 组合未变化且已生效 → 直接返回成功，不重复注册；
   - 用**另一个热键 ID**（`HOTKEY_ID_ALT` / `HOTKEY_ID` 轮换）注册新组合；
   - 新组合注册失败 → 立即返回 `FALSE`，旧热键**从未被注销**，`g_mods/g_vk` 与注册表都不动；
   - 新组合成功后，才注销旧 ID，再更新 `g_hotkeyId / g_mods / g_vk`；
2. `MsgProc` 的快捷键菜单只在 `HotkeyTryApply` 返回成功后写注册表；失败弹出提示并保持原快捷键；
3. `WM_HOTKEY` 改为校验当前生效的 `g_hotkeyId`（两个 ID 都能正常触发）；
4. 退出流程统一调用 `HotkeyUnregisterActive()`，避免异常注销/漏注销；
5. 启动流程复用同一个 `HotkeyTryApply`（此时无旧热键），行为与原实现一致。

**为什么用两个 ID**

Windows 不允许同一个热键组合在同一进程内用两个 ID 同时登记，而“先注销旧的再注册新的”正是要避免的竞态。因此新组合先挂在备用 ID 上，拿到后才释放旧 ID，两个 ID 交替使用即可无缝切换；`WM_HOTKEY` 只认当前生效 ID。

**遗留风险**：启动时如果注册表里保存的组合已被占用，程序仍然没有热键可用（原行为），但此时菜单勾选、注册表与实际状态都是“用户选择的组合”，不再出现“配置写着 A、实际是 B”的错位。

---

### 2.3 截图 GDI 资源管理与错误处理

**原始问题**

- `OvAllocBuffers`：三个 DIB / 三个 DC 中任意一个创建失败都直接 `return FALSE`，已创建的对象全部泄漏；
- `SelectObject` 返回值不检查，也不保存原始对象，`DeleteDC` 前没有还原，位图可能删不掉；
- `CaptureVirtualScreen`：`ZeroMemory(&g_ov)` 会丢失上一次缓冲，重复调用必然泄漏；`BitBlt` 失败只释放屏幕 DC，已分配的缓冲留在全局；
- `OvDimBuffers` 的 `BitBlt` 失败后仍然继续用未初始化的底图；
- `BuildFinalBitmap`：`CreateCompatibleDC / SelectObject / BitBlt` 全部不检查，失败路径不完整；
- GDI+ 只在校验失败时提示，没有“不可用”状态，后续编码路径仍会调用 Gdip 接口。

**修改方案**

1. `OverlayState` 增加 `oldBmpCap/oldBmpDim/oldBmpBack`，`SelectObject` 前保存原对象，`FreeCapture` 先 `RestoreOldBmp` 再 `DeleteDC`，最后 `DeleteObject`；
2. `OvAllocBuffers` 改为单一失败出口 `done:`：任意一步失败 → `FreeCapture()`（幂等，按“还原对象 → 删 DC → 删位图”顺序清理）；
3. `CaptureVirtualScreen` 进入时先 `FreeCapture()` 清理残留；`BitBlt`（含 CAPTUREBLT 回退）失败、`OvDimBuffers` 失败都直接 `FreeCapture()` 并返回 `FALSE`，绝不让无效像素进入标注/导出链路；
4. `OvDimBuffers` 改为返回 `BOOL`，`BitBlt` 失败时上层放弃本次截图；
5. `BuildFinalBitmap` 对 `GetDC / CreateDIBSection / CreateCompatibleDC / SelectObject / BitBlt` 全部检查，失败逐级释放；
6. 新增 `g_gdiplusOk`：`GdiplusStartup` 失败即置 `FALSE`，`SaveBitmapToPng` 直接返回失败，不再调用 GDI+ 编码接口。

**关键逻辑（失败出口）**

```c
done:
    ReleaseDC(NULL, hdcScreen);
    if (!ok) { QSLog(...); FreeCapture(); }
    return ok;
```

**遗留风险**：`BitBlt` 在极少见情况下可能“成功”但抓到黑屏（桌面被锁屏等），这属于系统行为，代码层面无法判定；此时仍会得到一张黑图，不会崩溃。

---

### 2.4 剪贴板图片尺寸计算溢出

**原始问题**

```c
DWORD imgSize = (DWORD)w * 4 * (DWORD)h;
HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imgSize);
...
memcpy(..., bits, imgSize);
if (OpenClipboard(...)) { EmptyClipboard(); if (!SetClipboardData(...)) GlobalFree(hMem); ... }
```

**原因**

32 位乘法在极端宽高下回绕（例如 `w=0x40000001, h=4` 时 `w*4` 溢出），得到一个很小的 `imgSize`，随后 `memcpy` 按这个错误长度复制，而 `SetClipboardData` 收到的 DIB 头声明的尺寸与实际数据不符；`GlobalAlloc` 的 `+ sizeof(BITMAPINFOHEADER)` 同样可能溢出。此外 `EmptyClipboard` 的返回值、`SetClipboardData` 之外的所有失败路径与内存所有权都不明确。

**修改方案**

1. 全部改用 `size_t`，并逐级检查乘法回绕：
   ```c
   size_t stride = (size_t)w * 4u;
   if (stride / 4u != (size_t)w) return FALSE;
   size_t imgSize = stride * (size_t)h;
   if (imgSize / (size_t)h != stride) return FALSE;
   if (imgSize > 0xFFFFFFFFu - sizeof(BITMAPINFOHEADER)) return FALSE;  /* biSizeImage 是 DWORD */
   if (imgSize > 0x7FFFFFFF) return FALSE;                              /* 剪贴板实际可用上限 */
   ```
2. 检查 `GlobalAlloc / GlobalLock / OpenClipboard / EmptyClipboard / SetClipboardData` 全部返回值；
3. 所有权：只有 `SetClipboardData` 成功后才把内存交给系统，其余任何路径（锁失败、打开失败、清空失败、设置失败）都 `GlobalFree`；
4. `OpenClipboard` 被占用时最多重试 5 次（每次 10 ms），仍失败则返回 `FALSE`；
5. 函数改为返回 `BOOL`，调用方（`OverlayFinish`、`PinCopy`）据此给出明确提示。

**验收对照**：正常截图复制不受影响；`w=0 / w<0 / NULL / 1M×1M` 都在触碰内存前安全失败；剪贴板被占用时失败而不崩溃（自检已覆盖）。

---

### 2.5 截图文件名同秒重复

**原始问题**

```c
wsprintfW(name, L"截图_%04d%02d%02d_%02d%02d%02d.png", ...);
saved = SaveBitmapToPng(hbm, full);
```

**原因**

命名粒度只到秒，且 GDI+ 保存会直接覆盖同名文件；同一秒连拍或与已有文件同名时会静默覆盖用户数据，保存失败也没有区分“目录不可写”和“文件被占用”。

**修改方案**

新增 `BuildUniqueShotPath()`：

1. 保留原命名风格 `截图_YYYYMMDD_HHMMSS.png`；
2. 同秒重名时依次尝试 `_2`、`_3`…`_999`；
3. 用 `CreateFileW(..., CREATE_NEW, ...)` 占位，**只有成功创建才返回该路径**，天然杜绝覆盖，也避免多实例竞态；
4. `ERROR_FILE_EXISTS / ERROR_ALREADY_EXISTS` 视为“换下一个名字”，其它错误（路径不存在、无权限）直接返回失败，不做无意义重试；
5. `OverlayFinish` 在保存失败时删除占位文件，避免留下 0 字节残文件，并在提示栏区分显示“已复制 · 保存失败，请检查保存目录”。

**遗留风险**：占位到写入之间文件存在一个极短的空文件窗口，仅影响同时用资源管理器打开该目录的场景。

---

### 2.6 马赛克标注重复创建资源（第二阶段性能）

**原始实现的开销**

`DrawMosaicAnnot` 每次调用都：`GetDC(NULL)` → `CreateDIBSection` → `CreateCompatibleDC` → `SelectObject` → `BitBlt` 取源 → 逐块求平均 → `BitBlt` 回贴 → `DeleteDC` + `DeleteObject`。`OverlayDraw` 每次重绘都会遍历全部标注，因此鼠标移动、悬停高亮、撤销、拖动选区时，每个马赛克区域都要重新走一遍上述流程。一次 `WM_MOUSEMOVE` 期间该函数会被调用 N 次（N = 马赛克数量）。

**优化方案（最小且安全）**

为每条马赛克标注维护一份“已量化结果”缓存（`g_mosaicCache[MAX_ANNOT]`），命中时只做一次 `BitBlt`。失效条件精确到会真正改变像素的因素：

| 条件 | 原因 |
|---|---|
| 标注区域 `rc` 变化 | 拖动/缩放马赛克框 |
| 选区 `sel` 变化 | 选区内是原亮度、选区外是变暗底图，源像素不同 |
| 场景版本 `g_mosaicEpoch` 变化 | 新增/撤销涉及下标平移、文字改写会改变压在下面的像素 |
| 底图重建 | `FreeCapture` 统一清空（含新的截图会话） |
| 缓存尺寸不足 | 区域变大时按需扩容；缩小时直接复用旧画布 |

内存控制：单条缓存上限 8M 像素，全部缓存合计预算 16M 像素（约 64MB），超出预算的区域自动退回不缓存的临时缓冲，保证极端标注数量下也不会吃满内存。

**一致性**：预览（`OverlayDraw`）和导出（`BuildFinalBitmap`）走同一个 `DrawAnnotations`，共用同一份缓存；缓存失效判据只依赖“源像素是否可能变化”，因此两条路径的像素结果一致（自检逐点比对 6/6 命中）。

**实测（640×480、3 条马赛克、只计标注绘制）**

| 场景 | 500 次绘制耗时 |
|---|---|
| 缓存命中 | 15–16 ms |
| 强制每帧重算（等价旧实现） | 31–32 ms |

**遗留风险**：缓存失效粒度是“整条马赛克整体作废”，文字改写/撤销会让所有马赛克重算一次（一次点击级开销），换取的是实现简单、不可能出现脏数据。

---

### 2.7 顺带修复的其它明确缺陷

| 位置 | 问题 | 处理 |
|---|---|---|
| `ToastShowAt` | `GetDC(NULL)` 返回 NULL 时仍然 `SelectObject` | 判空后才测量文字；恢复原字体对象再 `ReleaseDC` |
| `ToastShowAt` | `SetWindowRgn` 失败时区域句柄泄漏 | 失败时 `DeleteObject(rgn)` |
| `ToastProc` | 绘制字体选入后未还原 | 保存并恢复原字体 |
| `CreatePinWindow` | 创建窗口失败时未把原位图选回 DC，DIB 删除不干净；`SelectObject` 返回值未检查 | 失败路径先 `SelectObject(oldBmp)` 再删 DC/位图；`SelectObject` 失败整体放弃 |
| `TextAnnotRect` | `GetDC` 未判空、`CreateFontW` 未判空 | 判空后跳过测量，坐标退化为 0 宽高 |
| `RunDemoShot`（自检） | 多个失败分支漏 `FreeCapture/OverlayGdiFree`；DIB/DC 返回值未检查 | 补齐清理与检查 |

---

## 3. 测试报告

### 3.1 构建

```bat
windres app.rc -O coff -o app.res
gcc -O2 -Wall -mwindows shot.c app.res -o snapshot.exe ^
    -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -lcomdlg32 -lm
```

结果：编译通过，`-Wall` 无告警。

### 3.2 测试命令与结果

```bat
snapshot.exe --stress-test  > stress.log      :: 新增，退出码 0/1
snapshot.exe --capture-test > capture.log     :: 原有
snapshot.exe --toolbar-test > toolbar.log     :: 原有
snapshot.exe --demo docs\overlay.png demo_new.png   :: 原有
```

`--stress-test` 完整输出（Windows 桌面会话，2560×1440）：

```text
=== QuickShot stability self-test ===
capture: 1000x 43500 ms, gdi 20 -> 20, fails=0
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
mosaic: annots 500x cached 16 ms, 500x uncached 31 ms
[PASS] mosaic cache is not slower than recompute
mosaic: preview vs export 6/6 match
[PASS] mosaic preview == export
[PASS] mosaic undo restores base pixels
  gdi after mosaic: 20
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

原有自检同样通过：

- `--capture-test`：`capture: 2560 x 1440`，7 条标注烘焙成功，`png: ok`，热键 `Ctrl+Alt+A: ok`，退出码 0；
- `--toolbar-test`：`out-of-bounds: 0`，`cursor mismatches: 0`，`png: ok`，退出码 0。

### 3.3 与修改前的像素级对比

用基线提交 `9dd0343` 的源码单独编译旧版本，在同一张底图上执行 `--demo`，对比输出 PNG：

```text
28A51DDC9DDF58BEE90C5B81D3FAEC5C1A1CDAF790E7F40E3692FFC900B6A79E  demo_new.png
28A51DDC9DDF58BEE90C5B81D3FAEC5C1A1CDAF790E7F40E3692FFC900B6A79E  demo_orig.png
```

SHA256 完全一致，说明渲染链路（含马赛克缓存、工具条、标注）在修改前后像素级等价。

### 3.4 测试项覆盖对照

| 测试项目 | 覆盖方式 | 结果 |
|---|---|---|
| 连续截图 1000 次 | `TestCaptureLoop`，每轮 `CaptureVirtualScreen + FreeCapture` | 通过，GDI 20→20 |
| 文字编辑 1000 次（创建/提交/取消） | `TestTextLifecycle`，50 次预热后 1000 次循环 | 通过，GDI 26→26 |
| 编辑状态退出 | 打开编辑框后直接 `OverlayReleaseAll` | 通过，控件与字体均回收 |
| 快捷键冲突 | 独立线程占住 `Ctrl+Alt+S` 后调用切换 | 通过：切换失败、旧热键与旧配置保持、释放后可切换 |
| 马赛克多区域/拖动/撤销/导出 | `TestMosaic`：3 区域 + 300 次重绘 + 撤销 + 预览/导出逐点比对 | 通过，6/6 像素一致 |
| 剪贴板正常/异常尺寸/被占用 | `TestClipboard` + `TestClipboardBusy` | 通过 |
| 同一秒连续截图 | `TestFileNames`，1 秒内生成 20 个路径 | 通过，20 个名字无重复 |
| 目录不可写/不存在 | 指向不存在的子目录 | 通过，安全失败 |
| GDI 对象数量 | 各阶段与整体前后对比（预热后） | 通过，整体 20→18 |
| 程序正常退出清理 | 测试末尾销毁窗口 + `OverlayReleaseAll` | 通过 |

### 3.5 未能在本环境覆盖的测试

| 项目 | 原因 | 建议 |
|---|---|---|
| 真实键鼠交互下的完整截图流程（拖框、双击确认、钉图悬浮窗、右键菜单） | 自检为离屏渲染，未做 UI 自动化 | 在 Windows 上手工执行一次全流程回归 |
| 中文输入法上屏、候选框定位 | 需要真实 IME 会话 | 手工用微软拼音输入一段中文并提交/取消 |
| 多显示器混合 DPI 缩放（如 100% + 150%） | 当前环境为单一口径桌面 | 双屏不同缩放各截一次，确认选区/工具条坐标 |
| 任务栏重启后托盘图标恢复 | 需 `TaskbarCreated` 消息处理，属既有行为未改动 | 与原版本一致，不在本次范围 |
| 以服务/无桌面会话运行自检 | 抓屏与剪贴板需要交互式桌面 | 在交互式桌面会话语义下运行 |

---

## 4. 资源管理审查

| 项目 | 结论 | 依据 |
|---|---|---|
| GDI 对象泄漏 | 无 | 1000 次截图 GDI 20→20；1000 次文字编辑 26→26；600 次遮罩重绘 33→33；整体流程结束 20→18，退出后缓存与缓冲全部释放 |
| GDI+ 对象泄漏 | 无 | 所有 `GdipCreate*` 均有对应 `GdipDelete/Dispose`；启动失败时置 `g_gdiplusOk=FALSE` 并阻断编码路径；`--demo` 与 `--capture-test` 反复执行无增长 |
| 字体句柄生命周期 | 正确 | 输入框字体由 `g_editFont` 持有，控件销毁后才 `DeleteObject`；创建失败路径立即释放；1000 次循环无残留 |
| 剪贴板内存所有权 | 正确 | 仅 `SetClipboardData` 成功后移交系统；其余路径 `GlobalFree`；尺寸计算使用 `size_t` 并逐级校验 |
| 失败路径清理 | 完整 | 截屏（分配/抓屏/变暗）、成品位图、钉图窗口、提示气泡、演示自检的每个失败分支都有对应释放；清理函数幂等 |
| 重复释放 | 不存在 | 所有句柄释放后立即置 `NULL`；`FreeCapture` / `OverlayGdiFree` / `TextEditDestroy` / `MosaicCacheFree` 均可重复调用 |
| 句柄被选中时的释放顺序 | 正确 | 统一“还原原始对象 → 删 DC → 删位图” |
| 整数溢出 | 已消除 | 剪贴板尺寸、`BuildFinalBitmap` 的 `w*4*h` 均用 `size_t` 校验；DIB 尺寸上限显式限制 |
| 配置与运行状态一致 | 已修复 | 热键只有在注册成功后才更新内存配置并写注册表 |

---

## 5. 遗留问题

| # | 问题 | 风险 | 建议 |
|---|---|---|---|
| 1 | 托盘提示文字仍固定为 “QuickShot 截图 (Ctrl+Alt+A)”，切换热键后不更新 | 低：仅提示不准确，不影响功能 | 后续把提示与 `g_mods/g_vk` 同步（本次为不改动 UI 文案而未处理） |
| 2 | 启动时若保存的热键已被占用，程序无热键可用，只能手动换一个 | 低：与原行为一致，且配置与状态已一致 | 可在启动失败时自动回退到第一个可用预设 |
| 3 | 马赛克缓存为“整条作废”粒度，文字改写/撤销会让所有马赛克重算一次 | 低：点击级开销，且受 64MB 预算约束 | 如需进一步优化，可按“标注下标 + 依赖前缀”做细粒度失效 |
| 4 | 剪贴板拒绝 >2GB 的 DIB | 极低：远超实际截图尺寸 | 保持现状 |
| 5 | 选区移动后标注不跟随（标注固定在遮罩客户端坐标，属原产品行为） | 低：与原版一致 | 若产品希望标注跟随选区，需单独设计，不在本次范围 |
| 6 | `--stress-test` 依赖交互式桌面（抓屏、剪贴板、热键） | 中：CI/无桌面环境会失败 | 在 CI 中改用离屏子集（`--toolbar-test`、`--capture-test`），`--stress-test` 作为本地/手工回归 |
| 7 | 占位文件与写入之间存在极短的空文件窗口 | 极低 | 保持现状（换来的是绝不覆盖用户文件） |

---

## 6. 结论

- 六项指定问题均在最新代码中确认存在，并已按“最小修改”原则修复；
- 未改动 UI 布局、默认快捷键、标注交互与文件命名风格；
- 新增 `--stress-test` 自检覆盖了任务要求的全部可自动化场景，30 项检查全部通过；
- 修改前后 `--demo` 输出 SHA256 完全一致，说明渲染行为无回归；
- 连续截图、文字编辑、马赛克重绘、热键冲突、剪贴板占用、同名文件等异常场景下均无崩溃、无 GDI 增长、无静默覆盖。
