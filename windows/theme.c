#include "host.h"

// ---------------------------------------------------------------------------
// Themes: a list to switch, and Light / Dark / Auto
//
// The window menu's "Theme ▸" (the owner: "a list to switch theme, and
// light dark for quick"): favourites, each a light/dark pair (written
// `theme = light:X,dark:Y`, so the quick switch always changes something),
// "More themes…" (every single theme, a search list with a live preview on
// that window: Enter keeps, Esc reverts), "Default (from my config)", and
// Auto / Light / Dark (also Ctrl+Shift+F12, round; on a theme without a
// pair the title bar says so for a few seconds).
//
// His config is never rewritten. A chosen theme goes in
// %LOCALAPPDATA%\ghostty\theme.ghostty ("theme = <name>"), which the host
// loads after the default files and before the command line on every
// load, reloads included; "Default" deletes it. A theme it names that is
// gone is said on stderr and left out (the file stays). Auto / Light /
// Dark is the host's own (host.ini): what it tells the core the system's
// color scheme is (Auto: Windows' app mode).

#define IDM_THEME_DEFAULT 0x1050
#define IDM_THEME_MORE 0x1060
#define IDM_SCHEME_AUTO 0x1070
#define IDM_SCHEME_LIGHT 0x1080
#define IDM_SCHEME_DARK 0x1090
#define IDM_THEME_FAV 0x1100  // + 0x10 per favourite

// The favourites: one row per family, its light and dark theme (the names
// as share\ghostty\themes has them).
static const struct {
  const WCHAR* label;
  const char* light;
  const char* dark;
} kPairs[] = {
    {L"Catppuccin (Latte / Mocha)", "Catppuccin Latte", "Catppuccin Mocha"},
    {L"GitHub (Light / Dark)", "GitHub Light Default", "GitHub Dark Default"},
    {L"Solarized (Light / Dark)", "iTerm2 Solarized Light", "iTerm2 Solarized Dark"},
    {L"Gruvbox (Light / Dark)", "Gruvbox Light", "Gruvbox Dark"},
    {L"Tokyo Night (Day / Night)", "TokyoNight Day", "TokyoNight Night"},
    {L"Rose Pine (Dawn / Main)", "Rose Pine Dawn", "Rose Pine"},
    {L"Atom One (Light / Dark)", "Atom One Light", "Atom One Dark"},
    {L"Builtin (Light / Dark)", "Builtin Light", "Builtin Dark"},
};
#define NFAV ((int)(sizeof(kPairs) / sizeof(kPairs[0])))

static void pair_value(int i, char* out, size_t cap) {
  snprintf(out, cap, "light:%s,dark:%s", kPairs[i].light, kPairs[i].dark);
}

static int g_scheme_mode = -1;  // 0 auto, 1 light, 2 dark (host.ini)
static void menus_update(void);

// ---- files -------------------------------------------------------------------

static bool host_path(const WCHAR* name, WCHAR* out, size_t cap) {
  WCHAR dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  swprintf(out, cap, L"%ls\\ghostty%ls%ls", dir, name ? L"\\" : L"", name ? name : L"");
  return true;
}

static void write_text(const WCHAR* name, const char* text) {
  WCHAR dir[MAX_PATH + 32], path[MAX_PATH + 64];
  if (!host_path(NULL, dir, MAX_PATH + 32) || !host_path(name, path, MAX_PATH + 64)) return;
  CreateDirectoryW(dir, NULL);
  FILE* f = _wfopen(path, L"wb");
  if (!f) return;
  fputs(text, f);
  fclose(f);
}

// The value of `key = value` in a small file (UTF-8), or false.
static bool read_value(const WCHAR* name, const char* key, char* out, size_t cap) {
  WCHAR path[MAX_PATH + 64];
  if (!host_path(name, path, MAX_PATH + 64)) return false;
  FILE* f = _wfopen(path, L"rb");
  if (!f) return false;
  char line[512];
  bool found = false;
  size_t k = strlen(key);
  while (!found && fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, key, k) != 0) continue;
    p += k;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') continue;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strcspn(p, "\r\n");
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    found = true;
  }
  fclose(f);
  return found;
}

// ---- where themes are ----------------------------------------------------------

