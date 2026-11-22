#include "host.h"

// ---------------------------------------------------------------------------
// Presenting frames

void take_frame(Window* w) {
  ghostty_frame_s f;
  if (!ghostty_surface_take_frame(w->surface, &f)) return;

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
    uint8_t* dst = w->bgra + (size_t)y * f.width * 4;
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
  ghostty_frame_free(&f);
}

void paint(Window* w) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(w->hwnd, &ps);
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
  } else {
    FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
  }
  RestoreDC(dc, saved);
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
