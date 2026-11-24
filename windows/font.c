#include "host.h"

// ---------------------------------------------------------------------------
// Fonts: the window menu's "Font ▸", as its "Theme ▸" (theme.c)
//
// - the main font: "Default (from my config)", the monospace fonts most
//   people know when installed (Cascadia Mono, Consolas, JetBrains Mono,
//   Fira Code …), and "More fonts…": every installed monospace font, a
//   search list with a live preview on that window (Enter keeps, Esc
//   reverts);
// - "Chinese / Japanese ▸": the font for Han, kana and the CJK symbols
//   (Microsoft YaHei, DengXian, SimSun, Microsoft JhengHei, Yu Gothic,
//   Meiryo, when installed), or "Automatic", the fallback by display
//   language (discovery.zig);
// - the size (the config's) and "Reset size" (the window's panes).
//
// His config is never rewritten. The choice goes in
// %LOCALAPPDATA%\ghostty\font.ghostty, loaded after his files like
// theme.ghostty (main.c): `font-family = ""` then the font (his list is
// replaced, not added to), and `font-codepoint-map = <CJK ranges>=<font>`.
// Both "Default"/"Automatic": the file goes.
//
// The fonts are GDI's (EnumFontFamiliesEx): fixed pitch, TrueType or
// OpenType (Ghostty's discovery opens .ttf/.ttc/.otf), ASCII names only
// (GDI may give a localized name, "新宋体"; Ghostty matches the font's own
// family name, the English one).

#define IDM_FONT_DEFAULT 0x2000
#define IDM_FONT_MORE 0x2010
#define IDM_FONT_RESET_SIZE 0x2020
#define IDM_FONT_SIZE 0x2030  // shown only (grayed)
#define IDM_CJK_AUTO 0x2040
#define IDM_CJK 0x2050       // + 0x10 per CJK font
#define IDM_FONT_SHORT 0x2100  // + 0x10 per short-list font

static const char* const kShort[] = {
    "Cascadia Mono",   "Cascadia Code", "Consolas",         "JetBrains Mono", "Fira Code",
    "Source Code Pro", "Hack",          "DejaVu Sans Mono", "Lucida Console", "Courier New",
};
#define NSHORT ((int)(sizeof(kShort) / sizeof(kShort[0])))

static const struct {
  const char* family;  // the font's own (English) family name
  const WCHAR* label;
  const WCHAR* file;   // in %WINDIR%\Fonts
} kCjk[] = {
    {"Microsoft YaHei", L"Microsoft YaHei 微软雅黑", L"msyh.ttc"},
    {"DengXian", L"DengXian 等线", L"Deng.ttf"},
    {"SimSun", L"SimSun 宋体", L"simsun.ttc"},
    {"Microsoft JhengHei", L"Microsoft JhengHei 微軟正黑體", L"msjh.ttc"},
    {"Yu Gothic", L"Yu Gothic 游ゴシック", L"YuGothR.ttc"},
    {"Meiryo", L"Meiryo メイリオ", L"meiryo.ttc"},
};
#define NCJK ((int)(sizeof(kCjk) / sizeof(kCjk[0])))

// Han and its radicals, the CJK symbols, kana, bopomofo, the compatibility
// ideographs, the full-width forms, and the supplementary ideographs.
static const char kCjkRanges[] =
    "U+2E80-U+2FDF,U+3000-U+30FF,U+3100-U+312F,U+31C0-U+31FF,U+3400-U+4DBF,U+4E00-U+9FFF,"
    "U+F900-U+FAFF,U+FE30-U+FE4F,U+FF00-U+FFEF,U+20000-U+2FA1F";

// ---- the installed fonts ---------------------------------------------------------

static char** g_fonts;  // every monospace family, sorted, each once
static int g_nfonts, g_capfonts;