// The folders themes are found in: beside the exe (the package's
// share\ghostty\themes), GHOSTTY_RESOURCES_DIR's, the config folder's.
static int theme_dirs(WCHAR dirs[][MAX_PATH + 64]) {
  int n = 0;
  WCHAR exe[MAX_PATH];
  DWORD len = GetModuleFileNameW(NULL, exe, MAX_PATH);
  if (len && len < MAX_PATH) {
    WCHAR* slash = wcsrchr(exe, L'\\');
    if (slash) {
      *slash = 0;
      swprintf(dirs[n++], MAX_PATH + 64, L"%ls\\share\\ghostty\\themes", exe);
    }
  }
  WCHAR res[MAX_PATH];
  len = GetEnvironmentVariableW(L"GHOSTTY_RESOURCES_DIR", res, MAX_PATH);
  if (len && len < MAX_PATH) swprintf(dirs[n++], MAX_PATH + 64, L"%ls\\themes", res);
  if (host_path(L"themes", dirs[n], MAX_PATH + 64)) n++;
  return n;
}

static bool theme_exists(const char* name) {
  WCHAR* w = utf8_to_utf16(name, -1);
  if (!w) return false;
  bool found = false;
  if (wcschr(w, L'\\') || wcschr(w, L'/')) {
    found = GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;  // a path
  } else {
    WCHAR dirs[3][MAX_PATH + 64], path[2 * MAX_PATH + 80];
    int n = theme_dirs(dirs);
    for (int i = 0; i < n && !found; i++) {
      swprintf(path, 2 * MAX_PATH + 80, L"%ls\\%ls", dirs[i], w);
      found = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }
  }
  free(w);
  return found;
}

// A `theme` value's parts, as Ghostty reads it: "X", or
// "light:X,dark:Y" (either order). light/dark get the names ("" if none);
// a single theme is both.
static void theme_parts(const char* value, char* light, char* dark, size_t cap) {
  light[0] = dark[0] = 0;
  const char* p = value;
  bool any = false;
  while (*p) {
    size_t n = strcspn(p, ",");
    char part[256];
    size_t k = n < sizeof(part) - 1 ? n : sizeof(part) - 1;
    memcpy(part, p, k);
    part[k] = 0;
    char* q = part;
    while (*q == ' ' || *q == '\t') q++;
    size_t e = strlen(q);
    while (e && (q[e - 1] == ' ' || q[e - 1] == '\t')) q[--e] = 0;
    char* out = NULL;
    if (strncmp(q, "light:", 6) == 0) out = light, q += 6;
    else if (strncmp(q, "dark:", 5) == 0) out = dark, q += 5;
    while (*q == ' ' || *q == '\t') q++;
    if (out) {
      snprintf(out, cap, "%s", q);
      any = true;
    } else if (!any) {
      snprintf(light, cap, "%s", q);
      snprintf(dark, cap, "%s", q);
    }
    p += n;
    if (*p == ',') p++;
  }
}

// Every theme the value names is installed.
static bool theme_value_ok(const char* value) {
  char light[256], dark[256];
  theme_parts(value, light, dark, sizeof(light));
  return (light[0] || dark[0]) && (!light[0] || theme_exists(light)) &&
         (!dark[0] || theme_exists(dark));
}

// A light/dark pair of two themes (the quick switch changes something).
static bool theme_value_pair(const char* value) {
  char light[256], dark[256];
  theme_parts(value, light, dark, sizeof(light));
  return light[0] && dark[0] && strcmp(light, dark) != 0;
}

// The theme chosen in the menu ("" for none: his config's).
static void chosen_theme(char* out, size_t cap) {
  if (!read_value(L"theme.ghostty", "theme", out, cap)) out[0] = 0;
}

// For config_load: theme.ghostty's path (UTF-8, malloc'd) when it names a
// theme that is there; NULL otherwise (a missing one is said, the file kept).
char* theme_config_file(void) {
  char name[256];
  chosen_theme(name, sizeof(name));
  if (!name[0]) return NULL;
  if (!theme_value_ok(name)) {
    fprintf(stderr,
            "ghostty: theme.ghostty names a theme that is not installed: %s; "
            "using the config's (the file stays)\n",
            name);
    return NULL;
  }
  WCHAR path[MAX_PATH + 64];
  return host_path(L"theme.ghostty", path, MAX_PATH + 64) ? utf16_to_utf8(path, -1) : NULL;
}

// ---- Light / Dark / Auto -------------------------------------------------------

int scheme_mode(void) {
  if (g_scheme_mode < 0) {
    char v[32];
    g_scheme_mode = 0;
    if (read_value(L"host.ini", "scheme", v, sizeof(v))) {
      if (strcmp(v, "light") == 0) g_scheme_mode = 1;
      if (strcmp(v, "dark") == 0) g_scheme_mode = 2;
    }
  }
  return g_scheme_mode;
}

