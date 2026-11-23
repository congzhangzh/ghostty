#include "host.h"

#include <sddl.h>

// ---------------------------------------------------------------------------
// Stats over a named pipe
//
// The stats overlay's numbers, for a program rather than a person: one JSON
// line per period on \\.\pipe\ghostty-<pid>.stats, so an agent (over ssh)
// can change, build, install and run `ghostty.exe /stats --count 10` with no
// screenshot. The rules are our FreeRDP clients' stats pipe:
//
// - a reader gets the newest line at once, then each new one; a slow reader
//   gets the newest line when it is ready again, never a queue (each line
//   has a seq: the gaps are what it missed). Nothing here ever waits on a
//   reader: the UI thread only swaps in the newest line and sets an event;
//   a thread of its own does the pipe's overlapped I/O;
// - at most 8 readers at once;
// - only this user: the pipe has an access list of this user and SYSTEM, its
//   first instance claims the name (another program cannot take it over),
//   remote readers are refused, and each reader's process must run as this
//   user.
//
// On by default (cheap: the numbers are kept anyway, a line every 500 ms);
// GHOSTTY_STATS_IPC=off turns it off.

#define IPC_MAX_READERS 8
#define IPC_OUT_BUFFER (64 * 1024)

typedef struct {
  HANDLE pipe;
  OVERLAPPED ov;  // never moves while an operation is under way
  bool pending;   // a ConnectNamedPipe or a WriteFile is under way
  char* line;     // what the pending write sends (owned)
  size_t len;
  uint64_t seq;
} IpcInstance;

// A TOKEN_USER and the SID after it. Aligned as TOKEN_USER (it holds a
// pointer): Windows refuses a misaligned buffer with ERROR_NOACCESS (998);
// Wine does not check, so a BYTE array passed every Wine test and failed
// on every real Windows (the stats pipe off, every test-pipe client
// refused).
typedef union {
  TOKEN_USER tu;
  BYTE bytes[256];
} TokenUserBuf;

static struct {
  bool on;
  WCHAR name[64];
  TokenUserBuf user;  // this process's
  PSECURITY_DESCRIPTOR sd;
  SECURITY_ATTRIBUTES sa;
  HANDLE wake;
  CRITICAL_SECTION lock;
  char* latest;  // the newest line, "\n" included (under lock)
  size_t latest_len;
  uint64_t latest_seq;
  IpcInstance* listening;
  IpcInstance* readers[IPC_MAX_READERS];
  int nreaders;
} g_ipc;

static bool token_user(HANDLE process, TokenUserBuf* buf) {
  HANDLE token = NULL;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
  DWORD size = 0;
  BOOL ok = GetTokenInformation(token, TokenUser, buf, sizeof(*buf), &size);
  DWORD err = GetLastError();
  CloseHandle(token);
  SetLastError(err);
  return ok != FALSE;
}

static PSID sid_of(TokenUserBuf* buf) { return buf->tu.User.Sid; }

// The process at the other end of the pipe runs as us.
// A refusal is said on stderr with its reason (it was silent once).
static bool same_user(HANDLE pipe) {
  ULONG pid = 0;
  const char* why = NULL;
  DWORD err = 0;
  HANDLE process = NULL;
  TokenUserBuf buf;
  if (!GetNamedPipeClientProcessId(pipe, &pid))
    why = "no client process id";
  else if (!(process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)))
    why = "cannot open the client process";
  else if (!token_user(process, &buf))
    why = "cannot read the client's user";
  else if (!EqualSid(sid_of(&buf), sid_of(&g_ipc.user)))
    why = "another user";
  if (why) err = GetLastError();
  if (process) CloseHandle(process);
  if (why)
    fprintf(stderr, "ghostty: pipe client (pid %lu) refused: %s (error %lu)\n", pid, why, err);
  return !why;
}

static void close_instance(IpcInstance* in) {
  if (!in) return;
  if (in->pipe != INVALID_HANDLE_VALUE) {
    if (in->pending) {
      // The buffers of a cancelled operation are free once it has ended.
      CancelIoEx(in->pipe, &in->ov);
      DWORD n = 0;
      GetOverlappedResult(in->pipe, &in->ov, &n, TRUE);
    }
    DisconnectNamedPipe(in->pipe);
    CloseHandle(in->pipe);
  }
  if (in->ov.hEvent) CloseHandle(in->ov.hEvent);
  free(in->line);
  free(in);
}