static int CALLBACK enum_proc(const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD type, LPARAM lp) {
  (void)lp;
  const NEWTEXTMETRICW* ntm = (const NEWTEXTMETRICW*)tm;
  bool outline = (type & TRUETYPE_FONTTYPE) || (ntm->ntmFlags & (NTM_PS_OPENTYPE | NTM_TT_OPENTYPE));
  if (!outline || (lf->lfPitchAndFamily & 3) != FIXED_PITCH || lf->lfFaceName[0] == L'@') return 1;
  for (const WCHAR* p = lf->lfFaceName; *p; p++)
    if (*p > 0x7e) return 1;  // a localized name: not one discovery matches
  char* n = utf16_to_utf8(lf->lfFaceName, -1);
  if (!n) return 1;
  for (int i = 0; i < g_nfonts; i++)
    if (_stricmp(g_fonts[i], n) == 0) {
      free(n);
      return 1;
    }
  if (g_nfonts == g_capfonts) {
    int cap = g_capfonts ? g_capfonts * 2 : 64;
    char** p = realloc(g_fonts, (size_t)cap * sizeof(char*));
    if (!p) {
      free(n);
      return 0;
    }
    g_fonts = p;
    g_capfonts = cap;
  }
  g_fonts[g_nfonts++] = n;
  return 1;
}

static int cmp_font(const void* a, const void* b) { return _stricmp(*(char* const*)a, *(char* const*)b); }

static void load_fonts(void) {
  if (g_fonts) return;
  HDC dc = GetDC(NULL);
  LOGFONTW lf = {0};
  lf.lfCharSet = DEFAULT_CHARSET;
  EnumFontFamiliesExW(dc, &lf, enum_proc, 0, 0);
  ReleaseDC(NULL, dc);
  if (g_nfonts > 1) qsort(g_fonts, (size_t)g_nfonts, sizeof(char*), cmp_font);
}

static bool font_installed(const char* family) {
  load_fonts();
  for (int i = 0; i < g_nfonts; i++)
    if (_stricmp(g_fonts[i], family) == 0) return true;
  return false;
}

// A CJK font: its file in the Windows fonts folder, or GDI knows the family
// (any pitch).
static int CALLBACK any_proc(const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD type, LPARAM lp) {
  (void)lf, (void)tm, (void)type;
  *(bool*)lp = true;
  return 0;
}

static bool cjk_installed(int i) {
  WCHAR dir[MAX_PATH], path[MAX_PATH + 64];
  UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
  if (n && n < MAX_PATH) {
    swprintf(path, MAX_PATH + 64, L"%ls\\Fonts\\%ls", dir, kCjk[i].file);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return true;
  }
  bool found = false;
  HDC dc = GetDC(NULL);
  LOGFONTW lf = {0};
  lf.lfCharSet = DEFAULT_CHARSET;
  MultiByteToWideChar(CP_UTF8, 0, kCjk[i].family, -1, lf.lfFaceName, LF_FACESIZE);
  EnumFontFamiliesExW(dc, &lf, any_proc, (LPARAM)&found, 0);
  ReleaseDC(NULL, dc);
  return found;
}

// ---- font.ghostty -----------------------------------------------------------------

// What the file chose: the main font (the last non-empty font-family) and
// the CJK one (after the codepoint map's last '='); "" for none.
static void font_chosen(char* main_font, char* cjk, size_t cap) {
  main_font[0] = cjk[0] = 0;
  WCHAR path[MAX_PATH + 64];
  if (!host_path(L"font.ghostty", path, MAX_PATH + 64)) return;
  FILE* f = _wfopen(path, L"rb");
  if (!f) return;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    char* v = strchr(line, '=');
    if (!v) continue;
    bool family = strncmp(line, "font-family", 11) == 0 && (line[11] == ' ' || line[11] == '=');
    bool map = strncmp(line, "font-codepoint-map", 18) == 0;
    if (!family && !map) continue;
    if (map) v = strrchr(line, '=');
    v++;
    while (*v == ' ') v++;
    size_t n = strlen(v);
    while (n && v[n - 1] == ' ') n--;
    v[n] = 0;
    if (family && (n == 0 || strcmp(v, "\"\"") == 0)) continue;
    snprintf(family ? main_font : cjk, cap, "%s", v);
  }
  fclose(f);
}

