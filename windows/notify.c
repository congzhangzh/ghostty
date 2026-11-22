// Desktop notifications (OSC 9, OSC 777, OSC 99) and taskbar progress (OSC 9;4).
//
// A notification is a balloon from a notification-area icon that exists only while the
// balloon is up. Windows 10 and 11 show a balloon as a toast and keep it in the notification
// center, with no AppUserModelID or Start-menu shortcut needed (a WinRT toast needs both, so
// it waits for an installer). It is not shown when it comes from the selected tab of the
// foreground window, as on macOS (shown unless its window is the key window and its surface is
// focused). A click on it brings that window forward and selects that tab. The core already
// drops notifications when `desktop-notifications = false`.
//
// Progress goes to the window's taskbar button (ITaskbarList3): the selected tab's progress if
// it reports one, else the most severe of the window's tabs (error, paused, normal,
// indeterminate). A state is kept per tab, so bursts collapse to the last one.

#include "host.h"

#include <shobjidl.h>

#define WM_NOTIFY_ICON (WM_APP + 41)
#define NOTIFY_ICON_ID 1

// CLSID_TaskbarList and IID_ITaskbarList3, declared here so the host needs no uuid library.
static const CLSID kClsidTaskbarList = {0x56fdf344, 0xfd6d, 0x11d0, {0x95, 0x8a, 0x00, 0x60, 0x97, 0xc9, 0xa0, 0x90}};
static const IID kIidTaskbarList3 = {0xea1afb91, 0x9e28, 0x4b86, {0x90, 0xe9, 0x9e, 0x9f, 0x8a, 0x5e, 0xef, 0xaf}};

static HWND g_notify_hwnd;  // receives the icon's messages
static bool g_icon_shown;
static Window* g_toast_tab;  // the tab the shown balloon is from, NULL once it is closed

// ---------------------------------------------------------------------------
// Where a tab is

static bool find_tab(Window* w, Frame** fo, int* io) {
  for (Frame* f = g_frames; f; f = f->next)
    for (int i = 0; i < f->ntabs; i++)
      if (f->tabs[i] == w) {
        *fo = f;
        *io = i;
        return true;
      }
  return false;
}

// ---------------------------------------------------------------------------
// The notification-area icon and its balloon

static void icon_remove(void) {
  if (!g_icon_shown) return;
  NOTIFYICONDATAW nid = {.cbSize = sizeof nid, .hWnd = g_notify_hwnd, .uID = NOTIFY_ICON_ID};
  Shell_NotifyIconW(NIM_DELETE, &nid);
  g_icon_shown = false;
}

