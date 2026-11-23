#include "host.h"

// ---------------------------------------------------------------------------
// The test-control pipe
//
// For end-to-end tests (Wine, and real Windows x64 and arm64 runners) and
// for agents: \\.\pipe\ghostty-<pid>.ctl, ONLY when GHOSTTY_TEST_CONTROL=1
// (it can type into the terminal and read the screen, so it is never on by
// itself), with the stats pipe's guards: an access list of this user and
// SYSTEM, the first instance claims the name, no remote clients, each
// client's process checked to run as this user.
//
// A request is one JSON line, the answer one JSON line, in order; a client
// may send many. Commands (protocol 1, agreed with the e2e driver):
//   hello, list, text, key, action, read, screenshot, stats, window, events
// They run on the UI thread (posted to the wakeup window, answered within
// 5 s or "ui timeout"); a screenshot is then taken from the screen by the
// pipe's thread, after a frame, so it shows what was presented in either
// present mode. `ghostty.exe /ctl [pid] '<json>'` is a client for agents.

#define CTL_INSTANCES 4
#define CTL_MAX_LINE (1 << 20)
#define EVENT_RING 256

static bool g_ctl_on;
static WCHAR g_ctl_name[64];

// ---- the event ring --------------------------------------------------------

static CRITICAL_SECTION g_ev_lock;
static char* g_events[EVENT_RING];
static uint64_t g_ev_next = 1;  // the seq the next event gets

// One host event as a JSON object ({"kind":"…",…}); kept with a seq. A no-op
// unless the pipe is on.
void test_event(const char* json_object) {
  if (!g_ctl_on || !json_object) return;
  size_t n = strlen(json_object);
  char* copy = malloc(n + 1);
  if (!copy) return;
  memcpy(copy, json_object, n + 1);
  EnterCriticalSection(&g_ev_lock);
  size_t slot = (size_t)(g_ev_next % EVENT_RING);
  free(g_events[slot]);
  g_events[slot] = copy;
  g_ev_next++;
  LeaveCriticalSection(&g_ev_lock);
}

// ---- a small JSON reader: the request's top-level keys ---------------------

typedef struct {
  const char* key;
  size_t klen;
  const char* v;  // the value's first character
  size_t vlen;
} JField;

typedef struct {
  JField f[24];
  int n;
} JObj;

static const char* jws(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  return p;
}

// Past one value (string, number, literal, array, object).
static const char* jskip(const char* p) {
  p = jws(p);
  if (*p == '"') {
    for (p++; *p && *p != '"'; p++)
      if (*p == '\\' && p[1]) p++;
    return *p ? p + 1 : p;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; p++) {
      if (*p == '"') {
        p = jskip(p) - 1;
        continue;
      }
      if (*p == '{' || *p == '[') depth++;
      if (*p == '}' || *p == ']') {
        if (--depth == 0) return p + 1;
      }
    }
    return p;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n') p++;
  return p;
}

static bool jparse(const char* s, JObj* o) {
  o->n = 0;
  const char* p = jws(s);
  if (*p != '{') return false;
  p = jws(p + 1);
  while (*p && *p != '}') {
    if (*p != '"' || o->n >= 24) return false;
    JField* f = &o->f[o->n];
    f->key = p + 1;
    const char* e = jskip(p);
    f->klen = (size_t)(e - p - 2);
    p = jws(e);
    if (*p != ':') return false;
    p = jws(p + 1);
    f->v = p;
    e = jskip(p);
    f->vlen = (size_t)(e - p);
    o->n++;
    p = jws(e);
    if (*p == ',') p = jws(p + 1);
  }
  return true;
}

static const JField* jget(const JObj* o, const char* key) {
  size_t k = strlen(key);
  for (int i = 0; i < o->n; i++)
    if (o->f[i].klen == k && memcmp(o->f[i].key, key, k) == 0) return &o->f[i];
  return NULL;
}

