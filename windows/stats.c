#include "host.h"

#include <stdarg.h>

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

// The numbers are taken for every window every 500 ms on one timer (the
// wakeup window's), while an overlay shows or the stats pipe is on.
#define STATS_TIMER 7

static bool g_stats_timer;
static uint64_t g_stats_qpc, g_stats_start_qpc, g_stats_seq;
static ULONGLONG g_stats_cpu;  // process CPU time (100 ns) at g_stats_qpc
static double g_cpu_pct, g_ws_mb, g_private_mb, g_period_ms;

// One window's period: each tab's, and the selected tab's for the overlay.
static void frame_tick(Frame* f, double dt_ms) {
  for (int i = 0; i < f->ntabs; i++) {
    Window* w = f->tabs[i];
    w->fps = dt_ms > 0 ? w->tick_frames * 1000.0 / dt_ms : 0;
    w->swizzle_avg_ms = w->tick_frames ? w->tick_swizzle_ms / w->tick_frames : 0;
    w->blit_avg_ms = w->tick_frames ? w->tick_blit_ms / w->tick_frames : 0;
    w->tick_frames = 0;
    w->tick_swizzle_ms = w->tick_blit_ms = 0;
  }
  Window* w = f->active >= 0 ? f->tabs[f->active] : NULL;
  f->fps = w ? w->fps : 0;
  f->swizzle_ms = w ? w->swizzle_avg_ms : 0;
  f->blit_ms = w ? w->blit_avg_ms : 0;
  f->cpu_pct = g_cpu_pct;
  f->ws_mb = g_ws_mb;
  f->private_mb = g_private_mb;
  if (f->stats && w) InvalidateRect(w->hwnd, NULL, FALSE);
}

// A growing string for the pipe's line.
typedef struct {
  char* p;
  size_t len, cap;
} Line;

static void put(Line* b, const char* fmt, ...) {
  va_list ap;
  for (;;) {
    size_t room = b->cap - b->len;
    va_start(ap, fmt);
    int n = b->p ? vsnprintf(b->p + b->len, room, fmt, ap) : -1;
    va_end(ap);
    if (b->p && n >= 0 && (size_t)n < room) {
      b->len += (size_t)n;
      return;
    }
    size_t cap = b->cap ? b->cap * 2 : 2048;
    if (n >= 0 && cap < b->len + (size_t)n + 1) cap = b->len + (size_t)n + 1;
    char* q = realloc(b->p, cap);
    if (!q) return;
    b->p = q;
    b->cap = cap;
  }
}

// A JSON string: quotes, backslashes and control characters escaped.
static void put_string(Line* b, const char* s) {
  put(b, "\"");
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\')
      put(b, "\\%c", c);
    else if (c < 0x20)
      put(b, "\\u%04x", c);
    else
      put(b, "%c", c);
  }
  put(b, "\"");
}

static void publish_line(void) {
  Line b = {0};
  put(&b,
      "{\"seq\":%llu,\"t_ms\":%.1f,\"period_ms\":%.1f,\"pid\":%lu,"
      "\"build\":",
      (unsigned long long)++g_stats_seq,
      (double)(g_stats_qpc - g_stats_start_qpc) * g_qpc_ms, g_period_ms,
      GetCurrentProcessId());
  put_string(&b, GHOSTTY_BUILD_TAG);
  put(&b, ",\"cpu_pct\":%.1f,\"ws_mb\":%.1f,\"private_mb\":%.1f,\"gl\":",
      g_cpu_pct, g_ws_mb, g_private_mb);
  put_string(&b, gl_driver());
  put(&b, ",\"windows\":[");
  int fi = 0;
  for (Frame* f = g_frames; f; f = f->next, fi++) {
    put(&b, "%s{\"fullscreen\":%s,\"overlay\":%s,\"tabs\":[", fi ? "," : "",
        f->fullscreen ? "true" : "false", f->stats ? "true" : "false");
    for (int i = 0; i < f->ntabs; i++) {
      Window* w = f->tabs[i];
      ghostty_surface_size_s sz = ghostty_surface_size(w->surface);
      char* title = w->title ? utf16_to_utf8(w->title, -1) : NULL;
      put(&b, "%s{\"selected\":%s,\"title\":", i ? "," : "",
          i == f->active ? "true" : "false");
      put_string(&b, title ? title : "");
      free(title);
      put(&b,
          ",\"fps\":%.1f,\"key_ms\":%.1f,\"swizzle_ms\":%.3f,"
          "\"blit_ms\":%.3f,\"frame\":[%u,%u],\"grid\":[%u,%u],"
          "\"cell\":[%u,%u]}",
          w->fps, w->latency_ms, w->swizzle_avg_ms, w->blit_avg_ms, w->frame_w,
          w->frame_h, sz.columns, sz.rows, sz.cell_width_px, sz.cell_height_px);
    }
    put(&b, "]}");
  }
  put(&b, "]}");
  if (b.p) stats_ipc_publish(b.p, b.len);
  free(b.p);
}

void stats_tick_all(void) {
  uint64_t now = qpc();
  double dt_ms = (double)(now - g_stats_qpc) * g_qpc_ms;
  FILETIME c, e, k, u;
  ULONGLONG cpu = 0;
  if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
    cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
          (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
  PROCESS_MEMORY_COUNTERS_EX m = {sizeof(m)};
  if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m))) {
    g_ws_mb = m.WorkingSetSize / 1048576.0;
    g_private_mb = m.PrivateUsage / 1048576.0;
  }
  bool baseline = !g_stats_qpc || dt_ms <= 0;
  // CPU in percent of one core (100 ns units over milliseconds).
  g_cpu_pct = baseline ? 0 : (double)(cpu - g_stats_cpu) / 1e4 / dt_ms * 100.0;
  g_period_ms = baseline ? 0 : dt_ms;
  for (Frame* f = g_frames; f; f = f->next) frame_tick(f, baseline ? 0 : dt_ms);
  g_stats_qpc = now;
  g_stats_cpu = cpu;
  if (!baseline && stats_ipc_on()) publish_line();
}

// The timer runs while anyone looks: an overlay, or the pipe.
static void stats_timer_update(void) {
  bool want = stats_ipc_on();
  for (Frame* f = g_frames; f && !want; f = f->next) want = f->stats;
  if (want && !g_stats_timer) {
    g_stats_qpc = 0;
    stats_tick_all();  // the baseline
    g_stats_timer = SetTimer(g_wakeup_hwnd, STATS_TIMER, 500, NULL) != 0;
  } else if (!want && g_stats_timer) {
    KillTimer(g_wakeup_hwnd, STATS_TIMER);
    g_stats_timer = false;
  }
}

// At start, once the wakeup window exists: the pipe, and its timer.
void stats_start(void) {
  g_stats_start_qpc = qpc();
  stats_ipc_start();
  stats_timer_update();
}

void set_stats(Frame* f, bool on) {
  f->stats = on;
  stats_timer_update();
  if (f->active >= 0) InvalidateRect(f->tabs[f->active]->hwnd, NULL, FALSE);
}

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
