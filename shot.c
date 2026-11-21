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
static BOOL       g_saveFile = TRUE;
static BOOL       g_hotkeyOk = FALSE;
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
#define TB_NBTN    11      /* 选择 画笔 矩形 椭圆 箭头 马赛克 文字 序号 撤销 ✓ ✗ */
#define TB_ROW1    34      /* 第一行高（工具按钮） */
#define TB_ROW2    26      /* 第二行高（颜色 / 粗细 / 钉图） */
#define TB_H       (TB_ROW1 + TB_ROW2)
#define TB_W       358     /* 工具条总宽 */

/* 截图状态 */
typedef struct {
    HWND    hwnd;
    HBITMAP hbmCap, hbmDim, hbmBack;
    HDC     hdcCap, hdcDim, hdcBack;
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

/* 分配三份缓冲：原始 / 变暗底图 / 绘制后备 */
static BOOL OvAllocBuffers(int w, int h)
{
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
    g_ov.hbmDim  = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, NULL, NULL, 0);
    g_ov.hbmBack = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, NULL, NULL, 0);
    if (!g_ov.hbmCap || !g_ov.hbmDim || !g_ov.hbmBack) {
        ReleaseDC(NULL, hdcScreen);
        return FALSE;
    }

    g_ov.hdcCap  = CreateCompatibleDC(hdcScreen);
    g_ov.hdcDim  = CreateCompatibleDC(hdcScreen);
    g_ov.hdcBack = CreateCompatibleDC(hdcScreen);
    ReleaseDC(NULL, hdcScreen);
    if (!g_ov.hdcCap || !g_ov.hdcDim || !g_ov.hdcBack) return FALSE;

    SelectObject(g_ov.hdcCap,  g_ov.hbmCap);
    SelectObject(g_ov.hdcDim,  g_ov.hbmDim);
    SelectObject(g_ov.hdcBack, g_ov.hbmBack);
    return TRUE;
}

/* 由 hdcCap 生成变暗底图（约 45% 亮度），并把原图 alpha 归位 */
static void OvDimBuffers(void)
{
    BitBlt(g_ov.hdcDim, 0, 0, g_ov.vw, g_ov.vh, g_ov.hdcCap, 0, 0, SRCCOPY);

    BITMAP bm;
    if (GetObjectW(g_ov.hbmDim, sizeof(bm), &bm)) {
        BYTE *dst = (BYTE *)bm.bmBits;
        if (dst) {
            size_t n = (size_t)g_ov.vw * g_ov.vh;
            for (size_t i = 0; i < n; i++) {
                BYTE *p = dst + i * 4;
                p[0] = (BYTE)(p[0] * 45 / 100);
                p[1] = (BYTE)(p[1] * 45 / 100);
                p[2] = (BYTE)(p[2] * 45 / 100);
                p[3] = 255;
            }
        }
    }

    BYTE *cap = (BYTE *)g_ov.pixCap;
    if (cap) {
        size_t n = (size_t)g_ov.vw * g_ov.vh;
        for (size_t i = 0; i < n; i++) cap[i * 4 + 3] = 255;
    }
}

static BOOL CaptureVirtualScreen(void)
{
    ZeroMemory(&g_ov, sizeof(g_ov));

    g_ov.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_ov.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_ov.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_ov.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_ov.vw <= 0 || g_ov.vh <= 0) return FALSE;

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) return FALSE;

    HDC hdcScreen = GetDC(NULL);
    if (!hdcScreen) return FALSE;

    /* 抓屏；带 CAPTUREBLT 才能抓到分层窗口，失败则退回普通模式 */
    if (!BitBlt(g_ov.hdcCap, 0, 0, g_ov.vw, g_ov.vh,
                hdcScreen, g_ov.vx, g_ov.vy, SRCCOPY | CAPTUREBLT)) {
        if (!BitBlt(g_ov.hdcCap, 0, 0, g_ov.vw, g_ov.vh,
                    hdcScreen, g_ov.vx, g_ov.vy, SRCCOPY)) {
            ReleaseDC(NULL, hdcScreen);
            return FALSE;
        }
    }
    ReleaseDC(NULL, hdcScreen);

    OvDimBuffers();
    return TRUE;
}

