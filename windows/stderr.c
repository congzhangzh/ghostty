#include "host.h"

// ---------------------------------------------------------------------------
// stderr that never blocks
//
// A write to a pipe nobody reads blocks once the pipe's buffer is full:
// Windows PowerShell's `ghostty.exe 2> log` gives us such a pipe and stops
// reading it when the prompt returns (a GUI program is not waited for).
// Then the next log line (GHOSTTY_LOG=stderr: a warning per click) blocks
// whichever thread writes it, the UI thread too. A console can block the
// same way while text is selected in it.
//
// So when stderr is a pipe or a character device, it is replaced by a pipe
// of our own, before libghostty or the CRT writes anything: SetStdHandle
// for Zig's std (it reads the PEB's hStdError, once) and fd 2 for the CRT.
// A reader thread empties that pipe into a ring at once; a writer thread
// forwards the ring to the real stderr and is the only one that can block.
// When the ring is full, what comes in is dropped and counted, and the
// count is written when the real stderr takes writes again. A disk file
// (cmd's `2> log`) is left alone: a write to it does not wait on anyone.
// GHOSTTY_STDERR=direct turns this off.

#define ERR_RING_SIZE (256 * 1024)

static struct {
  HANDLE out;  // the real stderr
  HANDLE rd;   // our pipe's read end
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE more;  // bytes in the ring, or bytes dropped
  CONDITION_VARIABLE less;  // the writer took bytes out (for the flush)
  char ring[ERR_RING_SIZE];
  size_t head, len;  // len counts the bytes being written, too
  unsigned long long dropped, dropped_total;
  bool on;
} g_err;

static DWORD WINAPI err_reader_main(void* arg) {
  (void)arg;
  char buf[4096];
  DWORD n;
  while (ReadFile(g_err.rd, buf, sizeof(buf), &n, NULL) && n > 0) {
    EnterCriticalSection(&g_err.lock);
    if (ERR_RING_SIZE - g_err.len < n) {
      // Whole reads only: a read is usually whole lines.
      g_err.dropped += n;
      g_err.dropped_total += n;
    } else {
      size_t tail = (g_err.head + g_err.len) % ERR_RING_SIZE;
      size_t first = ERR_RING_SIZE - tail < n ? ERR_RING_SIZE - tail : n;
      memcpy(g_err.ring + tail, buf, first);
      memcpy(g_err.ring, buf + first, n - first);
      g_err.len += n;
    }
    WakeConditionVariable(&g_err.more);
    LeaveCriticalSection(&g_err.lock);
  }
  return 0;
}

static DWORD WINAPI err_writer_main(void* arg) {
  (void)arg;
  for (;;) {
    EnterCriticalSection(&g_err.lock);
    while (g_err.len == 0 && g_err.dropped == 0)
      SleepConditionVariableCS(&g_err.more, &g_err.lock, INFINITE);
    char note[96];
    const char* p;
    size_t n;
    bool is_note = g_err.len == 0;
    if (is_note) {
      n = (size_t)snprintf(note, sizeof(note),
                           "ghostty: %llu bytes of log dropped: stderr was "
                           "not read\n",
                           g_err.dropped);
      g_err.dropped = 0;
      p = note;
    } else {
      p = g_err.ring + g_err.head;
      n = ERR_RING_SIZE - g_err.head < g_err.len ? ERR_RING_SIZE - g_err.head
                                                 : g_err.len;
    }
    LeaveCriticalSection(&g_err.lock);

    // May block for ever; only this thread waits then.
    DWORD w = 0;
    size_t done = 0;
    while (done < n &&
           WriteFile(g_err.out, p + done, (DWORD)(n - done), &w, NULL) && w)
      done += w;

    EnterCriticalSection(&g_err.lock);
    if (!is_note) {
      // Taken out even when the write failed: nobody would read it anyway.
      g_err.head = (g_err.head + n) % ERR_RING_SIZE;
      g_err.len -= n;
    }
    WakeAllConditionVariable(&g_err.less);
    LeaveCriticalSection(&g_err.lock);
  }
}

void stderr_guard_start(void) {
  const char* mode = getenv("GHOSTTY_STDERR");
  if (mode && strcmp(mode, "direct") == 0) return;
  HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
  if (!out || out == INVALID_HANDLE_VALUE) return;
  DWORD type = GetFileType(out);
  if (type != FILE_TYPE_PIPE && type != FILE_TYPE_CHAR) return;

  // Not inheritable (no SECURITY_ATTRIBUTES): the shells in the tabs and
  // the report helper never hold our pipe.
  HANDLE rd, wr;
  if (!CreatePipe(&rd, &wr, NULL, 64 * 1024)) return;
  // Our own copy of the real stderr: _dup2 below closes the CRT's fd 2,
  // and the handle under it with it.
  if (!DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &out, 0,
                       FALSE, DUPLICATE_SAME_ACCESS)) {
    CloseHandle(rd);
    CloseHandle(wr);
    return;
  }
  // Text mode, as the CRT's own stderr was (its "\n" becomes "\r\n").
  int fd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
  if (fd < 0) {
    CloseHandle(out);
    CloseHandle(rd);
    CloseHandle(wr);
    return;
  }
  g_err.out = out;
  g_err.rd = rd;
  InitializeCriticalSection(&g_err.lock);
  InitializeConditionVariable(&g_err.more);
  InitializeConditionVariable(&g_err.less);
  // Suspended until both exist: a thread that never ran is safe to end.
  HANDLE t1 = CreateThread(NULL, 64 * 1024, err_reader_main, NULL,
                           CREATE_SUSPENDED, NULL);
  HANDLE t2 = CreateThread(NULL, 64 * 1024, err_writer_main, NULL,
                           CREATE_SUSPENDED, NULL);
  if (!t1 || !t2) {
    // Leave stderr as it was.
    if (t1) TerminateThread(t1, 0), CloseHandle(t1);
    if (t2) TerminateThread(t2, 0), CloseHandle(t2);
    _close(fd);
    CloseHandle(rd);
    CloseHandle(out);
    return;
  }
  ResumeThread(t1);
  ResumeThread(t2);
  CloseHandle(t1);
  CloseHandle(t2);
  fflush(stderr);
  _dup2(fd, 2);  // the CRT's stderr (fd 2) now writes to our pipe
  _close(fd);
  SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(2));
  setvbuf(stderr, NULL, _IONBF, 0);
  g_err.on = true;
}

// At exit: give what is still in our pipe and ring up to `ms` to reach the
// real stderr (which may never take it).
void stderr_guard_flush(DWORD ms) {
  if (!g_err.on) return;
  fflush(stderr);
  DWORD start = GetTickCount();
  for (;;) {
    DWORD left = ms - (GetTickCount() - start);
    if ((int)left <= 0) return;
    DWORD avail = 0;
    PeekNamedPipe(g_err.rd, NULL, 0, NULL, &avail, NULL);
    EnterCriticalSection(&g_err.lock);
    bool empty = avail == 0 && g_err.len == 0 && g_err.dropped == 0;
    if (!empty)
      SleepConditionVariableCS(&g_err.less, &g_err.lock, left < 20 ? left : 20);
    LeaveCriticalSection(&g_err.lock);
    if (empty) return;
  }
}
