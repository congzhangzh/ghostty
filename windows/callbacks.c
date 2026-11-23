#include "host.h"

// ---------------------------------------------------------------------------
// Clipboard

// The clipboard's text, its Windows line ends (CRLF) made LF, as Windows
// Terminal does: Ghostty turns each LF into the Enter a shell expects (CR)
// when the program has no bracketed paste, but a CRLF into two (CR CR: an
// empty command after each line, a Python block ended early); in a
// bracketed paste the program gets the LF it expects, not a stray CR.
static char* clipboard_text(HWND hwnd) {
  char* out = NULL;
  if (!OpenClipboard(hwnd)) return NULL;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    const WCHAR* s = GlobalLock(h);
    if (s) {
      out = utf16_to_utf8(s, -1);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  if (out) {
    char* d = out;
    for (const char* p = out; *p; p++)
      if (!(p[0] == '\r' && p[1] == '\n')) *d++ = *p;
    *d = 0;
  }
  return out;
}

// Files dropped on a pane (Explorer, the desktop): their paths go in as
// text at the cursor, as on macOS, quoted where cmd or PowerShell would
// split or read them (a space, & ( ) [ ] { } ^ = ; ! ' + , ` ~ $ % @ #),
// one space between them. A path cannot hold a double quote.
void drop_files(Window* w, HDROP drop) {
  UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
  Line t = {0};
  for (UINT i = 0; i < n; i++) {
    UINT len = DragQueryFileW(drop, i, NULL, 0);
    WCHAR* p = malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (!p) continue;
    DragQueryFileW(drop, i, p, len + 1);
    char* s = utf16_to_utf8(p, -1);
    free(p);
    if (!s) continue;
    bool quote = s[strcspn(s, " &()[]{}^=;!'+,`~$%@#")] != 0;
    put(&t, "%s%s%s%s", t.len ? " " : "", quote ? "\"" : "", s, quote ? "\"" : "");
    free(s);
  }
  DragFinish(drop);
  if (t.len && w->surface) {
    ghostty_surface_text(w->surface, t.p, t.len);
    Line e = {0};
    put(&e, "{\"kind\":\"drop\",\"pane\":%d,\"text\":", w->id);
    put_string(&e, t.p);
    put(&e, "}");
    test_event(e.p);
    free(e.p);
  }
  free(t.p);
}

static void set_clipboard_text(HWND hwnd, const char* data, size_t len) {
  int n = MultiByteToWideChar(CP_UTF8, 0, data, (int)len, NULL, 0);
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)n + 1) * sizeof(WCHAR));
  if (!h) return;
  WCHAR* p = GlobalLock(h);
  MultiByteToWideChar(CP_UTF8, 0, data, (int)len, p, n);
  p[n] = 0;
  GlobalUnlock(h);
  if (OpenClipboard(hwnd)) {
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    CloseClipboard();
  } else {
    GlobalFree(h);
  }
}

ghostty_clipboard_read_result_e read_clipboard_cb(
    void* ud, ghostty_clipboard_e which, void* state,
    const char* const* mimes, size_t mimes_len, bool list) {
  (void)mimes;
  (void)mimes_len;
  (void)list;
  Window* w = ud;
  if (which != GHOSTTY_CLIPBOARD_STANDARD)
    return GHOSTTY_CLIPBOARD_READ_UNSUPPORTED;
  char* text = clipboard_text(w->hwnd);
  if (!text) return GHOSTTY_CLIPBOARD_READ_UNAVAILABLE;
  static const char* const available[] = {"text/plain"};
  ghostty_clipboard_content_s content = {"text/plain", text, strlen(text)};
  ghostty_clipboard_complete_s done = {&content, 1, available, 1, false, false};
  ghostty_surface_complete_clipboard_request(w->surface, &done, state);
  free(text);
  return GHOSTTY_CLIPBOARD_READ_STARTED;
}

void confirm_read_clipboard_cb(void* ud,
                                      const ghostty_clipboard_confirm_s* c,
                                      void* state,
                                      ghostty_clipboard_request_e req) {
  Window* w = ud;
  const WCHAR* what = req == GHOSTTY_CLIPBOARD_REQUEST_PASTE
                          ? L"The text you are pasting may run commands. Paste it?"
                          : L"A program in the terminal wants to read the clipboard. Allow it?";
  int r = MessageBoxW(w->frame->hwnd, what, L"Ghostty", MB_YESNO | MB_ICONWARNING);
  if (r == IDYES) {
    ghostty_clipboard_complete_s done = {c->contents, c->contents_len,
                                         c->available, c->available_len,
                                         true, false};
    ghostty_surface_complete_clipboard_request(w->surface, &done, state);
  } else {
    ghostty_surface_deny_clipboard_request(w->surface, state);
  }
}