// A new instance of the pipe, waiting for a reader.
static bool ipc_listen(bool first) {
  IpcInstance* in = calloc(1, sizeof(*in));
  if (!in) return false;
  in->pipe = INVALID_HANDLE_VALUE;
  in->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!in->ov.hEvent) {
    close_instance(in);
    return false;
  }
  DWORD open = PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED |
               (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
  SetLastError(ERROR_SUCCESS);
  in->pipe = CreateNamedPipeW(
      g_ipc.name, open,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
          PIPE_REJECT_REMOTE_CLIENTS,
      IPC_MAX_READERS, IPC_OUT_BUFFER, 0, 0, &g_ipc.sa);
  // Windows refuses a first instance of a name that exists; Wine creates
  // it and only says ERROR_ALREADY_EXISTS: refuse that too.
  if (first && in->pipe != INVALID_HANDLE_VALUE &&
      GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(in->pipe);
    in->pipe = INVALID_HANDLE_VALUE;
  }
  if (in->pipe == INVALID_HANDLE_VALUE) {
    close_instance(in);
    return false;
  }
  if (!ConnectNamedPipe(in->pipe, &in->ov)) {
    switch (GetLastError()) {
      case ERROR_IO_PENDING: in->pending = true; break;
      case ERROR_PIPE_CONNECTED: SetEvent(in->ov.hEvent); break;  // came early
      default: close_instance(in); return false;
    }
  } else {
    in->pending = true;  // overlapped: never TRUE; if so, the event tells
  }
  g_ipc.listening = in;
  return true;
}

static void drop_reader(int i) {
  close_instance(g_ipc.readers[i]);
  memmove(&g_ipc.readers[i], &g_ipc.readers[i + 1],
          (size_t)(g_ipc.nreaders - i - 1) * sizeof(g_ipc.readers[0]));
  g_ipc.nreaders--;
}

static DWORD WINAPI ipc_main(void* arg) {
  (void)arg;
  for (;;) {
    // One instance waits for the next reader while there is room.
    if (!g_ipc.listening && g_ipc.nreaders < IPC_MAX_READERS)
      ipc_listen(false);  // tried again on the next wake

    // Each reader done with its line gets the newest one.
    EnterCriticalSection(&g_ipc.lock);
    for (int i = 0; i < g_ipc.nreaders; i++) {
      IpcInstance* r = g_ipc.readers[i];
      if (r->pending || r->seq >= g_ipc.latest_seq || !g_ipc.latest) continue;
      char* copy = malloc(g_ipc.latest_len);
      if (!copy) continue;
      memcpy(copy, g_ipc.latest, g_ipc.latest_len);
      free(r->line);
      r->line = copy;
      r->len = g_ipc.latest_len;
      r->seq = g_ipc.latest_seq;
      if (WriteFile(r->pipe, r->line, (DWORD)r->len, NULL, &r->ov) ||
          GetLastError() == ERROR_IO_PENDING)
        r->pending = true;  // done at once or not: the event tells
      else
        r->pending = false, r->seq = UINT64_MAX;  // gone: dropped below
    }
    LeaveCriticalSection(&g_ipc.lock);
    for (int i = g_ipc.nreaders - 1; i >= 0; i--)
      if (g_ipc.readers[i]->seq == UINT64_MAX) drop_reader(i);

    HANDLE h[2 + IPC_MAX_READERS];
    DWORD n = 0;
    h[n++] = g_ipc.wake;
    if (g_ipc.listening) h[n++] = g_ipc.listening->ov.hEvent;
    for (int i = 0; i < g_ipc.nreaders; i++)
      if (g_ipc.readers[i]->pending) h[n++] = g_ipc.readers[i]->ov.hEvent;
    // Without a listening instance (it failed), look again soon.
    DWORD timeout = g_ipc.listening || g_ipc.nreaders >= IPC_MAX_READERS
                        ? INFINITE
                        : 100;
    if (WaitForMultipleObjects(n, h, FALSE, timeout) == WAIT_FAILED) return 0;

    // Finished writes; a reader that left fails its write.
    for (int i = g_ipc.nreaders - 1; i >= 0; i--) {
      IpcInstance* r = g_ipc.readers[i];
      if (!r->pending || !HasOverlappedIoCompleted(&r->ov)) continue;
      DWORD done = 0;
      r->pending = false;
      if (!GetOverlappedResult(r->pipe, &r->ov, &done, FALSE) ||
          done != r->len)
        drop_reader(i);
    }

    // A reader came: ours, or closed at once.
    IpcInstance* in = g_ipc.listening;
    if (in && (!in->pending || HasOverlappedIoCompleted(&in->ov)) &&
        WaitForSingleObject(in->ov.hEvent, 0) == WAIT_OBJECT_0) {
      g_ipc.listening = NULL;
      DWORD done = 0;
      bool connected = !in->pending ||
                       GetOverlappedResult(in->pipe, &in->ov, &done, FALSE) ||
                       GetLastError() == ERROR_PIPE_CONNECTED;
      in->pending = false;
      if (connected && same_user(in->pipe)) {
        ResetEvent(in->ov.hEvent);
        g_ipc.readers[g_ipc.nreaders++] = in;
      } else {
        close_instance(in);
      }
    }
  }
}

