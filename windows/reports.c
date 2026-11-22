#include "host.h"

// ---------------------------------------------------------------------------
// Hang and crash reports
//
// Hangs: a watchdog thread sends the main thread an empty message every
// second. When it gets no answer for 5 seconds (the point where Windows
// shows "Not Responding"), it starts this same exe again as
//   ghostty.exe --windows-hang-report <pid> <main thread id> <file>
// Crashes: an unhandled exception (an access violation, a Zig panic, which
// libghostty raises as GHOSTTY_PANIC_CODE on Windows) or abort() starts
//   ghostty.exe --windows-crash-report <pid> <thread id> <file> <exception pointers>
// and the crashed thread waits for it. Either way the second process
// suspends our threads, walks their stacks (the crashed one from the
// exception's own context) and writes them, module+offset, with a minidump
// beside them, to %LOCALAPPDATA%\ghostty\hangs or ...\crashes. Another
// process does the walking because a thread stopped inside the heap or the
// loader would deadlock a walker in this one (Crashpad's reason too). No
// log setup, no tool beside the exe.

// libghostty's panic exception (src/main_c.zig windows_panic_code): its
// parameters are the message's address and length.

volatile LONG g_loop_done;  // the message loop has ended (quitting)
static WCHAR g_exe[MAX_PATH];       // this exe, for the report process

// %LOCALAPPDATA%\ghostty\<kind>, created; false if there is no such folder.
static bool report_dir(const WCHAR* kind, WCHAR* out, size_t cap) {
  WCHAR base[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  _snwprintf(out, cap, L"%ls\\ghostty", base);
  CreateDirectoryW(out, NULL);
  _snwprintf(out, cap, L"%ls\\ghostty\\%ls", base, kind);
  CreateDirectoryW(out, NULL);
  return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// Start the report process; `wait`: until it has finished (a crash: the
// process must still be here while it is read). Returns the report's path
// in `file` (empty if none was started).
static void start_report(const WCHAR* kind, DWORD tid, const void* ep,
                         bool wait, WCHAR* file, size_t cap) {
  WCHAR dir[MAX_PATH], cmd[3 * MAX_PATH + 64];
  file[0] = 0;
  if (!g_exe[0] || !report_dir(kind, dir, MAX_PATH)) return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  _snwprintf(file, cap, L"%ls\\%.5ls-%04d%02d%02d-%02d%02d%02d-%lu.txt", dir,
             kind, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             GetCurrentProcessId());
  _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]),
             L"\"%ls\" --windows-%.5ls-report %lu %lu \"%ls\" %llx", g_exe,
             kind, GetCurrentProcessId(), tid, file,
             (unsigned long long)(uintptr_t)ep);
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi;
  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                      NULL, &si, &pi)) {
    file[0] = 0;
    return;
  }
  CloseHandle(pi.hThread);
  if (wait) WaitForSingleObject(pi.hProcess, 60000);
  CloseHandle(pi.hProcess);
}

static void say_report(const char* what, const WCHAR* file) {
  // Last, and after the report: stderr itself may be what blocks.
  char* f8 = utf16_to_utf8(file, -1);
  fprintf(stderr, "ghostty: %s; report: %s\n", what, f8 && *f8 ? f8 : "none");
  fflush(stderr);
  free(f8);
}

static DWORD WINAPI watchdog_main(void* arg) {
  (void)arg;
  DWORD silent = 0;
  int reports = 0;
  for (;;) {
    Sleep(1000);
    DWORD_PTR r;
    if (SendMessageTimeoutW(g_wakeup_hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 1000,
                            &r)) {
      silent = 0;
      continue;
    }
    silent += 2;  // the second asleep and the second waited
    // One report per hang, at most three per run.
    if (silent >= 5 && silent < 7 && reports < 3) {
      reports++;
      WCHAR file[MAX_PATH];
      start_report(L"hangs", g_main_tid, NULL, false, file, MAX_PATH);
      say_report(g_loop_done
                     ? "the main thread has not answered for 5s (after the "
                       "message loop ended)"
                     : "the main thread has not answered for 5s",
                 file);
    }
  }
  return 0;
}

