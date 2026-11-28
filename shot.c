/*
 * QuickShot — 极简常驻截图工具 (纯 Win32 / C)
 *
 * 特性：
 *   - 全局热键（默认 Ctrl+Alt+A，可在托盘菜单切换）
 *   - 按下热键冻结整屏，拖框选择区域
 *   - 松手即复制到剪贴板（可选同时存 PNG）
 *   - 托盘常驻，空闲内存 ~2 MB，CPU 0%
 *
 * 编译（MinGW-w64）：
 *   windres app.rc -O coff -o app.res
 *   gcc -O2 -s -mwindows shot.c app.res -o snapshot.exe \
 *       -lgdiplus -lgdi32 -luser32 -lshell32
 */

#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define APP_NAME   L"QuickShot"
#define APP_TITLE  L"QuickShot  截图 (Ctrl+Alt+A)"

#define HOTKEY_ID        0xA001
#define HOTKEY_ID_ALT    0xA002   /* 切换热键时的备用 ID：先注册新的，成功后再注销旧的 */
#define WM_TRAY          (WM_APP + 1)
#define WM_OV_CLOSE      (WM_APP + 2)
#define TRAY_UID         1

#define MENU_SHOT        1
#define MENU_SAVEFILE    2
#define MENU_AUTOSTART   3
#define MENU_OPENDIR     4
#define MENU_EXIT        5
#define MENU_HK_BASE     100        /* 100..103 预设热键 */

#define REG_PATH         L"Software\\QuickShot"
#define RUN_PATH         L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define OVERLAY_CLASS    L"QS_Overlay"
#define TOAST_CLASS      L"QS_Toast"
#define PIN_CLASS        L"QS_Pin"

/* ---------------------------------------------------------------- 全局状态 */

static HINSTANCE  g_hInst;
static HWND       g_hMsgWnd;          /* 隐藏的消息窗口（托盘 + 热键） */
static NOTIFYICONDATAW g_nid;
static ULONG_PTR  g_gdiToken;
static BOOL       g_gdiplusOk = FALSE;   /* GDI+ 初始化是否成功，失败时禁止走导出/编码路径 */
static BOOL       g_saveFile = TRUE;
static BOOL       g_hotkeyOk = FALSE;
static UINT       g_hotkeyId = 0;        /* 当前实际注册的热键 ID，0 = 未注册 */
static UINT       g_mods = MOD_CONTROL | MOD_ALT;
static UINT       g_vk   = 'A';
static WCHAR      g_saveDir[MAX_PATH];

static HFONT      g_fontLabel;
static HFONT      g_fontToast;
static HWND       g_toast;
static WCHAR      g_toastText[256];

/* 热键预设 */
typedef struct { UINT mods; UINT vk; const WCHAR *label; } HotkeyPreset;
static const HotkeyPreset g_presets[] = {
    { MOD_CONTROL | MOD_ALT,   'A', L"Ctrl + Alt + A" },
    { MOD_CONTROL | MOD_ALT,   'S', L"Ctrl + Alt + S" },
    { MOD_CONTROL | MOD_SHIFT, 'A', L"Ctrl + Shift + A" },
    { MOD_ALT  | MOD_SHIFT,    'A', L"Alt + Shift + A" },
};
#define PRESET_COUNT ((int)(sizeof(g_presets) / sizeof(g_presets[0])))

/* 选区交互状态 */
enum {
    ST_IDLE = 0,   /* 还没拉框 */
    ST_NEW,        /* 正在拉新框 */
    ST_SELECTED,   /* 框已存在，可移动/缩放/确认 */
    ST_MOVE,       /* 正在拖动整块选区 */
    ST_RESIZE,     /* 正在拖手柄缩放 */
    ST_DRAW,       /* 正在画标注 */
    ST_DONE        /* 已提交，等窗口关闭 */
};

/* 手柄编号：0 左上 1 上 2 右上 3 右 4 右下 5 下 6 左下 7 左 */
enum { H_TL = 0, H_T, H_TR, H_R, H_BR, H_B, H_BL, H_L, H_COUNT };
#define HANDLE_HIT 8       /* 手柄命中半径（像素） */
#define TB_BTN     30      /* 第一行按钮宽 */
#define TB_NBTN    11      /* 选择 画笔 矩形 椭圆 箭头 马赛克 文字 序号 撤销 ✗ ✓ */
#define TB_ROW1    34      /* 第一行高（工具按钮） */
#define TB_ROW2    26      /* 第二行高（颜色 / 粗细 / 钉图） */
#define TB_H       (TB_ROW1 + TB_ROW2)
#define TB_W       358     /* 工具条总宽 */

/* 截图状态 */
typedef struct {
    HWND    hwnd;
    HBITMAP hbmCap, hbmDim, hbmBack;
    HDC     hdcCap, hdcDim, hdcBack;
    /* SelectObject 前 DC 里原来的位图，DeleteDC 之前必须选回去，否则位图删不掉 */
    HGDIOBJ oldBmpCap, oldBmpDim, oldBmpBack;
    void   *pixCap;        /* 32bpp top-down 原始像素 */
    int     vx, vy, vw, vh;

    int     state;
    BOOL    hasSel;
    RECT    sel;           /* 当前选区（客户端坐标，已归一化） */
    RECT    selAtDown;     /* 按下瞬间的选区，用于换算移动/缩放 */
    POINT   ptDown;        /* 按下点 */
    int     handle;        /* 正在拖的手柄编号 */
    int     hotBtn;        /* 鼠标悬停/按下的第一行按钮 */
    int     hotSwatch;     /* 鼠标悬停的色块 */
    int     hotSize;       /* 鼠标悬停的粗细档位 */
    BOOL    hotPin;        /* 鼠标悬停在钉住按钮上 */
} OverlayState;

static OverlayState g_ov;

/* 遮罩绘制用的 GDI 对象，创建一次反复用，避免每帧新建 */
static HBRUSH g_brHandle, g_brHandleEdge, g_brLabelBg, g_brTbBg, g_brTbBorder, g_brTbHover, g_brTbActive;
static HPEN   g_penSel, g_penOuter, g_penSep, g_penOk, g_penNo;

/* ---------------------------------------------------------------- 标注 */

enum { TOOL_MOVE = 0, TOOL_PEN, TOOL_RECT, TOOL_ELLIPSE, TOOL_ARROW, TOOL_MOSAIC, TOOL_TEXT, TOOL_NUMBER };

enum { AN_PEN = 0, AN_RECT, AN_ELLIPSE, AN_ARROW, AN_MOSAIC, AN_TEXT, AN_NUMBER };

#define MAX_ANNOT   32
#define MAX_PEN_PTS 1024
#define PEN_W       3
#define TEXT_H      20

typedef struct {
    int      kind;
    COLORREF color;
    int      width;
    POINT    pts[MAX_PEN_PTS];   /* 画笔轨迹 */
    int      npts;
    RECT     rc;                 /* 椭圆外框 */
    WCHAR    text[256];          /* 文字内容 */
    int      x, y, fontH;        /* 文字锚点 */
} Annot;

static Annot   g_ann[MAX_ANNOT];
static int     g_annCount;
static int     g_tool = TOOL_MOVE;
static int     g_editX, g_editY;

#define ANN_COLOR RGB(235, 45, 45)

/* 标注调色板 */
static COLORREF g_annColor = RGB(235, 45, 45);
static const COLORREF g_palette[] = {
    RGB(235,  45,  45),   /* 红 */
    RGB(247, 178,  34),   /* 黄 */
    RGB( 46, 184,  92),   /* 绿 */
    RGB( 38, 132, 255),   /* 蓝 */
    RGB(255, 255, 255),   /* 白 */
};
#define PALETTE_N     ((int)(sizeof(g_palette) / sizeof(g_palette[0])))
#define SWATCH_SIZE   18
#define SWATCH_GAP    3
#define MOSAIC_BLOCK  10

/* 粗细 / 字号档位：0 细 1 中 2 粗 */
static int g_sizeLevel = 1;
static const int g_penW[3]  = { 2, 3, 5 };
static const int g_textH[3] = { 16, 20, 28 };
static const int g_numD[3]  = { 22, 28, 36 };

static int g_numberNext = 1;        /* 下一个序号标注的编号 */
static int g_editEditing = -1;      /* 正在二次编辑的文字标注下标，-1 = 新建 */

static HWND    g_edit;           /* 文字输入框 */
static WNDPROC g_editOldProc;
static HFONT   g_editFont;       /* 输入框字体：WM_SETFONT 只存句柄不复制，控件销毁后才能释放 */
static HFONT   g_fontText, g_fontIcon;

#define WM_EDIT_COMMIT (WM_APP + 11)
#define WM_EDIT_CANCEL (WM_APP + 12)
#define ID_EDIT        3001

/* ---------------------------------------------------------------- 小工具 */

/* 诊断日志：仅在设置了环境变量 QS_LOG 时写入 exe 同目录 quickshot.log */
static FILE *g_logf;

static void QSLog(const char *fmt, ...)
{
    if (!g_logf) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
}

/* 高精度计时（微秒）：自检里的性能对比不能依赖 15.6ms 粒度的 GetTickCount64 */
static ULONGLONG NowUsec(void)
{
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0) return 0;
    QueryPerformanceCounter(&c);
    return (ULONGLONG)((double)c.QuadPart * 1000000.0 / (double)f.QuadPart);
}

static void GetExePath(WCHAR *buf, DWORD n)
{
    GetModuleFileNameW(NULL, buf, n);
}

/* 把 "\a\b\c.exe" 换成 "\a\b\out.png" */
static void MakeSiblingPath(const WCHAR *outName, WCHAR *out, DWORD n)
{
    WCHAR exe[MAX_PATH];
    GetExePath(exe, MAX_PATH);
    WCHAR *slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    lstrcpynW(out, exe, n);
    lstrcatW(out, outName);
}

static void EnableDpiAwareness(void)
{
    typedef BOOL (WINAPI *FnDpiCtx)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    FnDpiCtx f = u ? (FnDpiCtx)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (f && f((HANDLE)-4)) return;          /* PER_MONITOR_AWARE_V2 */
    SetProcessDPIAware();
}

static BOOL GetEncoderClsid(const WCHAR *mime, CLSID *clsid)
{
    UINT num = 0, size = 0;
    if (GdipGetImageEncodersSize(&num, &size) != 0 || size == 0) return FALSE;
    ImageCodecInfo *info = (ImageCodecInfo *)malloc(size);
    if (!info) return FALSE;
    BOOL ok = FALSE;
    if (GdipGetImageEncoders(num, size, info) == 0) {
        for (UINT i = 0; i < num; i++) {
            if (wcscmp(info[i].MimeType, mime) == 0) {
                *clsid = info[i].Clsid;
                ok = TRUE;
                break;
            }
        }
    }
    free(info);
    return ok;
}

/* ---------------------------------------------------------------- GDI 对象生命周期 */

/* 统一的选取/恢复入口，避免每个绘制函数各写一套容易出错的清理代码。
 *
 * 生命周期约定：
 *   1. 临时对象创建成功后，用 GdiSelect 选入 DC，同时拿到原对象；
 *   2. 绘制只发生在 GdiSelect 成功之后；
 *   3. 结束后用 GdiRelease 一步完成“恢复原对象 + 删除临时对象”，
 *      恢复失败时先用同类 stock 对象把临时对象顶出 DC，保证 DeleteObject 不会静默失败。
 * 位图/DC 这类没有 stock 替代对象的资源，选入时用 GdiSelect、销毁前用 GdiRestore 恢复，
 * 因为 DeleteDC 本身会解除位图引用，之后再 DeleteObject 一定成功。
 */

/* 供自检使用的故障注入：>0 时让下一次对应调用直接失败（不改动任何 DC 状态），
 * 用来覆盖各类失败分支。正常运行时恒为 0。 */
static int g_gdiFailInject        = 0;   /* GdiSelect 失败 */
static int g_gdiRestoreFailInject = 0;   /* GdiRestore 失败 */
static int g_gdiDeleteFailInject  = 0;   /* GdiRelease 中 DeleteObject 失败 */
static int g_gdiHeldFailInject    = 0;   /* 托管链表节点分配失败（模拟 OOM） */

/* 失败计数：正常渲染路径必须一直为 0，自检会断言这一点。
 * 绘图函数无法“撤销”已经画出去的像素，所以失败时以「日志 + 计数」升级，
 * 让异常真实可见，而不是被静默忽略。 */
static int g_gdiSelectFailures;
static int g_gdiRestoreFailures;
static int g_gdiReleaseFailures;

/* ---------------------------------------------------------------- 临时 GDI 对象托管
 *
 * 固定容量数组无法真正闭环：只要存在上限，第 N+1 个“删不掉”的对象就没有持有者。
 * 因此不再分级扩容，改为单一动态托管链表：
 *
 *   - 节点在登记时按需 malloc，容量只受进程堆限制，不存在“第 77 个”这样的硬上限；
 *   - 节点本身就是所有权凭据：只有 DeleteObject 成功（或对象已不存在）才摘链并 free；
 *   - 链表是进程级唯一持有结构，遮罩会话结束、DC 销毁、进程退出都会重试，
 *     未释放条目继续留在链表中，退出时逐条报告，不会被静默丢弃；
 *   - 不主动销毁任何 DC，只在对象已不被 DC 引用时重试删除。
 *
 * 绘制路径的所有权交接：
 *   - GdiRelease() 删除失败时调用 GdiHeldTake() 转交所有权，成功返回 GdiRelHeld；
 *   - 仅当节点分配失败（OOM）时返回 GdiRelUnreleased；
 *   - 绘制函数把 Unreleased 句柄写入调用方提供的 GdiPaintBatch（单次绘制最多 2 个
 *     临时对象：笔+刷 / 刷+字体），DrawAnnotations 在每个标注结束后立即
 *     GdiBatchDrain() 重试删除/托管；帧末剩余转入遮罩会话兜底批次，
 *     随 FreeCapture/OverlayReleaseAll/进程退出再次重试。
 *
 * 所有权契约：
 *   - GdiRelFreed / GdiRelHeld → 句柄已不由调用方负责；
 *   - GdiRelUnreleased → 句柄仍归调用方，必须写入 GdiPaintBatch 或继续持有；
 *   - 托管链表绝不假装“已释放”，未删掉的条目始终留在链表中可见。
 */
typedef enum {
    GdiRelFreed = 0,   /* 已确认删除（或对象本来就不存在） */
    GdiRelHeld,        /* 已转交托管链表，调用方不再负责 */
    GdiRelUnreleased   /* 托管节点分配失败，所有权仍在调用方 */
} GdiReleaseStatus;

typedef struct GdiHeldNode {
    HGDIOBJ obj;
    struct GdiHeldNode *next;
} GdiHeldNode;

static GdiHeldNode *g_gdiHeldHead;     /* 唯一持有结构：动态链表 */
static int g_gdiHeldCount;             /* 当前链表中等待释放的对象数 */
static int g_gdiHeldDone;              /* 已确认释放并摘链的对象总数 */
static int g_gdiHeldAllocFailures;     /* 节点分配失败、所有权退回调用方的次数 */

/* 单次绘制的句柄交接上下文。容量等于一次绘制可能创建的临时对象数上限
 * （箭头=笔+刷，序号=刷+字体，其余=1），不是托管层级。 */
#define GDI_BATCH_SLOTS 2
typedef struct {
    HGDIOBJ obj[GDI_BATCH_SLOTS];
    int count;
} GdiPaintBatch;

/* 遮罩会话级兜底批次：帧末仍未能交出的句柄先留在这里，
 * 生命周期与截屏会话一致，FreeCapture/OverlayReleaseAll 时再次 drain。 */
static GdiPaintBatch g_gdiSessionBatch;

static int g_gdiDeferLostHandles;    /* 删除与托管都失败、所有权退回调用方的次数 */

/* 统一的“尝试删除”入口：自检可注入失败，正常路径等价于 DeleteObject。
 * 队列/会话/驱逐/GdiRelease 全部走这里，保证注入时行为一致。 */
static BOOL GdiTryDelete(HGDIOBJ obj)
{
    if (!obj) return TRUE;
    if (g_gdiDeleteFailInject > 0) {
        g_gdiDeleteFailInject--;
        return FALSE;
    }
    return DeleteObject(obj) ? TRUE : FALSE;
}

static BOOL GdiHeldContains(HGDIOBJ obj)
{
    for (GdiHeldNode *n = g_gdiHeldHead; n; n = n->next)
        if (n->obj == obj) return TRUE;
    return FALSE;
}

/* 接管所有权：节点按需分配，容量只受进程堆限制。
 * 分配失败时返回 FALSE，句柄仍归调用方（绝不假装成功）。 */
static BOOL GdiHeldTake(HGDIOBJ obj)
{
    if (!obj) return TRUE;

    if (GdiHeldContains(obj)) {         /* 去重：同一句柄只登记一次 */
        QSLog("GdiHeldTake: obj=%p 已在托管链表，忽略重复登记", (void *)obj);
        return TRUE;
    }

    if (g_gdiHeldFailInject > 0) {      /* 自检注入：模拟节点分配失败 */
        g_gdiHeldFailInject--;
        g_gdiHeldAllocFailures++;
        QSLog("GdiHeldTake: 注入分配失败 obj=%p（所有权仍归调用方）", (void *)obj);
        return FALSE;
    }

    GdiHeldNode *n = (GdiHeldNode *)malloc(sizeof(*n));
    if (!n) {
        g_gdiHeldAllocFailures++;
        QSLog("GdiHeldTake: ERROR 节点分配失败 obj=%p（所有权仍归调用方）", (void *)obj);
        return FALSE;
    }

    n->obj  = obj;
    n->next = g_gdiHeldHead;
    g_gdiHeldHead = n;
    g_gdiHeldCount++;
    return TRUE;
}

static void GdiBatchInit(GdiPaintBatch *b)
{
    if (!b) return;
    for (int i = 0; i < GDI_BATCH_SLOTS; i++) b->obj[i] = NULL;
    b->count = 0;
}

/* 记录一个“删除失败且未能转交托管”的句柄。容量是绘制操作的结构上限，
 * 正常流程（含 76+ 对象被 DC 锁住）都不会走到这里，只有 OOM 才会。 */
static void GdiBatchAdd(GdiPaintBatch *b, HGDIOBJ obj)
{
    if (!b || !obj) return;

    if (b->count >= GDI_BATCH_SLOTS) {
        g_gdiDeferLostHandles++;
        QSLog("GdiBatchAdd: ERROR 批次已满，obj=%p 无可靠持有者", (void *)obj);
        return;
    }
    b->obj[b->count++] = obj;
}

/* 依次尝试删除/托管批次里的句柄；成功的移出批次，失败的留下。返回剩余数。 */
static int GdiBatchDrain(GdiPaintBatch *b)
{
    if (!b) return 0;

    int keep = 0;
    for (int i = 0; i < b->count; i++) {
        HGDIOBJ o = b->obj[i];
        if (!o) continue;
        if (GdiTryDelete(o) || GdiHeldTake(o)) continue;
        b->obj[keep++] = o;
    }
    for (int i = keep; i < b->count; i++) b->obj[i] = NULL;
    b->count = keep;
    return keep;
}

/* 帧末把局部批次剩余句柄转入遮罩会话兜底批次。
 * 先 drain 兜底批次腾位置；仍放不下只可能是持续 OOM，如实计数并记录。 */
static void GdiBatchAdopt(GdiPaintBatch *src)
{
    if (!src || src->count == 0) return;
    if (g_gdiSessionBatch.count > 0) GdiBatchDrain(&g_gdiSessionBatch);

    for (int i = 0; i < src->count; i++) {
        HGDIOBJ o = src->obj[i];
        if (!o) continue;
        if (GdiTryDelete(o) || GdiHeldTake(o)) continue;
        if (g_gdiSessionBatch.count < GDI_BATCH_SLOTS) {
            g_gdiSessionBatch.obj[g_gdiSessionBatch.count++] = o;
        } else {
            g_gdiDeferLostHandles++;
            QSLog("GdiBatchAdopt: ERROR 会话批次已满，obj=%p 无可靠持有者", (void *)o);
        }
    }
    GdiBatchInit(src);
}

/* 重试释放全部托管对象：遮罩会话兜底批次 + 进程级托管链表。
 * 不主动销毁任何 DC：只对“已不再被任何 DC 引用”的条目重试 DeleteObject。
 * 删不掉的条目留在链表中，等待下一次 DC 销毁/会话结束/进程退出重试。 */
static void GdiFlushDeferred(void)
{
    GdiBatchDrain(&g_gdiSessionBatch);

    GdiHeldNode **pp = &g_gdiHeldHead;
    while (*pp) {
        GdiHeldNode *n = *pp;
        BOOL gone = (!n->obj || GetObjectType(n->obj) == 0);

        if (!gone && GdiTryDelete(n->obj)) {
            gone = TRUE;
            g_gdiHeldDone++;
            QSLog("GdiFlushDeferred: 托管对象 obj=%p 已释放", (void *)n->obj);
        }

        if (gone) {
            *pp = n->next;
            n->obj = NULL;
            free(n);
            g_gdiHeldCount--;
        } else {
            pp = &n->next;
        }
    }
}

/* 进程退出时调用：业务窗口与 DC 都已销毁，做最后一次重试，然后如实报告。
 * 不把“仍在等待”的对象当成已释放，也不在报告前把它们从链表里抹掉。 */
static void GdiDeferReleaseAll(void)
{
    GdiFlushDeferred();

    if (g_gdiHeldCount > 0) {
        QSLog("GdiDeferReleaseAll: 退出时仍有 %d 个对象等待释放", g_gdiHeldCount);
        for (GdiHeldNode *n = g_gdiHeldHead; n; n = n->next)
            QSLog("  仍在等待: obj=%p type=%lu",
                  (void *)n->obj, (unsigned long)GetObjectType(n->obj));
    }
    if (g_gdiSessionBatch.count > 0) {
        QSLog("GdiDeferReleaseAll: 退出时遮罩会话批次仍有 %d 个句柄", g_gdiSessionBatch.count);
        for (int i = 0; i < g_gdiSessionBatch.count; i++)
            QSLog("  仍在等待: obj=%p type=%lu (batch)",
                  (void *)g_gdiSessionBatch.obj[i],
                  (unsigned long)GetObjectType(g_gdiSessionBatch.obj[i]));
    }
    QSLog("GdiDeferReleaseAll: freed=%d, held=%d, allocFailures=%d, unreleased=%d",
          g_gdiHeldDone, g_gdiHeldCount, g_gdiHeldAllocFailures, g_gdiDeferLostHandles);
}

static BOOL GdiSelect(HDC hdc, HGDIOBJ obj, HGDIOBJ *old)
{
    if (!old) {                             /* 输出指针必须有效，否则无法记录原对象 */
        QSLog("GdiSelect: NULL old pointer");
        g_gdiSelectFailures++;
        return FALSE;
    }
    *old = NULL;                            /* 先初始化，失败时调用方拿到的一定是 NULL */

    if (!hdc || !obj) {
        QSLog("GdiSelect: 无效参数 hdc=%p obj=%p", (void *)hdc, (void *)obj);
        g_gdiSelectFailures++;
        return FALSE;
    }

    if (g_gdiFailInject > 0) {              /* 自检注入：只报失败，不碰 DC */
        g_gdiFailInject--;
        QSLog("GdiSelect: 注入失败 hdc=%p obj=%p", (void *)hdc, (void *)obj);
        g_gdiSelectFailures++;
        return FALSE;
    }

    HGDIOBJ o = SelectObject(hdc, obj);
    if (!o || o == (HGDIOBJ)HGDI_ERROR) {
        QSLog("GdiSelect: SelectObject FAIL hdc=%p obj=%p err=%lu",
              (void *)hdc, (void *)obj, GetLastError());
        g_gdiSelectFailures++;
        return FALSE;
    }
    *old = o;
    return TRUE;
}

/* 把选入前的原对象换回去。失败会写日志并返回 FALSE：
 * 此时 DC 里仍然是临时对象，调用方不能直接删它（用 GdiRelease 处理这种场景）。 */
static BOOL GdiRestore(HDC hdc, HGDIOBJ old, const char *ctx)
{
    if (!hdc || !old || old == (HGDIOBJ)HGDI_ERROR) {
        QSLog("GdiRestore(%s): 无效参数 hdc=%p old=%p", ctx, (void *)hdc, (void *)old);
        g_gdiRestoreFailures++;
        return FALSE;
    }

    if (g_gdiRestoreFailInject > 0) {       /* 自检注入：只报失败，不碰 DC */
        g_gdiRestoreFailInject--;
        QSLog("GdiRestore(%s): 注入失败 hdc=%p old=%p", ctx, (void *)hdc, (void *)old);
        g_gdiRestoreFailures++;
        return FALSE;
    }

    HGDIOBJ o = SelectObject(hdc, old);
    if (!o || o == (HGDIOBJ)HGDI_ERROR) {
        QSLog("GdiRestore(%s): FAIL err=%lu（DC 里可能仍是临时对象）", ctx, GetLastError());
        g_gdiRestoreFailures++;
        return FALSE;
    }
    return TRUE;
}

/* 临时对象的标准释放流程。顺序是有意这样排的：
 *   1. 先恢复原对象；
 *   2. 恢复失败说明临时对象可能仍被 DC 引用，先用同类 stock 对象把它顶出 DC
 *      （stockFallback=-1 表示无替代，如 HBITMAP）；
 *   3. 删除临时对象并检查结果；
 *   4. 删不掉时转交动态托管链表（容量只受进程堆限制）；
 *   5. 只有节点分配失败（OOM）才返回 GdiRelUnreleased，把所有权明确交还调用方。
 *
 * 返回值表示句柄的最终归属，不能只用一个 BOOL 概括：
 *   GdiRelFreed          已删除，调用方无需再管；
 *   GdiRelHeld           托管链表已接管，调用方无需再管；
 *   GdiRelUnreleased     未释放也未托管，调用方必须写入 GdiPaintBatch 或继续持有。 */
static GdiReleaseStatus GdiRelease(HDC hdc, HGDIOBJ obj, HGDIOBJ old,
                                   int stockFallback, const char *ctx)
{
    GdiFlushDeferred();                     /* 顺手清理之前排队、现在已能释放的对象 */

    /* old == NULL 表示 obj 从未成功选入 DC（GdiSelect 失败），没有需要恢复的原对象，
     * 直接进入删除/托管流程，避免把“无需恢复”误计为恢复失败。 */
    BOOL restored = old ? GdiRestore(hdc, old, ctx) : TRUE;

    if (!restored && hdc && obj && stockFallback >= 0) {
        HGDIOBJ s = GetStockObject(stockFallback);
        HGDIOBJ r = s ? SelectObject(hdc, s) : NULL;
        if (r && r != (HGDIOBJ)HGDI_ERROR)
            QSLog("GdiRelease(%s): 已用 stock 顶出 obj=%p（原对象 old=%p 仍未恢复）",
                  ctx, (void *)obj, (void *)old);
        else
            QSLog("GdiRelease(%s): stock 顶出失败 obj=%p（对象仍被 DC 引用）",
                  ctx, (void *)obj);
    }

    GdiReleaseStatus st = GdiRelFreed;

    if (obj) {
        if (GdiTryDelete(obj)) {
            st = GdiRelFreed;
        } else {
            QSLog("GdiRelease(%s): 删除失败 obj=%p（仍被 DC 引用）", ctx, (void *)obj);

            if (GdiHeldTake(obj)) {
                st = GdiRelHeld;
                QSLog("GdiRelease(%s): obj=%p 已转交托管链表（当前 %d 个）",
                      ctx, (void *)obj, g_gdiHeldCount);
            } else {
                /* 节点分配失败：所有权明确留在调用方，绝不假装成功。
                 * 绘制路径必须把句柄写入 GdiPaintBatch，其它调用方自行持有。 */
                st = GdiRelUnreleased;
                g_gdiDeferLostHandles++;
                QSLog("GdiRelease(%s): ERROR obj=%p 未释放也未托管（节点分配失败）",
                      ctx, (void *)obj);
            }
        }
    }

    if (!restored || st == GdiRelUnreleased) g_gdiReleaseFailures++;
    return st;
}

static int g_gdiUnreleasedInPaint;   /* 绘制路径遇到 GdiRelUnreleased 的次数，正常应为 0 */

/* 绘制路径的释放封装：返回状态，由绘制函数决定是否把句柄交给调用方批次。 */
static GdiReleaseStatus GdiReleaseInPaint(HDC hdc, HGDIOBJ obj, HGDIOBJ old,
                                          int stock, const char *ctx)
{
    GdiReleaseStatus st = GdiRelease(hdc, obj, old, stock, ctx);
    if (st == GdiRelUnreleased) {
        g_gdiUnreleasedInPaint++;
        QSLog("GdiReleaseInPaint(%s): ERROR obj=%p 未释放也未托管", ctx, (void *)obj);
    }
    return st;
}

/* 绘制路径专用：释放失败且未托管时，把句柄写进调用方的批次，
 * 让上层（DrawAnnotations → 遮罩会话）稍后继续重试，而不是丢掉局部变量。 */