// A string value, unescaped into UTF-8 (malloc'd), or NULL.
static char* jstr_at(const char* v) {
  if (*v != '"') return NULL;
  size_t cap = strlen(v) + 1;
  char* out = malloc(cap);
  if (!out) return NULL;
  size_t n = 0;
  for (const char* p = v + 1; *p && *p != '"'; p++) {
    if (*p != '\\') {
      out[n++] = *p;
      continue;
    }
    p++;
    switch (*p) {
      case 'n': out[n++] = '\n'; break;
      case 'r': out[n++] = '\r'; break;
      case 't': out[n++] = '\t'; break;
      case 'b': out[n++] = '\b'; break;
      case 'f': out[n++] = '\f'; break;
      case 'u': {
        unsigned c = 0;
        for (int i = 1; i <= 4 && p[i]; i++) {
          char h = p[i];
          c = c * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
        }
        p += 4;
        // A surrogate pair: the low half follows as \uDCxx.
        if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
          unsigned lo = 0;
          for (int i = 3; i <= 6 && p[i]; i++) {
            char h = p[i];
            lo = lo * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
          }
          c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
          p += 6;
        }
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) out[n++] = (char)(0xC0 | c >> 6), out[n++] = (char)(0x80 | (c & 63));
        else if (c < 0x10000)
          out[n++] = (char)(0xE0 | c >> 12), out[n++] = (char)(0x80 | ((c >> 6) & 63)),
          out[n++] = (char)(0x80 | (c & 63));
        else
          out[n++] = (char)(0xF0 | c >> 18), out[n++] = (char)(0x80 | ((c >> 12) & 63)),
          out[n++] = (char)(0x80 | ((c >> 6) & 63)), out[n++] = (char)(0x80 | (c & 63));
        break;
      }
      default: out[n++] = *p; break;  // \" \\ \/
    }
  }
  out[n] = 0;
  return out;
}

static char* jstr(const JObj* o, const char* key) {
  const JField* f = jget(o, key);
  return f ? jstr_at(f->v) : NULL;
}

static bool jint(const JObj* o, const char* key, long* out) {
  const JField* f = jget(o, key);
  if (!f || !((*f->v >= '0' && *f->v <= '9') || *f->v == '-')) return false;
  *out = strtol(f->v, NULL, 10);
  return true;
}

// Up to `cap` numbers of an array value ([1,2,3]); how many.
static int jints(const JObj* o, const char* key, long* out, int cap) {
  const JField* f = jget(o, key);
  if (!f || *f->v != '[') return 0;
  int n = 0;
  const char* p = f->v + 1;
  while (n < cap) {
    p = jws(p);
    if (*p == ']' || !*p) break;
    char* end;
    out[n++] = strtol(p, &end, 10);
    p = jws(end);
    if (*p == ',') p++;
  }
  return n;
}

// Is `word` among the strings of an array value (["ctrl","shift"])?
static bool jhas(const JObj* o, const char* key, const char* word) {
  const JField* f = jget(o, key);
  if (!f || *f->v != '[') return false;
  size_t w = strlen(word);
  for (const char* p = f->v; p < f->v + f->vlen; p++)
    if (*p == '"' && strncmp(p + 1, word, w) == 0 && p[1 + w] == '"') return true;
  return false;
}

// ---- finding windows and panes ----------------------------------------------

static Window* find_pane(int id) {
  for (Frame* f = g_frames; f; f = f->next)
    for (int i = 0; i < f->ntabs; i++) {
      Window* panes[64];
      int n = tab_panes(f->tabs[i]->tab, panes, 64);
      for (int k = 0; k < n && k < 64; k++)
        if (panes[k]->id == id) return panes[k];
    }
  return NULL;
}

// The pane a request means: "pane", or the front window's selected tab's
// focused pane.
static Window* target_pane(const JObj* o) {
  long id;
  if (jint(o, "pane", &id)) return find_pane((int)id);
  Frame* f = front_frame();
  return f && f->active >= 0 ? f->tabs[f->active] : NULL;
}

static Frame* target_frame(const JObj* o) {
  long n;
  if (jint(o, "window", &n)) {
    long i = 0;
    for (Frame* f = g_frames; f; f = f->next, i++)
      if (i == n) return f;
    return NULL;
  }
  return front_frame();
}

static void put_rect(Line* b, HWND h) {
  RECT r;
  GetWindowRect(h, &r);
  put(b, "[%ld,%ld,%ld,%ld]", r.left, r.top, r.right - r.left, r.bottom - r.top);
}

