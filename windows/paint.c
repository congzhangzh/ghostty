#include "host.h"

// ---------------------------------------------------------------------------
// Presenting frames

void take_frame(Window* w) {
  ghostty_frame_s f = {0};  // zeroed: a lib older than this header leaves its new fields (shared) alone
  if (!ghostty_surface_take_frame(w->surface, &f)) return;

  // A new pane's first frame must be at the size the host gave it. The
  // renderer can draw one before that, at the surface's starting size and
  // not yet painted (on windows-latest: a white box over part of a new tab
  // for a frame, lane B's demo). Dropped until then (10 at most, so a pane
  // is never left blank); after the first, every frame goes (a resize
  // shows the old size until the new one comes).
  if (!w->shown && f.pixels && w->dropped < 10 && (f.width != w->set_w || f.height != w->set_h)) {
    w->dropped++;
    char e[160];
    snprintf(e, sizeof(e), "{\"kind\":\"frame\",\"what\":\"dropped\",\"pane\":%d,\"size\":[%u,%u],\"want\":[%u,%u]}",
             w->id, f.width, f.height, w->set_w, w->set_h);
    test_event(e);
    ghostty_frame_free(&f);
    return;
  }
  if (!w->shown) {
    w->shown = true;
    char e[160];
    snprintf(e, sizeof(e), "{\"kind\":\"frame\",\"what\":\"first\",\"pane\":%d,\"size\":[%u,%u],\"want\":[%u,%u],\"dropped\":%d}",
             w->id, f.width, f.height, w->set_w, w->set_h, w->dropped);
    test_event(e);
  }

  if (!f.pixels && f.shared && w->dxgi) {
    // Interop: drawn into a shared texture; the GPU copies it to the
    // swap chain (dxgi.c). No pixels went through the CPU.
    if (dxgi_present_shared(w, f.shared - 1, f.width, f.height)) {
      w->tick_frames++;
      w->tick_readback_ms += f.readback_ns / 1e6;
      if (w->key_qpc) {
        w->latency_ms = (double)(qpc() - w->key_qpc) * g_qpc_ms;
        w->key_qpc = 0;
      }
      w->frame_w = f.width;
      w->frame_h = f.height;
    }
    ghostty_frame_free(&f);
    return;
  }
  if (!f.pixels) {
    // Swapped into the window by the renderer: on screen already. Counted
    // for the stats; the key that it answers is on screen now.
    w->swap = true;
    w->tick_frames++;
    w->tick_readback_ms += f.readback_ns / 1e6;
    if (w->key_qpc) {
      w->latency_ms = (double)(qpc() - w->key_qpc) * g_qpc_ms;
      w->key_qpc = 0;
    }
    w->frame_w = f.width;
    w->frame_h = f.height;
    ghostty_frame_free(&f);
    return;
  }
  w->swap = false;

  size_t need = (size_t)f.width * f.height * 4;
  if (need > w->bgra_cap) {
    uint8_t* p = realloc(w->bgra, need);
    if (!p) {
      ghostty_frame_free(&f);
      return;
    }
    w->bgra = p;
    w->bgra_cap = need;
  }
  // Premultiplied RGBA to the BGRA GDI wants.
  uint64_t t0 = qpc();
  for (uint32_t y = 0; y < f.height; y++) {
    const uint8_t* src = f.pixels + (size_t)y * f.stride;
    // GL's rows come bottom first: GDI takes them so (a bottom-up DIB);
    // a D3D texture is top first, so the DXGI path writes them reversed.
    uint32_t row = w->dxgi ? f.height - 1 - y : y;
    uint8_t* dst = w->bgra + (size_t)row * f.width * 4;
    for (uint32_t x = 0; x < f.width; x++) {
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
      dst[3] = src[3];
      src += 4;
      dst += 4;
    }
  }
  w->tick_swizzle_ms += (double)(qpc() - t0) * g_qpc_ms;
  w->tick_frames++;
  w->tick_gpu_wait_ms += f.gpu_wait_ns / 1e6;
  w->tick_readback_ms += f.readback_ns / 1e6;
  if (w->key_qpc) {
    w->frame_key_qpc = w->key_qpc;
    w->key_qpc = 0;
  }
  w->frame_w = f.width;
  w->frame_h = f.height;
  // The DXGI path: on screen now, no GDI paint (a failure: GDI from now on).
  if (w->dxgi && dxgi_present(w, w->bgra, f.width, f.height, f.width * 4) && w->frame_key_qpc) {
    w->latency_ms = (double)(qpc() - w->frame_key_qpc) * g_qpc_ms;
    w->frame_key_qpc = 0;
  }
  ghostty_frame_free(&f);
}