char* font_config_file(void) {
  WCHAR path[MAX_PATH + 64];
  if (!host_path(L"font.ghostty", path, MAX_PATH + 64) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    return NULL;
  return utf16_to_utf8(path, -1);
}

// The lines for a main font and a CJK one (either NULL or "": none).
static void font_lines(const char* main_font, const char* cjk, char* out, size_t cap) {
  out[0] = 0;
  size_t used = 0;
  if (main_font && *main_font)
    used += (size_t)snprintf(out, cap, "font-family = \"\"\nfont-family = %s\n", main_font);
  if (cjk && *cjk && used < cap) snprintf(out + used, cap - used, "font-codepoint-map = %s=%s\n", kCjkRanges, cjk);
}

static void font_write(const char* main_font, const char* cjk) {
  char lines[1200];
  font_lines(main_font, cjk, lines, sizeof(lines));
  if (lines[0]) {
    char text[1500];
    snprintf(text, sizeof(text),
             "# Written by Ghostty for Windows (the window menu's Font).\n"
             "# Delete it, or choose \"Default (from my config)\", for your config's fonts.\n"
             "%s",
             lines);
    write_text(L"font.ghostty", text);
  } else {
    WCHAR path[MAX_PATH + 64];
    if (host_path(L"font.ghostty", path, MAX_PATH + 64)) DeleteFileW(path);
  }
  config_reload(NULL, false);
  menus_update();
  Line e = {0};
  put(&e, "{\"kind\":\"font\",\"family\":");
  put_string(&e, main_font ? main_font : "");
  put(&e, ",\"cjk\":");
  put_string(&e, cjk ? cjk : "");
  put(&e, "}");
  test_event(e.p);
  free(e.p);
}

static void choose_main(const char* family) {
  char m[256], c[256];
  font_chosen(m, c, sizeof(m));
  font_write(family, c);
}

static void choose_cjk(const char* family) {
  char m[256], c[256];
  font_chosen(m, c, sizeof(m));
  font_write(m, family);
}

// ---- More fonts…: theme.c's picker with the fonts --------------------------------

static Frame* g_more_frame;

static void preview_main(const char* family) {
  if (!family) {
    config_preview(g_more_frame, NULL);
    return;
  }
  char text[400];
  snprintf(text, sizeof(text), "font-family = \"\"\nfont-family = %s\n", family);
  config_preview(g_more_frame, text);
}

static void more_fonts(Frame* f) {
  load_fonts();
  g_more_frame = f;
  picker_open(f, L"Ghostty fonts", g_fonts, g_nfonts, preview_main, choose_main);
}

// ---- the menu ---------------------------------------------------------------------

void font_menu_add(HMENU sys) {
  HMENU m = CreatePopupMenu(), cjk = CreatePopupMenu();
  if (!m || !cjk) return;
  AppendMenuW(m, MF_STRING, IDM_FONT_DEFAULT, tr(L"Default (from my config)"));
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  for (int i = 0; i < NSHORT; i++)
    if (font_installed(kShort[i])) {
      WCHAR* w = utf8_to_utf16(kShort[i], -1);
      if (w) AppendMenuW(m, MF_STRING, IDM_FONT_SHORT + i * 0x10, w);
      free(w);
    }
  AppendMenuW(m, MF_STRING, IDM_FONT_MORE, tr(L"More fonts…"));
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(cjk, MF_STRING, IDM_CJK_AUTO, tr(L"Automatic (by display language)"));
  AppendMenuW(cjk, MF_SEPARATOR, 0, NULL);
  for (int i = 0; i < NCJK; i++)
    if (cjk_installed(i)) AppendMenuW(cjk, MF_STRING, IDM_CJK + i * 0x10, kCjk[i].label);
  AppendMenuW(m, MF_POPUP, (UINT_PTR)cjk, tr(L"Chinese / Japanese"));
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_FONT_SIZE, L"");
  AppendMenuW(m, MF_STRING, IDM_FONT_RESET_SIZE, tr(L"Reset size"));
  AppendMenuW(sys, MF_POPUP, (UINT_PTR)m, tr(L"Font"));
  font_menu_update(sys);
}