static GdiReleaseStatus GdiPaintRelease(HDC hdc, HGDIOBJ obj, HGDIOBJ old,
                                        int stock, const char *ctx,
                                        GdiPaintBatch *batch)
{
    GdiReleaseStatus st = GdiReleaseInPaint(hdc, obj, old, stock, ctx);
    if (st == GdiRelUnreleased) GdiBatchAdd(batch, obj);
    return st;
}

/* 对象从未成功选入 DC（GdiSelect 失败）时也走统一出口：
 * 删除失败时同样转交托管，必要时写入调用方批次。 */
static GdiReleaseStatus GdiPaintDiscard(HGDIOBJ obj, const char *ctx,
                                        GdiPaintBatch *batch)
{
    GdiReleaseStatus st = GdiReleaseInPaint(NULL, obj, NULL, -1, ctx);
    if (st == GdiRelUnreleased) GdiBatchAdd(batch, obj);
    return st;
}

/* ---------------------------------------------------------------- 配置 */

static void LoadSettings(void)
{
    HKEY k;
    DWORD v, sz, type;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        sz = sizeof(v); type = 0;
        if (RegQueryValueExW(k, L"SaveFile", NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS)
            g_saveFile = (v != 0);
        sz = sizeof(v); type = 0;
        if (RegQueryValueExW(k, L"HotMods", NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS)
            g_mods = v;
        sz = sizeof(v); type = 0;
        if (RegQueryValueExW(k, L"HotVk", NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS)
            g_vk = v;
        RegCloseKey(k);
    }
}

static void SaveSettingDword(const WCHAR *name, DWORD val)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
        RegCloseKey(k);
    }
}

static BOOL IsAutoStart(void)
{
    HKEY k;
    DWORD sz = 0, type = 0;
    BOOL r = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_PATH, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        r = (RegQueryValueExW(k, APP_NAME, NULL, &type, NULL, &sz) == ERROR_SUCCESS);
        RegCloseKey(k);
    }
    return r;
}

static void SetAutoStart(BOOL on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_PATH, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR exe[MAX_PATH + 4];
        GetExePath(exe, MAX_PATH);
        WCHAR quoted[MAX_PATH + 8];
        wsprintfW(quoted, L"\"%s\"", exe);
        RegSetValueExW(k, APP_NAME, 0, REG_SZ,
                       (const BYTE *)quoted,
                       (DWORD)((lstrlenW(quoted) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

/* 保存目录：优先 图片\Screenshots，失败退到桌面 */
static void ResolveSaveDir(void)
{
    WCHAR base[MAX_PATH] = {0};
    BOOL got = FALSE;

    if (SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, base) == S_OK && base[0])
        got = TRUE;
    if (!got) {
        if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, base) == S_OK && base[0])
            got = TRUE;
    }
    if (!got) lstrcpynW(base, L"C:\\", MAX_PATH);

    lstrcpynW(g_saveDir, base, MAX_PATH);
    lstrcatW(g_saveDir, L"\\Screenshots");
    if (!CreateDirectoryW(g_saveDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        /* 建不出来就退到原目录 */
        lstrcpynW(g_saveDir, base, MAX_PATH);
    }
}

/* ---------------------------------------------------------------- 捕获 */

static void FreeCapture(void);
static void MosaicCacheFreeAll(void);

/* 从 DC 上摘下我们自己选进去的位图。只有 SelectObject 成功过的才需要还原；
 * 就算还原失败，后面的 DeleteDC 也会解除位图引用，DeleteObject 仍能成功。 */
static void RestoreOldBmp(HDC dc, HGDIOBJ *old)
{
    if (dc && *old) GdiRestore(dc, *old, "capture bitmap");
    *old = NULL;
}

/* 分配三份缓冲：原始 / 变暗底图 / 绘制后备。
 * 任何一步失败都直接走 FreeCapture 收干净，不留下半套资源。 */
static BOOL OvAllocBuffers(int w, int h)
{
    BOOL ok = FALSE;
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return FALSE;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;            /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    g_ov.hbmCap  = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &g_ov.pixCap, NULL, 0);
    if (!g_ov.hbmCap || !g_ov.pixCap) goto done;
    g_ov.hbmDim  = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, NULL, NULL, 0);
    if (!g_ov.hbmDim) goto done;
    g_ov.hbmBack = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, NULL, NULL, 0);
    if (!g_ov.hbmBack) goto done;

    g_ov.hdcCap  = CreateCompatibleDC(hdcScreen);
    g_ov.hdcDim  = CreateCompatibleDC(hdcScreen);
    g_ov.hdcBack = CreateCompatibleDC(hdcScreen);
    if (!g_ov.hdcCap || !g_ov.hdcDim || !g_ov.hdcBack) goto done;

    g_ov.oldBmpCap  = NULL;
    g_ov.oldBmpDim  = NULL;
    g_ov.oldBmpBack = NULL;
    if (!GdiSelect(g_ov.hdcCap, g_ov.hbmCap, &g_ov.oldBmpCap)) goto done;
    if (!GdiSelect(g_ov.hdcDim, g_ov.hbmDim, &g_ov.oldBmpDim)) goto done;
    if (!GdiSelect(g_ov.hdcBack, g_ov.hbmBack, &g_ov.oldBmpBack)) goto done;

    ok = TRUE;

done:
    ReleaseDC(NULL, hdcScreen);
    if (!ok) {
        QSLog("OvAllocBuffers: FAIL %dx%d err=%lu", w, h, GetLastError());
        FreeCapture();
    }
    return ok;
}

/* 由 hdcCap 生成变暗底图（约 45% 亮度），并把原图 alpha 归位。
 * BitBlt 失败时底图内容不可信，返回 FALSE 让上层放弃这次截图。
 * 只有确认拿到合法的 DIB 像素并完整做完变暗，才返回 TRUE。 */
static BOOL OvDimBuffers(void)
{
    if (!g_ov.hdcDim || !g_ov.hdcCap || !g_ov.pixCap) return FALSE;
    if (g_ov.vw <= 0 || g_ov.vh <= 0) return FALSE;
    /* vw * vh * 4 不能溢出（size_t 下也做一次上限保护） */
    if ((size_t)g_ov.vw > (((size_t)-1) / 4u) / (size_t)g_ov.vh) return FALSE;

    if (!BitBlt(g_ov.hdcDim, 0, 0, g_ov.vw, g_ov.vh, g_ov.hdcCap, 0, 0, SRCCOPY))
        return FALSE;

    /* 取 DIB 像素指针：拿不到或尺寸对不上就说明底图不是我们建的那块，直接失败 */
    BITMAP bm;
    ZeroMemory(&bm, sizeof(bm));
    if (!GetObjectW(g_ov.hbmDim, sizeof(bm), &bm)) {
        QSLog("OvDimBuffers: GetObject FAIL err=%lu", GetLastError());
        return FALSE;
    }
    if (!bm.bmBits || bm.bmBitsPixel != 32) {
        QSLog("OvDimBuffers: bad DIB (bits=%p bpp=%u)", bm.bmBits, bm.bmBitsPixel);
        return FALSE;
    }
    if (bm.bmWidth != g_ov.vw || bm.bmHeight != g_ov.vh) {
        QSLog("OvDimBuffers: size mismatch %ldx%ld vs %dx%d",
              bm.bmWidth, bm.bmHeight, g_ov.vw, g_ov.vh);
        return FALSE;
    }
    if (bm.bmWidthBytes < (LONG)((size_t)g_ov.vw * 4u)) {
        QSLog("OvDimBuffers: stride too small %ld", bm.bmWidthBytes);
        return FALSE;
    }

    /* 按 DIB 实际行字节数遍历，不假设行宽恰好等于 vw*4 */
    BYTE *dst = (BYTE *)bm.bmBits;
    size_t stride = (size_t)bm.bmWidthBytes;
    for (int y = 0; y < g_ov.vh; y++) {
        BYTE *row = dst + (size_t)y * stride;
        for (int x = 0; x < g_ov.vw; x++) {
            BYTE *p = row + (size_t)x * 4;
            p[0] = (BYTE)(p[0] * 45 / 100);
            p[1] = (BYTE)(p[1] * 45 / 100);
            p[2] = (BYTE)(p[2] * 45 / 100);
            p[3] = 255;
        }
    }

    BYTE *cap = (BYTE *)g_ov.pixCap;
    size_t n = (size_t)g_ov.vw * g_ov.vh;
    for (size_t i = 0; i < n; i++) cap[i * 4 + 3] = 255;
    return TRUE;
}

/* 抓整屏。任一步失败都会把已经建好的资源收干净，返回 FALSE 时不留半套缓冲。 */
static BOOL CaptureVirtualScreen(void)
{
    FreeCapture();                     /* 上一次的残留先收干净，避免重复调用时泄漏 */
    ZeroMemory(&g_ov, sizeof(g_ov));

    g_ov.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_ov.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_ov.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_ov.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_ov.vw <= 0 || g_ov.vh <= 0) return FALSE;

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) return FALSE;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) { FreeCapture(); return FALSE; }

    /* 抓屏；带 CAPTUREBLT 才能抓到分层窗口，失败则退回普通模式 */
    BOOL blt = BitBlt(g_ov.hdcCap, 0, 0, g_ov.vw, g_ov.vh,
                      hdcScreen, g_ov.vx, g_ov.vy, SRCCOPY | CAPTUREBLT);
    if (!blt) {
        blt = BitBlt(g_ov.hdcCap, 0, 0, g_ov.vw, g_ov.vh,
                     hdcScreen, g_ov.vx, g_ov.vy, SRCCOPY);
    }
    ReleaseDC(NULL, hdcScreen);

    if (!blt) {
        QSLog("CaptureVirtualScreen: BitBlt FAIL err=%lu", GetLastError());
        FreeCapture();                 /* 像素不可信，别继续用 */
        return FALSE;
    }
    if (!OvDimBuffers()) {
        QSLog("CaptureVirtualScreen: dim FAIL");
        FreeCapture();
        return FALSE;
    }
    return TRUE;
}

static void FreeCapture(void)
{
    /* 先选回原位图，再删 DC，最后删位图：顺序错了位图会删不掉 */
    RestoreOldBmp(g_ov.hdcCap,  &g_ov.oldBmpCap);
    RestoreOldBmp(g_ov.hdcDim,  &g_ov.oldBmpDim);
    RestoreOldBmp(g_ov.hdcBack, &g_ov.oldBmpBack);

    if (g_ov.hdcCap)  { DeleteDC(g_ov.hdcCap);  g_ov.hdcCap = NULL; }
    if (g_ov.hdcDim)  { DeleteDC(g_ov.hdcDim);  g_ov.hdcDim = NULL; }
    if (g_ov.hdcBack) { DeleteDC(g_ov.hdcBack); g_ov.hdcBack = NULL; }
    if (g_ov.hbmCap)  { DeleteObject(g_ov.hbmCap);  g_ov.hbmCap = NULL; }
    if (g_ov.hbmDim)  { DeleteObject(g_ov.hbmDim);  g_ov.hbmDim = NULL; }
    if (g_ov.hbmBack) { DeleteObject(g_ov.hbmBack); g_ov.hbmBack = NULL; }
    g_ov.pixCap = NULL;

    /* DC 已销毁，之前因恢复失败而挂起的临时对象现在可以真正释放 */
    GdiFlushDeferred();

    /* 底图没了，马赛克缓存也没意义了 */
    MosaicCacheFreeAll();
}

/* ---------------------------------------------------------------- 读像素 / 输出 */

static GdiReleaseStatus DrawPenStroke(HDC hdc, const Annot *a, GdiPaintBatch *batch)
{
    if (a->npts < 2) return GdiRelFreed;
    HPEN pen = CreatePen(PS_SOLID, a->width, a->color);
    HGDIOBJ old = NULL;
    if (!GdiSelect(hdc, pen, &old))
        return GdiPaintDiscard(pen, "pen stroke unselected", batch);
    Polyline(hdc, a->pts, a->npts);
    return GdiPaintRelease(hdc, pen, old, NULL_PEN, "pen stroke", batch);
}

static GdiReleaseStatus DrawEllipseAnnot(HDC hdc, const Annot *a, GdiPaintBatch *batch)
{
    HPEN pen = CreatePen(PS_SOLID, a->width, a->color);
    HGDIOBJ oldPen = NULL, oldBr = NULL;
    if (!GdiSelect(hdc, pen, &oldPen))
        return GdiPaintDiscard(pen, "ellipse pen unselected", batch);

    if (GdiSelect(hdc, GetStockObject(NULL_BRUSH), &oldBr)) {
        Ellipse(hdc, a->rc.left, a->rc.top, a->rc.right, a->rc.bottom);
        GdiRestore(hdc, oldBr, "ellipse brush");
    }
    return GdiPaintRelease(hdc, pen, oldPen, NULL_PEN, "ellipse pen", batch);
}

static GdiReleaseStatus DrawRectAnnot(HDC hdc, const Annot *a, GdiPaintBatch *batch)
{
    HPEN pen = CreatePen(PS_SOLID, a->width, a->color);
    HGDIOBJ oldPen = NULL, oldBr = NULL;
    if (!GdiSelect(hdc, pen, &oldPen))
        return GdiPaintDiscard(pen, "rect pen unselected", batch);

    if (GdiSelect(hdc, GetStockObject(NULL_BRUSH), &oldBr)) {
        Rectangle(hdc, a->rc.left, a->rc.top, a->rc.right, a->rc.bottom);
        GdiRestore(hdc, oldBr, "rect brush");
    }
    return GdiPaintRelease(hdc, pen, oldPen, NULL_PEN, "rect pen", batch);
}

static GdiReleaseStatus DrawArrowAnnot(HDC hdc, const Annot *a, GdiPaintBatch *batch)
{
    if (a->npts < 2) return GdiRelFreed;

    int x1 = a->pts[0].x, y1 = a->pts[0].y;
    int x2 = a->pts[1].x, y2 = a->pts[1].y;

    double ang = atan2((double)(y2 - y1), (double)(x2 - x1));
    int hl = 9 + a->width * 3;          /* 箭头长度随线宽走 */

    POINT head[3];
    head[0].x = x2;
    head[0].y = y2;
    head[1].x = x2 - (int)(hl * cos(ang - 0.42));
    head[1].y = y2 - (int)(hl * sin(ang - 0.42));
    head[2].x = x2 - (int)(hl * cos(ang + 0.42));
    head[2].y = y2 - (int)(hl * sin(ang + 0.42));

    HPEN pen = CreatePen(PS_SOLID, a->width, a->color);
    HGDIOBJ oldPen = NULL;
    if (!GdiSelect(hdc, pen, &oldPen))
        return GdiPaintDiscard(pen, "arrow pen unselected", batch);

    /* 杆：缩短一点，别从箭头三角里穿出来 */
    MoveToEx(hdc, x1, y1, NULL);
    LineTo(hdc, (int)(x2 - hl * 0.55 * cos(ang)), (int)(y2 - hl * 0.55 * sin(ang)));

    HBRUSH br = CreateSolidBrush(a->color);
    HGDIOBJ oldBr = NULL, oldNullPen = NULL;
    GdiReleaseStatus stBr = GdiRelFreed;
    if (GdiSelect(hdc, br, &oldBr)) {
        if (GdiSelect(hdc, GetStockObject(NULL_PEN), &oldNullPen)) {
            Polygon(hdc, head, 3);
            GdiRestore(hdc, oldNullPen, "arrow null pen");
        }
        stBr = GdiPaintRelease(hdc, br, oldBr, WHITE_BRUSH, "arrow brush", batch);
    } else {
        /* 没选入 DC：仍走统一出口，删除失败时同样进入托管/批次 */
        stBr = GdiPaintDiscard(br, "arrow brush unselected", batch);
    }

    GdiReleaseStatus stPen = GdiPaintRelease(hdc, pen, oldPen, NULL_PEN, "arrow pen", batch);
    /* 笔和刷各自独立交接：任一个 Unreleased 都已写入批次，这里只做汇总 */
    return (stPen == GdiRelUnreleased || stBr == GdiRelUnreleased) ? GdiRelUnreleased
                                                                  : GdiRelFreed;
}

/* 马赛克：把目标区域切块、逐块取平均色涂平。
 * 直接读 hdc 上现有的像素，所以实时预览和烘焙成图两条路都能复用。
 *
 * 性能：每帧重绘都临时建 DIB、逐块算平均色，标注一多就会拖慢重绘和拖动。
 * 这里给每条马赛克标注留一份结果缓存，只有下面这些情况才重算：
 *   - 标注区域变了（拖动/缩放马赛克框）
 *   - 选区变了（选区内外明暗不同，取到的底图会变）
 *   - 场景版本变了（新增/撤销/文字改写后由 MosaicEpochBump 递增）
 *   - 换了底图（FreeCapture 会整体清空缓存）
 * 预览和导出共用同一份缓存，因此两条路的像素结果保持一致。
 */
#define MOSAIC_CACHE_MAX_PIXELS (8 * 1024 * 1024)   /* 单条马赛克缓存上限，超过就不缓存 */
#define MOSAIC_CACHE_BUDGET     (16 * 1024 * 1024)  /* 所有缓存合计上限（约 64MB），防止极端情况吃满内存 */

typedef struct {
    HBITMAP  hbm;
    HDC      hdc;
    HGDIOBJ  oldBmp;       /* 建 DC 时原来的位图，销毁前要选回去 */
    void    *bits;
    int      capW, capH;   /* DIB 实际尺寸，够用就复用，避免拖动时反复重建 */
    RECT     rc;           /* 生成缓存时的标注区域 */
    RECT     sel;          /* 生成缓存时的选区 */
    unsigned epoch;        /* 生成缓存时的场景版本，0 表示空 */
} MosaicCache;

static MosaicCache g_mosaicCache[MAX_ANNOT];
static MosaicCache g_mosaicScratch;      /* 超大区域走的临时缓冲，不占正式缓存预算 */
static unsigned     g_mosaicEpoch = 1;
static size_t       g_mosaicCachedPixels;  /* 只统计 g_mosaicCache 里的正式缓存，不含量 scratch */

static BOOL RectEq(RECT a, RECT b);

/* scratch 是单块临时缓冲，用完即弃，不参与正式缓存的预算统计 */
static BOOL MosaicBudgeted(const MosaicCache *c)
{
    return c != &g_mosaicScratch;
}

static void MosaicCacheFree(MosaicCache *c)
{
    if (MosaicBudgeted(c) && c->capW > 0 && c->capH > 0) {   /* 归还正式缓存预算 */
        size_t px = (size_t)c->capW * (size_t)c->capH;
        g_mosaicCachedPixels = (g_mosaicCachedPixels >= px) ? g_mosaicCachedPixels - px : 0;
    }
    if (c->hdc) {
        if (c->oldBmp) GdiRestore(c->hdc, c->oldBmp, "mosaic cache bitmap");
        DeleteDC(c->hdc);
        c->hdc = NULL;
        GdiFlushDeferred();                 /* 缓存的 DC 已销毁，挂起对象可释放 */
    }
    c->oldBmp = NULL;
    if (c->hbm) { DeleteObject(c->hbm); c->hbm = NULL; }
    c->bits = NULL;
    c->capW = c->capH = 0;
    c->epoch = 0;
    SetRectEmpty(&c->rc);
    SetRectEmpty(&c->sel);
}

static void MosaicCacheFreeAll(void)
{
    for (int i = 0; i < MAX_ANNOT; i++) MosaicCacheFree(&g_mosaicCache[i]);
    MosaicCacheFree(&g_mosaicScratch);
}

/* 标注内容或顺序变化后调用：旧缓存全部作废，下次绘制时重算 */
static void MosaicEpochBump(void)
{
    if (++g_mosaicEpoch == 0) {          /* 版本号回绕，直接清空避免误命中 */
        MosaicCacheFreeAll();
        g_mosaicEpoch = 1;
    }
}

/* 准备一块至少 w×h 的 32bpp DIB 作为缓存画布 */
static BOOL MosaicEnsure(MosaicCache *c, int w, int h)
{
    if (c->hbm && c->hdc && c->bits && c->capW >= w && c->capH >= h) return TRUE;
    MosaicCacheFree(c);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    c->hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &c->bits, NULL, 0);
    if (!c->hbm || !c->bits) { MosaicCacheFree(c); return FALSE; }

    c->hdc = CreateCompatibleDC(NULL);
    if (!c->hdc) { MosaicCacheFree(c); return FALSE; }

    c->oldBmp = NULL;
    if (!GdiSelect(c->hdc, c->hbm, &c->oldBmp)) { MosaicCacheFree(c); return FALSE; }

    c->capW = w;
    c->capH = h;
    if (MosaicBudgeted(c)) g_mosaicCachedPixels += (size_t)w * (size_t)h;
    return TRUE;
}

/* 逐块取平均色并涂平。stride 是 DIB 的实际行字节数（capW * 4），
 * 可能比 w * 4 宽，所以按 stride 定位行、只处理前 w 个像素。 */
static void MosaicQuantize(BYTE *p, int stride, int w, int h)
{
    const int bs = MOSAIC_BLOCK;
    for (int by = 0; by < h; by += bs) {
        int bh = min(bs, h - by);
        for (int bx = 0; bx < w; bx += bs) {
            int bw = min(bs, w - bx);
            long sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int y = 0; y < bh; y++) {
                BYTE *row = p + (size_t)(by + y) * (size_t)stride + (size_t)bx * 4;
                for (int x = 0; x < bw; x++) {
                    sb += row[x * 4 + 0];
                    sg += row[x * 4 + 1];
                    sr += row[x * 4 + 2];
                    n++;
                }
            }
            if (!n) continue;
            BYTE ar = (BYTE)(sr / n), ag = (BYTE)(sg / n), ab = (BYTE)(sb / n);
            for (int y = 0; y < bh; y++) {
                BYTE *row = p + (size_t)(by + y) * (size_t)stride + (size_t)bx * 4;
                for (int x = 0; x < bw; x++) {
                    row[x * 4 + 0] = ab;
                    row[x * 4 + 1] = ag;
                    row[x * 4 + 2] = ar;
                }
            }
        }
    }
}

static void DrawMosaicAnnot(HDC hdc, const Annot *a, int idx)
{
    int w = a->rc.right - a->rc.left;
    int h = a->rc.bottom - a->rc.top;
    if (w < 2 || h < 2) return;

    size_t pixels = (size_t)w * (size_t)h;
    MosaicCache *slot = (idx >= 0 && idx < MAX_ANNOT) ? &g_mosaicCache[idx] : NULL;
    size_t slotPixels = slot ? (size_t)slot->capW * (size_t)slot->capH : 0;
    /* 单条超限、或者加上它要突破总预算时，就退回不缓存的临时缓冲。
     * 预算只统计正式缓存槽位；scratch 只是单块临时画布，不计入。 */
    BOOL cacheable = slot && pixels <= (size_t)MOSAIC_CACHE_MAX_PIXELS &&
                     g_mosaicCachedPixels - slotPixels + pixels <= (size_t)MOSAIC_CACHE_BUDGET;
    MosaicCache *c = cacheable ? slot : &g_mosaicScratch;

    /* 缓存命中：直接贴回去，不碰 DIB、不做逐块平均 */
    if (cacheable && c->hbm && c->hdc && c->bits &&
        c->epoch == g_mosaicEpoch && c->capW >= w && c->capH >= h &&
        RectEq(c->rc, a->rc) && RectEq(c->sel, g_ov.sel)) {
        BitBlt(hdc, a->rc.left, a->rc.top, w, h, c->hdc, 0, 0, SRCCOPY);
        return;
    }

    if (!MosaicEnsure(c, w, h)) return;
    /* 没取到底图像素就别往下算，否则会把垃圾数据当马赛克贴出去 */
    if (!BitBlt(c->hdc, 0, 0, w, h, hdc, a->rc.left, a->rc.top, SRCCOPY)) return;

    MosaicQuantize((BYTE *)c->bits, c->capW * 4, w, h);
    if (cacheable) {
        c->rc    = a->rc;
        c->sel   = g_ov.sel;
        c->epoch = g_mosaicEpoch;
    }
    BitBlt(hdc, a->rc.left, a->rc.top, w, h, c->hdc, 0, 0, SRCCOPY);
}

/* 序号：实心圆 + 白色数字 */
static GdiReleaseStatus DrawNumberAnnot(HDC hdc, const Annot *a, GdiPaintBatch *batch)
{
    if (!a->text[0]) return GdiRelFreed;

    int d = a->fontH;                       /* 直径 */
    int l = a->x - d / 2, t = a->y - d / 2;
    RECT r = { l, t, l + d, t + d };

    HBRUSH br = CreateSolidBrush(a->color);
    HGDIOBJ oldBr = NULL, oldPen = NULL;
    GdiReleaseStatus stBr = GdiRelFreed;
    if (GdiSelect(hdc, br, &oldBr)) {
        if (GdiSelect(hdc, GetStockObject(NULL_PEN), &oldPen)) {
            Ellipse(hdc, r.left, r.top, r.right, r.bottom);
            GdiRestore(hdc, oldPen, "number circle null pen");
        }
        stBr = GdiPaintRelease(hdc, br, oldBr, WHITE_BRUSH, "number circle brush", batch);
    } else {
        stBr = GdiPaintDiscard(br, "number circle brush unselected", batch);
    }

    HFONT f = CreateFontW(-(int)(d * 0.58), 0, 0, 0, FW_BOLD, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei UI");
    if (!f) return stBr;                    /* 没字体就只保留圆底 */
    HGDIOBJ oldF = NULL;
    GdiReleaseStatus stF = GdiRelFreed;
    if (GdiSelect(hdc, f, &oldF)) {
        int oldBk = SetBkMode(hdc, TRANSPARENT);
        COLORREF oldTc = SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, a->text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, oldTc);
        SetBkMode(hdc, oldBk);
        stF = GdiPaintRelease(hdc, f, oldF, DEFAULT_GUI_FONT, "number font", batch);
    } else {
        stF = GdiPaintDiscard(f, "number font unselected", batch);
    }
    /* 画刷与字体各自独立交接，汇总只影响返回值 */
    return (stBr == GdiRelUnreleased || stF == GdiRelUnreleased) ? GdiRelUnreleased
                                                                : GdiRelFreed;
}

static GdiReleaseStatus DrawTextAnnot(HDC hdc, const Annot *a, GdiPaintBatch *batch)
{
    if (!a->text[0]) return GdiRelFreed;
    HFONT f = CreateFontW(-a->fontH, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei UI");
    if (!f) return GdiRelFreed;
    HGDIOBJ oldF = NULL;
    if (GdiSelect(hdc, f, &oldF)) {
        int oldBk = SetBkMode(hdc, TRANSPARENT);
        COLORREF oldTc = SetTextColor(hdc, a->color);
        RECT r = { a->x, a->y, a->x + 4000, a->y + a->fontH * 3 };
        DrawTextW(hdc, a->text, -1, &r, DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE);
        SetTextColor(hdc, oldTc);
        SetBkMode(hdc, oldBk);
        return GdiPaintRelease(hdc, f, oldF, DEFAULT_GUI_FONT, "text font", batch);
    }
    return GdiPaintDiscard(f, "text font unselected", batch);
}

/* 全部标注画到 hdc，坐标就是遮罩客户端坐标。
 * 每个标注的临时对象独立交接：删除失败→托管链表；连托管都失败（OOM）→调用方批次，
 * 每个标注结束后立即重试，帧末剩余转交遮罩会话兜底批次。 */
static void DrawAnnotations(HDC hdc)
{
    GdiPaintBatch batch;
    GdiBatchInit(&batch);

    for (int i = 0; i < g_annCount; i++) {
        GdiReleaseStatus st = GdiRelFreed;
        switch (g_ann[i].kind) {
        case AN_PEN:     st = DrawPenStroke(hdc, &g_ann[i], &batch);    break;
        case AN_RECT:    st = DrawRectAnnot(hdc, &g_ann[i], &batch);    break;
        case AN_ELLIPSE: st = DrawEllipseAnnot(hdc, &g_ann[i], &batch); break;
        case AN_ARROW:   st = DrawArrowAnnot(hdc, &g_ann[i], &batch);   break;
        case AN_MOSAIC:  DrawMosaicAnnot(hdc, &g_ann[i], i);            break;
        case AN_TEXT:    st = DrawTextAnnot(hdc, &g_ann[i], &batch);    break;
        case AN_NUMBER:  st = DrawNumberAnnot(hdc, &g_ann[i], &batch);  break;
        }

        if (GdiBatchDrain(&batch) > 0) {
            QSLog("DrawAnnotations: annot %d (kind=%d) 仍有 %d 个句柄未托管（status=%d）",
                  i, (int)g_ann[i].kind, batch.count, (int)st);
        }
    }

    /* 帧末还没交出去的句柄不能随局部变量消失，转入遮罩会话兜底批次 */
    GdiBatchAdopt(&batch);
}

/* 生成成品位图（bottom-up 32bpp DIB Section），标注一并烘焙进去。
 * *ppBits 返回像素指针，布局正好是 CF_DIB 要的 bottom-up。
 * 任一步失败都会把已建好的资源释放掉并返回 NULL。 */
static HBITMAP BuildFinalBitmap(int x, int y, int w, int h, void **ppBits)
{
    if (ppBits) *ppBits = NULL;
    if (w <= 0 || h <= 0 || !g_ov.hdcCap) return NULL;
    if ((size_t)w > (((size_t)-1) / 4u) / (size_t)h) return NULL;   /* 行字节 * 高度不能溢出 */

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = h;      /* 正数 = bottom-up */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return NULL;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdcScreen);
    if (!hbm || !bits) { if (hbm) DeleteObject(hbm); return NULL; }

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) { DeleteObject(hbm); return NULL; }

    HGDIOBJ old = NULL;
    if (!GdiSelect(dc, hbm, &old)) {
        DeleteDC(dc);
        DeleteObject(hbm);
        return NULL;
    }

    BOOL ok = BitBlt(dc, 0, 0, w, h, g_ov.hdcCap, x, y, SRCCOPY);

    /* 挪视口原点，标注坐标不必逐个减 */
    if (ok && g_annCount > 0) {
        SetViewportOrgEx(dc, -x, -y, NULL);
        DrawAnnotations(dc);
        SetViewportOrgEx(dc, 0, 0, NULL);
    }

    /* 还原失败也不要紧：DeleteDC 会解除位图引用，随后 DeleteObject 才有效 */
    GdiRestore(dc, old, "final bitmap");
    DeleteDC(dc);
    GdiFlushDeferred();                    /* 临时 DC 已销毁，挂起对象可释放 */
    if (!ok) {                             /* 底图都没贴成功，返回的位图没有意义 */
        DeleteObject(hbm);
        return NULL;
    }

    /* BitBlt 会把 alpha 清零，补回不透明 */
    BYTE *p = (BYTE *)bits;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) p[i * 4 + 3] = 255;

    if (ppBits) *ppBits = bits;
    return hbm;
}

