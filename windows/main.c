#include "host.h"

ghostty_app_t g_app;
ghostty_config_t g_config;
HINSTANCE g_inst;
HWND g_wakeup_hwnd;  // message-only window for wakeups
volatile LONG g_wakeup_posted;  // a WM_GHOSTTY_WAKEUP is queued
Frame* g_frames;     // open windows
bool g_debug_keys;   // GHOSTTY_DEBUG_KEYS is set: trace key messages
DWORD g_main_tid;    // the thread that runs the message loop
TabBarMode g_tab_bar;     // window-show-tab-bar
bool g_new_tab_at_end;    // window-new-tab-position = end
COLORREF g_bg, g_fg;      // the terminal's colors, for the tab bar
double g_qpc_ms;          // milliseconds per QueryPerformanceCounter tick
const WCHAR* const kFrameClass = L"GhosttyWindow";
const WCHAR* const kTerminalClass = L"GhosttyTerminal";
static const WCHAR* const kWakeupClass = L"GhosttyWakeup";

// ---------------------------------------------------------------------------
// /buildinfo

// Text to where it can be read: stdout when it goes somewhere (a pipe, a
// file: `ghostty.exe /buildinfo > info.txt`), else the console this was
// started from (a GUI program has none of its own), else a message box.
void print_text(const char* s) {
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

// The config as Ghostty loads it: the default files, the theme chosen in
// the window menu (theme.c: theme.ghostty), the command line, the files
// they include; its problems on stderr. theme_file: NULL for the chosen
// one, "" for none, else that file (the picker's preview).
ghostty_config_t config_load_with(const char* theme_file) {
  ghostty_config_t c = ghostty_config_new();
  if (!c) return NULL;
  ghostty_config_load_default_files(c);
  char* chosen = theme_file ? NULL : theme_config_file();
  const char* t = theme_file ? theme_file : chosen;
  if (t && *t) ghostty_config_load_file(c, t);
  free(chosen);
  ghostty_config_load_cli_args(c);
  ghostty_config_load_recursive_files(c);
  ghostty_config_finalize(c);
  for (uint32_t i = 0; i < ghostty_config_diagnostics_count(c); i++) {
    ghostty_diagnostic_s d = ghostty_config_get_diagnostic(c, i);
    fprintf(stderr, "ghostty: config: %s\n", d.message);
  }
  return c;
}

static ghostty_config_t config_load(void) { return config_load_with(NULL); }

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

// Reload the config (Ctrl+Shift+, or the window menu): read the files
// again and hand the result to the app (every surface) or one surface; a
// soft reload hands the current one again. The core keeps a copy: this one
// is the host's to free. The settled config comes back in CONFIG_CHANGE.
//
// Never g_config itself: the core answers with CONFIG_CHANGE from inside
// the update, and config_changed replaces (frees) g_config there, while the
// update still reads the one it was given. A soft reload passes a clone.
void config_reload(ghostty_surface_t surface, bool soft) {
  ghostty_config_t c = soft ? ghostty_config_clone(g_config) : config_load();
  if (!c) return;
  if (surface)
    ghostty_surface_update_config(surface, c);
  else
    ghostty_app_update_config(g_app, c);
  ghostty_config_free(c);
}

// The app's config changed (a reload, or a light/dark theme following the
// system): the host's copy and what it reads from it (the tab bar's and
// the title bar's colors) follow.
void config_changed(ghostty_config_t config) {
  ghostty_config_t copy = ghostty_config_clone(config);
  if (!copy) return;
  ghostty_config_t old = g_config;
  g_config = copy;
  ghostty_config_free(old);
  read_host_config();
  for (Frame* f = g_frames; f; f = f->next) {
    layout(f);  // the title bar's colors (titlebar_update) and the bar
    InvalidateRect(f->hwnd, NULL, FALSE);
  }
}

// Open the config file (Ctrl+, or the window menu) in the editor Windows
// has for it, else Notepad; made first, with a hint, if it is missing.
void config_open(void) {
  ghostty_string_s s = ghostty_config_open_path();
  if (!s.ptr || !s.len) {
    ghostty_string_free(s);
    return;
  }
  WCHAR* path = utf8_to_utf16(s.ptr, (int)s.len);
  ghostty_string_free(s);
  if (!path) return;
  if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
    WCHAR dir[MAX_PATH];
    wcsncpy(dir, path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    for (WCHAR* p = dir + 3; *p; p++)  // each parent folder
      if (*p == L'\\' || *p == L'/') {
        WCHAR c = *p;
        *p = 0;
        CreateDirectoryW(dir, NULL);
        *p = c;
      }
    FILE* f = _wfopen(path, L"wb");
    if (f) {
      fputs("# Ghostty's config: one setting per line. For example:\n"
            "# theme = light:Builtin Light,dark:Builtin Dark\n"
            "# font-size = 12\n",
            f);
      fclose(f);
    }
  }
  if ((INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32)
    ShellExecuteW(NULL, L"open", L"notepad.exe", path, NULL, SW_SHOWNORMAL);
  free(path);
}

// Windows' app mode, light or dark (Settings > Personalization > Colors).
ghostty_color_scheme_e color_scheme_now(void) {
  int mode = scheme_mode();  // the window menu's Light / Dark / Auto
  if (mode == 1) return GHOSTTY_COLOR_SCHEME_LIGHT;
  if (mode == 2) return GHOSTTY_COLOR_SCHEME_DARK;
  DWORD light = 1, n = sizeof(light);
  if (RegGetValueW(HKEY_CURRENT_USER,
                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &n) != ERROR_SUCCESS)
    light = 1;  // Windows' own default
  return light ? GHOSTTY_COLOR_SCHEME_LIGHT : GHOSTTY_COLOR_SCHEME_DARK;
}

// Tell Ghostty the system's scheme, so light:/dark: themes follow it: at
// start and when Windows says it changed (WM_SETTINGCHANGE
// "ImmersiveColorSet").
void color_scheme_apply(void) {
  ghostty_color_scheme_e e = color_scheme_now();
  ghostty_app_set_color_scheme(g_app, e);
  for (Frame* f = g_frames; f; f = f->next)
    for (int i = 0; i < f->ntabs; i++) {
      Window* panes[64];
      int n = tab_panes(f->tabs[i]->tab, panes, 64);
      for (int k = 0; k < n && k < 64; k++)
        ghostty_surface_set_color_scheme(panes[k]->surface, e);
    }
}

static LRESULT CALLBACK wakeup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_GHOSTTY_TESTCTL) return testctl_ui(lp);
  if (msg == WM_TIMER && wp == 7) {  // the stats period (stats.c)
    stats_tick_all();
    return 0;
  }
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
#ifdef GHOSTTY_PRESENT_DEFAULT_SWAP
  // A dogfood build (HOST_PRESENT_DEFAULT=swap for windows/build.sh): the
  // renderer draws into the windows itself unless GHOSTTY_PRESENT says
  // otherwise (=readback: frames through the CPU). _putenv, not
  // SetEnvironmentVariable: libghostty reads the CRT's copy (getenv).
  if (!getenv("GHOSTTY_PRESENT")) _putenv("GHOSTTY_PRESENT=swap");
#endif
  if (stats_client_mode(argc, argv, &rc)) return rc;
  if (ctl_client_mode(argc, argv, &rc)) return rc;
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

  g_config = config_load();
  read_host_config();
  titlebar_choose();

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
  // CS_OWNDC: GHOSTTY_PRESENT=swap gives the window's DC to OpenGL for the
  // window's life.
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
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

  ghostty_app_set_color_scheme(g_app, color_scheme_now());
  start_reports();
  stats_start();
  testctl_start();  // only with GHOSTTY_TEST_CONTROL=1
  if (!new_window(NULL)) {
    glcheck_explain();  // why, in a message box (glcheck.c)
    return 1;
  }
  whatsnew_once(g_frames->hwnd);  // after an upgrade, once

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