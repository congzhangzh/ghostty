// Ghostty for Windows: a Win32 host on libghostty's embedded runtime.
//
// The macOS app is a Swift program on top of libghostty; this is the same
// shape for Windows, in C on Win32. libghostty runs the terminal, the pty
// (ConPTY) and the renderer. The renderer draws with OpenGL (WGL) into its
// own offscreen framebuffer and hands each finished frame back; this host
// owns the window: it turns Win32 messages into Ghostty input, and on
// GHOSTTY_ACTION_RENDER it takes the newest frame and blits it.
//
// One window, one surface for now. Built by windows/build.sh.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "ghostty.h"

#define WM_GHOSTTY_WAKEUP (WM_APP + 1)
// Destroy the window later: libghostty asks to close a surface from inside
// its own call stack, and freeing the surface there would pull it out from
// under that code.
#define WM_GHOSTTY_DESTROY (WM_APP + 2)

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif

typedef struct Window {
  HWND hwnd;
  ghostty_surface_t surface;
  double scale;  // DPI / 96

  // The frame shown now, converted to BGRA for GDI, kept for repaints.
  uint8_t* bgra;
  uint32_t frame_w, frame_h;
  size_t bgra_cap;

  // A high surrogate from WM_CHAR waiting for its low half.
  WCHAR high_surrogate;

  // Full screen: the placement to go back to when it ends.
  bool fullscreen;
  WINDOWPLACEMENT placement;
} Window;

static ghostty_app_t g_app;
static ghostty_config_t g_config;
static HWND g_wakeup_hwnd;  // message-only window for wakeups
static volatile LONG g_wakeup_posted;  // a WM_GHOSTTY_WAKEUP is queued
static int g_windows;       // open terminal windows
static bool g_debug_keys;   // GHOSTTY_DEBUG_KEYS is set: trace key messages
static const WCHAR* const kTerminalClass = L"GhosttyWindow";
static const WCHAR* const kWakeupClass = L"GhosttyWakeup";

// ---------------------------------------------------------------------------
// UTF-8 / UTF-16

// Returns a malloc'd UTF-8 string (caller frees), or NULL.
static char* utf16_to_utf8(const WCHAR* s, int len) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char* out = malloc((size_t)n + 1);
  if (!out) return NULL;
  WideCharToMultiByte(CP_UTF8, 0, s, len, out, n, NULL, NULL);
  out[n] = 0;
  return out;
}

// Returns a malloc'd UTF-16 string (caller frees), or NULL.
static WCHAR* utf8_to_utf16(const char* s, int len) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
  if (n <= 0) return NULL;
  WCHAR* out = malloc(((size_t)n + 1) * sizeof(WCHAR));
  if (!out) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, s, len, out, n);
  out[n] = 0;
  return out;
}

// ---------------------------------------------------------------------------
// Input

static bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

static ghostty_input_mods_e current_mods(void) {
  int m = GHOSTTY_MODS_NONE;
  if (key_down(VK_SHIFT)) m |= GHOSTTY_MODS_SHIFT;
  if (key_down(VK_CONTROL)) m |= GHOSTTY_MODS_CTRL;
  if (key_down(VK_MENU)) m |= GHOSTTY_MODS_ALT;
  if (key_down(VK_LWIN) || key_down(VK_RWIN)) m |= GHOSTTY_MODS_SUPER;
  if (GetKeyState(VK_CAPITAL) & 1) m |= GHOSTTY_MODS_CAPS;
  if (GetKeyState(VK_NUMLOCK) & 1) m |= GHOSTTY_MODS_NUM;
  if (key_down(VK_RSHIFT)) m |= GHOSTTY_MODS_SHIFT_RIGHT;
  if (key_down(VK_RCONTROL)) m |= GHOSTTY_MODS_CTRL_RIGHT;
  if (key_down(VK_RMENU)) m |= GHOSTTY_MODS_ALT_RIGHT;
  if (key_down(VK_RWIN)) m |= GHOSTTY_MODS_SUPER_RIGHT;
  return (ghostty_input_mods_e)m;
}