/* 把 32bpp bottom-up 像素打包成 CF_DIB 放进剪贴板。
 * 所有权：只有 SetClipboardData 成功后 hMem 才交给系统，其余失败路径由这里释放。 */
static BOOL CopyDibToClipboard(const void *bits, int w, int h)
{
    if (!bits || w <= 0 || h <= 0) return FALSE;

    /* 用 size_t 计算并逐步校验，避免 w*4*h 这类 32 位乘法溢出 */
    size_t sw = (size_t)w;
    size_t sh = (size_t)h;
    size_t stride = sw * 4u;
    if (stride / 4u != sw) return FALSE;
    size_t imgSize = stride * sh;
    if (sh && imgSize / sh != stride) return FALSE;
    if (imgSize > (size_t)0xFFFFFFFFu - sizeof(BITMAPINFOHEADER)) return FALSE;  /* biSizeImage 是 DWORD */
    if (imgSize > (size_t)0x7FFFFFFF) return FALSE;                              /* 超过剪贴板可接受范围 */

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imgSize);
    if (!hMem) return FALSE;
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)GlobalLock(hMem);
    if (!bih) { GlobalFree(hMem); return FALSE; }

    ZeroMemory(bih, sizeof(*bih));
    bih->biSize        = sizeof(BITMAPINFOHEADER);
    bih->biWidth       = w;
    bih->biHeight      = h;          /* 正数 = bottom-up */
    bih->biPlanes      = 1;
    bih->biBitCount    = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage   = (DWORD)imgSize;
    memcpy((BYTE *)bih + sizeof(BITMAPINFOHEADER), bits, imgSize);
    GlobalUnlock(hMem);

    /* 剪贴板可能被别的程序短暂占用，重试几次再放弃 */
    BOOL ok = FALSE;
    for (int attempt = 0; attempt < 5 && !ok; attempt++) {
        if (!OpenClipboard(g_hMsgWnd)) { Sleep(10); continue; }
        if (EmptyClipboard()) ok = (SetClipboardData(CF_DIB, hMem) != NULL);
        CloseClipboard();
        if (!ok) Sleep(10);
    }

    if (!ok) {
        GlobalFree(hMem);
        QSLog("clipboard: copy %dx%d FAIL err=%lu", w, h, GetLastError());
    }
    return ok;
}

static BOOL SaveBitmapToPng(HBITMAP hbm, const WCHAR *path)
{
    if (!hbm || !path) return FALSE;
    if (!g_gdiplusOk) return FALSE;         /* GDI+ 没起来就别调 Gdip 接口 */
    BOOL ok = FALSE;
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromHBITMAP(hbm, NULL, &bmp) == 0 && bmp) {
        CLSID clsid;
        if (GetEncoderClsid(L"image/png", &clsid)) {
            ok = (GdipSaveImageToFile((GpImage *)bmp, path, &clsid, NULL) == 0);
        }
        GdipDisposeImage((GpImage *)bmp);
    }
    return ok;
}

/* 生成不重名的截图路径：沿用"截图_年月日_时分秒.png"的风格，
 * 同一秒重名就追加 _2、_3……，并用 CREATE_NEW 占位，
 * 绝不会静默覆盖用户已有文件。目录不可写/不存在时返回 FALSE。 */
