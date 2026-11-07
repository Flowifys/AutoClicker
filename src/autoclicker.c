// Auto Clicker - a fast, lightweight auto clicker for Windows with a Windows XP look.
//
//   F6   start / stop (works from any window)
//   Esc  emergency stop (only captured while clicking)
//
// Build: see build.bat / build.sh (Zig or MinGW-w64).
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include "../res/resource.h"

// ================================================================ layout (96-dpi units)
#define WIN_W     460
#define WIN_H     474
#define TITLE_H   30
#define FRAME     3
#define STATUS_H  24
static const RECT PANEL   = { 16, 140, 444, 432 };
static const RECT G_SPEED = { 28, 150, 432, 238 };
static const RECT G_CLICK = { 28, 246, 432, 302 };
static const RECT G_STOP  = { 28, 310, 432, 366 };

enum {
    ID_MODE_INT = 101, ID_MODE_MAX, ID_HOURS, ID_MINS, ID_SECS, ID_MS, ID_CPS,
    ID_BUTTON, ID_TYPE, ID_STOP_NEVER, ID_STOP_AFTER, ID_STOP_CNT, ID_START, ID_STOP
};
#define HOTKEY_F6   1
#define HOTKEY_ESC  2
#define TIMER_UI    1
#define WM_CLICKER_DONE (WM_APP + 1)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// ================================================================ Windows XP "Luna" palette
#define XP_BEIGE        RGB(0xEC, 0xE9, 0xD8)   // classic XP window face
#define XP_GROUP_LINE   RGB(0xD0, 0xD0, 0xBF)
#define XP_GROUP_TEXT   RGB(0x00, 0x46, 0xD5)
#define XP_PANEL_EDGE   RGB(0x7A, 0x96, 0xDF)
#define XP_TEXT         RGB(0x00, 0x00, 0x00)
#define XP_DIS_TEXT     RGB(0xAC, 0xA8, 0x99)
#define XP_TITLE_TOP    RGB(0x3D, 0x95, 0xFF)
#define XP_TITLE_MID    RGB(0x00, 0x58, 0xEE)
#define XP_TITLE_BOT    RGB(0x00, 0x44, 0xD0)
#define XP_FRAME        RGB(0x00, 0x55, 0xE5)
#define XP_FRAME_DARK   RGB(0x00, 0x2C, 0x9E)

// ================================================================ state
static HINSTANCE g_inst;
static HWND g_hwnd;
static HWND hModeInt, hModeMax, hHours, hMins, hSecs, hMs, hCps, hButton, hType;
static HWND hStopNever, hStopAfter, hStopCnt, hStart, hStop;
static HFONT fUI, fGroup, fTitle, fBig, fHead, fSmall, fBtn;
static HBRUSH brBeige;
static HBITMAP g_bg;            // background, pre-scaled to the window size
static HICON g_icon16;
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)

// title bar buttons
enum { TB_NONE, TB_MIN, TB_CLOSE };
static int g_hot = TB_NONE, g_down = TB_NONE;
static BOOL g_tracking = FALSE;

// click engine
static volatile LONG g_running = 0;
static volatile LONG64 g_sent = 0;
static HANDLE g_thread = NULL, g_stopEvent = NULL, g_timer = NULL;
typedef struct { DWORD interval; int button, dbl, limit, maxspeed, cps; } ClickCfg;
static ClickCfg g_cfg;

// live clicks-per-second
#define NSAMP 11
static LONGLONG sampT[NSAMP]; static LONG64 sampC[NSAMP]; static int sampN = 0, sampI = 0;
static double g_liveCps = 0, g_peakCps = 0;
static LARGE_INTEGER g_qpf;

// ================================================================ helpers
static int GetInt(HWND h) { wchar_t b[32]; GetWindowTextW(h, b, 32); int v = _wtoi(b); return v < 0 ? 0 : v; }
static BOOL Checked(HWND h) { return SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void SetRadio(HWND on, HWND off) {
    SendMessage(on, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(off, BM_SETCHECK, BST_UNCHECKED, 0);
}
static RECT SR(RECT r) { RECT o = { S(r.left), S(r.top), S(r.right), S(r.bottom) }; return o; }
static RECT SRect(int l, int t, int r, int b) { RECT o = { S(l), S(t), S(r), S(b) }; return o; }
static void FmtInt(wchar_t *out, int cap, long long v) {       // 12345 -> "12,345"
    wchar_t tmp[32]; int n = swprintf(tmp, 32, L"%lld", v), o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) {
        out[o++] = tmp[i];
        int rem = n - i - 1;
        if (rem > 0 && rem % 3 == 0 && o < cap - 1) out[o++] = L',';
    }
    out[o] = 0;
}

// ================================================================ click engine
// Precise, interruptible wait (high-resolution timer, not limited to the 15.6 ms tick).
static BOOL WaitMs(double ms) {
    if (!g_timer) {
        g_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!g_timer) g_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(ms * 10000.0);
    if (due.QuadPart == 0) due.QuadPart = -1;
    SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE);
    HANDLE h[2] = { g_stopEvent, g_timer };
    return WaitForMultipleObjects(2, h, FALSE, INFINITE) == WAIT_OBJECT_0;
}

