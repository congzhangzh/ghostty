#include "host.h"

// ---------------------------------------------------------------------------
// About, What's new, and the motto
//
// The window menu (Alt+Space, or a right-click on the title bar) has "About
// Ghostty for Windows…" and "What's new…". What's new also opens once after
// an upgrade, when the build has notes (the release kit passes them,
// GHOSTTY_WHATSNEW; a test build has none): the last build seen is kept in
// %LOCALAPPDATA%\ghostty\whatsnew-seen.txt. GHOSTTY_WHATSNEW=off keeps it
// from opening by itself (tests: a modal box would take their typing).
//
// The motto is Ghostty's own; the family line after it comes from pys's
// about.py through the kit (GHOSTTY_FAMILY), so the two never drift.

#ifndef GHOSTTY_WHATSNEW
#define GHOSTTY_WHATSNEW ""
#endif
#ifndef GHOSTTY_FAMILY
#define GHOSTTY_FAMILY ""
#endif

#define GHOSTTY_MOTTO "one immersive terminal, many agent teams"
const WCHAR* const kMotto = WIDEN(GHOSTTY_MOTTO);

// GHOSTTY_SLOGAN=false hides the motto in the title bar (as pys's SLOGAN).
bool slogan_shown(void) {
  static int shown = -1;
  if (shown < 0) {
    const char* v = getenv("GHOSTTY_SLOGAN");
    shown = !(v && (strcmp(v, "false") == 0 || strcmp(v, "0") == 0));
  }
  return shown;
}

// The window menu's own items (below 0xF000, multiples of 16: WM_SYSCOMMAND
// keeps the low four bits for itself).
#define IDM_ABOUT 0x1010
#define IDM_WHATSNEW 0x1020
#define IDM_OPEN_CONFIG 0x1030
#define IDM_RELOAD_CONFIG 0x1040

void about_menu_add(HWND hwnd) {
  HMENU m = GetSystemMenu(hwnd, FALSE);
  if (!m) return;
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, IDM_ABOUT, L"About Ghostty for Windows…");
  AppendMenuW(m, MF_STRING, IDM_WHATSNEW, L"What's new…");
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  AppendMenuW(m, MF_STRING, IDM_OPEN_CONFIG, L"Open config…");
  AppendMenuW(m, MF_STRING, IDM_RELOAD_CONFIG, L"Reload config");
}

// Microsoft's ConPTY is used when its files are beside ghostty.exe (and
// GHOSTTY_CONPTY is not "inbox"), as libghostty decides.
static bool conpty_side_loaded(void) {
  const char* v = getenv("GHOSTTY_CONPTY");
  if (v && strcmp(v, "inbox") == 0) return false;
  WCHAR dir[MAX_PATH], path[MAX_PATH + 32];
  DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  WCHAR* slash = wcsrchr(dir, L'\\');
  if (!slash) return false;
  slash[1] = 0;
  swprintf(path, MAX_PATH + 32, L"%lsconpty.dll", dir);
  if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
  swprintf(path, MAX_PATH + 32, L"%lsOpenConsole.exe", dir);
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// The lines of BUILDINFO that say which build this is.
static void build_lines(char* out, size_t cap) {
  out[0] = 0;
  size_t used = 0;
  const char* b = GHOSTTY_BUILDINFO;
  while (*b && used + 1 < cap) {
    size_t len = strcspn(b, "\n");
    bool keep = strncmp(b, "release:", 8) == 0 ||
                strncmp(b, "version:", 8) == 0 ||
                strncmp(b, "source:", 7) == 0 ||
                strncmp(b, "family:", 7) == 0;
    if (keep && used + len + 2 < cap) {
      memcpy(out + used, b, len);
      used += len;
      out[used++] = '\n';
      out[used] = 0;
    }
    b += len;
    if (*b == '\n') b++;
  }
}

static void message(HWND owner, const char* title8, const char* text8) {
  WCHAR* title = utf8_to_utf16(title8, -1);
  WCHAR* text = utf8_to_utf16(text8, -1);
  if (title && text) MessageBoxW(owner, text, title, MB_OK | MB_ICONINFORMATION);
  free(title);
  free(text);
}

void show_about(HWND owner) {
  char builds[1024];
  build_lines(builds, sizeof(builds));
  const char* family = GHOSTTY_FAMILY;
  char text[4096];
  snprintf(text, sizeof(text),
           "Ghostty for Windows %s — %s%s%s\n\n"
           "Part of the family: pys (sessions) · Ghostty (immersive "
           "terminal) · Brahman (task flow)\n\n"
           "An unofficial build of Ghostty (ghostty.org)\n\n"
           "%s\n"
           "Releases: https://github.com/congzhangzh/ghostty-release-kit/releases\n"
           "Ghostty: https://ghostty.org\n"
           "%s",
           GHOSTTY_BUILD_TAG, GHOSTTY_MOTTO, *family ? " · " : "", family,
           builds,
           conpty_side_loaded()
               ? "\nIncludes Microsoft's ConPTY (conpty.dll, OpenConsole.exe), "
                 "under its own license: see THIRD-PARTY-NOTICES.txt beside "
                 "ghostty.exe.\n"
               : "");
  message(owner, "About Ghostty for Windows", text);
}

void show_whatsnew(HWND owner) {
  char title[128];
  snprintf(title, sizeof(title), "What's new in Ghostty for Windows %s",
           GHOSTTY_BUILD_TAG);
  const char* notes = GHOSTTY_WHATSNEW;
  message(owner, title, *notes ? notes : "Nothing new in this test build.");
}

bool about_command(HWND hwnd, WPARAM wp) {
  switch (wp & 0xFFF0) {
    case IDM_ABOUT: show_about(hwnd); return true;
    case IDM_WHATSNEW: show_whatsnew(hwnd); return true;
    case IDM_OPEN_CONFIG: config_open(); return true;
    case IDM_RELOAD_CONFIG: config_reload(NULL, false); return true;
  }
  return false;
}

// Once after an upgrade: What's new, if this build has notes and another
// build was seen last (or none). The build is then recorded either way.
void whatsnew_once(HWND owner) {
  const char* v = getenv("GHOSTTY_WHATSNEW");
  WCHAR dir[MAX_PATH], path[MAX_PATH + 32];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  swprintf(path, MAX_PATH + 32, L"%ls\\ghostty", dir);
  CreateDirectoryW(path, NULL);
  swprintf(path, MAX_PATH + 32, L"%ls\\ghostty\\whatsnew-seen.txt", dir);
  char seen[128] = {0};
  FILE* in = _wfopen(path, L"rb");
  if (in) {
    size_t got = fread(seen, 1, sizeof(seen) - 1, in);
    seen[got] = 0;
    fclose(in);
  }
  if (strcmp(seen, GHOSTTY_BUILD_TAG) == 0) return;
  FILE* out = _wfopen(path, L"wb");
  if (out) {
    fputs(GHOSTTY_BUILD_TAG, out);
    fclose(out);
  }
  if (v && strcmp(v, "off") == 0) return;
  if (*GHOSTTY_WHATSNEW) show_whatsnew(owner);
}
