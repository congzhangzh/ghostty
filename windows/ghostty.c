// Ghostty for Windows: a Win32 host on libghostty's embedded runtime.
//
// The macOS app is a Swift program on top of libghostty; this is the same
// shape for Windows, in C on Win32. libghostty runs the terminal, the pty
// (ConPTY) and the renderer. The renderer draws with OpenGL (WGL) into its
// own offscreen framebuffer and hands each finished frame back; this host
// owns the window: it turns Win32 messages into Ghostty input, and on
// GHOSTTY_ACTION_RENDER it takes the newest frame and blits it.
//
// A window (Frame) holds tabs; each tab (Tab) is a child window with one
// surface, and only the selected tab is shown. The tab bar shows from the
// first tab, so its + is there to find (window-show-tab-bar, whose default
// is always on Windows). Built by windows/build.sh.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
// After windows.h:
#define PSAPI_VERSION 2
#include <dbghelp.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <fcntl.h>
#include <io.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "ghostty.h"

// What `ghostty.exe /buildinfo` prints: windows/build.sh writes it (with the
// release kit's own lines first in a release build).
#ifdef GHOSTTY_HAVE_BUILDINFO_H
#include "buildinfo.h"
#else
#define GHOSTTY_BUILDINFO "build info: not recorded (built without windows/build.sh)\n"
#endif
// Which build runs, for the window title: a release ("20261122.6"), else
// the source's short commit ("+mod" when built from a changed tree).
#ifndef GHOSTTY_BUILD_TAG
#define GHOSTTY_BUILD_TAG "dev"
#endif
#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)

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

typedef struct Frame Frame;

// A tab: a child window of its Frame, with one surface drawing into it.
typedef struct Window {
  Frame* frame;
  HWND hwnd;
  ghostty_surface_t surface;
  double scale;  // DPI / 96

  // The frame shown now, converted to BGRA for GDI, kept for repaints.
  uint8_t* bgra;
  uint32_t frame_w, frame_h;
  size_t bgra_cap;

  // A high surrogate from WM_CHAR waiting for its low half.
  WCHAR high_surrogate;

  // The title the terminal set (malloc'd), or NULL.
  WCHAR* title;

  // Counters for the stats overlay (Ctrl+Shift+I), kept always (cheap):
  // frames taken and the host's time on them since the overlay's last
  // update, and a key press waiting for the frame that follows it.
  uint32_t tick_frames;
  double tick_swizzle_ms, tick_blit_ms;
  uint64_t key_qpc;        // a key went down, no frame taken since
  uint64_t frame_key_qpc;  // the frame shown now is the first after it
  double latency_ms;       // that key to the end of its frame's paint
} Window;

// A top-level window: its tabs, the selected one, and the tab bar.
struct Frame {
  HWND hwnd;
  Frame* next;  // every open Frame, newest first (g_frames)
  Window** tabs;
  int ntabs, cap;
  int active;  // the selected tab, or -1
  double scale;

  // Full screen: the placement to go back to when it ends.
  bool fullscreen;
  WINDOWPLACEMENT placement;

  bool destroying;  // DestroyWindow is taking its tabs down with it

  // The tab bar under the mouse: a tab (and whether its close button),
  // the new-tab button, or nothing.
  int hover;
  bool hover_close, hover_plus, tracking;
  HFONT font;
  UINT font_dpi;

  // The stats overlay (Ctrl+Shift+I, Ghostty's inspector key: the
  // inspector itself is not in this host yet), updated every 500 ms.
  bool stats;
  uint64_t stats_qpc;
  ULONGLONG stats_cpu;  // process CPU time (100 ns) at stats_qpc
  double fps, swizzle_ms, blit_ms, cpu_pct, ws_mb, private_mb;
  HFONT stats_font;
  UINT stats_font_dpi;
};

typedef enum { TAB_BAR_AUTO, TAB_BAR_ALWAYS, TAB_BAR_NEVER } TabBarMode;

static ghostty_app_t g_app;
static ghostty_config_t g_config;
static HINSTANCE g_inst;
static HWND g_wakeup_hwnd;  // message-only window for wakeups
static volatile LONG g_wakeup_posted;  // a WM_GHOSTTY_WAKEUP is queued
static Frame* g_frames;     // open windows
static bool g_debug_keys;   // GHOSTTY_DEBUG_KEYS is set: trace key messages
static DWORD g_main_tid;    // the thread that runs the message loop
static TabBarMode g_tab_bar;     // window-show-tab-bar
static bool g_new_tab_at_end;    // window-new-tab-position = end
static COLORREF g_bg, g_fg;      // the terminal's colors, for the tab bar
static double g_qpc_ms;          // milliseconds per QueryPerformanceCounter tick
static const WCHAR* const kFrameClass = L"GhosttyWindow";
static const WCHAR* const kTerminalClass = L"GhosttyTerminal";
static const WCHAR* const kWakeupClass = L"GhosttyWakeup";

static uint64_t qpc(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (uint64_t)t.QuadPart;
}

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
// key queues WM_DEADCHAR instead: the key is composing. With Alt held they
// come as WM_SYSCHAR / WM_SYSDEADCHAR, so the range runs up to those: left
// in the queue, an Alt+key's character would arrive again as text after
// the key (a bound Alt+2 also typed "2"; Alt+b sent ESC b, then b).
static int take_chars(HWND hwnd, WCHAR* buf, int cap, bool* composing) {
  MSG msg;
  int n = 0;
  *composing = false;
  while (PeekMessageW(&msg, hwnd, WM_CHAR, WM_SYSDEADCHAR, PM_NOREMOVE)) {
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
  // Latency: a press (or repeat) starts the clock if none is running.
  if (!up && !w->key_qpc) w->key_qpc = qpc();
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
  if (state == GHOSTTY_MOUSE_PRESS) {
    SetCapture(w->hwnd);
    if (GetFocus() != w->hwnd) SetFocus(w->hwnd);
  } else
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
  uint64_t t0 = qpc();
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
  w->tick_swizzle_ms += (double)(qpc() - t0) * g_qpc_ms;
  w->tick_frames++;
  if (w->key_qpc) {
    w->frame_key_qpc = w->key_qpc;
    w->key_qpc = 0;
  }
  w->frame_w = f.width;
  w->frame_h = f.height;
  ghostty_frame_free(&f);
}

static void draw_stats(Window* w, HDC dc, RECT rc);

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
    uint64_t t0 = qpc();
    SetDIBitsToDevice(dc, 0, 0, w->frame_w, w->frame_h, 0, 0, 0, w->frame_h,
                      w->bgra, &bmi, DIB_RGB_COLORS);
    w->tick_blit_ms += (double)(qpc() - t0) * g_qpc_ms;
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
  if (w->frame->stats) draw_stats(w, dc, rc);
  EndPaint(w->hwnd, &ps);
  // The key that this frame was the first answer to is now on screen.
  if (w->frame_key_qpc) {
    w->latency_ms = (double)(qpc() - w->frame_key_qpc) * g_qpc_ms;
    w->frame_key_qpc = 0;
  }
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
  int r = MessageBoxW(w->frame->hwnd, what, L"Ghostty", MB_YESNO | MB_ICONWARNING);
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
        MessageBoxW(w->frame->hwnd,
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
static void set_fullscreen(Frame* w, bool on) {
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
// Hang and crash reports
//
// Hangs: a watchdog thread sends the main thread an empty message every
// second. When it gets no answer for 5 seconds (the point where Windows
// shows "Not Responding"), it starts this same exe again as
//   ghostty.exe --windows-hang-report <pid> <main thread id> <file>
// Crashes: an unhandled exception (an access violation, a Zig panic, which
// libghostty raises as GHOSTTY_PANIC_CODE on Windows) or abort() starts
//   ghostty.exe --windows-crash-report <pid> <thread id> <file> <exception pointers>
// and the crashed thread waits for it. Either way the second process
// suspends our threads, walks their stacks (the crashed one from the
// exception's own context) and writes them, module+offset, with a minidump
// beside them, to %LOCALAPPDATA%\ghostty\hangs or ...\crashes. Another
// process does the walking because a thread stopped inside the heap or the
// loader would deadlock a walker in this one (Crashpad's reason too). No
// log setup, no tool beside the exe.

// libghostty's panic exception (src/main_c.zig windows_panic_code): its
// parameters are the message's address and length.
#define GHOSTTY_PANIC_CODE 0xE0474850u

static volatile LONG g_loop_done;  // the message loop has ended (quitting)
static WCHAR g_exe[MAX_PATH];       // this exe, for the report process

// %LOCALAPPDATA%\ghostty\<kind>, created; false if there is no such folder.
static bool report_dir(const WCHAR* kind, WCHAR* out, size_t cap) {
  WCHAR base[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  _snwprintf(out, cap, L"%ls\\ghostty", base);
  CreateDirectoryW(out, NULL);
  _snwprintf(out, cap, L"%ls\\ghostty\\%ls", base, kind);
  CreateDirectoryW(out, NULL);
  return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// Start the report process; `wait`: until it has finished (a crash: the
// process must still be here while it is read). Returns the report's path
// in `file` (empty if none was started).
static void start_report(const WCHAR* kind, DWORD tid, const void* ep,
                         bool wait, WCHAR* file, size_t cap) {
  WCHAR dir[MAX_PATH], cmd[3 * MAX_PATH + 64];
  file[0] = 0;
  if (!g_exe[0] || !report_dir(kind, dir, MAX_PATH)) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  _snwprintf(file, cap, L"%ls\\%.5ls-%04d%02d%02d-%02d%02d%02d-%lu.txt", dir,
             kind, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             GetCurrentProcessId());
  _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]),
             L"\"%ls\" --windows-%.5ls-report %lu %lu \"%ls\" %llx", g_exe,
             kind, GetCurrentProcessId(), tid, file,
             (unsigned long long)(uintptr_t)ep);
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                      NULL, &si, &pi)) {
    file[0] = 0;
    return;
  }
  CloseHandle(pi.hThread);
  if (wait) WaitForSingleObject(pi.hProcess, 60000);
  CloseHandle(pi.hProcess);
}

