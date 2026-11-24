// Desktop notifications (OSC 9, OSC 777, OSC 99) and taskbar progress (OSC 9;4).
//
// A notification is a WinRT toast when Ghostty was installed (the installer's Start-menu
// shortcut carries the AppUserModelID GhosttyForWindows.Ghostty, which this process then takes,
// so Windows shows the toast as Ghostty's, with its name and icon, and keeps it in the
// notification center). Otherwise, or when the toast fails, it is a balloon from a
// notification-area icon that exists only while the balloon is up (Windows 10 and 11 show
// that as a toast too, under the icon's name). It is not shown when it comes from the selected tab of the
// foreground window, as on macOS (shown unless its window is the key window and its surface is
// focused). A click on it brings that window forward and selects that tab. The core already
// drops notifications when `desktop-notifications = false`.
//
// Progress goes to the window's taskbar button (ITaskbarList3): the selected tab's progress if
// it reports one, else the most severe of the window's tabs (error, paused, normal,
// indeterminate). A state is kept per tab, so bursts collapse to the last one. When the
// taskbar (re)creates a window's button it sends "TaskbarButtonCreated" (also after Explorer
// restarts), and the window's progress is sent again; for an elevated ghostty.exe that message
// is let through UIPI. Every progress action and every COM result is logged.

#include "host.h"

#include <shobjidl.h>
#include <roapi.h>
#include <winstring.h>

#define WM_NOTIFY_ICON (WM_APP + 41)
#define WM_TOAST_CLICK (WM_APP + 42)
#define GHOSTTY_AUMID L"GhosttyForWindows.Ghostty"
#define NOTIFY_ICON_ID 1

// CLSID_TaskbarList and IID_ITaskbarList3, declared here so the host needs no uuid library.
static const CLSID kClsidTaskbarList = {0x56fdf344, 0xfd6d, 0x11d0, {0x95, 0x8a, 0x00, 0x60, 0x97, 0xc9, 0xa0, 0x90}};
static const IID kIidTaskbarList3 = {0xea1afb91, 0x9e28, 0x4b86, {0x90, 0xe9, 0x9e, 0x9f, 0x8a, 0x5e, 0xef, 0xaf}};

static HWND g_notify_hwnd;  // receives the icon's messages
static bool g_icon_shown;
static Window* g_toast_tab;  // the tab the shown balloon is from, NULL once it is closed

// ---------------------------------------------------------------------------
// Where a tab is

// `w` is a pane: the tab it is in (tab_index knows split panes; f->tabs[i] is only a tab's
// focused pane).
static bool find_tab(Window* w, Frame** fo, int* io) {
  for (Frame* f = g_frames; f; f = f->next) {
    int i = tab_index(f, w);
    if (i >= 0) {
      *fo = f;
      *io = i;
      return true;
    }
  }
  return false;
}

// Is `w` still a pane of tab `i` of `f`? (A click can come after it closed.)
static bool pane_alive(Frame* f, int i, Window* w) {
  if (f->tabs[i] == w) return true;
  Window* panes[64];
  int n = f->tabs[i]->tab ? tab_panes(f->tabs[i]->tab, panes, 64) : 0;
  for (int k = 0; k < n && k < 64; k++)
    if (panes[k] == w) return true;
  return false;
}

// Bring `w` forward, as a click on its notification does on macOS: its window, its tab, and
// the pane itself focused.
static void show_pane(Window* w, const char* why) {
  Frame* f;
  int i;
  if (w && find_tab(w, &f, &i)) {
    fprintf(stderr, "ghostty: notify: %s, showing its window and tab %d\n", why, i);
    if (IsIconic(f->hwnd)) ShowWindow(f->hwnd, SW_RESTORE);
    SetForegroundWindow(f->hwnd);
    select_tab(f, i);
    if (f->tabs[i] != w && pane_alive(f, i, w)) SetFocus(w->hwnd);
  } else {
    fprintf(stderr, "ghostty: notify: %s, but its tab is closed\n", why);
  }
}

// ---------------------------------------------------------------------------
// The notification-area icon and its balloon