// The native keycode Ghostty's keycode table expects on Windows: the
// scan code, with 0xe000 for extended keys (src/input/keycodes.zig).
static uint32_t native_keycode(LPARAM lp) {
  uint32_t sc = (uint32_t)(lp >> 16) & 0xff;
  if (lp & (1 << 24)) sc |= 0xe000;
  return sc;
}

// The character a key makes with no modifiers, lower case, or 0.
static uint32_t unshifted_codepoint(WPARAM vk) {
  UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;
  if (ch == 0) return 0;
  return (uint32_t)towlower((wint_t)ch);
}

// Collect the WM_CHAR / WM_SYSCHAR messages TranslateMessage queued for
// this key press, so the key event carries the text it produced. A dead
// key queues WM_DEADCHAR instead: the key is composing.
static int take_chars(HWND hwnd, WCHAR* buf, int cap, bool* composing) {
  MSG msg;
  int n = 0;
  *composing = false;
  while (PeekMessageW(&msg, hwnd, WM_CHAR, WM_DEADCHAR, PM_NOREMOVE)) {
    if (msg.message != WM_CHAR && msg.message != WM_SYSCHAR &&
        msg.message != WM_DEADCHAR && msg.message != WM_SYSDEADCHAR)
      break;
    PeekMessageW(&msg, hwnd, msg.message, msg.message, PM_REMOVE);
    if (msg.message == WM_DEADCHAR || msg.message == WM_SYSDEADCHAR) {
      *composing = true;
      continue;
    }
    if (n < cap) buf[n++] = (WCHAR)msg.wParam;
  }
  return n;
}

static void handle_key(Window* w, UINT msg, WPARAM vk, LPARAM lp) {
  if (!w->surface) return;
  // Unicode injected by SendInput and IME results arrive as WM_CHAR.
  if (vk == VK_PACKET || vk == VK_PROCESSKEY) return;

  bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
  bool repeat = !up && (lp & (1 << 30));

  ghostty_input_key_s key = {0};
  key.action = up ? GHOSTTY_ACTION_RELEASE
                  : (repeat ? GHOSTTY_ACTION_REPEAT : GHOSTTY_ACTION_PRESS);
  key.mods = current_mods();
  key.keycode = native_keycode(lp);
  key.unshifted_codepoint = unshifted_codepoint(vk);

  char* text = NULL;
  if (!up) {
    WCHAR chars[16];
    bool composing = false;
    int n = take_chars(w->hwnd, chars, 16, &composing);
    key.composing = composing;
    // Control characters (Enter, Tab, Ctrl+C...) are encoded by Ghostty
    // from the key and its mods, not taken as text.
    if (n > 0 && !(n == 1 && (chars[0] < 0x20 || chars[0] == 0x7f))) {
      text = utf16_to_utf8(chars, n);
      // AltGr arrives as Ctrl+Alt; when it produced text those mods
      // were used up making it.
      if (key_down(VK_RMENU) && key_down(VK_LCONTROL))
        key.consumed_mods =
            (ghostty_input_mods_e)(GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT);
      else if (key.mods & GHOSTTY_MODS_SHIFT)
        key.consumed_mods = GHOSTTY_MODS_SHIFT;
    }
  }
  key.text = text;
  bool consumed = ghostty_surface_key(w->surface, key);
  // GHOSTTY_DEBUG_KEYS=1: one line per key message on stderr, to debug a
  // layout, AltGr or IME report (start it from cmd.exe: 2> keys.log).
  if (g_debug_keys)
    fprintf(stderr,
            "key msg=0x%04x vk=0x%02x scan=0x%04x mods=0x%x action=%d "
            "text=%s consumed=%d\n",
            msg, (unsigned)vk, key.keycode, (unsigned)key.mods,
            (int)key.action, text ? text : "", consumed);
  free(text);
}