static void ButtonFlags(int button, DWORD *down, DWORD *up) {
    switch (button) {
        case 1: *down = MOUSEEVENTF_RIGHTDOWN;  *up = MOUSEEVENTF_RIGHTUP;  break;
        case 2: *down = MOUSEEVENTF_MIDDLEDOWN; *up = MOUSEEVENTF_MIDDLEUP; break;
        default:*down = MOUSEEVENTF_LEFTDOWN;   *up = MOUSEEVENTF_LEFTUP;   break;
    }
}
static void SendClick(int button) {
    DWORD d, u; ButtonFlags(button, &d, &u);
    INPUT in[2] = {0};
    in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = d;
    in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = u;
    SendInput(2, in, sizeof(INPUT));
    InterlockedIncrement64(&g_sent);
}

// Max speed:
//  * Each INPUT carries both press and release (DOWN|UP), so one click = one input event.
//  * Clicks are evenly spaced using a spin-wait on the high-precision counter.
//  * If Windows accepts clicks slower than the target, the owed clicks go out together,
//    but never more than MINI_BURST at once - big bursts overflow Windows' input queue,
//    which makes the PC beep and drops clicks.
//  * If we fall far behind (PC busy), we skip ahead instead of bursting to catch up.
#define MINI_BURST 8
static void MaxSpeedLoop(void) {
    DWORD d, u; ButtonFlags(g_cfg.button, &d, &u);
    INPUT in[MINI_BURST * 2];
    ZeroMemory(in, sizeof(in));
    for (int i = 0; i < MINI_BURST * 2; i++) { in[i].type = INPUT_MOUSE; in[i].mi.dwFlags = d | u; }
    const int per = g_cfg.dbl ? 2 : 1;
    const LONGLONG period = (LONGLONG)((double)g_qpf.QuadPart / (double)g_cfg.cps);
    const LONGLONG slack = g_qpf.QuadPart / 50;
    long long target = g_cfg.limit > 0 ? g_cfg.limit : -1, done = 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG next = now.QuadPart;
    while (g_running) {
        if (target >= 0 && done >= target) break;
        for (;;) {
            QueryPerformanceCounter(&now);
            LONGLONG left = next - now.QuadPart;
            if (left <= 0) break;
            if (left > g_qpf.QuadPart / 500) { if (WaitMs(1.0)) return; }
            else YieldProcessor();
        }
        long long owed = 1 + (now.QuadPart - next) / period;
        if (owed > MINI_BURST) owed = MINI_BURST;
        if (target >= 0 && done + owed > target) owed = target - done;
        UINT ok = SendInput((UINT)(owed * per), in, sizeof(INPUT));
        InterlockedExchangeAdd64(&g_sent, ok);
        done += owed;
        next += period * owed;
        if (now.QuadPart - next > slack) next = now.QuadPart;
        if ((done & 63) < owed && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) return;
    }
}

static DWORD WINAPI ClickThread(LPVOID p) {
    (void)p;
    timeBeginPeriod(1);
    if (g_cfg.maxspeed) {
        MaxSpeedLoop();
    } else {
        int count = 0;
        while (g_running) {
            SendClick(g_cfg.button);
            if (g_cfg.dbl) { if (WaitMs(10.0)) break; SendClick(g_cfg.button); }
            count++;
            if (g_cfg.limit > 0 && count >= g_cfg.limit) break;
            if (WaitMs((double)g_cfg.interval)) break;
        }
    }
    timeEndPeriod(1);
    PostMessage(g_hwnd, WM_CLICKER_DONE, 0, 0);
    return 0;
}

