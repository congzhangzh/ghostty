#include "host.h"

// ---------------------------------------------------------------------------
// The command palette (toggle_command_palette), as the macOS app's
// (macos/Sources/Features/Command Palette): a search box over a list of
// what Ghostty can do, each command with its key on the right (the owner:
// "ctrl shift + ? to show keyboard overview"), and the open terminals to
// jump to. Enter runs the selected one on the pane it was opened from,
// Esc or a click elsewhere closes it. Ctrl+Shift+P (upstream's) and, on
// Windows, Ctrl+Shift+/ (Config.zig) open it.
//
// The commands are the config's command-palette-entry list, minus those
// this host does not do yet (as macOS leaves out its own unsupported ones).

#define PAL_ROW_H 40   // points: the title, the description under it
#define PAL_HEAD_H 40  // points: the header, the slogan and the build line (as pys's help)

typedef struct {
  WCHAR* title;
  WCHAR* desc;
  WCHAR key[48];
  char* action;  // NULL: a jump
  int pane;      // a jump's pane id
} PalItem;

static HWND g_pal, g_pal_edit, g_pal_list;
static Frame* g_pal_frame;
static int g_pal_pane;  // the pane it was opened from (id)
static PalItem* g_items;
static int g_nitems;
static WNDPROC g_pal_edit_proc;
static HFONT g_pal_font, g_pal_small;

// Actions that need this host and that it does not do yet (beside macOS's
// own list): the palette leaves them out.
static bool supported(const char* key) {
  static const char* const no[] = {
      "toggle_tab_overview", "toggle_window_decorations", "prompt_window_title", "show_gtk_inspector",
      "prompt_surface_title", "prompt_tab_title", "check_for_updates", "goto_window",
      "reset_window_size", "show_on_screen_keyboard", "toggle_background_opacity", "toggle_maximize",
      "toggle_secure_input", "toggle_window_float_on_top", "undo", "redo", "start_search",
      "end_search", "navigate_search", "search_selection", "copy_title_to_clipboard", "open",
      "set_window_theme",
  };
  for (size_t i = 0; i < sizeof(no) / sizeof(no[0]); i++)
    if (strcmp(key, no[i]) == 0) return false;
  return true;
}

static void items_free(void) {
  for (int i = 0; i < g_nitems; i++) {
    free(g_items[i].title);
    free(g_items[i].desc);
    free(g_items[i].action);
  }
  free(g_items);
  g_items = NULL;
  g_nitems = 0;
}

static void items_add(const WCHAR* title, const WCHAR* desc, const char* action, int pane) {
  PalItem* p = realloc(g_items, (size_t)(g_nitems + 1) * sizeof(PalItem));
  if (!p) return;
  g_items = p;
  PalItem* it = &g_items[g_nitems++];
  memset(it, 0, sizeof(*it));
  it->title = _wcsdup(title ? title : L"");
  it->desc = _wcsdup(desc ? desc : L"");
  it->action = action ? _strdup(action) : NULL;
  it->pane = pane;
  if (action && g_config) {
    ghostty_input_trigger_s t = ghostty_config_trigger(g_config, action, strlen(action));
    trigger_text(t, it->key, 48);
  }
}

static void items_load(void) {
  items_free();
  ghostty_config_command_list_s list = {0};
  static const char k[] = "command-palette-entry";
  if (g_config && ghostty_config_get(g_config, &list, k, strlen(k))) {
    for (size_t i = 0; i < list.len; i++) {
      const ghostty_command_s* c = &list.commands[i];
      if (!c->action || !c->action_key || !supported(c->action_key)) continue;
      WCHAR* t = utf8_to_utf16(c->title ? c->title : "", -1);
      WCHAR* d = utf8_to_utf16(c->description ? c->description : "", -1);
      items_add(t, d, c->action, 0);
      free(t);
      free(d);
    }
  }
  // The open terminals, to jump to (macOS: "Focus: <title>").
  for (Frame* f = g_frames; f; f = f->next)
    for (int i = 0; i < f->ntabs; i++) {
      Window* panes[64];
      int n = f->tabs[i]->tab ? tab_panes(f->tabs[i]->tab, panes, 64) : 0;
      if (n <= 0) panes[0] = f->tabs[i], n = 1;
      for (int p = 0; p < n && p < 64; p++) {
        WCHAR title[300];
        swprintf(title, 300, L"Focus: %ls", panes[p]->title && *panes[p]->title ? panes[p]->title : L"Untitled");
        items_add(title, L"Jump to this terminal", NULL, panes[p]->id);
      }
    }
}