static BOOL BuildUniqueShotPath(const WCHAR *dir, WCHAR *out, DWORD cch)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    WCHAR base[96];
    wsprintfW(base, L"截图_%04d%02d%02d_%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    for (int n = 1; n <= 999; n++) {
        WCHAR name[128];
        if (n == 1) wsprintfW(name, L"%s.png", base);
        else        wsprintfW(name, L"%s_%d.png", base, n);

        if ((size_t)lstrlenW(dir) + 1 + (size_t)lstrlenW(name) + 1 > (size_t)cch) return FALSE;
        wsprintfW(out, L"%s\\%s", dir, name);

        HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return TRUE; }

        DWORD err = GetLastError();
        if (err != ERROR_FILE_EXISTS && err != ERROR_ALREADY_EXISTS) {
            QSLog("shot path: %ls -> err=%lu", out, err);
            return FALSE;                  /* 目录缺失 / 没权限，重试也没用 */
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------- 提示气泡 */

static LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(RGB(28, 28, 28));
        if (b) {
            FillRect(dc, &rc, b);
            DeleteObject(b);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(238, 238, 238));
        HGDIOBJ oldF = NULL;
        if (GdiSelect(dc, g_fontToast, &oldF)) {
            DrawTextW(dc, g_toastText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            GdiRestore(dc, oldF, "toast font");
        } else {
            /* 字体没建出来就用系统默认字体画，别让提示变成空白 */
            DrawTextW(dc, g_toastText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(h, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_toast == h) g_toast = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void ToastShowAt(const WCHAR *text, int sx, int sy)
{
    if (g_toast) { DestroyWindow(g_toast); g_toast = NULL; }
    if (!text || !*text) return;
    lstrcpynW(g_toastText, text, 256);

    HDC dc = GetDC(NULL);
    SIZE sz = {0, 0};
    if (dc) {
        HGDIOBJ oldF = NULL;
        if (GdiSelect(dc, g_fontToast, &oldF)) {
            GetTextExtentPoint32W(dc, g_toastText, lstrlenW(g_toastText), &sz);
            GdiRestore(dc, oldF, "toast measure font");
        }
        ReleaseDC(NULL, dc);
    }

    int w = sz.cx + 36;
    int h = 34;

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int x = sx - w / 2;
    int y = sy;
    if (x < vx + 8) x = vx + 8;
    if (x + w > vx + vw - 8) x = vx + vw - 8 - w;
    if (y < vy + 8) y = vy + 8;
    if (y + h > vy + vh - 8) y = vy + vh - 8 - h;

    g_toast = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        TOAST_CLASS, L"", WS_POPUP, x, y, w, h,
        NULL, NULL, g_hInst, NULL);
    if (!g_toast) return;

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, 10, 10);
    if (rgn && !SetWindowRgn(g_toast, rgn, TRUE)) DeleteObject(rgn);   /* 设置失败时区域还归我们，要删 */

    ShowWindow(g_toast, SW_SHOWNOACTIVATE);
    SetTimer(g_toast, 1, 1500, NULL);
}

/* ---------------------------------------------------------------- 遮罩窗口 */

static int g_pressBtn = -1;    /* 工具条上被按住的按钮 */

static void OverlayGdiInit(void)
{
    g_brHandle     = CreateSolidBrush(RGB(255, 255, 255));
    g_brHandleEdge = CreateSolidBrush(RGB(0, 153, 255));
    g_brLabelBg    = CreateSolidBrush(RGB(18, 18, 18));
    g_brTbBg       = CreateSolidBrush(RGB(250, 250, 250));
    g_brTbBorder   = CreateSolidBrush(RGB(150, 150, 150));
    g_brTbHover    = CreateSolidBrush(RGB(226, 238, 250));
    g_brTbActive   = CreateSolidBrush(RGB(198, 224, 250));
    g_penSel       = CreatePen(PS_SOLID, 1, RGB(0, 153, 255));
    g_penOuter     = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    g_penSep       = CreatePen(PS_SOLID, 1, RGB(205, 205, 205));
    g_penOk        = CreatePen(PS_SOLID, 2, RGB(34, 150, 80));
    g_penNo        = CreatePen(PS_SOLID, 2, RGB(200, 60, 60));
}

/* 自检用：确保工具条/选框的全局 GDI 对象存在。
 * 应用里由捕获入口（OverlayGdiInit）负责，自检里没有走那条路径，需要显式补齐。 */
static void TestOverlayEnsureGdi(void)
{
    if (!g_penSel) OverlayGdiInit();
}

static void OverlayGdiFree(void)
{
    if (g_brHandle)     { DeleteObject(g_brHandle);     g_brHandle = NULL; }
    if (g_brHandleEdge) { DeleteObject(g_brHandleEdge); g_brHandleEdge = NULL; }
    if (g_brLabelBg)    { DeleteObject(g_brLabelBg);    g_brLabelBg = NULL; }
    if (g_brTbBg)       { DeleteObject(g_brTbBg);       g_brTbBg = NULL; }
    if (g_brTbBorder)   { DeleteObject(g_brTbBorder);   g_brTbBorder = NULL; }
    if (g_brTbHover)    { DeleteObject(g_brTbHover);    g_brTbHover = NULL; }
    if (g_brTbActive)   { DeleteObject(g_brTbActive);   g_brTbActive = NULL; }
    if (g_penSel)       { DeleteObject(g_penSel);       g_penSel = NULL; }
    if (g_penOuter)     { DeleteObject(g_penOuter);     g_penOuter = NULL; }
    if (g_penSep)       { DeleteObject(g_penSep);       g_penSep = NULL; }
    if (g_penOk)        { DeleteObject(g_penOk);        g_penOk = NULL; }
    if (g_penNo)        { DeleteObject(g_penNo);        g_penNo = NULL; }
}

static RECT RectNorm(int l, int t, int r, int b)
{
    RECT o;
    o.left = min(l, r); o.right = max(l, r);
    o.top  = min(t, b); o.bottom = max(t, b);
    return o;
}

static BOOL RectEq(RECT a, RECT b)
{
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

static int SelW(void) { return g_ov.sel.right - g_ov.sel.left; }
static int SelH(void) { return g_ov.sel.bottom - g_ov.sel.top; }

static BOOL PtIn(RECT r, int x, int y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static void GetHandlePoints(RECT r, POINT pts[H_COUNT])
{
    int mx = (r.left + r.right) / 2;
    int my = (r.top + r.bottom) / 2;
    pts[H_TL].x = r.left;  pts[H_TL].y = r.top;
    pts[H_T ].x = mx;      pts[H_T ].y = r.top;
    pts[H_TR].x = r.right; pts[H_TR].y = r.top;
    pts[H_R ].x = r.right; pts[H_R ].y = my;
    pts[H_BR].x = r.right; pts[H_BR].y = r.bottom;
    pts[H_B ].x = mx;      pts[H_B ].y = r.bottom;
    pts[H_BL].x = r.left;  pts[H_BL].y = r.bottom;
    pts[H_L ].x = r.left;  pts[H_L ].y = my;
}

static int HitHandle(int x, int y)
{
    POINT pts[H_COUNT];
    GetHandlePoints(g_ov.sel, pts);
    for (int i = 0; i < H_COUNT; i++) {
        if (abs(x - pts[i].x) <= HANDLE_HIT && abs(y - pts[i].y) <= HANDLE_HIT)
            return i;
    }
    return -1;
}

/* 第一行按钮序号（0..7 与 TOOL_* 一一对应） */
enum { BTN_MOVE = 0, BTN_PEN, BTN_RECT, BTN_ELLIPSE, BTN_ARROW, BTN_MOSAIC, BTN_TEXT,
       BTN_NUMBER, BTN_UNDO, BTN_OK, BTN_NO };

/* 第 i 个按钮相对工具条左边的偏移。两行布局：
 *   第一行：工具组(0..7) | 撤销(8) | ✗(10) ✓(9)
 *   第二行：颜色 | 粗细 | 钉住 */
static int BtnX(int i)
{
    if (i <= 7) return 6 + i * TB_BTN;       /* 工具组 6..246 */
    if (i == 8) return 254;                  /* 撤销   254..284 */
    /* 末尾两个按钮：取消在左、确认在右（确认放在最外侧） */
    if (i == BTN_OK) return 292 + TB_BTN;    /* ✓ 322..352 */
    return 292;                              /* ✗ 292..322 */
}

/* 工具条贴着选区右下角；下方放不下就放上方 */
static RECT ToolbarRect(void)
{
    RECT s = g_ov.sel;
    int x = s.right - TB_W;
    int y = s.bottom + 12;
    if (y + TB_H > g_ov.vh - 4) y = s.top - TB_H - 12;
    if (y < 4) y = s.bottom + 12;
    if (y + TB_H > g_ov.vh - 4) y = g_ov.vh - 4 - TB_H;
    if (x < 4) x = 4;
    if (x + TB_W > g_ov.vw - 4) x = g_ov.vw - 4 - TB_W;
    RECT r = { x, y, x + TB_W, y + TB_H };
    return r;
}

static RECT BtnRect(int i)
{
    RECT t = ToolbarRect();
    int x = t.left + BtnX(i);
    RECT r = { x, t.top + 2, x + TB_BTN, t.top + TB_ROW1 - 2 };
    return r;
}

/* 返回按钮序号，-1 表示不在按钮上 */
static int HitButton(int x, int y)
{
    if (!(g_ov.hasSel && g_ov.state == ST_SELECTED)) return -1;
    for (int i = 0; i < TB_NBTN; i++) {
        if (PtIn(BtnRect(i), x, y)) return i;
    }
    return -1;
}

/* -------- 第二行：颜色 / 粗细 / 钉住 -------- */

#define ROW2_Y(t)   ((t).top + TB_ROW1)
#define SWATCH_X    6
#define SIZE_X      124
#define SIZE_W      24
#define SIZE_H      20
#define SIZE_STEP   27
#define PIN_X       216
#define PIN_W       28
#define PIN_H       20

static RECT SwatchRect(int i)
{
    RECT t = ToolbarRect();
    int x = t.left + SWATCH_X + i * (SWATCH_SIZE + SWATCH_GAP);
    int y = ROW2_Y(t) + (TB_ROW2 - SWATCH_SIZE) / 2;
    RECT r = { x, y, x + SWATCH_SIZE, y + SWATCH_SIZE };
    return r;
}

static int HitSwatch(int x, int y)
{
    if (!(g_ov.hasSel && g_ov.state == ST_SELECTED)) return -1;
    for (int i = 0; i < PALETTE_N; i++) {
        if (PtIn(SwatchRect(i), x, y)) return i;
    }
    return -1;
}

static RECT SizeRect(int i)
{
    RECT t = ToolbarRect();
    int x = t.left + SIZE_X + i * SIZE_STEP;
    int y = ROW2_Y(t) + (TB_ROW2 - SIZE_H) / 2;
    RECT r = { x, y, x + SIZE_W, y + SIZE_H };
    return r;
}

static int HitSize(int x, int y)
{
    if (!(g_ov.hasSel && g_ov.state == ST_SELECTED)) return -1;
    for (int i = 0; i < 3; i++) {
        if (PtIn(SizeRect(i), x, y)) return i;
    }
    return -1;
}

static RECT PinRect(void)
{
    RECT t = ToolbarRect();
    int x = t.left + PIN_X;
    int y = ROW2_Y(t) + (TB_ROW2 - PIN_H) / 2;
    RECT r = { x, y, x + PIN_W, y + PIN_H };
    return r;
}

static BOOL HitPin(int x, int y)
{
    if (!(g_ov.hasSel && g_ov.state == ST_SELECTED)) return FALSE;
    return PtIn(PinRect(), x, y);
}

static HCURSOR OverlayCursorAt(int x, int y)
{
    if (g_ov.hasSel && g_ov.state == ST_SELECTED) {
        if (HitButton(x, y) >= 0) return LoadCursorW(NULL, IDC_HAND);
        if (HitSwatch(x, y) >= 0) return LoadCursorW(NULL, IDC_HAND);
        if (HitSize(x, y) >= 0) return LoadCursorW(NULL, IDC_HAND);
        if (HitPin(x, y)) return LoadCursorW(NULL, IDC_HAND);

        /* 工具条空白处（分组缝、内边距）用普通箭头，别显示十字 */
        if (PtIn(ToolbarRect(), x, y)) return LoadCursorW(NULL, IDC_ARROW);

        int hd = HitHandle(x, y);
        switch (hd) {
        case H_TL: case H_BR: return LoadCursorW(NULL, IDC_SIZENWSE);
        case H_TR: case H_BL: return LoadCursorW(NULL, IDC_SIZENESW);
        case H_T:  case H_B:  return LoadCursorW(NULL, IDC_SIZENS);
        case H_R:  case H_L:  return LoadCursorW(NULL, IDC_SIZEWE);
        default: break;
        }
        if (PtIn(g_ov.sel, x, y)) {
            return (g_tool == TOOL_MOVE)
                ? LoadCursorW(NULL, IDC_SIZEALL)
                : LoadCursorW(NULL, IDC_CROSS);
        }
    }
    return LoadCursorW(NULL, IDC_CROSS);
}

/* 整块平移，保证不跑出屏幕 */
static void SelMoveBy(int dx, int dy)
{
    int w = SelW(), h = SelH();
    int l = g_ov.sel.left + dx;
    int t = g_ov.sel.top + dy;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (l + w > g_ov.vw) l = g_ov.vw - w;
    if (t + h > g_ov.vh) t = g_ov.vh - h;
    g_ov.sel.left = l;  g_ov.sel.top = t;
    g_ov.sel.right = l + w; g_ov.sel.bottom = t + h;
}

/* 拖某个手柄缩放：只动对应的边，允许越过对边翻转 */
static void SelResizeByHandle(int handle, int mx, int my)
{
    RECT r = g_ov.selAtDown;
    int l = r.left, t = r.top, rr = r.right, b = r.bottom;

    switch (handle) {
    case H_TL: l = mx; t = my; break;
    case H_T:  t = my; break;
    case H_TR: rr = mx; t = my; break;
    case H_R:  rr = mx; break;
    case H_BR: rr = mx; b = my; break;
    case H_B:  b = my; break;
    case H_BL: l = mx; b = my; break;
    case H_L:  l = mx; break;
    }

    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (rr > g_ov.vw) rr = g_ov.vw;
    if (b > g_ov.vh) b = g_ov.vh;

    g_ov.sel = RectNorm(l, t, rr, b);
}

static void DrawHandles(HDC hdc)
{
    POINT pts[H_COUNT];
    GetHandlePoints(g_ov.sel, pts);
    for (int i = 0; i < H_COUNT; i++) {
        RECT q = { pts[i].x - 3, pts[i].y - 3, pts[i].x + 4, pts[i].y + 4 };
        FillRect(hdc, &q, g_brHandle);
        FrameRect(hdc, &q, g_brHandleEdge);
    }
}

/* ---------------- 图标绘制：GDI+ 抗锯齿 ----------------
 * 之前用 GDI 画 1px 硬线，边缘全是台阶。这里统一走 GDI+，
 * 圆头笔触 + 抗锯齿，各图标光学粗细一致。
 */
#define ICON_INK   RGB(40, 40, 40)
#define ICON_OK    RGB(28, 158, 82)
#define ICON_NO    RGB(206, 62, 62)

static ARGB IconARGB(COLORREF c)
{
    return (ARGB)0xFF000000u
         | ((ARGB)GetRValue(c) << 16)
         | ((ARGB)GetGValue(c) << 8)
         |  (ARGB)GetBValue(c);
}

static GpPen *IconPen(COLORREF c, float w)
{
    GpPen *p = NULL;
    if (GdipCreatePen1(IconARGB(c), w, UnitPixel, &p) != 0 || !p) return NULL;
    GdipSetPenStartCap(p, LineCapRound);
    GdipSetPenEndCap(p, LineCapRound);
    GdipSetPenLineJoin(p, LineJoinRound);
    return p;
}

static void IconFillPoly(GpGraphics *g, COLORREF c, const float *xy, int n)
{
    GpPath *path = NULL;
    if (GdipCreatePath(FillModeWinding, &path) != 0 || !path) return;

    GdipAddPathLine(path, xy[0], xy[1], xy[2], xy[3]);
    for (int i = 1; i < n - 1; i++)
        GdipAddPathLine(path, xy[i * 2], xy[i * 2 + 1], xy[(i + 1) * 2], xy[(i + 1) * 2 + 1]);
    GdipClosePathFigure(path);

    GpSolidFill *b = NULL;
    if (GdipCreateSolidFill(IconARGB(c), &b) == 0 && b) {
        GdipFillPath(g, b, path);
        GdipDeleteBrush(b);
    }
    GdipDeletePath(path);
}

static void IconFillTri(GpGraphics *g, COLORREF c,
                        float x1, float y1, float x2, float y2, float x3, float y3)
{
    float xy[6] = { x1, y1, x2, y2, x3, y3 };
    IconFillPoly(g, c, xy, 3);
}

static void IconFillRect(GpGraphics *g, COLORREF c, float x, float y, float w, float h)
{
    GpSolidFill *b = NULL;
    if (GdipCreateSolidFill(IconARGB(c), &b) == 0 && b) {
        GdipFillRectangle(g, b, x, y, w, h);
        GdipDeleteBrush(b);
    }
}

static void IconFillCircle(GpGraphics *g, COLORREF c, float cx, float cy, float r)
{
    GpSolidFill *b = NULL;
    if (GdipCreateSolidFill(IconARGB(c), &b) == 0 && b) {
        GdipFillEllipse(g, b, cx - r, cy - r, r * 2, r * 2);
        GdipDeleteBrush(b);
    }
}

static void IconLine(GpGraphics *g, COLORREF c, float w,
                     float x1, float y1, float x2, float y2)
{
    GpPen *p = IconPen(c, w);
    if (!p) return;
    GdipDrawLine(g, p, x1, y1, x2, y2);
    GdipDeletePen(p);
}

static void DrawToolIcon(GpGraphics *g, int i, float cx, float cy)
{
    if (!g) return;
    GpPen *p = NULL;

    switch (i) {
    case BTN_MOVE: {
        /* 鼠标指针轮廓 */
        float xy[14] = {
            cx - 4.6f, cy - 8.0f,
            cx - 4.6f, cy + 4.6f,
            cx - 1.7f, cy + 1.9f,
            cx + 0.3f, cy + 7.4f,
            cx + 3.2f, cy + 6.2f,
            cx + 1.2f, cy + 1.0f,
            cx + 5.2f, cy + 1.0f
        };
        IconFillPoly(g, ICON_INK, xy, 7);
        break;
    }
    case BTN_PEN:
        /* 笔身 + 笔尖 */
        IconLine(g, ICON_INK, 2.0f, cx - 5.0f, cy + 5.0f, cx + 6.3f, cy - 6.3f);
        IconFillTri(g, ICON_INK, cx - 8.3f, cy + 8.3f,
                    cx - 3.0f, cy + 6.5f, cx - 6.5f, cy + 3.0f);
        break;

    case BTN_RECT:
        p = IconPen(ICON_INK, 1.7f);
        if (p) { GdipDrawRectangle(g, p, cx - 8.0f, cy - 6.0f, 16.0f, 12.0f); GdipDeletePen(p); }
        break;

    case BTN_ELLIPSE:
        p = IconPen(ICON_INK, 1.7f);
        if (p) { GdipDrawEllipse(g, p, cx - 8.0f, cy - 6.2f, 16.0f, 12.4f); GdipDeletePen(p); }
        break;

    case BTN_ARROW:
        IconLine(g, ICON_INK, 1.8f, cx - 7.2f, cy + 7.2f, cx + 2.4f, cy - 2.4f);
        IconFillTri(g, ICON_INK, cx + 7.7f, cy - 7.7f,
                    cx + 1.0f, cy - 6.4f, cx + 6.4f, cy - 1.0f);
        break;

    case BTN_MOSAIC: {
        /* 3×3 灰阶块，像打了码 */
        static const COLORREF sh[3] = { RGB(88, 88, 88), RGB(142, 142, 142), RGB(196, 196, 196) };
        float s = 4.4f, gp = 1.0f;
        float span = 3 * s + 2 * gp;
        float ox = cx - span / 2, oy = cy - span / 2;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                IconFillRect(g, sh[(r * 3 + c) % 3],
                             ox + c * (s + gp), oy + r * (s + gp), s, s);
        break;
    }
    case BTN_TEXT:
        /* 折线画 A，和其他图标同一套笔触 */
        p = IconPen(ICON_INK, 1.8f);
        if (p) {
            GdipDrawLine(g, p, cx - 5.4f, cy + 6.4f, cx, cy - 6.6f);
            GdipDrawLine(g, p, cx, cy - 6.6f, cx + 5.4f, cy + 6.4f);
            GdipDrawLine(g, p, cx - 3.3f, cy + 1.3f, cx + 3.3f, cy + 1.3f);
            GdipDeletePen(p);
        }
        break;

    case BTN_NUMBER:
        IconFillCircle(g, ICON_INK, cx, cy, 8.0f);
        p = IconPen(RGB(255, 255, 255), 1.5f);
        if (p) {
            GdipDrawLine(g, p, cx + 0.2f, cy - 3.8f, cx + 0.2f, cy + 3.6f);
            GdipDrawLine(g, p, cx - 1.9f, cy - 1.7f, cx + 0.2f, cy - 3.8f);
            GdipDrawLine(g, p, cx - 2.4f, cy + 3.6f, cx + 2.6f, cy + 3.6f);
            GdipDeletePen(p);
        }
        break;

    case BTN_UNDO:
        p = IconPen(ICON_INK, 1.8f);
        if (p) {
            GdipDrawArc(g, p, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f, 180.0f, 180.0f);
            GdipDeletePen(p);
        }
        IconFillTri(g, ICON_INK, cx - 7.0f, cy + 4.2f,
                    cx - 10.5f, cy - 1.4f, cx - 3.5f, cy - 1.4f);
        break;

    case BTN_OK:
        p = IconPen(ICON_OK, 2.2f);
        if (p) {
            GdipDrawLine(g, p, cx - 6.0f, cy - 0.2f, cx - 1.6f, cy + 4.4f);
            GdipDrawLine(g, p, cx - 1.6f, cy + 4.4f, cx + 6.8f, cy - 5.6f);
            GdipDeletePen(p);
        }
        break;

    case BTN_NO:
        p = IconPen(ICON_NO, 2.2f);
        if (p) {
            GdipDrawLine(g, p, cx - 5.4f, cy - 5.4f, cx + 5.4f, cy + 5.4f);
            GdipDrawLine(g, p, cx + 5.4f, cy - 5.4f, cx - 5.4f, cy + 5.4f);
            GdipDeletePen(p);
        }
        break;
    }
}

static void DrawPalette(HDC hdc)
{
    for (int i = 0; i < PALETTE_N; i++) {
        RECT r = SwatchRect(i);

        HBRUSH b = CreateSolidBrush(g_palette[i]);
        if (b) {
            FillRect(hdc, &r, b);
            DeleteObject(b);
        }

        BOOL sel = (g_annColor == g_palette[i]);
        BOOL hot = (g_ov.hotSwatch == i);

        COLORREF ec = sel ? RGB(25, 25, 25)
                    : hot ? RGB(70, 70, 70)
                          : RGB(145, 145, 145);
        HBRUSH edge = CreateSolidBrush(ec);
        if (edge) {
            FrameRect(hdc, &r, edge);
            if (sel) {
                RECT o = { r.left - 1, r.top - 1, r.right + 1, r.bottom + 1 };
                FrameRect(hdc, &o, edge);
            }
            DeleteObject(edge);
        }
    }
}

/* 粗细 / 钉住：底色边框走 GDI，图形走 GDI+ */
static void DrawSizeRowBg(HDC hdc)
{
    for (int i = 0; i < 3; i++) {
        RECT r = SizeRect(i);
        BOOL sel = (g_sizeLevel == i);
        BOOL hot = (g_ov.hotSize == i);

        FillRect(hdc, &r, sel ? g_brTbActive : (hot ? g_brTbHover : g_brTbBg));

        HBRUSH edge = CreateSolidBrush(sel ? RGB(60, 130, 200) : RGB(178, 178, 178));
        if (edge) {
            FrameRect(hdc, &r, edge);
            DeleteObject(edge);
        }
    }
}

static void DrawSizeRowIcon(GpGraphics *g)
{
    static const float th[3] = { 1.2f, 2.2f, 4.0f };
    for (int i = 0; i < 3; i++) {
        RECT r = SizeRect(i);
        BOOL sel = (g_sizeLevel == i);
        float cx = (r.left + r.right) / 2.0f;
        float cy = (r.top + r.bottom) / 2.0f;
        IconLine(g, sel ? RGB(18, 68, 128) : RGB(55, 55, 55),
                 th[i], cx - 7.0f, cy, cx + 7.0f, cy);
    }
}

static void DrawPinBg(HDC hdc)
{
    RECT r = PinRect();
    BOOL hot = g_ov.hotPin;

    FillRect(hdc, &r, hot ? g_brTbHover : g_brTbBg);

    HBRUSH edge = CreateSolidBrush(hot ? RGB(70, 70, 70) : RGB(178, 178, 178));
    if (edge) {
        FrameRect(hdc, &r, edge);
        DeleteObject(edge);
    }
}

static void DrawPinIcon(GpGraphics *g)
{
    RECT r = PinRect();
    float cx = (r.left + r.right) / 2.0f;
    float cy = (r.top + r.bottom) / 2.0f;

    /* 图钉：扁盖 + 收窄的身 + 细针 */
    IconFillRect(g, ICON_INK, cx - 6.2f, cy - 8.6f, 12.4f, 3.4f);

    float body[8] = {
        cx - 3.2f, cy - 5.2f,
        cx + 3.2f, cy - 5.2f,
        cx + 1.1f, cy + 0.8f,
        cx - 1.1f, cy + 0.8f
    };
    IconFillPoly(g, ICON_INK, body, 4);

    IconLine(g, ICON_INK, 1.3f, cx, cy + 0.8f, cx, cy + 8.2f);
}

static void DrawToolbar(HDC hdc)
{
    RECT t = ToolbarRect();
    FillRect(hdc, &t, g_brTbBg);

    /* 当前工具 / 悬停 / 按下 高亮 */
    for (int i = 0; i < TB_NBTN; i++) {
        BOOL activeTool = (i <= BTN_NUMBER && g_tool == i);
        BOOL hot = (g_ov.hotBtn == i || g_pressBtn == i);
        if (activeTool || hot) {
            RECT b = BtnRect(i);
            FillRect(hdc, &b, activeTool ? g_brTbActive : g_brTbHover);
        }
    }

    FrameRect(hdc, &t, g_brTbBorder);

    /* 行分隔 + 分组竖线；画笔没建出来（极端情况下）就跳过这几条线 */
    HGDIOBJ oldPen = NULL;
    if (GdiSelect(hdc, g_penSep, &oldPen)) {
        int ry = ROW2_Y(t);
        MoveToEx(hdc, t.left + 4, ry, NULL);
        LineTo(hdc, t.right - 4, ry);

        int v1 = t.left + BtnX(BTN_UNDO) - 4;
        int v2 = t.left + min(BtnX(BTN_OK), BtnX(BTN_NO)) - 4;   /* 分组线始终在 ✗✓ 左侧 */
        MoveToEx(hdc, v1, t.top + 8, NULL);
        LineTo(hdc, v1, ry - 6);
        MoveToEx(hdc, v2, t.top + 8, NULL);
        LineTo(hdc, v2, ry - 6);

        int s1 = t.left + SIZE_X - 8;
        int s2 = t.left + PIN_X - 8;
        MoveToEx(hdc, s1, ry + 5, NULL);
        LineTo(hdc, s1, t.bottom - 5);
        MoveToEx(hdc, s2, ry + 5, NULL);
        LineTo(hdc, s2, t.bottom - 5);
        GdiRestore(hdc, oldPen, "toolbar separator pen");
    }

    /* 底色元素先画完，再统一上矢量图标，避免 GDI / GDI+ 混用 */
    DrawPalette(hdc);
    DrawSizeRowBg(hdc);
    DrawPinBg(hdc);

    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(hdc, &g) == 0 && g) {
        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
        GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);

        for (int i = 0; i < TB_NBTN; i++) {
            RECT b = BtnRect(i);
            DrawToolIcon(g, i, (b.left + b.right) / 2.0f, (b.top + b.bottom) / 2.0f);
        }
        DrawSizeRowIcon(g);
        DrawPinIcon(g);

        GdipDeleteGraphics(g);
    }
}

static void OverlayDraw(HDC hdcTarget)
{
    /* 底色：整幅变暗图 */
    BitBlt(g_ov.hdcBack, 0, 0, g_ov.vw, g_ov.vh, g_ov.hdcDim, 0, 0, SRCCOPY);

    if (g_ov.hasSel && SelW() > 0 && SelH() > 0) {
        RECT s = g_ov.sel;

        /* 选区内还原原始亮度 */
        BitBlt(g_ov.hdcBack, s.left, s.top, SelW(), SelH(),
               g_ov.hdcCap, s.left, s.top, SRCCOPY);

        /* 标注：裁到选区内画，避免溢到变暗区 */
        if (g_annCount > 0) {
            int saved = SaveDC(g_ov.hdcBack);
            IntersectClipRect(g_ov.hdcBack, s.left, s.top, s.right, s.bottom);
            DrawAnnotations(g_ov.hdcBack);
            RestoreDC(g_ov.hdcBack, saved);
        }

        /* 选中框：外白内蓝（画笔没建出来就跳过边框，不影响内容） */
        HGDIOBJ oldBr = NULL, oldPen = NULL;
        BOOL haveBr = GdiSelect(g_ov.hdcBack, GetStockObject(NULL_BRUSH), &oldBr);
        if (haveBr && GdiSelect(g_ov.hdcBack, g_penOuter, &oldPen)) {
            Rectangle(g_ov.hdcBack, s.left - 1, s.top - 1, s.right + 1, s.bottom + 1);
            GdiRestore(g_ov.hdcBack, oldPen, "selection outer frame");
        }
        if (haveBr && GdiSelect(g_ov.hdcBack, g_penSel, &oldPen)) {
            Rectangle(g_ov.hdcBack, s.left, s.top, s.right, s.bottom);
            GdiRestore(g_ov.hdcBack, oldPen, "selection inner frame");
        }
        GdiRestore(g_ov.hdcBack, oldBr, "selection frame brush");

        /* 尺寸标签：贴选区左上角外侧，放不下就挪进去 */
        WCHAR label[64];
        wsprintfW(label, L"%d × %d", SelW(), SelH());

        /* 字体没建出来时用默认字体量，此时不需要（也不能）恢复 */
        HGDIOBJ oldFont = NULL;
        BOOL haveFont = GdiSelect(g_ov.hdcBack, g_fontLabel, &oldFont);
        SIZE ts = {0, 0};
        GetTextExtentPoint32W(g_ov.hdcBack, label, lstrlenW(label), &ts);

        int tw = ts.cx + 16, th = ts.cy + 8;
        int lx = s.left;
        int ly = s.top - th - 6;
        if (ly < 4) ly = s.top + 6;
        if (lx + tw > g_ov.vw - 4) lx = g_ov.vw - 4 - tw;
        if (lx < 4) lx = 4;

        RECT tr = { lx, ly, lx + tw, ly + th };
        FillRect(g_ov.hdcBack, &tr, g_brLabelBg);
        int oldBk = SetBkMode(g_ov.hdcBack, TRANSPARENT);
        COLORREF oldTc = SetTextColor(g_ov.hdcBack, RGB(255, 255, 255));
        DrawTextW(g_ov.hdcBack, label, -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(g_ov.hdcBack, oldTc);
        SetBkMode(g_ov.hdcBack, oldBk);
        if (haveFont) GdiRestore(g_ov.hdcBack, oldFont, "size label font");

        /* 松手之后才显示手柄和工具条 */
        if (g_ov.state != ST_NEW) {
            DrawHandles(g_ov.hdcBack);
            if (g_ov.state == ST_SELECTED) DrawToolbar(g_ov.hdcBack);
        }
    }

    BitBlt(hdcTarget, 0, 0, g_ov.vw, g_ov.vh, g_ov.hdcBack, 0, 0, SRCCOPY);
}

static void OverlayRedraw(void)
{
    if (!g_ov.hwnd) return;
    HDC dc = GetDC(g_ov.hwnd);
    if (dc) {
        OverlayDraw(dc);
        ReleaseDC(g_ov.hwnd, dc);
    }
}

/* ---------------------------------------------------------------- 文字输入 */

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) { PostMessageW(g_ov.hwnd, WM_EDIT_COMMIT, 0, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(g_ov.hwnd, WM_EDIT_CANCEL, 0, 0); return 0; }
    }
    if (m == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcW(g_editOldProc, h, m, wp, lp);
}

static void TextCancel(void);

/* 输入框统一销毁出口：先把控件销毁，再释放它的字体。
 * WM_SETFONT 只是把句柄存进控件、不会复制字体对象，
 * 控件还活着的时候 DeleteObject 会让后续绘制/重绘拿到失效句柄。 */
static void TextEditDestroy(void)
{
    if (g_edit) {
        HWND e = g_edit;
        g_edit = NULL;
        DestroyWindow(e);
    }
    if (g_editFont) {
        DeleteObject(g_editFont);
        g_editFont = NULL;
    }
}

static void TextCommit(void)
{
    if (!g_edit) return;

    WCHAR buf[256] = {0};
    GetWindowTextW(g_edit, buf, 256);

    TextEditDestroy();                     /* 控件和字体一起收掉 */
    if (g_ov.hwnd) SetFocus(g_ov.hwnd);

    int editing = g_editEditing;
    g_editEditing = -1;

    if (editing >= 0 && editing < g_annCount) {
        /* 二次编辑：原地改内容；清空则把这整条删掉 */
        if (buf[0]) {
            lstrcpynW(g_ann[editing].text, buf, 256);
            QSLog("text edit commit: idx=%d", editing);
        } else {
            for (int i = editing; i < g_annCount - 1; i++) g_ann[i] = g_ann[i + 1];
            g_annCount--;
            QSLog("text edit cleared: idx=%d", editing);
        }
    } else if (buf[0] && g_annCount < MAX_ANNOT) {
        Annot *a = &g_ann[g_annCount];
        ZeroMemory(a, sizeof(*a));
        a->kind  = AN_TEXT;
        a->color = g_annColor;
        a->x     = g_editX;
        a->y     = g_editY;
        a->fontH = g_textH[g_sizeLevel];
        lstrcpynW(a->text, buf, 256);
        g_annCount++;
        QSLog("text commit: %dx%d n=%d", g_editX, g_editY, g_annCount);
    }
    /* 文字变了 / 下标可能平移，压在上面或下面的马赛克缓存都得作废 */
    MosaicEpochBump();
    OverlayRedraw();
}

static void TextCancel(void)
{
    if (!g_edit) return;
    g_editEditing = -1;
    TextEditDestroy();
    if (g_ov.hwnd) SetFocus(g_ov.hwnd);
    OverlayRedraw();
}

/* 量出一条文字标注占多大地方，用于判断有没有点到它。
 * 返回测量用字体的最终归属；无法托管（OOM）时句柄写入调用方 batch，
 * 由调用方在返回后立即 drain，不能随局部变量消失。 */
static GdiReleaseStatus TextAnnotRect(const Annot *a, RECT *out, GdiPaintBatch *batch)
{
    SIZE s = {0, 0};
    GdiReleaseStatus st = GdiRelFreed;
    HDC dc = GetDC(NULL);
    if (dc) {
        HFONT f = CreateFontW(-a->fontH, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
        if (f) {
            HGDIOBJ of = NULL;
            if (GdiSelect(dc, f, &of)) {
                GetTextExtentPoint32W(dc, a->text, lstrlenW(a->text), &s);
                st = GdiPaintRelease(dc, f, of, DEFAULT_GUI_FONT, "measure font", batch);
            } else {
                st = GdiPaintDiscard(f, "measure font unselected", batch);
            }
        }
        ReleaseDC(NULL, dc);
    }

    out->left   = a->x;
    out->top    = a->y;
    out->right  = a->x + s.cx;
    out->bottom = a->y + s.cy;
    return st;
}

static void TextStart(int x, int y)
{
    TextCommit();                          /* 先落定上一个 */

    /* 点在已有文字上 → 二次编辑它 */
    int hitIdx = -1;
    GdiPaintBatch batch;
    GdiBatchInit(&batch);
    for (int i = g_annCount - 1; i >= 0; i--) {
        if (g_ann[i].kind != AN_TEXT) continue;
        RECT r;
        TextAnnotRect(&g_ann[i], &r, &batch);
        r.left -= 4; r.top -= 4; r.right += 4; r.bottom += 4;   /* 放宽一点好点中 */
        if (PtIn(r, x, y)) { hitIdx = i; break; }
    }
    /* 测量字体的释放结果必须在这里收口：先重试，再把仍交不出去的交给会话兜底 */
    if (GdiBatchDrain(&batch) > 0)
        QSLog("TextStart: %d 个测量字体句柄未能托管", batch.count);
    GdiBatchAdopt(&batch);

    int fontH = (hitIdx >= 0) ? g_ann[hitIdx].fontH : g_textH[g_sizeLevel];

    RECT s = g_ov.sel;
    int ew = 260, eh = fontH + 14;
    if (hitIdx >= 0) {
        x = g_ann[hitIdx].x;
        y = g_ann[hitIdx].y;
    }
    if (x + ew > s.right)  ew = max(80, s.right - x);
    if (y + eh > s.bottom) y  = max(s.top, s.bottom - eh);
    if (x < s.left) x = s.left;

    g_editX = x;
    g_editY = y;

    TextEditDestroy();                     /* 保险：上一个输入框连字体一起清掉 */

    /* 编辑框的字号跟着当前档位走 */
    HFONT ef = CreateFontW(-fontH, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Microsoft YaHei UI");
    if (!ef) {
        QSLog("TextStart: CreateFont failed");
        return;
    }

    g_edit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | ES_NOHIDESEL,
        x, y, ew, eh, g_ov.hwnd, (HMENU)(INT_PTR)ID_EDIT, g_hInst, NULL);
    if (!g_edit) {
        DeleteObject(ef);
        QSLog("TextStart: CreateWindow EDIT failed");
        return;
    }

    /* 字体所有权跟着输入框走：WM_SETFONT 不复制字体，控件绘制时还要用这个句柄，
     * 所以先存到 g_editFont，等 TextEditDestroy 销毁控件之后再释放。 */
    g_editFont = ef;
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);

    g_editOldProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    g_editEditing = hitIdx;

    if (hitIdx >= 0) {
        SetWindowTextW(g_edit, g_ann[hitIdx].text);
        SendMessageW(g_edit, EM_SETSEL, 0, -1);    /* 全选，直接打字即替换 */
    }

    SetFocus(g_edit);
    QSLog("TextStart at %d,%d editing=%d", x, y, g_editEditing);
}

/* 遮罩资源统一释放出口：输入框(含字体) → 标注缓存 → 截屏缓冲 → 界面 GDI 对象。
 * 提交、取消、异常退出、窗口意外销毁都走这里，避免任何一条路径漏放资源。 */
static void OverlayReleaseAll(void)
{
    TextEditDestroy();
    g_editEditing = -1;
    g_annCount    = 0;
    g_numberNext  = 1;
    MosaicCacheFreeAll();
    FreeCapture();
    OverlayGdiFree();
}

/* ---------------------------------------------------------------- 钉在屏幕上 */

static void OverlayClose(void);

typedef struct {
    HBITMAP hbm;
    void   *bits;          /* DIB 像素指针，复制到剪贴板用 */
    HDC     hdc;
    HBITMAP oldBmp;        /* 建 DC 时原来的位图，销毁前要选回去 */
    int     w, h;
    POINT   dragFrom;      /* 按下时的屏幕坐标 */
    POINT   winAt;         /* 按下时的窗口左上角 */
    BOOL    dragging;
} PinData;

#define PIN_MENU_COPY   1
#define PIN_MENU_SAVE   2
#define PIN_MENU_CLOSE  3

static void PinToast(HWND h, const WCHAR *msg)
{
    RECT r;
    GetWindowRect(h, &r);
    ToastShowAt(msg, (r.left + r.right) / 2, r.bottom + 14);
}

static void PinCopy(HWND h, PinData *pd)
{
    if (!pd || !pd->bits) return;
    BOOL ok = CopyDibToClipboard(pd->bits, pd->w, pd->h);
    PinToast(h, ok ? L"已复制到剪贴板" : L"复制失败，剪贴板可能被其他程序占用");
    QSLog("pin: copy %dx%d -> %s", pd->w, pd->h, ok ? "ok" : "FAIL");
}

static void PinSaveAs(HWND h, PinData *pd)
{
    if (!pd || !pd->hbm) return;

    WCHAR path[MAX_PATH];
    SYSTEMTIME st;
    GetLocalTime(&st);
    wsprintfW(path, L"钉图_%04d%02d%02d_%02d%02d%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = h;
    ofn.lpstrFilter = L"PNG 图片\0*.png\0所有文件\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;

    if (!GetSaveFileNameW(&ofn)) { QSLog("pin: save cancelled"); return; }

    BOOL ok = SaveBitmapToPng(pd->hbm, path);
    PinToast(h, ok ? L"已保存" : L"保存失败");
    QSLog("pin: save %s -> %ls", ok ? "ok" : "FAIL", path);
}

static LRESULT CALLBACK PinProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    PinData *pd = (PinData *)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (m) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (pd && pd->hdc) {
            BitBlt(dc, 0, 0, pd->w, pd->h, pd->hdc, 0, 0, SRCCOPY);
            /* 描一圈细边，和白底页面区分开 */
            HBRUSH b = CreateSolidBrush(RGB(120, 120, 120));
            RECT r = { 0, 0, pd->w, pd->h };
            if (b) {
                FrameRect(dc, &r, b);
                DeleteObject(b);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (pd) {
            SetCapture(h);
            pd->dragging = TRUE;
            GetCursorPos(&pd->dragFrom);
            RECT r;
            GetWindowRect(h, &r);
            pd->winAt.x = r.left;
            pd->winAt.y = r.top;
        }
        return 0;

    case WM_MOUSEMOVE:
        if (pd && pd->dragging) {
            POINT p;
            GetCursorPos(&p);
            SetWindowPos(h, HWND_TOPMOST,
                         pd->winAt.x + (p.x - pd->dragFrom.x),
                         pd->winAt.y + (p.y - pd->dragFrom.y),
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (pd) {
            pd->dragging = FALSE;
            if (GetCapture() == h) ReleaseCapture();
        }
        return 0;

    case WM_RBUTTONUP: {
        /* 右键弹菜单，别直接关 —— 误点就没了 */
        HMENU mu = CreatePopupMenu();
        if (!mu) return 0;

        AppendMenuW(mu, MF_STRING,    PIN_MENU_COPY,  L"复制图片");
        AppendMenuW(mu, MF_STRING,    PIN_MENU_SAVE,  L"另存为 PNG...");
        AppendMenuW(mu, MF_SEPARATOR, 0,              NULL);
        AppendMenuW(mu, MF_STRING,    PIN_MENU_CLOSE, L"关闭钉图");

        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(h);
        int cmd = (int)TrackPopupMenu(mu,
                    TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                    p.x, p.y, 0, h, NULL);
        DestroyMenu(mu);
        PostMessageW(h, WM_NULL, 0, 0);     /* 不补这条菜单可能不消失 */

        if (cmd == PIN_MENU_COPY)       PinCopy(h, pd);
        else if (cmd == PIN_MENU_SAVE)  PinSaveAs(h, pd);
        else if (cmd == PIN_MENU_CLOSE) DestroyWindow(h);
        return 0;
    }

    case WM_MBUTTONUP:
        DestroyWindow(h);                    /* 中键仍可快速关掉 */
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(h); return 0; }
        return 0;

    case WM_DESTROY:
        if (pd) {
            if (pd->hdc) {
                /* 先选回原位图；即使还原失败，DeleteDC 也会解除引用，位图仍可删除 */
                if (pd->oldBmp) GdiRestore(pd->hdc, pd->oldBmp, "pin window bitmap");
                DeleteDC(pd->hdc);
                GdiFlushDeferred();        /* 钉图 DC 已销毁，挂起对象可释放 */
            }
            if (pd->hbm) DeleteObject(pd->hbm);
            free(pd);
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void CreatePinWindow(HBITMAP hbm, void *bits, int w, int h, int x, int y)
{
    PinData *pd = (PinData *)calloc(1, sizeof(PinData));
    if (!pd) { DeleteObject(hbm); return; }

    pd->hbm  = hbm;
    pd->bits = bits;
    pd->w    = w;
    pd->h    = h;
    pd->hdc = CreateCompatibleDC(NULL);
    if (!pd->hdc) { DeleteObject(hbm); free(pd); return; }

    HGDIOBJ oldBmp = NULL;
    if (!GdiSelect(pd->hdc, hbm, &oldBmp)) {
        DeleteDC(pd->hdc);
        GdiFlushDeferred();
        DeleteObject(hbm);
        free(pd);
        return;
    }
    pd->oldBmp = (HBITMAP)oldBmp;

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        PIN_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_hInst, pd);
    if (!hw) {
        GdiRestore(pd->hdc, pd->oldBmp, "pin create restore");
        DeleteDC(pd->hdc);
        GdiFlushDeferred();
        DeleteObject(pd->hbm);
        free(pd);
        return;
    }
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    QSLog("pin window created %dx%d at (%d,%d)", w, h, x, y);
    ToastShowAt(L"已钉住 · 拖动移动，右键出菜单", x + w / 2, y + h + 14);
}

/* 把当前选区和标注钉到屏幕上，并关掉遮罩 */
static void PinCurrentSelection(void)
{
    if (g_ov.state == ST_DONE) return;
    g_ov.state = ST_DONE;

    TextCommit();

    int x1 = g_ov.sel.left, y1 = g_ov.sel.top;
    int w  = SelW(), h = SelH();
    if (w < 3 || h < 3) { OverlayClose(); return; }

    void *bits = NULL;
    HBITMAP hbm = BuildFinalBitmap(x1, y1, w, h, &bits);
    g_ov.hasSel = FALSE;
    if (g_ov.hwnd) ShowWindow(g_ov.hwnd, SW_HIDE);

    if (hbm) {
        CreatePinWindow(hbm, bits, w, h, g_ov.vx + x1, g_ov.vy + y1);
    }
    OverlayClose();
}

static void OverlayClose(void)
{
    if (g_ov.hwnd) PostMessageW(g_ov.hwnd, WM_OV_CLOSE, 0, 0);
}

static void OverlayFinish(void)
{
    if (g_ov.state == ST_DONE) return;      /* 防止重复提交 */
    g_ov.state = ST_DONE;

    TextCommit();                           /* 未结束的文字先落定 */

    int x1 = g_ov.sel.left;
    int y1 = g_ov.sel.top;
    int w  = SelW();
    int h  = SelH();
    QSLog("OverlayFinish: rect=(%d,%d) size=%dx%d annots=%d", x1, y1, w, h, g_annCount);

    if (w < 3 || h < 3) { QSLog("OverlayFinish: too small -> cancel"); OverlayClose(); return; }

    void *bits = NULL;
    HBITMAP hbm = BuildFinalBitmap(x1, y1, w, h, &bits);
    g_ov.hasSel = FALSE;      /* 别再把选框画进去 */
    if (g_ov.hwnd) ShowWindow(g_ov.hwnd, SW_HIDE);

    BOOL copied = FALSE, saved = FALSE;
    WCHAR msg[512];

    if (hbm && bits) {
        copied = CopyDibToClipboard(bits, w, h);

        if (g_saveFile) {
            WCHAR full[MAX_PATH * 2];
            /* 唯一命名 + CREATE_NEW 占位：同一秒连拍不会重名，也不会覆盖已有截图 */
            if (BuildUniqueShotPath(g_saveDir, full, (DWORD)(sizeof(full) / sizeof(full[0])))) {
                saved = SaveBitmapToPng(hbm, full);
                if (!saved) DeleteFileW(full);   /* 别留下 0 字节的残文件 */
                QSLog("OverlayFinish: save %s -> %ls", saved ? "ok" : "FAIL", full);
            } else {
                QSLog("OverlayFinish: save path FAIL dir=%ls", g_saveDir);
            }
        }
    } else {
        QSLog("OverlayFinish: BuildFinalBitmap FAIL");
    }
    if (hbm) DeleteObject(hbm);

    if (!g_toast) {
        if (copied && saved)             lstrcpynW(msg, L"已复制  ·  已保存到 Screenshots", 512);
        else if (copied && !g_saveFile)  lstrcpynW(msg, L"已复制到剪贴板", 512);
        else if (copied)                 lstrcpynW(msg, L"已复制 · 保存失败，请检查保存目录", 512);
        else if (saved)                  lstrcpynW(msg, L"已保存，但复制到剪贴板失败", 512);
        else                             lstrcpynW(msg, L"复制和保存都失败了", 512);
        ToastShowAt(msg,
                    g_ov.vx + x1 + w / 2,
                    g_ov.vy + y1 + h + 12);
    }

    OverlayClose();
}

/* 撤销一条；如果撤掉的是序号，编号也要退回去 */
static void UndoOne(void)
{
    if (g_annCount <= 0) return;
    if (g_ann[g_annCount - 1].kind == AN_NUMBER && g_numberNext > 1)
        g_numberNext--;
    g_annCount--;
    MosaicEpochBump();                     /* 标注少了一条，缓存下标可能对不上，整体作废 */
    QSLog("overlay: undo -> %d left, next number=%d", g_annCount, g_numberNext);
    OverlayRedraw();
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        OverlayDraw(dc);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        SetCursor(OverlayCursorAt(p.x, p.y));
        return TRUE;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);

        if (g_ov.state == ST_NEW) {
            int cx = max(0, min(x, g_ov.vw - 1));
            int cy = max(0, min(y, g_ov.vh - 1));
            RECT nr = RectNorm(g_ov.ptDown.x, g_ov.ptDown.y, cx, cy);
            if (RectEq(nr, g_ov.sel)) return 0;      /* 没动就不重绘 */
            g_ov.sel = nr;
            OverlayRedraw();
        } else if (g_ov.state == ST_MOVE) {
            g_ov.sel = g_ov.selAtDown;
            SelMoveBy(x - g_ov.ptDown.x, y - g_ov.ptDown.y);
            if (RectEq(g_ov.sel, g_ov.selAtDown)) return 0;
            OverlayRedraw();
        } else if (g_ov.state == ST_RESIZE) {
            RECT before = g_ov.sel;
            SelResizeByHandle(g_ov.handle, x, y);
            if (RectEq(g_ov.sel, before)) return 0;
            OverlayRedraw();
        } else if (g_ov.state == ST_DRAW) {
            if (g_annCount > 0) {
                Annot *a = &g_ann[g_annCount - 1];
                int cx = max(0, min(x, g_ov.vw - 1));
                int cy = max(0, min(y, g_ov.vh - 1));
                if (a->kind == AN_PEN) {
                    /* 挨太近的点不记，省内存也省重绘 */
                    if (a->npts > 0) {
                        POINT p = a->pts[a->npts - 1];
                        if (abs(cx - p.x) + abs(cy - p.y) < 2) return 0;
                    }
                    if (a->npts < MAX_PEN_PTS) {
                        a->pts[a->npts].x = cx;
                        a->pts[a->npts].y = cy;
                        a->npts++;
                    }
                } else if (a->kind == AN_ARROW) {
                    if (a->pts[1].x == cx && a->pts[1].y == cy) return 0;
                    a->pts[1].x = cx;
                    a->pts[1].y = cy;
                } else {
                    /* 矩形 / 椭圆 / 马赛克 都按外框走 */
                    RECT nr = RectNorm(g_ov.ptDown.x, g_ov.ptDown.y, cx, cy);
                    if (RectEq(nr, a->rc)) return 0;
                    a->rc = nr;
                }
            }
            OverlayRedraw();
        } else if (g_ov.state == ST_SELECTED) {
            int hb = HitButton(x, y);
            int hs = HitSwatch(x, y);
            int hz = HitSize(x, y);
            BOOL hp = HitPin(x, y);
            if (hb != g_ov.hotBtn || hs != g_ov.hotSwatch ||
                hz != g_ov.hotSize || hp != g_ov.hotPin) {
                g_ov.hotBtn    = hb;
                g_ov.hotSwatch = hs;
                g_ov.hotSize   = hz;
                g_ov.hotPin    = hp;
                OverlayRedraw();
            }
            SetCursor(OverlayCursorAt(x, y));
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (g_ov.state == ST_DONE) return 0;
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        QSLog("overlay DOWN (%d,%d) state=%d", x, y, g_ov.state);

        /* 先判工具条按钮 */
        int btn = HitButton(x, y);
        if (btn >= 0) {
            g_pressBtn = btn;
            OverlayRedraw();
            return 0;
        }

        /* 调色板 */
        int sw = HitSwatch(x, y);
        if (sw >= 0) {
            g_annColor = g_palette[sw];
            QSLog("overlay: color=%06lX", (unsigned long)g_annColor);
            OverlayRedraw();
            return 0;
        }

        /* 粗细档位 */
        int sz = HitSize(x, y);
        if (sz >= 0) {
            g_sizeLevel = sz;
            QSLog("overlay: size=%d", g_sizeLevel);
            OverlayRedraw();
            return 0;
        }

        /* 钉在屏幕上 */
        if (HitPin(x, y)) {
            QSLog("overlay: pin");
            PinCurrentSelection();
            return 0;
        }

        g_ov.ptDown.x  = x;
        g_ov.ptDown.y  = y;
        g_ov.selAtDown = g_ov.sel;

        if (g_ov.state == ST_SELECTED) {
            int hd = HitHandle(x, y);

            /* 手柄永远优先，免得画图时误抢缩放 */
            if (hd >= 0) {
                g_ov.handle = hd;
                g_ov.state  = ST_RESIZE;
                QSLog("overlay: resize handle %d", hd);
            } else if (PtIn(g_ov.sel, x, y) && g_tool != TOOL_MOVE) {
                /* 画图工具：在选区内按下直接开始 */
                if (g_tool == TOOL_TEXT) {
                    TextStart(x, y);
                    return 0;                  /* 不 SetCapture，交给输入框 */
                }
                if (g_tool == TOOL_NUMBER) {
                    /* 序号：点一下放一个，编号自增 */
                    if (g_annCount < MAX_ANNOT) {
                        Annot *a = &g_ann[g_annCount++];
                        ZeroMemory(a, sizeof(*a));
                        a->kind  = AN_NUMBER;
                        a->color = g_annColor;
                        a->x     = x;
                        a->y     = y;
                        a->fontH = g_numD[g_sizeLevel];
                        wsprintfW(a->text, L"%d", g_numberNext);
                        g_numberNext++;
                        QSLog("overlay: number=%ls next=%d", a->text, g_numberNext);
                        OverlayRedraw();
                    }
                    return 0;
                }
                if (g_annCount < MAX_ANNOT) {
                    Annot *a = &g_ann[g_annCount];
                    ZeroMemory(a, sizeof(*a));
                    a->color = g_annColor;
                    a->width = g_penW[g_sizeLevel];
                    switch (g_tool) {
                    case TOOL_PEN:
                        a->kind = AN_PEN;
                        a->pts[0].x = x;
                        a->pts[0].y = y;
                        a->npts = 1;
                        break;
                    case TOOL_ARROW:
                        a->kind = AN_ARROW;
                        a->pts[0].x = x;
                        a->pts[0].y = y;
                        a->pts[1].x = x;
                        a->pts[1].y = y;
                        a->npts = 2;
                        break;
                    case TOOL_RECT:
                        a->kind = AN_RECT;
                        a->rc = RectNorm(x, y, x, y);
                        break;
                    case TOOL_MOSAIC:
                        a->kind = AN_MOSAIC;
                        a->rc = RectNorm(x, y, x, y);
                        break;
                    default:
                        a->kind = AN_ELLIPSE;
                        a->rc = RectNorm(x, y, x, y);
                        break;
                    }
                    g_annCount++;
                    g_ov.state = ST_DRAW;
                    QSLog("overlay: start annot kind=%d total=%d", a->kind, g_annCount);
                    SetCapture(h);
                    OverlayRedraw();
                    return 0;
                }
            } else if (PtIn(g_ov.sel, x, y)) {
                g_ov.state = ST_MOVE;
                QSLog("overlay: move selection");
            } else {
                g_ov.state  = ST_NEW;
                g_ov.hasSel = TRUE;
                g_ov.sel    = RectNorm(x, y, x, y);
                g_annCount  = 0;               /* 重新框选，清掉旧标注 */
                MosaicEpochBump();             /* 标注全清，马赛克缓存一并作废 */
                QSLog("overlay: new selection (clicked outside)");
            }
        } else {
            g_ov.state  = ST_NEW;
            g_ov.hasSel = TRUE;
            g_ov.sel    = RectNorm(x, y, x, y);
            g_annCount  = 0;
            MosaicEpochBump();
        }

        SetCapture(h);
        OverlayRedraw();
        return 0;
    }

    case WM_LBUTTONUP: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);

        if (g_pressBtn >= 0) {
            int pressed = g_pressBtn;
            g_pressBtn = -1;
            if (HitButton(x, y) == pressed) {
                QSLog("overlay: toolbar button %d", pressed);
                if (pressed == BTN_OK) {
                    OverlayFinish();
                } else if (pressed == BTN_NO) {
                    OverlayClose();
                } else if (pressed == BTN_UNDO) {
                    UndoOne();
                } else {
                    g_tool = pressed;              /* 0..3 正好对应 TOOL_* */
                    QSLog("overlay: tool=%d", g_tool);
                    OverlayRedraw();
                }
            } else {
                OverlayRedraw();
            }
            return 0;
        }

        if (g_ov.state == ST_DRAW) {
            if (GetCapture() == h) ReleaseCapture();
            if (g_annCount > 0) {
                Annot *a = &g_ann[g_annCount - 1];
                BOOL keep = TRUE;
                if (a->kind == AN_PEN) {
                    if (a->npts < 2) keep = FALSE;
                } else if (a->kind == AN_ARROW) {
                    if (abs(a->pts[1].x - a->pts[0].x) +
                        abs(a->pts[1].y - a->pts[0].y) < 6) keep = FALSE;
                } else {
                    if ((a->rc.right - a->rc.left) < 3 ||
                        (a->rc.bottom - a->rc.top) < 3) keep = FALSE;
                }
                if (!keep) {
                    g_annCount--;
                    /* 这条标注在拖动过程中可能已经生成过马赛克缓存，
                     * 丢弃后必须让它失效：否则同一下标复用时可能命中旧图。
                     * 其它删除路径（撤销、文字清空、重新选区）也都会 bump。 */
                    MosaicEpochBump();
                    QSLog("overlay: annot discarded (too small), epoch=%u", g_mosaicEpoch);
                }
            }
            g_ov.state = ST_SELECTED;
            OverlayRedraw();
            return 0;
        }

        if (g_ov.state == ST_NEW || g_ov.state == ST_MOVE || g_ov.state == ST_RESIZE) {
            if (GetCapture() == h) ReleaseCapture();
            if (SelW() < 3 || SelH() < 3) {
                g_ov.hasSel = FALSE;
                g_ov.state  = ST_IDLE;
                QSLog("overlay: too small, reset");
            } else {
                g_ov.state = ST_SELECTED;
                RECT tb = ToolbarRect();
                QSLog("overlay: selection %dx%d at (%d,%d) -> adjustable; toolbar=(%d,%d)-(%d,%d)",
                      SelW(), SelH(), g_ov.sel.left, g_ov.sel.top,
                      tb.left, tb.top, tb.right, tb.bottom);
            }
            OverlayRedraw();
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        if (g_ov.state == ST_SELECTED && PtIn(g_ov.sel, x, y)) {
            QSLog("overlay: double-click confirm");
            OverlayFinish();
        }
        return 0;
    }

    case WM_RBUTTONUP:
        QSLog("overlay: right-click cancel");
        OverlayClose();
        return 0;

    case WM_EDIT_COMMIT:
        TextCommit();
        return 0;

    case WM_EDIT_CANCEL:
        TextCancel();
        return 0;

    case WM_CTLCOLOREDIT: {
        /* 输入框用红字白底，和最终烘焙出来的文字一致 */
        HDC dc = (HDC)wp;
        SetTextColor(dc, ANN_COLOR);
        SetBkColor(dc, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_edit) { TextCancel(); return 0; }
            QSLog("overlay: Esc cancel");
            OverlayClose();
            return 0;
        }
        if (wp == VK_RETURN) {
            if (g_ov.state == ST_SELECTED) { QSLog("overlay: Enter confirm"); OverlayFinish(); }
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == 'Z') {
            UndoOne();
            return 0;
        }
        if (g_ov.state == ST_SELECTED) {
            int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
            int dx = 0, dy = 0;
            if (wp == VK_LEFT)  dx = -step;
            if (wp == VK_RIGHT) dx =  step;
            if (wp == VK_UP)    dy = -step;
            if (wp == VK_DOWN)  dy =  step;
            if (dx || dy) { SelMoveBy(dx, dy); OverlayRedraw(); }
        }
        return 0;

    case WM_OV_CLOSE:
        QSLog("overlay WM_OV_CLOSE");
        if (GetCapture() == h) ReleaseCapture();
        OverlayReleaseAll();
        g_ov.hwnd = NULL;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        if (g_ov.hwnd == h) {
            g_ov.hwnd = NULL;
            OverlayReleaseAll();           /* 兜底：就算没走 WM_OV_CLOSE 也不漏资源 */
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void StartCapture(void)
{
    QSLog("StartCapture: enter, existing hwnd=%p", (void *)g_ov.hwnd);
    if (g_ov.hwnd) return;                  /* 已在截图 */
    if (!CaptureVirtualScreen()) {
        QSLog("StartCapture: CaptureVirtualScreen FAILED");
        ToastShowAt(L"截图失败", 0, 0);
        FreeCapture();
        return;
    }
    QSLog("StartCapture: captured %dx%d at (%d,%d), pixCap=%p",
          g_ov.vw, g_ov.vh, g_ov.vx, g_ov.vy, g_ov.pixCap);

    g_ov.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS, L"", WS_POPUP,
        g_ov.vx, g_ov.vy, g_ov.vw, g_ov.vh,
        NULL, NULL, g_hInst, NULL);

    if (!g_ov.hwnd) {
        QSLog("StartCapture: CreateWindowEx FAILED err=%lu", GetLastError());
        FreeCapture();
        ToastShowAt(L"无法创建遮罩窗口", 0, 0);
        return;
    }
    QSLog("StartCapture: overlay created hwnd=%p", (void *)g_ov.hwnd);

    OverlayGdiInit();
    g_ov.state   = ST_IDLE;
    g_ov.hasSel  = FALSE;
    g_ov.handle  = -1;
    g_ov.hotBtn  = -1;
    g_ov.hotSwatch = -1;
    g_ov.hotSize   = -1;
    g_ov.hotPin    = FALSE;
    g_pressBtn   = -1;
    g_annCount   = 0;
    g_tool       = TOOL_MOVE;
    g_edit       = NULL;
    g_editEditing = -1;
    g_numberNext  = 1;

    ShowWindow(g_ov.hwnd, SW_SHOW);
    SetWindowPos(g_ov.hwnd, HWND_TOPMOST, g_ov.vx, g_ov.vy, g_ov.vw, g_ov.vh,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetForegroundWindow(g_ov.hwnd);
    SetFocus(g_ov.hwnd);
    SetCursor(LoadCursorW(NULL, IDC_CROSS));
    OverlayRedraw();
    QSLog("StartCapture: shown, visible=%d", IsWindowVisible(g_ov.hwnd));
}

/* ---------------------------------------------------------------- 托盘 / 菜单 */

/* 注销当前生效的热键（退出时用） */
static void HotkeyUnregisterActive(void)
{
    if (g_hotkeyId) {
        UnregisterHotKey(g_hMsgWnd, g_hotkeyId);
        g_hotkeyId = 0;
    }
    g_hotkeyOk = FALSE;
}

/* 切换全局热键：先注册新组合，成功后才注销旧组合。
 *
 * 原来的做法是先注销旧的再试新的，新组合被别的程序占用时就会出现
 * “旧热键已经没了、新热键也没注册上、注册表却写了新值”的三输局面。
 * 这里反过来：新组合注册成功之前不动旧组合，失败就直接原样返回，
 * 调用方也只在成功后才写注册表，保证运行状态和持久化配置一致。
 *
 * 两个 ID 轮换（HOTKEY_ID / HOTKEY_ID_ALT）的原因：
 * 同一个 ID 无法同时持有新旧两个组合，而新组合没拿到之前又不能放开旧组合，
 * 所以新组合先挂在另一个 ID 上，拿到手之后再注销旧 ID。 */
static BOOL HotkeyTryApply(UINT mods, UINT vk)
{
    if (g_hotkeyOk && mods == g_mods && vk == g_vk) return TRUE;   /* 没变化，不重复注册 */
    if (!g_hMsgWnd) return FALSE;

    UINT newId = (g_hotkeyId == HOTKEY_ID) ? HOTKEY_ID_ALT : HOTKEY_ID;
    if (!RegisterHotKey(g_hMsgWnd, newId, mods | MOD_NOREPEAT, vk)) {
        QSLog("hotkey: register 0x%X/0x%X FAIL err=%lu (old kept)",
              mods, vk, GetLastError());
        return FALSE;                  /* 旧热键仍然有效，g_mods/g_vk 不变 */
    }

    /* 新组合已经到手，这时放开旧的才是安全的 */
    if (g_hotkeyId && g_hotkeyId != newId) UnregisterHotKey(g_hMsgWnd, g_hotkeyId);

    g_hotkeyId = newId;
    g_hotkeyOk = TRUE;
    g_mods     = mods;
    g_vk       = vk;
    QSLog("hotkey: applied 0x%X/0x%X id=0x%X", mods, vk, newId);
    return TRUE;
}

/* 启动时注册热键：优先用上次保存的组合；被占用就依次尝试其它预设。
 * 回退只影响本次运行，不写注册表——下次启动仍优先尝试用户原来选的那个，
 * 避免把一次临时的占用变成永久的配置变更。 */
enum { HK_START_NONE = 0, HK_START_SAVED, HK_START_FALLBACK };

static int HotkeyRegisterOnStartup(void)
{
    UINT savedMods = g_mods, savedVk = g_vk;

    if (HotkeyTryApply(savedMods, savedVk)) return HK_START_SAVED;

    QSLog("hotkey: saved 0x%X/0x%X occupied, trying fallbacks", savedMods, savedVk);
    for (int i = 0; i < PRESET_COUNT; i++) {
        if (g_presets[i].mods == savedMods && g_presets[i].vk == savedVk) continue;
        if (HotkeyTryApply(g_presets[i].mods, g_presets[i].vk)) {
            QSLog("hotkey: fallback -> 0x%X/0x%X (not persisted)", g_mods, g_vk);
            return HK_START_FALLBACK;
        }
    }
    QSLog("hotkey: all presets occupied");
    return HK_START_NONE;
}

static void ShowTrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    HMENU hk = CreatePopupMenu();
    if (!m || !hk) {
        if (m) DestroyMenu(m);
        if (hk) DestroyMenu(hk);
        return;
    }
    for (int i = 0; i < PRESET_COUNT; i++) {
        BOOL active = (g_presets[i].mods == g_mods && g_presets[i].vk == g_vk);
        AppendMenuW(hk, MF_STRING | (active ? MF_CHECKED : 0),
                    MENU_HK_BASE + i, g_presets[i].label);
    }

    AppendMenuW(m, MF_STRING, MENU_SHOT, L"截图");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_saveFile ? MF_CHECKED : 0), MENU_SAVEFILE,
                L"同时保存到文件");
    AppendMenuW(m, MF_STRING | (IsAutoStart() ? MF_CHECKED : 0), MENU_AUTOSTART,
                L"开机自动启动");
    AppendMenuW(m, MF_STRING, MENU_OPENDIR, L"打开保存目录");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)hk, L"快捷键");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hMsgWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_hMsgWnd, NULL);
    PostMessageW(g_hMsgWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* 当前生效组合对应的菜单文案；不在预设里就返回通用文案 */
static const WCHAR *HotkeyLabel(void)
{
    for (int i = 0; i < PRESET_COUNT; i++) {
        if (g_presets[i].mods == g_mods && g_presets[i].vk == g_vk) return g_presets[i].label;
    }
    return L"当前快捷键";
}

static void TrayAdd(void)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hMsgWnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    0, 0, LR_DEFAULTSIZE);
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"QuickShot 截图", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

/* 托盘提示跟着实际生效的热键走（启动回退或手动切换后都要刷新） */
static void TrayUpdateTip(void)
{
    if (!g_nid.hWnd) return;

    WCHAR tip[128];
    if (g_hotkeyOk) wsprintfW(tip, L"QuickShot 截图  (%s)", HotkeyLabel());
    else            lstrcpynW(tip, L"QuickShot 截图  (快捷键未生效)", 128);

    lstrcpynW(g_nid.szTip, tip, 128);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void TrayRemove(void)
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

/* ---------------------------------------------------------------- 主消息窗口 */

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowTrayMenu();
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            StartCapture();
        }
        return 0;

    case WM_HOTKEY:
        QSLog("WM_HOTKEY id=0x%X", (unsigned)LOWORD(wp));
        if (g_hotkeyId && LOWORD(wp) == g_hotkeyId) StartCapture();
        return 0;

    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == MENU_SHOT) {
            StartCapture();
        } else if (id == MENU_SAVEFILE) {
            g_saveFile = !g_saveFile;
            SaveSettingDword(L"SaveFile", g_saveFile ? 1 : 0);
        } else if (id == MENU_AUTOSTART) {
            SetAutoStart(!IsAutoStart());
        } else if (id == MENU_OPENDIR) {
            ShellExecuteW(NULL, L"open", g_saveDir, NULL, NULL, SW_SHOWNORMAL);
        } else if (id == MENU_EXIT) {
            DestroyWindow(h);
        } else if (id >= MENU_HK_BASE && id < MENU_HK_BASE + PRESET_COUNT) {
            int idx = (int)(id - MENU_HK_BASE);
            /* 只有新热键真的注册成功，才更新内存配置并落盘；
             * 失败时旧热键和旧配置都保持原样。 */
            if (HotkeyTryApply(g_presets[idx].mods, g_presets[idx].vk)) {
                SaveSettingDword(L"HotMods", g_mods);
                SaveSettingDword(L"HotVk", g_vk);
                TrayUpdateTip();           /* 提示文字跟着实际生效的组合走 */
            } else {
                MessageBoxW(NULL,
                            L"该快捷键已被其他程序占用，请换一个。\n\n原快捷键保持不变。",
                            L"QuickShot", MB_ICONWARNING | MB_OK);
            }
        }
        return 0;
    }

    case WM_DESTROY:
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ---------------------------------------------------------------- 自检 */