static volatile LONG g_crashing;

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
  if (InterlockedExchange(&g_crashing, 1)) return EXCEPTION_CONTINUE_SEARCH;
  WCHAR file[MAX_PATH];
  start_report(L"crashes", GetCurrentThreadId(), ep, true, file, MAX_PATH);
  char what[64];
  snprintf(what, sizeof(what), "crashed (exception 0x%08lx)",
           ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
  say_report(what, file);
  // Windows goes on as before: Error Reporting, then the process ends.
  return EXCEPTION_CONTINUE_SEARCH;
}

static void abort_handler(int sig) {
  (void)sig;
  if (!InterlockedExchange(&g_crashing, 1)) {
    WCHAR file[MAX_PATH];
    start_report(L"crashes", GetCurrentThreadId(), NULL, true, file, MAX_PATH);
    say_report("aborted", file);
  }
  _exit(3);  // not the C runtime's abort dialog
}

void start_reports(void) {
  if (!GetModuleFileNameW(NULL, g_exe, MAX_PATH)) g_exe[0] = 0;
  if (getenv("GHOSTTY_NO_REPORTS")) return;
  SetUnhandledExceptionFilter(crash_filter);
  signal(SIGABRT, abort_handler);
  HANDLE h = CreateThread(NULL, 0, watchdog_main, NULL, 0, NULL);
  if (h) CloseHandle(h);
}

// The report itself, in the second process.

#define REPORT_MAX_FRAMES 64

static void report_frame(FILE* out, HANDLE proc, DWORD64 a) {
  IMAGEHLP_MODULEW64 mi = {sizeof(mi)};
  DWORD64 base = 0;
  WCHAR mod[64] = L"?";
  if (SymGetModuleInfoW64(proc, a, &mi)) {
    _snwprintf(mod, 64, L"%ls", mi.ModuleName);
    base = mi.BaseOfImage;
  }
  char buf[sizeof(SYMBOL_INFO) + 256];
  SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(proc, a, &disp, si))
    fprintf(out, "    %ls+0x%llx  %s+0x%llx\n", mod,
            (unsigned long long)(a - base), si->Name, (unsigned long long)disp);
  else
    fprintf(out, "    %ls+0x%llx\n", mod, (unsigned long long)(a - base));
}

// Walk a stack from `ctx` (a copy: StackWalk64 changes it). The CONTEXT
// and the machine are this build's own: the helper reads its own exe's
// threads.
#if defined(__aarch64__) || defined(_M_ARM64)
#define WALK_MACHINE IMAGE_FILE_MACHINE_ARM64
#define WALK_PC(c) (c).Pc
#define WALK_SP(c) (c).Sp
#define WALK_FP(c) (c).Fp
#elif defined(__x86_64__) || defined(_M_X64)
#define WALK_MACHINE IMAGE_FILE_MACHINE_AMD64
#define WALK_PC(c) (c).Rip
#define WALK_SP(c) (c).Rsp
#define WALK_FP(c) (c).Rbp
#else
#error "stack walk: x86_64 or aarch64 only"
#endif
static int walk(HANDLE proc, HANDLE th, CONTEXT ctx, DWORD64* frames) {
  STACKFRAME64 sf = {0};
  sf.AddrPC.Offset = WALK_PC(ctx);
  sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrStack.Offset = WALK_SP(ctx);
  sf.AddrStack.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = WALK_FP(ctx);
  sf.AddrFrame.Mode = AddrModeFlat;
  int n = 0;
  while (n < REPORT_MAX_FRAMES &&
         StackWalk64(WALK_MACHINE, proc, th, &sf, &ctx, NULL,
                     SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
    if (!sf.AddrPC.Offset) break;
    frames[n++] = sf.AddrPC.Offset;
  }
  return n;
}

typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);