// ---- the list --------------------------------------------------------------------

static bool matches(const PalItem* it, const WCHAR* q) {
  if (!*q) return true;
  WCHAR a[600];
  swprintf(a, 600, L"%ls %ls", it->title, it->desc);
  _wcslwr(a);
  WCHAR lq[128];
  wcsncpy(lq, q, 127);
  lq[127] = 0;
  _wcslwr(lq);
  // Every word of the query somewhere in the title or description.
  WCHAR* ctx = NULL;
  for (WCHAR* w = wcstok(lq, L" ", &ctx); w; w = wcstok(NULL, L" ", &ctx))
    if (!wcsstr(a, w)) return false;
  return true;
}

static void fill(void) {
  WCHAR q[128];
  GetWindowTextW(g_pal_edit, q, 128);
  SendMessageW(g_pal_list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(g_pal_list, LB_RESETCONTENT, 0, 0);
  // Titles holding the query as one phrase first ("split right": Split
  // Right before Focus Split: Right), then the rest that match word by word.
  WCHAR lq[128];
  wcsncpy(lq, q, 127);
  lq[127] = 0;
  _wcslwr(lq);
  for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < g_nitems; i++) {
      if (!matches(&g_items[i], q)) continue;
      WCHAR t[300];
      wcsncpy(t, g_items[i].title, 299);
      t[299] = 0;
      _wcslwr(t);
      bool phrase = *lq && wcsstr(t, lq);
      if ((pass == 0) == phrase || (!*lq && pass == 0))
        SendMessageW(g_pal_list, LB_ADDSTRING, 0, (LPARAM)i);
    }
  SendMessageW(g_pal_list, LB_SETCURSEL, 0, 0);
  SendMessageW(g_pal_list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_pal_list, NULL, TRUE);
}

static PalItem* selected(void) {
  LRESULT sel = SendMessageW(g_pal_list, LB_GETCURSEL, 0, 0);
  if (sel == LB_ERR) return NULL;
  LRESULT i = SendMessageW(g_pal_list, LB_GETITEMDATA, (WPARAM)sel, 0);
  return i >= 0 && i < g_nitems ? &g_items[i] : NULL;
}

static void pal_event(const char* what, const PalItem* it) {
  Line e = {0};
  put(&e, "{\"kind\":\"palette\",\"what\":\"%s\"", what);
  if (it) {
    char* t = utf16_to_utf8(it->title, -1);
    put(&e, ",\"title\":");
    put_string(&e, t ? t : "");
    put(&e, ",\"action\":");
    put_string(&e, it->action ? it->action : "");
    char* k = utf16_to_utf8(it->key, -1);
    put(&e, ",\"key\":");
    put_string(&e, k ? k : "");
    free(k);
    free(t);
  }
  put(&e, "}");
  test_event(e.p);
  free(e.p);
}

static Window* pane_by_id(int id) {
  for (Frame* f = g_frames; f; f = f->next)
    for (int i = 0; i < f->ntabs; i++) {
      Window* panes[64];
      int n = f->tabs[i]->tab ? tab_panes(f->tabs[i]->tab, panes, 64) : 0;
      if (n <= 0) panes[0] = f->tabs[i], n = 1;
      for (int k = 0; k < n && k < 64; k++)
        if (panes[k]->id == id) return panes[k];
    }
  return NULL;
}

static void pal_close(void) {
  if (!g_pal) return;
  HWND h = g_pal;
  g_pal = NULL;
  DestroyWindow(h);
}

// Enter (or a double-click): the selected command on the pane it was
// opened from, or focus the selected terminal.
static void pal_run(void) {
  PalItem* it = selected();
  if (!it) return;
  pal_event("run", it);
  char* action = it->action ? _strdup(it->action) : NULL;
  int jump = it->pane, from = g_pal_pane;
  pal_close();  // first: the command may open a window, change focus
  Window* w = pane_by_id(action ? from : jump);
  if (!w) {
    free(action);
    return;
  }
  Frame* f = w->frame;
  int ti = tab_index(f, w);
  if (ti >= 0) select_tab(f, ti);
  SetForegroundWindow(f->hwnd);
  SetFocus(w->hwnd);
  if (action && w->surface) ghostty_surface_binding_action(w->surface, action, strlen(action));
  free(action);
}

