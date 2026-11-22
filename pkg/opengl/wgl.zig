//! Thin WGL bindings: an offscreen OpenGL core context on Windows.
//!
//! Ghostty renders into its own framebuffer objects and hands the
//! finished frames to the apprt, so a context never draws to a window.
//! WGL still needs a device context with a pixel format to create one,
//! so each context owns a hidden 1x1 window that is never shown and
//! never receives input; it only provides the DC.
//!
//! Creating a core profile context takes two steps: a legacy context
//! (wglCreateContext) is made current only to look up
//! wglCreateContextAttribsARB, which then creates the real context.

const std = @import("std");

const log = std.log.scoped(.opengl_wgl);

pub const HWND = *opaque {};
pub const HDC = *opaque {};
pub const HGLRC = *opaque {};
pub const HINSTANCE = *opaque {};
const BOOL = c_int;
const PROC = *const fn () callconv(.c) void;

const WNDCLASSEXW = extern struct {
    cbSize: u32 = @sizeOf(WNDCLASSEXW),
    style: u32,
    lpfnWndProc: *const fn (HWND, u32, usize, isize) callconv(.winapi) isize,
    cbClsExtra: c_int = 0,
    cbWndExtra: c_int = 0,
    hInstance: ?HINSTANCE,
    hIcon: ?*anyopaque = null,
    hCursor: ?*anyopaque = null,
    hbrBackground: ?*anyopaque = null,
    lpszMenuName: ?[*:0]const u16 = null,
    lpszClassName: [*:0]const u16,
    hIconSm: ?*anyopaque = null,
};

const PIXELFORMATDESCRIPTOR = extern struct {
    nSize: u16 = @sizeOf(PIXELFORMATDESCRIPTOR),
    nVersion: u16 = 1,
    dwFlags: u32,
    iPixelType: u8,
    cColorBits: u8,
    cRedBits: u8 = 0,
    cRedShift: u8 = 0,
    cGreenBits: u8 = 0,
    cGreenShift: u8 = 0,
    cBlueBits: u8 = 0,
    cBlueShift: u8 = 0,
    cAlphaBits: u8,
    cAlphaShift: u8 = 0,
    cAccumBits: u8 = 0,
    cAccumRedBits: u8 = 0,
    cAccumGreenBits: u8 = 0,
    cAccumBlueBits: u8 = 0,
    cAccumAlphaBits: u8 = 0,
    cDepthBits: u8 = 0,
    cStencilBits: u8 = 0,
    cAuxBuffers: u8 = 0,
    iLayerType: u8 = 0,
    bReserved: u8 = 0,
    dwLayerMask: u32 = 0,
    dwVisibleMask: u32 = 0,
    dwDamageMask: u32 = 0,
};

const CS_OWNDC = 0x0020;
const WS_OVERLAPPEDWINDOW = 0x00CF0000;
const PFD_DRAW_TO_WINDOW = 0x00000004;
const PFD_SUPPORT_OPENGL = 0x00000020;
const PFD_DOUBLEBUFFER = 0x00000001;
const PFD_TYPE_RGBA = 0;

const WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
const WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
const WGL_CONTEXT_FLAGS_ARB = 0x2094;
const WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
const WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
const WGL_CONTEXT_DEBUG_BIT_ARB = 0x00000001;

const CreateContextAttribsARB = *const fn (
    hdc: HDC,
    share: ?HGLRC,
    attribs: [*]const c_int,
) callconv(.winapi) ?HGLRC;

extern "kernel32" fn GetModuleHandleW(name: ?[*:0]const u16) callconv(.winapi) ?HINSTANCE;
extern "kernel32" fn GetLastError() callconv(.winapi) u32;
extern "kernel32" fn LoadLibraryW(name: [*:0]const u16) callconv(.winapi) ?HINSTANCE;
extern "kernel32" fn GetProcAddress(module: HINSTANCE, name: [*:0]const u8) callconv(.winapi) ?PROC;
extern "user32" fn RegisterClassExW(class: *const WNDCLASSEXW) callconv(.winapi) u16;
extern "user32" fn CreateWindowExW(
    ex_style: u32,
    class_name: [*:0]const u16,
    window_name: [*:0]const u16,
    style: u32,
    x: c_int,
    y: c_int,
    width: c_int,
    height: c_int,
    parent: ?HWND,
    menu: ?*anyopaque,
    instance: ?HINSTANCE,
    param: ?*anyopaque,
) callconv(.winapi) ?HWND;
extern "user32" fn DestroyWindow(hwnd: HWND) callconv(.winapi) BOOL;
extern "user32" fn DefWindowProcW(hwnd: HWND, msg: u32, wparam: usize, lparam: isize) callconv(.winapi) isize;
extern "user32" fn GetDC(hwnd: HWND) callconv(.winapi) ?HDC;
extern "user32" fn ReleaseDC(hwnd: HWND, hdc: HDC) callconv(.winapi) c_int;
extern "gdi32" fn ChoosePixelFormat(hdc: HDC, pfd: *const PIXELFORMATDESCRIPTOR) callconv(.winapi) c_int;
extern "gdi32" fn SetPixelFormat(hdc: HDC, format: c_int, pfd: *const PIXELFORMATDESCRIPTOR) callconv(.winapi) BOOL;
extern "opengl32" fn wglCreateContext(hdc: HDC) callconv(.winapi) ?HGLRC;
extern "opengl32" fn wglDeleteContext(hglrc: HGLRC) callconv(.winapi) BOOL;
extern "opengl32" fn wglMakeCurrent(hdc: ?HDC, hglrc: ?HGLRC) callconv(.winapi) BOOL;
extern "opengl32" fn wglGetProcAddress(name: [*:0]const u8) callconv(.winapi) ?PROC;