// ---- frames waiting for their swap chain (the DXGI path) -------------------------
//
// A DXGI pane's frame is not presented in the RENDER action. The core's tick
// drains its mailbox in one loop while the renderer keeps adding frames, so
// the vsync wait that was there (up to 100 ms a frame) kept a single tick
// running for seconds. Nothing else ran on the UI thread meanwhile: the stats
// tick (WM_TIMER) came every 7-30 s, and the watchdog's WM_NULL went
// unanswered for 5 s, in every dxgi and interop run on a real GPU
// (2026-11-25). Now RENDER marks the pane, and the main loop
// (main.c) takes its newest frame and presents it once the swap chain has
// room: its frame latency waitable is signaled, waited for together with the
// messages (MsgWaitForMultipleObjectsEx), or 100 ms have passed (as before:
// an occluded window may not signal). The UI thread never blocks on it.

#define PENDING_MAX 63  // MsgWaitForMultipleObjectsEx: MAXIMUM_WAIT_OBJECTS - 1
#define PENDING_MAX_MS 100

static Window* g_pending[PENDING_MAX];
static int g_npending;

void frame_pending_add(Window* w) {
  if (w->frame_pending) return;  // the core keeps only the newest frame
  // Only a tick the main loop dispatched itself: in a modal loop (a window
  // being moved or sized, a menu, a dialog) the main loop does not run, so
  // the frame goes out at once, as before (a live resize keeps drawing).
  if (g_npending == PENDING_MAX || g_loop_msg != WM_GHOSTTY_WAKEUP || g_tick_depth != 1) {
    if (w->dxgi && w->dxgi_wait) WaitForSingleObjectEx(w->dxgi_wait, PENDING_MAX_MS, TRUE);  // room, as before
    double hold = w->dxgi ? dxgi_test_hold_ms(w) : 0;
    if (hold > 0) Sleep((DWORD)hold + 1);
    take_frame(w);
    if (!w->dxgi) InvalidateRect(w->hwnd, NULL, FALSE);  // DXGI failed on it: GDI paints it
    return;
  }
  w->frame_pending = true;
  w->pending_qpc = qpc();
  g_pending[g_npending++] = w;
}

void frame_pending_drop(Window* w) {
  for (int i = 0; i < g_npending; i++)
    if (g_pending[i] == w) {
      g_pending[i] = g_pending[--g_npending];
      break;
    }
  w->frame_pending = false;
}

static void present_now(Window* w) {
  frame_pending_drop(w);
  w->tick_wait_ms += (double)(qpc() - w->pending_qpc) * g_qpc_ms;  // the time it waited for room
  take_frame(w);
  if (!w->dxgi) InvalidateRect(w->hwnd, NULL, FALSE);  // DXGI failed on it: GDI paints it
}

// The latency waitable of w was signaled in the main loop's wait (which took
// its count): present w's newest frame.
void frame_pending_signaled(Window* w) {
  if (w->frame_pending) present_now(w);
}