static void put_title(Line* b, const WCHAR* t) {
  char* s = t ? utf16_to_utf8(t, -1) : NULL;
  put_string(b, s ? s : "");
  free(s);
}

// ---- the commands (UI thread) -----------------------------------------------

typedef struct {
  const char* line;
  Line out;
  // For a screenshot: what to take, filled on the UI thread.
  bool shot;
  RECT rect;
  UINT dpi;
} CtlReq;

static void cmd_list(Line* b) {
  put(b, ",\"windows\":[");
  long wi = 0;
  for (Frame* f = g_frames; f; f = f->next, wi++) {
    put(b, "%s{\"window\":%ld,\"active\":%s,\"fullscreen\":%s,\"maximized\":%s,\"rect\":",
        wi ? "," : "", wi, GetForegroundWindow() == f->hwnd ? "true" : "false",
        f->fullscreen ? "true" : "false", IsZoomed(f->hwnd) ? "true" : "false");
    put_rect(b, f->hwnd);
    int pv = -1;
    const char* ps = notify_progress_state(f, &pv);  // notify.c: NULL for none
    if (ps) {
      put(b, ",\"progress\":{\"state\":");
      put_string(b, ps);
      put(b, ",\"value\":%d}", pv);
    } else {
      put(b, ",\"progress\":null");
    }
    put(b, ",\"tabs\":[");
    for (int i = 0; i < f->ntabs; i++) {
      Window* focused = f->tabs[i];
      put(b, "%s{\"selected\":%s,\"title\":", i ? "," : "", i == f->active ? "true" : "false");
      put_title(b, focused->title);
      put(b, ",\"zoomed\":%s,\"panes\":[",
          focused->tab && focused->tab->zoomed ? "true" : "false");
      Window* panes[64];
      int n = tab_panes(focused->tab, panes, 64);
      if (n <= 0) panes[0] = focused, n = 1;
      for (int k = 0; k < n && k < 64; k++) {
        Window* w = panes[k];
        ghostty_surface_size_s sz = ghostty_surface_size(w->surface);
        put(b, "%s{\"pane\":%d,\"focused\":%s,\"rect\":", k ? "," : "", w->id,
            w == focused ? "true" : "false");
        put_rect(b, w->hwnd);
        put(b, ",\"grid\":[%u,%u],\"present\":\"%s\",\"title\":", sz.columns, sz.rows,
            w->swap ? "swap" : "copy");
        put_title(b, w->title);
        put(b, "}");
      }
      put(b, "]}");
    }
    put(b, "]}");
  }
  put(b, "]");
}

// A named key, as Windows' virtual key; *ext: an extended key (E0 scan).
static UINT named_vk(const char* k, bool* ext) {
  static const struct { const char* name; UINT vk; bool ext; } keys[] = {
      {"enter", VK_RETURN, false},  {"tab", VK_TAB, false},     {"backspace", VK_BACK, false},
      {"escape", VK_ESCAPE, false}, {"space", VK_SPACE, false}, {"up", VK_UP, true},
      {"down", VK_DOWN, true},      {"left", VK_LEFT, true},    {"right", VK_RIGHT, true},
      {"home", VK_HOME, true},      {"end", VK_END, true},      {"page_up", VK_PRIOR, true},
      {"page_down", VK_NEXT, true}, {"insert", VK_INSERT, true}, {"delete", VK_DELETE, true},
  };
  *ext = false;
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    if (strcmp(k, keys[i].name) == 0) {
      *ext = keys[i].ext;
      return keys[i].vk;
    }
  if ((k[0] == 'f' || k[0] == 'F') && k[1] >= '1' && k[1] <= '9') {
    int n = atoi(k + 1);
    if (n >= 1 && n <= 12) return (UINT)(VK_F1 + n - 1);
  }
  return 0;
}

