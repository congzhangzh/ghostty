#define COBJMACROS
#include "host.h"

#include <GL/gl.h>
#include <d3d10.h>  // ID3D10Multithread (the GL driver uses the device from the render thread)
#include <d3d11.h>
#include <dxgi1_3.h>  // IDXGISwapChain2: the latency object

// ---------------------------------------------------------------------------
// Zero-copy step (a), steps 1 and 2 (plans/interop-plan.v1.md): presenting
// through a DXGI flip-model swap chain, and finding out whether GL can share
// a texture with D3D11 (WGL_NV_DX_interop2).
//
// GHOSTTY_PRESENT (opt-in, never the default):
//   interop  the interop path when the extension is there; until its blit
//            lands (step 3), frames go through the DXGI path below. Without
//            the extension (Wine, Mesa, CI runners, likely Windows on Arm):
//            SwapBuffers ("swap"), said once.
//   dxgi     the frames read back as today, uploaded into a FLIP_DISCARD
//            swap chain on the pane's window (2 buffers, BGRA8, a waitable
//            latency object, at most 1 frame queued) instead of GDI.
// The D3D11 device is on GL's GPU (a hybrid laptop has two): the adapter
// whose LUID GL reports (GL_EXT_memory_object_win32), else the one of GL's
// vendor, else the default one. Any failure falls back to GDI, said once.

#define GL_DEVICE_LUID_EXT 0x9599

typedef const char*(WINAPI* GetExtStringARB)(HDC);
typedef void(APIENTRY* GetUnsignedBytevEXT)(GLenum, GLubyte*);
typedef const GLubyte*(APIENTRY* GetStringi)(GLenum, GLuint);

static struct {
  bool probed;
  bool interop;      // WGL_NV_DX_interop2 offered
  bool have_luid;    // GL told its device's LUID
  LUID luid;
  UINT vendor_id;    // PCI vendor of GL's vendor string, or 0
  char vendor[96], renderer[128];
  const char* why;   // why interop is not there
} g_gl;

static bool chain_ready(Window* w, UINT width, UINT height);
static bool device_ready(void);

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGIFactory2* g_factory;
static char g_adapter[128];
static bool g_dxgi_failed;

// ---- step 1: what GL offers ----------------------------------------------------

static bool gl_has_extension(const char* name) {
  GetStringi getStringi = (GetStringi)wglGetProcAddress("glGetStringi");
  if (getStringi) {
    GLint n = 0;
    glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &n);
    for (GLint i = 0; i < n; i++) {
      const char* e = (const char*)getStringi(GL_EXTENSIONS, (GLuint)i);
      if (e && strcmp(e, name) == 0) return true;
    }
    return false;
  }
  const char* all = (const char*)glGetString(GL_EXTENSIONS);
  return all && strstr(all, name);
}

// A throwaway context on a hidden window (the core's context lives on the
// render thread): the vendor, the WGL extensions, the device's LUID.
static void probe_gl(void) {
  if (g_gl.probed) return;
  g_gl.probed = true;
  if (strcmp(ghostty_windows_gl_driver(), "mesa") == 0) {
    g_gl.why = "Mesa's OpenGL (no D3D interop)";
    return;
  }
  HWND h = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 8, 8, NULL, NULL, g_inst, NULL);
  HDC dc = h ? GetDC(h) : NULL;
  PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                               PFD_TYPE_RGBA, 32};
  HGLRC rc = NULL;
  if (dc && SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) rc = wglCreateContext(dc);
  if (!rc || !wglMakeCurrent(dc, rc)) {
    g_gl.why = "no OpenGL context for the probe";
  } else {
    const char* v = (const char*)glGetString(GL_VENDOR);
    const char* r = (const char*)glGetString(GL_RENDERER);
    snprintf(g_gl.vendor, sizeof(g_gl.vendor), "%s", v ? v : "");
    snprintf(g_gl.renderer, sizeof(g_gl.renderer), "%s", r ? r : "");
    if (strstr(g_gl.vendor, "NVIDIA")) g_gl.vendor_id = 0x10DE;
    else if (strstr(g_gl.vendor, "ATI") || strstr(g_gl.vendor, "AMD")) g_gl.vendor_id = 0x1002;
    else if (strstr(g_gl.vendor, "Intel")) g_gl.vendor_id = 0x8086;
    GetExtStringARB ext = (GetExtStringARB)wglGetProcAddress("wglGetExtensionsStringARB");
    const char* e = ext ? ext(dc) : NULL;
    g_gl.interop = e && strstr(e, "WGL_NV_DX_interop2");
    if (!g_gl.interop) g_gl.why = "the OpenGL driver has no WGL_NV_DX_interop2";
    if (gl_has_extension("GL_EXT_memory_object_win32")) {
      GetUnsignedBytevEXT get = (GetUnsignedBytevEXT)wglGetProcAddress("glGetUnsignedBytevEXT");
      if (get) {
        GLubyte luid[8] = {0};
        get(GL_DEVICE_LUID_EXT, luid);
        memcpy(&g_gl.luid, luid, sizeof(LUID));
        g_gl.have_luid = g_gl.luid.LowPart || g_gl.luid.HighPart;
      }
    }
    wglMakeCurrent(NULL, NULL);
  }
  if (rc) wglDeleteContext(rc);
  if (dc) ReleaseDC(h, dc);
  if (h) DestroyWindow(h);
}