void write_clipboard_cb(void* ud, ghostty_clipboard_e which,
                               const ghostty_clipboard_content_s* contents,
                               size_t len, bool confirm) {
  Window* w = ud;
  if (which != GHOSTTY_CLIPBOARD_STANDARD) return;
  for (size_t i = 0; i < len; i++) {
    if (!contents[i].mime || strncmp(contents[i].mime, "text/plain", 10) != 0)
      continue;
    if (confirm &&
        MessageBoxW(w->frame->hwnd,
                    L"A program in the terminal wants to set the clipboard. Allow it?",
                    L"Ghostty", MB_YESNO | MB_ICONWARNING) != IDYES)
      return;
    set_clipboard_text(w->hwnd, contents[i].data, contents[i].len);
    return;
  }
}

// ---------------------------------------------------------------------------
// Runtime callbacks

void wakeup_cb(void* ud) {
  (void)ud;
  // Any thread: hand the tick to the main thread, one queued message at
  // a time (one tick handles every wakeup before it).
  if (InterlockedExchange(&g_wakeup_posted, 1) == 0)
    PostMessageW(g_wakeup_hwnd, WM_GHOSTTY_WAKEUP, 0, 0);
}

static Window* target_window(ghostty_target_s t) {
  if (t.tag != GHOSTTY_TARGET_SURFACE || !t.target.surface) return NULL;
  return ghostty_surface_userdata(t.target.surface);
}

