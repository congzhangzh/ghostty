#include "host.h"

// ---------------------------------------------------------------------------
// Shells: the window menu's "Shell ▸", as its "Theme ▸" (theme.c)
//
// "Default (from my config)", PowerShell 7 (when installed), Windows
// PowerShell, Command Prompt, and each WSL distribution. The choice is for
// NEW tabs, splits and windows: the running shells are left alone.
//
// His config is never rewritten: the choice goes in
// %LOCALAPPDATA%\ghostty\shell.ghostty (`command = …`), loaded after his
// files like theme.ghostty (main.c); "Default" deletes it.
//
// The commands are names Windows finds on PATH (pwsh.exe, powershell.exe,
// cmd.exe, wsl.exe), so the core's shell integration knows them by their
// name (cmd: its PROMPT; pwsh and powershell: ghostty.ps1): a Windows
// `command` is split on spaces with no quoting (Exec.zig), so a pwsh only
// in "Program Files" is written by its short (8.3) path. WSL gets none
// (Ghostty has no integration for a shell inside WSL yet).
// The distributions are read from the registry (HKCU ...\Lxss, the list
// `wsl.exe -l -q` prints), not by running wsl.exe each time a menu is
// built; a name with a space is left out (the command could not hold it).

#define IDM_SHELL_DEFAULT 0x2400
#define IDM_SHELL_NOTE 0x2410  // "For new tabs and windows" (grayed)
#define IDM_SHELL 0x2420       // + 0x10 per shell
#define MAX_SHELLS 24

typedef struct {
  WCHAR label[96];
  char command[MAX_PATH + 40];
} Shell;

static Shell g_shells[MAX_SHELLS];
static int g_nshells = -1;

static void add_shell(const WCHAR* label, const char* command) {
  if (g_nshells >= MAX_SHELLS) return;
  Shell* s = &g_shells[g_nshells++];
  swprintf(s->label, 96, L"%ls", label);
  snprintf(s->command, sizeof(s->command), "%s", command);
}

static bool on_path(const WCHAR* exe) {
  WCHAR out[MAX_PATH];
  return SearchPathW(NULL, exe, NULL, MAX_PATH, out, NULL) > 0;
}

static void find_shells(void) {
  if (g_nshells >= 0) return;
  g_nshells = 0;
  // PowerShell 7: on PATH (its installer's default), else in Program Files.
  if (on_path(L"pwsh.exe")) {
    add_shell(L"PowerShell 7", "pwsh.exe");
  } else {
    WCHAR pf[MAX_PATH], path[MAX_PATH + 40], shortp[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    if (n && n < MAX_PATH) {
      swprintf(path, MAX_PATH + 40, L"%ls\\PowerShell\\7\\pwsh.exe", pf);
      DWORD k = GetShortPathNameW(path, shortp, MAX_PATH);
      if (k && k < MAX_PATH && !wcschr(shortp, L' ')) {
        char* c = utf16_to_utf8(shortp, -1);
        if (c) add_shell(L"PowerShell 7", c);
        free(c);
      }
    }
  }
  if (on_path(L"powershell.exe")) add_shell(tr(L"Windows PowerShell"), "powershell.exe");
  add_shell(tr(L"Command Prompt"), "cmd.exe");
  // WSL: each registered distribution.
  HKEY lxss;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ,
                    &lxss) != ERROR_SUCCESS)
    return;
  WCHAR sub[128];
  for (DWORD i = 0; g_nshells < MAX_SHELLS; i++) {
    DWORD len = 128;
    if (RegEnumKeyExW(lxss, i, sub, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
    WCHAR name[64];
    DWORD size = sizeof(name);
    if (RegGetValueW(lxss, sub, L"DistributionName", RRF_RT_REG_SZ, NULL, name, &size) != ERROR_SUCCESS ||
        !name[0] || wcspbrk(name, L" \t\""))
      continue;
    char* n8 = utf16_to_utf8(name, -1);
    if (!n8) continue;
    char command[160];
    snprintf(command, sizeof(command), "wsl.exe -d %s", n8);
    free(n8);
    WCHAR label[96];
    swprintf(label, 96, L"%ls (WSL)", name);
    add_shell(label, command);
  }
  RegCloseKey(lxss);
}

static void chosen_shell(char* out, size_t cap) {
  if (!read_value(L"shell.ghostty", "command", out, cap)) out[0] = 0;
}

char* shell_config_file(void) {
  WCHAR path[MAX_PATH + 64];
  if (!host_path(L"shell.ghostty", path, MAX_PATH + 64) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    return NULL;
  return utf16_to_utf8(path, -1);
}

static void choose_shell(const char* command) {
  if (command) {
    char text[MAX_PATH + 300];
    snprintf(text, sizeof(text),
             "# Written by Ghostty for Windows (the window menu's Shell), for new tabs and windows.\n"
             "# Delete it, or choose \"Default (from my config)\", for your config's command.\n"
             "command = %s\n",
             command);
    write_text(L"shell.ghostty", text);
  } else {
    WCHAR path[MAX_PATH + 64];
    if (host_path(L"shell.ghostty", path, MAX_PATH + 64)) DeleteFileW(path);
  }
  config_reload(NULL, false);
  menus_update();
  Line e = {0};
  put(&e, "{\"kind\":\"shell\",\"command\":");
  put_string(&e, command ? command : "");
  put(&e, "}");
  test_event(e.p);
  free(e.p);
}

void shell_menu_add(HMENU sys) {
  find_shells();
  HMENU m = CreatePopupMenu();
  if (!m) return;
  AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_SHELL_NOTE, tr(L"For new tabs and windows"));
  AppendMenuW(m, MF_STRING, IDM_SHELL_DEFAULT, tr(L"Default (from my config)"));
  AppendMenuW(m, MF_SEPARATOR, 0, NULL);
  for (int i = 0; i < g_nshells; i++) AppendMenuW(m, MF_STRING, IDM_SHELL + i * 0x10, g_shells[i].label);
  AppendMenuW(sys, MF_POPUP, (UINT_PTR)m, tr(L"Shell"));
  shell_menu_update(sys);
}

void shell_menu_update(HMENU sys) {
  char cmd[MAX_PATH + 40];
  chosen_shell(cmd, sizeof(cmd));
  CheckMenuItem(sys, IDM_SHELL_DEFAULT, MF_BYCOMMAND | (cmd[0] ? MF_UNCHECKED : MF_CHECKED));
  for (int i = 0; i < g_nshells; i++)
    CheckMenuItem(sys, IDM_SHELL + i * 0x10,
                  MF_BYCOMMAND | (cmd[0] && _stricmp(cmd, g_shells[i].command) == 0 ? MF_CHECKED : MF_UNCHECKED));
}

bool shell_command(HWND hwnd, WPARAM wp) {
  (void)hwnd;
  UINT id = (UINT)(wp & 0xFFF0);
  if (id == IDM_SHELL_DEFAULT) {
    choose_shell(NULL);
    return true;
  }
  if (id >= IDM_SHELL && id < IDM_SHELL + MAX_SHELLS * 0x10) {
    int i = (int)((id - IDM_SHELL) / 0x10);
    if (i < g_nshells) choose_shell(g_shells[i].command);
    return true;
  }
  return false;
}
