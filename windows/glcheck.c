// glcheck.c (lane B): say why Ghostty could not start when its first window
// fails, instead of exiting with nothing on the screen.
//
// Ghostty draws with an OpenGL 4.3 core context (src/renderer/OpenGL.zig).
// pkg/opengl/wgl.zig tries the system's opengl32.dll first, then Mesa's in
// the package's opengl\ folder (software rendering). When neither gives a
// context, ghostty_surface_new fails and so does the first window. This
// looks at the system's OpenGL itself (a hidden window, a plain context,
// GL_VENDOR / GL_RENDERER / GL_VERSION, and whether the pixel format is
// Microsoft's GDI generic one, i.e. no graphics driver), checks for Mesa's
// DLL, and shows what it found and what helps in a message box, also on
// stderr (the log).

#include "host.h"

// wgl.zig: "system", "mesa", or "none" while no context was made.
extern const char* ghostty_windows_gl_driver(void);

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

typedef HGLRC(WINAPI* create_t)(HDC);
typedef BOOL(WINAPI* delete_t)(HGLRC);
typedef BOOL(WINAPI* current_t)(HDC, HGLRC);
typedef PROC(WINAPI* procaddr_t)(LPCSTR);
typedef const unsigned char*(WINAPI* getstring_t)(unsigned int);

// What the system's opengl32.dll gives: one line into `out`. Returns true
// when the pixel format is GDI generic (Microsoft's OpenGL 1.1, no driver).
static bool system_gl(char* out, size_t n) {
  WCHAR path[MAX_PATH + 32];
  UINT len = GetSystemDirectoryW(path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    snprintf(out, n, "(no system directory)");
    return false;
  }
  wcscpy(path + len, L"\\opengl32.dll");
  // By its full path: the system's, whatever else is loaded.
  HMODULE gl = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!gl) {
    snprintf(out, n, "opengl32.dll did not load (error %lu)", GetLastError());
    return false;
  }
  create_t create = (create_t)(void*)GetProcAddress(gl, "wglCreateContext");
  delete_t del = (delete_t)(void*)GetProcAddress(gl, "wglDeleteContext");
  current_t cur = (current_t)(void*)GetProcAddress(gl, "wglMakeCurrent");
  procaddr_t procaddr = (procaddr_t)(void*)GetProcAddress(gl, "wglGetProcAddress");
  getstring_t gs = (getstring_t)(void*)GetProcAddress(gl, "glGetString");
  if (!create || !del || !cur || !procaddr || !gs) {
    snprintf(out, n, "opengl32.dll lacks the WGL functions");
    return false;
  }

  // A class of its own with CS_OWNDC, as wgl.zig's: the DC keeps the pixel format.
  WNDCLASSW wc = {0};
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.lpszClassName = L"GhosttyGlCheck";
  RegisterClassW(&wc);  // fails harmlessly when it exists
  HWND hwnd = CreateWindowExW(0, L"GhosttyGlCheck", L"glcheck", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                              NULL, NULL, GetModuleHandleW(NULL), NULL);
  if (!hwnd) {
    snprintf(out, n, "(no window to test with, error %lu)", GetLastError());
    return false;
  }
  HDC dc = GetDC(hwnd);
  PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1};
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cAlphaBits = 8;
  bool generic = false;
  int pf = ChoosePixelFormat(dc, &pfd);
  PIXELFORMATDESCRIPTOR got = {0};
  if (pf && DescribePixelFormat(dc, pf, sizeof got, &got))
    generic = (got.dwFlags & PFD_GENERIC_FORMAT) &&
              !(got.dwFlags & PFD_GENERIC_ACCELERATED);
  HGLRC rc = NULL;
  if (pf && SetPixelFormat(dc, pf, &pfd)) rc = create(dc);
  if (rc && cur(dc, rc)) {
    const unsigned char* v = gs(GL_VENDOR);
    const unsigned char* r = gs(GL_RENDERER);
    const unsigned char* ver = gs(GL_VERSION);
    bool arb = procaddr("wglCreateContextAttribsARB") != NULL;
    snprintf(out, n, "OpenGL %s, %s (%s)%s%s", ver ? (const char*)ver : "?",
             r ? (const char*)r : "?", v ? (const char*)v : "?",
             generic ? ": Microsoft's GDI generic OpenGL, i.e. no graphics driver" : "",
             arb ? "" : "; no wglCreateContextAttribsARB");
    cur(NULL, NULL);
  } else {
    snprintf(out, n, "no OpenGL context at all (pixel format %d, error %lu)", pf,
             GetLastError());
  }
  if (rc) del(rc);
  ReleaseDC(hwnd, dc);
  DestroyWindow(hwnd);
  return generic;
}

// Is Mesa's opengl32.dll in the package (opengl\ beside ghostty.exe)?
static bool have_mesa(void) {
  WCHAR path[MAX_PATH + 32];
  DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return false;
  WCHAR* slash = wcsrchr(path, L'\\');
  if (!slash) return false;
  wcscpy(slash + 1, L"opengl\\opengl32.dll");
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void glcheck_explain(void) {
  const char* driver = ghostty_windows_gl_driver();
  if (strcmp(driver, "none") != 0) {
    // A context was made, so OpenGL is not why the window failed.
    fprintf(stderr, "ghostty: the first window could not be created (OpenGL: %s)\n", driver);
    MessageBoxW(NULL,
                L"Ghostty could not create its first window.\n\n"
                L"OpenGL works here, so the cause is elsewhere: the log says "
                L"more (start Ghostty from a console with GHOSTTY_LOG=stderr).",
                L"Ghostty", MB_OK | MB_ICONERROR);
    return;
  }
  char sys[512];
  bool generic = system_gl(sys, sizeof sys);
  bool mesa = have_mesa();
  fprintf(stderr,
          "ghostty: glcheck: no OpenGL 4.3 core context. System: %s. Mesa "
          "(opengl\\opengl32.dll): %s\n",
          sys, mesa ? "in the package, and it failed too" : "not in the package");

  char text[2048];
  snprintf(text, sizeof text,
           "Ghostty could not start: it draws with OpenGL 4.3, and this "
           "machine gave none.\n\n"
           "This Windows's OpenGL: %s.\n\n"
           "Mesa's software OpenGL (opengl\\opengl32.dll beside ghostty.exe): %s.\n\n"
           "What helps:\n"
           "%s"
           "- Use the whole package as it is unzipped: the opengl folder must stay "
           "beside ghostty.exe, so Ghostty can fall back to Mesa.\n"
           "- GHOSTTY_GL=mesa makes Ghostty use Mesa's OpenGL; GHOSTTY_GL=system, "
           "the system's.\n\n"
           "README.txt in the package says more.",
           sys,
           mesa ? "it is there, and it failed as well (see the log)" : "missing",
           generic ? "- This Windows has no graphics driver with OpenGL (a virtual "
                     "machine's basic display adapter, for one): install the "
                     "driver of the GPU or the VM's guest tools, or use Mesa's.\n"
                   : "- Update the graphics driver: Ghostty needs OpenGL 4.3.\n");
  WCHAR wtext[2048];
  if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, 2048)) wcscpy(wtext, L"Ghostty could not start: no OpenGL 4.3.");
  MessageBoxW(NULL, wtext, L"Ghostty: no OpenGL 4.3", MB_OK | MB_ICONERROR);
}