static void say_report(const char* what, const WCHAR* file) {
  // Last, and after the report: stderr itself may be what blocks.
  char* f8 = utf16_to_utf8(file, -1);
  fprintf(stderr, "ghostty: %s; report: %s\n", what, f8 && *f8 ? f8 : "none");
  fflush(stderr);
  free(f8);
}

static DWORD WINAPI watchdog_main(void* arg) {
  (void)arg;
  DWORD silent = 0;
  int reports = 0;
  for (;;) {
    Sleep(1000);
    DWORD_PTR r;
    if (SendMessageTimeoutW(g_wakeup_hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 1000,
                            &r)) {
      silent = 0;
      continue;
    }
    silent += 2;  // the second asleep and the second waited
    // One report per hang, at most three per run.
    if (silent >= 5 && silent < 7 && reports < 3) {
      reports++;
      WCHAR file[MAX_PATH];
      start_report(L"hangs", g_main_tid, NULL, false, file, MAX_PATH);
      say_report(g_loop_done
                     ? "the main thread has not answered for 5s (after the "
                       "message loop ended)"
                     : "the main thread has not answered for 5s",
                 file);
    }
  }
  return 0;
}

static volatile LONG g_crashing;

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
  if (InterlockedExchange(&g_crashing, 1)) return EXCEPTION_CONTINUE_SEARCH;
  WCHAR file[MAX_PATH];
  start_report(L"crashes", GetCurrentThreadId(), ep, true, file, MAX_PATH);
  char what[64];
  snprintf(what, sizeof(what), "crashed (exception 0x%08lx)",
           ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
  say_report(what, file);
  // Windows goes on as before: Error Reporting, then the process ends.
  return EXCEPTION_CONTINUE_SEARCH;
}

static void abort_handler(int sig) {
  (void)sig;
  if (!InterlockedExchange(&g_crashing, 1)) {
    WCHAR file[MAX_PATH];
    start_report(L"crashes", GetCurrentThreadId(), NULL, true, file, MAX_PATH);
    say_report("aborted", file);
  }
  _exit(3);  // not the C runtime's abort dialog
}

static void start_reports(void) {
  if (!GetModuleFileNameW(NULL, g_exe, MAX_PATH)) g_exe[0] = 0;
  if (getenv("GHOSTTY_NO_REPORTS")) return;
  SetUnhandledExceptionFilter(crash_filter);
  signal(SIGABRT, abort_handler);
  HANDLE h = CreateThread(NULL, 0, watchdog_main, NULL, 0, NULL);
  if (h) CloseHandle(h);
}

// The report itself, in the second process.

#define REPORT_MAX_FRAMES 64

static void report_frame(FILE* out, HANDLE proc, DWORD64 a) {
  IMAGEHLP_MODULEW64 mi = {sizeof(mi)};
  DWORD64 base = 0;
  WCHAR mod[64] = L"?";
  if (SymGetModuleInfoW64(proc, a, &mi)) {
    _snwprintf(mod, 64, L"%ls", mi.ModuleName);
    base = mi.BaseOfImage;
  }
  char buf[sizeof(SYMBOL_INFO) + 256];
  SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(proc, a, &disp, si))
    fprintf(out, "    %ls+0x%llx  %s+0x%llx\n", mod,
            (unsigned long long)(a - base), si->Name, (unsigned long long)disp);
  else
    fprintf(out, "    %ls+0x%llx\n", mod, (unsigned long long)(a - base));
}

// Walk a stack from `ctx` (a copy: StackWalk64 changes it). The CONTEXT
// and the machine are this build's own: the helper reads its own exe's
// threads.
#if defined(__aarch64__) || defined(_M_ARM64)
#define WALK_MACHINE IMAGE_FILE_MACHINE_ARM64
#define WALK_PC(c) (c).Pc
#define WALK_SP(c) (c).Sp
#define WALK_FP(c) (c).Fp
#elif defined(__x86_64__) || defined(_M_X64)
#define WALK_MACHINE IMAGE_FILE_MACHINE_AMD64
#define WALK_PC(c) (c).Rip
#define WALK_SP(c) (c).Rsp
#define WALK_FP(c) (c).Rbp
#else
#error "stack walk: x86_64 or aarch64 only"
#endif
static int walk(HANDLE proc, HANDLE th, CONTEXT ctx, DWORD64* frames) {
  STACKFRAME64 sf = {0};
  sf.AddrPC.Offset = WALK_PC(ctx);
  sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrStack.Offset = WALK_SP(ctx);
  sf.AddrStack.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = WALK_FP(ctx);
  sf.AddrFrame.Mode = AddrModeFlat;
  int n = 0;
  while (n < REPORT_MAX_FRAMES &&
         StackWalk64(WALK_MACHINE, proc, th, &sf, &ctx, NULL,
                     SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
    if (!sf.AddrPC.Offset) break;
    frames[n++] = sf.AddrPC.Offset;
  }
  return n;
}

typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);

static const char* exception_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    case 0xC0000374: return "heap corruption";
    case GHOSTTY_PANIC_CODE: return "Zig panic";
    default: return "?";
  }
}