// ================================================================ UI state
static void InvalidateLive(void) {
    RECT h = SRect(FRAME, TITLE_H, WIN_W - FRAME, PANEL.top);
    RECT s = SRect(FRAME, WIN_H - FRAME - STATUS_H, WIN_W - FRAME, WIN_H - FRAME);
    InvalidateRect(g_hwnd, &h, FALSE);
    InvalidateRect(g_hwnd, &s, FALSE);
}

static void UpdateUI(void) {
    BOOL on = g_running != 0, mx = Checked(hModeMax);
    HWND all[] = { hModeInt, hModeMax, hHours, hMins, hSecs, hMs, hCps, hButton, hType, hStopNever, hStopAfter, hStopCnt };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) EnableWindow(all[i], !on);
    if (!on) {
        EnableWindow(hHours, !mx); EnableWindow(hMins, !mx); EnableWindow(hSecs, !mx); EnableWindow(hMs, !mx);
        EnableWindow(hCps, mx);
        EnableWindow(hStopCnt, Checked(hStopAfter));
    }
    EnableWindow(hStart, !on);
    EnableWindow(hStop, on);
    InvalidateLive();
}

static void StartClicking(void) {
    if (g_running) return;
    DWORD ms = (DWORD)GetInt(hHours) * 3600000u + (DWORD)GetInt(hMins) * 60000u +
               (DWORD)GetInt(hSecs) * 1000u + (DWORD)GetInt(hMs);
    g_cfg.interval = ms < 1 ? 1 : ms;
    g_cfg.maxspeed = Checked(hModeMax);
    g_cfg.cps = GetInt(hCps);
    if (g_cfg.cps < 1) g_cfg.cps = 1;
    if (g_cfg.cps > 100000) g_cfg.cps = 100000;
    g_cfg.button = (int)SendMessage(hButton, CB_GETCURSEL, 0, 0);
    g_cfg.dbl = (int)SendMessage(hType, CB_GETCURSEL, 0, 0);
    g_cfg.limit = Checked(hStopAfter) ? GetInt(hStopCnt) : 0;
    if (Checked(hStopAfter) && g_cfg.limit == 0) g_cfg.limit = 1;

    InterlockedExchange64(&g_sent, 0);
    sampN = sampI = 0; g_liveCps = 0; g_peakCps = 0;
    ResetEvent(g_stopEvent);
    InterlockedExchange(&g_running, 1);
    RegisterHotKey(g_hwnd, HOTKEY_ESC, MOD_NOREPEAT, VK_ESCAPE);
    g_thread = CreateThread(NULL, 0, ClickThread, NULL, 0, NULL);
    SetThreadPriority(g_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    UpdateUI();
}

static void StopClicking(void) {
    if (!g_running) return;
    InterlockedExchange(&g_running, 0);
    SetEvent(g_stopEvent);
}

static void SampleCps(void) {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONG64 c = g_sent;
    sampT[sampI] = now.QuadPart; sampC[sampI] = c;
    sampI = (sampI + 1) % NSAMP;
    if (sampN < NSAMP) sampN++;
    double dt = 0;
    if (sampN >= 2) {
        int oldest = (sampI - sampN + NSAMP) % NSAMP;
        dt = (double)(now.QuadPart - sampT[oldest]) / (double)g_qpf.QuadPart;
        if (dt > 0) g_liveCps = (double)(c - sampC[oldest]) / dt;
    }
    // Peak only counts once we have close to a full second of data (by time, not by
    // sample count, so it still works if the timer is delayed by a busy message queue).
    if (g_running && dt >= 0.9 && g_liveCps > g_peakCps) g_peakCps = g_liveCps;
    if (!g_running && g_liveCps < 0.5) g_liveCps = 0;
    InvalidateLive();
}

// ================================================================ drawing helpers
static void VGradient(HDC dc, RECT r, COLORREF top, COLORREF bot) {
    TRIVERTEX v[2] = {
        { r.left,  r.top,    (COLOR16)(GetRValue(top) << 8), (COLOR16)(GetGValue(top) << 8), (COLOR16)(GetBValue(top) << 8), 0 },
        { r.right, r.bottom, (COLOR16)(GetRValue(bot) << 8), (COLOR16)(GetGValue(bot) << 8), (COLOR16)(GetBValue(bot) << 8), 0 },
    };
    GRADIENT_RECT g = { 0, 1 };
    GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_V);
}