/* 离屏合成一张"遮罩界面"示意图：把指定图片当截图底图，
 * 套上选区、标注和工具条后输出 PNG。用来生成文档配图。 */
static int RunDemoShot(const WCHAR *bgPath, const WCHAR *outPath, BOOL showAll)
{
    GpBitmap *bmp = NULL;
    if (GdipCreateBitmapFromFile(bgPath, &bmp) != 0 || !bmp) {
        printf("FAIL: load bg\n");
        return 1;
    }
    UINT iw = 0, ih = 0;
    GdipGetImageWidth((GpImage *)bmp, &iw);
    GdipGetImageHeight((GpImage *)bmp, &ih);
    if (!iw || !ih) { GdipDisposeImage((GpImage *)bmp); return 1; }

    OverlayGdiInit();          /* 工具条底刷/画笔都靠它，漏了会画不出来 */

    ZeroMemory(&g_ov, sizeof(g_ov));
    g_ov.vx = 0; g_ov.vy = 0;
    g_ov.vw = (int)iw;
    g_ov.vh = (int)ih;

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) {
        GdipDisposeImage((GpImage *)bmp);
        OverlayGdiFree();
        return 1;
    }

    /* 把图铺进"截图"缓冲 */
    GpGraphics *gp = NULL;
    if (GdipCreateFromHDC(g_ov.hdcCap, &gp) != 0 || !gp) {
        GdipDisposeImage((GpImage *)bmp);
        FreeCapture();
        OverlayGdiFree();
        return 1;
    }
    GdipSetCompositingMode(gp, CompositingModeSourceCopy);
    GdipSetInterpolationMode(gp, InterpolationModeHighQualityBicubic);
    GdipDrawImageRectI(gp, (GpImage *)bmp, 0, 0, g_ov.vw, g_ov.vh);
    GdipDeleteGraphics(gp);
    GdipDisposeImage((GpImage *)bmp);

    if (!OvDimBuffers()) {
        FreeCapture();
        OverlayGdiFree();
        return 1;
    }
    g_ov.hasSel = TRUE;
    g_ov.state  = ST_SELECTED;
    g_ov.sel.left  = 430;  g_ov.sel.top    = 196;
    g_ov.sel.right = 2150; g_ov.sel.bottom = 1158;
    g_ov.hotBtn = -1; g_ov.hotSwatch = -1; g_ov.hotSize = -1; g_ov.hotPin = FALSE;
    g_pressBtn  = -1;
    g_tool      = TOOL_ELLIPSE;
    g_annColor  = g_palette[0];
    g_sizeLevel = 1;

    g_annCount = 0;
    {
        Annot *a = &g_ann[g_annCount++];        /* 马赛克盖手机号 */
        ZeroMemory(a, sizeof(*a));
        a->kind = AN_MOSAIC; a->color = g_annColor; a->width = g_penW[g_sizeLevel];
        a->rc.left = 578; a->rc.top = 836; a->rc.right = 748; a->rc.bottom = 876;

        Annot *b = &g_ann[g_annCount++];        /* 圈住合计金额 */
        ZeroMemory(b, sizeof(*b));
        b->kind = AN_ELLIPSE; b->color = g_annColor; b->width = g_penW[g_sizeLevel];
        b->rc.left = 1646; b->rc.top = 644; b->rc.right = 1830; b->rc.bottom = 720;

        Annot *c = &g_ann[g_annCount++];        /* 箭头指向按钮 */
        ZeroMemory(c, sizeof(*c));
        c->kind = AN_ARROW; c->color = g_annColor; c->width = g_penW[g_sizeLevel];
        c->pts[0].x = 1700; c->pts[0].y = 980;
        c->pts[1].x = 1952; c->pts[1].y = 1048;
        c->npts = 2;

        Annot *d = &g_ann[g_annCount++];        /* 文字 */
        ZeroMemory(d, sizeof(*d));
        d->kind = AN_TEXT; d->color = g_annColor; d->fontH = g_textH[g_sizeLevel];
        d->x = 1690; d->y = 300;
        lstrcpynW(d->text, L"待核对", 256);

        if (showAll) {
            Annot *e = &g_ann[g_annCount++];    /* 矩形框住一行 */
            ZeroMemory(e, sizeof(*e));
            e->kind = AN_RECT; e->color = g_palette[3]; e->width = g_penW[g_sizeLevel];
            e->rc.left = 462; e->rc.top = 388; e->rc.right = 1896; e->rc.bottom = 456;

            Annot *f = &g_ann[g_annCount++];    /* 画笔圈最后一行 */
            ZeroMemory(f, sizeof(*f));
            f->kind = AN_PEN; f->color = g_palette[2]; f->width = g_penW[g_sizeLevel];
            for (int i = 0; i <= 74; i++) {
                double t = i / 74.0 * 6.28318;
                f->pts[i].x = 540 + i * 18;              /* 横向铺开 */
                f->pts[i].y = 618 + (int)(40 * sin(t));  /* 正弦波动 */
                f->npts++;
            }

            Annot *g2 = &g_ann[g_annCount++];   /* 序号 */
            ZeroMemory(g2, sizeof(*g2));
            g2->kind = AN_NUMBER; g2->color = g_palette[1];
            g2->x = 1560; g2->y = 228; g2->fontH = g_numD[1];
            lstrcpynW(g2->text, L"1", 256);
        }
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_ov.vw;
    bi.bmiHeader.biHeight      = -g_ov.vh;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *obits = NULL;
    HDC sdc = GetDC(NULL);
    if (!sdc) { FreeCapture(); OverlayGdiFree(); return 1; }
    HBITMAP out = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &obits, NULL, 0);
    ReleaseDC(NULL, sdc);
    if (!out || !obits) {
        if (out) DeleteObject(out);
        FreeCapture();
        OverlayGdiFree();
        return 1;
    }

    HDC odc = CreateCompatibleDC(NULL);
    if (!odc) { DeleteObject(out); FreeCapture(); OverlayGdiFree(); return 1; }
    HGDIOBJ old = NULL;
    if (!GdiSelect(odc, out, &old)) {
        DeleteDC(odc);
        DeleteObject(out);
        FreeCapture();
        OverlayGdiFree();
        return 1;
    }
    OverlayDraw(odc);
    GdiRestore(odc, old, "demo output bitmap");
    DeleteDC(odc);
    GdiFlushDeferred();

    BYTE *pp = (BYTE *)obits;
    size_t n = (size_t)g_ov.vw * g_ov.vh;
    for (size_t i = 0; i < n; i++) pp[i * 4 + 3] = 255;

    BOOL ok = SaveBitmapToPng(out, outPath);
    DeleteObject(out);
    FreeCapture();
    OverlayGdiFree();
    printf("demo: %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int RunCaptureTest(void)
{
    WCHAR out[MAX_PATH];
    MakeSiblingPath(L"selftest.png", out, MAX_PATH);

    /* 造一个可验证的图案，避免在无桌面会话里全黑看不清 */
    if (!CaptureVirtualScreen()) {
        printf("FAIL: capture\n");
        return 1;
    }
    printf("capture: %d x %d at (%d,%d)\n", g_ov.vw, g_ov.vh, g_ov.vx, g_ov.vy);

    int x = 10, y = 10;
    int w = 400, h = 300;

    /* 六种标注各来一条，验证烘焙链路 */
    g_annCount = 0;
    {
        Annot *a = &g_ann[g_annCount++];
        ZeroMemory(a, sizeof(*a));
        a->kind = AN_RECT;
        a->color = g_palette[0];
        a->width = PEN_W;
        a->rc.left = x + 20;  a->rc.top = y + 15;
        a->rc.right = x + 140; a->rc.bottom = y + 85;

        Annot *b = &g_ann[g_annCount++];
        ZeroMemory(b, sizeof(*b));
        b->kind = AN_ELLIPSE;
        b->color = g_palette[1];
        b->width = PEN_W;
        b->rc.left = x + 170; b->rc.top = y + 15;
        b->rc.right = x + 290; b->rc.bottom = y + 85;

        Annot *c = &g_ann[g_annCount++];
        ZeroMemory(c, sizeof(*c));
        c->kind = AN_ARROW;
        c->color = g_palette[3];
        c->width = PEN_W;
        c->pts[0].x = x + 320; c->pts[0].y = y + 85;
        c->pts[1].x = x + 385; c->pts[1].y = y + 20;
        c->npts = 2;

        Annot *d = &g_ann[g_annCount++];
        ZeroMemory(d, sizeof(*d));
        d->kind = AN_MOSAIC;
        d->color = g_palette[0];
        d->width = PEN_W;
        d->rc.left = x + 20;  d->rc.top = y + 105;
        d->rc.right = x + 130; d->rc.bottom = y + 175;

        Annot *e = &g_ann[g_annCount++];
        ZeroMemory(e, sizeof(*e));
        e->kind = AN_PEN;
        e->color = g_palette[2];
        e->width = PEN_W;
        for (int i = 0; i <= 60; i++) {
            int dx = i - 30;
            e->pts[i].x = x + 150 + i * 4;
            e->pts[i].y = y + 165 - (50 - dx * dx / 20);
            e->npts++;
        }

        Annot *f = &g_ann[g_annCount++];
        ZeroMemory(f, sizeof(*f));
        f->kind = AN_TEXT;
        f->color = g_palette[0];
        f->fontH = g_textH[1];
        f->x = x + 20;
        f->y = y + 212;
        lstrcpynW(f->text, L"矩形 椭圆 箭头 马赛克", 256);

        Annot *g = &g_ann[g_annCount++];
        ZeroMemory(g, sizeof(*g));
        g->kind = AN_NUMBER;
        g->color = g_palette[3];
        g->x = x + 340;
        g->y = y + 250;
        g->fontH = g_numD[1];
        lstrcpynW(g->text, L"1", 256);
    }
    printf("annots: %d\n", g_annCount);

    void *bits = NULL;
    HBITMAP hbm = BuildFinalBitmap(x, y, w, h, &bits);
    if (!hbm || !bits) { printf("FAIL: build bitmap\n"); FreeCapture(); return 1; }
    printf("crop: %d x %d\n", w, h);

    CopyDibToClipboard(bits, w, h);

    BOOL ok = SaveBitmapToPng(hbm, out);
    printf("png: %s -> %ls\n", ok ? "ok" : "FAIL", out);

    DeleteObject(hbm);
    FreeCapture();

    /* 验证热键能注册 */
    HWND tmp = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, NULL, NULL);
    BOOL hk = tmp ? RegisterHotKey(tmp, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'A') : FALSE;
    printf("hotkey Ctrl+Alt+A: %s\n", hk ? "ok" : "occupied");
    if (hk) UnregisterHotKey(tmp, HOTKEY_ID);
    if (tmp) DestroyWindow(tmp);

    return ok ? 0 : 1;
}