bool action_cb(ghostty_app_t app, ghostty_target_s target,
                      ghostty_action_s action) {
  (void)app;
  Window* w = target_window(target);
  Frame* f = w ? w->frame : NULL;
  switch (action.tag) {
    case GHOSTTY_ACTION_RENDER:
      if (!w) return false;
      take_frame(w);
      InvalidateRect(w->hwnd, NULL, FALSE);
      return true;

    case GHOSTTY_ACTION_SET_TITLE: {
      if (!w || !action.action.set_title.title) return false;
      WCHAR* t = utf8_to_utf16(action.action.set_title.title, -1);
      if (t) {
        free(w->title);
        w->title = t;
        if (f->active >= 0 && f->tabs[f->active] == w) update_title(f);
        invalidate_bar(f);
        Line e = {0};  // for the test-control pipe's events
        put(&e, "{\"kind\":\"title\",\"pane\":%d,\"title\":", w->id);
        put_string(&e, action.action.set_title.title);
        put(&e, "}");
        test_event(e.p);
        free(e.p);
      }
      return true;
    }

    case GHOSTTY_ACTION_NEW_SPLIT:
      // Beside the pane it came from, in its tab.
      return w && new_split(w, action.action.new_split);

    case GHOSTTY_ACTION_GOTO_SPLIT:
      return w && goto_split(w, action.action.goto_split);
    case GHOSTTY_ACTION_RESIZE_SPLIT:
      return w && resize_split(w, action.action.resize_split);
    case GHOSTTY_ACTION_EQUALIZE_SPLITS:
      return w && equalize_splits(w);
    case GHOSTTY_ACTION_TOGGLE_SPLIT_ZOOM:
      return w && toggle_split_zoom(w);

    case GHOSTTY_ACTION_RELOAD_CONFIG:
      config_reload(target.tag == GHOSTTY_TARGET_SURFACE && w ? w->surface : NULL,
                    action.action.reload_config.soft);
      return true;
    case GHOSTTY_ACTION_OPEN_CONFIG:
      config_open();
      return true;
    case GHOSTTY_ACTION_CONFIG_CHANGE:
      if (target.tag == GHOSTTY_TARGET_APP) config_changed(action.action.config_change.config);
      return true;

    case GHOSTTY_ACTION_NEW_TAB:
      if (!f) f = front_frame();
      if (!f) return new_window(NULL);
      return new_tab(f, w ? w->surface : NULL, GHOSTTY_SURFACE_CONTEXT_TAB) != NULL;

    case GHOSTTY_ACTION_NEW_WINDOW:
      return new_window(w ? w->surface : NULL);

    case GHOSTTY_ACTION_GOTO_TAB: {
      // With one tab there is nowhere to go: not performed, so the key
      // reaches the terminal (the bindings are performable).
      if (!f || f->ntabs < 2) return false;
      int n = f->ntabs, cur = tab_index(f, w), to;
      int v = (int)action.action.goto_tab;
      if (v == GHOSTTY_GOTO_TAB_PREVIOUS)
        to = (cur - 1 + n) % n;
      else if (v == GHOSTTY_GOTO_TAB_NEXT)
        to = (cur + 1) % n;
      else if (v == GHOSTTY_GOTO_TAB_LAST)
        to = n - 1;
      else if (v >= 1)
        to = v - 1 < n - 1 ? v - 1 : n - 1;  // 1-based; past the end: last
      else
        return false;
      select_tab(f, to);
      return true;
    }

    case GHOSTTY_ACTION_MOVE_TAB:
      if (!f || f->ntabs < 2) return false;
      move_tab(f, (int)action.action.move_tab.amount);
      return true;

    case GHOSTTY_ACTION_CLOSE_TAB: {
      if (!f) return false;
      int me = tab_index(f, w);
      // Collect first: closing changes the list (the close is posted, but
      // a surface may close at once).
      Window* list[256];
      int n = 0;
      for (int i = 0; i < f->ntabs && n < 256; i++) {
        bool pick = false;
        switch (action.action.close_tab_mode) {
          case GHOSTTY_ACTION_CLOSE_TAB_MODE_THIS: pick = i == me; break;
          case GHOSTTY_ACTION_CLOSE_TAB_MODE_OTHER: pick = i != me; break;
          case GHOSTTY_ACTION_CLOSE_TAB_MODE_RIGHT: pick = i > me; break;
        }
        if (pick) list[n++] = f->tabs[i];
      }
      for (int i = 0; i < n; i++) close_tab(list[i]);
      return true;
    }

    case GHOSTTY_ACTION_MOVE_TAB_TO_NEW_WINDOW: {
      if (!f || f->ntabs < 2) return false;
      Frame* nf = new_frame();
      if (!nf) return false;
      // The tab with all its panes (f->tabs holds its focused one).
      Window* tw = f->tabs[tab_index(f, w)];
      Window* panes[64];
      int np = tab_panes(tw->tab, panes, 64);
      if (np > 64) np = 64;
      detach_tab(f, tw);
      for (int i = 0; i < np; i++) {
        SetParent(panes[i]->hwnd, nf->hwnd);
        panes[i]->frame = nf;
      }
      w = tw;
      if (!insert_tab(nf, w, 0)) {
        // Out of memory: put it back where it was.
        for (int i = 0; i < np; i++) {
          SetParent(panes[i]->hwnd, f->hwnd);
          panes[i]->frame = f;
        }
        insert_tab(f, w, f->ntabs);
        layout(f);
        DestroyWindow(nf->hwnd);
        return false;
      }
      layout(nf);
      size_tab(w);
      select_tab(nf, 0);
      show_frame(nf);
      return true;
    }

    case GHOSTTY_ACTION_INSPECTOR:
      // Ghostty's inspector key shows the stats overlay for now.
      if (!f) return false;
      switch (action.action.inspector) {
        case GHOSTTY_INSPECTOR_TOGGLE: set_stats(f, !f->stats); break;
        case GHOSTTY_INSPECTOR_SHOW: set_stats(f, true); break;
        case GHOSTTY_INSPECTOR_HIDE: set_stats(f, false); break;
      }
      return true;

    case GHOSTTY_ACTION_OPEN_URL:  // only http, https, mailto (below)
      return open_url(action.action.open_url.kind, action.action.open_url.url,
                      action.action.open_url.len, false);

    case GHOSTTY_ACTION_TOGGLE_COMMAND_PALETTE:
      palette_toggle(w);  // palette.c
      return true;

    case GHOSTTY_ACTION_TOGGLE_QUICK_TERMINAL:
      quick_toggle();
      return true;

    case GHOSTTY_ACTION_TOGGLE_FULLSCREEN:
      if (!f) return false;
      set_fullscreen(f, !f->fullscreen);
      return true;

    case GHOSTTY_ACTION_RING_BELL:
      if (f) FlashWindow(f->hwnd, TRUE);
      MessageBeep(MB_OK);
      return true;

    case GHOSTTY_ACTION_DESKTOP_NOTIFICATION:
      notify_toast(w, action.action.desktop_notification.title,
                   action.action.desktop_notification.body);
      return true;

    case GHOSTTY_ACTION_PROGRESS_REPORT:
      notify_progress(w, action.action.progress_report);
      return true;

    case GHOSTTY_ACTION_CLOSE_WINDOW:
      if (!f) return false;
      PostMessageW(f->hwnd, WM_CLOSE, 0, 0);
      return true;

    case GHOSTTY_ACTION_CLOSE_ALL_WINDOWS:
      for (Frame* x = g_frames; x; x = x->next)
        PostMessageW(x->hwnd, WM_CLOSE, 0, 0);
      return true;

    case GHOSTTY_ACTION_QUIT:
      PostQuitMessage(0);
      return true;

    default:
      return false;
  }
}