static bool cmd_key(const JObj* o, Window* w, Line* b) {
  char* k = jstr(o, "key");
  if (!k || !*k) {
    free(k);
    put(b, ",\"error\":\"key: none\"");
    return false;
  }
  bool shift = jhas(o, "mods", "shift"), ctrl = jhas(o, "mods", "ctrl"),
       alt = jhas(o, "mods", "alt"), super = jhas(o, "mods", "super");
  bool ext = false;
  UINT vk = named_vk(k, &ext);
  if (!vk) {
    // One character: its key on this layout (and the Shift it needs).
    WCHAR* wc = utf8_to_utf16(k, -1);
    SHORT r = wc && wc[0] && !wc[1] ? VkKeyScanW(wc[0]) : -1;
    free(wc);
    if (r == -1) {
      put(b, ",\"error\":\"key: unknown\"");
      free(k);
      return false;
    }
    vk = (UINT)(r & 0xff);
    if (r & 0x100) shift = true;
  }
  free(k);
  int m = GHOSTTY_MODS_NONE;
  if (shift) m |= GHOSTTY_MODS_SHIFT;
  if (ctrl) m |= GHOSTTY_MODS_CTRL;
  if (alt) m |= GHOSTTY_MODS_ALT;
  if (super) m |= GHOSTTY_MODS_SUPER;
  ghostty_input_key_s key = {0};
  key.mods = (ghostty_input_mods_e)m;
  key.keycode = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) | (ext ? 0xe000 : 0);
  UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;
  key.unshifted_codepoint = ch ? (uint32_t)towlower((wint_t)ch) : 0;
  // The text the key types with Shift alone (Ghostty encodes control
  // characters and Ctrl/Alt combinations from the key itself).
  char text[8] = {0};
  if (!ctrl && !alt && !super) {
    BYTE state[256] = {0};
    if (shift) state[VK_SHIFT] = 0x80;
    WCHAR out[4];
    int n = ToUnicode(vk, key.keycode & 0xff, state, out, 4, 0);
    if (n == 1 && out[0] >= 0x20 && out[0] != 0x7f) {
      char* t = utf16_to_utf8(out, 1);
      if (t) {
        strncpy(text, t, sizeof(text) - 1);
        free(t);
      }
    }
  }
  key.text = text[0] ? text : NULL;
  key.action = GHOSTTY_ACTION_PRESS;
  bool handled = ghostty_surface_key(w->surface, key);
  key.action = GHOSTTY_ACTION_RELEASE;
  key.text = NULL;
  ghostty_surface_key(w->surface, key);
  put(b, ",\"handled\":%s", handled ? "true" : "false");
  return true;
}

static bool cmd_read(Window* w, Line* b) {
  ghostty_surface_size_s sz = ghostty_surface_size(w->surface);
  if (!sz.columns || !sz.rows) {
    put(b, ",\"error\":\"read: no grid\"");
    return false;
  }
  ghostty_selection_s sel = {0};
  sel.top_left.tag = GHOSTTY_POINT_VIEWPORT;
  sel.top_left.coord = GHOSTTY_POINT_COORD_TOP_LEFT;
  sel.bottom_right.tag = GHOSTTY_POINT_VIEWPORT;
  sel.bottom_right.coord = GHOSTTY_POINT_COORD_BOTTOM_RIGHT;
  ghostty_text_s t = {0};
  if (!ghostty_surface_read_text(w->surface, sel, &t)) {
    put(b, ",\"error\":\"read: failed\"");
    return false;
  }
  char* s = malloc(t.text_len + 1);
  if (s) {
    memcpy(s, t.text, t.text_len);
    s[t.text_len] = 0;
  }
  put(b, ",\"text\":");
  put_string(b, s ? s : "");
  // Not in the C API yet: the cursor and the alternate screen.
  put(b, ",\"cols\":%u,\"rows\":%u,\"cursor\":null,\"alt_screen\":null", sz.columns, sz.rows);
  free(s);
  ghostty_surface_free_text(w->surface, &t);
  return true;
}

