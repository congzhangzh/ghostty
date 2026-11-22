#include "host.h"

// ---------------------------------------------------------------------------
// Stats overlay
//
// Ctrl+Shift+I (Ghostty's inspector key) shows what the frame path costs,
// like our FreeRDP client's overlay: frames per second, key to screen, the
// host's time per frame (the RGBA to BGRA swizzle, the GDI blit), the
// process's CPU and memory, the grid, the OpenGL driver loaded, the build.
// The numbers to compare when the frame path changes (a BGRA readback,
// then zero-copy presentation).

// The OpenGL driver loaded in this process: which GPU really renders (on a
// laptop with two GPUs the display adapter can be the other one).
static const char* gl_driver(void) {
  static const struct { const WCHAR* dll; const char* name; } known[] = {
      {L"nvoglv64.dll", "NVIDIA (nvoglv64)"},
      {L"atio6axx.dll", "AMD (atio6axx)"},
      {L"amdxc64.dll", "AMD (amdxc64)"},
      {L"ig9icd64.dll", "Intel (ig9icd64)"},
      {L"ig11icd64.dll", "Intel (ig11icd64)"},
      {L"ig12icd64.dll", "Intel (ig12icd64)"},
      {L"igxelpicd64.dll", "Intel (igxelpicd64)"},
      {L"opengl32sw.dll", "software (opengl32sw)"},
  };
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    if (GetModuleHandleW(known[i].dll)) return known[i].name;
  return "other (Mesa or Microsoft's)";
}

void stats_tick(Frame* f) {
  uint64_t now = qpc();
  double dt_ms = (double)(now - f->stats_qpc) * g_qpc_ms;
  FILETIME c, e, k, u;
  ULONGLONG cpu = 0;
  if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
    cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
          (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
  if (f->stats_qpc && dt_ms > 0) {
    // CPU in percent of one core (100 ns units over milliseconds).
    f->cpu_pct = (double)(cpu - f->stats_cpu) / 1e4 / dt_ms * 100.0;
    Window* w = f->active >= 0 ? f->tabs[f->active] : NULL;
    if (w) {
      f->fps = w->tick_frames * 1000.0 / dt_ms;
      f->swizzle_ms = w->tick_frames ? w->tick_swizzle_ms / w->tick_frames : 0;
      f->blit_ms = w->tick_frames ? w->tick_blit_ms / w->tick_frames : 0;
    }
  }
  for (int i = 0; i < f->ntabs; i++) {
    f->tabs[i]->tick_frames = 0;
    f->tabs[i]->tick_swizzle_ms = f->tabs[i]->tick_blit_ms = 0;
  }
  PROCESS_MEMORY_COUNTERS_EX m = {sizeof(m)};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m))) {
    f->ws_mb = m.WorkingSetSize / 1048576.0;
    f->private_mb = m.PrivateUsage / 1048576.0;
  }
  f->stats_qpc = now;
  f->stats_cpu = cpu;
  if (f->active >= 0) InvalidateRect(f->tabs[f->active]->hwnd, NULL, FALSE);
}

void set_stats(Frame* f, bool on) {
  f->stats = on;
  if (on) {
    f->stats_qpc = 0;
    stats_tick(f);  // the baseline
    SetTimer(f->hwnd, 7, 500, NULL);
  } else {
    KillTimer(f->hwnd, 7);
  }
  if (f->active >= 0) InvalidateRect(f->tabs[f->active]->hwnd, NULL, FALSE);
}