bool stats_ipc_on(void) { return g_ipc.on; }

// For another pipe (the test-control one): the same guard, an access list
// of this user and SYSTEM (the security descriptor lives for the process).
bool pipe_security(SECURITY_ATTRIBUTES* sa) {
  static TokenUserBuf user;
  static PSECURITY_DESCRIPTOR sd;
  if (!sd) {
    LPWSTR sid = NULL;
    WCHAR sddl[256];
    if (!token_user(GetCurrentProcess(), &user) || !ConvertSidToStringSidW(sid_of(&user), &sid))
      return false;
    swprintf(sddl, sizeof(sddl) / sizeof(WCHAR), L"D:P(A;;GA;;;%ls)(A;;GA;;;SY)", sid);
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1,
                                                              &sd, NULL))
      return false;
  }
  sa->nLength = sizeof(*sa);
  sa->lpSecurityDescriptor = sd;
  sa->bInheritHandle = FALSE;
  return true;
}

// The client of a pipe runs as this user (g_ipc.user is set by
// stats_ipc_start; the test pipe fills it itself when that one is off).
bool pipe_client_same_user(HANDLE pipe) {
  if (!g_ipc.user.tu.User.Sid && !token_user(GetCurrentProcess(), &g_ipc.user))
    return false;
  return same_user(pipe);
}

// Opens \\.\pipe\ghostty-<pid>.stats unless GHOSTTY_STATS_IPC=off. A failure
// is said on stderr and the host runs without it.
void stats_ipc_start(void) {
  const char* v = getenv("GHOSTTY_STATS_IPC");
  if (v && (strcmp(v, "off") == 0 || strcmp(v, "0") == 0)) return;
  swprintf(g_ipc.name, sizeof(g_ipc.name) / sizeof(WCHAR),
           L"\\\\.\\pipe\\ghostty-%lu.stats", GetCurrentProcessId());
  const char* fail = NULL;
  LPWSTR sid = NULL;
  WCHAR sddl[256];
  if (!token_user(GetCurrentProcess(), &g_ipc.user) ||
      !ConvertSidToStringSidW(sid_of(&g_ipc.user), &sid)) {
    fail = "this process's user";
  } else {
    swprintf(sddl, sizeof(sddl) / sizeof(WCHAR), L"D:P(A;;GA;;;%ls)(A;;GA;;;SY)",
             sid);
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &g_ipc.sd, NULL))
      fail = "the access list";
  }
  if (!fail) {
    g_ipc.sa.nLength = sizeof(g_ipc.sa);
    g_ipc.sa.lpSecurityDescriptor = g_ipc.sd;
    g_ipc.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&g_ipc.lock);
    if (!g_ipc.wake)
      fail = "an event";
    else if (!ipc_listen(true))
      fail = "the pipe (its name taken?)";
  }
  if (!fail) {
    HANDLE t = CreateThread(NULL, 64 * 1024, ipc_main, NULL, 0, NULL);
    if (!t)
      fail = "its thread";
    else
      CloseHandle(t);
  }
  if (fail) {
    fprintf(stderr, "ghostty: stats pipe %ls: no %s (error %lu); running without it\n",
            g_ipc.name, fail, GetLastError());
    return;
  }
  g_ipc.on = true;
}