/* 离屏渲染工具条，用于核对布局和图标 */
static int RunToolbarTest(void)
{
    g_ov.vx = 0; g_ov.vy = 0;
    g_ov.vw = 1600; g_ov.vh = 1200;
    g_ov.sel.left = 600;  g_ov.sel.top = 400;
    g_ov.sel.right = 1200; g_ov.sel.bottom = 800;
    g_ov.hasSel = TRUE;
    g_ov.state = ST_SELECTED;
    g_ov.hotBtn = -1;
    g_ov.hotSwatch = 3;            /* 蓝色块显示成悬停态 */
    g_ov.hotSize   = 2;            /* "粗"显示成悬停态 */
    g_ov.hotPin    = FALSE;
    g_tool = TOOL_MOSAIC;          /* 让马赛克按钮显示成激活态 */
    g_annColor = g_palette[1];     /* 黄色色块显示成选中态 */

    OverlayGdiInit();

    RECT t = ToolbarRect();
    printf("toolbar: (%ld,%ld)-(%ld,%ld)  %ldx%ld\n",
           t.left, t.top, t.right, t.bottom, t.right - t.left, t.bottom - t.top);

    int bad = 0;
    for (int i = 0; i < TB_NBTN; i++) {
        RECT b = BtnRect(i);
        printf("  btn%-2d %3ld..%3ld\n", i, b.left - t.left, b.right - t.left);
        if (b.left < t.left || b.right > t.right) bad++;
    }
    for (int i = 0; i < PALETTE_N; i++) {
        RECT s = SwatchRect(i);
        printf("  sw %d  %3ld..%3ld\n", i, s.left - t.left, s.right - t.left);
        if (s.left < t.left || s.right > t.right) bad++;
    }
    for (int i = 0; i < 3; i++) {
        RECT s = SizeRect(i);
        printf("  size%d %3ld..%3ld\n", i, s.left - t.left, s.right - t.left);
        if (s.left < t.left || s.right > t.right) bad++;
    }
    {
        RECT s = PinRect();
        printf("  pin   %3ld..%3ld\n", s.left - t.left, s.right - t.left);
        if (s.left < t.left || s.right > t.right) bad++;
    }
    printf("out-of-bounds: %d\n", bad);

    /* 光标判定：按钮和色块都必须是手型，工具条空白处必须是箭头 */
    HCURSOR hHand  = LoadCursorW(NULL, IDC_HAND);
    HCURSOR hArrow = LoadCursorW(NULL, IDC_ARROW);
    int curBad = 0;
    for (int i = 0; i < TB_NBTN; i++) {
        RECT b = BtnRect(i);
        HCURSOR c = OverlayCursorAt((b.left + b.right) / 2, (b.top + b.bottom) / 2);
        printf("  cur btn%-2d %s\n", i, c == hHand ? "hand" : "WRONG");
        if (c != hHand) curBad++;
    }
    for (int i = 0; i < PALETTE_N; i++) {
        RECT s = SwatchRect(i);
        HCURSOR c = OverlayCursorAt((s.left + s.right) / 2, (s.top + s.bottom) / 2);
        printf("  cur sw %d  %s\n", i, c == hHand ? "hand" : "WRONG");
        if (c != hHand) curBad++;
    }
    for (int i = 0; i < 3; i++) {
        RECT s = SizeRect(i);
        HCURSOR c = OverlayCursorAt((s.left + s.right) / 2, (s.top + s.bottom) / 2);
        printf("  cur size%d %s\n", i, c == hHand ? "hand" : "WRONG");
        if (c != hHand) curBad++;
    }
    {
        RECT s = PinRect();
        HCURSOR c = OverlayCursorAt((s.left + s.right) / 2, (s.top + s.bottom) / 2);
        printf("  cur pin   %s\n", c == hHand ? "hand" : "WRONG");
        if (c != hHand) curBad++;
    }
    {
        HCURSOR c = OverlayCursorAt(t.left + 2, t.top + 4);   /* 首个按钮之前的空白 */
        printf("  cur gap    %s\n", c == hArrow ? "arrow" : "WRONG");
        if (c != hArrow) curBad++;
    }
    printf("cursor mismatches: %d\n", curBad);

    int w = TB_W, h = TB_H;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP hbm = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!hbm) { printf("FAIL: dib\n"); OverlayGdiFree(); return 1; }

    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old = NULL;
    if (!dc || !GdiSelect(dc, hbm, &old)) {
        if (dc) DeleteDC(dc);
        DeleteObject(hbm);
        OverlayGdiFree();
        printf("FAIL: dc\n");
        return 1;
    }

    RECT full = { 0, 0, w, h };
    FillRect(dc, &full, (HBRUSH)GetStockObject(GRAY_BRUSH));

    SetViewportOrgEx(dc, -t.left, -t.top, NULL);
    DrawToolbar(dc);
    SetViewportOrgEx(dc, 0, 0, NULL);

    GdiRestore(dc, old, "toolbar test bitmap");
    DeleteDC(dc);

    WCHAR out[MAX_PATH];
    MakeSiblingPath(L"toolbar.png", out, MAX_PATH);
    BOOL ok = SaveBitmapToPng(hbm, out);
    DeleteObject(hbm);
    OverlayGdiFree();
    printf("png: %s -> %ls\n", ok ? "ok" : "FAIL", out);
    return (ok && bad == 0 && curBad == 0) ? 0 : 1;
}

/* ---------------------------------------------------------------- 稳定性自检 */

/* 用 --stress-test 触发：覆盖连续截图、文字编辑、马赛克、剪贴板、
 * 文件命名、快捷键冲突和退出清理，并对比操作前后的 GDI 对象数量。 */

static int g_testFail = 0;

static void TestCheck(BOOL ok, const char *what)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) g_testFail++;
}

static DWORD GdiCount(void)
{
    return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

static COLORREF TopDownPixel(const void *bits, int w, int x, int y)
{
    const BYTE *p = (const BYTE *)bits + ((size_t)y * (size_t)w + (size_t)x) * 4;
    return RGB(p[2], p[1], p[0]);
}

static COLORREF BottomUpPixel(const void *bits, int w, int h, int x, int y)
{
    const BYTE *p = (const BYTE *)bits + ((size_t)(h - 1 - y) * (size_t)w + (size_t)x) * 4;
    return RGB(p[2], p[1], p[0]);
}

/* 离屏渲染一份"遮罩预览"，像素布局和 g_ov 的 top-down 缓冲一致 */
static HBITMAP RenderOverlayToDib(void **ppBits)
{
    if (ppBits) *ppBits = NULL;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_ov.vw;
    bi.bmiHeader.biHeight      = -g_ov.vh;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) { if (hbm) DeleteObject(hbm); return NULL; }

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) { DeleteObject(hbm); return NULL; }

    HGDIOBJ old = NULL;
    if (!GdiSelect(dc, hbm, &old)) {
        DeleteDC(dc);
        DeleteObject(hbm);
        return NULL;
    }
    OverlayDraw(dc);
    GdiRestore(dc, old, "overlay render bitmap");
    DeleteDC(dc);

    if (ppBits) *ppBits = bits;
    return hbm;
}

static void TestCaptureLoop(void)
{
    DWORD g0 = GdiCount();
    int fails = 0;
    ULONGLONG t0 = GetTickCount64();
    for (int i = 0; i < 1000; i++) {
        if (!CaptureVirtualScreen()) fails++;
        FreeCapture();
    }
    ULONGLONG dt = GetTickCount64() - t0;
    DWORD g1 = GdiCount();
    printf("capture: 1000x %llu ms, gdi %lu -> %lu, fails=%d\n",
           (unsigned long long)dt, g0, g1, fails);
    TestCheck(fails == 0, "capture 1000 times");
    TestCheck(g1 <= g0 + 4, "capture loop gdi stable");
}

static void TestTextLifecycle(HWND parent)
{
    g_ov.hwnd = parent;
    g_ov.vx = 0; g_ov.vy = 0;
    g_ov.vw = 640; g_ov.vh = 480;
    g_ov.hasSel = TRUE;
    g_ov.state = ST_IDLE;
    g_ov.sel.left = 0; g_ov.sel.top = 0;
    g_ov.sel.right = 640; g_ov.sel.bottom = 480;

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) { TestCheck(FALSE, "text: alloc buffers"); return; }

    /* 预热：EDIT 控件第一次创建时系统会分配一次性的 GDI 缓存，
     * 预热之后再看增量，才能区分"一次性缓存"和"每次泄漏" */
    for (int i = 0; i < 50; i++) {
        g_annCount = 0;
        TextStart(20, 20);
        TextCancel();
    }

    DWORD g0 = GdiCount();
    int fails = 0;
    for (int i = 0; i < 1000; i++) {
        g_annCount = 0;
        TextStart(20 + (i % 40), 20 + (i % 30));
        if (!g_edit) { fails++; continue; }
        if (i % 3 == 0) {
            SetWindowTextW(g_edit, L"文本标注");
            TextCommit();
        } else {
            TextCancel();
        }
        if (g_edit || g_editFont) fails++;    /* 提交/取消后不能残留 */
    }
    g_annCount = 0;
    DWORD g1 = GdiCount();
    printf("text: 1000x commit/cancel, gdi %lu -> %lu, fails=%d\n", g0, g1, fails);
    TestCheck(fails == 0, "text create/commit/cancel 1000x");
    TestCheck(g1 <= g0 + 2, "text loop gdi stable");

    /* 编辑状态下直接退出：控件和字体都要回收 */
    TextStart(30, 30);
    BOOL hadEdit = (g_edit != NULL);
    OverlayReleaseAll();
    TestCheck(hadEdit && !g_edit && !g_editFont, "teardown with editor open");
    g_ov.hwnd = NULL;
}

static void FillTestPattern(void)
{
    BYTE *p = (BYTE *)g_ov.pixCap;
    if (!p) return;
    for (int y = 0; y < g_ov.vh; y++) {
        for (int x = 0; x < g_ov.vw; x++) {
            BYTE *q = p + ((size_t)y * (size_t)g_ov.vw + (size_t)x) * 4;
            q[0] = (BYTE)(x * 255 / g_ov.vw);
            q[1] = (BYTE)(y * 255 / g_ov.vh);
            q[2] = (BYTE)((x + y) & 0xFF);
            q[3] = 255;
        }
    }
}

/* 逐像素比较预览图与导出图的重合区域。
 * preview 是遮罩客户区大小的 top-down DIB，export 是选区的 bottom-up DIB。
 * 传入的 area 已避开选框边框/手柄/尺寸标签那几像素（它们是界面装饰，
 * 本来就不属于导出内容），区域内所有像素必须完全一致。 */
static void ComparePreviewVsExport(const void *pbits, const void *ebits,
                                   int ew, int eh, RECT area,
                                   int offX, int offY, const char *what)
{
    size_t total = 0, diff = 0;
    if (pbits && ebits) {
        for (int y = area.top; y < area.bottom; y++) {
            for (int x = area.left; x < area.right; x++) {
                COLORREF pv = TopDownPixel(pbits, g_ov.vw, x, y);
                COLORREF ex = BottomUpPixel(ebits, ew, eh, x - offX, y - offY);
                total++;
                if (pv != ex) diff++;
            }
        }
    }
    printf("%s: compared %llu px, diff %llu (%.4f%%)\n", what,
           (unsigned long long)total, (unsigned long long)diff,
           total ? (double)diff * 100.0 / (double)total : 0.0);
    TestCheck(total > 0 && diff == 0, what);
}

static void TestMosaic(HWND parent)
{
    g_ov.hwnd = parent;
    g_ov.vx = 0; g_ov.vy = 0;
    g_ov.vw = 640; g_ov.vh = 480;
    g_ov.hasSel = TRUE;
    g_ov.state = ST_SELECTED;
    /* 选区下移：尺寸标签会落在选区上方而不进入画面内容；
     * 比对区域再向内缩 6px，避开 1px 选框和 8 个手柄 */
    g_ov.sel.left = 10;  g_ov.sel.top = 60;
    g_ov.sel.right = 620; g_ov.sel.bottom = 400;
    g_tool = TOOL_MOVE;

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) { TestCheck(FALSE, "mosaic: alloc buffers"); return; }
    FillTestPattern();
    if (!OvDimBuffers()) {
        TestCheck(FALSE, "mosaic: dim buffers");
        FreeCapture();
        g_ov.hwnd = NULL;
        return;
    }
    MosaicEpochBump();

    /* 三条马赛克，全部落在比对区域内 */
    static const RECT rects[3] = {
        {  40,  80, 180, 180 },
        { 200,  80, 320, 160 },
        { 360,  80, 480, 180 },
    };
    g_annCount = 0;
    for (int i = 0; i < 3; i++) {
        Annot *a = &g_ann[g_annCount++];
        ZeroMemory(a, sizeof(*a));
        a->kind = AN_MOSAIC;
        a->rc   = rects[i];
    }

    /* 1) 有缓存：重复重绘应该很便宜。先预热一次把缓存建起来，
     *    这样后面测到的增量只是"每次重绘额外创建"的对象。 */
    HBITMAP prv = RenderOverlayToDib(NULL);
    void   *pbits = NULL;
    if (prv) { DeleteObject(prv); prv = NULL; }
    prv = RenderOverlayToDib(&pbits);

    DWORD g0 = GdiCount();
    for (int i = 0; i < 300; i++) {
        if (prv) { DeleteObject(prv); prv = NULL; }
        prv = RenderOverlayToDib(&pbits);
    }
    DWORD g1 = GdiCount();
    TestCheck(prv != NULL, "mosaic 300 redraws render");
    TestCheck(g1 <= g0 + 2, "mosaic redraw gdi stable");

    /* 2) 只对标注绘制做微基准：整窗拷贝会淹没马赛克自身的开销。
     *    先测缓存命中，再强制每帧重算作为缓存前的基线。 */
    BITMAPINFO dbi;
    ZeroMemory(&dbi, sizeof(dbi));
    dbi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    dbi.bmiHeader.biWidth       = g_ov.vw;
    dbi.bmiHeader.biHeight      = -g_ov.vh;
    dbi.bmiHeader.biPlanes      = 1;
    dbi.bmiHeader.biBitCount    = 32;
    dbi.bmiHeader.biCompression = BI_RGB;

    void   *dbits = NULL;
    HBITMAP dhbm = CreateDIBSection(NULL, &dbi, DIB_RGB_COLORS, &dbits, NULL, 0);
    HDC     ddc  = dhbm ? CreateCompatibleDC(NULL) : NULL;
    HGDIOBJ dold = NULL;
    if (ddc && !GdiSelect(ddc, dhbm, &dold)) { DeleteDC(ddc); ddc = NULL; }

    ULONGLONG dtCached = 0, dtDirect = 0;
    if (ddc) {
        DrawAnnotations(ddc);                       /* 先建缓存 */
        ULONGLONG t0 = NowUsec();
        for (int i = 0; i < 500; i++) DrawAnnotations(ddc);
        dtCached = NowUsec() - t0;

        ULONGLONG t1 = NowUsec();
        for (int i = 0; i < 500; i++) {
            MosaicEpochBump();
            DrawAnnotations(ddc);
        }
        dtDirect = NowUsec() - t1;

        GdiRestore(ddc, dold, "mosaic bench bitmap");
        DeleteDC(ddc);
    }
    if (dhbm) DeleteObject(dhbm);
    printf("mosaic: annots 500x cached %llu us, 500x uncached %llu us\n",
           (unsigned long long)dtCached, (unsigned long long)dtDirect);
    /* 用微秒计时：GetTickCount64 的 15.6ms 粒度会让两条路径都落在同一刻度上，
     * 造成随机误判。缓存命中本就不该比强制重算慢。 */
    TestCheck(dtCached <= dtDirect, "mosaic cache is not slower than recompute");

    /* 3) 预览 vs 导出：选区内容逐像素比较，不做抽样。
     *    先让导出路径自己重算一遍（不复用预览缓存），再整块比对。 */
    int sw = g_ov.sel.right - g_ov.sel.left;
    int sh = g_ov.sel.bottom - g_ov.sel.top;
    void *ebits = NULL;
    MosaicEpochBump();
    HBITMAP exp = BuildFinalBitmap(g_ov.sel.left, g_ov.sel.top, sw, sh, &ebits);
    if (prv && pbits && exp && ebits) {
        RECT area = { g_ov.sel.left + 6, g_ov.sel.top + 6,
                      g_ov.sel.right - 6, g_ov.sel.bottom - 6 };
        ComparePreviewVsExport(pbits, ebits, sw, sh, area,
                               g_ov.sel.left, g_ov.sel.top,
                               "mosaic preview == export (all pixels)");
    } else {
        TestCheck(FALSE, "mosaic preview == export (all pixels)");
    }
    if (exp) DeleteObject(exp);

    /* 4) 撤销最后一条马赛克，原区域要还原成底图 */
    UndoOne();
    void *pbits2 = NULL;
    HBITMAP prv2 = RenderOverlayToDib(&pbits2);
    COLORREF after = 0, base = 0;
    if (prv2 && pbits2 && g_ov.pixCap) {
        after = TopDownPixel(pbits2, g_ov.vw, 400, 120);
        base  = TopDownPixel(g_ov.pixCap, g_ov.vw, 400, 120);
    }
    TestCheck(prv2 && after == base, "mosaic undo restores base pixels");
    if (prv)  DeleteObject(prv);
    if (prv2) DeleteObject(prv2);

    g_annCount = 0;
    FreeCapture();
    g_ov.hwnd = NULL;
}

/* 测试用离屏画布：一块 top-down 32bpp DIB + 配套 DC */
typedef struct { HDC dc; HBITMAP hbm; HGDIOBJ old; void *bits; } TestSurface;

static void TestSurfaceDestroy(TestSurface *s)
{
    if (s->dc && s->old) GdiRestore(s->dc, s->old, "test surface bitmap");
    if (s->dc) {
        DeleteDC(s->dc);
        GdiFlushDeferred();         /* DC 已销毁，挂起对象可释放 */
    }
    if (s->hbm) DeleteObject(s->hbm);
    ZeroMemory(s, sizeof(*s));
}

static BOOL TestSurfaceCreate(TestSurface *s, int w, int h)
{
    ZeroMemory(s, sizeof(*s));

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s->hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &s->bits, NULL, 0);
    if (!s->hbm || !s->bits) goto fail;
    s->dc = CreateCompatibleDC(NULL);
    if (!s->dc) goto fail;
    s->old = NULL;
    if (!GdiSelect(s->dc, s->hbm, &s->old)) goto fail;
    return TRUE;

fail:
    TestSurfaceDestroy(s);
    return FALSE;
}

/* 回归：拖动中被丢弃的窄马赛克不能留下可命中的缓存 */
static void TestMosaicDiscard(HWND parent)
{
    g_ov.hwnd = parent;
    g_ov.vx = 0; g_ov.vy = 0;
    g_ov.vw = 640; g_ov.vh = 480;
    g_ov.hasSel = TRUE;
    g_ov.state = ST_SELECTED;
    g_ov.sel.left = 10;  g_ov.sel.top = 60;
    g_ov.sel.right = 620; g_ov.sel.bottom = 400;
    g_tool = TOOL_MOSAIC;

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) { TestCheck(FALSE, "discard: alloc buffers"); return; }
    FillTestPattern();
    if (!OvDimBuffers()) {
        TestCheck(FALSE, "discard: dim buffers");
        FreeCapture();
        g_ov.hwnd = NULL;
        g_tool = TOOL_MOVE;
        return;
    }
    g_annCount = 0;
    MosaicEpochBump();
    unsigned epoch0 = g_mosaicEpoch;

    /* 走真实的鼠标消息链路：2px 宽的马赛克在拖动时会建缓存，
     * 松手因为太窄被丢弃 */
    SendMessageW(parent, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
    SendMessageW(parent, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(102, 110));
    SendMessageW(parent, WM_LBUTTONUP, 0, MAKELPARAM(102, 110));

    printf("discard: annots=%d, epoch %u -> %u\n", g_annCount, epoch0, g_mosaicEpoch);
    TestCheck(g_annCount == 0, "narrow mosaic discarded on mouse up");
    TestCheck(g_mosaicEpoch != epoch0, "discard invalidates mosaic cache");

    /* 同一下标再建一个正常马赛克：结果必须来自当前底图 */
    SendMessageW(parent, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 100));
    SendMessageW(parent, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(180, 170));
    SendMessageW(parent, WM_LBUTTONUP, 0, MAKELPARAM(180, 170));
    TestCheck(g_annCount == 1 && g_ann[0].kind == AN_MOSAIC, "mosaic recreated at same index");

    void *pbits = NULL, *ebits = NULL;
    HBITMAP prv = RenderOverlayToDib(&pbits);
    int sw = g_ov.sel.right - g_ov.sel.left;
    int sh = g_ov.sel.bottom - g_ov.sel.top;
    MosaicEpochBump();                     /* 导出不复用预览缓存 */
    HBITMAP exp = BuildFinalBitmap(g_ov.sel.left, g_ov.sel.top, sw, sh, &ebits);
    if (prv && pbits && exp && ebits) {
        RECT area = { g_ov.sel.left + 6, g_ov.sel.top + 6,
                      g_ov.sel.right - 6, g_ov.sel.bottom - 6 };
        ComparePreviewVsExport(pbits, ebits, sw, sh, area,
                               g_ov.sel.left, g_ov.sel.top,
                               "discard regression matches current base");
    } else {
        TestCheck(FALSE, "discard regression matches current base");
    }
    if (prv) DeleteObject(prv);
    if (exp) DeleteObject(exp);

    if (GetCapture() == parent) ReleaseCapture();
    g_annCount = 0;
    FreeCapture();
    g_ov.hwnd = NULL;
    g_tool = TOOL_MOVE;
}

/* 缓存预算：扩容 / 缩容 / 超限 / 多区域 / 释放归零 / scratch 不计费 */
static void TestMosaicBudget(void)
{
    g_ov.sel.left = 0;    g_ov.sel.top = 0;
    g_ov.sel.right = 1600; g_ov.sel.bottom = 1200;
    g_ov.hasSel = TRUE;

    MosaicCacheFreeAll();
    TestCheck(g_mosaicCachedPixels == 0, "budget: starts at zero");

    TestSurface s;
    if (!TestSurfaceCreate(&s, 1600, 1200)) { TestCheck(FALSE, "budget: surface"); return; }

    g_annCount = 0;
    Annot *a = &g_ann[g_annCount++];
    ZeroMemory(a, sizeof(*a));
    a->kind = AN_MOSAIC;

    /* 1) 首次 100x100 */
    a->rc.left = 10;  a->rc.top = 10;
    a->rc.right = 110; a->rc.bottom = 110;
    DrawAnnotations(s.dc);
    TestCheck(g_mosaicCachedPixels == 10000 && g_mosaicCache[0].capW == 100,
              "budget: first cache billed 100x100");

    /* 2) 扩到 300x200：重新分配，按新尺寸计费 */
    a->rc.right = 310; a->rc.bottom = 210;
    DrawAnnotations(s.dc);
    TestCheck(g_mosaicCachedPixels == 60000 && g_mosaicCache[0].capW == 300,
              "budget: grow bills new size");

    /* 3) 缩到 150x100：复用更大的画布，不重复计费 */
    a->rc.right = 160; a->rc.bottom = 110;
    DrawAnnotations(s.dc);
    TestCheck(g_mosaicCachedPixels == 60000 && g_mosaicCache[0].capW == 300,
              "budget: shrink reuses canvas without extra billing");

    /* 4) 多区域：13 条 1200x1000 能进预算，第 14 条超预算退回 scratch */
    g_annCount = 0;
    int cached = 0;
    for (int i = 0; i < 14; i++) {
        Annot *m = &g_ann[g_annCount++];
        ZeroMemory(m, sizeof(*m));
        m->kind = AN_MOSAIC;
        m->rc.left = i * 5;  m->rc.top = i * 5;
        m->rc.right = m->rc.left + 1200;
        m->rc.bottom = m->rc.top + 1000;
        DrawAnnotations(s.dc);
        if (g_mosaicCache[i].capW == 1200) cached++;
    }
    printf("budget: %d of 14 large mosaics cached, pixels=%llu, scratch.capW=%d\n",
           cached, (unsigned long long)g_mosaicCachedPixels, g_mosaicScratch.capW);
    TestCheck(cached == 13, "budget: 13 large regions fit, 14th rejected");
    TestCheck(g_mosaicCachedPixels == (size_t)cached * 1200u * 1000u,
              "budget: accounting matches cached slots");
    TestCheck(g_mosaicScratch.capW == 1200, "budget: over-budget region uses scratch");
    TestCheck(g_mosaicCachedPixels <= (size_t)MOSAIC_CACHE_BUDGET,
              "budget: total never exceeds limit");
    /* scratch 已占 1200x1000，但上面的等式只统计正式槽位，
     * 说明 scratch 没有混进预算 */

    /* 5) 全部释放后统计归零 */
    MosaicCacheFreeAll();
    TestCheck(g_mosaicCachedPixels == 0, "budget: free all returns to zero");
    TestCheck(g_mosaicScratch.capW == 0, "budget: scratch released");

    /* 6) 单条超过 8M 像素：不缓存，只用 scratch，不计费 */
    TestSurface big;
    if (TestSurfaceCreate(&big, 2900, 2900)) {
        g_annCount = 0;
        Annot *m = &g_ann[g_annCount++];
        ZeroMemory(m, sizeof(*m));
        m->kind = AN_MOSAIC;
        m->rc.left = 0; m->rc.top = 0;
        m->rc.right = 2900; m->rc.bottom = 2900;     /* 8.41M 像素 > 8M 上限 */
        DrawAnnotations(big.dc);
        printf("budget: oversize -> slot.capW=%d scratch.capW=%d pixels=%llu\n",
               g_mosaicCache[0].capW, g_mosaicScratch.capW,
               (unsigned long long)g_mosaicCachedPixels);
        TestCheck(g_mosaicCache[0].capW == 0, "budget: single region over 8M not cached");
        TestCheck(g_mosaicScratch.capW == 2900, "budget: oversize single region uses scratch");
        TestCheck(g_mosaicCachedPixels == 0, "budget: scratch is not billed");
        TestSurfaceDestroy(&big);
    } else {
        printf("[SKIP] budget: 2900x2900 surface unavailable\n");
    }

    TestSurfaceDestroy(&s);
    g_annCount = 0;
    MosaicCacheFreeAll();
    TestCheck(g_mosaicCachedPixels == 0, "budget: zero after cleanup");
}

static void TestClipboard(void)
{
    int w = 64, h = 48;
    BYTE *bits = (BYTE *)calloc((size_t)w * h * 4, 1);
    if (!bits) { TestCheck(FALSE, "clipboard: alloc"); return; }

    TestCheck(CopyDibToClipboard(bits, w, h), "clipboard copy 64x48");
    TestCheck(!CopyDibToClipboard(bits, 0, 10), "clipboard rejects w=0");
    TestCheck(!CopyDibToClipboard(bits, -4, 10), "clipboard rejects w<0");
    TestCheck(!CopyDibToClipboard(bits, 1 << 20, 1 << 20), "clipboard rejects huge size");
    TestCheck(!CopyDibToClipboard(NULL, 10, 10), "clipboard rejects NULL bits");

    free(bits);
}

/* 另一个线程占住剪贴板，模拟"剪贴板被其他程序占用" */
static HANDLE g_clipHold, g_clipRelease;
static volatile LONG g_clipHeld = 0;

static DWORD WINAPI ClipboardBlockerThread(LPVOID unused)
{
    (void)unused;
    /* 外部剪贴板管理器可能短暂占用，重试几次再判定失败 */
    for (int i = 0; i < 20; i++) {
        if (OpenClipboard(NULL)) {
            InterlockedExchange(&g_clipHeld, 1);
            SetEvent(g_clipHold);
            WaitForSingleObject(g_clipRelease, INFINITE);
            CloseClipboard();
            return 0;
        }
        Sleep(20);
    }
    SetEvent(g_clipHold);
    return 0;
}

static void TestClipboardBusy(void)
{
    g_clipHold    = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_clipRelease = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&g_clipHeld, 0);

    HANDLE th = CreateThread(NULL, 0, ClipboardBlockerThread, NULL, 0, NULL);
    if (!th || !g_clipHold || !g_clipRelease) {
        if (th) CloseHandle(th);
        if (g_clipHold) CloseHandle(g_clipHold);
        if (g_clipRelease) CloseHandle(g_clipRelease);
        g_clipHold = g_clipRelease = NULL;
        TestCheck(FALSE, "clipboard busy: setup");
        return;
    }
    WaitForSingleObject(g_clipHold, 3000);

    if (InterlockedCompareExchange(&g_clipHeld, 1, 1) == 1) {
        BYTE *bits = (BYTE *)calloc(32 * 32 * 4, 1);
        BOOL ok = CopyDibToClipboard(bits, 32, 32);
        free(bits);
        TestCheck(!ok, "clipboard busy: copy fails safely");
    } else {
        printf("[SKIP] clipboard busy: OpenClipboard failed in helper thread\n");
    }

    SetEvent(g_clipRelease);
    WaitForSingleObject(th, 3000);
    CloseHandle(th);
    CloseHandle(g_clipHold);
    CloseHandle(g_clipRelease);
    g_clipHold = g_clipRelease = NULL;
}