// The overlay's text (malloc'd) and where its box goes in rc (the top right
// corner); dc only measures.
WCHAR* stats_layout(Window* w, HDC dc, RECT rc, RECT* box) {
  Frame* f = w->frame;
  UINT dpi = (UINT)(w->scale * 96 + 0.5);
  if (!f->stats_font || f->stats_font_dpi != dpi) {
    if (f->stats_font) DeleteObject(f->stats_font);
    f->stats_font = CreateFontW(-MulDiv(9, (int)dpi, 72), 0, 0, 0, FW_NORMAL,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    f->stats_font_dpi = dpi;
  }
  ghostty_surface_size_s sz = ghostty_surface_size(w->surface);
  // The build: the first line of /buildinfo (the release, or the source).
  char build[80];
  const char* b = GHOSTTY_BUILDINFO;
  size_t bl = strcspn(b, "\n");
  if (bl >= sizeof(build)) bl = sizeof(build) - 1;
  memcpy(build, b, bl);
  build[bl] = 0;
  char text[1024];
  snprintf(text, sizeof(text),
           "Ghostty stats (Ctrl+Shift+I)\n"
           "frames   %5.1f /s   %ux%u px\n"
           "key      %5.1f ms   key to its first frame on screen\n"
           "swizzle  %5.2f ms   RGBA to BGRA, per frame\n"
           "blit     %5.2f ms   GDI SetDIBits, per frame\n"
           "cpu      %5.1f %%    of one core (the whole process)\n"
           "memory   %5.0f MB   working set, %.0f MB private\n"
           "grid     %ux%u cells of %ux%u px, %d tab%s\n"
           "opengl   4.3 core, WGL, frames through the CPU\n"
           "driver   %s\n"
           "%s",
           f->fps, w->frame_w, w->frame_h, w->latency_ms, f->swizzle_ms,
           f->blit_ms, f->cpu_pct, f->ws_mb, f->private_mb, sz.columns,
           sz.rows, sz.cell_width_px, sz.cell_height_px, f->ntabs,
           f->ntabs == 1 ? "" : "s", gl_driver(), build);
  WCHAR* wt = utf8_to_utf16(text, -1);
  if (!wt) return NULL;
  HGDIOBJ old = SelectObject(dc, f->stats_font);
  RECT t = {0, 0, 0, 0};
  DrawTextW(dc, wt, -1, &t, DT_CALCRECT | DT_NOPREFIX);
  SelectObject(dc, old);
  int pad = (int)(8 * w->scale + 0.5);
  box->left = rc.right - (t.right + 2 * pad) - pad;
  box->top = pad;
  box->right = rc.right - pad;
  box->bottom = pad + t.bottom + 2 * pad;
  return wt;
}

// The box drawn off screen, then copied to dc in one go.
void present_stats(Window* w, HDC dc, RECT box, const WCHAR* text) {
  int bw = box.right - box.left, bh = box.bottom - box.top;
  if (bw <= 0 || bh <= 0) return;
  HDC mem = CreateCompatibleDC(dc);
  HBITMAP bmp = CreateCompatibleBitmap(dc, bw, bh);
  if (!mem || !bmp) {
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    return;
  }
  HGDIOBJ oldbmp = SelectObject(mem, bmp);
  RECT r = {0, 0, bw, bh};
  SetDCBrushColor(mem, RGB(16, 16, 20));
  FillRect(mem, &r, (HBRUSH)GetStockObject(DC_BRUSH));
  SetDCBrushColor(mem, RGB(90, 90, 100));
  FrameRect(mem, &r, (HBRUSH)GetStockObject(DC_BRUSH));
  int pad = (int)(8 * w->scale + 0.5);
  RECT tr = {pad, pad, bw - pad, bh - pad};
  HGDIOBJ oldfont = SelectObject(mem, w->frame->stats_font);
  SetBkMode(mem, TRANSPARENT);
  SetTextColor(mem, RGB(220, 220, 225));
  DrawTextW(mem, text, -1, &tr, DT_NOPREFIX);
  SelectObject(mem, oldfont);
  BitBlt(dc, box.left, box.top, bw, bh, mem, 0, 0, SRCCOPY);
  SelectObject(mem, oldbmp);
  DeleteObject(bmp);
  DeleteDC(mem);
}
