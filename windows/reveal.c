#include "host.h"

// ---------------------------------------------------------------------------
// Full screen's reveal bar (the owner: "for full screen draw ? possible …
// I don't make people scare when full screen"; the macOS app's model)
//
// In full screen the terminal takes the whole screen (bar_height is 0 for
// the layout). The mouse held at the top edge (~300 ms, so a flick does not
// do it) slides the tab bar down OVER the terminal: a popup owned by the
// frame, so the grid never reflows. It has the tabs, the +, the "?" (the
// overview) and an exit-full-screen button at the right; it goes ~1 s after
// the mouse leaves it. Typing never shows it (only the pointer's place
// does). The first full screen ever shows it for ~3 s with a hint line,
// remembered in host.ini. The quick terminal has none (no title bar, by
// design).
//
// The popup sits at the frame's top-left, so its coordinates are the
// frame's client ones: it paints with draw_bar and hands its clicks to the
// frame's own handler, with reveal_painting set (bar_height, help_rect …
// then answer for the bar).

#define REVEAL_TIMER 24
#define REVEAL_TICK 100  // ms

static LRESULT CALLBACK reveal_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  Frame* f = (Frame*)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (!f) return DefWindowProcW(h, msg, wp, lp);
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      f->reveal_painting = true;
      int b = bar_height(f);
      draw_bar(f, mem, rc.right, b);
      f->reveal_painting = false;
      // The first time's hint: a row of its own under the tabs (in full
      // screen the tabs take the bar's whole width, so it has no gap).
      const WCHAR* hint = theme_hint();
      if (hint && rc.bottom > b) {
        RECT row = {0, b, rc.right, rc.bottom};
        HBRUSH br = CreateSolidBrush(bar_color());
        FillRect(mem, &row, br);
        DeleteObject(br);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, g_fg);
        SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(mem, hint, -1, &row, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      int i;
      f->reveal_painting = true;
      BarHit hit = bar_hit(f, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &i);
      f->reveal_painting = false;
      bool close = hit == HIT_CLOSE, plus = hit == HIT_PLUS, help = hit == HIT_HELP, ex = hit == HIT_EXITFS;
      if (i != f->hover || close != f->hover_close || plus != f->hover_plus || help != f->hover_help ||
          ex != f->hover_exitfs) {
        f->hover = i;
        f->hover_close = close;
        f->hover_plus = plus;
        f->hover_help = help;
        f->hover_exitfs = ex;
        InvalidateRect(h, NULL, FALSE);
      }
      TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      f->hover = -1;
      f->hover_close = f->hover_plus = f->hover_help = f->hover_exitfs = false;
      InvalidateRect(h, NULL, FALSE);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {  // a right-click: the window menu, as on the bar
      // The frame's own handling (select, close, +, ?, exit), as if the
      // bar were its own.
      f->reveal_painting = true;
      SendMessageW(f->hwnd, msg, wp, lp);
      f->reveal_painting = false;
      if (f->reveal) InvalidateRect(f->reveal, NULL, FALSE);
      return 0;
    }
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;  // the terminal keeps the focus
  }
  return DefWindowProcW(h, msg, wp, lp);
}

static void reveal_place(Frame* f) {
  RECT wr;
  GetWindowRect(f->hwnd, &wr);
  f->reveal_painting = true;
  int b = bar_height(f);
  f->reveal_painting = false;
  if (theme_hint()) b *= 2;  // a row for the first time's hint
  SetWindowPos(f->reveal, HWND_TOP, wr.left, wr.top, wr.right - wr.left, b,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void reveal_event(Frame* f, const char* what) {
  char e[96];
  snprintf(e, sizeof(e), "{\"kind\":\"reveal\",\"what\":\"%s\"}", what);
  test_event(e);
  (void)f;
}

static void reveal_show(Frame* f) {
  if (f->reveal_on) return;
  if (!f->reveal) {
    static bool registered;
    if (!registered) {
      WNDCLASSW wc = {0};
      wc.lpfnWndProc = reveal_proc;
      wc.hInstance = g_inst;
      wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
      wc.lpszClassName = L"GhosttyReveal";
      registered = RegisterClassW(&wc) != 0;
    }
    f->reveal = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"GhosttyReveal", L"", WS_POPUP, 0, 0,
                                0, 0, f->hwnd, NULL, g_inst, NULL);
    if (!f->reveal) return;
    SetWindowLongPtrW(f->reveal, GWLP_USERDATA, (LONG_PTR)f);
  }
  reveal_place(f);
  InvalidateRect(f->reveal, NULL, FALSE);
  f->reveal_on = true;
  f->reveal_away = 0;
  reveal_event(f, "shown");
}

static void reveal_hide(Frame* f) {
  if (!f->reveal_on) return;
  if (f->reveal) ShowWindow(f->reveal, SW_HIDE);
  f->reveal_on = false;
  f->reveal_dwell = 0;
  reveal_event(f, "hidden");
}

bool reveal_shown(Frame* f) { return f->reveal_on; }

// set_fullscreen: on, the watch starts (and the first time, the hint);
// off, the bar goes.
void reveal_fullscreen(Frame* f, bool on) {
  if (f->quick) return;
  if (!on) {
    KillTimer(f->hwnd, REVEAL_TIMER);
    reveal_hide(f);
    return;
  }
  f->reveal_dwell = f->reveal_away = 0;
  SetTimer(f->hwnd, REVEAL_TIMER, REVEAL_TICK, NULL);
  char seen[16];
  if (!host_ini_get("fullscreen_hint", seen, sizeof(seen))) {
    host_ini_set("fullscreen_hint", "seen");
    hint_show(tr(L"Move the mouse to the top for tabs and ?, F11 to exit"));
    reveal_show(f);
    f->reveal_hold = GetTickCount64() + 3000;
  }
}

// WM_TIMER on the frame: where the pointer is.
bool reveal_timer(Frame* f, WPARAM id) {
  if (id != REVEAL_TIMER) return false;
  if (!f->fullscreen) {
    KillTimer(f->hwnd, REVEAL_TIMER);
    reveal_hide(f);
    return true;
  }
  POINT p;
  RECT wr;
  if (!GetCursorPos(&p) || !GetWindowRect(f->hwnd, &wr)) return true;
  bool across = p.x >= wr.left && p.x < wr.right;
  if (!f->reveal_on) {
    bool top = across && p.y <= wr.top + 2 && GetForegroundWindow() == f->hwnd;
    f->reveal_dwell = top ? f->reveal_dwell + REVEAL_TICK : 0;
    if (f->reveal_dwell >= 300) reveal_show(f);
    return true;
  }
  RECT br;
  GetWindowRect(f->reveal, &br);
  bool inside = across && p.y >= br.top - 2 && p.y < br.bottom + 4;
  if (GetTickCount64() < f->reveal_hold) return true;  // the first time's hint
  if (f->reveal_hold) {  // its time is up: back to the bar alone
    f->reveal_hold = 0;
    reveal_place(f);
  }
  f->reveal_away = inside ? 0 : f->reveal_away + REVEAL_TICK;
  if (f->reveal_away >= 1000) reveal_hide(f);
  return true;
}