// The exception, read from the crashed process; fills *ctx and returns true
// if there was one.
static bool report_exception(FILE* out, HANDLE proc, uintptr_t ep_addr,
                             CONTEXT* ctx) {
  EXCEPTION_POINTERS ep;
  EXCEPTION_RECORD er;
  SIZE_T got;
  if (!ep_addr ||
      !ReadProcessMemory(proc, (void*)ep_addr, &ep, sizeof(ep), &got) ||
      !ReadProcessMemory(proc, ep.ExceptionRecord, &er, sizeof(er), &got) ||
      !ReadProcessMemory(proc, ep.ContextRecord, ctx, sizeof(*ctx), &got)) {
    fprintf(out, "exception: none (abort)\n\n");
    return false;
  }
  fprintf(out, "exception: 0x%08lx %s\nat: ", er.ExceptionCode,
          exception_name(er.ExceptionCode));
  report_frame(out, proc, (DWORD64)(uintptr_t)er.ExceptionAddress);
  if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2)
    fprintf(out, "  %s address 0x%llx\n",
            er.ExceptionInformation[0] == 0   ? "reading"
            : er.ExceptionInformation[0] == 8 ? "executing"
                                              : "writing",
            (unsigned long long)er.ExceptionInformation[1]);
  if (er.ExceptionCode == GHOSTTY_PANIC_CODE && er.NumberParameters >= 2) {
    char msg[2048];
    size_t len = er.ExceptionInformation[1];
    if (len >= sizeof(msg)) len = sizeof(msg) - 1;
    if (ReadProcessMemory(proc, (void*)er.ExceptionInformation[0], msg, len,
                          &got)) {
      msg[got] = 0;
      fprintf(out, "  panic: %s\n", msg);
    }
  }
  fprintf(out, "\n");
  return true;
}

static int write_report(bool crash, DWORD pid, DWORD tid, const WCHAR* path,
                        uintptr_t ep_addr) {
  FILE* out = _wfopen(path, L"w");
  if (!out) return 1;
  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                PROCESS_DUP_HANDLE,
                            FALSE, pid);
  if (!proc) {
    fprintf(out, "OpenProcess(%lu): error %lu\n", pid, GetLastError());
    fclose(out);
    return 1;
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  WCHAR exe[MAX_PATH] = L"?";
  DWORD exe_len = MAX_PATH;
  QueryFullProcessImageNameW(proc, 0, exe, &exe_len);
  fprintf(out,
          "ghostty %s report %04d-%02d-%02d %02d:%02d:%02d\n"
          "pid %lu, %s thread %lu\nexe %ls\n\n",
          crash ? "crash" : "hang", t.wYear, t.wMonth, t.wDay, t.wHour,
          t.wMinute, t.wSecond, pid, crash ? "crashed" : "main (UI)", tid, exe);

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitializeW(proc, NULL, TRUE);

  // A crash: the exception, and the crashed thread's stack from its own
  // context (its current stack is only our filter waiting for us).
  CONTEXT ectx;
  if (crash && report_exception(out, proc, ep_addr, &ectx)) {
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    DWORD64 frames[REPORT_MAX_FRAMES];
    int n = walk(proc, th, ectx, frames);
    fprintf(out, "crashed thread %lu, at the exception:\n", tid);
    for (int i = 0; i < n; i++) report_frame(out, proc, frames[i]);
    fprintf(out, "\n");
    if (th) CloseHandle(th);
  }

  // The modules, so an offset can be matched to its build.
  HMODULE mods[512];
  DWORD need = 0;
  if (K32EnumProcessModules(proc, mods, sizeof(mods), &need)) {
    fprintf(out, "modules:\n");
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) {
      WCHAR name[MAX_PATH];
      MODULEINFO mi;
      if (K32GetModuleFileNameExW(proc, mods[i], name, MAX_PATH) &&
          K32GetModuleInformation(proc, mods[i], &mi, sizeof(mi)))
        fprintf(out, "  %p %8lx %ls\n", mi.lpBaseOfDll, mi.SizeOfImage, name);
    }
    fprintf(out, "\n");
  }

  GetThreadDescriptionFn describe = (GetThreadDescriptionFn)(void*)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");

  // Every thread: three samples for a hang (what moves, what does not),
  // one for a crash.
  int samples = crash ? 1 : 3;
  for (int s = 0; s < samples; s++) {
    if (s) Sleep(700);
    fprintf(out, "=== %s %d\n", crash ? "threads" : "sample", s + 1);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid) continue;
      HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_INFORMATION,
                             FALSE, te.th32ThreadID);
      if (!th) continue;
      FILETIME c, e, k, u;
      GetThreadTimes(th, &c, &e, &k, &u);
      double ks = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) / 1e7;
      double us = (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime) / 1e7;
      DWORD64 frames[REPORT_MAX_FRAMES];
      int n = 0;
      if (SuspendThread(th) != (DWORD)-1) {
        CONTEXT ctx = {0};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) n = walk(proc, th, ctx, frames);
        ResumeThread(th);
      }
      PWSTR desc = NULL;
      if (describe && FAILED(describe(th, &desc))) desc = NULL;
      fprintf(out, "tid %lu%s%s%ls  kernel %.1fs user %.1fs  frames %d\n",
              te.th32ThreadID, te.th32ThreadID == tid ? (crash ? " (crashed)" : " (main)") : "",
              desc && *desc ? " " : "", desc && *desc ? desc : L"", ks, us, n);
      if (desc) LocalFree(desc);
      for (int i = 0; i < n; i++) report_frame(out, proc, frames[i]);
      CloseHandle(th);
    }
    CloseHandle(snap);
    fflush(out);
  }

  // A minidump beside it: every thread's stack memory (and, for a crash,
  // the exception), for a debugger.
  WCHAR dmp[MAX_PATH];
  _snwprintf(dmp, MAX_PATH, L"%ls", path);
  WCHAR* dot = wcsrchr(dmp, L'.');
  if (dot) wcscpy(dot, L".dmp");
  HANDLE f = CreateFileW(dmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei = {tid, (EXCEPTION_POINTERS*)ep_addr,
                                          TRUE};
    BOOL ok = MiniDumpWriteDump(
        proc, pid, f,
        (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                        MiniDumpWithHandleData),
        crash && ep_addr ? &mei : NULL, NULL, NULL);
    CloseHandle(f);
    fprintf(out, "\nminidump: %ls (%s)\n", dmp, ok ? "written" : "failed");
  }
  fclose(out);
  SymCleanup(proc);
  CloseHandle(proc);
  return 0;
}

// `ghostty.exe --windows-hang-report|--windows-crash-report <pid> <tid>
// <file> <exception pointers, hex>`: true if this run is one (and it has
// been done).
static bool report_mode(int* rc) {
  int argc = 0;
  WCHAR** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  bool hang = argc == 6 && wcscmp(argv[1], L"--windows-hangs-report") == 0;
  bool crash = argc == 6 && wcscmp(argv[1], L"--windows-crash-report") == 0;
  if (hang || crash)
    *rc = write_report(crash, (DWORD)wcstoul(argv[2], NULL, 10),
                       (DWORD)wcstoul(argv[3], NULL, 10), argv[4],
                       (uintptr_t)wcstoull(argv[5], NULL, 16));
  LocalFree(argv);
  return hang || crash;
}

// ---------------------------------------------------------------------------
// stderr that never blocks
//
// A write to a pipe nobody reads blocks once the pipe's buffer is full:
// Windows PowerShell's `ghostty.exe 2> log` gives us such a pipe and stops
// reading it when the prompt returns (a GUI program is not waited for).
// Then the next log line (GHOSTTY_LOG=stderr: a warning per click) blocks
// whichever thread writes it, the UI thread too. A console can block the
// same way while text is selected in it.
//
// So when stderr is a pipe or a character device, it is replaced by a pipe
// of our own, before libghostty or the CRT writes anything: SetStdHandle
// for Zig's std (it reads the PEB's hStdError, once) and fd 2 for the CRT.
// A reader thread empties that pipe into a ring at once; a writer thread
// forwards the ring to the real stderr and is the only one that can block.
// When the ring is full, what comes in is dropped and counted, and the
// count is written when the real stderr takes writes again. A disk file
// (cmd's `2> log`) is left alone: a write to it does not wait on anyone.
// GHOSTTY_STDERR=direct turns this off.

