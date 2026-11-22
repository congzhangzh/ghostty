#include "host.h"

// ---------------------------------------------------------------------------
// Clipboard

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
  return out;
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
      }
      return true;
    }

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
      detach_tab(f, w);
      SetParent(w->hwnd, nf->hwnd);
      if (!insert_tab(nf, w, 0)) {
        // Out of memory: put it back where it was.
        SetParent(w->hwnd, f->hwnd);
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
