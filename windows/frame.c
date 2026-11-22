#include "host.h"

// ---------------------------------------------------------------------------
// Full screen

// Full screen as Windows programs do it: no frame, the whole monitor the
// window is on (not just its work area), on top. Windows hides the taskbar
// for a foreground window that covers its monitor. Leaving restores the
// placement saved on entry, maximized or not.
void set_fullscreen(Frame* w, bool on) {
  if (on == w->fullscreen) return;
  LONG style = GetWindowLongW(w->hwnd, GWL_STYLE);
  if (on) {
    MONITORINFO mi = {sizeof(mi)};
    w->placement.length = sizeof(w->placement);
    if (!GetWindowPlacement(w->hwnd, &w->placement) ||
        !GetMonitorInfoW(MonitorFromWindow(w->hwnd, MONITOR_DEFAULTTONEAREST),
                         &mi))
      return;
    // First: WM_NCCALCSIZE during the change asks titlebar_active.
    w->fullscreen = true;
    SetWindowLongW(w->hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(w->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    w->fullscreen = false;
    SetWindowLongW(w->hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(w->hwnd, &w->placement);
    SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
  }
  layout(w);  // the bar: the title bar again, or the full screen one
}

// ---------------------------------------------------------------------------
// Tabs in the title bar (the bar itself is drawn in tabs.c)

// The resize border's height: also how far a maximized window reaches
// past its monitor's top.
int frame_border_y(HWND hwnd) {
  UINT dpi = GetDpiForWindow(hwnd);
  return GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// Windows' buttons' place in the bar: minimize | maximize | close in equal
// columns, or 0 elsewhere (screen point in lp).
static LRESULT button_hit(Frame* f, LPARAM lp) {
  if (f->buttons_w <= 0) return 0;
  POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(f->hwnd, &p);
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  if (p.y < 0 || p.y >= bar_height(f) || p.x < rc.right - f->buttons_w ||
      p.x >= rc.right)
    return 0;
  int col = (p.x - (rc.right - f->buttons_w)) * 3 / f->buttons_w;
  return col <= 0 ? HTMINBUTTON : col == 1 ? HTMAXBUTTON : HTCLOSE;
}

LRESULT titlebar_hit(Frame* f, WPARAM wp, LPARAM lp) {
  // The side and bottom borders are still the system's.
  LRESULT r = DefWindowProcW(f->hwnd, WM_NCHITTEST, wp, lp);
  if (r != HTCLIENT) return r;
  POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(f->hwnd, &p);
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  int i, edge = frame_border_y(f->hwnd);
  // The top edge resizes, over the tabs too.
  if (p.y >= 0 && p.y < edge && !IsZoomed(f->hwnd))
    return p.x < edge ? HTTOPLEFT : p.x >= rc.right - edge ? HTTOPRIGHT : HTTOP;
  LRESULT button = button_hit(f, lp);
  if (button) return button;
  BarHit hit = bar_hit(f, p.x, p.y, &i);
  // In the bar, what is not a tab or the + is the caption.
  if (hit == HIT_NONE && p.y >= 0 && p.y < bar_height(f)) return HTCAPTION;
  return HTCLIENT;
}

// Is this Windows 11 (build 22000) or later.
static bool windows_11(void) {
  typedef LONG(WINAPI * GetVersionFn)(OSVERSIONINFOW*);
  GetVersionFn get = (GetVersionFn)(void*)GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  OSVERSIONINFOW v = {sizeof(v)};
  return get && get(&v) == 0 && v.dwBuildNumber >= 22000;
}

// Where DWM puts the caption buttons, in client pixels.
static void update_buttons(Frame* f) {
  f->buttons_w = f->buttons_h = 0;
  if (!titlebar_active(f)) return;
  RECT bb, wr, rc;
  POINT o = {0, 0};
  GetWindowRect(f->hwnd, &wr);
  GetClientRect(f->hwnd, &rc);
  ClientToScreen(f->hwnd, &o);
  if (SUCCEEDED(DwmGetWindowAttribute(f->hwnd, DWMWA_CAPTION_BUTTON_BOUNDS,
                                      &bb, sizeof(bb))) &&
      bb.right > bb.left) {
    // Window-relative: from the window rect's corner.
    int ox = o.x - wr.left, oy = o.y - wr.top;
    f->buttons_w = rc.right - (bb.left - ox);
    f->buttons_h = bb.bottom - oy;
  } else {
    // No DWM (Wine): keep the usual room, for the same layout.
    f->buttons_w = (int)(3 * 46 * f->scale + 0.5);
  }
  if (f->buttons_w < 0) f->buttons_w = 0;
  if (f->buttons_h < 0) f->buttons_h = 0;
}

// Before each layout: where the buttons are, DWM's frame over the bar
// (none in full screen), and the buttons' background in the bar's color,
// their glyphs light or dark to match.
void titlebar_update(Frame* f) {
  if (!g_titlebar_tabs) return;
  update_buttons(f);
  MARGINS m = {0, 0, titlebar_active(f) ? bar_height(f) : 0, 0};
  DwmExtendFrameIntoClientArea(f->hwnd, &m);
  COLORREF bar = bar_color();
  DwmSetWindowAttribute(f->hwnd, DWMWA_CAPTION_COLOR, &bar, sizeof(bar));
  BOOL dark = bar_is_dark();
  DwmSetWindowAttribute(f->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
}

// The window menu (Windows' own items, with About and What's new) at a
// screen point: from a right-click on the title bar or the tab bar, from
// Alt+Space or a Shift+right-click in the terminal (full screen has no
// title bar). DefWindowProc does not open it for a caption in the client
// area, so the host does, and sends the choice as WM_SYSCOMMAND.
void show_window_menu(Frame* f, int x, int y) {
  HMENU m = GetSystemMenu(f->hwnd, FALSE);
  if (!m) return;
  bool zoomed = IsZoomed(f->hwnd), fixed = zoomed || f->fullscreen;
  EnableMenuItem(m, SC_RESTORE, MF_BYCOMMAND | (zoomed ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(m, SC_MOVE, MF_BYCOMMAND | (fixed ? MF_GRAYED : MF_ENABLED));
  EnableMenuItem(m, SC_SIZE, MF_BYCOMMAND | (fixed ? MF_GRAYED : MF_ENABLED));
  EnableMenuItem(m, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
  EnableMenuItem(m, SC_MAXIMIZE, MF_BYCOMMAND | (fixed ? MF_GRAYED : MF_ENABLED));
  SetForegroundWindow(f->hwnd);  // else the menu does not close on a click away
  UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
  int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, x, y, 0,
                           f->hwnd, NULL);
  if (cmd) PostMessageW(f->hwnd, WM_SYSCOMMAND, (WPARAM)cmd, 0);
}

void titlebar_choose(void) {
  const char* v = getenv("GHOSTTY_TITLEBAR");
  BOOL composition = FALSE;
  if (v && strcmp(v, "tabs") == 0)
    g_titlebar_tabs = true;
  else if (v && strcmp(v, "system") == 0)
    g_titlebar_tabs = false;
  else
    g_titlebar_tabs = g_tab_bar != TAB_BAR_NEVER && windows_11() &&
                      SUCCEEDED(DwmIsCompositionEnabled(&composition)) &&
                      composition;
}

// The config's `fullscreen` (false, true, or a macOS-only variant, all
// of which mean full screen here): whether a new window starts in it.
static bool config_fullscreen(void) {
  const char* v = NULL;
  const char* key = "fullscreen";
  if (!ghostty_config_get(g_config, &v, key, strlen(key)) || !v) return false;
  return strcmp(v, "false") != 0;
}

// ---------------------------------------------------------------------------
// Windows

static double window_scale(HWND hwnd) {
  UINT dpi = GetDpiForWindow(hwnd);
  return dpi ? dpi / 96.0 : 1.0;
}

Frame* new_frame(void) {
  Frame* f = calloc(1, sizeof(Frame));
  if (!f) return NULL;
  f->active = -1;
  f->hover = -1;
  f->hwnd = CreateWindowExW(0, kFrameClass, L"Ghostty",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 640, NULL,
                            NULL, g_inst, NULL);
  if (!f->hwnd) {
    free(f);
    return NULL;
  }
  f->scale = window_scale(f->hwnd);
  SetWindowLongPtrW(f->hwnd, GWLP_USERDATA, (LONG_PTR)f);
  about_menu_add(f->hwnd);  // About and What's new in the window menu
  if (g_titlebar_tabs)  // WM_NCCALCSIZE again, now that frame_proc knows f
    SetWindowPos(f->hwnd, NULL, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE);
  f->next = g_frames;
  g_frames = f;
  return f;
}

void show_frame(Frame* f) {
  ShowWindow(f->hwnd, f->next ? SW_SHOWNORMAL : SW_SHOWDEFAULT);
  if (config_fullscreen()) set_fullscreen(f, true);
  SetForegroundWindow(f->hwnd);
  if (f->active >= 0) SetFocus(f->tabs[f->active]->hwnd);
}

bool new_window(ghostty_surface_t from) {
  Frame* f = new_frame();
  if (!f) return false;
  if (!new_tab(f, from, GHOSTTY_SURFACE_CONTEXT_WINDOW)) {
    DestroyWindow(f->hwnd);
    return false;
  }
  show_frame(f);
  return true;
}

// The window an action without a surface means: the one in front.
Frame* front_frame(void) {
  HWND fg = GetForegroundWindow();
  for (Frame* f = g_frames; f; f = f->next)
    if (f->hwnd == fg) return f;
  return g_frames;
}

static void track_bar_mouse(Frame* f, int x, int y) {
  int i;
  BarHit hit = bar_hit(f, x, y, &i);
  bool close = hit == HIT_CLOSE, plus = hit == HIT_PLUS;
  if (i != f->hover || close != f->hover_close || plus != f->hover_plus) {
    f->hover = i;
    f->hover_close = close;
    f->hover_plus = plus;
    invalidate_bar(f);
  }
  if (!f->tracking) {
    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, f->hwnd, 0};
    f->tracking = TrackMouseEvent(&t);
  }
}

// A window: the tab bar, and the tabs as child windows below it.
LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (notify_frame_msg(hwnd, msg)) return 0;  // TaskbarButtonCreated (notify.c)
  Frame* f = (Frame*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!f) return DefWindowProcW(hwnd, msg, wp, lp);

  if (titlebar_active(f)) {
    // Windows' caption buttons, drawn by DWM in the bar. Where they are the
    // host says so itself (a maximized window's buttons did not answer on
    // the owner's box when DWM alone did); then DWM handles the rest (hover,
    // presses, snap layouts), and what it leaves, the host does below.
    if (msg == WM_NCHITTEST) {
      LRESULT b = button_hit(f, lp);
      if (b) return b;
    }
    LRESULT r;
    if (DwmDefWindowProc(hwnd, msg, wp, lp, &r)) return r;
  }

  switch (msg) {
    case WM_NCCALCSIZE:
      if (wp && titlebar_active(f)) {
        // The system's borders on the sides and bottom; at the top, the
        // client area from the window's own top (a maximized window's top
        // is past its monitor's by the border: skip that much).
        NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
        LONG top = p->rgrc[0].top;
        DefWindowProcW(hwnd, msg, wp, lp);
        p->rgrc[0].top = top + (IsZoomed(hwnd) ? frame_border_y(hwnd) : 0);
        return 0;
      }
      break;
    case WM_NCHITTEST:
      if (titlebar_active(f)) return titlebar_hit(f, wp, lp);
      break;
    case WM_SYSCOMMAND:
      if (about_command(hwnd, wp)) return 0;
      break;
    // Windows' caption buttons when DwmDefWindowProc did not take the click:
    // the press is ours (DefWindowProc would draw a classic button), the
    // release over the same button acts.
    case WM_NCLBUTTONDOWN:
      if (titlebar_active(f) &&
          (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE)) {
        f->nc_pressed = wp;
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (titlebar_active(f) &&
          (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE)) {
        WPARAM pressed = f->nc_pressed;
        f->nc_pressed = 0;
        if (pressed == wp)
          SendMessageW(hwnd, WM_SYSCOMMAND,
                       wp == HTMINBUTTON   ? SC_MINIMIZE
                       : wp == HTCLOSE     ? SC_CLOSE
                       : IsZoomed(hwnd)    ? SC_RESTORE
                                           : SC_MAXIMIZE,
                       0);
        return 0;
      }
      break;
    case WM_NCRBUTTONDOWN:
      // Ours, so the release comes to WM_NCRBUTTONUP: DefWindowProc would
      // take it in a loop of its own and, judging the point by its own
      // frame (client area here), open no menu.
      if (titlebar_active(f) && wp == HTCAPTION) return 0;
      break;
    case WM_NCRBUTTONUP:
      if (titlebar_active(f) && wp == HTCAPTION) {
        show_window_menu(f, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
      }
      break;
    case WM_CONTEXTMENU:
      // DefWindowProc turns a right-click on a caption into this (it takes
      // the button's release itself), and Shift+F10 too (lp -1): the window
      // menu, at the point or the window's top left.
      if (titlebar_active(f)) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (lp == -1) {
          p.x = p.y = 0;
          ClientToScreen(hwnd, &p);
        } else if (SendMessageW(hwnd, WM_NCHITTEST, 0, lp) != HTCAPTION) {
          break;
        }
        show_window_menu(f, p.x, p.y);
        return 0;
      }
      break;
    case WM_NCLBUTTONDBLCLK:
      // The bar's caption, double-clicked: maximize or restore, here and
      // not left to DefWindowProc (Wine's leaves a maximized window as it
      // is when its caption is in the client area).
      if (wp == HTCAPTION && titlebar_active(f)) {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return 0;
      }
      break;
    case WM_ACTIVATE:
      titlebar_update(f);
      break;
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) layout(f);
      return 0;
    case WM_DPICHANGED: {
      f->scale = HIWORD(wp) / 96.0;
      for (int i = 0; i < f->ntabs; i++) {
        Window* w = f->tabs[i];
        w->scale = f->scale;
        ghostty_surface_set_content_scale(w->surface, w->scale, w->scale);
      }
      const RECT* r = (const RECT*)lp;
      SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      layout(f);
      return 0;
    }
    case WM_SETFOCUS:
      if (f->active >= 0) SetFocus(f->tabs[f->active]->hwnd);
      return 0;

    case WM_MOUSEMOVE:
      if (f->pressing) {
        // A tab in the title bar dragged: the window moves, as in Chrome.
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (abs(x - f->press.x) > GetSystemMetrics(SM_CXDRAG) ||
            abs(y - f->press.y) > GetSystemMetrics(SM_CYDRAG)) {
          f->pressing = false;
          ReleaseCapture();
          POINT sp = f->press;
          ClientToScreen(hwnd, &sp);
          SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION,
                       MAKELPARAM(sp.x, sp.y));
        }
        return 0;
      }
      track_bar_mouse(f, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE:
      f->tracking = false;
      f->hover = -1;
      f->hover_close = f->hover_plus = false;
      invalidate_bar(f);
      return 0;
    case WM_LBUTTONUP:
      if (f->pressing) {
        f->pressing = false;
        ReleaseCapture();
      }
      return 0;
    case WM_CAPTURECHANGED:
      f->pressing = false;
      return 0;
    case WM_LBUTTONDOWN: {
      int i;
      switch (bar_hit(f, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &i)) {
        case HIT_TAB:
          select_tab(f, i);
          if (titlebar_active(f)) {
            f->pressing = true;
            f->press.x = GET_X_LPARAM(lp);
            f->press.y = GET_Y_LPARAM(lp);
            SetCapture(hwnd);
          }
          break;
        case HIT_CLOSE: close_tab(f->tabs[i]); break;
        case HIT_PLUS: {
          Window* cur = f->active >= 0 ? f->tabs[f->active] : NULL;
          new_tab(f, cur ? cur->surface : NULL, GHOSTTY_SURFACE_CONTEXT_TAB);
          break;
        }
        default: break;
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      // A right-click in the tab bar (also full screen's): the window menu.
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (y >= 0 && y < bar_height(f)) {
        POINT sp = {x, y};
        ClientToScreen(hwnd, &sp);
        show_window_menu(f, sp.x, sp.y);
      }
      return 0;
    }
    case WM_MBUTTONUP: {
      int i;
      BarHit hit = bar_hit(f, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &i);
      if (hit == HIT_TAB || hit == HIT_CLOSE) close_tab(f->tabs[i]);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint_frame(f);
      return 0;

    case WM_CLOSE: {
      // Close every tab; the last one to go takes the window with it.
      Window* list[256];
      int n = 0;
      for (int i = 0; i < f->ntabs && n < 256; i++) list[n++] = f->tabs[i];
      for (int i = 0; i < n; i++) close_tab(list[i]);
      if (n == 0) DestroyWindow(hwnd);
      return 0;
    }
    case WM_GHOSTTY_DESTROY:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      // The tabs' own WM_DESTROY comes after this one (children go after
      // their parent); they must not rearrange a window that is going.
      f->destroying = true;
      return 0;
    case WM_NCDESTROY: {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      for (Frame** p = &g_frames; *p; p = &(*p)->next)
        if (*p == f) {
          *p = f->next;
          break;
        }
      if (f->font) DeleteObject(f->font);
      if (f->stats_font) DeleteObject(f->stats_font);
      free(f->tabs);
      free(f);
      if (!g_frames) PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