// Rounded rectangle with a vertical gradient fill and a 1px border.
static void RoundGradient(HDC dc, RECT r, int rad, COLORREF top, COLORREF bot, COLORREF border) {
    HRGN rg = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
    SelectClipRgn(dc, rg);
    VGradient(dc, r, top, bot);
    SelectClipRgn(dc, NULL);
    HBRUSH b = CreateSolidBrush(border);
    FrameRgn(dc, rg, b, 1, 1);
    DeleteObject(b); DeleteObject(rg);
}

static void FillRoundRect(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}

static void ShadowText(HDC dc, const wchar_t *t, RECT r, UINT fmt, COLORREF fg, COLORREF sh, int off) {
    RECT s = r; OffsetRect(&s, off, off);
    SetTextColor(dc, sh); DrawTextW(dc, t, -1, &s, fmt);
    SetTextColor(dc, fg); DrawTextW(dc, t, -1, &r, fmt);
}

static RECT TitleButtonRect(int which) {
    int right = WIN_W - FRAME - 5;
    if (which == TB_CLOSE) return SRect(right - 21, 5, right, 26);
    return SRect(right - 21 - 3 - 21, 5, right - 21 - 3, 26);
}

// ================================================================ painting
static void PaintTitleBar(HDC dc, int w) {
    RECT tb = { 0, 0, w, S(TITLE_H) };
    RECT top = tb; top.bottom = S(7);
    RECT bot = tb; bot.top = S(7);
    VGradient(dc, top, XP_TITLE_TOP, XP_TITLE_MID);
    VGradient(dc, bot, XP_TITLE_MID, XP_TITLE_BOT);

    DrawIconEx(dc, S(8), S(7), g_icon16, S(16), S(16), 0, NULL, DI_NORMAL);
    SelectObject(dc, fTitle);
    RECT tr = { S(30), 0, w - S(60), S(TITLE_H) };
    ShadowText(dc, L"Auto Clicker", tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER, RGB(255, 255, 255), RGB(0x0A, 0x1E, 0x6E), 1);

    for (int which = TB_MIN; which <= TB_CLOSE; which++) {
        RECT r = TitleButtonRect(which);
        BOOL hot = g_hot == which, down = g_down == which && hot;
        COLORREF t, b;
        if (which == TB_CLOSE) { t = hot ? RGB(0xF3, 0x8E, 0x75) : RGB(0xE8, 0x72, 0x55); b = down ? RGB(0x9E, 0x2A, 0x10) : RGB(0xC4, 0x3A, 0x1A); }
        else                   { t = hot ? RGB(0x6A, 0xA8, 0xFF) : RGB(0x4A, 0x90, 0xFF); b = down ? RGB(0x0A, 0x3C, 0xB0) : RGB(0x1A, 0x5C, 0xE0); }
        RoundGradient(dc, r, S(5), t, b, RGB(255, 255, 255));
        HPEN pen = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, k = S(4);
        if (which == TB_CLOSE) {
            MoveToEx(dc, cx - k, cy - k, NULL); LineTo(dc, cx + k + 1, cy + k + 1);
            MoveToEx(dc, cx + k, cy - k, NULL); LineTo(dc, cx - k - 1, cy + k + 1);
        } else {
            MoveToEx(dc, cx - k, cy + k - S(1), NULL); LineTo(dc, cx + S(1), cy + k - S(1));
        }
        SelectObject(dc, op); DeleteObject(pen);
    }
}

static void PaintFrame(HDC dc, int w, int h) {
    HBRUSH fb = CreateSolidBrush(XP_FRAME);
    RECT l = { 0, S(TITLE_H), S(FRAME), h }, r = { w - S(FRAME), S(TITLE_H), w, h }, b = { 0, h - S(FRAME), w, h };
    FillRect(dc, &l, fb); FillRect(dc, &r, fb); FillRect(dc, &b, fb);
    DeleteObject(fb);
    HBRUSH db = CreateSolidBrush(XP_FRAME_DARK);
    RECT ol = { 0, 0, 1, h }, orr = { w - 1, 0, w, h }, ob = { 0, h - 1, w, h };
    FillRect(dc, &ol, db); FillRect(dc, &orr, db); FillRect(dc, &ob, db);
    DeleteObject(db);
}