static bool cmd_window(const JObj* o, Line* b) {
  Frame* f = target_frame(o);
  if (!f) {
    put(b, ",\"error\":\"window: none\"");
    return false;
  }
  long r[4];
  if (jints(o, "rect", r, 4) == 4) {
    if (f->fullscreen) set_fullscreen(f, false);
    ShowWindow(f->hwnd, SW_RESTORE);
    SetWindowPos(f->hwnd, NULL, (int)r[0], (int)r[1], (int)r[2], (int)r[3],
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
  char* st = jstr(o, "state");
  if (st) {
    if (strcmp(st, "fullscreen") == 0)
      set_fullscreen(f, true);
    else {
      if (f->fullscreen) set_fullscreen(f, false);
      if (strcmp(st, "maximize") == 0) ShowWindow(f->hwnd, SW_MAXIMIZE);
      if (strcmp(st, "minimize") == 0) ShowWindow(f->hwnd, SW_MINIMIZE);
      if (strcmp(st, "restore") == 0) ShowWindow(f->hwnd, SW_RESTORE);
    }
    free(st);
  }
  put(b, ",\"rect\":");
  put_rect(b, f->hwnd);
  return true;
}

static void cmd_events(const JObj* o, Line* b) {
  long since = 0;
  jint(o, "since", &since);
  EnterCriticalSection(&g_ev_lock);
  uint64_t first = g_ev_next > EVENT_RING ? g_ev_next - EVENT_RING : 1;
  if ((uint64_t)since < first) since = (long)first;
  put(b, ",\"events\":[");
  bool any = false;
  for (uint64_t s = (uint64_t)since; s < g_ev_next; s++) {
    const char* e = g_events[s % EVENT_RING];
    if (!e || *e != '{') continue;
    put(b, "%s{\"seq\":%llu,%s", any ? "," : "", (unsigned long long)s, e + 1);
    any = true;
  }
  put(b, "],\"next\":%llu", (unsigned long long)g_ev_next);
  LeaveCriticalSection(&g_ev_lock);
}

// Run one request on the UI thread (from the wakeup window).
LRESULT testctl_ui(LPARAM lp) {
  CtlReq* q = (CtlReq*)lp;
  Line* b = &q->out;
  JObj o;
  if (!jparse(q->line, &o)) {
    put(b, "{\"ok\":false,\"error\":\"not a JSON object\"}");
    return 0;
  }
  const JField* id = jget(&o, "id");
  put(b, "{\"id\":");
  if (id)
    put(b, "%.*s", (int)id->vlen, id->v);
  else
    put(b, "null");
  char* cmd = jstr(&o, "cmd");
  bool ok = true;
  if (!cmd) {
    put(b, ",\"error\":\"no cmd\"");
    ok = false;
  } else if (strcmp(cmd, "hello") == 0) {
    ghostty_info_s gi = ghostty_info();
    put(b, ",\"protocol\":1,\"version\":\"%.*s\",\"build\":", (int)gi.version_len, gi.version);
    put_string(b, GHOSTTY_BUILD_TAG);
    put(b, ",\"pid\":%lu", GetCurrentProcessId());
  } else if (strcmp(cmd, "list") == 0) {
    cmd_list(b);
  } else if (strcmp(cmd, "stats") == 0) {
    put(b, ",\"stats\":");
    stats_json(b);
  } else if (strcmp(cmd, "events") == 0) {
    cmd_events(&o, b);
  } else if (strcmp(cmd, "window") == 0) {
    ok = cmd_window(&o, b);
  } else {
    Window* w = target_pane(&o);
    if (!w || !w->surface) {
      put(b, ",\"error\":\"no such pane\"");
      ok = false;
    } else if (strcmp(cmd, "text") == 0) {
      char* t = jstr(&o, "text");
      if (t) ghostty_surface_text(w->surface, t, strlen(t));
      ok = t != NULL;
      if (!ok) put(b, ",\"error\":\"text: none\"");
      free(t);
    } else if (strcmp(cmd, "key") == 0) {
      ok = cmd_key(&o, w, b);
    } else if (strcmp(cmd, "action") == 0) {
      char* a = jstr(&o, "action");
      bool done = a && ghostty_surface_binding_action(w->surface, a, strlen(a));
      put(b, ",\"performed\":%s", done ? "true" : "false");
      ok = a != NULL;
      free(a);
    } else if (strcmp(cmd, "read") == 0) {
      ok = cmd_read(w, b);
    } else if (strcmp(cmd, "screenshot") == 0) {
      char* t = jstr(&o, "target");
      HWND h = w->hwnd;
      if (t && strcmp(t, "window") == 0) h = w->frame->hwnd;
      GetWindowRect(h, &q->rect);
      if (t && strcmp(t, "tab") == 0) {
        // The tab's panes together: the frame's client area under the bar.
        RECT c;
        GetClientRect(w->frame->hwnd, &c);
        POINT tl = {0, bar_height(w->frame)}, br = {c.right, c.bottom};
        ClientToScreen(w->frame->hwnd, &tl);
        ClientToScreen(w->frame->hwnd, &br);
        SetRect(&q->rect, tl.x, tl.y, br.x, br.y);
      }
      q->dpi = GetDpiForWindow(h);
      q->shot = true;
      free(t);
    } else {
      put(b, ",\"error\":\"unknown cmd\"");
      ok = false;
    }
  }
  free(cmd);
  if (!q->shot) put(b, ",\"ok\":%s}", ok ? "true" : "false");
  return 0;
}

// ---- screenshots: the screen, as PNG (stored, no compression) or RGBA ------

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      crc_table[i] = v;
    }
  c = ~c;
  while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
  return ~c;
}