static void FreeCapture(void)
{
    if (g_ov.hdcCap)  { DeleteDC(g_ov.hdcCap);  g_ov.hdcCap = NULL; }
    if (g_ov.hdcDim)  { DeleteDC(g_ov.hdcDim);  g_ov.hdcDim = NULL; }
    if (g_ov.hdcBack) { DeleteDC(g_ov.hdcBack); g_ov.hdcBack = NULL; }
    if (g_ov.hbmCap)  { DeleteObject(g_ov.hbmCap);  g_ov.hbmCap = NULL; }
    if (g_ov.hbmDim)  { DeleteObject(g_ov.hbmDim);  g_ov.hbmDim = NULL; }
    if (g_ov.hbmBack) { DeleteObject(g_ov.hbmBack); g_ov.hbmBack = NULL; }
    g_ov.pixCap = NULL;
}

/* ---------------------------------------------------------------- 读像素 / 输出 */

static void DrawPenStroke(HDC hdc, const Annot *a)
{
    if (a->npts < 2) return;
    HPEN pen = CreatePen(PS_SOLID, a->width, a->color);
    HGDIOBJ old = SelectObject(hdc, pen);
    Polyline(hdc, a->pts, a->npts);
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void DrawEllipseAnnot(HDC hdc, const Annot *a)
{
    HPEN pen = CreatePen(PS_SOLID, a->width, a->color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Ellipse(hdc, a->rc.left, a->rc.top, a->rc.right, a->rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void DrawRectAnnot(HDC hdc, const Annot *a)
{
    HPEN pen = CreatePen(PS_SOLID, a->width, a->color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, a->rc.left, a->rc.top, a->rc.right, a->rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

static void DrawArrowAnnot(HDC hdc, const Annot *a)
{
    if (a->npts < 2) return;

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
    HGDIOBJ oldPen = SelectObject(hdc, pen);

    /* 杆：缩短一点，别从箭头三角里穿出来 */
    MoveToEx(hdc, x1, y1, NULL);
    LineTo(hdc, (int)(x2 - hl * 0.55 * cos(ang)), (int)(y2 - hl * 0.55 * sin(ang)));

    HBRUSH br = CreateSolidBrush(a->color);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldBk = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, head, 3);
    SelectObject(hdc, oldBk);
    SelectObject(hdc, oldBr);
    DeleteObject(br);

    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

/* 马赛克：把目标区域切块、逐块取平均色涂平。
 * 直接读 hdc 上现有的像素，所以实时预览和烘焙成图两条路都能复用。 */
static void DrawMosaicAnnot(HDC hdc, const Annot *a)
{
    int w = a->rc.right - a->rc.left;
    int h = a->rc.bottom - a->rc.top;
    if (w < 2 || h < 2) return;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC screen = GetDC(NULL);
    HBITMAP hbm = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!hbm || !bits) { if (hbm) DeleteObject(hbm); return; }

    HDC dc = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(dc, hbm);

    BitBlt(dc, 0, 0, w, h, hdc, a->rc.left, a->rc.top, SRCCOPY);

    int bs = MOSAIC_BLOCK;
    BYTE *p = (BYTE *)bits;
    for (int by = 0; by < h; by += bs) {
        int bh = min(bs, h - by);
        for (int bx = 0; bx < w; bx += bs) {
            int bw = min(bs, w - bx);
            long sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int y = 0; y < bh; y++) {
                BYTE *row = p + ((size_t)(by + y) * w + bx) * 4;
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
                BYTE *row = p + ((size_t)(by + y) * w + bx) * 4;
                for (int x = 0; x < bw; x++) {
                    row[x * 4 + 0] = ab;
                    row[x * 4 + 1] = ag;
                    row[x * 4 + 2] = ar;
                }
            }
        }
    }

    BitBlt(hdc, a->rc.left, a->rc.top, w, h, dc, 0, 0, SRCCOPY);

    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(hbm);
}

/* 序号：实心圆 + 白色数字 */
static void DrawNumberAnnot(HDC hdc, const Annot *a)
{
    if (!a->text[0]) return;

    int d = a->fontH;                       /* 直径 */
    int l = a->x - d / 2, t = a->y - d / 2;
    RECT r = { l, t, l + d, t + d };

    HBRUSH br = CreateSolidBrush(a->color);
    HGDIOBJ oldBr = SelectObject(hdc, br);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Ellipse(hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(br);

    HFONT f = CreateFontW(-(int)(d * 0.58), 0, 0, 0, FW_BOLD, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei UI");
    HGDIOBJ oldF = SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, a->text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);
    DeleteObject(f);
}

static void DrawTextAnnot(HDC hdc, const Annot *a)
{
    if (!a->text[0]) return;
    HFONT f = CreateFontW(-a->fontH, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei UI");
    HGDIOBJ oldF = SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, a->color);
    RECT r = { a->x, a->y, a->x + 4000, a->y + a->fontH * 3 };
    DrawTextW(hdc, a->text, -1, &r, DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE);
    SelectObject(hdc, oldF);
    DeleteObject(f);
}

/* 全部标注画到 hdc，坐标就是遮罩客户端坐标 */
static void DrawAnnotations(HDC hdc)
{
    for (int i = 0; i < g_annCount; i++) {
        switch (g_ann[i].kind) {
        case AN_PEN:     DrawPenStroke(hdc, &g_ann[i]);    break;
        case AN_RECT:    DrawRectAnnot(hdc, &g_ann[i]);    break;
        case AN_ELLIPSE: DrawEllipseAnnot(hdc, &g_ann[i]); break;
        case AN_ARROW:   DrawArrowAnnot(hdc, &g_ann[i]);   break;
        case AN_MOSAIC:  DrawMosaicAnnot(hdc, &g_ann[i]);  break;
        case AN_TEXT:    DrawTextAnnot(hdc, &g_ann[i]);    break;
        case AN_NUMBER:  DrawNumberAnnot(hdc, &g_ann[i]);  break;
        }
    }
}

/* 生成成品位图（bottom-up 32bpp DIB Section），标注一并烘焙进去。
 * *ppBits 返回像素指针，布局正好是 CF_DIB 要的 bottom-up。 */
static HBITMAP BuildFinalBitmap(int x, int y, int w, int h, void **ppBits)
{
    if (ppBits) *ppBits = NULL;
    if (w <= 0 || h <= 0 || !g_ov.hdcCap) return NULL;

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
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdcScreen);
    if (!hbm || !bits) { if (hbm) DeleteObject(hbm); return NULL; }

    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(dc, hbm);

    BitBlt(dc, 0, 0, w, h, g_ov.hdcCap, x, y, SRCCOPY);

    /* 挪视口原点，标注坐标不必逐个减 */
    if (g_annCount > 0) {
        SetViewportOrgEx(dc, -x, -y, NULL);
        DrawAnnotations(dc);
        SetViewportOrgEx(dc, 0, 0, NULL);
    }

    SelectObject(dc, old);
    DeleteDC(dc);

    /* BitBlt 会把 alpha 清零，补回不透明 */
    BYTE *p = (BYTE *)bits;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) p[i * 4 + 3] = 255;

    if (ppBits) *ppBits = bits;
    return hbm;
}

static void CopyDibToClipboard(const void *bits, int w, int h)
{
    if (!bits || w <= 0 || h <= 0) return;
    DWORD imgSize = (DWORD)w * 4 * (DWORD)h;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imgSize);
    if (!hMem) return;
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)GlobalLock(hMem);
    if (!bih) { GlobalFree(hMem); return; }

    ZeroMemory(bih, sizeof(*bih));
    bih->biSize        = sizeof(BITMAPINFOHEADER);
    bih->biWidth       = w;
    bih->biHeight      = h;          /* 正数 = bottom-up */
    bih->biPlanes      = 1;
    bih->biBitCount    = 32;
    bih->biCompression = BI_RGB;
    bih->biSizeImage   = imgSize;
    memcpy((BYTE *)bih + sizeof(BITMAPINFOHEADER), bits, imgSize);
    GlobalUnlock(hMem);

    if (OpenClipboard(g_hMsgWnd)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_DIB, hMem)) GlobalFree(hMem);
        CloseClipboard();
    } else {
        GlobalFree(hMem);
    }
}