void close_surface_cb(void* ud, bool process_alive) {
  (void)process_alive;
  Window* w = ud;
  if (w && w->hwnd) PostMessageW(w->hwnd, WM_GHOSTTY_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// Opening links (OPEN_URL): Ctrl+click on a link, OSC 8 hyperlinks
//
// A link's target comes from what programs print, so it is untrusted (the
// macOS app's UntrustedURL, Helpers/UntrustedURL.swift, likewise): only
// http, https (with a //host) and mailto are opened, with ShellExecute
// "open", i.e. in the default browser or mail app. Never a path, a file:
// URL or another scheme: ShellExecute would start a program for them
// (calc.exe, a .bat, a custom protocol handler). A quote, whitespace or a
// control character is refused too: the handler gets the URL in its own
// command line ("%1"), where a quote could add arguments.
// Files the core wrote itself (write_screen_file …, kind text or html) open
// only when they are a regular file in the temp folder: text in Notepad,
// html in the browser.

static bool url_scheme(const char* u, char* out, size_t cap) {
  size_t i = 0;
  if (!isalpha((unsigned char)u[0])) return false;
  while (u[i] && u[i] != ':' && i + 1 < cap) {
    char c = u[i];
    if (!isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.') return false;
    out[i] = (char)tolower((unsigned char)c);
    i++;
  }
  out[i] = 0;
  return u[i] == ':';
}

// NULL when `url` may be opened, else why not.
const char* url_refused(const char* url) {
  for (const unsigned char* p = (const unsigned char*)url; *p; p++)
    if (*p <= ' ' || *p == 0x7f || *p == '"' || *p == '<' || *p == '>' || *p == '`' || *p == '\\')
      return "a quote, space, control character or backslash in it";
  char scheme[16];
  if (!url_scheme(url, scheme, sizeof(scheme))) return "no scheme (a path is never opened)";
  const char* rest = url + strlen(scheme) + 1;
  if (strcmp(scheme, "http") == 0 || strcmp(scheme, "https") == 0) {
    if (strncmp(rest, "//", 2) != 0 || !rest[2] || rest[2] == '/') return "no //host";
    return NULL;
  }
  if (strcmp(scheme, "mailto") == 0) return *rest ? NULL : "an empty mailto";
  return "only http, https and mailto are opened";
}

// A file the core wrote: a regular file in the temp folder, of `ext`.
static bool temp_file(const WCHAR* path, const WCHAR* ext) {
  WCHAR tmp[MAX_PATH], full[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, tmp);
  if (!n || n >= MAX_PATH || !GetFullPathNameW(path, MAX_PATH, full, NULL)) return false;
  DWORD a = GetFileAttributesW(full);
  if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
  if (_wcsnicmp(full, tmp, wcslen(tmp)) != 0) return false;
  const WCHAR* dot = wcsrchr(full, L'.');
  return dot && _wcsicmp(dot, ext) == 0;
}

static void url_event(const char* url, bool opened, const char* why) {
  Line e = {0};
  put(&e, "{\"kind\":\"open_url\",\"url\":");
  put_string(&e, url);
  put(&e, ",\"opened\":%s,\"why\":", opened ? "true" : "false");
  put_string(&e, why ? why : "");
  put(&e, "}");
  test_event(e.p);
  free(e.p);
}

// The action; `dry`: decide only (the test pipe).
bool open_url(ghostty_action_open_url_kind_e kind, const char* p, size_t len, bool dry) {
  char* url = malloc(len + 1);
  if (!url) return false;
  memcpy(url, p, len);
  url[len] = 0;
  const char* why = NULL;
  bool opened = false;
  WCHAR* w = utf8_to_utf16(url, -1);
  if (!w) why = "not UTF-8";
  else if (kind == GHOSTTY_ACTION_OPEN_URL_KIND_TEXT || kind == GHOSTTY_ACTION_OPEN_URL_KIND_HTML) {
    bool text = kind == GHOSTTY_ACTION_OPEN_URL_KIND_TEXT;
    if (!temp_file(w, text ? L".txt" : L".html")) why = "not a file Ghostty wrote in the temp folder";
    else if (!dry) {
      if (text) {
        WCHAR args[MAX_PATH + 4];
        swprintf(args, MAX_PATH + 4, L"\"%ls\"", w);
        opened = (INT_PTR)ShellExecuteW(NULL, L"open", L"notepad.exe", args, NULL, SW_SHOWNORMAL) > 32;
      } else {
        opened = (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
      }
      if (!opened) why = "ShellExecute failed";
    }
  } else if (!(why = url_refused(url)) && !dry) {
    opened = (INT_PTR)ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL) > 32;
    if (!opened) why = "ShellExecute failed";
  }
  if (why) fprintf(stderr, "ghostty: open url: not opened: %s (%s)\n", url, why);
  url_event(url, opened || (dry && !why), why);
  free(w);
  free(url);
  return true;
}