static const char* exception_name(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    case 0xC0000374: return "heap corruption";
    case GHOSTTY_PANIC_CODE: return "Zig panic";
    default: return "?";
  }
}

// The exception, read from the crashed process; fills *ctx and returns true
// if there was one.
static bool report_exception(FILE* out, HANDLE proc, uintptr_t ep_addr,
                             CONTEXT* ctx) {
  EXCEPTION_POINTERS ep;
  EXCEPTION_RECORD er;
  SIZE_T got;
  if (!ep_addr ||
      !ReadProcessMemory(proc, (void*)ep_addr, &ep, sizeof(ep), &got) ||
      !ReadProcessMemory(proc, ep.ExceptionRecord, &er, sizeof(er), &got) ||
      !ReadProcessMemory(proc, ep.ContextRecord, ctx, sizeof(*ctx), &got)) {
    fprintf(out, "exception: none (abort)\n\n");
    return false;
  }
  fprintf(out, "exception: 0x%08lx %s\nat: ", er.ExceptionCode,
          exception_name(er.ExceptionCode));
  report_frame(out, proc, (DWORD64)(uintptr_t)er.ExceptionAddress);
  if (er.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er.NumberParameters >= 2)
    fprintf(out, "  %s address 0x%llx\n",
            er.ExceptionInformation[0] == 0   ? "reading"
            : er.ExceptionInformation[0] == 8 ? "executing"
                                              : "writing",
            (unsigned long long)er.ExceptionInformation[1]);
  if (er.ExceptionCode == GHOSTTY_PANIC_CODE && er.NumberParameters >= 2) {
    char msg[2048];
    size_t len = er.ExceptionInformation[1];
    if (len >= sizeof(msg)) len = sizeof(msg) - 1;
    if (ReadProcessMemory(proc, (void*)er.ExceptionInformation[0], msg, len,
                          &got)) {
      msg[got] = 0;
      fprintf(out, "  panic: %s\n", msg);
    }
  }
  fprintf(out, "\n");
  return true;
}