// Text that came without a key press of ours: IME commits, VK_PACKET.
static void handle_char(Window* w, WCHAR ch) {
  if (!w->surface) return;
  if (IS_HIGH_SURROGATE(ch)) {
    w->high_surrogate = ch;
    return;
  }
  WCHAR buf[2];
  int n = 0;
  if (IS_LOW_SURROGATE(ch) && w->high_surrogate) buf[n++] = w->high_surrogate;
  w->high_surrogate = 0;
  buf[n++] = ch;
  if (n == 1 && (ch < 0x20 || ch == 0x7f)) return;
  char* text = utf16_to_utf8(buf, n);
  if (!text) return;
  ghostty_surface_text(w->surface, text, strlen(text));
  free(text);
}

static void mouse_pos(Window* w, LPARAM lp) {
  // Ghostty takes the cursor in points and scales it itself.
  double x = GET_X_LPARAM(lp) / w->scale;
  double y = GET_Y_LPARAM(lp) / w->scale;
  ghostty_surface_mouse_pos(w->surface, x, y, current_mods());
}

static void mouse_button(Window* w, ghostty_input_mouse_state_e state,
                         ghostty_input_mouse_button_e button) {
  if (state == GHOSTTY_MOUSE_PRESS)
    SetCapture(w->hwnd);
  else
    ReleaseCapture();
  ghostty_surface_mouse_button(w->surface, state, button, current_mods());
}

// ---------------------------------------------------------------------------
// Presenting frames

static void take_frame(Window* w) {
  ghostty_frame_s f;
  if (!ghostty_surface_take_frame(w->surface, &f)) return;

  size_t need = (size_t)f.width * f.height * 4;
  if (need > w->bgra_cap) {
    uint8_t* p = realloc(w->bgra, need);
    if (!p) {
      ghostty_frame_free(&f);
      return;
    }
    w->bgra = p;
    w->bgra_cap = need;
  }
  // Premultiplied RGBA to the BGRA GDI wants.
  for (uint32_t y = 0; y < f.height; y++) {
    const uint8_t* src = f.pixels + (size_t)y * f.stride;
    uint8_t* dst = w->bgra + (size_t)y * f.width * 4;
    for (uint32_t x = 0; x < f.width; x++) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
      src += 4;
      dst += 4;
    }
  }
  w->frame_w = f.width;
  w->frame_h = f.height;
  ghostty_frame_free(&f);
}

static void paint(Window* w) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w->hwnd, &ps);
  RECT rc;
  GetClientRect(w->hwnd, &rc);
  if (w->bgra && w->frame_w && w->frame_h) {
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = (LONG)w->frame_w;
    // Positive: a bottom-up DIB, the row order OpenGL reads pixels in.
    bmi.bmiHeader.biHeight = (LONG)w->frame_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, w->frame_w, w->frame_h, 0, 0, 0, w->frame_h,
                      w->bgra, &bmi, DIB_RGB_COLORS);
    // While a resize waits for its frame, fill the uncovered part.
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if ((LONG)w->frame_w < rc.right) {
      RECT r = {(LONG)w->frame_w, 0, rc.right, rc.bottom};
      FillRect(dc, &r, black);
    }
    if ((LONG)w->frame_h < rc.bottom) {
      RECT r = {0, (LONG)w->frame_h, (LONG)w->frame_w, rc.bottom};
      FillRect(dc, &r, black);
    }
  } else {
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
  }
  EndPaint(w->hwnd, &ps);
}

// ---------------------------------------------------------------------------
// Clipboard

