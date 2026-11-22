// Ghostty for Windows: what the host's modules share (windows/*.c).
#pragma once

// Ghostty for Windows: a Win32 host on libghostty's embedded runtime.
//
// The macOS app is a Swift program on top of libghostty; this is the same
// shape for Windows, in C on Win32. libghostty runs the terminal, the pty
// (ConPTY) and the renderer. The renderer draws with OpenGL (WGL) into its
// own offscreen framebuffer and hands each finished frame back; this host
// owns the window: it turns Win32 messages into Ghostty input, and on
// GHOSTTY_ACTION_RENDER it takes the newest frame and blits it.
//
// A window (Frame) holds tabs; each tab (Tab) is a child window with one
// surface, and only the selected tab is shown. The tab bar shows from the
// first tab, so its + is there to find (window-show-tab-bar, whose default
// is always on Windows). Built by windows/build.sh.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
// After windows.h:
#define PSAPI_VERSION 2
#include <dbghelp.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <fcntl.h>
#include <io.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "ghostty.h"

// What `ghostty.exe /buildinfo` prints: windows/build.sh writes it (with the
// release kit's own lines first in a release build).
#ifdef GHOSTTY_HAVE_BUILDINFO_H
#include "buildinfo.h"
#else
#define GHOSTTY_BUILDINFO "build info: not recorded (built without windows/build.sh)\n"
#endif
// Which build runs, for the window title: a release ("20261122.6"), else
// the source's short commit ("+mod" when built from a changed tree).
#ifndef GHOSTTY_BUILD_TAG
#define GHOSTTY_BUILD_TAG "dev"
#endif
#define WIDEN2(s) L##s
#define WIDEN(s) WIDEN2(s)

#define WM_GHOSTTY_WAKEUP (WM_APP + 1)
// Destroy the window later: libghostty asks to close a surface from inside
// its own call stack, and freeing the surface there would pull it out from
// under that code.
#define WM_GHOSTTY_DESTROY (WM_APP + 2)

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif

typedef struct Frame Frame;

// What a point of the tab bar is (tabs.c).
typedef enum {
  HIT_NONE,
  HIT_TAB,
  HIT_CLOSE,
  HIT_PLUS,
} BarHit;

// A tab: a child window of its Frame, with one surface drawing into it.
typedef struct Window {
  Frame* frame;
  HWND hwnd;
  ghostty_surface_t surface;
  double scale;  // DPI / 96

  // The frame shown now, converted to BGRA for GDI, kept for repaints.
  uint8_t* bgra;
  uint32_t frame_w, frame_h;
  size_t bgra_cap;

  // The renderer draws into this window itself (GHOSTTY_PRESENT=swap):
  // frames come without pixels, and the host paints nothing over them.
  bool swap;

  // A high surrogate from WM_CHAR waiting for its low half.
  WCHAR high_surrogate;

  // The title the terminal set (malloc'd), or NULL.
  WCHAR* title;

  // Counters for the stats overlay (Ctrl+Shift+I), kept always (cheap):
  // frames taken and the host's time on them since the overlay's last
  // update, and a key press waiting for the frame that follows it.
  uint32_t tick_frames;
  double tick_swizzle_ms, tick_blit_ms;
  double tick_gpu_wait_ms, tick_readback_ms;  // the render thread's, per frame taken
  uint64_t key_qpc;        // a key went down, no frame taken since
  uint64_t frame_key_qpc;  // the frame shown now is the first after it
  double latency_ms;       // that key to the end of its frame's paint
  // Over the last stats period: frames per second, host ms per frame.
  double fps, swizzle_avg_ms, blit_avg_ms, gpu_wait_avg_ms, readback_avg_ms;
} Window;

// A top-level window: its tabs, the selected one, and the tab bar.
struct Frame {
  HWND hwnd;
  Frame* next;  // every open Frame, newest first (g_frames)
  Window** tabs;
  int ntabs, cap;
  int active;  // the selected tab, or -1
  double scale;

  // Full screen: the placement to go back to when it ends.
  bool fullscreen;
  WINDOWPLACEMENT placement;

  bool destroying;  // DestroyWindow is taking its tabs down with it

  // The tab bar under the mouse: a tab (and whether its close button),
  // the new-tab button, or nothing.
  int hover;
  bool hover_close, hover_plus, tracking;

  // Tabs in the title bar: the width and height Windows' caption buttons
  // take (client pixels), and a tab pressed that may become a window drag.
  int buttons_w, buttons_h;
  WPARAM nc_pressed;  // a caption button pressed (HTCLOSE…) the host acts on
  bool pressing;
  POINT press;
  HFONT font;
  UINT font_dpi;

  // The stats overlay (Ctrl+Shift+I, Ghostty's inspector key: the
  // inspector itself is not in this host yet), updated every 500 ms.
  bool stats;
  uint64_t stats_qpc;
  ULONGLONG stats_cpu;  // process CPU time (100 ns) at stats_qpc
  double fps, swizzle_ms, blit_ms, cpu_pct, ws_mb, private_mb;
  double gpu_wait_ms, readback_ms;
  HFONT stats_font;
  UINT stats_font_dpi;
};