#define ERR_RING_SIZE (256 * 1024)

static struct {
  HANDLE out;  // the real stderr
  HANDLE rd;   // our pipe's read end
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE more;  // bytes in the ring, or bytes dropped
  CONDITION_VARIABLE less;  // the writer took bytes out (for the flush)
  char ring[ERR_RING_SIZE];
  size_t head, len;  // len counts the bytes being written, too
  unsigned long long dropped, dropped_total;
  bool on;
} g_err;

static DWORD WINAPI err_reader_main(void* arg) {
  (void)arg;
  char buf[4096];
  DWORD n;
  while (ReadFile(g_err.rd, buf, sizeof(buf), &n, NULL) && n > 0) {
    EnterCriticalSection(&g_err.lock);
    if (ERR_RING_SIZE - g_err.len < n) {
      // Whole reads only: a read is usually whole lines.
      g_err.dropped += n;
      g_err.dropped_total += n;
    } else {
      size_t tail = (g_err.head + g_err.len) % ERR_RING_SIZE;
      size_t first = ERR_RING_SIZE - tail < n ? ERR_RING_SIZE - tail : n;
      memcpy(g_err.ring + tail, buf, first);
      memcpy(g_err.ring, buf + first, n - first);
      g_err.len += n;
    }
    WakeConditionVariable(&g_err.more);
    LeaveCriticalSection(&g_err.lock);
  }
  return 0;
}

static DWORD WINAPI err_writer_main(void* arg) {
  (void)arg;
  for (;;) {
    EnterCriticalSection(&g_err.lock);
    while (g_err.len == 0 && g_err.dropped == 0)
      SleepConditionVariableCS(&g_err.more, &g_err.lock, INFINITE);
    char note[96];
    const char* p;
    size_t n;
    bool is_note = g_err.len == 0;
    if (is_note) {
      n = (size_t)snprintf(note, sizeof(note),
                           "ghostty: %llu bytes of log dropped: stderr was "
                           "not read\n",
                           g_err.dropped);
      g_err.dropped = 0;
      p = note;
    } else {
      p = g_err.ring + g_err.head;
      n = ERR_RING_SIZE - g_err.head < g_err.len ? ERR_RING_SIZE - g_err.head
                                                 : g_err.len;
    }
    LeaveCriticalSection(&g_err.lock);

    // May block for ever; only this thread waits then.
    DWORD w = 0;
    size_t done = 0;
    while (done < n &&
           WriteFile(g_err.out, p + done, (DWORD)(n - done), &w, NULL) && w)
      done += w;

    EnterCriticalSection(&g_err.lock);
    if (!is_note) {
      // Taken out even when the write failed: nobody would read it anyway.
      g_err.head = (g_err.head + n) % ERR_RING_SIZE;
      g_err.len -= n;
    }
    WakeAllConditionVariable(&g_err.less);
    LeaveCriticalSection(&g_err.lock);
  }
}

static void stderr_guard_start(void) {
  const char* mode = getenv("GHOSTTY_STDERR");
  if (mode && strcmp(mode, "direct") == 0) return;
  HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
  if (!out || out == INVALID_HANDLE_VALUE) return;
  DWORD type = GetFileType(out);
  if (type != FILE_TYPE_PIPE && type != FILE_TYPE_CHAR) return;

  // Not inheritable (no SECURITY_ATTRIBUTES): the shells in the tabs and
  // the report helper never hold our pipe.
  HANDLE rd, wr;
  if (!CreatePipe(&rd, &wr, NULL, 64 * 1024)) return;
  // Our own copy of the real stderr: _dup2 below closes the CRT's fd 2,
  // and the handle under it with it.
  if (!DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &out, 0,
                       FALSE, DUPLICATE_SAME_ACCESS)) {
    CloseHandle(rd);
    CloseHandle(wr);
    return;
  }
  // Text mode, as the CRT's own stderr was (its "\n" becomes "\r\n").
  int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
  if (fd < 0) {
    CloseHandle(out);
    CloseHandle(rd);
    CloseHandle(wr);
    return;
  }
  g_err.out = out;
  g_err.rd = rd;
  InitializeCriticalSection(&g_err.lock);
  InitializeConditionVariable(&g_err.more);
  InitializeConditionVariable(&g_err.less);
  // Suspended until both exist: a thread that never ran is safe to end.
  HANDLE t1 = CreateThread(NULL, 64 * 1024, err_reader_main, NULL,
                           CREATE_SUSPENDED, NULL);
  HANDLE t2 = CreateThread(NULL, 64 * 1024, err_writer_main, NULL,
                           CREATE_SUSPENDED, NULL);
  if (!t1 || !t2) {
    // Leave stderr as it was.
    if (t1) TerminateThread(t1, 0), CloseHandle(t1);
    if (t2) TerminateThread(t2, 0), CloseHandle(t2);
    _close(fd);
    CloseHandle(rd);
    CloseHandle(out);
    return;
  }
  ResumeThread(t1);
  ResumeThread(t2);
  CloseHandle(t1);
  CloseHandle(t2);
  fflush(stderr);
  _dup2(fd, 2);  // the CRT's stderr (fd 2) now writes to our pipe
  _close(fd);
  SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
  setvbuf(stderr, NULL, _IONBF, 0);
  g_err.on = true;
}

// At exit: give what is still in our pipe and ring up to `ms` to reach the
// real stderr (which may never take it).
static void stderr_guard_flush(DWORD ms) {
  if (!g_err.on) return;
  fflush(stderr);
  DWORD start = GetTickCount();
  for (;;) {
    DWORD left = ms - (GetTickCount() - start);
    if ((int)left <= 0) return;
    DWORD avail = 0;
    PeekNamedPipe(g_err.rd, NULL, 0, NULL, &avail, NULL);
    EnterCriticalSection(&g_err.lock);
    bool empty = avail == 0 && g_err.len == 0 && g_err.dropped == 0;
    if (!empty)
      SleepConditionVariableCS(&g_err.less, &g_err.lock, left < 20 ? left : 20);
    LeaveCriticalSection(&g_err.lock);
    if (empty) return;
  }
}

// ---------------------------------------------------------------------------
// /buildinfo

// Text to where it can be read: stdout when it goes somewhere (a pipe, a
// file: `ghostty.exe /buildinfo > info.txt`), else the console this was
// started from (a GUI program has none of its own), else a message box.
static void print_text(const char* s) {
  DWORD w;
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) {
    WriteFile(h, s, (DWORD)strlen(s), &w, NULL);
    return;
  }
  WCHAR* ws = utf8_to_utf16(s, -1);
  if (!ws) return;
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    HANDLE c = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (c != INVALID_HANDLE_VALUE) {
      WriteConsoleW(c, L"\n", 1, &w, NULL);
      WriteConsoleW(c, ws, (DWORD)wcslen(ws), &w, NULL);
      CloseHandle(c);
      free(ws);
      return;
    }
  }
  MessageBoxW(NULL, ws, L"Ghostty build info", MB_OK | MB_ICONINFORMATION);
  free(ws);
}

static bool buildinfo_mode(int argc, char** argv) {
  bool asked = false;
  for (int i = 1; i < argc; i++)
    if (strcmp(argv[i], "/buildinfo") == 0 || strcmp(argv[i], "--buildinfo") == 0)
      asked = true;
  if (!asked) return false;
  ghostty_info_s gi = ghostty_info();
  static const char* const modes[] = {"Debug", "ReleaseSafe", "ReleaseFast",
                                      "ReleaseSmall"};
  const char* mode = (unsigned)gi.build_mode < 4 ? modes[gi.build_mode] : "?";
  char text[16384];
  snprintf(text, sizeof(text),
           "Ghostty for Windows\n%s"
           "libghostty: %.*s, %s; renderer OpenGL 4.3 core through WGL, "
           "frames through the CPU; pty: ConPTY\n"
           "host: windows/ghostty.c, clang %s\n",
           GHOSTTY_BUILDINFO, (int)gi.version_len, gi.version, mode,
           __clang_version__);
  print_text(text);
  return true;
}