// GHOSTTY_PRESENT=interop: kept (and served by the DXGI path until the
// blit lands) when the extension is there, else swap. At start, before any
// surface (the core reads GHOSTTY_PRESENT on its render thread).
// GHOSTTY_INTEROP_FORCE=1: the textures are made and handed over even
// without the extension (tests: the core then reads frames back, and says
// why).
static bool interop_forced(void) {
  const char* v = getenv("GHOSTTY_INTEROP_FORCE");
  return v && strcmp(v, "1") == 0;
}

void present_mode_resolve(void) {
  const char* v = getenv("GHOSTTY_PRESENT");
  if (!v || strcmp(v, "interop") != 0) return;
  probe_gl();
  if (interop_forced()) {
    fprintf(stderr, "ghostty: present: interop forced (GHOSTTY_INTEROP_FORCE=1)%s\n",
            g_gl.interop ? "" : ", though the probe found no WGL_NV_DX_interop2");
  } else if (g_gl.interop) {
    fprintf(stderr, "ghostty: present: interop available (%s, %s%s); frames through DXGI (the GL blit "
                    "is not in yet)\n",
            g_gl.vendor, g_gl.renderer, g_gl.have_luid ? ", LUID known" : "");
  } else {
    fprintf(stderr, "ghostty: present: interop unavailable: %s; using swap\n", g_gl.why);
    _putenv("GHOSTTY_PRESENT=swap");
  }
}

// The present mode a new pane uses on the host's side: the DXGI path?
bool present_dxgi_wanted(void) {
  const char* v = getenv("GHOSTTY_PRESENT");
  return v && (strcmp(v, "dxgi") == 0 || strcmp(v, "interop") == 0) && !g_dxgi_failed;
}

// For the stats line.
void present_stats_json(Line* b) {
  put(b, ",\"interop\":{\"probed\":%s,\"available\":%s,\"vendor\":", g_gl.probed ? "true" : "false",
      g_gl.interop ? "true" : "false");
  put_string(b, g_gl.vendor);
  put(b, ",\"luid\":%s,\"adapter\":", g_gl.have_luid ? "true" : "false");
  put_string(b, g_adapter);
  put(b, ",\"why\":");
  put_string(b, g_gl.why ? g_gl.why : "");
  put(b, "}");
}

// ---- step 2: the swap chain -----------------------------------------------------------

// GL's adapter: by LUID, else by vendor, else the first.
static IDXGIAdapter1* pick_adapter(IDXGIFactory1* f) {
  probe_gl();
  IDXGIAdapter1 *a = NULL, *by_vendor = NULL, *first = NULL;
  for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
    DXGI_ADAPTER_DESC1 d;
    IDXGIAdapter1_GetDesc1(a, &d);
    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE && first) {
      IDXGIAdapter1_Release(a);
      continue;
    }
    if (g_gl.have_luid && d.AdapterLuid.LowPart == g_gl.luid.LowPart &&
        d.AdapterLuid.HighPart == g_gl.luid.HighPart) {
      if (by_vendor) IDXGIAdapter1_Release(by_vendor);
      if (first) IDXGIAdapter1_Release(first);
      return a;
    }
    if (!by_vendor && g_gl.vendor_id && d.VendorId == g_gl.vendor_id) {
      by_vendor = a;
      IDXGIAdapter1_AddRef(a);
    }
    if (!first)
      first = a;
    else
      IDXGIAdapter1_Release(a);
  }
  if (by_vendor) {
    if (first) IDXGIAdapter1_Release(first);
    return by_vendor;
  }
  return first;
}