typedef struct {
  uint8_t* p;
  size_t len, cap;
} Bytes;

static void bput(Bytes* b, const void* d, size_t n) {
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 65536;
    while (cap < b->len + n) cap *= 2;
    uint8_t* q = realloc(b->p, cap);
    if (!q) return;
    b->p = q;
    b->cap = cap;
  }
  memcpy(b->p + b->len, d, n);
  b->len += n;
}

static void bput32(Bytes* b, uint32_t v) {
  uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  bput(b, x, 4);
}

static void chunk(Bytes* b, const char* type, const uint8_t* data, size_t n) {
  bput32(b, (uint32_t)n);
  size_t at = b->len;
  bput(b, type, 4);
  bput(b, data, n);
  bput32(b, crc(0, b->p + at, n + 4));
}

// RGBA rows top to bottom → a PNG (zlib "stored" blocks: big, simple, exact).
static Bytes png(const uint8_t* rgba, int w, int h) {
  Bytes out = {0}, raw = {0}, z = {0};
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  bput(&out, sig, 8);
  uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                      (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                      8, 6, 0, 0, 0};
  chunk(&out, "IHDR", ihdr, 13);
  for (int y = 0; y < h; y++) {
    uint8_t filter = 0;
    bput(&raw, &filter, 1);
    bput(&raw, rgba + (size_t)y * w * 4, (size_t)w * 4);
  }
  static const uint8_t zhead[2] = {0x78, 0x01};
  bput(&z, zhead, 2);
  uint32_t a = 1, bsum = 0;
  for (size_t i = 0; i < raw.len; i++) {
    a = (a + raw.p[i]) % 65521;
    bsum = (bsum + a) % 65521;
  }
  for (size_t at = 0; at < raw.len || raw.len == 0;) {
    size_t n = raw.len - at > 65535 ? 65535 : raw.len - at;
    uint8_t head[5] = {(uint8_t)(at + n >= raw.len), (uint8_t)n, (uint8_t)(n >> 8),
                       (uint8_t)~n, (uint8_t)(~n >> 8)};
    bput(&z, head, 5);
    bput(&z, raw.p + at, n);
    at += n;
    if (raw.len == 0) break;
  }
  bput32(&z, (bsum << 16) | a);
  chunk(&out, "IDAT", z.p, z.len);
  chunk(&out, "IEND", NULL, 0);
  free(raw.p);
  free(z.p);
  return out;
}

static void put_base64(Line* b, const uint8_t* p, size_t n) {
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  put(b, "\"");
  char quad[5] = {0};
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) |
                 (i + 2 < n ? p[i + 2] : 0);
    quad[0] = t[v >> 18 & 63];
    quad[1] = t[v >> 12 & 63];
    quad[2] = i + 1 < n ? t[v >> 6 & 63] : '=';
    quad[3] = i + 2 < n ? t[v & 63] : '=';
    put(b, "%s", quad);
  }
  put(b, "\"");
}

