#include "host.h"

// ---------------------------------------------------------------------------
// The cheat sheet: every key at a glance (the owner: "the help overlay want
// to be hacking style cover all pannel"; "no search and keep simple"; "if
// room is not enough, show just key part")
//
// A translucent layer over the whole window (the terminal shows faintly),
// in a monospace font at the terminal's size, keys in the theme's accent
// colour, grouped in columns: Tabs, Splits, Window & full screen,
// Clipboard & links, Help, Themes & config, Quick Terminal (that order is
// also the priority). The slogan at the top right, the build and how to
// close it at the bottom. When the window is too small, the slogan and
// build lines go first, then whole groups from the end: never a cut line,
// never a smaller font, never a scroll bar.
// The "?" button and Ctrl+Shift+? open it; Esc, ?, a click (or another
// window) close it. Ctrl+Shift+P stays the command palette.
//
// The keys are the config's own bindings (ghostty_config_trigger, so a
// rebound key shows as rebound; an action with no key is left out), the
// labels the palette's titles in the display language (i18n.c).

typedef struct {
  const char* action;  // its key from the config, if it has one
  const WCHAR* key;    // the key of an entry with no action (the host's own); else kDefaultKeys
  const WCHAR* label;  // English (tr)
} SheetEntry;

typedef struct {
  const WCHAR* name;
  SheetEntry e[12];
} SheetGroup;

// The config's lookup (its reverse map) leaves out "performable" keys
// (Binding.zig): the default copy/paste (Ctrl+Shift+C/V; the lookup gives
// Ctrl+Insert) and the split focus keys (Ctrl+Alt+arrows, [ and ]) are
// written here. Unbound on Windows, left out: Close Terminal (Ctrl+Shift+W
// is Close Tab), Equalize Splits (a macOS key only).
//
// The keys of actions the lookup can't give, for the sheet AND the command
// palette (palette.c), one table so they can't drift: performable ones (the
// split focus keys) and keys the config doesn't hold as a plain trigger
// (F11; the quick terminal's global Win+`).
static const struct {
  const char* action;
  const WCHAR* key;
} kDefaultKeys[] = {
    {"goto_split:left", L"Ctrl+Alt+Left"},   {"goto_split:right", L"Ctrl+Alt+Right"},
    {"goto_split:up", L"Ctrl+Alt+Up"},       {"goto_split:down", L"Ctrl+Alt+Down"},
    {"goto_split:previous", L"Ctrl+Alt+["},  {"goto_split:next", L"Ctrl+Alt+]"},
    {"toggle_fullscreen", L"F11"},           {"toggle_quick_terminal", L"Win+`"},
};

// That table's key for an action, or NULL.
const WCHAR* default_key(const char* action) {
  if (!action) return NULL;
  for (size_t i = 0; i < sizeof(kDefaultKeys) / sizeof(kDefaultKeys[0]); i++)
    if (strcmp(kDefaultKeys[i].action, action) == 0) return kDefaultKeys[i].key;
  return NULL;
}

static const SheetGroup kGroups[] = {
    {L"Tabs",
     {{"new_tab", NULL, L"New Tab"},
      {"next_tab", NULL, L"Next Tab"},
      {"previous_tab", NULL, L"Previous Tab"},
      {"goto_tab:1", NULL, L"Go to tab 1…9"},
      {"close_tab", NULL, L"Close Tab"}}},
    {L"Splits",
     {{"new_split:right", NULL, L"Split Right"},
      {"new_split:down", NULL, L"Split Down"},
      {"goto_split:left", NULL, L"Focus Split: Left"},
      {"goto_split:right", NULL, L"Focus Split: Right"},
      {"goto_split:up", NULL, L"Focus Split: Up"},
      {"goto_split:down", NULL, L"Focus Split: Down"},
      {"goto_split:previous", NULL, L"Focus Split: Previous"},
      {"goto_split:next", NULL, L"Focus Split: Next"},
      {"toggle_split_zoom", NULL, L"Toggle Split Zoom"},
      {"equalize_splits", NULL, L"Equalize Splits"},
      {"close_surface", NULL, L"Close Terminal"}}},
    {L"Window & full screen",
     {{"new_window", NULL, L"New Window"},
      {"toggle_fullscreen", NULL, L"Toggle Fullscreen"},
      {NULL, L"Alt+Space", L"Window menu"},
      {"close_window", NULL, L"Close Window"}}},
    {L"Clipboard & links",
     {{NULL, L"Ctrl+Shift+C", L"Copy to Clipboard"},
      {NULL, L"Ctrl+Shift+V", L"Paste from Clipboard"},
      {"select_all", NULL, L"Select All"},
      {NULL, L"Ctrl+click", L"Open a link"},
      {NULL, L"Drag & drop", L"Type a file's path"}}},
    {L"Help",
     {{NULL, L"Ctrl+Shift+? / ?", L"This sheet"},
      {"toggle_command_palette", NULL, L"Command Palette"},
      {"inspector:toggle", NULL, L"Stats overlay"}}},
    {L"Themes & config",
     {{"increase_font_size:1", NULL, L"Increase Font Size"},
      {"decrease_font_size:1", NULL, L"Decrease Font Size"},
      {"reset_font_size", NULL, L"Reset Font Size"},
      {"open_config", NULL, L"Open Config"},
      {"reload_config", NULL, L"Reload Config"},
      {NULL, L"Ctrl+Shift+F12", L"Light / Dark / Auto"}}},
    // A global key: not in the config's lookup.
    {L"Quick Terminal", {{"toggle_quick_terminal", NULL, L"Show or hide"}}},
};
#define NGROUPS ((int)(sizeof(kGroups) / sizeof(kGroups[0])))

