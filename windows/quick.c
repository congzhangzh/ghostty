#include "host.h"

// ---------------------------------------------------------------------------
// The quick terminal
//
// A terminal that drops down from an edge of the screen on a key, from any
// app, and goes back up on the same key (or when it loses focus, with
// quick-terminal-autohide), as Ghostty's on macOS. It is a frame of its
// own: no caption (it takes the full-screen path in frame.c, so the bar is
// a plain tab bar), no taskbar button (WS_EX_TOOLWINDOW), on top; it can
// hold tabs and splits; closing its last tab ends it, and the next toggle
// makes a new one.
//
// The key is Ghostty's: a `global:` keybind for toggle_quick_terminal
// (e.g. `keybind = global:super+grave_accent=toggle_quick_terminal`),
// registered with Windows (RegisterHotKey) on the wakeup window, again on
// each config change. Not a keyboard hook: it sees only its own key. A
// binding without `global:` toggles it from inside Ghostty only.
//
// Config read: quick-terminal-position (top, bottom, left, right, center),
// quick-terminal-size (as macOS: 400 points on the primary axis, the full
// screen on the other, unless given), quick-terminal-screen (main: the
// primary monitor; mouse: the monitor under the pointer),
// quick-terminal-animation-duration (a slide from its edge; 0: none) and
// quick-terminal-autohide.

#define QUICK_HOTKEY 0x51  // the hotkey's id on the wakeup window
#define QUICK_TIMER 22     // the slide's steps, on the quick frame

static Frame* g_quick;
static bool g_hotkey;

// ---- config --------------------------------------------------------------------

static const char* config_enum(const char* key, const char* fallback) {
  const char* v = NULL;
  if (g_config && ghostty_config_get(g_config, &v, key, strlen(key)) && v) return v;
  return fallback;
}

static double config_double(const char* key, double fallback) {
  double v = fallback;
  if (g_config) ghostty_config_get(g_config, &v, key, strlen(key));
  return v;
}

static bool config_bool(const char* key, bool fallback) {
  bool v = fallback;
  if (g_config) ghostty_config_get(g_config, &v, key, strlen(key));
  return v;
}

// One size of quick-terminal-size in pixels: a percentage of `whole`, or
// points times the monitor's scale; `dflt` (points, or -1 for the whole)
// when not given.
static int size_px(ghostty_quick_terminal_size_s s, int whole, double scale, int dflt) {
  if (s.tag == GHOSTTY_QUICK_TERMINAL_SIZE_PERCENTAGE) return (int)(whole * s.value.percentage / 100.0);
  if (s.tag == GHOSTTY_QUICK_TERMINAL_SIZE_PIXELS) return (int)(s.value.pixels * scale);
  return dflt < 0 ? whole : (int)(dflt * scale);
}