static void PaintHeader(HDC dc, int w) {
    BOOL on = g_running != 0;
    SetBkMode(dc, TRANSPARENT);
    wchar_t num[32]; FmtInt(num, 32, (long long)(g_liveCps + 0.5));
    SelectObject(dc, fBig);
    RECT n = { 0, S(34), w, S(94) };
    ShadowText(dc, num, n, DT_CENTER | DT_SINGLELINE | DT_VCENTER,
               on ? RGB(255, 255, 255) : RGB(0xE4, 0xEE, 0xFF), RGB(0x14, 0x36, 0x7A), S(2));
    SelectObject(dc, fHead);
    RECT l = { 0, S(92), w, S(110) };
    ShadowText(dc, L"clicks per second", l, DT_CENTER | DT_SINGLELINE | DT_VCENTER, RGB(255, 255, 255), RGB(0x14, 0x36, 0x7A), 1);
    wchar_t pk[32], tot[32], line[128];
    FmtInt(pk, 32, (long long)(g_peakCps + 0.5)); FmtInt(tot, 32, (long long)g_sent);
    swprintf(line, 128, L"peak %ls     total %ls", pk, tot);
    SelectObject(dc, fSmall);
    RECT p = { 0, S(110), w, S(128) };
    ShadowText(dc, line, p, DT_CENTER | DT_SINGLELINE | DT_VCENTER, RGB(0xF0, 0xF6, 0xFF), RGB(0x14, 0x36, 0x7A), 1);
}

