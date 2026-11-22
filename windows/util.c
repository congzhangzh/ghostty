#include "host.h"

uint64_t qpc(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (uint64_t)t.QuadPart;
}

// ---------------------------------------------------------------------------
// UTF-8 / UTF-16

// Returns a malloc'd UTF-8 string (caller frees), or NULL.
char* utf16_to_utf8(const WCHAR* s, int len) {
  int n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char* out = malloc((size_t)n + 1);
  if (!out) return NULL;
  WideCharToMultiByte(CP_UTF8, 0, s, len, out, n, NULL, NULL);
  out[n] = 0;
  return out;
}

// Returns a malloc'd UTF-16 string (caller frees), or NULL.
WCHAR* utf8_to_utf16(const char* s, int len) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
  if (n <= 0) return NULL;
  WCHAR* out = malloc(((size_t)n + 1) * sizeof(WCHAR));
  if (!out) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, s, len, out, n);
  out[n] = 0;
  return out;
}