static void TestFileNames(void)
{
    WCHAR dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) { TestCheck(FALSE, "temp path"); return; }
    lstrcatW(dir, L"qs_stab_test");
    RemoveDirectoryW(dir);
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        TestCheck(FALSE, "create temp dir");
        return;
    }

    static WCHAR names[20][MAX_PATH * 2];
    int made = 0, dup = 0;
    for (int i = 0; i < 20; i++) {
        if (!BuildUniqueShotPath(dir, names[i], MAX_PATH * 2)) break;
        for (int j = 0; j < i; j++) if (!wcscmp(names[j], names[i])) dup++;
        made++;
    }
    printf("files: made=%d dup=%d\n", made, dup);
    TestCheck(made == 20, "20 shots in one second get unique names");
    TestCheck(dup == 0, "no duplicate file names");

    /* 已有文件不能被覆盖：再要一个名字必须和之前那 20 个都不同 */
    WCHAR again[MAX_PATH * 2];
    BOOL okAgain = BuildUniqueShotPath(dir, again, MAX_PATH * 2);
    int same = 0;
    if (okAgain) for (int i = 0; i < made; i++) if (!wcscmp(again, names[i])) same++;
    TestCheck(okAgain && same == 0, "existing files are not overwritten");
    if (okAgain) DeleteFileW(again);

    for (int i = 0; i < made; i++) DeleteFileW(names[i]);

    /* 目录不存在 / 不可写时必须安全失败 */
    WCHAR missing[MAX_PATH], p[MAX_PATH * 2];
    wsprintfW(missing, L"%s\\no_such_sub_dir", dir);
    TestCheck(!BuildUniqueShotPath(missing, p, MAX_PATH * 2), "missing dir rejected");

    RemoveDirectoryW(dir);
}

/* 用独立线程占住指定组合，模拟"被其他程序占用"。
 * 支持多个槽位，用于"所有预设都被占用"的场景。 */
#define HK_BLOCK_MAX 4

typedef struct {
    HANDLE ready, release, thread;
    UINT   mods, vk;
    volatile LONG ok;
} HotkeyBlock;

static HotkeyBlock g_hkBlocks[HK_BLOCK_MAX];

static DWORD WINAPI HotkeyBlockerThread(LPVOID param)
{
    HotkeyBlock *b = (HotkeyBlock *)param;
    HWND w = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, NULL, NULL);
    if (w && RegisterHotKey(w, 0xBEEF, b->mods, b->vk)) {
        InterlockedExchange(&b->ok, 1);
        SetEvent(b->ready);
        WaitForSingleObject(b->release, INFINITE);
        UnregisterHotKey(w, 0xBEEF);
    } else {
        SetEvent(b->ready);            /* 没占上，主线程会跳过该用例 */
    }
    if (w) DestroyWindow(w);
    return 0;
}

static void StopHotkeyBlocker(int slot)
{
    if (slot < 0 || slot >= HK_BLOCK_MAX) return;
    HotkeyBlock *b = &g_hkBlocks[slot];

    if (b->release) SetEvent(b->release);
    if (b->thread) {
        WaitForSingleObject(b->thread, 3000);
        CloseHandle(b->thread);
        b->thread = NULL;
    }
    if (b->ready)   { CloseHandle(b->ready);   b->ready = NULL; }
    if (b->release) { CloseHandle(b->release); b->release = NULL; }
    b->mods = b->vk = 0;
    InterlockedExchange(&b->ok, 0);
}

/* 启动一个占位线程；成功占住返回 TRUE */
static BOOL StartHotkeyBlocker(int slot, UINT mods, UINT vk)
{
    if (slot < 0 || slot >= HK_BLOCK_MAX) return FALSE;
    StopHotkeyBlocker(slot);

    HotkeyBlock *b = &g_hkBlocks[slot];
    b->mods    = mods;
    b->vk      = vk;
    b->ready   = CreateEventW(NULL, TRUE, FALSE, NULL);
    b->release = CreateEventW(NULL, TRUE, FALSE, NULL);
    InterlockedExchange(&b->ok, 0);
    if (!b->ready || !b->release) return FALSE;

    b->thread = CreateThread(NULL, 0, HotkeyBlockerThread, b, 0, NULL);
    if (!b->thread) return FALSE;

    WaitForSingleObject(b->ready, 3000);
    return (InterlockedCompareExchange(&b->ok, 1, 1) == 1);
}

/* 探测某个组合当前是否可注册（用于避免触发会阻塞的 MessageBox） */
static BOOL HotkeyComboFree(UINT mods, UINT vk)
{
    HWND w = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, NULL, NULL);
    if (!w) return FALSE;
    BOOL ok = RegisterHotKey(w, 0xBEEA, mods, vk);
    if (ok) UnregisterHotKey(w, 0xBEEA);
    DestroyWindow(w);
    return ok;
}

static void TestHotkey(void)
{
    g_hotkeyId = 0;
    g_hotkeyOk = FALSE;
    g_mods = MOD_CONTROL | MOD_ALT;
    g_vk   = VK_F9;

    /* 用功能键而不是 Ctrl+Alt+A：避免和正在运行的真实 QuickShot 实例抢热键，
     * 否则测试会因环境占用而失败（测的是切换逻辑，不是某个具体组合）。 */
    if (!HotkeyTryApply(g_mods, g_vk)) {
        g_vk = VK_F10;
        if (!HotkeyTryApply(g_mods, g_vk)) {
            TestCheck(FALSE, "hotkey: initial register");
            return;
        }
    }
    UINT idBefore = g_hotkeyId;
    UINT aM = g_mods, aV = g_vk;

    if (!StartHotkeyBlocker(0, MOD_CONTROL | MOD_ALT, VK_F11)) {
        StopHotkeyBlocker(0);
        HotkeyUnregisterActive();
        TestCheck(!g_hotkeyOk && g_hotkeyId == 0, "hotkey unregistered");
        return;
    }

    /* 新组合被占用：必须失败，且旧热键、旧配置都不能动 */
    BOOL switched = HotkeyTryApply(MOD_CONTROL | MOD_ALT, VK_F11);
    TestCheck(!switched, "occupied hotkey rejected");
    TestCheck(g_mods == aM && g_vk == aV && g_hotkeyId == idBefore,
              "old hotkey kept after failed switch");

    TestCheck(HotkeyTryApply(MOD_CONTROL | MOD_SHIFT, VK_F12), "switch to free hotkey works");

    StopHotkeyBlocker(0);
    TestCheck(HotkeyTryApply(MOD_CONTROL | MOD_ALT, VK_F11), "released combo can be applied");

    HotkeyUnregisterActive();
    TestCheck(!g_hotkeyOk && g_hotkeyId == 0, "hotkey unregistered");
}

/* ---- 测试用的注册表读写（仅在自检里使用，测试结束会还原） ---- */

static BOOL RegReadDword(const WCHAR *name, DWORD *out)
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v), type = 0;
    BOOL ok = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &sz) == ERROR_SUCCESS &&
            type == REG_DWORD && sz == sizeof(DWORD)) {
            *out = v;
            ok = TRUE;
        }
        RegCloseKey(k);
    }
    return ok;
}

static void RegDeleteDword(const WCHAR *name)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_PATH, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, name);
        RegCloseKey(k);
    }
}

typedef struct { BOOL hadMods, hadVk; DWORD mods, vk; } RegBackup;

static void RegBackupHotkey(RegBackup *b)
{
    b->hadMods = RegReadDword(L"HotMods", &b->mods);
    b->hadVk   = RegReadDword(L"HotVk", &b->vk);
}

static void RegRestoreHotkey(const RegBackup *b)
{
    if (b->hadMods) SaveSettingDword(L"HotMods", b->mods);
    else            RegDeleteDword(L"HotMods");
    if (b->hadVk)   SaveSettingDword(L"HotVk", b->vk);
    else            RegDeleteDword(L"HotVk");
}

/* 启动恢复：保存的组合被占用时自动回退到其它预设，且不写注册表 */
static void TestHotkeyStartupFallback(void)
{
    const UINT savedM = MOD_CONTROL | MOD_SHIFT;
    const UINT savedV = 'A';               /* 预设 3，故意占住它 */

    RegBackup bk;
    RegBackupHotkey(&bk);
    SaveSettingDword(L"HotMods", savedM);  /* 让注册表与"保存的配置"一致 */
    SaveSettingDword(L"HotVk", savedV);

    g_hotkeyId = 0;
    g_hotkeyOk = FALSE;
    g_mods = savedM;
    g_vk   = savedV;

    BOOL blocked = StartHotkeyBlocker(0, savedM, savedV);
    int r = HotkeyRegisterOnStartup();
    printf("startup hotkey: blocked=%d result=%d mods=0x%X vk=0x%X\n",
           blocked, r, g_mods, g_vk);

    if (blocked) {
        TestCheck(r == HK_START_FALLBACK, "startup falls back when saved hotkey is occupied");
        TestCheck(g_hotkeyOk && !(g_mods == savedM && g_vk == savedV),
                  "fallback differs from saved combo");
        TestCheck(g_mods == (UINT)(MOD_CONTROL | MOD_ALT) ||
                  g_mods == (UINT)(MOD_ALT | MOD_SHIFT),
                  "fallback comes from preset list");

        DWORD rm = 0, rv = 0;
        TestCheck(RegReadDword(L"HotMods", &rm) && rm == savedM &&
                  RegReadDword(L"HotVk", &rv) && rv == savedV,
                  "fallback keeps the saved config in registry");
    } else {
        printf("[SKIP] startup fallback: Ctrl+Shift+A already taken by another app\n");
    }
    StopHotkeyBlocker(0);
    HotkeyUnregisterActive();
    TestCheck(!g_hotkeyOk && g_hotkeyId == 0, "startup hotkey released");

    /* 原组合释放后再启动一次：应优先回到用户保存的组合 */
    g_hotkeyId = 0;
    g_hotkeyOk = FALSE;
    g_mods = savedM;
    g_vk   = savedV;
    int r2 = HotkeyRegisterOnStartup();
    TestCheck(r2 == HK_START_SAVED && g_hotkeyOk &&
              g_mods == savedM && g_vk == savedV,
              "next start prefers the saved combo once it is free");
    HotkeyUnregisterActive();

    /* 保存的组合空闲时，启动应直接用它，不做无谓回退 */
    g_hotkeyId = 0;
    g_hotkeyOk = FALSE;
    g_mods = MOD_CONTROL | MOD_ALT;
    g_vk   = VK_F9;
    int r3 = HotkeyRegisterOnStartup();
    TestCheck(r3 == HK_START_SAVED && g_hotkeyOk, "startup uses saved hotkey when free");
    HotkeyUnregisterActive();

    RegRestoreHotkey(&bk);
}

/* 所有预设都被占用：程序仍能启动，托盘菜单仍能切换并持久化 */
static void TestHotkeyAllOccupied(void)
{
    RegBackup bk;
    RegBackupHotkey(&bk);

    /* 哨兵值：回退/失败都不能动它 */
    SaveSettingDword(L"HotMods", MOD_CONTROL | MOD_SHIFT);
    SaveSettingDword(L"HotVk", 'A');

    int blockedAll = 0;
    for (int i = 0; i < PRESET_COUNT && i < HK_BLOCK_MAX; i++) {
        if (StartHotkeyBlocker(i, g_presets[i].mods, g_presets[i].vk)) blockedAll++;
    }

    g_hotkeyId = 0;
    g_hotkeyOk = FALSE;
    g_mods = MOD_CONTROL | MOD_SHIFT;      /* 保存的是预设 3 */
    g_vk   = 'A';

    int r = HotkeyRegisterOnStartup();
    printf("startup all occupied: blocked=%d/%d result=%d hotkeyOk=%d\n",
           blockedAll, PRESET_COUNT, r, (int)g_hotkeyOk);

    if (blockedAll == PRESET_COUNT) {
        TestCheck(r == HK_START_NONE && !g_hotkeyOk,
                  "startup survives when every preset is occupied");

        DWORD rm = 0, rv = 0;
        TestCheck(RegReadDword(L"HotMods", &rm) && rm == (MOD_CONTROL | MOD_SHIFT) &&
                  RegReadDword(L"HotVk", &rv) && rv == (DWORD)'A',
                  "failed startup leaves registry untouched");

        /* 释放预设 1（Ctrl+Alt+A）后，模拟用户从托盘菜单选择它 */
        StopHotkeyBlocker(0);
        if (HotkeyComboFree(g_presets[0].mods, g_presets[0].vk)) {
            SendMessageW(g_hMsgWnd, WM_COMMAND, MENU_HK_BASE + 0, 0);
            TestCheck(g_hotkeyOk && g_mods == g_presets[0].mods && g_vk == g_presets[0].vk,
                      "menu switch works after a failed startup");
            TestCheck(RegReadDword(L"HotMods", &rm) && rm == g_presets[0].mods &&
                      RegReadDword(L"HotVk", &rv) && rv == (DWORD)g_presets[0].vk,
                      "menu switch persists the new hotkey");
        } else {
            printf("[SKIP] all-occupied menu switch: Ctrl+Alt+A not free for the probe\n");
        }
    } else {
        printf("[SKIP] all-occupied: 无法同时占住全部预设（本机已有程序占用）\n");
    }

    for (int i = 0; i < HK_BLOCK_MAX; i++) StopHotkeyBlocker(i);
    HotkeyUnregisterActive();

    RegRestoreHotkey(&bk);
}

/* GDI 句柄生命周期：选入/恢复/释放的成功与失败分支、临时字体与画笔循环 */
static void TestGdiLifecycle(void)
{
    TestSurface s;
    if (!TestSurfaceCreate(&s, 16, 16)) { TestCheck(FALSE, "gdi: surface"); return; }

    /* --- 非法入参 --- */
    HGDIOBJ old = NULL;
    TestCheck(!GdiSelect(NULL, GetStockObject(BLACK_PEN), &old), "gdi: NULL dc rejected");
    TestCheck(!GdiSelect(s.dc, NULL, &old), "gdi: NULL object rejected");
    TestCheck(!GdiRestore(NULL, GetStockObject(BLACK_PEN), "null dc"),
              "gdi: restore with NULL dc rejected");
    TestCheck(!GdiRestore(s.dc, NULL, "null obj"), "gdi: restore with NULL object rejected");

    /* --- 成功路径：选入 → 使用 → 恢复 → 删除 --- */
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 0, 0));
    if (pen) {
        HGDIOBJ op = NULL;
        TestCheck(GdiSelect(s.dc, pen, &op), "gdi: select pen");
        TestCheck(GdiRestore(s.dc, op, "pen restore"), "gdi: restore pen");
        TestCheck(DeleteObject(pen) ? TRUE : FALSE, "gdi: delete after successful restore");
    }

    /* --- 恢复失败识别：拿一个已删除对象的句柄当“原对象” --- */
    HPEN dead = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    if (dead) {
        DeleteObject(dead);                     /* 句柄失效，之后再无 GDI 分配，不会被复用 */
        TestCheck(!GdiRestore(s.dc, dead, "dead handle"), "gdi: restore failure detected");

        /* 失败之后 DC 仍然可用（说明没有把 DC 卡在非法状态） */
        HGDIOBJ op2 = NULL;
        TestCheck(GdiSelect(s.dc, GetStockObject(BLACK_PEN), &op2) &&
                  GdiRestore(s.dc, op2, "dc usable"),
                  "gdi: dc usable after failed restore");
    }

    /* --- 释放流程：恢复失败时必须如实返回 FALSE，且临时对象仍要删掉 --- */
    HPEN temp = CreatePen(PS_SOLID, 1, RGB(0, 0, 255));
    HPEN dead2 = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    if (temp && dead2) {
        DeleteObject(dead2);
        HGDIOBJ op = NULL;
        BOOL sel = GdiSelect(s.dc, temp, &op);
        DWORD gBefore = GdiCount();
        int rfail0 = g_gdiRestoreFailures;
        GdiReleaseStatus rel = GdiRelease(s.dc, temp, dead2, NULL_PEN, "release fallback");
        DWORD gAfter = GdiCount();
        /* 恢复失败由 g_gdiRestoreFailures 记录；句柄归属仍应是“已释放” */
        TestCheck(sel && rel == GdiRelFreed && g_gdiRestoreFailures > rfail0,
                  "gdi: handle freed even when restore fails");
        TestCheck(gAfter <= gBefore, "gdi: temp object not leaked after failed restore");

        LOGPEN lp;
        TestCheck(GetObjectW(temp, sizeof(lp), &lp) == 0, "gdi: temp object really deleted");
    } else {
        if (temp)  DeleteObject(temp);
        if (dead2) DeleteObject(dead2);
    }
    GdiRestore(s.dc, (HGDIOBJ)GetStockObject(BLACK_PEN), "reset test dc");

    /* --- 临时字体：选入、测量、恢复、释放，循环 1000 次 --- */
    DWORD f0 = GdiCount();
    int ffail = 0;
    for (int i = 0; i < 1000; i++) {
        HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
        if (!f) { ffail++; continue; }

        HGDIOBJ of = NULL;
        if (!GdiSelect(s.dc, f, &of)) { DeleteObject(f); ffail++; continue; }

        SIZE sz = {0, 0};
        GetTextExtentPoint32W(s.dc, L"loop", 4, &sz);
        if (GdiRelease(s.dc, f, of, DEFAULT_GUI_FONT, "loop font") != GdiRelFreed) ffail++;
        if (sz.cx <= 0) ffail++;
    }
    DWORD f1 = GdiCount();
    printf("gdi font loop: 1000x, gdi %lu -> %lu, fails=%d\n", f0, f1, ffail);
    TestCheck(ffail == 0, "gdi: font create/select/restore/delete 1000x");
    TestCheck(f1 <= f0 + 2, "gdi: font loop does not grow gdi count");

    /* --- 临时画笔：同样循环 1000 次 --- */
    DWORD p0 = GdiCount();
    int pfail = 0;
    for (int i = 0; i < 1000; i++) {
        HPEN p = CreatePen(PS_SOLID, 1 + (i % 3), RGB(10, 20, 30));
        if (!p) { pfail++; continue; }
        HGDIOBJ op = NULL;
        if (!GdiSelect(s.dc, p, &op)) { DeleteObject(p); pfail++; continue; }
        MoveToEx(s.dc, 0, 0, NULL);
        LineTo(s.dc, 8, 8);
        if (GdiRelease(s.dc, p, op, NULL_PEN, "loop pen") != GdiRelFreed) pfail++;
    }
    DWORD p1 = GdiCount();
    printf("gdi pen loop: 1000x, gdi %lu -> %lu, fails=%d\n", p0, p1, pfail);
    TestCheck(pfail == 0, "gdi: pen create/select/restore/delete 1000x");
    TestCheck(p1 <= p0 + 2, "gdi: pen loop does not grow gdi count");

    /* 再跑一轮：第一次画线可能产生一次性的 GDI 内部缓存，
     * 第二轮增量必须为 0，才能证明不是每次调用都在泄漏 */
    for (int i = 0; i < 1000; i++) {
        HPEN p = CreatePen(PS_SOLID, 1 + (i % 3), RGB(30, 20, 10));
        if (!p) { pfail++; continue; }
        HGDIOBJ op = NULL;
        if (!GdiSelect(s.dc, p, &op)) { DeleteObject(p); pfail++; continue; }
        MoveToEx(s.dc, 0, 0, NULL);
        LineTo(s.dc, 8, 8);
        if (GdiRelease(s.dc, p, op, NULL_PEN, "loop pen 2") != GdiRelFreed) pfail++;
    }
    DWORD p2 = GdiCount();
    printf("gdi pen loop: second 1000x, gdi %lu -> %lu\n", p1, p2);
    TestCheck(pfail == 0, "gdi: pen loop second batch has no failures");
    TestCheck(p2 <= p1, "gdi: pen loop is steady after first batch");

    TestSurfaceDestroy(&s);
}

/* TextAnnotRect / DrawTextAnnot 在字体选入失败时必须安全退出且不泄漏字体 */
static void TestTextAnnotSelectFailure(void)
{
    Annot a;
    ZeroMemory(&a, sizeof(a));
    a.kind  = AN_TEXT;
    a.x     = 10;
    a.y     = 20;
    a.fontH = 20;
    lstrcpynW(a.text, L"测量失败测试", 256);

    DWORD g0 = GdiCount();
    g_gdiFailInject = 1;                   /* 让第一次 GdiSelect 失败 */
    RECT r;
    GdiPaintBatch mb;
    GdiBatchInit(&mb);
    GdiReleaseStatus mst = TextAnnotRect(&a, &r, &mb);
    g_gdiFailInject = 0;
    DWORD g1 = GdiCount();

    printf("text measure: injected select failure, status=%d rect %ldx%ld, gdi %lu -> %lu\n",
           (int)mst, r.right - r.left, r.bottom - r.top, g0, g1);
    TestCheck(r.left == a.x && r.top == a.y && r.right == r.left && r.bottom == r.top,
              "text measure: failed select yields zero-size rect");
    TestCheck(mst == GdiRelFreed && GdiBatchDrain(&mb) == 0,
              "text measure: failed select leaves nothing to the caller");
    TestCheck(g1 <= g0, "text measure: font not leaked on select failure");

    /* 非 void 接口的关键路径：字体删不掉 + 节点分配失败时，
     * 句柄必须落在调用方批次里，drain 后再进托管链表 */
    int heldBase = g_gdiHeldCount;
    GdiPaintBatch ob;
    GdiBatchInit(&ob);
    g_gdiDeleteFailInject = 1024;
    g_gdiHeldFailInject   = 1;
    GdiReleaseStatus ost = TextAnnotRect(&a, &r, &ob);
    g_gdiDeleteFailInject = 0;
    g_gdiHeldFailInject   = 0;
    printf("text measure: oom path status=%d batch=%d held=%d\n",
           (int)ost, ob.count, g_gdiHeldCount);
    TestCheck(ost == GdiRelUnreleased && ob.count == 1 && ob.obj[0] != NULL,
              "text measure: unreleased font is kept in caller batch");

    g_gdiDeleteFailInject = 1024;          /* 仍然删不掉，但允许托管 */
    TestCheck(GdiBatchDrain(&ob) == 0 && g_gdiHeldCount == heldBase + 1,
              "text measure: batch drain hands font to custody");
    g_gdiDeleteFailInject = 0;
    GdiFlushDeferred();                    /* 屏幕 DC 已 Release，字体可真正删除 */
    TestCheck(g_gdiHeldCount == heldBase, "text measure: custody releases after dc release");

    /* 绘制路径同理：只跳过这次绘制，不泄漏字体、不影响 DC */
    TestSurface s;
    if (TestSurfaceCreate(&s, 32, 32)) {
        DWORD h0 = GdiCount();
        GdiPaintBatch b;
        GdiBatchInit(&b);
        g_gdiFailInject = 1;
        DrawTextAnnot(s.dc, &a, &b);
        g_gdiFailInject = 0;
        DWORD h1 = GdiCount();
        TestCheck(h1 <= h0 && GdiBatchDrain(&b) == 0,
                  "text annot: select failure does not leak font");
        TestSurfaceDestroy(&s);
    } else {
        TestCheck(FALSE, "text annot: surface");
    }
}

/* 恢复失败 + stock 顶出失败：临时对象不能丢，必须进托管链表，DC 销毁后释放 */
static void TestGdiDeferredRelease(void)
{
    TestSurface s;
    if (!TestSurfaceCreate(&s, 16, 16)) { TestCheck(FALSE, "defer: surface"); return; }

    DWORD g0 = GdiCount();
    int  d0 = g_gdiHeldCount;
    int  done0 = g_gdiHeldDone;

    HPEN pen  = CreatePen(PS_SOLID, 1, RGB(9, 9, 9));
    HPEN dead = CreatePen(PS_SOLID, 1, RGB(1, 1, 1));
    if (!pen || !dead) {
        if (pen)  DeleteObject(pen);
        if (dead) DeleteObject(dead);
        TestSurfaceDestroy(&s);
        TestCheck(FALSE, "defer: create pens");
        return;
    }
    DeleteObject(dead);                        /* 失效句柄冒充“原对象” */

    HGDIOBJ oldPen = NULL;
    BOOL selected = GdiSelect(s.dc, pen, &oldPen);

    /* stockFallback = -1：没有替代对象；再注入删除失败——
     * 模拟“对象仍被 DC 引用、DeleteObject 失败”，此时句柄必须被托管 */
    g_gdiDeleteFailInject = 1;
    GdiReleaseStatus rel = GdiRelease(s.dc, pen, dead, -1, "defer test");
    g_gdiDeleteFailInject = 0;

    printf("defer: selected=%d status=%d held=%d (base %d) objType=%lu\n",
           selected, (int)rel, g_gdiHeldCount, d0,
           (unsigned long)GetObjectType(pen));

    TestCheck(selected && rel == GdiRelHeld, "defer: handle is held, not dropped");
    TestCheck(g_gdiHeldCount == d0 + 1, "defer: un-deletable object is retained");
    TestCheck(GdiHeldContains((HGDIOBJ)pen), "defer: exact handle is reachable in custody list");
    TestCheck(GetObjectType(pen) != 0, "defer: handle still valid before DC teardown");

    /* DC 销毁后重试：对象应被真正释放 */
    TestSurfaceDestroy(&s);
    TestCheck(g_gdiHeldCount == d0, "defer: custody list drained after DC destroyed");
    TestCheck(g_gdiHeldDone == done0 + 1, "defer: held object finally released");
    /* GetObjectType 对“已删除但句柄尚未回收”的对象仍返回类型，不能作为唯一判据；
     * 这里用 GDI 计数确认没有残留 */
    TestCheck(GdiCount() <= g0, "defer: no gdi growth after deferred release");
}

/* 批量托管：一次登记 80 个对象（超过旧 64+8+4 的全部槽位），
 * 链表按需增长，不允许再出现“容量上限”。 */
static void TestGdiDeferredBulk(void)
{
    enum { NBULK = 80 };
    DWORD g0 = GdiCount();
    int base  = g_gdiHeldCount;
    int done0 = g_gdiHeldDone;

    HPEN pens[NBULK];
    int made = 0;
    for (int i = 0; i < NBULK; i++) {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(i, 0, 0));
        if (!p) break;
        if (!GdiHeldTake(p)) { DeleteObject(p); break; }
        pens[made++] = p;
    }

    printf("defer bulk: made=%d held=%d (base %d)\n", made, g_gdiHeldCount, base);
    TestCheck(made == NBULK, "defer bulk: 80 objects accepted (no fixed cap)");
    TestCheck(g_gdiHeldCount == base + NBULK, "defer bulk: 80 entries in custody list");
    TestCheck(GdiHeldContains((HGDIOBJ)pens[0]) && GdiHeldContains((HGDIOBJ)pens[NBULK - 1]),
              "defer bulk: first and last handles are reachable");

    GdiFlushDeferred();                     /* 都未被选入 DC，可直接删 */
    printf("defer bulk: after flush held=%d freed=%d\n",
           g_gdiHeldCount, g_gdiHeldDone - done0);
    TestCheck(g_gdiHeldCount == base, "defer bulk: list drained after flush");
    TestCheck(g_gdiHeldDone - done0 == NBULK, "defer bulk: all 80 objects released");
    TestCheck(GdiCount() <= g0, "defer bulk: no gdi growth");
}

/* 同一句柄重复登记只保留一条，且最终只释放一次 */
static void TestGdiDeferredDuplicate(void)
{
    HPEN p = CreatePen(PS_SOLID, 1, RGB(1, 2, 3));
    if (!p) { TestCheck(FALSE, "defer dup: create pen"); return; }

    DWORD g0 = GdiCount();
    int base  = g_gdiHeldCount;
    int done0 = g_gdiHeldDone;

    BOOL a = GdiHeldTake(p);
    BOOL b = GdiHeldTake(p);
    printf("defer dup: first=%d second=%d held=%d (base %d)\n",
           a, b, g_gdiHeldCount, base);
    TestCheck(a && b, "defer dup: duplicate registration is accepted silently");
    TestCheck(g_gdiHeldCount == base + 1, "defer dup: only one entry kept");

    GdiFlushDeferred();                     /* 未被选入，可以直接删 */
    TestCheck(g_gdiHeldCount == base, "defer dup: released exactly once");
    TestCheck(g_gdiHeldDone == done0 + 1, "defer dup: free counted once");
    TestCheck(GdiCount() <= g0, "defer dup: no gdi growth");
}

/* 容量耗尽（真实调用链）：
 *   先让 64 + 8 + 4 = 76 个“仍被 DC 引用”的对象占满旧三级结构对应的全部槽位，
 *   再用真实 GdiRelease() 释放第 77 个对象。托管链表按需分配，因此第 77 个
 *   以及后续连续失败的对象都必须仍然可达，而不是只剩计数和日志。
 *   最后销毁全部 DC，验证 partial 释放、重试失败、重复登记、批次兜底和最终清空。 */