// ---------------------------------------------------------------------------
// Config the host reads

// A config value that is a string (enums come back as their name), or NULL.
static const char* config_string(const char* key) {
  const char* v = NULL;
  if (!ghostty_config_get(g_config, &v, key, strlen(key))) return NULL;
  return v;
}

static COLORREF config_color(const char* key, COLORREF fallback) {
  ghostty_config_color_s c;
  if (!ghostty_config_get(g_config, &c, key, strlen(key))) return fallback;
  return RGB(c.r, c.g, c.b);
}

static void read_host_config(void) {
  const char* v = config_string("window-show-tab-bar");
  g_tab_bar = !v                         ? TAB_BAR_AUTO
              : strcmp(v, "always") == 0 ? TAB_BAR_ALWAYS
              : strcmp(v, "never") == 0  ? TAB_BAR_NEVER
                                         : TAB_BAR_AUTO;
  v = config_string("window-new-tab-position");
  g_new_tab_at_end = v && strcmp(v, "end") == 0;
  g_bg = config_color("background", RGB(0x28, 0x2c, 0x34));
  g_fg = config_color("foreground", RGB(0xff, 0xff, 0xff));
}

// ---------------------------------------------------------------------------
// The tab bar
//
// Drawn by the Frame in its client area above the tabs, in the terminal's
// own colors as macOS does: the selected tab has the terminal's background
// and the others are darker; tabs share the width equally, the title is
// centered, a tab under the mouse shows its close button on the left, and
// the + at the right end opens a new tab.

static int px(Frame* f, double points) { return (int)(points * f->scale + 0.5); }

static int bar_height(Frame* f) {
  bool show = g_tab_bar == TAB_BAR_ALWAYS ||
              (g_tab_bar == TAB_BAR_AUTO && f->ntabs > 1);
  return show ? px(f, 30) : 0;
}

static COLORREF mix(COLORREF a, COLORREF b, double t) {
  return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t + 0.5),
             (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t + 0.5),
             (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t + 0.5));
}

static bool is_dark(COLORREF c) {
  return 0.299 * GetRValue(c) + 0.587 * GetGValue(c) + 0.114 * GetBValue(c) <
         128;
}

static void tab_rect(Frame* f, int width, int b, int i, RECT* r) {
  int area = width - b;  // the + button is b wide
  if (area < 0) area = 0;
  r->left = (int)((long long)area * i / f->ntabs);
  r->right = (int)((long long)area * (i + 1) / f->ntabs);
  r->top = 0;
  r->bottom = b;
}

static void close_rect(Frame* f, const RECT* tab, RECT* r) {
  int s = px(f, 16), pad = px(f, 7);
  r->left = tab->left + pad;
  r->right = r->left + s;
  r->top = (tab->bottom - s) / 2;
  r->bottom = r->top + s;
}

typedef enum { HIT_NONE, HIT_TAB, HIT_CLOSE, HIT_PLUS } BarHit;

static BarHit bar_hit(Frame* f, int x, int y, int* index) {
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  int b = bar_height(f);
  *index = -1;
  if (b == 0 || y < 0 || y >= b || x < 0 || x >= rc.right) return HIT_NONE;
  if (x >= rc.right - b) return HIT_PLUS;
  for (int i = 0; i < f->ntabs; i++) {
    RECT t, c;
    tab_rect(f, rc.right, b, i, &t);
    if (x < t.left || x >= t.right) continue;
    *index = i;
    close_rect(f, &t, &c);
    POINT p = {x, y};
    return PtInRect(&c, p) ? HIT_CLOSE : HIT_TAB;
  }
  return HIT_NONE;
}