static LRESULT CALLBACK pal_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_KEYDOWN) {
    LRESULT cur = SendMessageW(g_pal_list, LB_GETCURSEL, 0, 0);
    LRESULT n = SendMessageW(g_pal_list, LB_GETCOUNT, 0, 0);
    int row = cur == LB_ERR ? 0 : (int)cur, to = -1;
    switch (wp) {
      case VK_DOWN: to = row + 1; break;
      case VK_UP: to = row - 1; break;
      case VK_NEXT: to = row + 8; break;
      case VK_PRIOR: to = row - 8; break;
      case VK_RETURN: pal_run(); return 0;
      case VK_ESCAPE:
        pal_event("closed", NULL);
        pal_close();
        return 0;
    }
    if (to != -1) {
      if (to >= n) to = (int)n - 1;
      if (to < 0) to = 0;
      SendMessageW(g_pal_list, LB_SETCURSEL, (WPARAM)to, 0);
      return 0;
    }
  }
  if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;  // no beep
  return CallWindowProcW(g_pal_edit_proc, h, msg, wp, lp);
}

static LRESULT CALLBACK pal_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
      if ((HWND)lp == g_pal_edit && HIWORD(wp) == EN_CHANGE) fill();
      else if ((HWND)lp == g_pal_list && HIWORD(wp) == LBN_DBLCLK) pal_run();
      return 0;
    case WM_MEASUREITEM: {
      MEASUREITEMSTRUCT* m = (MEASUREITEMSTRUCT*)lp;
      m->itemHeight = (UINT)(PAL_ROW_H * (g_pal_frame ? g_pal_frame->scale : 1));
      return TRUE;
    }
    case WM_DRAWITEM: {
      DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
      if (d->itemID == (UINT)-1) return TRUE;
      LRESULT i = SendMessageW(g_pal_list, LB_GETITEMDATA, d->itemID, 0);
      if (i < 0 || i >= g_nitems) return TRUE;
      PalItem* it = &g_items[i];
      bool sel = d->itemState & ODS_SELECTED;
      COLORREF bg = sel ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
      COLORREF fg = sel ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT);
      COLORREF dim = sel ? fg : GetSysColor(COLOR_GRAYTEXT);
      HBRUSH b = CreateSolidBrush(bg);
      FillRect(d->hDC, &d->rcItem, b);
      DeleteObject(b);
      SetBkMode(d->hDC, TRANSPARENT);
      RECT r = d->rcItem;
      int pad = (int)(8 * g_pal_frame->scale), half = (r.bottom - r.top) / 2;
      RECT key = {r.right - (int)(160 * g_pal_frame->scale), r.top, r.right - pad, r.top + half + pad / 2};
      RECT title = {r.left + pad, r.top + pad / 2, key.left - pad, r.top + half + pad / 2};
      RECT desc = {r.left + pad, r.top + half, r.right - pad, r.bottom - pad / 4};
      SelectObject(d->hDC, g_pal_font);
      SetTextColor(d->hDC, fg);
      DrawTextW(d->hDC, it->title, -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      SetTextColor(d->hDC, dim);
      DrawTextW(d->hDC, it->key, -1, &key, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(d->hDC, g_pal_small);
      DrawTextW(d->hDC, it->desc, -1, &desc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
      return TRUE;
    }
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE && g_pal == h) {  // a click elsewhere
        pal_event("closed", NULL);
        PostMessageW(h, WM_CLOSE, 0, 0);
      }
      return 0;
    case WM_PAINT: {
      // The header: the slogan quietly at the top right, the build under it
      // (as pys's help).
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      int m = (int)(8 * g_pal_frame->scale), line = (int)(PAL_HEAD_H * g_pal_frame->scale) / 2;
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      SelectObject(dc, g_pal_small);
      int r = rc.right - 2 * m;
      // The slogan when it fits whole, else the motto (pys's rule).
      RECT need = {0, 0, 0, 0};
      DrawTextW(dc, kSlogan, -1, &need, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      const WCHAR* text = need.right <= r - m ? kSlogan : kMotto;
      // Right-aligned by measure: placed from the right edge by its own
      // width (DT_RIGHT let the last glyph overhang the edge under Wine).
      const WCHAR* lines[2] = {text, L"Ghostty for Windows " WIDEN(GHOSTTY_BUILD_TAG)};
      for (int k = 0; k < 2; k++) {
        SIZE sz;
        GetTextExtentPoint32W(dc, lines[k], (int)wcslen(lines[k]), &sz);
        TextOutW(dc, r - sz.cx, m / 2 + k * line + (line - sz.cy) / 2, lines[k], (int)wcslen(lines[k]));
      }
      EndPaint(h, &ps);
      return 0;
    }
    case WM_SIZE: {
      int w = LOWORD(lp), hh = HIWORD(lp), eh = (int)(30 * g_pal_frame->scale), m = (int)(8 * g_pal_frame->scale);
      int head = (int)(PAL_HEAD_H * g_pal_frame->scale);
      MoveWindow(g_pal_edit, m, head + m, w - 2 * m, eh, TRUE);
      MoveWindow(g_pal_list, m, head + m + eh + m / 2, w - 2 * m, hh - head - eh - m * 2, TRUE);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      if (g_pal == h) g_pal = NULL;
      g_pal_edit = g_pal_list = NULL;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// toggle_command_palette from pane `w`: open it over w's window, or close it.
void palette_toggle(Window* w) {
  if (g_pal) {
    pal_event("closed", NULL);
    pal_close();
    return;
  }
  if (!w) return;
  Frame* f = w->frame;
  static bool registered;
  if (!registered) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = pal_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"GhosttyPalette";
    registered = RegisterClassExW(&wc) != 0;
  }
  if (g_pal_font) DeleteObject(g_pal_font);
  if (g_pal_small) DeleteObject(g_pal_small);
  g_pal_font = CreateFontW(-(int)(15 * f->scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_pal_small = CreateFontW(-(int)(12 * f->scale), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Segoe UI");
  items_load();
  g_pal_frame = f;
  g_pal_pane = w->id;
  RECT fr;
  GetWindowRect(f->hwnd, &fr);
  int pw = (int)(560 * f->scale), ph = (int)((420 + PAL_HEAD_H) * f->scale);
  int x = fr.left + ((fr.right - fr.left) - pw) / 2, y = fr.top + (int)(48 * f->scale);
  g_pal = CreateWindowExW(WS_EX_TOOLWINDOW, L"GhosttyPalette", L"Command palette",
                          WS_POPUP | WS_BORDER, x, y, pw, ph, f->hwnd, NULL, g_inst, NULL);
  if (!g_pal) return;
  g_pal_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0,
                               0, 0, 0, g_pal, NULL, g_inst, NULL);
  g_pal_list = CreateWindowExW(0, L"LISTBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT |
                                   LBS_OWNERDRAWFIXED,
                               0, 0, 0, 0, g_pal, NULL, g_inst, NULL);
  SendMessageW(g_pal_edit, WM_SETFONT, (WPARAM)g_pal_font, TRUE);
  SendMessageW(g_pal_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type a command, or a terminal's title");
  g_pal_edit_proc = (WNDPROC)SetWindowLongPtrW(g_pal_edit, GWLP_WNDPROC, (LONG_PTR)pal_edit_proc);
  fill();
  RECT rc;
  GetClientRect(g_pal, &rc);
  SendMessageW(g_pal, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
  ShowWindow(g_pal, SW_SHOW);
  SetForegroundWindow(g_pal);
  SetFocus(g_pal_edit);
  int keyed = 0, jumps = 0;
  for (int i = 0; i < g_nitems; i++) {
    keyed += g_items[i].key[0] != 0;
    jumps += g_items[i].action == NULL;
  }
  char ev[160];
  snprintf(ev, sizeof(ev), "{\"kind\":\"palette\",\"what\":\"opened\",\"items\":%d,\"with_key\":%d,\"jumps\":%d}",
           g_nitems, keyed, jumps);
  test_event(ev);
}