static void icon_remove(void) {
  if (!g_icon_shown) return;
  NOTIFYICONDATAW nid = {.cbSize = sizeof nid, .hWnd = g_notify_hwnd, .uID = NOTIFY_ICON_ID};
  Shell_NotifyIconW(NIM_DELETE, &nid);
  g_icon_shown = false;
}

static Window* pane_by_id(int id);
static LRESULT CALLBACK notify_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_TOAST_CLICK) {
    show_pane(pane_by_id((int)wp), "toast clicked");
    return 0;
  }
  if (msg == WM_NOTIFY_ICON) {
    // NOTIFYICON_VERSION_4: the event is in LOWORD(lParam).
    switch (LOWORD(lp)) {
      case NIN_BALLOONUSERCLICK:
        show_pane(g_toast_tab, "clicked");
        g_toast_tab = NULL;
        icon_remove();
        return 0;
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

// ---------------------------------------------------------------------------
// WinRT toasts (Windows.UI.Notifications), through their COM interfaces: mingw has no
// windows.ui.notifications.h, so the few interfaces used are declared here. A toast needs an
// AppUserModelID that a Start-menu shortcut carries (Windows ignores toasts from an app it
// cannot name), so it is used only when the installer's shortcut is there.


typedef struct IInspectableVtblHead {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(void*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(void*);
  ULONG(STDMETHODCALLTYPE* Release)(void*);
  void* GetIids;
  void* GetRuntimeClassName;
  void* GetTrustLevel;
} IInspectableVtblHead;

typedef struct { const struct {
  IInspectableVtblHead h;
  void* CreateToastNotifier;
  HRESULT(STDMETHODCALLTYPE* CreateToastNotifierWithId)(void*, HSTRING, void**);
} * v; } ToastManagerStatics;
typedef struct { const struct {
  IInspectableVtblHead h;
  HRESULT(STDMETHODCALLTYPE* Show)(void*, void*);
} * v; } ToastNotifier;
typedef struct { const struct {
  IInspectableVtblHead h;
  HRESULT(STDMETHODCALLTYPE* CreateToastNotification)(void*, void*, void**);
} * v; } ToastFactory;
typedef struct { const struct {
  IInspectableVtblHead h;
  HRESULT(STDMETHODCALLTYPE* LoadXml)(void*, HSTRING);
} * v; } XmlDocumentIO;
typedef struct { const struct {
  IInspectableVtblHead h;
  void* get_Content;
  void* put_ExpirationTime;
  void* get_ExpirationTime;
  void* add_Dismissed;
  void* remove_Dismissed;
  HRESULT(STDMETHODCALLTYPE* add_Activated)(void*, void*, INT64*);
} * v; } ToastNotification;

// Windows.UI.Notifications and Windows.Data.Xml.Dom, from the Windows SDK's metadata.
static const IID kIidToastManagerStatics = {0x50ac103f, 0xd235, 0x4598, {0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4}};
static const IID kIidToastFactory = {0x04124b20, 0x82c6, 0x4229, {0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53}};
static const IID kIidXmlDocumentIO = {0x6cd0e74e, 0xee65, 0x4489, {0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37}};
static const IID kIidXmlDocument = {0xf7f3a506, 0x1e87, 0x42d6, {0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94}};
// TypedEventHandler<ToastNotification, IInspectable> (ToastNotification.Activated)
static const IID kIidActivatedHandler = {0xab54de2d, 0x97d9, 0x5528, {0xb6, 0xad, 0x10, 0x5a, 0xfe, 0x15, 0x65, 0x30}};
static const IID kIidAgileObject = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
static const IID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

static int g_toasts = -1;  // -1 not decided, 0 balloons only, 1 toasts

// The runtime classes, by name. Their lengths are taken with wcslen, never counted by hand
// (the first version passed 48 for the manager's 49 characters: "class not registered").
static const WCHAR kToastManager[] = L"Windows.UI.Notifications.ToastNotificationManager";
static const WCHAR kXmlDocument[] = L"Windows.Data.Xml.Dom.XmlDocument";
static const WCHAR kToastNotification[] = L"Windows.UI.Notifications.ToastNotification";

// combase.dll's WinRT functions, loaded when toasts are first wanted: the host links no WinRT
// import library, and a Windows without them keeps balloons.
static struct {
  HRESULT(WINAPI* Initialize)(RO_INIT_TYPE);
  HRESULT(WINAPI* GetActivationFactory)(HSTRING, REFIID, void**);
  HRESULT(WINAPI* ActivateInstance)(HSTRING, void**);
  HRESULT(WINAPI* CreateString)(PCNZWCH, UINT32, HSTRING*);
  HRESULT(WINAPI* DeleteString)(HSTRING);
} g_ro;

static bool ro_load(void) {
  HMODULE m = LoadLibraryExW(L"combase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!m) return false;
  g_ro.Initialize = (void*)GetProcAddress(m, "RoInitialize");
  g_ro.GetActivationFactory = (void*)GetProcAddress(m, "RoGetActivationFactory");
  g_ro.ActivateInstance = (void*)GetProcAddress(m, "RoActivateInstance");
  g_ro.CreateString = (void*)GetProcAddress(m, "WindowsCreateString");
  g_ro.DeleteString = (void*)GetProcAddress(m, "WindowsDeleteString");
  return g_ro.Initialize && g_ro.GetActivationFactory && g_ro.ActivateInstance && g_ro.CreateString && g_ro.DeleteString;
}
static ToastNotifier* g_notifier;

// The pane a toast is from, kept by its id (a pane's id is never reused) until a click.
typedef struct ActivatedHandler {
  const struct ActivatedHandlerVtbl* v;
  LONG ref;
  int pane;
} ActivatedHandler;
struct ActivatedHandlerVtbl {
  HRESULT(STDMETHODCALLTYPE* QueryInterface)(ActivatedHandler*, REFIID, void**);
  ULONG(STDMETHODCALLTYPE* AddRef)(ActivatedHandler*);
  ULONG(STDMETHODCALLTYPE* Release)(ActivatedHandler*);
  HRESULT(STDMETHODCALLTYPE* Invoke)(ActivatedHandler*, void*, void*);
};
static HRESULT STDMETHODCALLTYPE ah_qi(ActivatedHandler* h, REFIID iid, void** out) {
  if (IsEqualIID(iid, &kIidActivatedHandler) || IsEqualIID(iid, &kIidUnknown) || IsEqualIID(iid, &kIidAgileObject)) {
    *out = h;
    InterlockedIncrement(&h->ref);
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ah_addref(ActivatedHandler* h) { return (ULONG)InterlockedIncrement(&h->ref); }
static ULONG STDMETHODCALLTYPE ah_release(ActivatedHandler* h) {
  LONG r = InterlockedDecrement(&h->ref);
  if (r == 0) free(h);
  return (ULONG)r;
}
// On a thread of Windows' own: the UI thread does the rest.
static HRESULT STDMETHODCALLTYPE ah_invoke(ActivatedHandler* h, void* sender, void* args) {
  (void)sender, (void)args;
  if (g_notify_hwnd) PostMessageW(g_notify_hwnd, WM_TOAST_CLICK, (WPARAM)h->pane, 0);
  return S_OK;
}
static const struct ActivatedHandlerVtbl kActivatedVtbl = {ah_qi, ah_addref, ah_release, ah_invoke};

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

// Is Ghostty installed, i.e. is there a Start-menu shortcut that names our AppUserModelID?
// (The installer's: %APPDATA%\Microsoft\Windows\Start Menu\Programs\Ghostty.lnk.)
static bool shortcut_installed(void) {
  WCHAR path[MAX_PATH * 2];
  DWORD n = GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  wcscat(path, L"\\Microsoft\\Windows\\Start Menu\\Programs\\Ghostty.lnk");
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Once, before the first window is shown: take the AppUserModelID when the installer's
// shortcut is there (the taskbar then groups and pins Ghostty under it too), and get a
// toast notifier for it. GHOSTTY_TOAST=off keeps balloons.
static void toasts_init(void) {
  if (g_toasts >= 0) return;
  g_toasts = 0;
  WCHAR env[8];
  if (GetEnvironmentVariableW(L"GHOSTTY_TOAST", env, 8) && _wcsicmp(env, L"off") == 0) {
    fprintf(stderr, "ghostty: notify: toasts off (GHOSTTY_TOAST=off): balloons\n");
    return;
  }
  if (!shortcut_installed()) {
    fprintf(stderr, "ghostty: notify: no Start-menu shortcut (not installed): balloons\n");
    return;
  }
  HRESULT hr = SetCurrentProcessExplicitAppUserModelID(GHOSTTY_AUMID);
  if (!ro_load()) {
    fprintf(stderr, "ghostty: notify: no WinRT in combase.dll: balloons\n");
    return;
  }
  HRESULT ro = g_ro.Initialize(RO_INIT_SINGLETHREADED);  // the UI thread; S_FALSE or RPC_E_CHANGED_MODE when COM is up already
  HSTRING cls = NULL, id = NULL;
  ToastManagerStatics* statics = NULL;
  HRESULT h2 = g_ro.CreateString(kToastManager, (UINT32)wcslen(kToastManager), &cls);
  if (SUCCEEDED(h2)) h2 = g_ro.GetActivationFactory(cls, &kIidToastManagerStatics, (void**)&statics);
  if (SUCCEEDED(h2)) h2 = g_ro.CreateString(GHOSTTY_AUMID, (UINT32)wcslen(GHOSTTY_AUMID), &id);
  if (SUCCEEDED(h2)) h2 = statics->v->CreateToastNotifierWithId(statics, id, (void**)&g_notifier);
  if (statics) statics->v->h.Release(statics);
  if (cls) g_ro.DeleteString(cls);
  if (id) g_ro.DeleteString(id);
  fprintf(stderr, "ghostty: notify: AppUserModelID %ls (HRESULT 0x%08lx), RoInitialize 0x%08lx, toast notifier 0x%08lx: %s\n",
          GHOSTTY_AUMID, (unsigned long)hr, (unsigned long)ro, (unsigned long)h2, g_notifier ? "toasts" : "balloons");
  g_toasts = g_notifier ? 1 : 0;
}

// A growing wide string for the toast's XML: the text in it comes from programs (OSC 9 and
// OSC 777 bodies), so it is never written into a fixed buffer.
typedef struct {
  WCHAR* p;
  size_t n, cap;
  bool failed;
} WBuf;

static void wb_add(WBuf* b, const WCHAR* s, size_t len) {
  if (b->failed) return;
  if (b->n + len + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 512;
    while (b->n + len + 1 > cap) cap *= 2;
    WCHAR* p = realloc(b->p, cap * sizeof(WCHAR));
    if (!p) {
      b->failed = true;
      return;
    }
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->n, s, len * sizeof(WCHAR));
  b->n += len;
  b->p[b->n] = 0;
}

static void wb_tag(WBuf* b, const WCHAR* s) { wb_add(b, s, wcslen(s)); }

// UTF-8 `s` as XML text, at most `limit` UTF-16 units of it (a toast shows a few lines; the
// rest is cut, never a pair of surrogates in half), control characters but tab and newline left
// out.
static void wb_text(WBuf* b, const char* s, size_t limit) {
  WCHAR* w = s && *s ? utf8_to_utf16(s, -1) : NULL;
  size_t len = w ? wcslen(w) : 0;
  if (len > limit) {
    len = limit;
    if (len && w[len - 1] >= 0xD800 && w[len - 1] <= 0xDBFF) len--;
  }
  for (size_t i = 0; i < len; i++) {
    WCHAR c = w[i];
    if (c == L'&') wb_tag(b, L"&amp;");
    else if (c == L'<') wb_tag(b, L"&lt;");
    else if (c == L'>') wb_tag(b, L"&gt;");
    else if (c == L'"') wb_tag(b, L"&quot;");
    else if (c >= 0x20 || c == L'\n' || c == L'\t') wb_add(b, &c, 1);
  }
  free(w);
}

// The toast: title, body, and the tab's title as the attribution line (macOS shows it as the
// subtitle). Returns the HRESULT; on a failure the caller shows a balloon instead.
static HRESULT toast_show(Window* w, const char* title, const char* body) {
  WBuf x = {0};
  wb_tag(&x, L"<toast><visual><binding template=\"ToastGeneric\"><text>");
  wb_text(&x, title && *title ? title : "Ghostty", 256);
  wb_tag(&x, L"</text><text>");
  wb_text(&x, body, 2048);
  wb_tag(&x, L"</text>");
  if (w->title && *w->title) {
    char* t = utf16_to_utf8(w->title, -1);
    wb_tag(&x, L"<text placement=\"attribution\">");
    wb_text(&x, t, 256);
    wb_tag(&x, L"</text>");
    free(t);
  }
  wb_tag(&x, L"</binding></visual></toast>");
  if (x.failed || !x.p) {
    free(x.p);
    return E_OUTOFMEMORY;
  }
  const WCHAR* xml = x.p;

  HSTRING cls_doc = NULL, cls_toast = NULL, hxml = NULL;
  void* insp = NULL;
  XmlDocumentIO* io = NULL;
  void* doc = NULL;
  ToastFactory* factory = NULL;
  ToastNotification* toast = NULL;
  HRESULT hr = g_ro.CreateString(kXmlDocument, (UINT32)wcslen(kXmlDocument), &cls_doc);
  if (SUCCEEDED(hr)) hr = g_ro.ActivateInstance(cls_doc, (void**)&insp);
  if (SUCCEEDED(hr)) hr = ((IInspectableVtblHead*)*(void**)insp)->QueryInterface(insp, &kIidXmlDocumentIO, (void**)&io);
  if (SUCCEEDED(hr)) hr = g_ro.CreateString(xml, (UINT32)wcslen(xml), &hxml);
  if (SUCCEEDED(hr)) hr = io->v->LoadXml(io, hxml);
  if (SUCCEEDED(hr)) hr = io->v->h.QueryInterface(io, &kIidXmlDocument, &doc);
  if (SUCCEEDED(hr)) hr = g_ro.CreateString(kToastNotification, (UINT32)wcslen(kToastNotification), &cls_toast);
  if (SUCCEEDED(hr)) hr = g_ro.GetActivationFactory(cls_toast, &kIidToastFactory, (void**)&factory);
  if (SUCCEEDED(hr)) hr = factory->v->CreateToastNotification(factory, doc, (void**)&toast);
  if (SUCCEEDED(hr)) {
    // A click brings its pane forward (in-process, while Ghostty runs; no COM activator).
    ActivatedHandler* h = calloc(1, sizeof *h);
    if (h) {
      h->v = &kActivatedVtbl;
      h->ref = 1;
      h->pane = w->id;
      INT64 token = 0;
      HRESULT ha = toast->v->add_Activated(toast, h, &token);
      if (FAILED(ha)) fprintf(stderr, "ghostty: notify: toast click handler not set (HRESULT 0x%08lx)\n", (unsigned long)ha);
      ah_release(h);  // the toast holds its own reference now
    }
    hr = g_notifier->v->Show(g_notifier, toast);
  }
  if (toast) toast->v->h.Release(toast);
  if (factory) factory->v->h.Release(factory);
  if (doc) ((IInspectableVtblHead*)*(void**)doc)->Release(doc);
  if (io) io->v->h.Release(io);
  if (insp) ((IInspectableVtblHead*)*(void**)insp)->Release(insp);
  if (hxml) g_ro.DeleteString(hxml);
  free(x.p);
  if (cls_toast) g_ro.DeleteString(cls_toast);
  if (cls_doc) g_ro.DeleteString(cls_doc);
  return hr;
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

// A JSON string of `s` into dst (quotes included), for test_event.
static void json_str(char* dst, size_t n, const char* s) {
  size_t o = 0;
  if (n < 3) return;
  dst[o++] = '"';
  for (; s && *s && o + 8 < n; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') o += (size_t)snprintf(dst + o, n - o, "\\%c", c);
    else if (c < 0x20) o += (size_t)snprintf(dst + o, n - o, "\\u%04x", c);
    else dst[o++] = (char)c;
  }
  dst[o++] = '"';
  dst[o] = 0;
}

// A window's index in g_frames, as the test pipe's "list" numbers them.
static int frame_index(Frame* f) {
  int i = 0;
  for (Frame* g = g_frames; g; g = g->next, i++)
    if (g == f) return i;
  return -1;
}

void notify_toast(Window* w, const char* title, const char* body) {
  if (!w) return;
  Frame* f;
  int i;
  if (!find_tab(w, &f, &i)) return;
  // macOS: not shown when its window is the key window and its surface is focused. Here: the
  // foreground window, its selected tab, and that tab's focused pane.
  bool seen = GetForegroundWindow() == f->hwnd && f->active == i && f->tabs[i] == w;
  fprintf(stderr, "ghostty: notify: \"%s\" / \"%s\" %s\n", title ? title : "", body ? body : "",
          seen ? "not shown: its pane is the focused pane of the foreground window" : "shown");
  {
    char t[600], bd[1200], ev[2048];
    json_str(t, sizeof t, title);
    json_str(bd, sizeof bd, body);
    snprintf(ev, sizeof ev, "{\"kind\":\"notification\",\"window\":%d,\"title\":%s,\"body\":%s,\"shown\":%s,\"why\":\"%s\"}",
             frame_index(f), t, bd, seen ? "false" : "true",
             seen ? "its pane is the focused pane of the foreground window" : "");
    test_event(ev);
  }
  if (seen || !notify_window()) return;
  if (g_toasts == 1) {
    HRESULT hr = toast_show(w, title, body);
    fprintf(stderr, "ghostty: notify: toast %s (HRESULT 0x%08lx)\n", SUCCEEDED(hr) ? "shown" : "failed, a balloon instead", (unsigned long)hr);
    if (SUCCEEDED(hr)) return;
  }

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

static bool elevated(void) {
  HANDLE token;
  TOKEN_ELEVATION e = {0};
  DWORD n = 0;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  BOOL ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &n);
  CloseHandle(token);
  return ok && e.TokenIsElevated;
}

static ITaskbarList3* taskbar(void) {
  if (g_taskbar || g_taskbar_failed) return g_taskbar;
  // The UI thread; S_FALSE (already) or RPC_E_CHANGED_MODE (an MTA) are fine.
  HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
  HRESULT create = CoCreateInstance(&kClsidTaskbarList, NULL, CLSCTX_INPROC_SERVER, &kIidTaskbarList3, (void**)&g_taskbar);
  HRESULT hr = create;
  if (SUCCEEDED(hr)) hr = g_taskbar->lpVtbl->HrInit(g_taskbar);
  fprintf(stderr, "ghostty: notify: ITaskbarList3: CoInitializeEx 0x%08lx, CoCreateInstance 0x%08lx, HrInit 0x%08lx; process elevated: %s\n",
          (unsigned long)init, (unsigned long)create, (unsigned long)hr, elevated() ? "yes" : "no");
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

static const char* flag_name(TBPFLAG flag) {
  return flag == TBPF_NORMAL ? "normal" : flag == TBPF_ERROR ? "error" : flag == TBPF_PAUSED ? "paused" : flag == TBPF_INDETERMINATE ? "indeterminate" : "none";
}

// What `f`'s taskbar button should show: the selected tab's progress, or the
// most severe of its other tabs'. *value: 0-100, or -1 for none.
static TBPFLAG wanted(Frame* f, int* value) {
  TabProgress* pick = NULL;
  if (f->active >= 0 && f->active < f->ntabs) {
    TabProgress* p = progress_of(f->tabs[f->active]);
    if (p && p->state != GHOSTTY_PROGRESS_STATE_REMOVE) pick = p;
  }
  // Else the most severe of every pane of the window: a split's other panes report too
  // (progress is kept per pane, f->tabs[i] is only a tab's focused pane).
  if (!pick)
    for (int i = 0; i < f->ntabs; i++) {
      Window* panes[64];
      int n = f->tabs[i]->tab ? tab_panes(f->tabs[i]->tab, panes, 64) : 0;
      if (n <= 0) panes[0] = f->tabs[i], n = 1;
      for (int k = 0; k < n && k < 64; k++) {
        TabProgress* p = progress_of(panes[k]);
        if (p && severity(p->state) > (pick ? severity(pick->state) : 0)) pick = p;
      }
    }
  TBPFLAG flag = pick ? flag_of(pick->state) : TBPF_NOPROGRESS;
  *value = pick && flag != TBPF_INDETERMINATE && flag != TBPF_NOPROGRESS ? pick->value : -1;
  return flag;
}

// For the test pipe's "list": the progress `f`'s button should show ("normal",
// "error", "paused", "indeterminate"), or NULL for none; *value as wanted().
const char* notify_progress_state(Frame* f, int* value) {
  int v = -1;
  TBPFLAG flag = f && f->hwnd && !f->destroying ? wanted(f, &v) : TBPF_NOPROGRESS;
  if (value) *value = v;
  return flag == TBPF_NOPROGRESS ? NULL : flag_name(flag);
}

// Shows on `f`'s taskbar button the progress its tabs report.
static void apply(Frame* f) {
  if (!f || !f->hwnd || f->destroying) return;
  int value;
  TBPFLAG flag = wanted(f, &value);

  Shown* s = NULL;
  for (int i = 0; i < g_nshown; i++)
    if (g_shown[i].hwnd == f->hwnd) s = &g_shown[i];
  if (s && s->flag == flag && s->value == value) return;
  if (!s && flag == TBPF_NOPROGRESS) return;  // nothing shown, nothing to show

  ITaskbarList3* tb = taskbar();
  {
    char ev[160];
    snprintf(ev, sizeof ev, "{\"kind\":\"progress\",\"window\":%d,\"state\":\"%s\",\"value\":%d,\"taskbar\":%s}",
             frame_index(f), flag_name(flag), value, tb ? "true" : "false");
    test_event(ev);
  }
  if (!tb) return;
  HRESULT hr = tb->lpVtbl->SetProgressState(tb, f->hwnd, flag);
  if (SUCCEEDED(hr) && value >= 0) hr = tb->lpVtbl->SetProgressValue(tb, f->hwnd, (ULONGLONG)value, 100);
  char num[8] = "";
  if (value >= 0) snprintf(num, sizeof num, " %d", value);
  fprintf(stderr, "ghostty: notify: taskbar progress %s%s (HRESULT 0x%08lx) on window %p%s\n",
          flag_name(flag), num, (unsigned long)hr, (void*)f->hwnd, IsWindowVisible(f->hwnd) ? "" : " (not visible)");

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
  fprintf(stderr, "ghostty: notify: progress action: state %d, value %d%s\n", (int)r.state, (int)r.progress,
          w ? "" : " (no tab: ignored)");
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

// A pane's own progress (OSC 9;4), for the bar drawn on it (paint.c):
// false for none; *value -1 when it has no number (indeterminate, or an
// error/pause without one).
bool notify_pane_progress(Window* w, int* state, int* value) {
  TabProgress* p = w ? progress_of(w) : NULL;
  if (!p || p->state == GHOSTTY_PROGRESS_STATE_REMOVE) return false;
  *state = (int)p->state;
  *value = p->value;
  return true;
}

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

// The frame window's messages, first thing in frame_proc (its Frame isn't set yet at
// WM_NCCREATE). True when it was "TaskbarButtonCreated", which this handles.
bool notify_frame_msg(HWND hwnd, UINT msg) {
  static UINT button_created;
  if (!button_created) button_created = RegisterWindowMessageW(L"TaskbarButtonCreated");
  if (msg == WM_NCCREATE) {
    toasts_init();  // the AppUserModelID must be set before the first window is shown
    // Before the button exists: an elevated process would not get the message otherwise.
    ChangeWindowMessageFilterEx(hwnd, button_created, MSGFLT_ALLOW, NULL);
    return false;
  }
  if (!button_created || msg != button_created) return false;
  Frame* f = (Frame*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  fprintf(stderr, "ghostty: notify: taskbar button created for window %p\n", (void*)hwnd);
  for (int i = 0; i < g_nshown; i++)  // a new button shows nothing: send it all again
    if (g_shown[i].hwnd == hwnd) {
      g_shown[i] = g_shown[--g_nshown];
      break;
    }
  if (f) apply(f);
  return true;
}
