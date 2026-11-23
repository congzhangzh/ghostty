#include "host.h"

#include <imm.h>

// ---------------------------------------------------------------------------
// IME: Chinese, Japanese, Korean input
//
// An input method composes text before it is typed: Pinyin "zhongwen"
// becomes 中文, romaji "nihongo" becomes 日本語. While it composes, the
// text being composed (the preedit) is Ghostty's to draw, at the cursor,
// as the macOS and GTK apps do; the IME's own composition window is hidden
// (WM_IME_SETCONTEXT). The candidate list is the IME's, placed just below
// the cursor's cell and never over it (CFS_EXCLUDE). The result goes to the
// terminal as text once: WM_IME_COMPOSITION is not handed to
// DefWindowProc, so no WM_IME_CHAR follows with the same text.
//
// The keys the IME takes come as VK_PROCESSKEY and never reach Ghostty
// (input.c); their releases come with the real key and are kept back too,
// as every release whose press Ghostty did not get (input.c's pressed).
//
// The positions are the core's (ghostty_surface_ime_point): the middle of
// the cursor's cell and its bottom, in points.

static void ime_place(Window* w, HIMC imc) {
  double x = 0, y = 0, width = 0, height = 0;
  ghostty_surface_ime_point(w->surface, &x, &y, &width, &height);
  int px = (int)(x * w->scale), bottom = (int)(y * w->scale);
  int top = bottom - (int)(height * w->scale);
  COMPOSITIONFORM cf = {0};
  cf.dwStyle = CFS_POINT;  // for an IME that shows its own window anyway
  cf.ptCurrentPos.x = px;
  cf.ptCurrentPos.y = top;
  ImmSetCompositionWindow(imc, &cf);
  CANDIDATEFORM cand = {0};
  cand.dwIndex = 0;
  cand.dwStyle = CFS_EXCLUDE;  // below the cell, or above it at the bottom
  cand.ptCurrentPos.x = px;
  cand.ptCurrentPos.y = bottom;
  cand.rcArea.left = px;
  cand.rcArea.top = top;
  cand.rcArea.right = px + 1;
  cand.rcArea.bottom = bottom;
  ImmSetCandidateWindow(imc, &cand);
  w->ime_x = px;
  w->ime_y = bottom;
}

// One of the composition's strings (GCS_COMPSTR, GCS_RESULTSTR) in UTF-8,
// or NULL when empty.
static char* ime_string(HIMC imc, DWORD which) {
  LONG bytes = ImmGetCompositionStringW(imc, which, NULL, 0);
  if (bytes <= 0) return NULL;
  WCHAR* buf = malloc((size_t)bytes + sizeof(WCHAR));
  if (!buf) return NULL;
  bytes = ImmGetCompositionStringW(imc, which, buf, (DWORD)bytes);
  char* s = bytes > 0 ? utf16_to_utf8(buf, (int)(bytes / sizeof(WCHAR))) : NULL;
  free(buf);
  return s;
}

static void ime_event(Window* w, const char* what, const char* text) {
  Line e = {0};
  put(&e, "{\"kind\":\"ime\",\"pane\":%d,\"what\":\"%s\",\"text\":", w->id, what);
  put_string(&e, text ? text : "");
  put(&e, ",\"x\":%d,\"y\":%d}", w->ime_x, w->ime_y);
  test_event(e.p);
  free(e.p);
}

static void ime_preedit(Window* w, const char* s) {
  ghostty_surface_preedit(w->surface, s, s ? strlen(s) : 0);
  free(w->ime_preedit);
  w->ime_preedit = s ? _strdup(s) : NULL;
}