// Present the panes with room (or which waited 100 ms); the handles of the
// others go in hs (their panes in ws), for the main loop's wait, and
// timeout_ms is lowered to when the first of them has waited 100 ms.
int frames_pending_wait(HANDLE* hs, Window** ws, int max, DWORD* timeout_ms) {
  int n = 0;
  for (int i = 0; i < g_npending;) {
    Window* w = g_pending[i];
    double age = (double)(qpc() - w->pending_qpc) * g_qpc_ms;
    double hold = w->dxgi ? dxgi_test_hold_ms(w) : 0;  // a test's vsync (dxgi.c), else 0
    if (!w->dxgi || age >= PENDING_MAX_MS || n == max ||
        (hold <= 0 && (!w->dxgi_wait || WaitForSingleObject(w->dxgi_wait, 0) == WAIT_OBJECT_0))) {
      present_now(w);  // g_pending[i] is another one now
      continue;
    }
    DWORD left = (DWORD)(PENDING_MAX_MS - age) + 1;
    if (hold > 0) {
      if ((DWORD)hold + 1 < left) left = (DWORD)hold + 1;
    } else {
      hs[n] = w->dxgi_wait;
      ws[n++] = w;
    }
    if (left < *timeout_ms) *timeout_ms = left;
    i++;
  }
  return n;
}

// ---- the progress bar on a pane (macOS's SurfaceProgressBar) -------------------
//
// OSC 9;4 progress, 2 points high along the pane's top, over the terminal:
// the accent color (Windows'), red on an error, orange paused; its share of
// the width when there is a number (paused without one: full), else a
// quarter that bounces end to end (a timer while one does).

#define PROGRESS_TIMER 33
#define PROGRESS_TICK 50  // ms

static COLORREF accent_color(void) {
  DWORD c = 0;
  BOOL opaque;
  if (DwmGetColorizationColor(&c, &opaque) == S_OK && c) return RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
  return RGB(0, 120, 215);
}

bool pane_has_progress(Window* w) {
  int state, value;
  return notify_pane_progress(w, &state, &value);
}

bool progress_draw(Window* w, HDC dc, int width) {
  int state, value;
  if (!notify_pane_progress(w, &state, &value) || width <= 0) return false;
  int h = (int)(2 * w->scale + 0.5);
  if (h < 2) h = 2;
  COLORREF color = state == GHOSTTY_PROGRESS_STATE_ERROR   ? RGB(232, 17, 35)
                   : state == GHOSTTY_PROGRESS_STATE_PAUSE ? RGB(255, 140, 0)
                                                           : accent_color();
  if (value < 0 && state == GHOSTTY_PROGRESS_STATE_PAUSE) value = 100;
  RECT r = {0, 0, 0, h};
  if (value >= 0) {
    r.right = width * (value > 100 ? 100 : value) / 100;
  } else {  // bouncing: a quarter of the width, end to end and back, 2 s a round
    int seg = width / 4, span = width - seg;
    double t = (double)(GetTickCount64() % 2000) / 1000.0;  // 0..2
    double at = t < 1 ? t : 2 - t;
    r.left = (int)(span * at);
    r.right = r.left + seg;
  }
  HBRUSH b = CreateSolidBrush(color);
  FillRect(dc, &r, b);
  DeleteObject(b);
  return true;
}

static bool bouncing(Window* w) {
  int state, value;
  return notify_pane_progress(w, &state, &value) && value < 0 && state != GHOSTTY_PROGRESS_STATE_PAUSE;
}

// A pane's progress changed: draw it again (no new frame may come), and
// run the bounce's timer while any pane of its window bounces.
void progress_changed(Window* w) {
  if (!w || !w->hwnd || !w->frame) return;
  w->repaint_once = true;  // a bar that went must go from the screen too
  InvalidateRect(w->hwnd, NULL, FALSE);
  Frame* f = w->frame;
  bool any = false;
  for (int i = 0; i < f->ntabs && !any; i++) {
    Window* panes[64];
    int n = tab_panes(f->tabs[i]->tab, panes, 64);
    if (n <= 0) panes[0] = f->tabs[i], n = 1;
    for (int k = 0; k < n && k < 64; k++) any = any || bouncing(panes[k]);
  }
  if (any)
    SetTimer(f->hwnd, PROGRESS_TIMER, PROGRESS_TICK, NULL);
  else
    KillTimer(f->hwnd, PROGRESS_TIMER);
}