// Where the quick terminal is when shown (`shown`) and where it slides
// from (`hidden`, just past its edge; the same for center).
static void quick_rects(RECT* shown, RECT* hidden) {
  POINT pt = {0, 0};
  if (strcmp(config_enum("quick-terminal-screen", "main"), "mouse") == 0) GetCursorPos(&pt);
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(mon, &mi);
  RECT wa = mi.rcWork;  // not over the taskbar
  UINT dpi = 96, dpiy = 96;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiy);
  double scale = dpi / 96.0;
  int W = wa.right - wa.left, H = wa.bottom - wa.top;
  ghostty_config_quick_terminal_size_s sz = {0};
  if (g_config) ghostty_config_get(g_config, &sz, "quick-terminal-size", strlen("quick-terminal-size"));
  const char* pos = config_enum("quick-terminal-position", "top");
  int w, h;
  if (strcmp(pos, "left") == 0 || strcmp(pos, "right") == 0) {
    w = size_px(sz.primary, W, scale, 400);
    h = size_px(sz.secondary, H, scale, -1);
  } else if (strcmp(pos, "center") == 0) {
    bool landscape = W >= H;
    w = landscape ? size_px(sz.primary, W, scale, 800) : size_px(sz.secondary, W, scale, 400);
    h = landscape ? size_px(sz.secondary, H, scale, 400) : size_px(sz.primary, H, scale, 800);
  } else {
    w = size_px(sz.secondary, W, scale, -1);
    h = size_px(sz.primary, H, scale, 400);
  }
  if (w > W) w = W;
  if (h > H) h = H;
  int x = wa.left + (W - w) / 2, y = wa.top + (H - h) / 2;  // center
  if (strcmp(pos, "top") == 0) y = wa.top;
  if (strcmp(pos, "bottom") == 0) y = wa.bottom - h;
  if (strcmp(pos, "left") == 0) x = wa.left;
  if (strcmp(pos, "right") == 0) x = wa.right - w;
  SetRect(shown, x, y, x + w, y + h);
  *hidden = *shown;
  if (strcmp(pos, "top") == 0) OffsetRect(hidden, 0, -(y - mi.rcMonitor.top) - h);
  if (strcmp(pos, "bottom") == 0) OffsetRect(hidden, 0, mi.rcMonitor.bottom - y);
  if (strcmp(pos, "left") == 0) OffsetRect(hidden, -(x - mi.rcMonitor.left) - w, 0);
  if (strcmp(pos, "right") == 0) OffsetRect(hidden, mi.rcMonitor.right - x, 0);
}

// ---- the slide ------------------------------------------------------------------

static RECT g_from, g_to;
static uint64_t g_anim_start;
static double g_anim_secs;
static bool g_anim_hiding;

static void quick_event(const char* what) {
  char e[96];
  snprintf(e, sizeof(e), "{\"kind\":\"quick\",\"what\":\"%s\"}", what);
  test_event(e);
}

static void slide_to(Frame* f, double t) {  // t in 0..1, eased
  double k = 1 - (1 - t) * (1 - t) * (1 - t);  // ease-out cubic
  int x = g_from.left + (int)((g_to.left - g_from.left) * k);
  int y = g_from.top + (int)((g_to.top - g_from.top) * k);
  SetWindowPos(f->hwnd, HWND_TOPMOST, x, y, g_to.right - g_to.left, g_to.bottom - g_to.top,
               SWP_NOACTIVATE);
}

static void slide_done(Frame* f) {
  KillTimer(f->hwnd, QUICK_TIMER);
  g_anim_secs = 0;
  if (g_anim_hiding) {
    ShowWindow(f->hwnd, SW_HIDE);
    quick_event("hidden");
  } else {
    slide_to(f, 1);
    quick_event("shown");
  }
}

// WM_TIMER on a frame: a step of the slide.
bool quick_timer(Frame* f, WPARAM id) {
  if (id != QUICK_TIMER || f != g_quick) return false;
  double t = g_anim_secs > 0 ? (double)(GetTickCount64() - g_anim_start) / (g_anim_secs * 1000) : 1;
  if (t >= 1)
    slide_done(f);
  else
    slide_to(f, t);
  return true;
}

static void slide(Frame* f, bool hiding) {
  RECT shown, hidden;
  quick_rects(&shown, &hidden);
  RECT cur;
  GetWindowRect(f->hwnd, &cur);
  bool visible = IsWindowVisible(f->hwnd);
  g_from = hiding ? cur : (visible ? cur : hidden);
  g_to = hiding ? hidden : shown;
  g_anim_hiding = hiding;
  g_anim_secs = config_double("quick-terminal-animation-duration", 0.2);
  bool center = strcmp(config_enum("quick-terminal-position", "top"), "center") == 0;
  if (g_anim_secs <= 0 || center) {  // center: no edge to slide from
    slide_done(f);
    return;
  }
  g_anim_start = GetTickCount64();
  slide_to(f, 0);
  SetTimer(f->hwnd, QUICK_TIMER, 10, NULL);
}

// ---- show and hide ----------------------------------------------------------------