// host.ini, the host's own settings: one `key = value` set, the others
// (and the header) kept.
bool host_ini_get(const char* key, char* out, size_t cap) { return read_value(L"host.ini", key, out, cap); }

void host_ini_set(const char* key, const char* value) {
  WCHAR path[MAX_PATH + 64];
  Line t = {0};
  bool done = false;
  size_t k = strlen(key);
  if (host_path(L"host.ini", path, MAX_PATH + 64)) {
    FILE* f = _wfopen(path, L"rb");
    char line[512];
    while (f && fgets(line, sizeof(line), f)) {
      const char* p = line;
      while (*p == ' ' || *p == '\t') p++;
      const char* q = p + k;
      while (*q == ' ' || *q == '\t') q++;
      if (strncmp(p, key, k) == 0 && *q == '=') {
        if (!done) put(&t, "%s = %s\n", key, value);
        done = true;
      } else {
        put(&t, "%s", line);
      }
    }
    if (f) fclose(f);
  }
  if (!t.len) put(&t, "# Ghostty for Windows' own settings (the window menu writes them).\n");
  if (!done) put(&t, "%s = %s\n", key, value);
  write_text(L"host.ini", t.p);
  free(t.p);
}

static void set_scheme_mode(int mode) {
  static const char* const names[] = {"auto", "light", "dark"};
  g_scheme_mode = mode;
  host_ini_set("scheme", names[mode]);
  color_scheme_apply();
  menus_update();
  char ev[64];
  snprintf(ev, sizeof(ev), "{\"kind\":\"scheme\",\"mode\":\"%s\"}", names[mode]);
  test_event(ev);
}

// The theme in use: the one chosen here, else the last `theme =` of his
// config file (its includes are not followed: a guess good enough for a
// hint).
static void theme_in_use(char* out, size_t cap) {
  chosen_theme(out, cap);
  if (out[0] && theme_value_ok(out)) return;
  out[0] = 0;
  ghostty_string_s s = ghostty_config_open_path();
  WCHAR* path = s.ptr && s.len ? utf8_to_utf16(s.ptr, (int)s.len) : NULL;
  ghostty_string_free(s);
  if (!path) return;
  FILE* f = _wfopen(path, L"rb");
  free(path);
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "theme", 5) != 0) continue;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') continue;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
  }
  fclose(f);
}

// A line in the title bar for a few seconds (tabs.c paints it in the
// motto's place).
static WCHAR g_hint[200];
#define HINT_TIMER 21

const WCHAR* theme_hint(void) { return g_hint[0] ? g_hint : NULL; }

void hint_show(const WCHAR* text) {
  swprintf(g_hint, 200, L"%ls", text);
  for (Frame* f = g_frames; f; f = f->next) {
    SetTimer(f->hwnd, HINT_TIMER, 4000, NULL);
    update_title(f);  // the hint, with Windows' own caption
    InvalidateRect(f->hwnd, NULL, FALSE);
  }
  char* t = utf16_to_utf8(text, -1);
  Line e = {0};
  put(&e, "{\"kind\":\"hint\",\"text\":");
  put_string(&e, t ? t : "");
  put(&e, "}");
  test_event(e.p);
  free(e.p);
  free(t);
}

// WM_TIMER on a frame: the hint's time is up.
bool theme_timer(Frame* f, WPARAM id) {
  if (id != HINT_TIMER) return false;
  for (Frame* g = g_frames; g; g = g->next) {
    KillTimer(g->hwnd, HINT_TIMER);
    InvalidateRect(g->hwnd, NULL, FALSE);
  }
  g_hint[0] = 0;
  for (Frame* g = g_frames; g; g = g->next) update_title(g);
  return true;
}

// Ctrl+Shift+F12: Auto → Light → Dark → Auto. The mode is kept either way
// (for a pair chosen later); on a single theme the title bar says why
// nothing changed.
void scheme_cycle(void) {
  static const WCHAR* const names[] = {L"Auto", L"Light", L"Dark"};
  set_scheme_mode((scheme_mode() + 1) % 3);
  char theme[512];
  theme_in_use(theme, sizeof(theme));
  WCHAR text[200];
  if (theme_value_pair(theme))
    swprintf(text, 200, L"%ls", names[scheme_mode()]);
  else
    swprintf(text, 200, L"%ls: this theme has no light/dark pair (Theme menu: pick a pair)",
             names[scheme_mode()]);
  hint_show(text);
}