static char* clipboard_text(HWND hwnd) {
  char* out = NULL;
  if (!OpenClipboard(hwnd)) return NULL;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const WCHAR* s = GlobalLock(h);
    if (s) {
      out = utf16_to_utf8(s, -1);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

static void set_clipboard_text(HWND hwnd, const char* data, size_t len) {
  int n = MultiByteToWideChar(CP_UTF8, 0, data, (int)len, NULL, 0);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(WCHAR));
  if (!h) return;
  WCHAR* p = GlobalLock(h);
  MultiByteToWideChar(CP_UTF8, 0, data, (int)len, p, n);
  p[n] = 0;
  GlobalUnlock(h);
  if (OpenClipboard(hwnd)) {
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    CloseClipboard();
  } else {
    GlobalFree(h);
  }
}

static ghostty_clipboard_read_result_e read_clipboard_cb(
    void* ud, ghostty_clipboard_e which, void* state,
    const char* const* mimes, size_t mimes_len, bool list) {
  (void)mimes;
  (void)mimes_len;
  (void)list;
  Window* w = ud;
  if (which != GHOSTTY_CLIPBOARD_STANDARD)
    return GHOSTTY_CLIPBOARD_READ_UNSUPPORTED;
  char* text = clipboard_text(w->hwnd);
  if (!text) return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
  static const char* const available[] = {"text/plain"};
  ghostty_clipboard_content_s content = {"text/plain", text, strlen(text)};
  ghostty_clipboard_complete_s done = {&content, 1, available, 1, false, false};
  ghostty_surface_complete_clipboard_request(w->surface, &done, state);
  free(text);
  return GHOSTTY_CLIPBOARD_READ_STARTED;
}

static void confirm_read_clipboard_cb(void* ud,
                                      const ghostty_clipboard_confirm_s* c,
                                      void* state,
                                      ghostty_clipboard_request_e req) {
  Window* w = ud;
  const WCHAR* what = req == GHOSTTY_CLIPBOARD_REQUEST_PASTE
                          ? L"The text you are pasting may run commands. Paste it?"
                          : L"A program in the terminal wants to read the clipboard. Allow it?";
  int r = MessageBoxW(w->hwnd, what, L"Ghostty", MB_YESNO | MB_ICONWARNING);
  if (r == IDYES) {
    ghostty_clipboard_complete_s done = {c->contents, c->contents_len,
                                         c->available, c->available_len,
                                         true, false};
    ghostty_surface_complete_clipboard_request(w->surface, &done, state);
  } else {
    ghostty_surface_deny_clipboard_request(w->surface, state);
  }
}

static void write_clipboard_cb(void* ud, ghostty_clipboard_e which,
                               const ghostty_clipboard_content_s* contents,
                               size_t len, bool confirm) {
  Window* w = ud;
  if (which != GHOSTTY_CLIPBOARD_STANDARD) return;
  for (size_t i = 0; i < len; i++) {
    if (!contents[i].mime || strncmp(contents[i].mime, "text/plain", 10) != 0)
      continue;
    if (confirm &&
        MessageBoxW(w->hwnd,
                    L"A program in the terminal wants to set the clipboard. Allow it?",
                    L"Ghostty", MB_YESNO | MB_ICONWARNING) != IDYES)
      return;
    set_clipboard_text(w->hwnd, contents[i].data, contents[i].len);
    return;
  }
}

// ---------------------------------------------------------------------------
// Full screen

// Full screen as Windows programs do it: no frame, the whole monitor the
// window is on (not just its work area), on top. Windows hides the taskbar
// for a foreground window that covers its monitor. Leaving restores the
// placement saved on entry, maximized or not.
static void set_fullscreen(Window* w, bool on) {
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
// Runtime callbacks

static void wakeup_cb(void* ud) {
  (void)ud;
  // Any thread: hand the tick to the main thread, one queued message at
  // a time (one tick handles every wakeup before it).
  if (InterlockedExchange(&g_wakeup_posted, 1) == 0)
    PostMessageW(g_wakeup_hwnd, WM_GHOSTTY_WAKEUP, 0, 0);
}

static Window* target_window(ghostty_target_s t) {
  if (t.tag != GHOSTTY_TARGET_SURFACE || !t.target.surface) return NULL;
  return ghostty_surface_userdata(t.target.surface);
}

static bool action_cb(ghostty_app_t app, ghostty_target_s target,
                      ghostty_action_s action) {
  (void)app;
  Window* w = target_window(target);
  switch (action.tag) {
    case GHOSTTY_ACTION_RENDER:
      if (!w) return false;
      take_frame(w);
      InvalidateRect(w->hwnd, NULL, FALSE);
      return true;

    case GHOSTTY_ACTION_SET_TITLE: {
      if (!w || !action.action.set_title.title) return false;
      WCHAR* t = utf8_to_utf16(action.action.set_title.title, -1);
      if (t) {
        SetWindowTextW(w->hwnd, t);
        free(t);
      }
      return true;
    }

    case GHOSTTY_ACTION_TOGGLE_FULLSCREEN:
      if (!w) return false;
      set_fullscreen(w, !w->fullscreen);
      return true;

    case GHOSTTY_ACTION_RING_BELL:
      if (w) FlashWindow(w->hwnd, TRUE);
      MessageBeep(MB_OK);
      return true;

    case GHOSTTY_ACTION_CLOSE_WINDOW:
      if (!w) return false;
      PostMessageW(w->hwnd, WM_GHOSTTY_DESTROY, 0, 0);
      return true;

    case GHOSTTY_ACTION_QUIT:
      PostQuitMessage(0);
      return true;

    default:
      return false;
  }
}

static void close_surface_cb(void* ud, bool process_alive) {
  (void)process_alive;
  Window* w = ud;
  if (w && w->hwnd) PostMessageW(w->hwnd, WM_GHOSTTY_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// Windows

static double window_scale(HWND hwnd) {
  UINT dpi = GetDpiForWindow(hwnd);
  return dpi ? dpi / 96.0 : 1.0;
}

static LRESULT CALLBACK terminal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Window* w = (Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!w) return DefWindowProcW(hwnd, msg, wp, lp);

  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // Let Windows have Alt+F4 and Alt+Space (the window menu).
      if (msg == WM_SYSKEYDOWN && (wp == VK_F4 || wp == VK_SPACE)) break;
      handle_key(w, msg, wp, lp);
      return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      handle_key(w, msg, wp, lp);
      if (msg == WM_SYSKEYUP && wp == VK_MENU) return 0;  // no menu focus
      break;
    case WM_CHAR:
    case WM_SYSCHAR:
      handle_char(w, (WCHAR)wp);
      return 0;
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
      return 0;

    case WM_MOUSEMOVE:
      mouse_pos(w, lp);
      return 0;
    case WM_LBUTTONDOWN:
      mouse_button(w, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT);
      return 0;
    case WM_LBUTTONUP:
      mouse_button(w, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT);
      return 0;
    case WM_RBUTTONDOWN:
      mouse_button(w, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT);
      return 0;
    case WM_RBUTTONUP:
      mouse_button(w, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_RIGHT);
      return 0;
    case WM_MBUTTONDOWN:
      mouse_button(w, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_MIDDLE);
      return 0;
    case WM_MBUTTONUP:
      mouse_button(w, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_MIDDLE);
      return 0;
    case WM_MOUSEWHEEL:
      ghostty_surface_mouse_scroll(
          w->surface, 0, (double)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, 0);
      return 0;
    case WM_MOUSEHWHEEL:
      ghostty_surface_mouse_scroll(
          w->surface, (double)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, 0, 0);
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(NULL, IDC_IBEAM));
        return TRUE;
      }
      break;

    case WM_SIZE:
      if (w->surface && wp != SIZE_MINIMIZED)
        ghostty_surface_set_size(w->surface, LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_DPICHANGED: {
      w->scale = HIWORD(wp) / 96.0;
      ghostty_surface_set_content_scale(w->surface, w->scale, w->scale);
      const RECT* r = (const RECT*)lp;
      SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_SETFOCUS:
      ghostty_surface_set_focus(w->surface, true);
      return 0;
    case WM_KILLFOCUS:
      ghostty_surface_set_focus(w->surface, false);
      return 0;

    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint(w);
      return 0;

    case WM_CLOSE:
      // Ghostty asks for confirmation if a process still runs, then
      // calls close_surface_cb.
      ghostty_surface_request_close(w->surface);
      return 0;
    case WM_GHOSTTY_DESTROY:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (w->surface) ghostty_surface_free(w->surface);
      w->surface = NULL;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      free(w->bgra);
      free(w);
      if (--g_windows == 0) PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK wakeup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_GHOSTTY_WAKEUP) {
    // Clear first: a wakeup during the tick queues the next one. (Not
    // drained with PeekMessage: that would also take a WM_QUIT, which
    // every filter lets through.)
    InterlockedExchange(&g_wakeup_posted, 0);
    ghostty_app_tick(g_app);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool new_window(HINSTANCE inst) {
  Window* w = calloc(1, sizeof(Window));
  if (!w) return false;
  w->hwnd = CreateWindowExW(0, kTerminalClass, L"Ghostty", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 640, NULL, NULL,
                            inst, NULL);
  if (!w->hwnd) {
    free(w);
    return false;
  }
  w->scale = window_scale(w->hwnd);
  SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);

  ghostty_surface_config_s cfg = ghostty_surface_config_new();
  cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
  cfg.platform.windows.hwnd = w->hwnd;
  cfg.userdata = w;
  cfg.scale_factor = w->scale;
  w->surface = ghostty_surface_new(g_app, &cfg);
  if (!w->surface) {
    fprintf(stderr, "ghostty: could not create the terminal surface\n");
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(w->hwnd);
    free(w);
    return false;
  }
  g_windows++;

  RECT rc;
  GetClientRect(w->hwnd, &rc);
  ghostty_surface_set_content_scale(w->surface, w->scale, w->scale);
  ghostty_surface_set_size(w->surface, (uint32_t)rc.right, (uint32_t)rc.bottom);
  ShowWindow(w->hwnd, SW_SHOWDEFAULT);
  if (config_fullscreen()) set_fullscreen(w, true);
  SetForegroundWindow(w->hwnd);
  SetFocus(w->hwnd);
  return true;
}

int main(int argc, char** argv) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  g_debug_keys = getenv("GHOSTTY_DEBUG_KEYS") != NULL;

  if (ghostty_init((uintptr_t)argc, argv) != GHOSTTY_SUCCESS) {
    fprintf(stderr, "ghostty: ghostty_init failed\n");
    return 1;
  }

  g_config = ghostty_config_new();
  ghostty_config_load_default_files(g_config);
  ghostty_config_load_cli_args(g_config);
  ghostty_config_load_recursive_files(g_config);
  ghostty_config_finalize(g_config);
  for (uint32_t i = 0; i < ghostty_config_diagnostics_count(g_config); i++) {
    ghostty_diagnostic_s d = ghostty_config_get_diagnostic(g_config, i);
    fprintf(stderr, "ghostty: config: %s\n", d.message);
  }

  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = terminal_proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.lpszClassName = kTerminalClass;
  RegisterClassExW(&wc);

  WNDCLASSEXW wk = {sizeof(wk)};
  wk.lpfnWndProc = wakeup_proc;
  wk.hInstance = inst;
  wk.lpszClassName = kWakeupClass;
  RegisterClassExW(&wk);
  g_wakeup_hwnd = CreateWindowExW(0, kWakeupClass, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, NULL, inst, NULL);

  ghostty_runtime_config_s rt = {0};
  rt.userdata = NULL;
  rt.supports_selection_clipboard = false;
  rt.wakeup_cb = wakeup_cb;
  rt.action_cb = action_cb;
  rt.read_clipboard_cb = read_clipboard_cb;
  rt.confirm_read_clipboard_cb = confirm_read_clipboard_cb;
  rt.write_clipboard_cb = write_clipboard_cb;
  rt.close_surface_cb = close_surface_cb;

  g_app = ghostty_app_new(&rt, g_config);
  if (!g_app) {
    fprintf(stderr, "ghostty: ghostty_app_new failed\n");
    return 1;
  }

  if (!new_window(inst)) return 1;

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  ghostty_app_free(g_app);
  ghostty_config_free(g_config);
  return 0;
}