bool progress_timer(Frame* f, WPARAM id) {
  if (id != PROGRESS_TIMER) return false;
  bool any = false;
  for (int i = 0; i < f->ntabs; i++) {
    Window* panes[64];
    int n = tab_panes(f->tabs[i]->tab, panes, 64);
    if (n <= 0) panes[0] = f->tabs[i], n = 1;
    for (int k = 0; k < n && k < 64; k++)
      if (bouncing(panes[k])) {
        any = true;
        if (IsWindowVisible(panes[k]->hwnd)) InvalidateRect(panes[k]->hwnd, NULL, FALSE);
      }
  }
  if (!any) KillTimer(f->hwnd, PROGRESS_TIMER);
  return true;
}

void paint(Window* w) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w->hwnd, &ps);
  if (w->dxgi) {
    // The DXGI swap chain owns the window's pixels; the stats overlay is
    // drawn on its back buffer (dxgi.c), and goes out again here when it
    // changed without a new frame.
    // Before its first frame (no swap chain yet), the pane is the theme's
    // background, not whatever the window had.
    if (!w->dxgi_chain) {
      RECT rc;
      GetClientRect(w->hwnd, &rc);
      HBRUSH bg = CreateSolidBrush(g_bg);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
    }
    EndPaint(w->hwnd, &ps);
    dxgi_repaint(w);
    return;
  }
  if (w->swap) {
    // OpenGL owns the window's pixels (the stats overlay is not drawn in
    // this mode yet; the stats pipe still has the numbers).
    EndPaint(w->hwnd, &ps);
    return;
  }
  RECT rc;
  GetClientRect(w->hwnd, &rc);
  // The stats overlay's box is left out of the frame's blit and drawn once
  // on its own: every pixel on screen is written once per paint, so the box
  // never shows the frame under it for a moment (a flash).
  RECT box;
  WCHAR* stats = w->frame->stats ? stats_layout(w, dc, rc, &box) : NULL;
  int saved = SaveDC(dc);
  if (stats) ExcludeClipRect(dc, box.left, box.top, box.right, box.bottom);
  if (w->bgra && w->frame_w && w->frame_h) {
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = (LONG)w->frame_w;
    // Positive: a bottom-up DIB, the row order OpenGL reads pixels in.
    bmi.bmiHeader.biHeight = (LONG)w->frame_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    uint64_t t0 = qpc();
    SetDIBitsToDevice(dc, 0, 0, w->frame_w, w->frame_h, 0, 0, 0, w->frame_h,
                      w->bgra, &bmi, DIB_RGB_COLORS);
    w->tick_blit_ms += (double)(qpc() - t0) * g_qpc_ms;
    // While a resize waits for its frame, fill the uncovered part.
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if ((LONG)w->frame_w < rc.right) {
      RECT r = {(LONG)w->frame_w, 0, rc.right, rc.bottom};
      FillRect(dc, &r, black);
    }
    if ((LONG)w->frame_h < rc.bottom) {
      RECT r = {0, (LONG)w->frame_h, (LONG)w->frame_w, rc.bottom};
      FillRect(dc, &r, black);
    }
  } else {  // no frame yet: the theme's background
    HBRUSH bg = CreateSolidBrush(g_bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
  }
  RestoreDC(dc, saved);
  progress_draw(w, dc, rc.right);  // over the frame, under the stats box
  if (stats) {
    present_stats(w, dc, box, stats);
    free(stats);
  }
  EndPaint(w->hwnd, &ps);
  // The key that this frame was the first answer to is now on screen.
  if (w->frame_key_qpc) {
    w->latency_ms = (double)(qpc() - w->frame_key_qpc) * g_qpc_ms;
    w->frame_key_qpc = 0;
  }
}