static void PaintGroup(HDC dc, RECT gr, const wchar_t *title) {
    RECT r = SR(gr); r.top += S(7);
    FillRoundRect(dc, r, S(8), XP_BEIGE, XP_GROUP_LINE);
    SelectObject(dc, fGroup);
    SIZE sz; GetTextExtentPoint32W(dc, title, lstrlenW(title), &sz);
    RECT t = { S(gr.left) + S(8), S(gr.top), S(gr.left) + S(8) + sz.cx + S(6), S(gr.top) + S(15) };
    FillRect(dc, &t, brBeige);
    SetTextColor(dc, XP_GROUP_TEXT);
    DrawTextW(dc, title, -1, &t, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
}

static void PaintPanel(HDC dc) {
    RECT p = SR(PANEL);
    // soft drop shadow, then the panel
    RECT sh = p; OffsetRect(&sh, S(2), S(2));
    FillRoundRect(dc, sh, S(10), RGB(0x2E, 0x5A, 0x1E), RGB(0x2E, 0x5A, 0x1E));
    FillRoundRect(dc, p, S(10), XP_BEIGE, XP_PANEL_EDGE);
    SetBkMode(dc, TRANSPARENT);
    PaintGroup(dc, G_SPEED, L"Speed");
    PaintGroup(dc, G_CLICK, L"Click");
    PaintGroup(dc, G_STOP,  L"Stop after");
}

static void PaintStatusBar(HDC dc, int w, int h) {
    RECT sb = { S(FRAME), h - S(FRAME) - S(STATUS_H), w - S(FRAME), h - S(FRAME) };
    FillRect(dc, &sb, brBeige);
    HPEN hl = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(dc, hl);
    MoveToEx(dc, sb.left, sb.top, NULL); LineTo(dc, sb.right, sb.top);
    SelectObject(dc, op); DeleteObject(hl);

    int split = w - S(FRAME) - S(170);
    RECT p1 = { sb.left + S(3), sb.top + S(3), split - S(2), sb.bottom - S(2) };
    RECT p2 = { split, sb.top + S(3), sb.right - S(3), sb.bottom - S(2) };
    DrawEdge(dc, &p1, BDR_SUNKENOUTER, BF_RECT);
    DrawEdge(dc, &p2, BDR_SUNKENOUTER, BF_RECT);

    wchar_t status[128];
    if (!g_running) lstrcpyW(status, L"Ready");
    else if (g_cfg.maxspeed) {
        wchar_t t[32]; FmtInt(t, 32, g_cfg.cps);
        swprintf(status, 128, L"Clicking at max speed (target %ls/sec)", t);
    } else swprintf(status, 128, L"Clicking every %lu ms", (unsigned long)g_cfg.interval);
    SetBkMode(dc, TRANSPARENT); SelectObject(dc, fSmall); SetTextColor(dc, XP_TEXT);
    RECT t1 = p1; t1.left += S(6);
    DrawTextW(dc, status, -1, &t1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT t2 = p2; t2.left += S(6);
    DrawTextW(dc, L"F6 start/stop   Esc stop", -1, &t2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
}

// XP-style Start (green) and Stop (red) buttons.
static void DrawButton(DRAWITEMSTRUCT *di) {
    BOOL start = di->CtlID == ID_START;
    BOOL dis = (di->itemState & ODS_DISABLED) != 0, pressed = (di->itemState & ODS_SELECTED) != 0;
    HDC dc = di->hDC; RECT r = di->rcItem;
    FillRect(dc, &r, brBeige);
    COLORREF top, bot, border;
    if (dis)        { top = RGB(0xF5, 0xF4, 0xEA); bot = RGB(0xE3, 0xE1, 0xD5); border = RGB(0xC9, 0xC7, 0xBA); }
    else if (start) { top = RGB(0x6C, 0xC6, 0x5C); bot = RGB(0x2A, 0x8A, 0x24); border = RGB(0x1C, 0x5E, 0x18); }
    else            { top = RGB(0xF2, 0x80, 0x62); bot = RGB(0xC4, 0x36, 0x16); border = RGB(0x86, 0x22, 0x0E); }
    if (pressed && !dis) { COLORREF t = top; top = bot; bot = t; }
    RoundGradient(dc, r, S(12), top, bot, border);

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, fBtn);
    const wchar_t *label = start ? L"Start  (F6)" : L"Stop  (F6)";
    SIZE sz; GetTextExtentPoint32W(dc, label, lstrlenW(label), &sz);
    int icon = S(11), gap = S(10);
    int x0 = (r.left + r.right - (icon + gap + sz.cx)) / 2, cy = (r.top + r.bottom) / 2;
    if (pressed && !dis) { x0 += 1; cy += 1; }
    COLORREF fg = dis ? XP_DIS_TEXT : RGB(255, 255, 255);
    COLORREF shc = start ? RGB(0x14, 0x4A, 0x12) : RGB(0x6E, 0x1A, 0x08);
    for (int pass = dis ? 1 : 0; pass < 2; pass++) {       // pass 0 = shadow, 1 = face
        int o = pass == 0 ? 1 : 0;
        COLORREF c = pass == 0 ? shc : fg;
        HBRUSH ib = CreateSolidBrush(c); HPEN ip = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ ob = SelectObject(dc, ib), op = SelectObject(dc, ip);
        if (start) { POINT tri[3] = { {x0 + o, cy - icon/2 + o}, {x0 + o, cy + icon/2 + o}, {x0 + icon + o, cy + o} }; Polygon(dc, tri, 3); }
        else Rectangle(dc, x0 + o, cy - icon/2 + o, x0 + icon + o, cy + icon/2 + o);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(ib); DeleteObject(ip);
        RECT tr = { x0 + icon + gap + o, r.top + (pressed && !dis ? 1 : 0) + o, r.right, r.bottom + o };
        SetTextColor(dc, c);
        DrawTextW(dc, label, -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    if ((di->itemState & ODS_FOCUS) && !dis) {
        RECT fr = r; InflateRect(&fr, -S(4), -S(4));
        SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, bot);
        DrawFocusRect(dc, &fr);
    }
}

// ================================================================ controls
static HWND Ctl(LPCWSTR cls, LPCWSTR text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)fUI, TRUE);
    return c;
}
static HWND Edit(LPCWSTR text, int x, int y, int w, int id) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER,
                             S(x), S(y), S(w), S(22), g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)fUI, TRUE);
    return c;
}
static void Label(LPCWSTR t, int x, int y, int w) { Ctl(L"STATIC", t, SS_LEFT, x, y + 3, w, 18, 0); }
static HWND Combo(int x, int y, int w, int id, const wchar_t **items, int n) {
    HWND c = Ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 200, id);
    for (int i = 0; i < n; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessage(c, CB_SETCURSEL, 0, 0);
    return c;
}
static HFONT MakeFont(const wchar_t *face, int pt, int weight, BOOL italic) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, face);
}