static HFONT bar_font(Frame* f) {
  UINT dpi = (UINT)(f->scale * 96 + 0.5);
  if (f->font && f->font_dpi == dpi) return f->font;
  if (f->font) DeleteObject(f->font);
  NONCLIENTMETRICSW m = {sizeof(m)};
  LOGFONTW lf;
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0,
                                 dpi)) {
    lf = m.lfMessageFont;
  } else {
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(9, (int)dpi, 72);
    wcscpy(lf.lfFaceName, L"Segoe UI");
  }
  f->font = CreateFontIndirectW(&lf);
  f->font_dpi = dpi;
  return f->font;
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  RECT x = {l, t, r, b};
  SetDCBrushColor(dc, c);
  FillRect(dc, &x, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void stroke(HDC dc, int x0, int y0, int x1, int y1, int width,
                   COLORREF c) {
  HPEN pen = CreatePen(PS_SOLID, width, c);
  HGDIOBJ old = SelectObject(dc, pen);
  MoveToEx(dc, x0, y0, NULL);
  LineTo(dc, x1, y1);
  SelectObject(dc, old);
  DeleteObject(pen);
}

static void draw_bar(Frame* f, HDC dc, int width, int b) {
  bool dark = is_dark(g_bg);
  COLORREF bar = mix(g_bg, RGB(0, 0, 0), dark ? 0.35 : 0.10);
  COLORREF hover = mix(bar, g_bg, 0.5);
  COLORREF line = mix(bar, g_fg, 0.12);
  COLORREF dim = mix(g_fg, bar, 0.45);
  int lw = px(f, 1.25) > 0 ? px(f, 1.25) : 1;

  fill(dc, 0, 0, width, b, bar);
  SelectObject(dc, bar_font(f));
  SetBkMode(dc, TRANSPARENT);

  for (int i = 0; i < f->ntabs; i++) {
    Window* t = f->tabs[i];
    RECT r, c;
    tab_rect(f, width, b, i, &r);
    bool sel = i == f->active, over = i == f->hover;
    if (sel || over) fill(dc, r.left, r.top, r.right, r.bottom, sel ? g_bg : hover);
    // A divider between two tabs that are not selected.
    if (i + 1 < f->ntabs && !sel && i + 1 != f->active)
      fill(dc, r.right - 1, px(f, 7), r.right, b - px(f, 7), line);
    if (!sel) fill(dc, r.left, b - 1, r.right, b, line);  // the bar's edge

    close_rect(f, &r, &c);
    RECT text = r;
    int inset = c.right - r.left + px(f, 4);  // keep the title centered
    text.left += inset;
    text.right -= inset;
    SetTextColor(dc, sel ? g_fg : dim);
    DrawTextW(dc, t->title ? t->title : L"Ghostty", -1, &text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

    if (over) {
      if (f->hover_close)
        fill(dc, c.left, c.top, c.right, c.bottom,
             mix(sel ? g_bg : hover, g_fg, 0.15));
      int m = px(f, 4.5);
      COLORREF xc = sel ? g_fg : dim;
      stroke(dc, c.left + m, c.top + m, c.right - m, c.bottom - m, lw, xc);
      stroke(dc, c.right - m, c.top + m, c.left + m, c.bottom - m, lw, xc);
    }
  }

  // The new-tab button.
  int x0 = width - b;
  if (f->hover_plus) fill(dc, x0, 0, width, b, hover);
  fill(dc, x0, b - 1, width, b, line);
  int cx = x0 + b / 2, cy = b / 2, h = px(f, 6);
  stroke(dc, cx - h, cy, cx + h + 1, cy, lw, dim);
  stroke(dc, cx, cy - h, cx, cy + h + 1, lw, dim);
}

static void paint_frame(Frame* f) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(f->hwnd, &ps);
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  int b = bar_height(f);
  if (b > 0 && rc.right > 0) {
    // Draw off screen, then copy: no flicker while the mouse moves.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, b);
    HGDIOBJ old = SelectObject(mem, bmp);
    draw_bar(f, mem, rc.right, b);
    BitBlt(dc, 0, 0, rc.right, b, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
  }
  EndPaint(f->hwnd, &ps);
}

static void invalidate_bar(Frame* f) {
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  rc.bottom = bar_height(f);
  if (rc.bottom > 0) InvalidateRect(f->hwnd, &rc, FALSE);
}

// ---------------------------------------------------------------------------
// Tabs

static int tab_index(Frame* f, Window* w) {
  for (int i = 0; i < f->ntabs; i++)
    if (f->tabs[i] == w) return i;
  return -1;
}

// The window's title (the taskbar's too): the selected tab's title and
// which build runs, "<title> \u2014 Ghostty <build>". The tabs show the
// titles alone.
static void update_title(Frame* f) {
  static const WCHAR app[] = L"Ghostty " WIDEN(GHOSTTY_BUILD_TAG);
  Window* w = f->active >= 0 ? f->tabs[f->active] : NULL;
  if (!w || !w->title || !*w->title) {
    SetWindowTextW(f->hwnd, app);
    return;
  }
  size_t n = wcslen(w->title) + wcslen(app) + 4;
  WCHAR* t = malloc(n * sizeof(WCHAR));
  if (!t) {
    SetWindowTextW(f->hwnd, w->title);
    return;
  }
  swprintf(t, n, L"%ls \u2014 %ls", w->title, app);
  SetWindowTextW(f->hwnd, t);
  free(t);
}

// Size every tab to the client area under the tab bar. The hidden ones
// too, so switching to one shows a frame of the right size at once.
static void layout(Frame* f) {
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) return;  // minimized
  int b = bar_height(f);
  int h = rc.bottom > b ? rc.bottom - b : 0;
  for (int i = 0; i < f->ntabs; i++)
    SetWindowPos(f->tabs[i]->hwnd, NULL, 0, b, rc.right, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  InvalidateRect(f->hwnd, NULL, FALSE);
}

// Show tab i and hide the one shown before. A hidden tab's surface is
// told it is occluded, so its renderer stops drawing frames nobody sees.
static void select_tab(Frame* f, int i) {
  if (i < 0 || i >= f->ntabs) return;
  Window* old = f->active >= 0 && f->active < f->ntabs ? f->tabs[f->active]
                                                        : NULL;
  Window* w = f->tabs[i];
  f->active = i;
  if (old != w) ghostty_surface_set_occlusion(w->surface, true);
  ShowWindow(w->hwnd, SW_SHOW);
  if (GetActiveWindow() == f->hwnd) SetFocus(w->hwnd);
  if (old && old != w) {
    ShowWindow(old->hwnd, SW_HIDE);
    ghostty_surface_set_occlusion(old->surface, false);
  }
  update_title(f);
  invalidate_bar(f);
}

// Take a tab out of its Frame (it closed, or moves to another window).
static void detach_tab(Frame* f, Window* w) {
  int i = tab_index(f, w);
  if (i < 0) return;
  memmove(&f->tabs[i], &f->tabs[i + 1],
          (size_t)(f->ntabs - i - 1) * sizeof(f->tabs[0]));
  f->ntabs--;
  if (f->destroying) return;
  if (f->ntabs == 0) {
    f->active = -1;
    PostMessageW(f->hwnd, WM_GHOSTTY_DESTROY, 0, 0);
    return;
  }
  if (i == f->active) {
    // The tab that takes its place, or the last one.
    f->active = -1;
    layout(f);
    select_tab(f, i < f->ntabs ? i : f->ntabs - 1);
  } else {
    if (i < f->active) f->active--;
    layout(f);
  }
}

static bool insert_tab(Frame* f, Window* w, int at) {
  if (f->ntabs == f->cap) {
    int cap = f->cap ? f->cap * 2 : 4;
    Window** p = realloc(f->tabs, (size_t)cap * sizeof(*p));
    if (!p) return false;
    f->tabs = p;
    f->cap = cap;
  }
  if (at < 0 || at > f->ntabs) at = f->ntabs;
  memmove(&f->tabs[at + 1], &f->tabs[at],
          (size_t)(f->ntabs - at) * sizeof(f->tabs[0]));
  f->tabs[at] = w;
  f->ntabs++;
  if (f->active >= at) f->active++;
  w->frame = f;
  return true;
}

// Give a tab the size of its place in the Frame now (its WM_SIZE may have
// come before it had a surface).
static void size_tab(Window* w) {
  RECT rc;
  GetClientRect(w->hwnd, &rc);
  if (rc.right > 0 && rc.bottom > 0)
    ghostty_surface_set_size(w->surface, (uint32_t)rc.right,
                             (uint32_t)rc.bottom);
}

// A new tab in f, after the selected one (or at the end), inheriting the
// working directory and font size of `from` when there is one.
static Window* new_tab(Frame* f, ghostty_surface_t from,
                       ghostty_surface_context_e context) {
  Window* w = calloc(1, sizeof(Window));
  if (!w) return NULL;
  w->frame = f;
  w->scale = f->scale;
  w->hwnd = CreateWindowExW(0, kTerminalClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, f->hwnd, NULL, g_inst, NULL);
  if (!w->hwnd) {
    free(w);
    return NULL;
  }
  SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);

  ghostty_surface_config_s cfg = from ? ghostty_surface_inherited_config(from, context)
                                      : ghostty_surface_config_new();
  cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
  cfg.platform.windows.hwnd = w->hwnd;
  cfg.userdata = w;
  cfg.scale_factor = w->scale;
  cfg.context = context;
  w->surface = ghostty_surface_new(g_app, &cfg);
  if (!w->surface || !insert_tab(f, w, g_new_tab_at_end || f->active < 0
                                           ? f->ntabs
                                           : f->active + 1)) {
    fprintf(stderr, "ghostty: could not create the terminal surface\n");
    if (w->surface) ghostty_surface_free(w->surface);
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(w->hwnd);
    free(w);
    return NULL;
  }
  ghostty_surface_set_content_scale(w->surface, w->scale, w->scale);
  layout(f);
  size_tab(w);
  select_tab(f, tab_index(f, w));
  return w;
}

// Move the selected tab by `amount`, wrapping around the ends.
static void move_tab(Frame* f, int amount) {
  int n = f->ntabs, from = f->active;
  int to = ((from + amount) % n + n) % n;
  Window* w = f->tabs[from];
  if (to > from)
    memmove(&f->tabs[from], &f->tabs[from + 1],
            (size_t)(to - from) * sizeof(w));
  else
    memmove(&f->tabs[to + 1], &f->tabs[to], (size_t)(from - to) * sizeof(w));
  f->tabs[to] = w;
  f->active = to;
  invalidate_bar(f);
}

// Close a tab as its close button does: Ghostty asks the surface to close
// and calls close_surface_cb when it does.
static void close_tab(Window* w) { ghostty_surface_request_close(w->surface); }

// ---------------------------------------------------------------------------
// Stats overlay
//
// Ctrl+Shift+I (Ghostty's inspector key) shows what the frame path costs,
// like our FreeRDP client's overlay: frames per second, key to screen, the
// host's time per frame (the RGBA to BGRA swizzle, the GDI blit), the
// process's CPU and memory, the grid, the OpenGL driver loaded, the build.
// The numbers to compare when the frame path changes (a BGRA readback,
// then zero-copy presentation).

// The OpenGL driver loaded in this process: which GPU really renders (on a
// laptop with two GPUs the display adapter can be the other one).
static const char* gl_driver(void) {
  static const struct { const WCHAR* dll; const char* name; } known[] = {
      {L"nvoglv64.dll", "NVIDIA (nvoglv64)"},
      {L"atio6axx.dll", "AMD (atio6axx)"},
      {L"amdxc64.dll", "AMD (amdxc64)"},
      {L"ig9icd64.dll", "Intel (ig9icd64)"},
      {L"ig11icd64.dll", "Intel (ig11icd64)"},
      {L"ig12icd64.dll", "Intel (ig12icd64)"},
      {L"igxelpicd64.dll", "Intel (igxelpicd64)"},
      {L"opengl32sw.dll", "software (opengl32sw)"},
  };
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    if (GetModuleHandleW(known[i].dll)) return known[i].name;
  return "other (Mesa or Microsoft's)";
}

static void stats_tick(Frame* f) {
  uint64_t now = qpc();
  double dt_ms = (double)(now - f->stats_qpc) * g_qpc_ms;
  FILETIME c, e, k, u;
  ULONGLONG cpu = 0;
  if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
    cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
          (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
  if (f->stats_qpc && dt_ms > 0) {
    // CPU in percent of one core (100 ns units over milliseconds).
    f->cpu_pct = (double)(cpu - f->stats_cpu) / 1e4 / dt_ms * 100.0;
    Window* w = f->active >= 0 ? f->tabs[f->active] : NULL;
    if (w) {
      f->fps = w->tick_frames * 1000.0 / dt_ms;
      f->swizzle_ms = w->tick_frames ? w->tick_swizzle_ms / w->tick_frames : 0;
      f->blit_ms = w->tick_frames ? w->tick_blit_ms / w->tick_frames : 0;
    }
  }
  for (int i = 0; i < f->ntabs; i++) {
    f->tabs[i]->tick_frames = 0;
    f->tabs[i]->tick_swizzle_ms = f->tabs[i]->tick_blit_ms = 0;
  }
  PROCESS_MEMORY_COUNTERS_EX m = {sizeof(m)};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m))) {
    f->ws_mb = m.WorkingSetSize / 1048576.0;
    f->private_mb = m.PrivateUsage / 1048576.0;
  }
  f->stats_qpc = now;
  f->stats_cpu = cpu;
  if (f->active >= 0) InvalidateRect(f->tabs[f->active]->hwnd, NULL, FALSE);
}