// ---- choosing a theme ------------------------------------------------------------

static void choose_theme(const char* name) {
  if (name) {
    char text[400];
    snprintf(text, sizeof(text),
             "# Written by Ghostty for Windows (the window menu's Theme).\n"
             "# Delete it, or choose \"Default (from my config)\", for your config's theme.\n"
             "theme = %s\n",
             name);
    write_text(L"theme.ghostty", text);
  } else {
    WCHAR path[MAX_PATH + 64];
    if (host_path(L"theme.ghostty", path, MAX_PATH + 64)) DeleteFileW(path);
  }
  config_reload(NULL, false);
  menus_update();
  Line e = {0};
  put(&e, "{\"kind\":\"theme\",\"theme\":");
  put_string(&e, name ? name : "");
  put(&e, "}");
  test_event(e.p);
  free(e.p);
}

// ---- the menu -------------------------------------------------------------------

// Each frame has its own copy of the window menu; MF_BYCOMMAND finds the
// items inside its Theme submenu.
void theme_menu_add(HMENU sys) {
  HMENU m = CreatePopupMenu();
  if (!m) return;
  AppendMenuW(m, MF_STRING, IDM_THEME_DEFAULT, L"Default (from my config)");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  for (int i = 0; i < NFAV; i++)
    if (theme_exists(kPairs[i].light) && theme_exists(kPairs[i].dark))
      AppendMenuW(m, MF_STRING, IDM_THEME_FAV + i * 0x10, kPairs[i].label);
  AppendMenuW(m, MF_STRING, IDM_THEME_MORE, L"More themes\u2026");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, IDM_SCHEME_AUTO, L"Auto (follow Windows)\tCtrl+Shift+F12");
  AppendMenuW(m, MF_STRING, IDM_SCHEME_LIGHT, L"Light");
  AppendMenuW(m, MF_STRING, IDM_SCHEME_DARK, L"Dark");
  AppendMenuW(sys, MF_POPUP, (UINT_PTR)m, L"Theme");
  theme_menu_update(sys);
}