// On the pipe's thread: the screen's pixels in q->rect, as the request asks.
static void take_shot(CtlReq* q) {
  Line* b = &q->out;
  JObj o;
  jparse(q->line, &o);
  int w = q->rect.right - q->rect.left, h = q->rect.bottom - q->rect.top;
  if (w <= 0 || h <= 0) {
    put(b, ",\"error\":\"screenshot: empty\",\"ok\":false}");
    return;
  }
  Sleep(150);  // a frame presented after the request (60 Hz: 9 of them)
  HDC screen = GetDC(NULL);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
  uint8_t* px = NULL;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&px, NULL, 0);
  if (!bmp) {
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    put(b, ",\"error\":\"screenshot: no bitmap\",\"ok\":false}");
    return;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, screen, q->rect.left, q->rect.top, SRCCOPY | CAPTUREBLT);
  GdiFlush();
  for (size_t i = 0; i < (size_t)w * h; i++) {  // BGRA → RGBA, opaque
    uint8_t t = px[i * 4];
    px[i * 4] = px[i * 4 + 2];
    px[i * 4 + 2] = t;
    px[i * 4 + 3] = 255;
  }
  char* fmt = jstr(&o, "format");
  char* path = jstr(&o, "path");
  bool raw = fmt && strcmp(fmt, "rgba") == 0;
  Bytes img = {0};
  if (!raw) img = png(px, w, h);
  const uint8_t* data = raw ? px : img.p;
  size_t n = raw ? (size_t)w * h * 4 : img.len;
  put(b, ",\"width\":%d,\"height\":%d,\"dpi\":%u,\"format\":\"%s\"", w, h, q->dpi,
      raw ? "rgba" : "png");
  bool ok = data != NULL;
  if (path && ok) {
    WCHAR* wp = utf8_to_utf16(path, -1);
    FILE* f = wp ? _wfopen(wp, L"wb") : NULL;
    ok = f && fwrite(data, 1, n, f) == n;
    if (f) fclose(f);
    free(wp);
    put(b, ",\"path\":");
    put_string(b, path);
  } else if (ok) {
    put(b, ",\"data\":");
    put_base64(b, data, n);
  }
  put(b, ",\"ok\":%s}", ok ? "true" : "false");
  free(img.p);
  free(fmt);
  free(path);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(NULL, screen);
}

// ---- the pipe ----------------------------------------------------------------

static bool write_all(HANDLE p, const char* s, size_t n) {
  DWORD w;
  while (n) {
    if (!WriteFile(p, s, n > 0x100000 ? 0x100000 : (DWORD)n, &w, NULL) || !w) return false;
    s += w;
    n -= w;
  }
  return true;
}

static DWORD WINAPI ctl_main(void* arg) {
  bool first = arg != NULL;
  SECURITY_ATTRIBUTES sa;
  if (!pipe_security(&sa)) return 0;
  char* buf = malloc(CTL_MAX_LINE + 1);
  if (!buf) return 0;
  for (;;) {
    DWORD open = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    SetLastError(ERROR_SUCCESS);
    HANDLE p = CreateNamedPipeW(g_ctl_name, open,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                    PIPE_REJECT_REMOTE_CLIENTS,
                                CTL_INSTANCES, 1 << 16, 1 << 16, 0, &sa);
    // Wine creates a first instance of a taken name and only says so.
    if (first && p != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(p);
      p = INVALID_HANDLE_VALUE;
    }
    first = false;
    if (p == INVALID_HANDLE_VALUE) {
      fprintf(stderr, "ghostty: test-control pipe: error %lu\n", GetLastError());
      free(buf);
      return 0;
    }
    bool connected = ConnectNamedPipe(p, NULL) || GetLastError() == ERROR_PIPE_CONNECTED;
    if (connected && pipe_client_same_user(p)) {
      size_t have = 0;
      DWORD n;
      while (ReadFile(p, buf + have, (DWORD)(CTL_MAX_LINE - have), &n, NULL) && n) {
        have += n;
        char* nl;
        while ((nl = memchr(buf, '\n', have))) {
          *nl = 0;
          // On the heap with its own copy of the line: a request that times
          // out is not taken back (SendMessageTimeout leaves it queued), so
          // the UI thread may still run it later; it is then leaked, never
          // freed under it.
          size_t len = (size_t)(nl - buf);
          CtlReq* q = calloc(1, sizeof(CtlReq));
          char* line = malloc(len + 1);
          Line reply = {0};
          DWORD_PTR res;
          if (!q || !line) {
            free(q);
            free(line);
            put(&reply, "{\"ok\":false,\"error\":\"out of memory\"}");
          } else {
            memcpy(line, buf, len + 1);
            q->line = line;
            if (!SendMessageTimeoutW(g_wakeup_hwnd, WM_GHOSTTY_TESTCTL, 0, (LPARAM)q,
                                     SMTO_NORMAL, 5000, &res)) {
              put(&reply, "{\"ok\":false,\"error\":\"ui timeout\"}");  // q leaks
            } else {
              if (q->shot) take_shot(q);
              reply = q->out;
              free(line);
              free(q);
            }
          }
          put(&reply, "\n");
          bool sent = reply.p && write_all(p, reply.p, reply.len);
          free(reply.p);
          size_t used = (size_t)(nl - buf) + 1;
          memmove(buf, nl + 1, have - used);
          have -= used;
          if (!sent) goto gone;
        }
        if (have == CTL_MAX_LINE) have = 0;  // a line too long: dropped
      }
    }
  gone:
    FlushFileBuffers(p);
    DisconnectNamedPipe(p);
    CloseHandle(p);
  }
}

