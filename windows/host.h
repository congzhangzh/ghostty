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
#include <shellscalingapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <fcntl.h>
#include <io.h>

#include <signal.h>
#include <ctype.h>
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

// Splits: a tab is a tree of panes. A Node is a pane (a leaf) or a split of
// two nodes, side by side (left|right) or `vertical` (above|below), the
// first taking `ratio` of the room. Every pane of a tab points to the Tab.
typedef struct Window Window;
typedef struct Node Node;
struct Node {
  Node* parent;
  Node* a;
  Node* b;
  Window* pane;  // a leaf: this pane; a split: NULL
  bool vertical;
  double ratio;
};
typedef struct Tab {
  Node* root;
  Window* zoomed;  // a pane that takes the whole tab, or NULL
} Tab;

// What a point of the tab bar is (tabs.c).
typedef enum {
  HIT_NONE,
  HIT_TAB,
  HIT_CLOSE,
  HIT_PLUS,
  HIT_HELP,    // the "?" left of Windows' caption buttons: the overview
  HIT_EXITFS,  // the reveal bar's exit-full-screen button (reveal.c)
} BarHit;

// A tab: a child window of its Frame, with one surface drawing into it.
struct Window {
  int id;  // never reused in this process (the test-control pipe's "pane")
  Frame* frame;
  Tab* tab;    // the tab this pane is in (shared by its panes)
  Node* node;  // its leaf in the tab's tree
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
  // IME (ime.c): composing, the preedit Ghostty draws, the cursor point
  // the candidates were placed at (pixels, the pane's).
  bool ime_composing;
  char* ime_preedit;
  int ime_x, ime_y;
  // The keys whose press Ghostty got, by scan code (0x100: extended): a
  // release goes only after its press (input.c).
  uint8_t pressed[64];

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
  // The DXGI path (dxgi.c): the pane's flip swap chain, its latency object,
  // its size; the time waiting for room and presenting, per frame.
  bool dxgi;
  void* dxgi_chain;
  HANDLE dxgi_wait;
  UINT dxgi_w, dxgi_h;
  double tick_wait_ms, tick_present_ms, wait_avg_ms, present_avg_ms;
  // Interop (dxgi.c): the two textures the renderer draws into, their size,
  // the pair before a resize (kept until the renderer moves), frames so far.
  void* shared_tex[2];
  void* shared_old[2];
  UINT shared_w, shared_h;
  uint64_t interop_frames;
  int last_shared;          // the shared texture last presented, -1 none
  // The size the host last gave the surface, and whether a frame has been
  // shown yet: the first one must have that size (paint.c take_frame).
  uint32_t set_w, set_h;
  bool shown;
  int dropped;
  bool repaint_once;  // present the last frame again once (an overlay went: dxgi.c)
  // A DXGI pane's frame waiting for room in its swap chain (paint.c: the
  // main loop presents it), and since when.
  bool frame_pending;
  uint64_t pending_qpc;
  uint64_t last_present_qpc;  // the DXGI path's last Present (dxgi.c: GHOSTTY_TEST_VSYNC_MS)
  int notes;                  // desktop notifications received (OSC 9/777)
  char *note_title, *note_body;  // the last one's, as received (UTF-8): for the test pipe
  bool attention;  // the bell rang, or a notification came while not looked at (tabs.c)
  uint64_t overlay_frames;  // frames the stats overlay was drawn on (DXGI path)
  HDC ov_dc;                // the readback DXGI path's overlay: a DIB of the frame (dxgi.c)
  HBITMAP ov_bmp;
  void* ov_bits;
  UINT ov_w, ov_h;
  uint64_t key_qpc;        // a key went down, no frame taken since
  uint64_t frame_key_qpc;  // the frame shown now is the first after it
  double latency_ms;       // that key to the end of its frame's paint
  // Over the last stats period: frames per second, host ms per frame.
  double fps, swizzle_avg_ms, blit_avg_ms, gpu_wait_avg_ms, readback_avg_ms;
};

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
  int slogan_shown;  // what the bar's gap showed last: 0 nothing, 1 the motto, 2 the slogan
  bool quick;  // the quick terminal (quick.c): also fullscreen, for the caption-less path
  WINDOWPLACEMENT placement;

  bool destroying;  // DestroyWindow is taking its tabs down with it

  // The tab bar under the mouse: a tab (and whether its close button),
  // the new-tab button, or nothing.
  int hover;
  bool hover_close, hover_plus, hover_help, hover_exitfs, tracking;
  // Full screen's reveal bar (reveal.c): its window, shown, the dwell and
  // the time away (ms), shown until (a tick), and "being painted or hit
  // tested now" (bar_height then answers the bar's height).
  HWND reveal;
  bool reveal_on, reveal_painting;
  int reveal_dwell, reveal_away;
  uint64_t reveal_hold;

  // Tabs in the title bar: the width and height Windows' caption buttons
  // take (client pixels), and a tab pressed that may become a window drag.
  int buttons_w, buttons_h;
  WPARAM nc_pressed;  // a caption button pressed (HTCLOSE…) the host acts on
  Node* drag;         // a split whose divider the mouse is dragging
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
extern UINT g_loop_msg;                // the message the main loop is dispatching (main.c)
extern int g_tick_depth;               // ghostty_app_tick calls running (main.c)
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
bool ime_message(Window* w, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out);
void present_mode_resolve(void);
bool present_dxgi_wanted(void);
bool dxgi_present(Window* w, const uint8_t* bgra, UINT width, UINT height, UINT stride);
void dxgi_pane_gone(Window* w);
bool dxgi_present_shared(Window* w, unsigned i, UINT width, UINT height);
void dxgi_repaint(Window* w);
extern const char* ghostty_windows_gl_driver(void);  // pkg/opengl/wgl.zig
void drop_files(Window* w, HDROP drop);
void mouse_pos(Window* w, LPARAM lp);
void mouse_button(Window* w, ghostty_input_mouse_state_e state,
                         ghostty_input_mouse_button_e button);