// Check marks on what font.ghostty chose; the size from the config.
void font_menu_update(HMENU sys) {
  char main_font[256], cjk[256];
  font_chosen(main_font, cjk, sizeof(main_font));
  bool listed = false;
  CheckMenuItem(sys, IDM_FONT_DEFAULT, MF_BYCOMMAND | (main_font[0] ? MF_UNCHECKED : MF_CHECKED));
  for (int i = 0; i < NSHORT; i++) {
    bool on = main_font[0] && _stricmp(main_font, kShort[i]) == 0;
    listed = listed || on;
    CheckMenuItem(sys, IDM_FONT_SHORT + i * 0x10, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
  }
  WCHAR more[320];
  swprintf(more, 320, L"%ls", tr(L"More fonts…"));
  if (main_font[0] && !listed) {
    WCHAR* w = utf8_to_utf16(main_font, -1);
    if (w) swprintf(more, 320, L"%ls (%ls)", tr(L"More fonts…"), w);
    free(w);
  }
  ModifyMenuW(sys, IDM_FONT_MORE, MF_BYCOMMAND | MF_STRING | (main_font[0] && !listed ? MF_CHECKED : 0),
              IDM_FONT_MORE, more);
  CheckMenuItem(sys, IDM_CJK_AUTO, MF_BYCOMMAND | (cjk[0] ? MF_UNCHECKED : MF_CHECKED));
  for (int i = 0; i < NCJK; i++)
    CheckMenuItem(sys, IDM_CJK + i * 0x10,
                  MF_BYCOMMAND | (cjk[0] && _stricmp(cjk, kCjk[i].family) == 0 ? MF_CHECKED : MF_UNCHECKED));
  float pt = 0;
  static const char k[] = "font-size";
  WCHAR size[96];
  if (g_config && ghostty_config_get(g_config, &pt, k, strlen(k)) && pt > 0)
    swprintf(size, 96, L"%ls %g pt\tCtrl+= / Ctrl+-", tr(L"Size"), (double)pt);
  else
    swprintf(size, 96, L"%ls", tr(L"Size"));
  ModifyMenuW(sys, IDM_FONT_SIZE, MF_BYCOMMAND | MF_STRING | MF_GRAYED, IDM_FONT_SIZE, size);
}

bool font_command(HWND hwnd, WPARAM wp) {
  UINT id = (UINT)(wp & 0xFFF0);
  Frame* f = (Frame*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (id == IDM_FONT_DEFAULT) {
    choose_main(NULL);
    return true;
  }
  if (id == IDM_FONT_MORE) {
    if (f) more_fonts(f);
    return true;
  }
  if (id == IDM_FONT_RESET_SIZE) {  // the window's panes
    static const char a[] = "reset_font_size";
    if (f)
      for (int i = 0; i < f->ntabs; i++) {
        Window* panes[64];
        int n = tab_panes(f->tabs[i]->tab, panes, 64);
        for (int k = 0; k < n && k < 64; k++) ghostty_surface_binding_action(panes[k]->surface, a, strlen(a));
      }
    test_event("{\"kind\":\"font\",\"what\":\"reset_size\"}");
    return true;
  }
  if (id == IDM_CJK_AUTO) {
    choose_cjk(NULL);
    return true;
  }
  if (id >= IDM_CJK && id < IDM_CJK + NCJK * 0x10) {
    choose_cjk(kCjk[(id - IDM_CJK) / 0x10].family);
    return true;
  }
  if (id >= IDM_FONT_SHORT && id < IDM_FONT_SHORT + NSHORT * 0x10) {
    choose_main(kShort[(id - IDM_FONT_SHORT) / 0x10]);
    return true;
  }
  return false;
}