typedef enum { TAB_BAR_AUTO, TAB_BAR_ALWAYS, TAB_BAR_NEVER } TabBarMode;


#define GHOSTTY_PANIC_CODE 0xE0474850u

extern ghostty_app_t g_app;
extern ghostty_config_t g_config;
extern HINSTANCE g_inst;
extern HWND g_wakeup_hwnd;  // message-only window for wakeups
extern volatile LONG g_wakeup_posted;  // a WM_GHOSTTY_WAKEUP is queued
extern Frame* g_frames;  // open windows
extern bool g_debug_keys;  // GHOSTTY_DEBUG_KEYS is set: trace key messages
extern DWORD g_main_tid;  // the thread that runs the message loop
extern TabBarMode g_tab_bar;  // window-show-tab-bar
extern bool g_new_tab_at_end;  // window-new-tab-position = end
extern COLORREF g_bg, g_fg;  // the terminal's colors, for the tab bar
extern double g_qpc_ms;  // milliseconds per QueryPerformanceCounter tick
extern const WCHAR* const kFrameClass;
extern const WCHAR* const kTerminalClass;
extern volatile LONG g_loop_done;  // the message loop has ended (quitting)

uint64_t qpc(void);
char* utf16_to_utf8(const WCHAR* s, int len);
WCHAR* utf8_to_utf16(const char* s, int len);
void handle_key(Window* w, UINT msg, WPARAM vk, LPARAM lp);
void handle_char(Window* w, WCHAR ch);
void mouse_pos(Window* w, LPARAM lp);
void mouse_button(Window* w, ghostty_input_mouse_state_e state,
                         ghostty_input_mouse_button_e button);
void take_frame(Window* w);
void paint(Window* w);
ghostty_clipboard_read_result_e read_clipboard_cb(
    void* ud, ghostty_clipboard_e which, void* state,
    const char* const* mimes, size_t mimes_len, bool list);
void confirm_read_clipboard_cb(void* ud,
                                      const ghostty_clipboard_confirm_s* c,
                                      void* state,
                                      ghostty_clipboard_request_e req);
void write_clipboard_cb(void* ud, ghostty_clipboard_e which,
                               const ghostty_clipboard_content_s* contents,
                               size_t len, bool confirm);
void set_fullscreen(Frame* w, bool on);
void start_reports(void);
bool report_mode(int* rc);
void stderr_guard_start(void);
void stderr_guard_flush(DWORD ms);
BarHit bar_hit(Frame* f, int x, int y, int* index);
void paint_frame(Frame* f);
void invalidate_bar(Frame* f);
int tab_index(Frame* f, Window* w);
void update_title(Frame* f);
void layout(Frame* f);
void select_tab(Frame* f, int i);
void detach_tab(Frame* f, Window* w);
bool insert_tab(Frame* f, Window* w, int at);
void size_tab(Window* w);
Window* new_tab(Frame* f, ghostty_surface_t from,
                       ghostty_surface_context_e context);
void move_tab(Frame* f, int amount);
void close_tab(Window* w);
extern bool g_titlebar_tabs;
void titlebar_choose(void);
bool titlebar_active(Frame* f);
LRESULT titlebar_hit(Frame* f, WPARAM wp, LPARAM lp);
int frame_border_y(HWND hwnd);
int bar_height(Frame* f);
COLORREF bar_color(void);
bool bar_is_dark(void);
void titlebar_update(Frame* f);
void show_window_menu(Frame* f, int x, int y);
extern const WCHAR* const kMotto;
bool slogan_shown(void);
void about_menu_add(HWND hwnd);
bool about_command(HWND hwnd, WPARAM wp);
void show_about(HWND owner);
void show_whatsnew(HWND owner);
void whatsnew_once(HWND owner);
void stats_start(void);
void stats_tick_all(void);
void set_stats(Frame* f, bool on);
void print_text(const char* s);
bool stats_ipc_on(void);
void stats_ipc_start(void);
void stats_ipc_publish(const char* line, size_t len);
bool stats_client_mode(int argc, char** argv, int* rc);
WCHAR* stats_layout(Window* w, HDC dc, RECT rc, RECT* box);
void present_stats(Window* w, HDC dc, RECT box, const WCHAR* text);
void notify_toast(Window* w, const char* title, const char* body);
void notify_progress(Window* w, ghostty_action_progress_report_s r);
void notify_frame_changed(Frame* f);
void notify_tab_closed(Window* w);
bool notify_frame_msg(HWND hwnd, UINT msg);
Frame* new_frame(void);
void show_frame(Frame* f);
bool new_window(ghostty_surface_t from);
Frame* front_frame(void);
void wakeup_cb(void* ud);
bool action_cb(ghostty_app_t app, ghostty_target_s target,
                      ghostty_action_s action);
void close_surface_cb(void* ud, bool process_alive);
LRESULT CALLBACK terminal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