void take_frame(Window* w);
void frame_pending_add(Window* w);
void frame_pending_drop(Window* w);
int frames_pending_wait(HANDLE* hs, Window** ws, int max, DWORD* timeout_ms);
void frame_pending_signaled(Window* w);
double dxgi_test_hold_ms(Window* w);
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
bool new_split(Window* w, ghostty_action_split_direction_e dir);
bool goto_split(Window* w, ghostty_action_goto_split_e to);
bool resize_split(Window* w, ghostty_action_resize_split_s r);
bool equalize_splits(Window* w);
bool toggle_split_zoom(Window* w);
Node* divider_at(Frame* f, POINT client);
void divider_drag_start(Frame* f, Node* n);
void divider_drag_to(Frame* f, POINT client);
LPCWSTR divider_cursor(Node* n);
void pane_gone(Frame* f, Window* w);
void pane_focused(Window* w);
int tab_panes(Tab* t, Window** out, int cap);
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
extern const WCHAR* const kSlogan;  // the motto and the family line (about.c)
bool slogan_shown(void);
void about_menu_add(HWND hwnd);
bool about_command(HWND hwnd, WPARAM wp);
void show_about(HWND owner);
void show_whatsnew(HWND owner);
void whatsnew_once(HWND owner);
// A growing string (stats.c): JSON lines for the stats and test pipes.
typedef struct {
  char* p;
  size_t len, cap;
} Line;
void put(Line* b, const char* fmt, ...);
void put_string(Line* b, const char* s);
void stats_json(Line* b);
bool pipe_security(SECURITY_ATTRIBUTES* sa);
bool pipe_client_same_user(HANDLE pipe);
#define WM_GHOSTTY_TESTCTL (WM_APP + 3)  // a test-control request (testctl.c)
void testctl_start(void);
bool ctl_client_mode(int argc, char** argv, int* rc);
LRESULT testctl_ui(LPARAM req);
void test_event(const char* json_object);
void ime_drive(Window* w, const char* comp, int commit, Line* b);
void present_stats_json(Line* b);  // dxgi.c
const char* dxgi_adapter(void);
ghostty_config_t config_load_with(const char* extra);
void config_reload(ghostty_surface_t surface, bool soft);
char* theme_config_file(void);
int scheme_mode(void);
void scheme_cycle(void);
void quick_toggle(void);
void palette_toggle(Window* w);
void sheet_toggle(Window* w);
// The bell's mark on a tab (as macOS's "🔔 " title), and the progress bar
// on a pane (as macOS's SurfaceProgressBar): tabs.c, paint.c.
void attention_set(Window* w, bool on);
void bell_rang(Frame* f, Window* w);  // callbacks.c
bool notify_pane_progress(Window* w, int* state, int* value);
bool progress_draw(Window* w, HDC dc, int width);  // false: none to draw
bool pane_has_progress(Window* w);
void progress_changed(Window* w);
bool progress_timer(Frame* f, WPARAM id);  // the cheat sheet (sheet.c); NULL closes it
bool help_rect(Frame* f, RECT* r);
bool exitfs_rect(Frame* f, RECT* r);
void draw_bar(Frame* f, HDC dc, int width, int b);
void reveal_fullscreen(Frame* f, bool on);
bool reveal_timer(Frame* f, WPARAM id);
bool reveal_shown(Frame* f);
bool open_url(ghostty_action_open_url_kind_e kind, const char* url, size_t len, bool dry);
void trigger_text(ghostty_input_trigger_s t, WCHAR* out, size_t cap);
const WCHAR* default_key(const char* action);  // sheet.c: keys the lookup can't give
bool quick_timer(Frame* f, WPARAM id);
void quick_activate(Frame* f, WPARAM wp, LPARAM lp);
void quick_gone(Frame* f);
void quick_hotkey_update(void);
bool quick_hotkey(WPARAM id);
bool quick_retry(WPARAM id);
extern bool g_batch;  // GHOSTTY_BATCH=1 (main.c)
const WCHAR* theme_hint(void);
void hint_show(const WCHAR* text);
const WCHAR* tr(const WCHAR* en);  // the display language's text (i18n.c)
const char* tr_lang(void);
bool host_ini_get(const char* key, char* out, size_t cap);
void host_ini_set(const char* key, const char* value);  // a line in the title bar for a few seconds (theme.c)
bool theme_timer(Frame* f, WPARAM id);
void theme_menu_add(HMENU sys);
void theme_menu_update(HMENU sys);
bool theme_command(HWND hwnd, WPARAM wp);
void theme_picker_open(Frame* f);
// theme.c's small files beside the config (%LOCALAPPDATA%\ghostty), the
// preview and the picker, for font.c and shell.c too.
bool host_path(const WCHAR* name, WCHAR* out, size_t cap);
void write_text(const WCHAR* name, const char* text);
bool read_value(const WCHAR* name, const char* key, char* out, size_t cap);
void config_preview(Frame* f, const char* text);
void picker_open(Frame* f, const WCHAR* title, char** names, int n, void (*preview_fn)(const char*),
                 void (*choose_fn)(const char*));
void menus_update(void);
// Font ▸ (font.c) and Shell ▸ (shell.c) in the window menu.
char* font_config_file(void);
void font_menu_add(HMENU sys);
void font_menu_update(HMENU sys);
bool font_command(HWND hwnd, WPARAM wp);
char* shell_config_file(void);
void shell_menu_add(HMENU sys);
void shell_menu_update(HMENU sys);
bool shell_command(HWND hwnd, WPARAM wp);
void config_changed(ghostty_config_t config);
void config_open(void);
void color_scheme_apply(void);
ghostty_color_scheme_e color_scheme_now(void);
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
// glcheck.c: why the first window failed (OpenGL 4.3 missing?), in a message box.
void glcheck_explain(void);
void notify_toast(Window* w, const char* title, const char* body);
void notify_progress(Window* w, ghostty_action_progress_report_s r);
void notify_frame_changed(Frame* f);
const char* notify_progress_state(Frame* f, int* value);  // for the test pipe's "list"
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
