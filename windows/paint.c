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