static void set_stats(Frame* f, bool on) {
  f->stats = on;
  if (on) {
    f->stats_qpc = 0;
    stats_tick(f);  // the baseline
    SetTimer(f->hwnd, 7, 500, NULL);
  } else {
    KillTimer(f->hwnd, 7);
  }
  if (f->active >= 0) InvalidateRect(f->tabs[f->active]->hwnd, NULL, FALSE);
}

static void draw_stats(Window* w, HDC dc, RECT rc) {
  Frame* f = w->frame;
  UINT dpi = (UINT)(w->scale * 96 + 0.5);
  if (!f->stats_font || f->stats_font_dpi != dpi) {
    if (f->stats_font) DeleteObject(f->stats_font);
    f->stats_font = CreateFontW(-MulDiv(9, (int)dpi, 72), 0, 0, 0, FW_NORMAL,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    f->stats_font_dpi = dpi;
  }
  ghostty_surface_size_s sz = ghostty_surface_size(w->surface);
  // The build: the first line of /buildinfo (the release, or the source).
  char build[80];
  const char* b = GHOSTTY_BUILDINFO;
  size_t bl = strcspn(b, "\n");
  if (bl >= sizeof(build)) bl = sizeof(build) - 1;
  memcpy(build, b, bl);
  build[bl] = 0;
  char text[1024];
  snprintf(text, sizeof(text),
           "Ghostty stats (Ctrl+Shift+I)\n"
           "frames   %5.1f /s   %ux%u px\n"
           "key      %5.1f ms   key to its first frame on screen\n"
           "swizzle  %5.2f ms   RGBA to BGRA, per frame\n"
           "blit     %5.2f ms   GDI SetDIBits, per frame\n"
           "cpu      %5.1f %%    of one core (the whole process)\n"
           "memory   %5.0f MB   working set, %.0f MB private\n"
           "grid     %ux%u cells of %ux%u px, %d tab%s\n"
           "opengl   4.3 core, WGL, frames through the CPU\n"
           "driver   %s\n"
           "%s",
           f->fps, w->frame_w, w->frame_h, w->latency_ms, f->swizzle_ms,
           f->blit_ms, f->cpu_pct, f->ws_mb, f->private_mb, sz.columns,
           sz.rows, sz.cell_width_px, sz.cell_height_px, f->ntabs,
           f->ntabs == 1 ? "" : "s", gl_driver(), build);
  WCHAR* wt = utf8_to_utf16(text, -1);
  if (!wt) return;
  HGDIOBJ old = SelectObject(dc, f->stats_font);
  RECT t = {0, 0, 0, 0};
  DrawTextW(dc, wt, -1, &t, DT_CALCRECT | DT_NOPREFIX);
  int pad = (int)(8 * w->scale + 0.5);
  RECT box = {rc.right - (t.right + 2 * pad) - pad, pad, rc.right - pad,
              pad + t.bottom + 2 * pad};
  SetDCBrushColor(dc, RGB(16, 16, 20));
  FillRect(dc, &box, (HBRUSH)GetStockObject(DC_BRUSH));
  SetDCBrushColor(dc, RGB(90, 90, 100));
  FrameRect(dc, &box, (HBRUSH)GetStockObject(DC_BRUSH));
  RECT tr = {box.left + pad, box.top + pad, box.right - pad, box.bottom - pad};
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(220, 220, 225));
  DrawTextW(dc, wt, -1, &tr, DT_NOPREFIX);
  SelectObject(dc, old);
  free(wt);
}

// ---------------------------------------------------------------------------
// Windows

static double window_scale(HWND hwnd) {
  UINT dpi = GetDpiForWindow(hwnd);
  return dpi ? dpi / 96.0 : 1.0;
}

