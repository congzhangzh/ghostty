#include "host.h"

// ---------------------------------------------------------------------------
// The tab bar
//
// Drawn by the Frame in its client area above the tabs, in the terminal's
// own colors as macOS does: the selected tab has the terminal's background
// and the others are darker; tabs share the width equally, the title is
// centered, a tab under the mouse shows its close button on the left, and
// the + at the right end opens a new tab.
//
// On Windows 11 the bar is the title bar, as in Chrome, to give the
// terminal the row a separate bar would take: the client area takes the
// caption's place (WM_NCCALCSIZE), DWM's frame reaches down over it
// (DwmExtendFrameIntoClientArea) so Windows still draws its own minimize,
// maximize and close buttons there (snap layouts, hover, dark mode,
// accessibility: DwmDefWindowProc answers for them), and the bar keeps
// their place clear. The rest of the bar that is not a tab or the + is the
// caption: drag it to move the window, double-click to maximize; a tab
// dragged moves the window too. GHOSTTY_TITLEBAR=tabs|system chooses.

static int px(Frame* f, double points) { return (int)(points * f->scale + 0.5); }

bool g_titlebar_tabs;

bool titlebar_active(Frame* f) { return g_titlebar_tabs && !f->fullscreen; }

int bar_height(Frame* f) {
  // In the title bar: as tall as Windows' caption buttons (the caption at
  // this monitor's DPI), at least the bar's own height.
  if (titlebar_active(f))
    return f->buttons_h > px(f, 30) ? f->buttons_h : px(f, 30);
  bool show = g_tab_bar == TAB_BAR_ALWAYS ||
              (g_tab_bar == TAB_BAR_AUTO && f->ntabs > 1);
  return show ? px(f, 30) : 0;
}