static LRESULT CALLBACK notify_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NOTIFY_ICON) {
    // NOTIFYICON_VERSION_4: the event is in LOWORD(lParam).
    switch (LOWORD(lp)) {
      case NIN_BALLOONUSERCLICK: {
        Frame* f;
        int i;
        if (g_toast_tab && find_tab(g_toast_tab, &f, &i)) {
          fprintf(stderr, "ghostty: notify: clicked, showing its window and tab %d\n", i);
          if (IsIconic(f->hwnd)) ShowWindow(f->hwnd, SW_RESTORE);
          SetForegroundWindow(f->hwnd);
          select_tab(f, i);
        } else {
          fprintf(stderr, "ghostty: notify: clicked, but its tab is closed\n");
        }
        g_toast_tab = NULL;
        icon_remove();
        return 0;
      }
      case NIN_BALLOONTIMEOUT:
      case NIN_BALLOONHIDE:
        g_toast_tab = NULL;
        icon_remove();
        return 0;
    }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool notify_window(void) {
  if (g_notify_hwnd) return true;
  WNDCLASSW wc = {.lpfnWndProc = notify_proc, .hInstance = g_inst, .lpszClassName = L"GhosttyNotify"};
  RegisterClassW(&wc);
  // A hidden top-level window rather than a message-only one: the shell broadcasts to
  // top-level windows, and the icon's messages come from it.
  g_notify_hwnd = CreateWindowExW(0, L"GhosttyNotify", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
  if (!g_notify_hwnd) fprintf(stderr, "ghostty: notify: no window for the notification icon (error %lu)\n", GetLastError());
  return g_notify_hwnd != NULL;
}

// Copies UTF-8 `s` into `dst` (n WCHARs), cut to fit.
static void copy_utf8(WCHAR* dst, size_t n, const char* s) {
  dst[0] = 0;
  if (!s || !*s) return;
  WCHAR* w = utf8_to_utf16(s, -1);
  if (!w) return;
  wcsncpy(dst, w, n - 1);
  dst[n - 1] = 0;
  free(w);
}

void notify_toast(Window* w, const char* title, const char* body) {
  if (!w) return;
  Frame* f;
  int i;
  if (!find_tab(w, &f, &i)) return;
  bool seen = GetForegroundWindow() == f->hwnd && f->active == i;
  fprintf(stderr, "ghostty: notify: \"%s\" / \"%s\" %s\n", title ? title : "", body ? body : "",
          seen ? "not shown: its tab is the selected tab of the foreground window" : "shown");
  if (seen || !notify_window()) return;

  NOTIFYICONDATAW nid = {
      .cbSize = sizeof nid,
      .hWnd = g_notify_hwnd,
      .uID = NOTIFY_ICON_ID,
      .uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO,
      .uCallbackMessage = WM_NOTIFY_ICON,
      .dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON,
  };
  nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
  if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
  nid.hBalloonIcon = nid.hIcon;
  wcscpy(nid.szTip, L"Ghostty");
  copy_utf8(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
  copy_utf8(nid.szInfo, ARRAYSIZE(nid.szInfo), body);
  // The tab's title under the body, as macOS shows it as the subtitle.
  if (w->title && *w->title) {
    size_t len = wcslen(nid.szInfo), room = ARRAYSIZE(nid.szInfo) - 1;
    if (len && len + 2 < room) wcscat(nid.szInfo, L"\n"), len++;
    if (len < room) wcsncat(nid.szInfo, w->title, room - len);
  }
  if (!nid.szInfoTitle[0] && !nid.szInfo[0]) wcscpy(nid.szInfo, L" ");

  if (!g_icon_shown) {
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
      fprintf(stderr, "ghostty: notify: Shell_NotifyIcon(NIM_ADD) failed (error %lu)\n", GetLastError());
      return;
    }
    g_icon_shown = true;
    NOTIFYICONDATAW v = {.cbSize = sizeof v, .hWnd = g_notify_hwnd, .uID = NOTIFY_ICON_ID};
    v.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &v);
  } else if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {  // a newer balloon replaces the shown one
    fprintf(stderr, "ghostty: notify: Shell_NotifyIcon(NIM_MODIFY) failed (error %lu)\n", GetLastError());
    return;
  }
  g_toast_tab = w;
}

// ---------------------------------------------------------------------------
// Taskbar progress

typedef struct {
  Window* tab;
  ghostty_action_progress_report_state_e state;
  int8_t value;  // 0-100, or -1
} TabProgress;

static TabProgress* g_progress;
static int g_nprogress, g_capprogress;

typedef struct {
  HWND hwnd;
  TBPFLAG flag;
  int value;
} Shown;  // what a window's taskbar button shows now, so only changes are logged

static Shown* g_shown;
static int g_nshown, g_capshown;

static ITaskbarList3* g_taskbar;
static bool g_taskbar_failed;

static ITaskbarList3* taskbar(void) {
  if (g_taskbar || g_taskbar_failed) return g_taskbar;
  HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);  // the UI thread; S_FALSE or RPC_E_CHANGED_MODE are fine
  (void)hr;
  hr = CoCreateInstance(&kClsidTaskbarList, NULL, CLSCTX_INPROC_SERVER, &kIidTaskbarList3, (void**)&g_taskbar);
  if (SUCCEEDED(hr)) hr = g_taskbar->lpVtbl->HrInit(g_taskbar);
  if (FAILED(hr)) {
    fprintf(stderr, "ghostty: notify: no taskbar progress (ITaskbarList3: HRESULT 0x%08lx)\n", (unsigned long)hr);
    if (g_taskbar) g_taskbar->lpVtbl->Release(g_taskbar);
    g_taskbar = NULL;
    g_taskbar_failed = true;
  }
  return g_taskbar;
}

static TabProgress* progress_of(Window* w) {
  for (int i = 0; i < g_nprogress; i++)
    if (g_progress[i].tab == w) return &g_progress[i];
  return NULL;
}

static int severity(ghostty_action_progress_report_state_e s) {
  switch (s) {
    case GHOSTTY_PROGRESS_STATE_ERROR: return 4;
    case GHOSTTY_PROGRESS_STATE_PAUSE: return 3;
    case GHOSTTY_PROGRESS_STATE_SET: return 2;
    case GHOSTTY_PROGRESS_STATE_INDETERMINATE: return 1;
    default: return 0;
  }
}