static Frame* new_frame(void) {
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

static void show_frame(Frame* f) {
  ShowWindow(f->hwnd, f->next ? SW_SHOWNORMAL : SW_SHOWDEFAULT);
  if (config_fullscreen()) set_fullscreen(f, true);
  SetForegroundWindow(f->hwnd);
  if (f->active >= 0) SetFocus(f->tabs[f->active]->hwnd);
}

static bool new_window(ghostty_surface_t from) {
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
static Frame* front_frame(void) {
  HWND fg = GetForegroundWindow();
  for (Frame* f = g_frames; f; f = f->next)
    if (f->hwnd == fg) return f;
  return g_frames;
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
  Frame* f = w ? w->frame : NULL;
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
        free(w->title);
        w->title = t;
        if (f->active >= 0 && f->tabs[f->active] == w) update_title(f);
        invalidate_bar(f);
      }
      return true;
    }

    case GHOSTTY_ACTION_NEW_TAB:
      if (!f) f = front_frame();
      if (!f) return new_window(NULL);
      return new_tab(f, w ? w->surface : NULL, GHOSTTY_SURFACE_CONTEXT_TAB) != NULL;

    case GHOSTTY_ACTION_NEW_WINDOW:
      return new_window(w ? w->surface : NULL);

    case GHOSTTY_ACTION_GOTO_TAB: {
      // With one tab there is nowhere to go: not performed, so the key
      // reaches the terminal (the bindings are performable).
      if (!f || f->ntabs < 2) return false;
      int n = f->ntabs, cur = tab_index(f, w), to;
      int v = (int)action.action.goto_tab;
      if (v == GHOSTTY_GOTO_TAB_PREVIOUS)
        to = (cur - 1 + n) % n;
      else if (v == GHOSTTY_GOTO_TAB_NEXT)
        to = (cur + 1) % n;
      else if (v == GHOSTTY_GOTO_TAB_LAST)
        to = n - 1;
      else if (v >= 1)
        to = v - 1 < n - 1 ? v - 1 : n - 1;  // 1-based; past the end: last
      else
        return false;
      select_tab(f, to);
      return true;
    }

    case GHOSTTY_ACTION_MOVE_TAB:
      if (!f || f->ntabs < 2) return false;
      move_tab(f, (int)action.action.move_tab.amount);
      return true;

    case GHOSTTY_ACTION_CLOSE_TAB: {
      if (!f) return false;
      int me = tab_index(f, w);
      // Collect first: closing changes the list (the close is posted, but
      // a surface may close at once).
      Window* list[256];
      int n = 0;
      for (int i = 0; i < f->ntabs && n < 256; i++) {
        bool pick = false;
        switch (action.action.close_tab_mode) {
          case GHOSTTY_ACTION_CLOSE_TAB_MODE_THIS: pick = i == me; break;
          case GHOSTTY_ACTION_CLOSE_TAB_MODE_OTHER: pick = i != me; break;
          case GHOSTTY_ACTION_CLOSE_TAB_MODE_RIGHT: pick = i > me; break;
        }
        if (pick) list[n++] = f->tabs[i];
      }
      for (int i = 0; i < n; i++) close_tab(list[i]);
      return true;
    }

    case GHOSTTY_ACTION_MOVE_TAB_TO_NEW_WINDOW: {
      if (!f || f->ntabs < 2) return false;
      Frame* nf = new_frame();
      if (!nf) return false;
      detach_tab(f, w);
      SetParent(w->hwnd, nf->hwnd);
      if (!insert_tab(nf, w, 0)) {
        // Out of memory: put it back where it was.
        SetParent(w->hwnd, f->hwnd);
        insert_tab(f, w, f->ntabs);
        layout(f);
        DestroyWindow(nf->hwnd);
        return false;
      }
      layout(nf);
      size_tab(w);
      select_tab(nf, 0);
      show_frame(nf);
      return true;
    }

    case GHOSTTY_ACTION_INSPECTOR:
      // Ghostty's inspector key shows the stats overlay for now.
      if (!f) return false;
      switch (action.action.inspector) {
        case GHOSTTY_INSPECTOR_TOGGLE: set_stats(f, !f->stats); break;
        case GHOSTTY_INSPECTOR_SHOW: set_stats(f, true); break;
        case GHOSTTY_INSPECTOR_HIDE: set_stats(f, false); break;
      }
      return true;

    case GHOSTTY_ACTION_TOGGLE_FULLSCREEN:
      if (!f) return false;
      set_fullscreen(f, !f->fullscreen);
      return true;

    case GHOSTTY_ACTION_RING_BELL:
      if (f) FlashWindow(f->hwnd, TRUE);
      MessageBeep(MB_OK);
      return true;

    case GHOSTTY_ACTION_CLOSE_WINDOW:
      if (!f) return false;
      PostMessageW(f->hwnd, WM_CLOSE, 0, 0);
      return true;

    case GHOSTTY_ACTION_CLOSE_ALL_WINDOWS:
      for (Frame* x = g_frames; x; x = x->next)
        PostMessageW(x->hwnd, WM_CLOSE, 0, 0);
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
// Window procedures

// A tab: its surface's input and frames.
static LRESULT CALLBACK terminal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Window* w = (Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!w || !w->surface) return DefWindowProcW(hwnd, msg, wp, lp);

  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // Let Windows have Alt+F4 and Alt+Space (the window menu); for a
      // child window DefWindowProc hands them to the top-level window.
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
      if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp))
        ghostty_surface_set_size(w->surface, LOWORD(lp), HIWORD(lp));
      return 0;
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

    case WM_GHOSTTY_DESTROY:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY: {
      Frame* f = w->frame;
      ghostty_surface_free(w->surface);
      w->surface = NULL;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      detach_tab(f, w);
      free(w->title);
      free(w->bgra);
      free(w);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
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
static LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_TIMER:
      if (wp == 7 && f->stats) stats_tick(f);
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

int main(int argc, char** argv) {
  int rc = 0;
  if (report_mode(&rc)) return rc;
  if (buildinfo_mode(argc, argv)) return 0;
  stderr_guard_start();
  g_main_tid = GetCurrentThreadId();
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  g_qpc_ms = 1000.0 / (double)freq.QuadPart;

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
  read_host_config();

  g_inst = GetModuleHandleW(NULL);
  WNDCLASSEXW fc = {sizeof(fc)};
  fc.style = CS_HREDRAW;
  fc.lpfnWndProc = frame_proc;
  fc.hInstance = g_inst;
  fc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  fc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
  fc.lpszClassName = kFrameClass;
  RegisterClassExW(&fc);

  WNDCLASSEXW wc = {sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = terminal_proc;
  wc.hInstance = g_inst;
  wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
  wc.lpszClassName = kTerminalClass;
  RegisterClassExW(&wc);

  WNDCLASSEXW wk = {sizeof(wk)};
  wk.lpfnWndProc = wakeup_proc;
  wk.hInstance = g_inst;
  wk.lpszClassName = kWakeupClass;
  RegisterClassExW(&wk);
  g_wakeup_hwnd = CreateWindowExW(0, kWakeupClass, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, NULL, g_inst, NULL);

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

  start_reports();
  if (!new_window(NULL)) return 1;

  // To prove the reports, 3 seconds after start: GHOSTTY_TEST_HANG=<seconds>
  // blocks the main thread once; GHOSTTY_TEST_CRASH=av|panic|abort crashes
  // it (a null write; the exception libghostty raises for a Zig panic;
  // abort()). GHOSTTY_TEST_STDERR=<KB> writes that much log on stderr from
  // the main thread, half through the CRT and half through the handle (as
  // Zig's std.log does), to prove a stderr nobody reads never blocks it.
  const char* test_hang = getenv("GHOSTTY_TEST_HANG");
  const char* test_crash = getenv("GHOSTTY_TEST_CRASH");
  const char* test_stderr = getenv("GHOSTTY_TEST_STDERR");
  if (test_hang || test_crash || test_stderr) SetTimer(NULL, 0, 3000, NULL);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (msg.message == WM_TIMER && msg.hwnd == NULL &&
        (test_hang || test_crash || test_stderr)) {
      KillTimer(NULL, msg.wParam);
      if (test_stderr) {
        char line[128];
        int kb = atoi(test_stderr);
        for (int i = 0; i < kb * 1024 / 64; i++) {
          int n = snprintf(line, sizeof(line),
                           "ghostty: stderr test %s %07d%*s\n",
                           i % 2 ? "crt " : "std ", i, 30, "");
          if (i % 2) {
            fwrite(line, 1, (size_t)n, stderr);
          } else {
            DWORD w;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)n, &w, NULL);
          }
        }
        fprintf(stderr, "ghostty: stderr test done\n");
      }
      if (test_hang) Sleep((DWORD)atoi(test_hang) * 1000);
      if (test_crash && strcmp(test_crash, "av") == 0) *(volatile int*)8 = 1;
      if (test_crash && strcmp(test_crash, "abort") == 0) abort();
      if (test_crash && strcmp(test_crash, "panic") == 0) {
        static const char m[] = "GHOSTTY_TEST_CRASH=panic";
        ULONG_PTR args[2] = {(ULONG_PTR)m, sizeof(m) - 1};
        RaiseException(GHOSTTY_PANIC_CODE, EXCEPTION_NONCONTINUABLE, 2, args);
      }
      test_hang = test_crash = test_stderr = NULL;
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  InterlockedExchange(&g_loop_done, 1);

  // Quit (not the last window closing): free the surfaces still open
  // before the app that owns them.
  while (g_frames) DestroyWindow(g_frames->hwnd);

  ghostty_app_free(g_app);
  ghostty_config_free(g_config);
  stderr_guard_flush(500);
  return 0;
}