static bool device_ready(void) {
  if (g_dev) return true;
  if (g_dxgi_failed) return false;
  IDXGIFactory1* f1 = NULL;
  if (CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&f1) != S_OK) goto fail;
  IDXGIAdapter1* a = pick_adapter(f1);
  if (a) {
    DXGI_ADAPTER_DESC1 d;
    IDXGIAdapter1_GetDesc1(a, &d);
    char* name = utf16_to_utf8(d.Description, -1);
    snprintf(g_adapter, sizeof(g_adapter), "%s", name ? name : "");
    free(name);
  }
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice((IDXGIAdapter*)a, a ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &g_dev, &fl,
                                 &g_ctx);
  if (a) IDXGIAdapter1_Release(a);
  if (hr != S_OK || IDXGIFactory1_QueryInterface(f1, &IID_IDXGIFactory2, (void**)&g_factory) != S_OK) {
    IDXGIFactory1_Release(f1);
    goto fail;
  }
  IDXGIFactory1_Release(f1);
  // The GL driver uses the device from the render thread (interop), the
  // host from this one.
  ID3D10Multithread* mt = NULL;
  if (ID3D11Device_QueryInterface(g_dev, &IID_ID3D10Multithread, (void**)&mt) == S_OK) {
    ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
    ID3D10Multithread_Release(mt);
  }
  fprintf(stderr, "ghostty: present: DXGI flip model on %s (feature level 0x%x)\n", g_adapter, (unsigned)fl);
  return true;
fail:
  fprintf(stderr, "ghostty: present: no D3D11 device for DXGI; GDI as before\n");
  g_dxgi_failed = true;
  return false;
}

static void chain_free(Window* w) {
  if (w->dxgi_wait) CloseHandle(w->dxgi_wait);
  if (w->dxgi_chain) IDXGISwapChain1_Release((IDXGISwapChain1*)w->dxgi_chain);
  w->dxgi_wait = NULL;
  w->dxgi_chain = NULL;
  w->dxgi_w = w->dxgi_h = 0;
}

static void shared_free(void** t) {
  for (int i = 0; i < 2; i++)
    if (t[i]) {
      ID3D11Texture2D_Release((ID3D11Texture2D*)t[i]);
      t[i] = NULL;
    }
}

// After the surface is freed (its render thread has stopped).
void dxgi_pane_gone(Window* w) {
  chain_free(w);
  shared_free(w->shared_tex);
  shared_free(w->shared_old);
}

// Interop: two textures of the frame's size, handed to the renderer (it
// draws frames into them, WGL_NV_DX_interop2); a new size, a new pair, the
// old one kept until a frame comes from the new (the renderer may still
// draw into it until then).
static void shared_sync(Window* w, UINT width, UINT height) {
  const char* v = getenv("GHOSTTY_PRESENT");
  if (!v || strcmp(v, "interop") != 0 || !(g_gl.interop || interop_forced())) return;
  if (w->shared_tex[0] && w->shared_w == width && w->shared_h == height) return;
  if (w->shared_old[0]) return;  // the last switch is not through yet
  D3D11_TEXTURE2D_DESC d = {0};
  d.Width = width;
  d.Height = height;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* t[2] = {NULL, NULL};
  for (int i = 0; i < 2; i++)
    if (ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t[i]) != S_OK) {
      shared_free((void**)t);
      return;
    }
  memcpy(w->shared_old, w->shared_tex, sizeof(w->shared_old));
  w->shared_tex[0] = t[0];
  w->shared_tex[1] = t[1];
  w->shared_w = width;
  w->shared_h = height;
  ghostty_surface_set_shared_textures(w->surface, g_dev, t[0], t[1], width, height);
  char ev[96];
  snprintf(ev, sizeof(ev), "{\"kind\":\"interop\",\"what\":\"textures\",\"size\":[%u,%u]}", width, height);
  test_event(ev);
}

