#include "host.h"

// ---------------------------------------------------------------------------
// Input

static bool key_down(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

static ghostty_input_mods_e current_mods(void) {
  int m = GHOSTTY_MODS_NONE;
  if (key_down(VK_SHIFT)) m |= GHOSTTY_MODS_SHIFT;
  if (key_down(VK_CONTROL)) m |= GHOSTTY_MODS_CTRL;
  if (key_down(VK_MENU)) m |= GHOSTTY_MODS_ALT;
  if (key_down(VK_LWIN) || key_down(VK_RWIN)) m |= GHOSTTY_MODS_SUPER;
  if (GetKeyState(VK_CAPITAL) & 1) m |= GHOSTTY_MODS_CAPS;
  if (GetKeyState(VK_NUMLOCK) & 1) m |= GHOSTTY_MODS_NUM;
  if (key_down(VK_RSHIFT)) m |= GHOSTTY_MODS_SHIFT_RIGHT;
  if (key_down(VK_RCONTROL)) m |= GHOSTTY_MODS_CTRL_RIGHT;
  if (key_down(VK_RMENU)) m |= GHOSTTY_MODS_ALT_RIGHT;
  if (key_down(VK_RWIN)) m |= GHOSTTY_MODS_SUPER_RIGHT;
  return (ghostty_input_mods_e)m;
}

// The native keycode Ghostty's keycode table expects on Windows: the
// scan code, with 0xe000 for extended keys (src/input/keycodes.zig).
static uint32_t native_keycode(LPARAM lp) {
  uint32_t sc = (uint32_t)(lp >> 16) & 0xff;
  if (lp & (1 << 24)) sc |= 0xe000;
  return sc;
}

// The character a key makes with no modifiers, lower case, or 0.
static uint32_t unshifted_codepoint(WPARAM vk) {
  UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;
  if (ch == 0) return 0;
  return (uint32_t)towlower((wint_t)ch);
}

// Collect the WM_CHAR / WM_SYSCHAR messages TranslateMessage queued for
// this key press, so the key event carries the text it produced. A dead
// key queues WM_DEADCHAR instead: the key is composing. With Alt held they
// come as WM_SYSCHAR / WM_SYSDEADCHAR, so the range runs up to those: left
// in the queue, an Alt+key's character would arrive again as text after
// the key (a bound Alt+2 also typed "2"; Alt+b sent ESC b, then b).
static int take_chars(HWND hwnd, WCHAR* buf, int cap, bool* composing) {
  MSG msg;
  int n = 0;
  *composing = false;
  while (PeekMessageW(&msg, hwnd, WM_CHAR, WM_SYSDEADCHAR, PM_NOREMOVE)) {
    if (msg.message != WM_CHAR && msg.message != WM_SYSCHAR &&
        msg.message != WM_DEADCHAR && msg.message != WM_SYSDEADCHAR)
      break;
    PeekMessageW(&msg, hwnd, msg.message, msg.message, PM_REMOVE);
    if (msg.message == WM_DEADCHAR || msg.message == WM_SYSDEADCHAR) {
      *composing = true;
      continue;
    }
    if (n < cap) buf[n++] = (WCHAR)msg.wParam;
  }
  return n;
}

void handle_key(Window* w, UINT msg, WPARAM vk, LPARAM lp) {
  if (!w->surface) return;
  // Unicode injected by SendInput and IME results arrive as WM_CHAR.
  if (vk == VK_PACKET || vk == VK_PROCESSKEY) return;

  bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
  bool repeat = !up && (lp & (1 << 30));

  // A release only after a press Ghostty got: the keys an IME took
  // (VK_PROCESSKEY, above) and the host's own (Ctrl+Shift+F12) come back
  // up as the real key, which would be a release out of nowhere for a
  // program asking for releases (the kitty keyboard protocol). Scan code 0
  // (synthetic input) is not tracked.
  uint32_t sc = ((uint32_t)(lp >> 16) & 0xff) | ((lp & (1 << 24)) ? 0x100 : 0);
  uint8_t bit = (uint8_t)(1u << (sc & 7));
  if (sc & 0xff) {
    if (up) {
      if (!(w->pressed[sc >> 3] & bit)) return;
      w->pressed[sc >> 3] &= (uint8_t)~bit;
    } else {
      w->pressed[sc >> 3] |= bit;
    }
  }

  ghostty_input_key_s key = {0};
  key.action = up ? GHOSTTY_ACTION_RELEASE
                  : (repeat ? GHOSTTY_ACTION_REPEAT : GHOSTTY_ACTION_PRESS);
  key.mods = current_mods();
  key.keycode = native_keycode(lp);
  key.unshifted_codepoint = unshifted_codepoint(vk);

  char* text = NULL;
  if (!up) {
    WCHAR chars[16];
    bool composing = false;
    int n = take_chars(w->hwnd, chars, 16, &composing);
    key.composing = composing;
    // Control characters (Enter, Tab, Ctrl+C...) are encoded by Ghostty
    // from the key and its mods, not taken as text.
    if (n > 0 && !(n == 1 && (chars[0] < 0x20 || chars[0] == 0x7f))) {
      text = utf16_to_utf8(chars, n);
      // AltGr arrives as Ctrl+Alt; when it produced text those mods
      // were used up making it.
      if (key_down(VK_RMENU) && key_down(VK_LCONTROL))
        key.consumed_mods =
            (ghostty_input_mods_e)(GHOSTTY_MODS_CTRL | GHOSTTY_MODS_ALT);
      else if (key.mods & GHOSTTY_MODS_SHIFT)
        key.consumed_mods = GHOSTTY_MODS_SHIFT;
    }
  }
  key.text = text;
  // Latency: a press (or repeat) starts the clock if none is running.
  if (!up && !w->key_qpc) w->key_qpc = qpc();
  bool consumed = ghostty_surface_key(w->surface, key);
  // GHOSTTY_DEBUG_KEYS=1: one line per key message on stderr, to debug a
  // layout, AltGr or IME report (start it from cmd.exe: 2> keys.log).
  if (g_debug_keys)
    fprintf(stderr,
            "key msg=0x%04x vk=0x%02x scan=0x%04x mods=0x%x action=%d "
            "text=%s consumed=%d\n",
            msg, (unsigned)vk, key.keycode, (unsigned)key.mods,
            (int)key.action, text ? text : "", consumed);
  free(text);
}

// Text that came without a key press of ours: IME commits, VK_PACKET.
void handle_char(Window* w, WCHAR ch) {
  if (!w->surface) return;
  if (IS_HIGH_SURROGATE(ch)) {
    w->high_surrogate = ch;
    return;
  }
  WCHAR buf[2];
  int n = 0;
  if (IS_LOW_SURROGATE(ch) && w->high_surrogate) buf[n++] = w->high_surrogate;
  w->high_surrogate = 0;
  buf[n++] = ch;
  if (n == 1 && (ch < 0x20 || ch == 0x7f)) return;
  char* text = utf16_to_utf8(buf, n);
  if (!text) return;
  ghostty_surface_text(w->surface, text, strlen(text));
  free(text);
}

void mouse_pos(Window* w, LPARAM lp) {
  // Ghostty takes the cursor in points and scales it itself.
  double x = GET_X_LPARAM(lp) / w->scale;
  double y = GET_Y_LPARAM(lp) / w->scale;
  ghostty_surface_mouse_pos(w->surface, x, y, current_mods());
}

void mouse_button(Window* w, ghostty_input_mouse_state_e state,
                         ghostty_input_mouse_button_e button) {
  if (state == GHOSTTY_MOUSE_PRESS) {
    SetCapture(w->hwnd);
    if (GetFocus() != w->hwnd) SetFocus(w->hwnd);
  } else
    ReleaseCapture();
  ghostty_surface_mouse_button(w->surface, state, button, current_mods());
}