static HWND g_sheet;
static Frame* g_sheet_frame;
static int g_sheet_pane;
static HFONT g_sheet_font, g_sheet_bold;

// One entry's key and label; false: no key for it (left out).
static bool entry_text(const SheetEntry* e, WCHAR* key, WCHAR* label, size_t cap) {
  key[0] = 0;
  if (e->action && g_config) {
    ghostty_input_trigger_s t = ghostty_config_trigger(g_config, e->action, strlen(e->action));
    trigger_text(t, key, cap);
  }
  if (!key[0] && (e->key || default_key(e->action)))
    swprintf(key, cap, L"%ls", e->key ? e->key : default_key(e->action));
  if (!key[0]) return false;  // unbound: left out
  swprintf(label, cap, L"%ls", tr(e->label));
  return true;
}

static COLORREF accent(void) {
  ghostty_config_palette_s p;
  static const char k[] = "palette";
  if (g_config && ghostty_config_get(g_config, &p, k, strlen(k))) {
    ghostty_config_color_s c = p.colors[6];  // cyan: a terminal's accent
    return RGB(c.r, c.g, c.b);
  }
  return RGB(0x56, 0xb6, 0xc2);
}

static void fonts(Frame* f) {
  if (g_sheet_font) DeleteObject(g_sheet_font);
  if (g_sheet_bold) DeleteObject(g_sheet_bold);
  float pt = 13;
  static const char k[] = "font-size";
  if (g_config) ghostty_config_get(g_config, &pt, k, strlen(k));
  int h = -(int)(pt * f->scale * 96 / 72 + 0.5);
  g_sheet_font = CreateFontW(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
  g_sheet_bold = CreateFontW(h, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
}

// The layout (draw = false: only measure): how many groups fit, the
// slogan/build lines on or off. Returns the number of groups shown.
static int layout_sheet(HDC dc, RECT rc, bool lines, bool draw) {
  TEXTMETRICW tm;
  SelectObject(dc, g_sheet_font);
  GetTextMetricsW(dc, &tm);
  int lh = tm.tmHeight + tm.tmExternalLeading, cw = tm.tmAveCharWidth;
  int m = 2 * cw, gap = 3 * cw;
  // The widest key and label, over every group: one column width for all.
  int kw = 0, lw = 0;
  WCHAR key[96], label[160];
  for (int g = 0; g < NGROUPS; g++)
    for (int i = 0; i < 12; i++) {
      if (!(kGroups[g].e[i].action || kGroups[g].e[i].key)) continue;
      if (!entry_text(&kGroups[g].e[i], key, label, 96)) continue;
      SIZE s;
      GetTextExtentPoint32W(dc, key, (int)wcslen(key), &s);
      if (s.cx > kw) kw = s.cx;
      GetTextExtentPoint32W(dc, label, (int)wcslen(label), &s);
      if (s.cx > lw) lw = s.cx;
    }
  SIZE arrow;
  GetTextExtentPoint32W(dc, L"  →  ", 5, &arrow);
  int colw = kw + arrow.cx + lw;
  int top = rc.top + m + (lines ? 2 * lh : 0), bottom = rc.bottom - m - (lines ? 2 * lh : 0);
  int x = rc.left + m, y = top, shown = 0;
  COLORREF acc = accent();
  for (int g = 0; g < NGROUPS; g++) {
    int n = 0;
    for (int i = 0; i < 12; i++)
      if ((kGroups[g].e[i].action || kGroups[g].e[i].key) && entry_text(&kGroups[g].e[i], key, label, 96)) n++;
    if (!n) continue;
    int need = (n + 1) * lh + lh / 2;  // the name, the lines, a gap
    if (y + need > bottom && y > top) {  // the next column
      x += colw + gap;
      y = top;
    }
    if (y + need > bottom || x + colw > rc.right - m) break;  // no room: this group and the rest go
    if (draw) {
      SetTextColor(dc, acc);
      SelectObject(dc, g_sheet_bold);
      TextOutW(dc, x, y, tr(kGroups[g].name), (int)wcslen(tr(kGroups[g].name)));
      SelectObject(dc, g_sheet_font);
    }
    int yy = y + lh;
    for (int i = 0; i < 12; i++) {
      if (!(kGroups[g].e[i].action || kGroups[g].e[i].key)) continue;
      if (!entry_text(&kGroups[g].e[i], key, label, 96)) continue;
      if (draw) {
        SetTextColor(dc, acc);
        TextOutW(dc, x, yy, key, (int)wcslen(key));
        SetTextColor(dc, g_fg);
        TextOutW(dc, x + kw, yy, L"  →  ", 5);
        TextOutW(dc, x + kw + arrow.cx, yy, label, (int)wcslen(label));
      }
      yy += lh;
    }
    y += need;
    shown++;
  }
  if (draw && lines) {
    SetTextColor(dc, RGB((GetRValue(g_fg) + GetRValue(g_bg)) / 2, (GetGValue(g_fg) + GetGValue(g_bg)) / 2,
                         (GetBValue(g_fg) + GetBValue(g_bg)) / 2));
    SIZE s;
    GetTextExtentPoint32W(dc, kSlogan, (int)wcslen(kSlogan), &s);
    const WCHAR* top_text = s.cx <= rc.right - 2 * m ? kSlogan : kMotto;
    GetTextExtentPoint32W(dc, top_text, (int)wcslen(top_text), &s);
    TextOutW(dc, rc.right - m - s.cx, rc.top + m, top_text, (int)wcslen(top_text));
    WCHAR foot[256];
    swprintf(foot, 256, L"Ghostty for Windows %ls    %ls", WIDEN(GHOSTTY_BUILD_TAG), tr(L"Esc, ? or a click: close"));
    TextOutW(dc, rc.left + m, rc.bottom - m - lh, foot, (int)wcslen(foot));
  }
  return shown;
}

static int g_shown, g_lines;

static LRESULT CALLBACK sheet_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      // A dark backdrop of the theme's own background.
      COLORREF bg = RGB(GetRValue(g_bg) / 3, GetGValue(g_bg) / 3, GetBValue(g_bg) / 3);
      HBRUSH b = CreateSolidBrush(bg);
      FillRect(mem, &rc, b);
      DeleteObject(b);
      SetBkMode(mem, TRANSPARENT);
      // With the slogan and build lines if every group fits with them; else
      // without (more room); the groups that still do not fit go.
      int with = layout_sheet(mem, rc, true, false), without = layout_sheet(mem, rc, false, false);
      g_lines = with >= without;
      g_shown = layout_sheet(mem, rc, g_lines, true);
      BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) {
        sheet_toggle(NULL);
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'?') sheet_toggle(NULL);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      sheet_toggle(NULL);
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE && g_sheet == h) PostMessageW(h, WM_CLOSE, 0, 0);
      return 0;
    case WM_CLOSE:
      sheet_toggle(NULL);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// The "?" button, Ctrl+Shift+?: open over w's window, or close (w ignored).
