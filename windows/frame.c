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
    SetWindowLongW(w->hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(w->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    SetWindowLongW(w->hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(w->hwnd, &w->placement);
    SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                     SWP_FRAMECHANGED);
  }
  w->fullscreen = on;
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
  if (!f->tracking && hit != HIT_NONE) {
    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, f->hwnd, 0};
    f->tracking = TrackMouseEvent(&t);
  }
}

// A window: the tab bar, and the tabs as child windows below it.
LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Frame* f = (Frame*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!f) return DefWindowProcW(hwnd, msg, wp, lp);

  switch (msg) {
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
      track_bar_mouse(f, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE:
      f->tracking = false;
      f->hover = -1;
      f->hover_close = f->hover_plus = false;
      invalidate_bar(f);
      return 0;
    case WM_LBUTTONDOWN: {
      int i;
      switch (bar_hit(f, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &i)) {
        case HIT_TAB: select_tab(f, i); break;
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