// Where the tabs and the + go, [0, end): in the title bar, Windows'
// buttons keep their place on the right, and a gap before them is left to
// drag the window by.
static int bar_end(Frame* f, int width) {
  if (!titlebar_active(f)) return width;
  int end = width - f->buttons_w - px(f, 40);
  return end > 0 ? end : 0;
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

// In the title bar a tab is at most 240 points wide, as in Chrome: with few
// tabs the + follows the last one, and the rest of the bar is the caption
// (drag it), with room for the motto. Elsewhere the tabs share the width.
static int tab_cap(Frame* f, int width, int b) {
  int area = bar_end(f, width) - b;  // the + button is b wide
  if (!titlebar_active(f) || f->ntabs <= 0) return 0;
  int cap = px(f, 240);
  return area / f->ntabs > cap ? cap : 0;
}

static void tab_rect(Frame* f, int width, int b, int i, RECT* r) {
  int area = bar_end(f, width) - b;  // the + button is b wide
  if (area < 0) area = 0;
  int cap = tab_cap(f, width, b);
  r->left = cap ? cap * i : (int)((long long)area * i / f->ntabs);
  r->right = cap ? cap * (i + 1) : (int)((long long)area * (i + 1) / f->ntabs);
  r->top = 0;
  r->bottom = b;
}

// Where the + starts: after the last tab.
static int plus_x(Frame* f, int width, int b) {
  int cap = tab_cap(f, width, b);
  return cap ? cap * f->ntabs : bar_end(f, width) - b;
}

static void close_rect(Frame* f, const RECT* tab, RECT* r) {
  int s = px(f, 16), pad = px(f, 7);
  r->left = tab->left + pad;
  r->right = r->left + s;
  r->top = (tab->bottom - s) / 2;
  r->bottom = r->top + s;
}

BarHit bar_hit(Frame* f, int x, int y, int* index) {
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  int b = bar_height(f);
  *index = -1;
  int end = bar_end(f, rc.right);
  int xp = plus_x(f, rc.right, b);
  if (b == 0 || y < 0 || y >= b || x < 0 || x >= end) return HIT_NONE;
  if (x >= xp + b) return HIT_NONE;  // after the +: the caption
  if (x >= xp) return HIT_PLUS;
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

COLORREF bar_color(void) {
  return mix(g_bg, RGB(0, 0, 0), is_dark(g_bg) ? 0.35 : 0.10);
}

bool bar_is_dark(void) { return is_dark(bar_color()); }

static void draw_bar(Frame* f, HDC dc, int width, int b) {
  COLORREF bar = bar_color();
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

  // The new-tab button, at the end of the tabs; then the bar's edge goes
  // on under the gap and Windows' buttons.
  int end = bar_end(f, width);
  int xp = plus_x(f, width, b);
  if (f->hover_plus) fill(dc, xp, 0, xp + b, b, hover);
  fill(dc, xp, b - 1, width, b, line);
  int cx = xp + b / 2, cy = b / 2, h = px(f, 6);
  stroke(dc, cx - h, cy, cx + h + 1, cy, lw, dim);
  stroke(dc, cx, cy - h, cx, cy + h + 1, lw, dim);

  // Ghostty's motto in the caption's empty part, dim, only when it fits
  // whole (the tabs come first). GHOSTTY_SLOGAN=false turns it off.
  // A hint from the Theme menu's key takes its place for a few seconds.
  const WCHAR* hint = theme_hint();
  if (hint) {
    RECT m = {xp + b + px(f, 16), 0, end, b};
    SetTextColor(dc, g_fg);
    DrawTextW(dc, hint, -1, &m,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
  } else if (titlebar_active(f) && slogan_shown()) {
    RECT m = {xp + b + px(f, 16), 0, end, b};
    RECT need = {0, 0, 0, 0};
    DrawTextW(dc, kSlogan, -1, &need, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    if (need.right <= m.right - m.left) {
      SetTextColor(dc, mix(dim, bar, 0.35));
      DrawTextW(dc, kSlogan, -1, &m,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
  }
}

void paint_frame(Frame* f) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(f->hwnd, &ps);
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  int b = bar_height(f);
  if (b > 0 && rc.right > 0) {
    // Draw off screen, then copy: no flicker while the mouse moves. A
    // 32-bit DIB, because in the title bar DWM reads its alpha: GDI leaves
    // alpha 0 (see-through to DWM's frame), so it is set to 255 for the bar,
    // and Windows' buttons' place is left clear (0) for DWM to draw them.
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), rc.right, -b, 1, 32, BI_RGB}};
    uint32_t* pixels = NULL;
    HBITMAP bmp =
        CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    if (!bmp) {
      EndPaint(f->hwnd, &ps);
      return;
    }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    draw_bar(f, mem, rc.right, b);
    GdiFlush();
    int c0 = rc.right, c1 = rc.right;  // the buttons' columns
    if (titlebar_active(f) && f->buttons_w > 0) c0 = rc.right - f->buttons_w;
    for (int y = 0; y < b; y++)
      for (int x = 0; x < rc.right; x++) {
        uint32_t* p = &pixels[(size_t)y * rc.right + x];
        *p = x >= c0 && x < c1 ? 0 : (*p | 0xff000000u);
      }
    BitBlt(dc, 0, 0, rc.right, b, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
  }
  // Under the bar the panes cover all but the dividers between them
  // (WS_CLIPCHILDREN): those get the bar's edge color.
  if (rc.bottom > b) {
    RECT r = {0, b, rc.right, rc.bottom};
    SetDCBrushColor(dc, mix(bar_color(), g_fg, 0.12));
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
  }
  EndPaint(f->hwnd, &ps);
}

void invalidate_bar(Frame* f) {
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  rc.bottom = bar_height(f);
  if (rc.bottom > 0) InvalidateRect(f->hwnd, &rc, FALSE);
}

// ---------------------------------------------------------------------------
// Tabs

// The tab a pane is in (any pane of it: f->tabs[i] is the tab's focused
// pane, so everything that looks at "the tab" sees the pane in use).
int tab_index(Frame* f, Window* w) {
  for (int i = 0; i < f->ntabs; i++)
    if (f->tabs[i] == w || (w && w->tab && f->tabs[i]->tab == w->tab)) return i;
  return -1;
}

// ---------------------------------------------------------------------------
// Splits

static void collect(Node* n, Window** out, int cap, int* count) {
  if (!n) return;
  if (n->pane) {
    if (*count < cap) out[*count] = n->pane;
    (*count)++;
    return;
  }
  collect(n->a, out, cap, count);
  collect(n->b, out, cap, count);
}

// A tab's panes in tree order (left to right, top to bottom); the count.
int tab_panes(Tab* t, Window** out, int cap) {
  int count = 0;
  if (t) collect(t->root, out, cap, &count);
  return count;
}

#define MAX_PANES 64

// Lay a node out in r: a pane gets the rectangle, a split divides it with
// a one-pixel divider (the frame paints it).
static void layout_node(Node* n, RECT r) {
  if (!n) return;
  if (n->pane) {
    SetWindowPos(n->pane->hwnd, NULL, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }
  RECT a = r, b = r;
  if (n->vertical) {
    int h = r.bottom - r.top - 1;
    int cut = r.top + (int)(h * n->ratio + 0.5);
    a.bottom = cut;
    b.top = cut + 1;
  } else {
    int w = r.right - r.left - 1;
    int cut = r.left + (int)(w * n->ratio + 0.5);
    a.right = cut;
    b.left = cut + 1;
  }
  layout_node(n->a, a);
  layout_node(n->b, b);
}

// Show (or hide) a tab's panes: all of them, or the zoomed one alone. A
// hidden pane's surface is told it is occluded.
static void show_tab(Window* any, bool show) {
  Window* panes[MAX_PANES];
  int n = tab_panes(any->tab, panes, MAX_PANES);
  if (n > MAX_PANES) n = MAX_PANES;
  for (int i = 0; i < n; i++) {
    bool on = show && (!any->tab->zoomed || panes[i] == any->tab->zoomed);
    ghostty_surface_set_occlusion(panes[i]->surface, on);
    ShowWindow(panes[i]->hwnd, on ? SW_SHOW : SW_HIDE);
  }
}

// A pane got the keyboard: it is its tab's focused pane now (the tab's
// title, the window's title, the bar follow it).
void pane_focused(Window* w) {
  Frame* f = w->frame;
  int i = tab_index(f, w);
  if (i < 0 || f->tabs[i] == w) return;
  f->tabs[i] = w;
  if (i == f->active) update_title(f);
  invalidate_bar(f);
}

// The window's title (the taskbar's too): the selected tab's title and
// which build runs, "<title> \u2014 Ghostty <build>". The tabs show the
// titles alone. With Windows' own caption (no tabs in the title bar) a
// hint from the Theme key goes first, for its few seconds.
void update_title(Frame* f) {
  static const WCHAR app[] = L"Ghostty " WIDEN(GHOSTTY_BUILD_TAG);
  Window* w = f->active >= 0 ? f->tabs[f->active] : NULL;
  const WCHAR* title = w && w->title && *w->title ? w->title : NULL;
  const WCHAR* hint = titlebar_active(f) ? NULL : theme_hint();
  size_t n = (title ? wcslen(title) : 0) + (hint ? wcslen(hint) : 0) + wcslen(app) + 8;
  WCHAR* t = malloc(n * sizeof(WCHAR));
  if (!t) {
    SetWindowTextW(f->hwnd, title ? title : app);
    return;
  }
  swprintf(t, n, L"%ls%ls%ls%ls%ls", hint ? hint : L"", hint ? L" \u2014 " : L"",
           title ? title : L"", title ? L" \u2014 " : L"", app);
  SetWindowTextW(f->hwnd, t);
  free(t);
}

// Size every tab to the client area under the tab bar. The hidden ones
// too, so switching to one shows a frame of the right size at once.
void layout(Frame* f) {
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) return;  // minimized
  titlebar_update(f);  // the caption buttons, DWM's frame over the bar
  int b = bar_height(f);
  RECT area = {0, b, rc.right, rc.bottom > b ? rc.bottom : b};
  for (int i = 0; i < f->ntabs; i++) {
    Tab* t = f->tabs[i]->tab;
    if (t && t->zoomed)
      layout_node(t->zoomed->node, area);  // the zoomed pane: all the room
    else
      layout_node(t ? t->root : NULL, area);
  }
  InvalidateRect(f->hwnd, NULL, FALSE);
}

// Show tab i and hide the one shown before. A hidden tab's surface is
// told it is occluded, so its renderer stops drawing frames nobody sees.
void select_tab(Frame* f, int i) {
  if (i < 0 || i >= f->ntabs) return;
  Window* old = f->active >= 0 && f->active < f->ntabs ? f->tabs[f->active]
                                                        : NULL;
  Window* w = f->tabs[i];
  f->active = i;
  show_tab(w, true);
  if (GetActiveWindow() == f->hwnd) SetFocus(w->hwnd);
  if (old && old->tab != w->tab) show_tab(old, false);
  update_title(f);
  invalidate_bar(f);
  notify_frame_changed(f);
}

// Take a tab out of its Frame (it closed, or moves to another window).
void detach_tab(Frame* f, Window* w) {
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

bool insert_tab(Frame* f, Window* w, int at) {
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
void size_tab(Window* w) {
  RECT rc;
  GetClientRect(w->hwnd, &rc);
  if (rc.right > 0 && rc.bottom > 0)
    ghostty_surface_set_size(w->surface, (uint32_t)rc.right,
                             (uint32_t)rc.bottom);
}

// A new pane in f: its child window and its surface, inheriting the
// working directory and font size of `from` when there is one. Not yet in
// a tab.
static Window* new_pane(Frame* f, ghostty_surface_t from,
                        ghostty_surface_context_e context) {
  static int next_id;
  Window* w = calloc(1, sizeof(Window));
  if (!w) return NULL;
  w->id = ++next_id;
  w->frame = f;
  w->scale = f->scale;
  w->hwnd = CreateWindowExW(0, kTerminalClass, L"", WS_CHILD | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, f->hwnd, NULL, g_inst, NULL);
  if (!w->hwnd) {
    free(w);
    return NULL;
  }
  SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, (LONG_PTR)w);
  DragAcceptFiles(w->hwnd, TRUE);  // files dropped on it: their paths (drop_files)
  w->dxgi = present_dxgi_wanted();  // frames through a DXGI flip swap chain (dxgi.c)

  ghostty_surface_config_s cfg = from ? ghostty_surface_inherited_config(from, context)
                                      : ghostty_surface_config_new();
  cfg.platform_tag = GHOSTTY_PLATFORM_WINDOWS;
  cfg.platform.windows.hwnd = w->hwnd;
  cfg.userdata = w;
  cfg.scale_factor = w->scale;
  cfg.context = context;
  w->surface = ghostty_surface_new(g_app, &cfg);
  w->node = calloc(1, sizeof(Node));
  if (!w->surface || !w->node) {
    fprintf(stderr, "ghostty: could not create the terminal surface\n");
    if (w->surface) ghostty_surface_free(w->surface);
    free(w->node);
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(w->hwnd);
    free(w);
    return NULL;
  }
  w->node->pane = w;
  ghostty_surface_set_content_scale(w->surface, w->scale, w->scale);
  ghostty_surface_set_color_scheme(w->surface, color_scheme_now());
  return w;
}

// Undo new_pane for a pane in no tree yet.
static void free_pane(Window* w) {
  ghostty_surface_free(w->surface);
  SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);
  DestroyWindow(w->hwnd);
  free(w->node);
  free(w);
}

// A new tab in f, after the selected one (or at the end): one pane.
Window* new_tab(Frame* f, ghostty_surface_t from,
                       ghostty_surface_context_e context) {
  Window* w = new_pane(f, from, context);
  if (!w) return NULL;
  w->tab = calloc(1, sizeof(Tab));
  if (!w->tab || !insert_tab(f, w, g_new_tab_at_end || f->active < 0
                                       ? f->ntabs
                                       : f->active + 1)) {
    free(w->tab);
    free_pane(w);
    return NULL;
  }
  w->tab->root = w->node;
  layout(f);
  size_tab(w);
  select_tab(f, tab_index(f, w));
  char ev[96];
  snprintf(ev, sizeof(ev), "{\"kind\":\"tab_opened\",\"pane\":%d}", w->id);
  test_event(ev);
  return w;
}

// A new pane beside `w` (right, down, left or up), in w's tab: the two
// share w's place half and half; the new one takes the keyboard (as the
// macOS app does). A zoomed tab is unzoomed first.
bool new_split(Window* w, ghostty_action_split_direction_e dir) {
  Frame* f = w->frame;
  int ti = tab_index(f, w);
  if (ti < 0 || tab_panes(w->tab, NULL, 0) >= MAX_PANES) return false;
  Window* n = new_pane(f, w->surface, GHOSTTY_SURFACE_CONTEXT_SPLIT);
  if (!n) return false;
  Node* split = calloc(1, sizeof(Node));
  if (!split) {
    free_pane(n);
    return false;
  }
  bool first = dir == GHOSTTY_SPLIT_DIRECTION_LEFT || dir == GHOSTTY_SPLIT_DIRECTION_UP;
  Node* old = w->node;
  split->parent = old->parent;
  split->vertical = dir == GHOSTTY_SPLIT_DIRECTION_DOWN || dir == GHOSTTY_SPLIT_DIRECTION_UP;
  split->ratio = 0.5;
  split->a = first ? n->node : old;
  split->b = first ? old : n->node;
  if (!old->parent)
    w->tab->root = split;
  else if (old->parent->a == old)
    old->parent->a = split;
  else
    old->parent->b = split;
  old->parent = n->node->parent = split;
  n->tab = w->tab;
  w->tab->zoomed = NULL;
  f->tabs[ti] = n;  // the tab's focused pane
  layout(f);
  size_tab(n);
  if (ti == f->active) {
    show_tab(n, true);
    if (GetActiveWindow() == f->hwnd) SetFocus(n->hwnd);
    update_title(f);
  } else {
    show_tab(n, false);
  }
  invalidate_bar(f);
  return true;
}

// The rectangle a node has in the tab (frame client coordinates), found by
// laying the tree out again in arithmetic: layout_node's rule.
static bool node_rect(Node* n, RECT r, Node* want, RECT* out) {
  if (n == want) {
    *out = r;
    return true;
  }
  if (!n || n->pane) return false;
  RECT a = r, b = r;
  if (n->vertical) {
    int cut = r.top + (int)((r.bottom - r.top - 1) * n->ratio + 0.5);
    a.bottom = cut;
    b.top = cut + 1;
  } else {
    int cut = r.left + (int)((r.right - r.left - 1) * n->ratio + 0.5);
    a.right = cut;
    b.left = cut + 1;
  }
  return node_rect(n->a, a, want, out) || node_rect(n->b, b, want, out);
}

static RECT tab_area(Frame* f) {
  RECT rc;
  GetClientRect(f->hwnd, &rc);
  int b = bar_height(f);
  RECT area = {0, b, rc.right, rc.bottom > b ? rc.bottom : b};
  return area;
}

// Give the keyboard to a pane of the selected tab (unzooming if another one
// is zoomed).
static void focus_pane(Window* to) {
  Frame* f = to->frame;
  if (to->tab->zoomed && to->tab->zoomed != to) {
    to->tab->zoomed = NULL;
    layout(f);
    show_tab(to, true);
  }
  SetFocus(to->hwnd);
  pane_focused(to);
}

// Previous/next in tree order (wrapping), or the nearest pane on a side:
// wholly on that side, overlapping along the other axis; the closest wins,
// then the one whose middle is nearest. Not performed (false) with one
// pane or none there, so the key goes on to the terminal.
bool goto_split(Window* w, ghostty_action_goto_split_e to) {
  Window* panes[MAX_PANES];
  int n = tab_panes(w->tab, panes, MAX_PANES);
  if (n > MAX_PANES) n = MAX_PANES;
  if (n < 2) return false;
  int me = 0;
  while (me < n && panes[me] != w) me++;
  if (me == n) return false;
  if (to == GHOSTTY_GOTO_SPLIT_PREVIOUS || to == GHOSTTY_GOTO_SPLIT_NEXT) {
    int step = to == GHOSTTY_GOTO_SPLIT_NEXT ? 1 : -1;
    focus_pane(panes[(me + step + n) % n]);
    return true;
  }
  RECT area = tab_area(w->frame), cur, r;
  node_rect(w->tab->root, area, w->node, &cur);
  Window* best = NULL;
  long best_gap = 0, best_mid = 0;
  for (int i = 0; i < n; i++) {
    if (i == me || !node_rect(w->tab->root, area, panes[i]->node, &r)) continue;
    long gap, overlap, mid;
    switch (to) {
      case GHOSTTY_GOTO_SPLIT_LEFT: gap = cur.left - r.right; break;
      case GHOSTTY_GOTO_SPLIT_RIGHT: gap = r.left - cur.right; break;
      case GHOSTTY_GOTO_SPLIT_UP: gap = cur.top - r.bottom; break;
      default: gap = r.top - cur.bottom; break;  // down
    }
    bool across = to == GHOSTTY_GOTO_SPLIT_LEFT || to == GHOSTTY_GOTO_SPLIT_RIGHT;
    if (across) {
      overlap = (r.bottom < cur.bottom ? r.bottom : cur.bottom) -
                (r.top > cur.top ? r.top : cur.top);
      mid = labs((r.top + r.bottom) - (cur.top + cur.bottom));
    } else {
      overlap = (r.right < cur.right ? r.right : cur.right) -
                (r.left > cur.left ? r.left : cur.left);
      mid = labs((r.left + r.right) - (cur.left + cur.right));
    }
    if (gap < 0 || overlap <= 0) continue;
    if (!best || gap < best_gap || (gap == best_gap && mid < best_mid)) {
      best = panes[i];
      best_gap = gap;
      best_mid = mid;
    }
  }
  if (!best) return false;
  focus_pane(best);
  return true;
}

// Move the divider of the nearest split around w that runs the right way,
// by `amount` pixels toward the direction (kept between 10% and 90%).
bool resize_split(Window* w, ghostty_action_resize_split_s rs) {
  bool across = rs.direction == GHOSTTY_RESIZE_SPLIT_LEFT ||
                rs.direction == GHOSTTY_RESIZE_SPLIT_RIGHT;
  Node* s = w->node ? w->node->parent : NULL;
  while (s && s->vertical == across) s = s->parent;
  if (!s) return false;
  RECT area = tab_area(w->frame), r;
  if (!node_rect(w->tab->root, area, s, &r)) return false;
  int room = (across ? r.right - r.left : r.bottom - r.top) - 1;
  if (room <= 0) return false;
  int sign = rs.direction == GHOSTTY_RESIZE_SPLIT_LEFT ||
                     rs.direction == GHOSTTY_RESIZE_SPLIT_UP
                 ? -1
                 : 1;
  double ratio = s->ratio + sign * (double)rs.amount / room;
  s->ratio = ratio < 0.1 ? 0.1 : ratio > 0.9 ? 0.9 : ratio;
  layout(w->frame);
  return true;
}

// How many panes a node lines up along a direction: a split that runs
// that way adds its sides, one across takes the larger side (two panes
// stacked in a column are one column wide).
static int weight(Node* n, bool vertical) {
  if (!n) return 0;
  if (n->pane) return 1;
  int a = weight(n->a, vertical), b = weight(n->b, vertical);
  return n->vertical == vertical ? a + b : (a > b ? a : b);
}

static void equalize(Node* n) {
  if (!n || n->pane) return;
  int a = weight(n->a, n->vertical), b = weight(n->b, n->vertical);
  n->ratio = (double)a / (a + b);
  equalize(n->a);
  equalize(n->b);
}

// Every split shares its room by how many panes line up on each side.
bool equalize_splits(Window* w) {
  if (!w->tab || !w->tab->root || w->tab->root->pane) return false;
  equalize(w->tab->root);
  layout(w->frame);
  return true;
}

// The focused pane takes the whole tab, or gives it back.
bool toggle_split_zoom(Window* w) {
  if (!w->tab || !w->tab->root || w->tab->root->pane) return false;
  w->tab->zoomed = w->tab->zoomed ? NULL : w;
  layout(w->frame);
  if (tab_index(w->frame, w) == w->frame->active) {
    show_tab(w, true);
    SetFocus(w->hwnd);
  }
  return true;
}

// ---- dragging a divider ----------------------------------------------------
//
// A divider is one pixel; it can be taken within 3 points of it, inside the
// panes (their windows get the mouse there: the pane sends the press to the
// frame, which captures the mouse and moves the split's ratio).

static Node* divider_in(Node* n, RECT r, POINT p, int tol) {
  if (!n || n->pane) return NULL;
  RECT a = r, b = r;
  int cut;
  if (n->vertical) {
    cut = r.top + (int)((r.bottom - r.top - 1) * n->ratio + 0.5);
    a.bottom = cut;
    b.top = cut + 1;
  } else {
    cut = r.left + (int)((r.right - r.left - 1) * n->ratio + 0.5);
    a.right = cut;
    b.left = cut + 1;
  }
  Node* d = divider_in(n->a, a, p, tol);  // the deeper one first
  if (!d) d = divider_in(n->b, b, p, tol);
  if (d) return d;
  // ("near" is a macro in Windows' headers, a 16-bit leftover.)
  bool on = n->vertical
                ? abs(p.y - cut) <= tol && p.x >= r.left && p.x < r.right
                : abs(p.x - cut) <= tol && p.y >= r.top && p.y < r.bottom;
  return on ? n : NULL;
}

// The split of the selected tab whose divider is under p (the frame's
// client coordinates), or NULL; none in a zoomed tab.
Node* divider_at(Frame* f, POINT p) {
  if (f->active < 0) return NULL;
  Tab* t = f->tabs[f->active]->tab;
  if (!t || t->zoomed || !t->root) return NULL;
  return divider_in(t->root, tab_area(f), p, px(f, 3));
}

LPCWSTR divider_cursor(Node* n) { return n->vertical ? IDC_SIZENS : IDC_SIZEWE; }

void divider_drag_start(Frame* f, Node* n) {
  f->drag = n;
  SetCapture(f->hwnd);
}

// The divider follows the mouse (kept between 10% and 90%).
void divider_drag_to(Frame* f, POINT p) {
  Node* n = f->drag;
  if (!n || f->active < 0) return;
  Tab* t = f->tabs[f->active]->tab;
  RECT r;
  if (!t || !node_rect(t->root, tab_area(f), n, &r)) return;
  int extent = (n->vertical ? r.bottom - r.top : r.right - r.left) - 1;
  if (extent <= 0) return;
  double ratio = (double)(n->vertical ? p.y - r.top : p.x - r.left) / extent;
  n->ratio = ratio < 0.1 ? 0.1 : ratio > 0.9 ? 0.9 : ratio;
  layout(f);
}

// A pane closed (its surface went): its leaf leaves the tree, its sibling
// takes the parent's place, and the tab's focus moves there. The tab's
// last pane takes the tab with it (detach_tab, as before splits).
void pane_gone(Frame* f, Window* w) {
  Tab* t = w->tab;
  Node* leaf = w->node;
  char ev[96];
  snprintf(ev, sizeof(ev), "{\"kind\":\"%s\",\"pane\":%d}",
           leaf && leaf->parent ? "pane_closed" : "tab_closed", w->id);
  test_event(ev);
  if (!t || !leaf || !leaf->parent) {
    detach_tab(f, w);
    free(leaf);
    free(t);
    return;
  }
  int ti = tab_index(f, w);
  Node* parent = leaf->parent;
  Node* sibling = parent->a == leaf ? parent->b : parent->a;
  if (f->drag == parent) f->drag = NULL;  // its divider goes with it
  sibling->parent = parent->parent;
  if (!parent->parent)
    t->root = sibling;
  else if (parent->parent->a == parent)
    parent->parent->a = sibling;
  else
    parent->parent->b = sibling;
  free(parent);
  free(leaf);
  w->node = NULL;
  if (t->zoomed == w) t->zoomed = NULL;
  if (ti >= 0 && f->tabs[ti] == w) {
    // The first pane of the side that took its place.
    Node* n = sibling;
    while (!n->pane) n = n->a;
    f->tabs[ti] = n->pane;
  }
  if (f->destroying) return;
  layout(f);
  if (ti >= 0) {
    if (ti == f->active) {
      show_tab(f->tabs[ti], true);
      if (GetActiveWindow() == f->hwnd) SetFocus(f->tabs[ti]->hwnd);
      update_title(f);
    }
    invalidate_bar(f);
  }
}

// Move the selected tab by `amount`, wrapping around the ends.
void move_tab(Frame* f, int amount) {
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
// All the tab's panes are asked (collected first: a pane may close at once).
void close_tab(Window* w) {
  Window* panes[MAX_PANES];
  int n = tab_panes(w->tab, panes, MAX_PANES);
  if (n <= 0) {
    ghostty_surface_request_close(w->surface);
    return;
  }
  if (n > MAX_PANES) n = MAX_PANES;
  for (int i = 0; i < n; i++) ghostty_surface_request_close(panes[i]->surface);
}

// ---------------------------------------------------------------------------
// Window procedures

// A tab: its surface's input and frames.
LRESULT CALLBACK terminal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Window* w = (Window*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (!w || !w->surface) return DefWindowProcW(hwnd, msg, wp, lp);
  LRESULT ime_result;
  if (ime_message(w, msg, wp, lp, &ime_result)) return ime_result;

  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      // Let Windows have Alt+F4 and Alt+Space (the window menu); for a
      // child window DefWindowProc hands them to the top-level window.
      if (msg == WM_SYSKEYDOWN && (wp == VK_F4 || wp == VK_SPACE)) break;
      // Ctrl+Shift+F12: Auto / Light / Dark (theme.c), the host's own key;
      // Ghostty binds nothing to F12. Its release is kept from the core too.
      if (wp == VK_F12 && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) &&
          !(GetKeyState(VK_MENU) & 0x8000)) {
        if (!(lp & (1 << 30))) scheme_cycle();  // not on auto-repeat
        return 0;  // its release is kept back too (input.c's pressed)
      }
      handle_key(w, msg, wp, lp);
      return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      handle_key(w, msg, wp, lp);
      if (msg == WM_SYSKEYUP && wp == VK_MENU) return 0;  // no menu focus
      break;
    case WM_SYSCHAR:
      if (wp == L' ') {
        // Alt+Space: the window menu, at the terminal's top left (its key
        // went to DefWindowProc above; the space must not reach the shell).
        POINT p = {0, 0};
        ClientToScreen(hwnd, &p);
        show_window_menu(w->frame, p.x, p.y);
        return 0;
      }
      handle_char(w, (WCHAR)wp);
      return 0;
    case WM_CHAR:
      handle_char(w, (WCHAR)wp);
      return 0;
    case WM_DEADCHAR:
    case WM_SYSDEADCHAR:
      return 0;

    case WM_DROPFILES:
      drop_files(w, (HDROP)wp);
      return 0;

    case WM_MOUSEMOVE:
      mouse_pos(w, lp);
      return 0;
    case WM_LBUTTONDOWN: {
      // A press near a divider drags it (the terminal never sees it).
      POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      MapWindowPoints(hwnd, w->frame->hwnd, &p, 1);
      Node* d = divider_at(w->frame, p);
      if (d) {
        divider_drag_start(w->frame, d);
        return 0;
      }
    }
      if (GetFocus() != hwnd) SetFocus(hwnd);  // a click picks the pane
      mouse_button(w, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_LEFT);
      return 0;
    case WM_LBUTTONUP:
      mouse_button(w, GHOSTTY_MOUSE_RELEASE, GHOSTTY_MOUSE_LEFT);
      return 0;
    case WM_RBUTTONDOWN:
      if (wp & MK_SHIFT) return 0;  // Shift+right-click: the window menu
      mouse_button(w, GHOSTTY_MOUSE_PRESS, GHOSTTY_MOUSE_RIGHT);
      return 0;
    case WM_RBUTTONUP:
      if (wp & MK_SHIFT) {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &p);
        show_window_menu(w->frame, p.x, p.y);
        return 0;
      }
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
        // Near a divider: the arrow that says it can be dragged.
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(w->frame->hwnd, &p);
        Node* d = divider_at(w->frame, p);
        SetCursor(LoadCursorW(NULL, d ? divider_cursor(d) : IDC_IBEAM));
        return TRUE;
      }
      break;

    case WM_SIZE:
      if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp))
        ghostty_surface_set_size(w->surface, LOWORD(lp), HIWORD(lp));
      return 0;
    case WM_SETFOCUS:
      ghostty_surface_set_focus(w->surface, true);
      pane_focused(w);
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
      notify_tab_closed(w);
      pane_gone(f, w);
      free(w->title);
      free(w->ime_preedit);
      dxgi_pane_gone(w);
      free(w->bgra);
      free(w);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