void sheet_toggle(Window* w) {
  if (g_sheet) {
    HWND h = g_sheet;
    int pane = g_sheet_pane;
    g_sheet = NULL;
    DestroyWindow(h);
    test_event("{\"kind\":\"sheet\",\"what\":\"closed\"}");
    // The focus back to the terminal it was opened over.
    for (Frame* f = g_frames; f; f = f->next)
      for (int i = 0; i < f->ntabs; i++)
        if (f->tabs[i]->id == pane) SetFocus(f->tabs[i]->hwnd);
    return;
  }
  if (!w) return;
  Frame* f = w->frame;
  static bool registered;
  if (!registered) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = sheet_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"GhosttySheet";
    registered = RegisterClassW(&wc) != 0;
  }
  fonts(f);
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  POINT a = {0, 0};
  ClientToScreen(f->hwnd, &a);
  g_sheet_frame = f;
  g_sheet_pane = w->id;
  g_sheet = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"GhosttySheet", L"", WS_POPUP, a.x, a.y,
                            rc.right, rc.bottom, f->hwnd, NULL, g_inst, NULL);
  if (!g_sheet) return;
  SetLayeredWindowAttributes(g_sheet, 0, 232, LWA_ALPHA);  // the terminal shows faintly
  ShowWindow(g_sheet, SW_SHOW);
  UpdateWindow(g_sheet);
  SetForegroundWindow(g_sheet);
  SetFocus(g_sheet);
  char ev[160];
  snprintf(ev, sizeof(ev), "{\"kind\":\"sheet\",\"what\":\"opened\",\"groups\":%d,\"of\":%d,\"lines\":%s}", g_shown,
           NGROUPS, g_lines ? "true" : "false");
  test_event(ev);
}