static void BuildUI(void) {
    fUI    = MakeFont(L"Tahoma", 9, FW_NORMAL, FALSE);
    fGroup = MakeFont(L"Tahoma", 9, FW_NORMAL, FALSE);
    fSmall = MakeFont(L"Tahoma", 8, FW_NORMAL, FALSE);
    fHead  = MakeFont(L"Tahoma", 9, FW_BOLD, FALSE);
    fTitle = MakeFont(L"Trebuchet MS", 10, FW_BOLD, FALSE);
    fBig   = MakeFont(L"Trebuchet MS", 34, FW_BOLD, FALSE);
    fBtn   = MakeFont(L"Trebuchet MS", 11, FW_BOLD, TRUE);

    // Speed
    hModeInt = Ctl(L"BUTTON", L"Every", BS_RADIOBUTTON | WS_TABSTOP | WS_GROUP, 40, 171, 60, 22, ID_MODE_INT);
    hHours = Edit(L"0",   104, 171, 36, ID_HOURS);  Label(L"h",  144, 171, 12);
    hMins  = Edit(L"0",   160, 171, 36, ID_MINS);   Label(L"m",  200, 171, 14);
    hSecs  = Edit(L"0",   218, 171, 36, ID_SECS);   Label(L"s",  258, 171, 10);
    hMs    = Edit(L"100", 274, 171, 52, ID_MS);     Label(L"ms", 330, 171, 24);
    hModeMax = Ctl(L"BUTTON", L"Max speed", BS_RADIOBUTTON | WS_TABSTOP, 40, 203, 84, 22, ID_MODE_MAX);
    hCps   = Edit(L"1000", 128, 203, 64, ID_CPS);   Label(L"clicks / sec", 198, 203, 100);
    SetRadio(hModeInt, hModeMax);

    // Click
    static const wchar_t *btns[] = { L"Left", L"Right", L"Middle" };
    static const wchar_t *types[] = { L"Single", L"Double" };
    Label(L"Button", 40, 267, 52);  hButton = Combo(96, 266, 110, ID_BUTTON, btns, 3);
    Label(L"Type", 234, 267, 40);   hType   = Combo(276, 266, 110, ID_TYPE, types, 2);

    // Stop after
    hStopNever = Ctl(L"BUTTON", L"Never (until I stop it)", BS_RADIOBUTTON | WS_TABSTOP | WS_GROUP, 40, 331, 180, 22, ID_STOP_NEVER);
    hStopAfter = Ctl(L"BUTTON", L"After", BS_RADIOBUTTON | WS_TABSTOP, 234, 331, 54, 22, ID_STOP_AFTER);
    hStopCnt   = Edit(L"100", 290, 331, 64, ID_STOP_CNT); Label(L"clicks", 360, 331, 60);
    SetRadio(hStopNever, hStopAfter);

    // Start / Stop
    hStart = Ctl(L"BUTTON", L"Start", BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, 28, 378, 198, 42, ID_START);
    hStop  = Ctl(L"BUTTON", L"Stop",  BS_OWNERDRAW | WS_TABSTOP, 234, 378, 198, 42, ID_STOP);
    UpdateUI();
}

