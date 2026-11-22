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

static LRESULT CALLBACK wakeup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
  if (stats_client_mode(argc, argv, &rc)) return rc;
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
  stats_start();
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