static void quick_show(void) {
  if (!g_quick) {
    Frame* f = new_frame();
    if (!f) return;
    f->quick = true;
    f->fullscreen = true;  // the caption-less path in frame.c
    SetWindowLongW(f->hwnd, GWL_STYLE, WS_POPUP | WS_CLIPCHILDREN);
    SetWindowLongW(f->hwnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (!new_tab(f, NULL, GHOSTTY_SURFACE_CONTEXT_WINDOW)) {
      DestroyWindow(f->hwnd);
      return;
    }
    g_quick = f;
    RECT shown, hidden;
    quick_rects(&shown, &hidden);
    SetWindowPos(f->hwnd, HWND_TOPMOST, hidden.left, hidden.top, shown.right - shown.left,
                 shown.bottom - shown.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    layout(f);
  }
  Frame* f = g_quick;
  ShowWindow(f->hwnd, SW_SHOW);
  SetForegroundWindow(f->hwnd);
  if (f->active >= 0) SetFocus(f->tabs[f->active]->hwnd);
  slide(f, false);
}

static void quick_hide(void) {
  if (g_quick && IsWindowVisible(g_quick->hwnd)) slide(g_quick, true);
}

// toggle_quick_terminal (the global key, or a binding inside Ghostty).
void quick_toggle(void) {
  bool up = g_quick && IsWindowVisible(g_quick->hwnd) && !(g_anim_secs > 0 && g_anim_hiding);
  // In front: away. Shown but behind another window: to the front.
  if (up && GetForegroundWindow() == g_quick->hwnd)
    quick_hide();
  else
    quick_show();
}

// WM_ACTIVATE on the quick frame: with quick-terminal-autohide, away when
// another app's window takes the focus (not one of ours: a confirmation
// box, the theme list).
void quick_activate(Frame* f, WPARAM wp, LPARAM lp) {
  if (f != g_quick || LOWORD(wp) != WA_INACTIVE) return;
  if (!config_bool("quick-terminal-autohide", false)) return;
  DWORD pid = 0;
  if (lp) GetWindowThreadProcessId((HWND)lp, &pid);
  if (lp && pid == GetCurrentProcessId()) return;
  quick_hide();
}

// The quick frame is going (its last tab closed).
void quick_gone(Frame* f) {
  if (f == g_quick) g_quick = NULL;
}

bool frame_is_quick(Frame* f) { return f && f->quick; }

// ---- the global key ----------------------------------------------------------------

// The Windows key for a trigger's key, or 0.
static UINT trigger_vk(ghostty_input_trigger_s t) {
  if (t.tag == GHOSTTY_TRIGGER_UNICODE) {
    SHORT r = VkKeyScanW((WCHAR)t.key.unicode);
    return r == -1 ? 0 : (UINT)(r & 0xff);
  }
  if (t.tag != GHOSTTY_TRIGGER_PHYSICAL) return 0;
  ghostty_input_key_e k = t.key.physical;
  if (k >= GHOSTTY_KEY_A && k <= GHOSTTY_KEY_Z) return 'A' + (k - GHOSTTY_KEY_A);
  if (k >= GHOSTTY_KEY_DIGIT_0 && k <= GHOSTTY_KEY_DIGIT_9) return '0' + (k - GHOSTTY_KEY_DIGIT_0);
  if (k >= GHOSTTY_KEY_F1 && k <= GHOSTTY_KEY_F24) return VK_F1 + (k - GHOSTTY_KEY_F1);
  if (k >= GHOSTTY_KEY_NUMPAD_0 && k <= GHOSTTY_KEY_NUMPAD_9) return VK_NUMPAD0 + (k - GHOSTTY_KEY_NUMPAD_0);
  switch (k) {
    case GHOSTTY_KEY_BACKQUOTE: return VK_OEM_3;
    case GHOSTTY_KEY_MINUS: return VK_OEM_MINUS;
    case GHOSTTY_KEY_EQUAL: return VK_OEM_PLUS;
    case GHOSTTY_KEY_BRACKET_LEFT: return VK_OEM_4;
    case GHOSTTY_KEY_BRACKET_RIGHT: return VK_OEM_6;
    case GHOSTTY_KEY_BACKSLASH: return VK_OEM_5;
    case GHOSTTY_KEY_SEMICOLON: return VK_OEM_1;
    case GHOSTTY_KEY_QUOTE: return VK_OEM_7;
    case GHOSTTY_KEY_COMMA: return VK_OEM_COMMA;
    case GHOSTTY_KEY_PERIOD: return VK_OEM_PERIOD;
    case GHOSTTY_KEY_SLASH: return VK_OEM_2;
    case GHOSTTY_KEY_SPACE: return VK_SPACE;
    case GHOSTTY_KEY_ENTER: return VK_RETURN;
    case GHOSTTY_KEY_TAB: return VK_TAB;
    case GHOSTTY_KEY_ESCAPE: return VK_ESCAPE;
    case GHOSTTY_KEY_BACKSPACE: return VK_BACK;
    case GHOSTTY_KEY_INSERT: return VK_INSERT;
    case GHOSTTY_KEY_DELETE: return VK_DELETE;
    case GHOSTTY_KEY_HOME: return VK_HOME;
    case GHOSTTY_KEY_END: return VK_END;
    case GHOSTTY_KEY_PAGE_UP: return VK_PRIOR;
    case GHOSTTY_KEY_PAGE_DOWN: return VK_NEXT;
    case GHOSTTY_KEY_ARROW_UP: return VK_UP;
    case GHOSTTY_KEY_ARROW_DOWN: return VK_DOWN;
    case GHOSTTY_KEY_ARROW_LEFT: return VK_LEFT;
    case GHOSTTY_KEY_ARROW_RIGHT: return VK_RIGHT;
    case GHOSTTY_KEY_PAUSE: return VK_PAUSE;
    case GHOSTTY_KEY_SCROLL_LOCK: return VK_SCROLL;
    case GHOSTTY_KEY_PRINT_SCREEN: return VK_SNAPSHOT;
    default: return 0;
  }
}

// (Re)register the global key from the config: at start and on each change.
void quick_hotkey_update(void) {
  if (g_hotkey) {
    UnregisterHotKey(g_wakeup_hwnd, QUICK_HOTKEY);
    g_hotkey = false;
  }
  if (!g_app || !g_config || !ghostty_app_has_global_keybinds(g_app)) return;
  static const char action[] = "toggle_quick_terminal";
  ghostty_input_trigger_s t = ghostty_config_trigger(g_config, action, strlen(action));
  UINT vk = trigger_vk(t);
  if (!vk) return;
  UINT mods = MOD_NOREPEAT;
  if (t.mods & GHOSTTY_MODS_SHIFT) mods |= MOD_SHIFT;
  if (t.mods & GHOSTTY_MODS_CTRL) mods |= MOD_CONTROL;
  if (t.mods & GHOSTTY_MODS_ALT) mods |= MOD_ALT;
  if (t.mods & GHOSTTY_MODS_SUPER) mods |= MOD_WIN;
  g_hotkey = RegisterHotKey(g_wakeup_hwnd, QUICK_HOTKEY, mods, vk);
  char e[128];
  snprintf(e, sizeof(e), "{\"kind\":\"quick\",\"what\":\"hotkey\",\"vk\":%u,\"mods\":%u,\"ok\":%s}",
           vk, mods & ~MOD_NOREPEAT, g_hotkey ? "true" : "false");
  test_event(e);
  if (!g_hotkey)
    fprintf(stderr,
            "ghostty: the quick terminal's global key (vk 0x%02x, mods 0x%x) is taken by another "
            "app (error %lu)\n",
            vk, mods & ~MOD_NOREPEAT, GetLastError());
}

// WM_HOTKEY on the wakeup window.
bool quick_hotkey(WPARAM id) {
  if (id != QUICK_HOTKEY) return false;
  quick_toggle();
  return true;
}