static TBPFLAG flag_of(ghostty_action_progress_report_state_e s) {
  switch (s) {
    case GHOSTTY_PROGRESS_STATE_SET: return TBPF_NORMAL;
    case GHOSTTY_PROGRESS_STATE_ERROR: return TBPF_ERROR;
    case GHOSTTY_PROGRESS_STATE_INDETERMINATE: return TBPF_INDETERMINATE;
    case GHOSTTY_PROGRESS_STATE_PAUSE: return TBPF_PAUSED;
    default: return TBPF_NOPROGRESS;
  }
}

// Shows on `f`'s taskbar button the progress its tabs report.
static void apply(Frame* f) {
  if (!f || !f->hwnd || f->destroying) return;
  TabProgress* pick = NULL;
  if (f->active >= 0 && f->active < f->ntabs) {
    TabProgress* p = progress_of(f->tabs[f->active]);
    if (p && p->state != GHOSTTY_PROGRESS_STATE_REMOVE) pick = p;
  }
  if (!pick)
    for (int i = 0; i < f->ntabs; i++) {
      TabProgress* p = progress_of(f->tabs[i]);
      if (p && severity(p->state) > (pick ? severity(pick->state) : 0)) pick = p;
    }
  TBPFLAG flag = pick ? flag_of(pick->state) : TBPF_NOPROGRESS;
  int value = pick && flag != TBPF_INDETERMINATE && flag != TBPF_NOPROGRESS ? pick->value : -1;

  Shown* s = NULL;
  for (int i = 0; i < g_nshown; i++)
    if (g_shown[i].hwnd == f->hwnd) s = &g_shown[i];
  if (s && s->flag == flag && s->value == value) return;
  if (!s && flag == TBPF_NOPROGRESS) return;  // nothing shown, nothing to show

  ITaskbarList3* tb = taskbar();
  if (!tb) return;
  HRESULT hr = tb->lpVtbl->SetProgressState(tb, f->hwnd, flag);
  if (SUCCEEDED(hr) && value >= 0) hr = tb->lpVtbl->SetProgressValue(tb, f->hwnd, (ULONGLONG)value, 100);
  char num[8] = "";
  if (value >= 0) snprintf(num, sizeof num, " %d", value);
  fprintf(stderr, "ghostty: notify: taskbar progress %s%s (HRESULT 0x%08lx)\n",
          flag == TBPF_NORMAL ? "normal" : flag == TBPF_ERROR ? "error" : flag == TBPF_PAUSED ? "paused" : flag == TBPF_INDETERMINATE ? "indeterminate" : "none",
          num, (unsigned long)hr);

  if (flag == TBPF_NOPROGRESS) {
    if (s) *s = g_shown[--g_nshown];
    return;
  }
  if (!s) {
    if (g_nshown == g_capshown) {
      int cap = g_capshown ? g_capshown * 2 : 4;
      Shown* n = realloc(g_shown, (size_t)cap * sizeof *n);
      if (!n) return;
      g_shown = n;
      g_capshown = cap;
    }
    s = &g_shown[g_nshown++];
    s->hwnd = f->hwnd;
  }
  s->flag = flag;
  s->value = value;
}

void notify_progress(Window* w, ghostty_action_progress_report_s r) {
  if (!w) return;
  TabProgress* p = progress_of(w);
  if (r.state == GHOSTTY_PROGRESS_STATE_REMOVE) {
    if (p) *p = g_progress[--g_nprogress];
  } else {
    if (!p) {
      if (g_nprogress == g_capprogress) {
        int cap = g_capprogress ? g_capprogress * 2 : 4;
        TabProgress* n = realloc(g_progress, (size_t)cap * sizeof *n);
        if (!n) return;
        g_progress = n;
        g_capprogress = cap;
      }
      p = &g_progress[g_nprogress++];
      p->tab = w;
    }
    p->state = r.state;
    p->value = r.progress;
  }
  apply(w->frame);
}

void notify_frame_changed(Frame* f) { apply(f); }

void notify_tab_closed(Window* w) {
  if (g_toast_tab == w) g_toast_tab = NULL;
  TabProgress* p = progress_of(w);
  if (p) *p = g_progress[--g_nprogress];
  Frame* f = w->frame;
  if (!f) return;
  if (f->ntabs <= 1 || f->destroying) {  // the window goes with it: forget what it showed
    for (int i = 0; i < g_nshown; i++)
      if (g_shown[i].hwnd == f->hwnd) {
        g_shown[i] = g_shown[--g_nshown];
        break;
      }
    return;
  }
  apply(f);
}