static int write_report(bool crash, DWORD pid, DWORD tid, const WCHAR* path,
                        uintptr_t ep_addr) {
  FILE* out = _wfopen(path, L"w");
  if (!out) return 1;
  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                PROCESS_DUP_HANDLE,
                            FALSE, pid);
  if (!proc) {
    fprintf(out, "OpenProcess(%lu): error %lu\n", pid, GetLastError());
    fclose(out);
    return 1;
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  WCHAR exe[MAX_PATH] = L"?";
  DWORD exe_len = MAX_PATH;
  QueryFullProcessImageNameW(proc, 0, exe, &exe_len);
  fprintf(out,
          "ghostty %s report %04d-%02d-%02d %02d:%02d:%02d\n"
          "pid %lu, %s thread %lu\nexe %ls\n\n",
          crash ? "crash" : "hang", t.wYear, t.wMonth, t.wDay, t.wHour,
          t.wMinute, t.wSecond, pid, crash ? "crashed" : "main (UI)", tid, exe);

  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitializeW(proc, NULL, TRUE);

  // A crash: the exception, and the crashed thread's stack from its own
  // context (its current stack is only our filter waiting for us).
  CONTEXT ectx;
  if (crash && report_exception(out, proc, ep_addr, &ectx)) {
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
    DWORD64 frames[REPORT_MAX_FRAMES];
    int n = walk(proc, th, ectx, frames);
    fprintf(out, "crashed thread %lu, at the exception:\n", tid);
    for (int i = 0; i < n; i++) report_frame(out, proc, frames[i]);
    fprintf(out, "\n");
    if (th) CloseHandle(th);
  }

  // The modules, so an offset can be matched to its build.
  HMODULE mods[512];
  DWORD need = 0;
  if (K32EnumProcessModules(proc, mods, sizeof(mods), &need)) {
    fprintf(out, "modules:\n");
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) {
      WCHAR name[MAX_PATH];
      MODULEINFO mi;
      if (K32GetModuleFileNameExW(proc, mods[i], name, MAX_PATH) &&
          K32GetModuleInformation(proc, mods[i], &mi, sizeof(mi)))
        fprintf(out, "  %p %8lx %ls\n", mi.lpBaseOfDll, mi.SizeOfImage, name);
    }
    fprintf(out, "\n");
  }

  GetThreadDescriptionFn describe = (GetThreadDescriptionFn)(void*)GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");

  // Every thread: three samples for a hang (what moves, what does not),
  // one for a crash.
  int samples = crash ? 1 : 3;
  for (int s = 0; s < samples; s++) {
    if (s) Sleep(700);
    fprintf(out, "=== %s %d\n", crash ? "threads" : "sample", s + 1);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid) continue;
      HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_INFORMATION,
                             FALSE, te.th32ThreadID);
      if (!th) continue;
      FILETIME c, e, k, u;
      GetThreadTimes(th, &c, &e, &k, &u);
      double ks = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) / 1e7;
      double us = (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime) / 1e7;
      DWORD64 frames[REPORT_MAX_FRAMES];
      int n = 0;
      if (SuspendThread(th) != (DWORD)-1) {
        CONTEXT ctx = {0};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) n = walk(proc, th, ctx, frames);
        ResumeThread(th);
      }
      PWSTR desc = NULL;
      if (describe && FAILED(describe(th, &desc))) desc = NULL;
      fprintf(out, "tid %lu%s%s%ls  kernel %.1fs user %.1fs  frames %d\n",
              te.th32ThreadID, te.th32ThreadID == tid ? (crash ? " (crashed)" : " (main)") : "",
              desc && *desc ? " " : "", desc && *desc ? desc : L"", ks, us, n);
      if (desc) LocalFree(desc);
      for (int i = 0; i < n; i++) report_frame(out, proc, frames[i]);
      CloseHandle(th);
    }
    CloseHandle(snap);
    fflush(out);
  }

  // A minidump beside it: every thread's stack memory (and, for a crash,
  // the exception), for a debugger.
  WCHAR dmp[MAX_PATH];
  _snwprintf(dmp, MAX_PATH, L"%ls", path);
  WCHAR* dot = wcsrchr(dmp, L'.');
  if (dot) wcscpy(dot, L".dmp");
  HANDLE f = CreateFileW(dmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei = {tid, (EXCEPTION_POINTERS*)ep_addr,
                                          TRUE};
    BOOL ok = MiniDumpWriteDump(
        proc, pid, f,
        (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                        MiniDumpWithHandleData),
        crash && ep_addr ? &mei : NULL, NULL, NULL);
    CloseHandle(f);
    fprintf(out, "\nminidump: %ls (%s)\n", dmp, ok ? "written" : "failed");
  }
  fclose(out);
  SymCleanup(proc);
  CloseHandle(proc);
  return 0;
}

// `ghostty.exe --windows-hang-report|--windows-crash-report <pid> <tid>
// <file> <exception pointers, hex>`: true if this run is one (and it has
// been done).
bool report_mode(int* rc) {
  int argc = 0;
  WCHAR** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  bool hang = argc == 6 && wcscmp(argv[1], L"--windows-hangs-report") == 0;
  bool crash = argc == 6 && wcscmp(argv[1], L"--windows-crash-report") == 0;
  if (hang || crash)
    *rc = write_report(crash, (DWORD)wcstoul(argv[2], NULL, 10),
                       (DWORD)wcstoul(argv[3], NULL, 10), argv[4],
                       (uintptr_t)wcstoull(argv[5], NULL, 16));
  LocalFree(argv);
  return hang || crash;
}