static void LoadBackground(void) {
    HBITMAP src = (HBITMAP)LoadImageW(g_inst, MAKEINTRESOURCEW(IDB_BACKGROUND), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    if (!src) return;
    BITMAP bm; GetObject(src, sizeof(bm), &bm);
    int w = S(WIN_W), h = S(WIN_H);
    HDC sdc = GetDC(NULL);
    HDC a = CreateCompatibleDC(sdc), b = CreateCompatibleDC(sdc);
    g_bg = CreateCompatibleBitmap(sdc, w, h);
    HGDIOBJ oa = SelectObject(a, src), ob = SelectObject(b, g_bg);
    SetStretchBltMode(b, HALFTONE); SetBrushOrgEx(b, 0, 0, NULL);
    StretchBlt(b, 0, 0, w, h, a, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
    SelectObject(a, oa); SelectObject(b, ob);
    DeleteDC(a); DeleteDC(b); ReleaseDC(NULL, sdc); DeleteObject(src);
}

static int HitTitleButton(int x, int y) {
    POINT pt = { x, y };
    RECT c = TitleButtonRect(TB_CLOSE), m = TitleButtonRect(TB_MIN);
    if (PtInRect(&c, pt)) return TB_CLOSE;
    if (PtInRect(&m, pt)) return TB_MIN;
    return TB_NONE;
}
static void InvalidateTitle(void) { RECT r = SRect(0, 0, WIN_W, TITLE_H); InvalidateRect(g_hwnd, &r, FALSE); }

// ================================================================ window proc
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        LoadBackground();
        g_icon16 = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, S(16), S(16), 0);
        BuildUI();
        // XP-style rounded top corners
        int w = S(WIN_W), h = S(WIN_H);
        HRGN rg = CreateRoundRectRgn(0, 0, w + 1, S(24), S(14), S(14));
        HRGN body = CreateRectRgn(0, S(8), w, h);
        CombineRgn(rg, rg, body, RGN_OR); DeleteObject(body);
        SetWindowRgn(hwnd, rg, FALSE);
        SetTimer(hwnd, TIMER_UI, 100, NULL);
        if (!RegisterHotKey(hwnd, HOTKEY_F6, MOD_NOREPEAT, VK_F6))
            MessageBoxW(hwnd, L"Couldn't register the F6 hotkey (another program may be using it).\nYou can still use the Start and Stop buttons.",
                        L"Auto Clicker", MB_ICONWARNING);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.y < S(TITLE_H) && HitTitleButton(pt.x, pt.y) == TB_NONE) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        int hot = HitTitleButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hot != g_hot) { g_hot = hot; InvalidateTitle(); }
        if (!g_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            g_tracking = TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (g_hot != TB_NONE) { g_hot = TB_NONE; InvalidateTitle(); }
        return 0;
    case WM_LBUTTONDOWN:
        g_down = HitTitleButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (g_down != TB_NONE) { SetCapture(hwnd); InvalidateTitle(); }
        return 0;
    case WM_LBUTTONUP: {
        int up = HitTitleButton(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), was = g_down;
        g_down = TB_NONE; ReleaseCapture(); InvalidateTitle();
        if (was != TB_NONE && was == up) {
            if (up == TB_CLOSE) PostMessage(hwnd, WM_CLOSE, 0, 0);
            else ShowWindow(hwnd, SW_MINIMIZE);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_UI) SampleCps();
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_ESC) StopClicking();
        else if (wp == HOTKEY_F6) { if (g_running) StopClicking(); else StartClicking(); }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_START: StartClicking(); break;
        case ID_STOP:  StopClicking(); break;
        case ID_MODE_INT:   SetRadio(hModeInt, hModeMax); UpdateUI(); break;
        case ID_MODE_MAX:   SetRadio(hModeMax, hModeInt); UpdateUI(); break;
        case ID_STOP_NEVER: SetRadio(hStopNever, hStopAfter); UpdateUI(); break;
        case ID_STOP_AFTER: SetRadio(hStopAfter, hStopNever); UpdateUI(); break;
        }
        return 0;
    case WM_CLICKER_DONE:
        if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; }
        InterlockedExchange(&g_running, 0);
        UnregisterHotKey(hwnd, HOTKEY_ESC);
        UpdateUI();
        return 0;
    case WM_DRAWITEM:
        DrawButton((DRAWITEMSTRUCT *)lp);
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        wchar_t cls[16]; GetClassNameW((HWND)lp, cls, 16);
        if (lstrcmpiW(cls, L"Edit") == 0) break;          // let disabled edit boxes look normal
        SetBkColor((HDC)wp, XP_BEIGE);
        SetTextColor((HDC)wp, IsWindowEnabled((HWND)lp) ? XP_TEXT : XP_DIS_TEXT);
        return (LRESULT)brBeige;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        if (g_bg) {
            HDC bdc = CreateCompatibleDC(dc); HGDIOBJ o2 = SelectObject(bdc, g_bg);
            BitBlt(mem, 0, 0, cr.right, cr.bottom, bdc, 0, 0, SRCCOPY);
            SelectObject(bdc, o2); DeleteDC(bdc);
        } else {
            VGradient(mem, cr, RGB(0x4A, 0x8C, 0xE8), RGB(0x4E, 0xA0, 0x2E));
        }
        PaintHeader(mem, cr.right);
        PaintPanel(mem);
        PaintStatusBar(mem, cr.right, cr.bottom);
        PaintFrame(mem, cr.right, cr.bottom);
        PaintTitleBar(mem, cr.right);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        StopClicking();
        if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); }
        KillTimer(hwnd, TIMER_UI);
        UnregisterHotKey(hwnd, HOTKEY_F6);
        UnregisterHotKey(hwnd, HOTKEY_ESC);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    (void)hPrev; (void)cmd;
    g_inst = hInst;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    {   // ask Windows 11 not to ignore our timer-resolution request while in the background
        typedef struct { ULONG Version, ControlMask, StateMask; } PTS;
        PTS st = { 1, 0x4, 0 };
        SetProcessInformation(GetCurrentProcess(), (PROCESS_INFORMATION_CLASS)4, &st, sizeof(st));
    }
    QueryPerformanceFrequency(&g_qpf);
    HDC sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    brBeige = CreateSolidBrush(XP_BEIGE);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = brBeige;
    wc.lpszClassName = L"XPAutoClicker";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    int w = S(WIN_W), h = S(WIN_H);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, wc.lpszClassName, L"Auto Clicker",
                                WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                                wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                                NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return 0;
}