// GHOSTTY_TEST_CONTROL=1: the pipe, CTL_INSTANCES clients at a time.
void testctl_start(void) {
  const char* v = getenv("GHOSTTY_TEST_CONTROL");
  if (!v || strcmp(v, "1") != 0) return;
  InitializeCriticalSection(&g_ev_lock);
  swprintf(g_ctl_name, 64, L"\\\\.\\pipe\\ghostty-%lu.ctl", GetCurrentProcessId());
  g_ctl_on = true;
  for (int i = 0; i < CTL_INSTANCES; i++) {
    HANDLE t = CreateThread(NULL, 256 * 1024, ctl_main, i == 0 ? (void*)1 : NULL, 0, NULL);
    if (t) CloseHandle(t);
    if (i == 0) Sleep(20);  // the first instance claims the name first
  }
  fprintf(stderr, "ghostty: test-control pipe %ls (GHOSTTY_TEST_CONTROL=1)\n", g_ctl_name);
}

// ---- ghostty.exe /ctl [pid] '<json>' ---------------------------------------

bool ctl_client_mode(int argc, char** argv, int* rc) {
  if (argc < 3 || (strcmp(argv[1], "/ctl") != 0 && strcmp(argv[1], "--ctl") != 0))
    return false;
  *rc = 1;
  DWORD pid = 0;
  const char* req = argv[argc - 1];
  if (argc > 3) pid = strtoul(argv[2], NULL, 10);
  if (!pid) {
    // The one Ghostty with a test-control pipe.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    int count = 0;
    for (BOOL ok = snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe); ok;
         ok = Process32NextW(snap, &pe)) {
      if (pe.th32ProcessID == GetCurrentProcessId() ||
          _wcsicmp(pe.szExeFile, L"ghostty.exe") != 0)
        continue;
      WCHAR name[64];
      swprintf(name, 64, L"\\\\.\\pipe\\ghostty-%lu.ctl", pe.th32ProcessID);
      if (WaitNamedPipeW(name, 1) || GetLastError() == ERROR_SEM_TIMEOUT) {
        pid = pe.th32ProcessID;
        count++;
      }
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    if (count != 1) {
      fprintf(stderr, "ghostty: %s Ghostty with a test-control pipe; give its pid\n",
              count ? "several" : "no");
      return true;
    }
  }
  WCHAR name[64];
  swprintf(name, 64, L"\\\\.\\pipe\\ghostty-%lu.ctl", pid);
  HANDLE p = INVALID_HANDLE_VALUE;
  for (int i = 0; i < 10 && p == INVALID_HANDLE_VALUE; i++) {
    p = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (p == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name, 1000);
  }
  if (p == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "ghostty: %lu: no test-control pipe (error %lu)\n", pid, GetLastError());
    return true;
  }
  DWORD n;
  WriteFile(p, req, (DWORD)strlen(req), &n, NULL);
  WriteFile(p, "\n", 1, &n, NULL);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  char buf[65536];
  while (ReadFile(p, buf, sizeof(buf), &n, NULL) && n) {
    DWORD w;
    WriteFile(out, buf, n, &w, NULL);
    if (memchr(buf, '\n', n)) break;
  }
  CloseHandle(p);
  *rc = 0;
  return true;
}