static BOOL SaveBitmapToPng(HBITMAP hbm, const WCHAR *path)
{
    if (!hbm) return FALSE;
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
        FillRect(dc, &rc, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(238, 238, 238));
        SelectObject(dc, g_fontToast);
        DrawTextW(dc, g_toastText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    SelectObject(dc, g_fontToast);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, g_toastText, lstrlenW(g_toastText), &sz);
    ReleaseDC(NULL, dc);

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
    if (rgn) SetWindowRgn(g_toast, rgn, TRUE);

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
 *   第一行：工具组(0..7) | 撤销(8) | ✓(9) ✗(10)
 *   第二行：颜色 | 粗细 | 钉住 */
static int BtnX(int i)
{
    if (i <= 7) return 6 + i * TB_BTN;       /* 工具组 6..246 */
    if (i == 8) return 254;                  /* 撤销   254..284 */
    return 292 + (i - 9) * TB_BTN;           /* ✓292 ✗322 */
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
        FillRect(hdc, &r, b);
        DeleteObject(b);

        BOOL sel = (g_annColor == g_palette[i]);
        BOOL hot = (g_ov.hotSwatch == i);

        COLORREF ec = sel ? RGB(25, 25, 25)
                    : hot ? RGB(70, 70, 70)
                          : RGB(145, 145, 145);
        HBRUSH edge = CreateSolidBrush(ec);
        FrameRect(hdc, &r, edge);
        if (sel) {
            RECT o = { r.left - 1, r.top - 1, r.right + 1, r.bottom + 1 };
            FrameRect(hdc, &o, edge);
        }
        DeleteObject(edge);
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
        FrameRect(hdc, &r, edge);
        DeleteObject(edge);
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
    FrameRect(hdc, &r, edge);
    DeleteObject(edge);
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

    /* 行分隔 + 分组竖线 */
    HGDIOBJ oldPen = SelectObject(hdc, g_penSep);
    int ry = ROW2_Y(t);
    MoveToEx(hdc, t.left + 4, ry, NULL);
    LineTo(hdc, t.right - 4, ry);

    int v1 = t.left + BtnX(BTN_UNDO) - 4;
    int v2 = t.left + BtnX(BTN_OK) - 4;
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
    SelectObject(hdc, oldPen);

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

        /* 选中框：外白内蓝 */
        HGDIOBJ oldBr = SelectObject(g_ov.hdcBack, GetStockObject(NULL_BRUSH));

        HGDIOBJ oldPen = SelectObject(g_ov.hdcBack, g_penOuter);
        Rectangle(g_ov.hdcBack, s.left - 1, s.top - 1, s.right + 1, s.bottom + 1);
        SelectObject(g_ov.hdcBack, oldPen);

        oldPen = SelectObject(g_ov.hdcBack, g_penSel);
        Rectangle(g_ov.hdcBack, s.left, s.top, s.right, s.bottom);
        SelectObject(g_ov.hdcBack, oldPen);

        SelectObject(g_ov.hdcBack, oldBr);

        /* 尺寸标签：贴选区左上角外侧，放不下就挪进去 */
        WCHAR label[64];
        wsprintfW(label, L"%d × %d", SelW(), SelH());

        SelectObject(g_ov.hdcBack, g_fontLabel);
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
        SetBkMode(g_ov.hdcBack, TRANSPARENT);
        SetTextColor(g_ov.hdcBack, RGB(255, 255, 255));
        DrawTextW(g_ov.hdcBack, label, -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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

static void TextCommit(void)
{
    if (!g_edit) return;

    WCHAR buf[256] = {0};
    GetWindowTextW(g_edit, buf, 256);

    HWND e = g_edit;
    g_edit = NULL;
    DestroyWindow(e);
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
    OverlayRedraw();
}

static void TextCancel(void)
{
    if (!g_edit) return;
    HWND e = g_edit;
    g_edit = NULL;
    g_editEditing = -1;
    DestroyWindow(e);
    if (g_ov.hwnd) SetFocus(g_ov.hwnd);
    OverlayRedraw();
}

/* 量出一条文字标注占多大地方，用于判断有没有点到它 */
static void TextAnnotRect(const Annot *a, RECT *out)
{
    HDC dc = GetDC(NULL);
    HFONT f = CreateFontW(-a->fontH, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                          L"Microsoft YaHei UI");
    HGDIOBJ of = SelectObject(dc, f);
    SIZE s = {0, 0};
    GetTextExtentPoint32W(dc, a->text, lstrlenW(a->text), &s);
    SelectObject(dc, of);
    DeleteObject(f);
    ReleaseDC(NULL, dc);

    out->left   = a->x;
    out->top    = a->y;
    out->right  = a->x + s.cx;
    out->bottom = a->y + s.cy;
}

static void TextStart(int x, int y)
{
    TextCommit();                          /* 先落定上一个 */

    /* 点在已有文字上 → 二次编辑它 */
    int hitIdx = -1;
    for (int i = g_annCount - 1; i >= 0; i--) {
        if (g_ann[i].kind != AN_TEXT) continue;
        RECT r;
        TextAnnotRect(&g_ann[i], &r);
        r.left -= 4; r.top -= 4; r.right += 4; r.bottom += 4;   /* 放宽一点好点中 */
        if (PtIn(r, x, y)) { hitIdx = i; break; }
    }

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

    /* 编辑框的字号跟着当前档位走 */
    HFONT ef = CreateFontW(-fontH, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           L"Microsoft YaHei UI");

    g_edit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL | ES_NOHIDESEL,
        x, y, ew, eh, g_ov.hwnd, (HMENU)(INT_PTR)ID_EDIT, g_hInst, NULL);
    if (!g_edit) {
        if (ef) DeleteObject(ef);
        QSLog("TextStart: CreateWindow EDIT failed");
        return;
    }

    /* 用一次性字体；EDIT 内部会自己存一份，销毁后即可释放 */
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)ef, TRUE);
    DeleteObject(ef);

    g_editOldProc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    g_editEditing = hitIdx;

    if (hitIdx >= 0) {
        SetWindowTextW(g_edit, g_ann[hitIdx].text);
        SendMessageW(g_edit, EM_SETSEL, 0, -1);    /* 全选，直接打字即替换 */
    }

    SetFocus(g_edit);
    QSLog("TextStart at %d,%d editing=%d", x, y, g_editEditing);
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
    CopyDibToClipboard(pd->bits, pd->w, pd->h);
    PinToast(h, L"已复制到剪贴板");
    QSLog("pin: copy %dx%d", pd->w, pd->h);
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
            FrameRect(dc, &r, b);
            DeleteObject(b);
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
                if (pd->oldBmp) SelectObject(pd->hdc, pd->oldBmp);  /* 先选回去，否则位图删不掉 */
                DeleteDC(pd->hdc);
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
    pd->oldBmp = (HBITMAP)SelectObject(pd->hdc, hbm);

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        PIN_CLASS, L"", WS_POPUP,
        x, y, w, h, NULL, NULL, g_hInst, pd);
    if (!hw) {
        DeleteDC(pd->hdc);
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

    BOOL saved = FALSE;
    WCHAR msg[512];

    if (hbm && bits) {
        CopyDibToClipboard(bits, w, h);

        if (g_saveFile) {
            WCHAR name[128], full[MAX_PATH * 2];
            SYSTEMTIME st;
            GetLocalTime(&st);
            wsprintfW(name, L"截图_%04d%02d%02d_%02d%02d%02d.png",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            lstrcpynW(full, g_saveDir, MAX_PATH);
            lstrcatW(full, L"\\");
            lstrcatW(full, name);
            saved = SaveBitmapToPng(hbm, full);
        }
    }
    if (hbm) DeleteObject(hbm);

    if (!g_toast) {
        if (saved) lstrcpynW(msg, L"已复制  ·  已保存到 Screenshots", 512);
        else       lstrcpynW(msg, L"已复制到剪贴板", 512);
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
                QSLog("overlay: new selection (clicked outside)");
            }
        } else {
            g_ov.state  = ST_NEW;
            g_ov.hasSel = TRUE;
            g_ov.sel    = RectNorm(x, y, x, y);
            g_annCount  = 0;
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
                if (!keep) g_annCount--;
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
        if (g_edit) { HWND e = g_edit; g_edit = NULL; DestroyWindow(e); }
        g_annCount = 0;
        FreeCapture();
        OverlayGdiFree();
        g_ov.hwnd = NULL;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        if (g_ov.hwnd == h) g_ov.hwnd = NULL;
        FreeCapture();
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

static void RegisterHotkeyNow(void)
{
    if (g_hotkeyOk) {
        UnregisterHotKey(g_hMsgWnd, HOTKEY_ID);
        g_hotkeyOk = FALSE;
    }
    g_hotkeyOk = RegisterHotKey(g_hMsgWnd, HOTKEY_ID,
                                g_mods | MOD_NOREPEAT, g_vk);
}

static void ShowTrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    HMENU hk = CreatePopupMenu();

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
    lstrcpynW(g_nid.szTip, L"QuickShot 截图  (Ctrl+Alt+A)", 128);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
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
        if (LOWORD(wp) == HOTKEY_ID) StartCapture();
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
            g_mods = g_presets[idx].mods;
            g_vk   = g_presets[idx].vk;
            SaveSettingDword(L"HotMods", g_mods);
            SaveSettingDword(L"HotVk", g_vk);
            RegisterHotkeyNow();
            if (!g_hotkeyOk) {
                MessageBoxW(NULL,
                            L"该快捷键已被其他程序占用，请换一个。",
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

    if (!OvAllocBuffers(g_ov.vw, g_ov.vh)) { GdipDisposeImage((GpImage *)bmp); return 1; }

    /* 把图铺进"截图"缓冲 */
    GpGraphics *gp = NULL;
    if (GdipCreateFromHDC(g_ov.hdcCap, &gp) != 0 || !gp) {
        GdipDisposeImage((GpImage *)bmp);
        return 1;
    }
    GdipSetCompositingMode(gp, CompositingModeSourceCopy);
    GdipSetInterpolationMode(gp, InterpolationModeHighQualityBicubic);
    GdipDrawImageRectI(gp, (GpImage *)bmp, 0, 0, g_ov.vw, g_ov.vh);
    GdipDeleteGraphics(gp);
    GdipDisposeImage((GpImage *)bmp);

    OvDimBuffers();

    /* 界面状态：选中一块区域，工具停在椭圆 */
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
    HBITMAP out = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &obits, NULL, 0);
    ReleaseDC(NULL, sdc);
    if (!out || !obits) { FreeCapture(); return 1; }

    HDC odc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(odc, out);
    OverlayDraw(odc);
    SelectObject(odc, old);
    DeleteDC(odc);

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
    HGDIOBJ old = SelectObject(dc, hbm);

    RECT full = { 0, 0, w, h };
    FillRect(dc, &full, (HBRUSH)GetStockObject(GRAY_BRUSH));

    SetViewportOrgEx(dc, -t.left, -t.top, NULL);
    DrawToolbar(dc);
    SetViewportOrgEx(dc, 0, 0, NULL);

    SelectObject(dc, old);
    DeleteDC(dc);

    WCHAR out[MAX_PATH];
    MakeSiblingPath(L"toolbar.png", out, MAX_PATH);
    BOOL ok = SaveBitmapToPng(hbm, out);
    DeleteObject(hbm);
    OverlayGdiFree();
    printf("png: %s -> %ls\n", ok ? "ok" : "FAIL", out);
    return (ok && bad == 0 && curBad == 0) ? 0 : 1;
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
        MessageBoxW(NULL, L"GDI+ 初始化失败。", L"QuickShot", MB_ICONERROR | MB_OK);
        return 1;
    }

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
    RegisterHotkeyNow();
    QSLog("hotkey registered=%d mods=0x%X vk=0x%X", (int)g_hotkeyOk, g_mods, g_vk);

    if (!g_hotkeyOk) {
        MessageBoxW(NULL,
                    L"快捷键 Ctrl+Alt+A 注册失败，可能被 QQ 等程序占用了。\n\n"
                    L"请右键托盘图标，在「快捷键」里换一个组合。",
                    L"QuickShot", MB_ICONWARNING | MB_OK);
    }

    {
        WCHAR tip[160];
        wsprintfW(tip, L"已启动  ·  按 %s 截图",
                  (g_hotkeyOk && g_mods == (MOD_CONTROL | MOD_ALT) && g_vk == 'A')
                      ? L"Ctrl+Alt+A" : L"已设置的快捷键");
        ToastShowAt(tip,
                    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) / 2,
                    GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) - 90);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hotkeyOk) UnregisterHotKey(g_hMsgWnd, HOTKEY_ID);
    FreeCapture();
    if (g_fontLabel) DeleteObject(g_fontLabel);
    if (g_fontToast) DeleteObject(g_fontToast);
    if (g_fontText)  DeleteObject(g_fontText);
    if (g_fontIcon)  DeleteObject(g_fontIcon);
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