// Check marks on the theme in use and on Auto / Light / Dark, before the
// menu shows; a theme from "More themes…" is named on that item.
void theme_menu_update(HMENU sys) {
  char name[256];
  chosen_theme(name, sizeof(name));
  bool fav = false;
  CheckMenuItem(sys, IDM_THEME_DEFAULT, MF_BYCOMMAND | (name[0] ? MF_UNCHECKED : MF_CHECKED));
  for (int i = 0; i < NFAV; i++) {
    char v[600];
    pair_value(i, v, sizeof(v));
    bool on = strcmp(name, v) == 0;
    fav = fav || on;
    CheckMenuItem(sys, IDM_THEME_FAV + i * 0x10, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
  }
  WCHAR more[320] = L"More themes\u2026";
  if (name[0] && !fav) {
    WCHAR* w = utf8_to_utf16(name, -1);
    if (w) swprintf(more, 320, L"More themes\u2026 (%ls)", w);
    free(w);
  }
  ModifyMenuW(sys, IDM_THEME_MORE, MF_BYCOMMAND | MF_STRING | (name[0] && !fav ? MF_CHECKED : 0),
              IDM_THEME_MORE, more);
  int mode = scheme_mode();
  CheckMenuRadioItem(sys, IDM_SCHEME_AUTO, IDM_SCHEME_DARK,
                     mode == 1 ? IDM_SCHEME_LIGHT : mode == 2 ? IDM_SCHEME_DARK : IDM_SCHEME_AUTO,
                     MF_BYCOMMAND);
}

// Every frame's marks, after a change.
static void menus_update(void) {
  for (Frame* f = g_frames; f; f = f->next) {
    HMENU m = GetSystemMenu(f->hwnd, FALSE);
    if (m) theme_menu_update(m);
  }
}

// ---- More themes…: a search list with a live preview -------------------------------

static HWND g_pick, g_pick_edit, g_pick_list;
static Frame* g_pick_frame;
static char** g_names;
static int g_nnames;
static WNDPROC g_edit_proc;

static int cmp_name(const void* a, const void* b) {
  return _stricmp(*(char* const*)a, *(char* const*)b);
}

// Every theme the folders hold, sorted, each name once.
static void load_names(void) {
  if (g_names) return;
  WCHAR dirs[3][MAX_PATH + 64], pattern[MAX_PATH + 80];
  int nd = theme_dirs(dirs), cap = 0;
  for (int d = 0; d < nd; d++) {
    swprintf(pattern, MAX_PATH + 80, L"%ls\\*", dirs[d]);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) continue;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      char* n = utf16_to_utf8(fd.cFileName, -1);
      if (!n) continue;
      if (g_nnames == cap) {
        cap = cap ? cap * 2 : 256;
        char** p = realloc(g_names, (size_t)cap * sizeof(char*));
        if (!p) {
          free(n);
          break;
        }
        g_names = p;
      }
      g_names[g_nnames++] = n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  if (g_nnames > 1) qsort(g_names, (size_t)g_nnames, sizeof(char*), cmp_name);
  int k = 0;
  for (int i = 0; i < g_nnames; i++)
    if (k == 0 || _stricmp(g_names[k - 1], g_names[i]) != 0)
      g_names[k++] = g_names[i];
    else
      free(g_names[i]);
  g_nnames = k;
}

// One window's panes with `theme` in place of the chosen one (NULL: back
// to the config as it is).
static void preview(const char* theme) {
  char* path8 = NULL;
  if (theme) {
    char text[300];
    snprintf(text, sizeof(text), "theme = %s\n", theme);
    write_text(L"theme-preview.ghostty", text);
    WCHAR path[MAX_PATH + 64];
    if (host_path(L"theme-preview.ghostty", path, MAX_PATH + 64)) path8 = utf16_to_utf8(path, -1);
    if (!path8) return;
  }
  ghostty_config_t c = config_load_with(path8);
  free(path8);
  if (!c) return;
  Frame* f = g_pick_frame;
  for (Frame* g = g_frames; g; g = g->next)
    if (g == f)
      for (int i = 0; i < f->ntabs; i++) {
        Window* panes[64];
        int n = tab_panes(f->tabs[i]->tab, panes, 64);
        for (int k = 0; k < n && k < 64; k++) ghostty_surface_update_config(panes[k]->surface, c);
      }
  ghostty_config_free(c);
}

static void fill_list(void) {
  WCHAR wq[128];
  GetWindowTextW(g_pick_edit, wq, 128);
  char* q = utf16_to_utf8(wq, -1);
  for (char* p = q; p && *p; p++) *p = (char)tolower((unsigned char)*p);
  SendMessageW(g_pick_list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(g_pick_list, LB_RESETCONTENT, 0, 0);
  for (int i = 0; i < g_nnames; i++) {
    char low[256];
    size_t n = strlen(g_names[i]);
    if (n >= sizeof(low)) n = sizeof(low) - 1;
    for (size_t k = 0; k < n; k++) low[k] = (char)tolower((unsigned char)g_names[i][k]);
    low[n] = 0;
    if (q && *q && !strstr(low, q)) continue;
    WCHAR* w = utf8_to_utf16(g_names[i], -1);
    if (!w) continue;
    LRESULT at = SendMessageW(g_pick_list, LB_ADDSTRING, 0, (LPARAM)w);
    SendMessageW(g_pick_list, LB_SETITEMDATA, (WPARAM)at, (LPARAM)i);
    free(w);
  }
  SendMessageW(g_pick_list, WM_SETREDRAW, TRUE, 0);
  free(q);
}

static const char* selected_name(void) {
  LRESULT sel = SendMessageW(g_pick_list, LB_GETCURSEL, 0, 0);
  if (sel == LB_ERR) return NULL;
  LRESULT i = SendMessageW(g_pick_list, LB_GETITEMDATA, (WPARAM)sel, 0);
  return i >= 0 && i < g_nnames ? g_names[i] : NULL;
}

static void select_row(int row) {
  LRESULT n = SendMessageW(g_pick_list, LB_GETCOUNT, 0, 0);
  if (n <= 0) return;
  if (row < 0) row = 0;
  if (row >= n) row = (int)n - 1;
  SendMessageW(g_pick_list, LB_SETCURSEL, (WPARAM)row, 0);
  preview(selected_name());
}

static void pick_close(bool keep) {
  if (!g_pick) return;
  const char* name = keep ? selected_name() : NULL;
  if (name)
    choose_theme(name);  // every window, and theme.ghostty
  else
    preview(NULL);  // back to what it was
  WCHAR path[MAX_PATH + 64];
  if (host_path(L"theme-preview.ghostty", path, MAX_PATH + 64)) DeleteFileW(path);
  DestroyWindow(g_pick);
}

static LRESULT CALLBACK pick_edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_KEYDOWN) {
    LRESULT cur = SendMessageW(g_pick_list, LB_GETCURSEL, 0, 0);
    int row = cur == LB_ERR ? -1 : (int)cur;
    switch (wp) {
      case VK_DOWN: select_row(row + 1); return 0;
      case VK_UP: select_row(row - 1); return 0;
      case VK_NEXT: select_row(row + 10); return 0;
      case VK_PRIOR: select_row(row - 10); return 0;
      case VK_RETURN: pick_close(true); return 0;
      case VK_ESCAPE: pick_close(false); return 0;
    }
  }
  if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;  // no beep
  return CallWindowProcW(g_edit_proc, h, msg, wp, lp);
}