static void TestGdiDeferredCapacity(void)
{
    enum { NPRIOR = 76, NEXTRA = 8, NCAP = NPRIOR + 1 + NEXTRA + 2 };
    static HDC  dcs[NCAP];
    static HPEN pens[NCAP];
    int ndc = 0;

    DWORD g0  = GdiCount();
    int base  = g_gdiHeldCount;
    int done0 = g_gdiHeldDone;
    int lost0 = g_gdiDeferLostHandles;
    int alloc0 = g_gdiHeldAllocFailures;

    /* 1) 填满旧模型的 64 + 8 + 4 = 76 个槽位 */
    for (int i = 0; i < NPRIOR; i++) {
        HDC dc = CreateCompatibleDC(NULL);
        HPEN p = CreatePen(PS_SOLID, 1, RGB(i & 0xFF, (i * 5) & 0xFF, 0));
        HGDIOBJ old = NULL;
        if (!dc || !p || !GdiSelect(dc, p, &old)) {
            if (p) DeleteObject(p);
            if (dc) DeleteDC(dc);
            break;
        }
        if (!GdiHeldTake(p)) { DeleteObject(p); DeleteDC(dc); break; }
        dcs[ndc] = dc; pens[ndc] = p; ndc++;
    }
    printf("custody cap: filled=%d/%d held=%d\n", ndc, NPRIOR, g_gdiHeldCount);
    TestCheck(ndc == NPRIOR, "custody cap: 76 prior objects held");
    TestCheck(g_gdiHeldCount == base + NPRIOR, "custody cap: list grows past old 76 slots");

    /* 2) 第 77 个：真实 GdiRelease()，删除失败也必须留下可达句柄 */
    HDC     extraDc = CreateCompatibleDC(NULL);
    HPEN    extra   = CreatePen(PS_SOLID, 2, RGB(99, 99, 99));
    HGDIOBJ eold    = NULL;
    int capMade = ndc;
    if (extraDc && extra && GdiSelect(extraDc, extra, &eold)) {
        HPEN dead = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        if (dead) DeleteObject(dead);

        g_gdiDeleteFailInject = 1024;      /* 覆盖入口 flush + 本次删除 */
        GdiReleaseStatus st = GdiRelease(extraDc, extra, dead, -1, "cap 77th");
        g_gdiDeleteFailInject = 0;
        printf("custody cap: 77th status=%d held=%d\n", (int)st, g_gdiHeldCount);

        TestCheck(st == GdiRelHeld, "custody cap: 77th is held, not unreleased");
        TestCheck(GdiHeldContains((HGDIOBJ)extra),
                  "custody cap: 77th handle is reachable after release returned");
        TestCheck(g_gdiHeldCount == base + NPRIOR + 1, "custody cap: 77th added to list");

        /* 重复释放同一句柄：只能保留一条记录 */
        g_gdiDeleteFailInject = 1024;
        GdiReleaseStatus st2 = GdiRelease(extraDc, extra, dead, -1, "cap dup");
        g_gdiDeleteFailInject = 0;
        TestCheck(st2 == GdiRelHeld && g_gdiHeldCount == base + NPRIOR + 1,
                  "custody cap: duplicate release keeps exactly one entry");

        dcs[ndc] = extraDc; pens[ndc] = extra; ndc++; capMade++;
    } else {
        if (extra) DeleteObject(extra);
        if (extraDc) DeleteDC(extraDc);
        TestCheck(FALSE, "custody cap: setup 77th object");
    }

    /* 3) 连续多个对象释放失败：每个都要有自己的可达记录 */
    int extraMade = 0;
    for (int i = 0; i < NEXTRA; i++) {
        HDC dc = CreateCompatibleDC(NULL);
        HPEN p = CreatePen(PS_SOLID, 1, RGB(0, 0, 200 + i));
        HGDIOBJ old = NULL;
        if (!dc || !p || !GdiSelect(dc, p, &old)) {
            if (p) DeleteObject(p);
            if (dc) DeleteDC(dc);
            break;
        }
        int h0 = g_gdiHeldCount;
        g_gdiDeleteFailInject = 1024;
        GdiReleaseStatus st = GdiRelease(dc, p, old, NULL_PEN, "cap extra");
        g_gdiDeleteFailInject = 0;
        if (st != GdiRelHeld || !GdiHeldContains((HGDIOBJ)p) || g_gdiHeldCount != h0 + 1) {
            dcs[ndc] = dc; pens[ndc] = p;
            TestCheck(FALSE, "custody cap: consecutive object keeps custody");
            break;
        }
        dcs[ndc] = dc; pens[ndc] = p; ndc++; capMade++; extraMade++;
    }
    printf("custody cap: consecutive=%d held=%d\n", extraMade, g_gdiHeldCount);
    TestCheck(extraMade == NEXTRA, "custody cap: consecutive failures all keep custody");

    /* 4) 绘制批次兜底：模拟 OOM（删除失败 + 节点分配失败），句柄先进批次，
     *    随后 drain 时必须进入托管链表，不能丢 */
    {
        HDC dc = CreateCompatibleDC(NULL);
        HPEN p = CreatePen(PS_SOLID, 1, RGB(45, 45, 45));
        HGDIOBJ old = NULL;
        if (dc && p && GdiSelect(dc, p, &old)) {
            GdiPaintBatch b;
            GdiBatchInit(&b);

            g_gdiDeleteFailInject = 1024;
            g_gdiHeldFailInject   = 1;      /* 模拟节点分配失败 */
            GdiReleaseStatus st = GdiPaintRelease(dc, p, old, NULL_PEN, "cap batch", &b);
            g_gdiHeldFailInject   = 0;
            TestCheck(st == GdiRelUnreleased && b.count == 1 && b.obj[0] == (HGDIOBJ)p,
                      "custody cap: unreleased handle is kept in caller batch");

            g_gdiDeleteFailInject = 1024;   /* 仍然删不掉，但这次允许托管 */
            int left = GdiBatchDrain(&b);
            g_gdiDeleteFailInject = 0;
            printf("custody cap: batch drained left=%d held=%d\n", left, g_gdiHeldCount);
            TestCheck(left == 0 && b.count == 0 && GdiHeldContains((HGDIOBJ)p),
                      "custody cap: batch drain hands handle to custody list");

            dcs[ndc] = dc; pens[ndc] = p; ndc++; capMade++;
        } else {
            if (p) DeleteObject(p);
            if (dc) DeleteDC(dc);
        }
    }

    /* 5) 遮罩会话兜底批次：帧末仍交不出去的句柄先进 g_gdiSessionBatch */
    {
        HDC dc = CreateCompatibleDC(NULL);
        HPEN p = CreatePen(PS_SOLID, 1, RGB(60, 60, 60));
        HGDIOBJ old = NULL;
        if (dc && p && GdiSelect(dc, p, &old)) {
            GdiPaintBatch b;
            GdiBatchInit(&b);
            g_gdiDeleteFailInject = 1024;
            g_gdiHeldFailInject   = 2;      /* 一次给 GdiRelease，一次给 Adopt */
            GdiPaintRelease(dc, p, old, NULL_PEN, "cap session batch", &b);
            GdiBatchAdopt(&b);
            g_gdiHeldFailInject   = 0;
            printf("custody cap: session batch=%d held=%d\n",
                   g_gdiSessionBatch.count, g_gdiHeldCount);
            TestCheck(g_gdiSessionBatch.count == 1 &&
                      g_gdiSessionBatch.obj[0] == (HGDIOBJ)p,
                      "custody cap: session batch keeps handle of last resort");

            GdiFlushDeferred();             /* 删除仍失败，但托管恢复，句柄转入链表 */
            g_gdiDeleteFailInject = 0;
            TestCheck(g_gdiSessionBatch.count == 0 && GdiHeldContains((HGDIOBJ)p),
                      "custody cap: session batch hands handle to custody list");

            dcs[ndc] = dc; pens[ndc] = p; ndc++; capMade++;
        } else {
            if (p) DeleteObject(p);
            if (dc) DeleteDC(dc);
        }
    }

    printf("custody cap: total held=%d (expect %d)\n", g_gdiHeldCount, base + capMade);
    TestCheck(g_gdiHeldCount == base + capMade, "custody cap: every released object is held");
    /* 3 次节点分配失败：步骤 4 一次；步骤 5 的 GdiRelease 与批次兜底各一次。
     * 句柄最终都通过批次/链表找回，lost 只记录“当场所有权退回调用方”的 2 次。 */
    TestCheck(g_gdiDeferLostHandles == lost0 + 2 && g_gdiHeldAllocFailures == alloc0 + 3,
              "custody cap: OOM fallbacks are counted, not silently dropped");

    /* 6) 部分成功：先销毁前 64 个 DC。链表是后进先出，先失败的 23 条
     *    （后建的 11 条 + 仍存活的 12 条）用注入挡住，其余 64 条应真正释放 */
    int releaseFirst = 64;
    for (int i = 0; i < releaseFirst && i < ndc; i++) {
        DeleteDC(dcs[i]);
        dcs[i] = NULL;
    }
    int keepCount = g_gdiHeldCount - releaseFirst;
    g_gdiDeleteFailInject = keepCount;
    GdiFlushDeferred();
    g_gdiDeleteFailInject = 0;
    printf("custody cap: partial flush held=%d freed=%d\n",
           g_gdiHeldCount, g_gdiHeldDone - done0);
    TestCheck(g_gdiHeldDone - done0 == releaseFirst,
              "custody cap: partial flush releases only unlocked objects");
    TestCheck(g_gdiHeldCount == base + capMade - releaseFirst,
              "custody cap: partial flush keeps the rest reachable");
    TestCheck(!GdiHeldContains((HGDIOBJ)pens[releaseFirst - 1]) &&
              GdiHeldContains((HGDIOBJ)pens[releaseFirst]),
              "custody cap: surviving handle still reachable after partial flush");

    /* 7) 释放重试失败：仍被 DC 引用的对象不能因重试而被误删 */
    int beforeRetry = g_gdiHeldCount;
    int doneBeforeRetry = g_gdiHeldDone;
    g_gdiDeleteFailInject = 1024;
    GdiFlushDeferred();
    g_gdiDeleteFailInject = 0;
    TestCheck(g_gdiHeldCount == beforeRetry && g_gdiHeldDone == doneBeforeRetry,
              "custody cap: failed retry keeps every entry");

    /* 8) 销毁剩余 DC 后（会话结束），所有对象都必须真正释放，链表清空 */
    for (int i = releaseFirst; i < ndc; i++) {
        if (dcs[i]) { DeleteDC(dcs[i]); dcs[i] = NULL; }
    }
    GdiFlushDeferred();
    GdiFlushDeferred();                     /* 重复清理必须幂等 */
    printf("custody cap: after dc destroy held=%d freed=%d batch=%d\n",
           g_gdiHeldCount, g_gdiHeldDone - done0, g_gdiSessionBatch.count);
    TestCheck(g_gdiHeldCount == base, "custody cap: custody list fully drained");
    TestCheck(g_gdiSessionBatch.count == 0, "custody cap: session batch fully drained");
    TestCheck(g_gdiHeldDone - done0 == capMade, "custody cap: release count matches");
    TestCheck(GdiCount() <= g0, "custody cap: no gdi growth");

    /* 9) 恢复后仍能正常登记与释放 */
    HPEN again = CreatePen(PS_SOLID, 1, RGB(7, 7, 7));
    if (again) {
        TestCheck(GdiHeldTake(again), "custody cap: registration works again after drain");
        GdiFlushDeferred();
        TestCheck(g_gdiHeldCount == base, "custody cap: recovered entry released");
    }
}

/* 托管链表清理不能释放仍被 DC 引用、且这次删除失败的对象 */
static void TestGdiDeferredOwnership(void)
{
    TestSurface s;
    if (!TestSurfaceCreate(&s, 8, 8)) { TestCheck(FALSE, "defer own: surface"); return; }

    int base  = g_gdiHeldCount;
    int done0 = g_gdiHeldDone;

    HPEN p = CreatePen(PS_SOLID, 1, RGB(3, 3, 3));
    if (!p) { TestSurfaceDestroy(&s); TestCheck(FALSE, "defer own: create pen"); return; }

    HGDIOBJ old = NULL;
    BOOL selected = GdiSelect(s.dc, p, &old);
    BOOL retained = GdiHeldTake(p);

    /* 注入让这次 flush 删不掉，模拟“对象仍被 DC 引用”；
     * 真实 GDI 对已被选入的画笔允许 DeleteObject 成功（延迟生效），
     * 所以这里必须用注入构造“删不掉”的状态 */
    g_gdiDeleteFailInject = 1;
    GdiFlushDeferred();
    g_gdiDeleteFailInject = 0;
    printf("defer own: selected=%d retained=%d held=%d type=%lu\n",
           selected, retained, g_gdiHeldCount, (unsigned long)GetObjectType(p));
    TestCheck(selected && retained, "defer own: object registered while selected");
    TestCheck(g_gdiHeldCount == base + 1, "defer own: still held while dc alive");
    TestCheck(GdiHeldContains((HGDIOBJ)p), "defer own: exact handle still reachable");
    TestCheck(GetObjectType(p) != 0, "defer own: flush does not force-delete referenced object");

    TestSurfaceDestroy(&s);                 /* DC 销毁后再清一次 */
    TestCheck(g_gdiHeldCount == base, "defer own: released after dc destroyed");
    TestCheck(g_gdiHeldDone == done0 + 1, "defer own: deferred release counted");
}

/* 绘制过程中恢复失败：不能影响后续绘制的 DC 状态 */
static void TestGdiRestoreFailureDrawing(void)
{
    TestSurface a, b;
    if (!TestSurfaceCreate(&a, 48, 48)) { TestCheck(FALSE, "draw fail: surface a"); return; }
    if (!TestSurfaceCreate(&b, 48, 48)) {
        TestSurfaceDestroy(&a);
        TestCheck(FALSE, "draw fail: surface b");
        return;
    }

    RECT full = { 0, 0, 48, 48 };
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    if (white) {
        FillRect(a.dc, &full, white);
        FillRect(b.dc, &full, white);
        DeleteObject(white);
    }

    Annot an;
    ZeroMemory(&an, sizeof(an));
    an.kind  = AN_RECT;
    an.width = 2;
    an.color = RGB(200, 30, 30);
    an.rc.left = 8; an.rc.top = 8;
    an.rc.right = 40; an.rc.bottom = 40;

    DWORD g0 = GdiCount();
    GdiPaintBatch batch;
    GdiBatchInit(&batch);
    g_gdiRestoreFailInject = 1;            /* 只让第一次恢复失败（画刷恢复） */
    DrawRectAnnot(a.dc, &an, &batch);
    g_gdiRestoreFailInject = 0;

    /* 反复注入失败：稳态下 GDI 数量必须不再增长（排除一次性的 DC 内部缓存） */
    DWORD g1 = GdiCount();
    for (int i = 0; i < 100; i++) {
        g_gdiRestoreFailInject = 1;
        DrawRectAnnot(a.dc, &an, &batch);
        g_gdiRestoreFailInject = 0;
    }
    DWORD g2 = GdiCount();
    printf("draw fail: gdi %lu -> %lu -> %lu (100 injected draws)\n", g0, g1, g2);
    TestCheck(g2 <= g1, "draw fail: repeated restore failures do not grow gdi");

    /* 恢复失败之后继续画，结果必须和“从未失败”的参照图完全一致 */
    DrawRectAnnot(a.dc, &an, &batch);
    DrawRectAnnot(b.dc, &an, &batch);
    TestCheck(GdiBatchDrain(&batch) == 0, "draw fail: no handle left in paint batch");

    size_t diff = 0;
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
            if (TopDownPixel(a.bits, 48, x, y) != TopDownPixel(b.bits, 48, x, y)) diff++;
        }
    }
    printf("draw fail: pixels compared=2304 diff=%llu\n", (unsigned long long)diff);
    TestCheck(diff == 0, "draw fail: dc state intact after failed restore");

    TestSurfaceDestroy(&a);
    TestSurfaceDestroy(&b);
}

/* RestoreOldBmp：正常释放路径 + 原对象句柄失效时的清理路径 */
static void TestRestoreOldBmp(void)
{
    /* 正常路径：分配三份缓冲 → FreeCapture，GDI 应回到原值 */
    DWORD g0 = GdiCount();
    if (!OvAllocBuffers(64, 48)) {
        TestCheck(FALSE, "restore: alloc capture buffers");
        return;
    }
    DWORD g1 = GdiCount();
    FreeCapture();
    DWORD g2 = GdiCount();
    printf("restore old bmp: gdi %lu -> %lu -> %lu\n", g0, g1, g2);
    TestCheck(g1 == g0 + 6, "restore: capture buffers add 6 gdi objects");
    TestCheck(g2 == g0, "restore: FreeCapture releases them");

    /* 异常路径：原对象句柄已失效时，RestoreOldBmp 仍要清空记录并安全收尾 */
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateBitmap(4, 4, 1, 32, NULL);
    if (!dc || !bmp) {
        if (dc) DeleteDC(dc);
        if (bmp) DeleteObject(bmp);
        TestCheck(FALSE, "restore: create test dc/bitmap");
        return;
    }

    HGDIOBJ selected = NULL;
    BOOL okSel = GdiSelect(dc, bmp, &selected);

    HPEN dead = CreatePen(PS_SOLID, 1, RGB(1, 2, 3));
    if (dead) DeleteObject(dead);          /* 失效句柄冒充“原对象” */

    HGDIOBJ bad = dead;
    RestoreOldBmp(dc, &bad);
    TestCheck(okSel && bad == NULL, "restore: invalid old handle is cleared");

    /* 还原失败时位图仍在 DC 里；DeleteDC 会解除引用，随后必须能删除成功 */
    DeleteDC(dc);
    TestCheck(DeleteObject(bmp) ? TRUE : FALSE,
              "restore: bitmap deletable after forced restore failure");
}

/* 预热：先把各子系统的一次性系统缓存（EDIT 控件内部对象、GDI+ 全局对象等）
 * 建出来，再测整个流程前后的 GDI 数量，避免把一次性缓存误判成泄漏。 */
static void WarmUpAll(HWND overlay)
{
    g_ov.hwnd = overlay;
    g_ov.vx = 0; g_ov.vy = 0;
    g_ov.vw = 640; g_ov.vh = 480;
    /* 自检里没有走 WM_CREATE 的初始化路径，这里补齐工具条/选框需要的全局对象 */
    TestOverlayEnsureGdi();
    g_ov.hasSel = TRUE;
    g_ov.state = ST_SELECTED;
    g_ov.sel.left = 10;  g_ov.sel.top = 10;
    g_ov.sel.right = 620; g_ov.sel.bottom = 400;

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) { g_ov.hwnd = NULL; return; }
    FillTestPattern();
    if (!OvDimBuffers()) {
        FreeCapture();
        g_ov.hwnd = NULL;
        return;
    }

    g_annCount = 0;
    Annot *a = &g_ann[g_annCount++];
    ZeroMemory(a, sizeof(*a));
    a->kind = AN_MOSAIC;
    a->rc.left = 40; a->rc.top = 40; a->rc.right = 180; a->rc.bottom = 140;

    HBITMAP h = RenderOverlayToDib(NULL);
    if (h) DeleteObject(h);

    for (int i = 0; i < 20; i++) {
        TextStart(30, 30);
        TextCancel();
    }

    void *bits = NULL;
    HBITMAP hb = BuildFinalBitmap(10, 10, 200, 150, &bits);
    if (hb && bits) CopyDibToClipboard(bits, 200, 150);
    if (hb) DeleteObject(hb);

    g_annCount = 0;
    OverlayReleaseAll();
    g_ov.hwnd = NULL;
}

static int RunStabilityTest(void)
{
    printf("=== QuickShot stability self-test ===\n");

    g_hMsgWnd = CreateWindowExW(0, L"QS_MsgWnd", L"", 0, 0, 0, 0, 0,
                                NULL, NULL, g_hInst, NULL);
    if (!g_hMsgWnd) { printf("FAIL: message window\n"); return 1; }

    HWND overlay = CreateWindowExW(WS_EX_TOOLWINDOW, OVERLAY_CLASS, L"", WS_POPUP,
                                   0, 0, 640, 480, NULL, NULL, g_hInst, NULL);
    if (!overlay) { printf("FAIL: overlay window\n"); DestroyWindow(g_hMsgWnd); return 1; }

    /* 预热两遍：第一遍会把 GDI / GDI+ / 字体子系统的进程级一次性缓存建出来，
     * 第二遍之后基线才稳定，避免把一次性缓存误判成泄漏 */
    WarmUpAll(overlay);
    WarmUpAll(overlay);
    TestOverlayEnsureGdi();                /* 预热结尾释放过全局对象，正式测试前重建 */
    DWORD g0 = GdiCount();
    printf("  gdi after warmup: %lu\n", g0);
    /* 正常渲染路径不允许出现任何 GDI 失败（注入测试在更后面才执行） */
    TestCheck(g_gdiSelectFailures == 0 && g_gdiRestoreFailures == 0 &&
              g_gdiReleaseFailures == 0,
              "no gdi failures during warmup rendering");

    TestCaptureLoop();
    printf("  gdi after capture: %lu\n", GdiCount());
    TestTextLifecycle(overlay);
    TestOverlayEnsureGdi();                /* 上一步收尾时释放过全局对象 */
    printf("  gdi after text: %lu\n", GdiCount());
    TestMosaic(overlay);
    printf("  gdi after mosaic: %lu\n", GdiCount());
    TestMosaicDiscard(overlay);
    TestMosaicBudget();
    TestClipboard();
    TestClipboardBusy();
    TestFileNames();
    TestHotkey();
    TestHotkeyStartupFallback();
    TestHotkeyAllOccupied();
    TestGdiLifecycle();
    TestTextAnnotSelectFailure();
    TestGdiDeferredRelease();
    TestGdiDeferredBulk();
    TestGdiDeferredDuplicate();
    TestGdiDeferredCapacity();
    TestGdiDeferredOwnership();
    TestGdiRestoreFailureDrawing();
    TestRestoreOldBmp();

    /* 模拟一次正常退出：窗口销毁 + 资源统一释放 */
    if (g_ov.hwnd == overlay) g_ov.hwnd = NULL;
    DestroyWindow(overlay);
    DestroyWindow(g_hMsgWnd);
    g_hMsgWnd = NULL;
    OverlayReleaseAll();

    DWORD g1 = GdiCount();
    printf("=== gdi total: %lu -> %lu ===\n", g0, g1);
    TestCheck(g1 <= g0, "no gdi leak across whole run");

    printf("=== gdi failures: select=%d restore=%d release=%d | held=%d batch=%d freed=%d allocFailures=%d unreleased=%d paintUnreleased=%d ===\n",
           g_gdiSelectFailures, g_gdiRestoreFailures, g_gdiReleaseFailures,
           g_gdiHeldCount, g_gdiSessionBatch.count, g_gdiHeldDone,
           g_gdiHeldAllocFailures, g_gdiDeferLostHandles, g_gdiUnreleasedInPaint);
    TestCheck(g_gdiHeldCount == 0 && g_gdiSessionBatch.count == 0,
              "no pending gdi objects at exit");
    /* 4 次 allocFailures / 3 次 unreleased 分别来自 TextAnnotRect 与容量测试的 OOM 注入，
     * 句柄最终都已通过批次或链表找回 */
    TestCheck(g_gdiHeldAllocFailures == 4 && g_gdiDeferLostHandles == 3 &&
              g_gdiUnreleasedInPaint == 3,
              "injected oom fallbacks are counted and recovered");

    GdiDeferReleaseAll();                  /* 正式退出前的最后一次清理（静态存储无需 free） */

    printf("=== %s (%d failures) ===\n", g_testFail ? "FAILED" : "OK", g_testFail);
    fflush(stdout);
    return g_testFail ? 1 : 0;
}

/* ---------------------------------------------------------------- 入口 */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)cmd; (void)show;
    g_hInst = hInst;

    EnableDpiAwareness();

    /* 可选诊断日志 */
    {
        WCHAR on[8] = {0};
        if (GetEnvironmentVariableW(L"QS_LOG", on, 8) > 0) {
            WCHAR lp[MAX_PATH];
            MakeSiblingPath(L"quickshot.log", lp, MAX_PATH);
            g_logf = _wfopen(lp, L"w");
            QSLog("=== QuickShot start, dpi aware ===");
        }
    }

    /* 单实例（自检模式跳过，免得被正在运行的实例挡掉） */
    BOOL isTest = (wcsstr(GetCommandLineW(), L"--capture-test") != NULL ||
                   wcsstr(GetCommandLineW(), L"--toolbar-test") != NULL ||
                   wcsstr(GetCommandLineW(), L"--stress-test") != NULL ||
                   wcsstr(GetCommandLineW(), L"--demo") != NULL);
    HANDLE mutex = NULL;
    if (!isTest) {
        mutex = CreateMutexW(NULL, TRUE, L"Global\\QuickShot_SingleInstance");
        if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            MessageBoxW(NULL, L"QuickShot 已经在运行了（看系统托盘）。",
                        L"QuickShot", MB_ICONINFORMATION | MB_OK);
            return 0;
        }
    }

    LoadSettings();
    ResolveSaveDir();

    /* GDI+ 初始化 */
    GdiplusStartupInput si;
    ZeroMemory(&si, sizeof(si));
    si.GdiplusVersion = 1;
    if (GdiplusStartup(&g_gdiToken, &si, NULL) != 0) {
        /* GDI+ 起不来，所有依赖它的导出/编码路径都不该继续走 */
        MessageBoxW(NULL, L"GDI+ 初始化失败。", L"QuickShot", MB_ICONERROR | MB_OK);
        return 1;
    }
    g_gdiplusOk = TRUE;

    g_fontLabel = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
    g_fontToast = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              L"Microsoft YaHei UI");
    g_fontText = CreateFontW(-TEXT_H, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei UI");
    g_fontIcon = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Segoe UI Symbol");

    /* 注册窗口类 */
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MsgProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"QS_MsgWnd";
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;          /* 双击确认需要 */
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_CROSS);
    wc.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ToastProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = TOAST_CLASS;
    RegisterClassExW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PinProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_SIZEALL);
    wc.lpszClassName = PIN_CLASS;
    RegisterClassExW(&wc);

    /* 自检模式不需要窗口 */
    {
        const WCHAR *cl = GetCommandLineW();
        if (wcsstr(cl, L"--capture-test")) {
            int r = RunCaptureTest();
            GdiplusShutdown(g_gdiToken);
            return r;
        }
        if (wcsstr(cl, L"--toolbar-test")) {
            int r = RunToolbarTest();
            GdiplusShutdown(g_gdiToken);
            return r;
        }
        if (wcsstr(cl, L"--stress-test")) {
            int r = RunStabilityTest();
            GdiplusShutdown(g_gdiToken);
            return r;
        }
        if (wcsstr(cl, L"--demo")) {
            int ac = 0, r = 1;
            LPWSTR *av = CommandLineToArgvW(cl, &ac);
            if (av && ac >= 4) r = RunDemoShot(av[2], av[3], ac >= 5);
            if (av) LocalFree(av);
            GdiplusShutdown(g_gdiToken);
            return r;
        }
    }

    /* 隐藏的消息窗口（不显示，只收消息） */
    g_hMsgWnd = CreateWindowExW(0, L"QS_MsgWnd", L"", 0,
                                0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hMsgWnd) return 1;

    TrayAdd();
    int hkStart = HotkeyRegisterOnStartup();
    TrayUpdateTip();
    QSLog("hotkey start=%d registered=%d mods=0x%X vk=0x%X",
          hkStart, (int)g_hotkeyOk, g_mods, g_vk);

    if (hkStart == HK_START_NONE) {
        MessageBoxW(NULL,
                    L"当前保存的快捷键和其它预设都被占用了。\n\n"
                    L"请右键托盘图标，在「快捷键」里换一个组合。",
                    L"QuickShot", MB_ICONWARNING | MB_OK);
    }

    {
        /* 提示里显示实际生效的组合；回退时说明原因，避免"配置和实际不一样"的困惑 */
        const WCHAR *hkLabel = HotkeyLabel();

        WCHAR tip[160];
        if (hkStart == HK_START_FALLBACK)
            wsprintfW(tip, L"原快捷键被占用，已临时改用 %s", hkLabel);
        else if (g_hotkeyOk)
            wsprintfW(tip, L"已启动  ·  按 %s 截图", hkLabel);
        else
            lstrcpynW(tip, L"已启动  ·  请右键托盘图标设置快捷键", 160);

        ToastShowAt(tip,
                    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) / 2,
                    GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 90);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    HotkeyUnregisterActive();
    OverlayReleaseAll();                   /* 业务窗口与所有 DC 先销毁 */
    if (g_fontLabel) DeleteObject(g_fontLabel);
    if (g_fontToast) DeleteObject(g_fontToast);
    if (g_fontText)  DeleteObject(g_fontText);
    if (g_fontIcon)  DeleteObject(g_fontIcon);
    g_fontLabel = g_fontToast = g_fontText = g_fontIcon = NULL;
    /* DC 与全局 GDI 对象都销毁后再做最后一次延迟释放，并报告无法释放的残留 */
    GdiDeferReleaseAll();
    g_gdiplusOk = FALSE;
    GdiplusShutdown(g_gdiToken);
    if (mutex) CloseHandle(mutex);
    return 0;
}

/* 控制台自检入口（只在 -DTEST_CONSOLE 构建时存在） */
#ifdef TEST_CONSOLE
int main(void)
{
    return WinMain(GetModuleHandleW(NULL), NULL, NULL, SW_SHOW);
}
#endif