// A frame the renderer drew into shared texture `i`: copied on the GPU
// into the back buffer and presented.
bool dxgi_present_shared(Window* w, unsigned i, UINT width, UINT height) {
  if (i > 1 || !w->shared_tex[i] || !device_ready() || !chain_ready(w, width, height)) return false;
  shared_free(w->shared_old);  // the renderer has moved to the new pair
  uint64_t t0 = qpc();
  if (w->dxgi_wait) WaitForSingleObjectEx(w->dxgi_wait, 100, TRUE);
  uint64_t t1 = qpc();
  ID3D11Texture2D* back = NULL;
  if (IDXGISwapChain1_GetBuffer((IDXGISwapChain1*)w->dxgi_chain, 0, &IID_ID3D11Texture2D, (void**)&back) != S_OK)
    return false;
  ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource*)back, (ID3D11Resource*)w->shared_tex[i]);
  ID3D11Texture2D_Release(back);
  HRESULT hr = IDXGISwapChain1_Present((IDXGISwapChain1*)w->dxgi_chain, 1, 0);
  uint64_t t2 = qpc();
  w->tick_wait_ms += (double)(t1 - t0) * g_qpc_ms;
  w->tick_present_ms += (double)(t2 - t1) * g_qpc_ms;
  w->interop_frames++;
  return SUCCEEDED(hr);
}

static bool chain_ready(Window* w, UINT width, UINT height) {
  IDXGISwapChain1* sc = w->dxgi_chain;
  if (sc && w->dxgi_w == width && w->dxgi_h == height) return true;
  const UINT flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  if (sc) {  // a new size
    if (IDXGISwapChain1_ResizeBuffers(sc, 0, width, height, DXGI_FORMAT_UNKNOWN, flags) != S_OK) {
      chain_free(w);
      return false;
    }
  } else {
    DXGI_SWAP_CHAIN_DESC1 d = {0};
    d.Width = width;
    d.Height = height;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = flags;
    if (IDXGIFactory2_CreateSwapChainForHwnd(g_factory, (IUnknown*)g_dev, w->hwnd, &d, NULL, NULL, &sc) != S_OK)
      return false;
    IDXGIFactory2_MakeWindowAssociation(g_factory, w->hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGISwapChain2* sc2 = NULL;
    if (IDXGISwapChain1_QueryInterface(sc, &IID_IDXGISwapChain2, (void**)&sc2) == S_OK) {
      IDXGISwapChain2_SetMaximumFrameLatency(sc2, 1);
      w->dxgi_wait = IDXGISwapChain2_GetFrameLatencyWaitableObject(sc2);
      IDXGISwapChain2_Release(sc2);
    }
    w->dxgi_chain = sc;
  }
  w->dxgi_w = width;
  w->dxgi_h = height;
  return true;
}

// A frame (BGRA, `stride` bytes a row) into the pane's swap chain. False:
// the pane goes back to GDI (said once).
bool dxgi_present(Window* w, const uint8_t* bgra, UINT width, UINT height, UINT stride) {
  if (!device_ready() || !chain_ready(w, width, height)) goto fail;
  uint64_t t0 = qpc();
  if (w->dxgi_wait) WaitForSingleObjectEx(w->dxgi_wait, 100, TRUE);  // the queue has room
  uint64_t t1 = qpc();
  IDXGISwapChain1* sc = w->dxgi_chain;
  ID3D11Texture2D* back = NULL;
  if (IDXGISwapChain1_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&back) != S_OK) goto fail;
  ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource*)back, 0, NULL, bgra, stride, 0);
  ID3D11Texture2D_Release(back);
  HRESULT hr = IDXGISwapChain1_Present(sc, 1, 0);
  uint64_t t2 = qpc();
  if (FAILED(hr)) goto fail;
  shared_sync(w, width, height);  // interop: textures for the renderer, this size
  w->tick_wait_ms += (double)(t1 - t0) * g_qpc_ms;
  w->tick_present_ms += (double)(t2 - t1) * g_qpc_ms;
  return true;
fail:
  fprintf(stderr, "ghostty: present: DXGI failed on a pane; GDI from now on\n");
  chain_free(w);
  w->dxgi = false;
  return false;
}