static LRESULT CALLBACK pick_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
      if ((HWND)lp == g_pick_edit && HIWORD(wp) == EN_CHANGE) {
        fill_list();
        select_row(0);
      } else if ((HWND)lp == g_pick_list && HIWORD(wp) == LBN_SELCHANGE) {
        preview(selected_name());
      } else if ((HWND)lp == g_pick_list && HIWORD(wp) == LBN_DBLCLK) {
        pick_close(true);
      }
      return 0;
    case WM_SIZE: {
      int w = LOWORD(lp), hh = HIWORD(lp), eh = 26;
      MoveWindow(g_pick_edit, 8, 8, w - 16, eh, TRUE);
      MoveWindow(g_pick_list, 8, 8 + eh + 6, w - 16, hh - eh - 22, TRUE);
      return 0;
    }
    case WM_CLOSE:
      pick_close(false);
      return 0;
    case WM_DESTROY:
      g_pick = g_pick_edit = g_pick_list = NULL;
      g_pick_frame = NULL;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// "More themes…": a small window over f with a search box and the list.
void theme_picker_open(Frame* f) {
  if (g_pick) {
    SetForegroundWindow(g_pick);
    return;
  }
  load_names();
  static bool registered;
  if (!registered) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = pick_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"GhosttyThemePicker";
    registered = RegisterClassExW(&wc) != 0;
  }
  RECT fr;
  GetWindowRect(f->hwnd, &fr);
  int w = (int)(360 * f->scale), h = (int)(460 * f->scale);
  g_pick_frame = f;
  g_pick = CreateWindowExW(WS_EX_TOOLWINDOW, L"GhosttyThemePicker", L"Ghostty themes",
                           WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                           fr.left + ((fr.right - fr.left) - w) / 2, fr.top + 60, w, h, f->hwnd,
                           NULL, g_inst, NULL);
  if (!g_pick) return;
  HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  g_pick_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, g_pick, NULL,
                                g_inst, NULL);
  g_pick_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
                                    LBS_NOINTEGRALHEIGHT,
                                0, 0, 0, 0, g_pick, NULL, g_inst, NULL);
  SendMessageW(g_pick_edit, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(g_pick_list, WM_SETFONT, (WPARAM)font, TRUE);
  g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_pick_edit, GWLP_WNDPROC, (LONG_PTR)pick_edit_proc);
  fill_list();
  RECT rc;
  GetClientRect(g_pick, &rc);
  SendMessageW(g_pick, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
  ShowWindow(g_pick, SW_SHOW);
  SetFocus(g_pick_edit);
}

// ---- the window menu's theme items --------------------------------------------------

bool theme_command(HWND hwnd, WPARAM wp) {
  UINT id = (UINT)(wp & 0xFFF0);
  if (id == IDM_THEME_DEFAULT) {
    choose_theme(NULL);
    return true;
  }
  if (id == IDM_THEME_MORE) {
    Frame* f = (Frame*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (f) theme_picker_open(f);
    return true;
  }
  if (id == IDM_SCHEME_AUTO || id == IDM_SCHEME_LIGHT || id == IDM_SCHEME_DARK) {
    set_scheme_mode(id == IDM_SCHEME_LIGHT ? 1 : id == IDM_SCHEME_DARK ? 2 : 0);
    return true;
  }
  if (id >= IDM_THEME_FAV && id < IDM_THEME_FAV + NFAV * 0x10) {
    char v[600];
    pair_value((int)((id - IDM_THEME_FAV) / 0x10), v, sizeof(v));
    choose_theme(v);
    return true;
  }
  return false;
}