// The newest line, for every reader ready for it. Never waits on a reader.
void stats_ipc_publish(const char* line, size_t len) {
  if (!g_ipc.on) return;
  char* copy = malloc(len + 1);
  if (!copy) return;
  memcpy(copy, line, len);
  copy[len] = '\n';
  EnterCriticalSection(&g_ipc.lock);
  char* old = g_ipc.latest;
  g_ipc.latest = copy;
  g_ipc.latest_len = len + 1;
  g_ipc.latest_seq++;
  LeaveCriticalSection(&g_ipc.lock);
  free(old);
  SetEvent(g_ipc.wake);
}

// ---------------------------------------------------------------------------
// ghostty.exe /stats [pid] [--count N]
//
// The pipe's client: prints N lines (1 by default, 0 = until the pipe
// ends) on stdout. Without a pid it takes the one Ghostty that has a stats
// pipe, and says which ones when there are several.

static void err_text(const char* s) {
  DWORD w;
  HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
  if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN)
    WriteFile(h, s, (DWORD)strlen(s), &w, NULL);
  else
    print_text(s);
}

// Asks without connecting (opening it would take a reader's place).
static bool pipe_exists(DWORD pid) {
  WCHAR name[64];
  swprintf(name, 64, L"\\\\.\\pipe\\ghostty-%lu.stats", pid);
  return WaitNamedPipeW(name, 1) || GetLastError() == ERROR_SEM_TIMEOUT;
}

static DWORD find_ghostty(char* why, size_t cap) {
  DWORD found = 0, self = GetCurrentProcessId();
  int count = 0;
  size_t used = (size_t)snprintf(why, cap, "ghostty: several Ghostty processes have a stats pipe; give one:");
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe = {sizeof(pe)};
  for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
    if (pe.th32ProcessID == self || _wcsicmp(pe.szExeFile, L"ghostty.exe") != 0)
      continue;
    if (!pipe_exists(pe.th32ProcessID)) continue;
    found = pe.th32ProcessID;
    count++;
    if (used < cap)
      used += (size_t)snprintf(why + used, cap - used, " %lu", pe.th32ProcessID);
  }
  CloseHandle(snap);
  if (used < cap) snprintf(why + used, cap - used, "\n");
  if (count == 0) snprintf(why, cap, "ghostty: no Ghostty with a stats pipe is running\n");
  return count == 1 ? found : 0;
}

bool stats_client_mode(int argc, char** argv, int* rc) {
  if (argc < 2 || (strcmp(argv[1], "/stats") != 0 && strcmp(argv[1], "--stats") != 0))
    return false;
  *rc = 1;
  DWORD pid = 0;
  long count = 1;
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--count") == 0 && i + 1 < argc)
      count = strtol(argv[++i], NULL, 10);
    else
      pid = strtoul(argv[i], NULL, 10);
  }
  char why[1024];
  if (!pid && !(pid = find_ghostty(why, sizeof(why)))) {
    err_text(why);
    return true;
  }
  WCHAR name[64];
  swprintf(name, 64, L"\\\\.\\pipe\\ghostty-%lu.stats", pid);
  HANDLE p = INVALID_HANDLE_VALUE;
  for (int tries = 0; tries < 10; tries++) {
    p = CreateFileW(name, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (p != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) break;
    WaitNamedPipeW(name, 1000);  // all 8 readers' places taken: wait for one
  }
  if (p == INVALID_HANDLE_VALUE) {
    snprintf(why, sizeof(why), "ghostty: %lu: no stats pipe (error %lu)\n", pid,
             GetLastError());
    err_text(why);
    return true;
  }
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  bool to_out = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
  char buf[16384];
  size_t have = 0;
  long lines = 0;
  DWORD n;
  while ((count <= 0 || lines < count) &&
         ReadFile(p, buf + have, (DWORD)(sizeof(buf) - 1 - have), &n, NULL) && n) {
    have += n;
    char* nl;
    while ((count <= 0 || lines < count) && (nl = memchr(buf, '\n', have))) {
      size_t len = (size_t)(nl - buf) + 1;
      DWORD w;
      if (to_out) {
        WriteFile(out, buf, (DWORD)len, &w, NULL);
      } else {
        char one[sizeof(buf)];
        memcpy(one, buf, len);
        one[len] = 0;
        print_text(one);
      }
      memmove(buf, buf + len, have - len);
      have -= len;
      lines++;
    }
    if (have == sizeof(buf) - 1) have = 0;  // a line longer than the buffer
  }
  CloseHandle(p);
  *rc = lines > 0 ? 0 : 1;
  return true;
}