// The pane's IME messages; true when handled (the result in *out).
bool ime_message(Window* w, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out) {
  HWND hwnd = w->hwnd;
  switch (msg) {
    case WM_IME_SETCONTEXT:
      // Ghostty draws the composition: not the IME's window for it.
      if (wp) lp &= ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW;
      *out = DefWindowProcW(hwnd, msg, wp, lp);
      return true;

    case WM_IME_STARTCOMPOSITION: {
      HIMC imc = ImmGetContext(hwnd);
      if (imc) {
        ime_place(w, imc);
        ImmReleaseContext(hwnd, imc);
      }
      w->ime_composing = true;
      ime_event(w, "start", NULL);
      *out = 0;  // not DefWindowProc: it would open the composition window
      return true;
    }

    case WM_IME_COMPOSITION: {
      HIMC imc = ImmGetContext(hwnd);
      if (!imc) break;
      // A result first: an IME may commit and start anew in one message.
      if (lp & GCS_RESULTSTR) {
        char* r = ime_string(imc, GCS_RESULTSTR);
        ime_preedit(w, NULL);
        if (r) {
          ghostty_surface_text(w->surface, r, strlen(r));
          ime_event(w, "commit", r);
        }
        free(r);
      }
      if (lp & GCS_COMPSTR) {
        char* c = ime_string(imc, GCS_COMPSTR);
        ime_preedit(w, c);
        ime_event(w, "preedit", c);
        free(c);
      }
      ime_place(w, imc);  // the preedit moved the cursor's end
      ImmReleaseContext(hwnd, imc);
      *out = 0;  // not DefWindowProc: no WM_IME_CHAR with the same text
      return true;
    }

    case WM_IME_ENDCOMPOSITION:
      ime_preedit(w, NULL);
      w->ime_composing = false;
      ime_event(w, "end", NULL);
      *out = 0;
      return true;

    case WM_IME_NOTIFY:
      if (wp == IMN_OPENCANDIDATE || wp == IMN_CHANGECANDIDATE) {
        HIMC imc = ImmGetContext(hwnd);
        if (imc) {
          ime_place(w, imc);
          ImmReleaseContext(hwnd, imc);
        }
      }
      break;  // DefWindowProc shows the candidates
  }
  return false;
}

// For the test pipe ("ime"): drive the pane's input context the way an IME
// does. comp: the composition string (the IME is opened); commit: true to
// complete it, false to cancel it. Answers what the host has seen.
void ime_drive(Window* w, const char* comp, int commit, Line* b) {
  HWND hwnd = w->hwnd;
  HIMC imc = ImmGetContext(hwnd);
  if (!imc) {
    put(b, ",\"error\":\"ime: no input context\"");
    return;
  }
  if (comp) {
    ImmSetOpenStatus(imc, TRUE);
    WCHAR* wc = utf8_to_utf16(comp, -1);
    if (wc) {
      ImmSetCompositionStringW(imc, SCS_SETSTR, wc, (DWORD)(wcslen(wc) * sizeof(WCHAR)), NULL, 0);
      free(wc);
    }
  }
  if (commit == 1) ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
  if (commit == 0) ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
  CANDIDATEFORM cand = {0};
  bool have = ImmGetCandidateWindow(imc, 0, &cand);
  COMPOSITIONFORM cf = {0};
  ImmGetCompositionWindow(imc, &cf);
  ImmReleaseContext(hwnd, imc);
  put(b, ",\"composing\":%s,\"preedit\":", w->ime_composing ? "true" : "false");
  put_string(b, w->ime_preedit ? w->ime_preedit : "");
  put(b, ",\"cursor\":[%d,%d]", w->ime_x, w->ime_y);
  if (have)
    put(b, ",\"candidate\":{\"style\":%lu,\"pos\":[%ld,%ld],\"exclude\":[%ld,%ld,%ld,%ld]}",
        (unsigned long)cand.dwStyle, cand.ptCurrentPos.x, cand.ptCurrentPos.y, cand.rcArea.left,
        cand.rcArea.top, cand.rcArea.right, cand.rcArea.bottom);
  put(b, ",\"composition_window\":{\"style\":%lu,\"pos\":[%ld,%ld]}", (unsigned long)cf.dwStyle,
      cf.ptCurrentPos.x, cf.ptCurrentPos.y);
}