pub const Error = error{
    RegisterClassFailed,
    CreateWindowFailed,
    GetDCFailed,
    PixelFormatFailed,
    CreateContextFailed,
    CreateContextAttribsUnsupported,
    MakeCurrentFailed,
};

const class_name = std.unicode.utf8ToUtf16LeStringLiteral("GhosttyWglContext");

/// Set once the window class exists. Registering it twice is harmless:
/// the second call fails with ERROR_CLASS_ALREADY_EXISTS, which counts
/// as success, so threads racing here need no lock.
var registered: std.atomic.Value(bool) = .init(false);
const ERROR_CLASS_ALREADY_EXISTS = 1410;

fn wndProc(hwnd: HWND, msg: u32, wparam: usize, lparam: isize) callconv(.winapi) isize {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

fn registerClass() Error!void {
    if (registered.load(.acquire)) return;
    const class: WNDCLASSEXW = .{
        .style = CS_OWNDC,
        .lpfnWndProc = &wndProc,
        .hInstance = GetModuleHandleW(null),
        .lpszClassName = class_name,
    };
    if (RegisterClassExW(&class) == 0) {
        const err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            log.err("RegisterClassExW failed err={}", .{err});
            return error.RegisterClassFailed;
        }
    }
    registered.store(true, .release);
}

/// An OpenGL context with the hidden window whose DC it uses.
pub const Context = struct {
    hwnd: HWND,
    hdc: HDC,
    hglrc: HGLRC,

    /// Create an OpenGL core profile context of at least `major.minor`.
    /// Nothing is current on the calling thread when this returns.
    pub fn create(major: c_int, minor: c_int, debug: bool) Error!Context {
        try registerClass();

        const hwnd = CreateWindowExW(
            0,
            class_name,
            class_name,
            WS_OVERLAPPEDWINDOW,
            0,
            0,
            1,
            1,
            null,
            null,
            GetModuleHandleW(null),
            null,
        ) orelse {
            log.err("CreateWindowExW failed err={}", .{GetLastError()});
            return error.CreateWindowFailed;
        };
        errdefer _ = DestroyWindow(hwnd);

        // CS_OWNDC: the DC belongs to the window for its whole life,
        // so it is never released while the context uses it.
        const hdc = GetDC(hwnd) orelse return error.GetDCFailed;

        const pfd: PIXELFORMATDESCRIPTOR = .{
            .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
            .iPixelType = PFD_TYPE_RGBA,
            .cColorBits = 32,
            .cAlphaBits = 8,
        };
        const format = ChoosePixelFormat(hdc, &pfd);
        if (format == 0 or SetPixelFormat(hdc, format, &pfd) == 0) {
            log.err("setting the pixel format failed err={}", .{GetLastError()});
            return error.PixelFormatFailed;
        }

        // The legacy context, current only to find the ARB entry point.
        const legacy = wglCreateContext(hdc) orelse {
            log.err("wglCreateContext failed err={}", .{GetLastError()});
            return error.CreateContextFailed;
        };
        defer _ = wglDeleteContext(legacy);
        if (wglMakeCurrent(hdc, legacy) == 0) return error.MakeCurrentFailed;
        defer _ = wglMakeCurrent(null, null);

        const create_attribs: CreateContextAttribsARB = @ptrCast(
            wglGetProcAddress("wglCreateContextAttribsARB") orelse
                return error.CreateContextAttribsUnsupported,
        );

        const attribs = [_]c_int{
            WGL_CONTEXT_MAJOR_VERSION_ARB, major,
            WGL_CONTEXT_MINOR_VERSION_ARB, minor,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            WGL_CONTEXT_FLAGS_ARB,         if (debug) WGL_CONTEXT_DEBUG_BIT_ARB else 0,
            0,
        };
        const hglrc = create_attribs(hdc, null, &attribs) orelse {
            log.err(
                "wglCreateContextAttribsARB failed for OpenGL {}.{} core err=0x{x}",
                .{ major, minor, GetLastError() },
            );
            return error.CreateContextFailed;
        };

        return .{ .hwnd = hwnd, .hdc = hdc, .hglrc = hglrc };
    }

    pub fn destroy(self: Context) void {
        _ = wglDeleteContext(self.hglrc);
        _ = ReleaseDC(self.hwnd, self.hdc);
        _ = DestroyWindow(self.hwnd);
    }

    /// Make this context current on the calling thread.
    pub fn makeCurrent(self: Context) Error!void {
        if (wglMakeCurrent(self.hdc, self.hglrc) == 0) {
            log.err("wglMakeCurrent failed err={}", .{GetLastError()});
            return error.MakeCurrentFailed;
        }
    }
};

/// Make no context current on the calling thread.
pub fn releaseCurrent() void {
    _ = wglMakeCurrent(null, null);
}

var opengl32: ?HINSTANCE = null;

/// The GL function loader for GLAD. wglGetProcAddress only knows the
/// functions beyond OpenGL 1.1 and returns null (or, on some drivers,
/// a small integer) for the rest, which opengl32.dll exports itself.
pub fn getProcAddress(name: [*c]const u8) callconv(.c) ?PROC {
    const n: [*:0]const u8 = @ptrCast(name);
    if (wglGetProcAddress(n)) |p| {
        switch (@intFromPtr(p)) {
            1, 2, 3, std.math.maxInt(usize) => {},
            else => return p,
        }
    }
    const lib = opengl32 orelse lib: {
        const l = LoadLibraryW(std.unicode.utf8ToUtf16LeStringLiteral("opengl32.dll")) orelse
            return null;
        opengl32 = l;
        break :lib l;
    };
    return GetProcAddress(lib, n);
}
