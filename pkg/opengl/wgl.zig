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
//!
//! Which opengl32.dll: the system's (System32, which hands a context to the
//! GPU driver's ICD) first. A machine without a GPU driver (a VM's basic
//! display adapter, as on UTM; some RDP sessions) gets only Microsoft's
//! GDI generic OpenGL 1.1 there, and the 4.3 core context fails. Then
//! Mesa's opengl32.dll shipped in the package's opengl\ folder (software
//! rendering) is loaded by its full path and the context made again; from
//! then on every context of the process uses it. GHOSTTY_GL=system or
//! GHOSTTY_GL=mesa forces one. Every WGL call goes through `Lib`, the
//! functions of the chosen DLL: with Mesa's, also the pixel format and
//! swap calls, which gdi32 would hand to whichever opengl32.dll it loads
//! by name.

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
extern "gdi32" fn DescribePixelFormat(hdc: HDC, format: c_int, size: u32, pfd: *PIXELFORMATDESCRIPTOR) callconv(.winapi) c_int;
extern "gdi32" fn SwapBuffers(hdc: HDC) callconv(.winapi) BOOL;
extern "gdi32" fn GetPixelFormat(hdc: HDC) callconv(.winapi) c_int;
extern "kernel32" fn LoadLibraryExW(name: [*:0]const u16, file: ?*anyopaque, flags: u32) callconv(.winapi) ?HINSTANCE;
extern "kernel32" fn GetSystemDirectoryW(buf: [*]u16, size: u32) callconv(.winapi) u32;
extern "kernel32" fn GetModuleFileNameW(module: ?HINSTANCE, buf: [*]u16, size: u32) callconv(.winapi) u32;
extern "kernel32" fn GetFileAttributesW(name: [*:0]const u16) callconv(.winapi) u32;
extern "kernel32" fn GetEnvironmentVariableW(name: [*:0]const u16, buf: [*]u16, size: u32) callconv(.winapi) u32;

const LOAD_WITH_ALTERED_SEARCH_PATH = 0x00000008;
/// Room for a path, in UTF-16 units (beyond MAX_PATH; small for a thread's stack).
const path_len = 4096;
const INVALID_FILE_ATTRIBUTES = 0xFFFFFFFF;

/// The WGL functions of one opengl32.dll.
pub const Lib = struct {
    pub const Kind = enum { system, mesa };

    kind: Kind,
    module: HINSTANCE,
    createContext: *const fn (HDC) callconv(.winapi) ?HGLRC,
    deleteContext: *const fn (HGLRC) callconv(.winapi) BOOL,
    makeCurrent: *const fn (?HDC, ?HGLRC) callconv(.winapi) BOOL,
    getProcAddress: *const fn ([*:0]const u8) callconv(.winapi) ?PROC,
    choosePixelFormat: *const fn (HDC, *const PIXELFORMATDESCRIPTOR) callconv(.winapi) c_int,
    setPixelFormat: *const fn (HDC, c_int, *const PIXELFORMATDESCRIPTOR) callconv(.winapi) BOOL,
    describePixelFormat: *const fn (HDC, c_int, u32, *PIXELFORMATDESCRIPTOR) callconv(.winapi) c_int,
    getPixelFormat: *const fn (HDC) callconv(.winapi) c_int,
    swapBuffers: *const fn (HDC) callconv(.winapi) BOOL,

    fn sym(module: HINSTANCE, comptime T: type, name: [*:0]const u8) ?T {
        const p = GetProcAddress(module, name) orelse return null;
        return @ptrCast(p);
    }

    /// Load the DLL at `path` (a full path: never a search) and take its
    /// functions. The system's keeps gdi32's pixel format and swap calls,
    /// which hand them to its ICD; Mesa's gives its own.
    fn load(kind: Kind, path: [*:0]const u16) ?Lib {
        const m = LoadLibraryExW(path, null, LOAD_WITH_ALTERED_SEARCH_PATH) orelse {
            log.warn("loading {s} opengl32.dll {f} failed err={}", .{ @tagName(kind), std.unicode.fmtUtf16Le(std.mem.span(path)), GetLastError() });
            return null;
        };
        var lib: Lib = .{
            .kind = kind,
            .module = m,
            .createContext = sym(m, *const fn (HDC) callconv(.winapi) ?HGLRC, "wglCreateContext") orelse return null,
            .deleteContext = sym(m, *const fn (HGLRC) callconv(.winapi) BOOL, "wglDeleteContext") orelse return null,
            .makeCurrent = sym(m, *const fn (?HDC, ?HGLRC) callconv(.winapi) BOOL, "wglMakeCurrent") orelse return null,
            .getProcAddress = sym(m, *const fn ([*:0]const u8) callconv(.winapi) ?PROC, "wglGetProcAddress") orelse return null,
            .choosePixelFormat = &ChoosePixelFormat,
            .setPixelFormat = &SetPixelFormat,
            .describePixelFormat = &DescribePixelFormat,
            .getPixelFormat = &GetPixelFormat,
            .swapBuffers = &SwapBuffers,
        };
        if (kind == .mesa) {
            lib.choosePixelFormat = sym(m, @TypeOf(lib.choosePixelFormat), "wglChoosePixelFormat") orelse return null;
            lib.setPixelFormat = sym(m, @TypeOf(lib.setPixelFormat), "wglSetPixelFormat") orelse return null;
            lib.describePixelFormat = sym(m, @TypeOf(lib.describePixelFormat), "wglDescribePixelFormat") orelse return null;
            lib.getPixelFormat = sym(m, @TypeOf(lib.getPixelFormat), "wglGetPixelFormat") orelse return null;
            lib.swapBuffers = sym(m, @TypeOf(lib.swapBuffers), "wglSwapBuffers") orelse return null;
        }
        return lib;
    }
};

/// What GHOSTTY_GL asks for: auto (the system's, then Mesa's), or one.
const Choice = enum { auto, system, mesa };

fn choiceFromEnv() Choice {
    var buf: [16]u16 = undefined;
    const n = GetEnvironmentVariableW(std.unicode.utf8ToUtf16LeStringLiteral("GHOSTTY_GL"), &buf, buf.len);
    if (n == 0 or n >= buf.len) return .auto;
    var ascii: [16]u8 = undefined;
    for (buf[0..n], 0..) |c, i| ascii[i] = if (c < 128) std.ascii.toLower(@intCast(c)) else '?';
    const v = ascii[0..n];
    if (std.mem.eql(u8, v, "system")) return .system;
    if (std.mem.eql(u8, v, "mesa")) return .mesa;
    log.warn("GHOSTTY_GL={s} is not system or mesa: choosing by itself", .{v});
    return .auto;
}

/// The system's opengl32.dll, by its full path in System32.
fn systemPath(buf: *[path_len]u16) ?[*:0]const u16 {
    const tail = std.unicode.utf8ToUtf16LeStringLiteral("\\opengl32.dll");
    const n = GetSystemDirectoryW(buf, buf.len - tail.len - 1);
    if (n == 0 or n + tail.len + 1 > buf.len) return null;
    @memcpy(buf[n..][0..tail.len], tail);
    buf[n + tail.len] = 0;
    return @ptrCast(buf);
}

/// Mesa's opengl32.dll in the package: opengl\opengl32.dll beside ghostty.exe.
/// Never beside the exe itself, where it would win every load by name.
fn mesaPath(buf: *[path_len]u16) ?[*:0]const u16 {
    const tail = std.unicode.utf8ToUtf16LeStringLiteral("opengl\\opengl32.dll");
    const n = GetModuleFileNameW(null, buf, buf.len);
    if (n == 0 or n >= buf.len) return null;
    var dir = n;
    while (dir > 0 and buf[dir - 1] != '\\' and buf[dir - 1] != '/') dir -= 1;
    if (dir + tail.len + 1 > buf.len) return null;
    @memcpy(buf[dir..][0..tail.len], tail);
    buf[dir + tail.len] = 0;
    const p: [*:0]const u16 = @ptrCast(buf);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return null;
    return p;
}

/// Guards the choice below. A spin lock: contexts are made rarely (one per
/// surface), and this package has no std.Io to lock a std.Io.Mutex with.
var lib_lock: std.atomic.Value(bool) = .init(false);
fn lock() void {
    while (lib_lock.cmpxchgWeak(false, true, .acquire, .monotonic) != null) std.atomic.spinLoopHint();
}
fn unlock() void {
    lib_lock.store(false, .release);
}
/// The DLL every context of the process uses, once one worked.
var chosen: ?Lib = null;
var system_lib: ?Lib = null;
var mesa_lib: ?Lib = null;
var tried_system = false;
var tried_mesa = false;

fn getSystem() ?Lib {
    if (!tried_system) {
        tried_system = true;
        var buf: [path_len]u16 = undefined;
        if (systemPath(&buf)) |p| system_lib = Lib.load(.system, p);
    }
    return system_lib;
}

fn getMesa() ?Lib {
    if (!tried_mesa) {
        tried_mesa = true;
        var buf: [path_len]u16 = undefined;
        if (mesaPath(&buf)) |p| {
            mesa_lib = Lib.load(.mesa, p);
            if (mesa_lib != null) log.info("Mesa's OpenGL is in the package: {f}", .{std.unicode.fmtUtf16Le(std.mem.span(p))});
        } else log.info("no Mesa OpenGL in the package (opengl\\opengl32.dll beside ghostty.exe)", .{});
    }
    return mesa_lib;
}

/// The DLL in use: the one chosen, or (before any context was made) the
/// one that will be tried first.
fn current() ?Lib {
    if (chosen) |l| return l;
    return switch (choiceFromEnv()) {
        .mesa => getMesa(),
        else => getSystem(),
    };
}

/// Which OpenGL the process renders with, for the host's /stats and
/// /buildinfo: "system", "mesa", or "none" before a context was made.
pub fn driverName() [*:0]const u8 {
    const l = chosen orelse return "none";
    return switch (l.kind) {
        .system => "system",
        .mesa => "mesa",
    };
}

export fn ghostty_windows_gl_driver() [*:0]const u8 {
    return driverName();
}

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

/// A window the context draws into and swaps (GHOSTTY_PRESENT=swap): its
/// DC, set to the context's pixel format.
pub const Window = struct {
    hwnd: HWND,
    hdc: HDC,

    /// Show what was drawn into the window's back buffer.
    pub fn swap(self: Window) bool {
        return chosen.?.swapBuffers(self.hdc) != 0;
    }
};

/// An OpenGL context with the hidden window whose DC it uses.
pub const Context = struct {
    hwnd: HWND,
    hdc: HDC,
    hglrc: HGLRC,
    format: c_int,

    /// Create an OpenGL core profile context of at least `major.minor`.
    /// Nothing is current on the calling thread when this returns.
    /// The first one chooses the opengl32.dll of the process (see the top
    /// of this file); later ones use it.
    pub fn create(major: c_int, minor: c_int, debug: bool) Error!Context {
        try registerClass();
        lock();
        defer unlock();
        if (chosen) |l| return createWith(l, major, minor, debug);

        const choice = choiceFromEnv();
        if (choice != .mesa) {
            if (getSystem()) |l| {
                if (createWith(l, major, minor, debug)) |c| {
                    chosen = l;
                    log.info("OpenGL: the system's opengl32.dll{s}", .{if (choice == .system) " (GHOSTTY_GL=system)" else ""});
                    return c;
                } else |err| {
                    if (choice == .system) return err;
                    log.warn("the system's OpenGL gives no {}.{} core context ({}): trying Mesa's from the package", .{ major, minor, err });
                }
            } else if (choice == .system) return error.CreateContextFailed;
        }
        const l = getMesa() orelse {
            log.err("no OpenGL {}.{} core context: the system's has none, and the package has no Mesa opengl\\opengl32.dll", .{ major, minor });
            return error.CreateContextFailed;
        };
        const c = try createWith(l, major, minor, debug);
        chosen = l;
        log.info("OpenGL: Mesa's opengl32.dll from the package{s}", .{if (choice == .mesa) " (GHOSTTY_GL=mesa)" else " (software rendering: the system's OpenGL is too old)"});
        return c;
    }

    fn createWith(l: Lib, major: c_int, minor: c_int, debug: bool) Error!Context {

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
        const format = l.choosePixelFormat(hdc, &pfd);
        if (format == 0 or l.setPixelFormat(hdc, format, &pfd) == 0) {
            log.err("setting the pixel format failed err={}", .{GetLastError()});
            return error.PixelFormatFailed;
        }

        // The legacy context, current only to find the ARB entry point.
        const legacy = l.createContext(hdc) orelse {
            log.err("wglCreateContext failed err={}", .{GetLastError()});
            return error.CreateContextFailed;
        };
        defer _ = l.deleteContext(legacy);
        if (l.makeCurrent(hdc, legacy) == 0) return error.MakeCurrentFailed;
        defer _ = l.makeCurrent(null, null);

        const create_attribs: CreateContextAttribsARB = @ptrCast(
            l.getProcAddress("wglCreateContextAttribsARB") orelse
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

        return .{ .hwnd = hwnd, .hdc = hdc, .hglrc = hglrc, .format = format };
    }

    pub fn destroy(self: Context) void {
        _ = chosen.?.deleteContext(self.hglrc);
        _ = ReleaseDC(self.hwnd, self.hdc);
        _ = DestroyWindow(self.hwnd);
    }

    /// Draw into `hwnd` itself from now on: its DC gets this context's
    /// pixel format (a window takes one once, for its life; the host's
    /// window class has CS_OWNDC, so the DC stays the window's), and the
    /// context is made current on it, on the calling thread.
    pub fn attachWindow(self: Context, hwnd: HWND) Error!Window {
        const hdc = GetDC(hwnd) orelse return error.GetDCFailed;
        errdefer _ = ReleaseDC(hwnd, hdc);
        var pfd: PIXELFORMATDESCRIPTOR = .{
            .dwFlags = 0,
            .iPixelType = 0,
            .cColorBits = 0,
            .cAlphaBits = 0,
        };
        const l = chosen.?;
        // A window takes a pixel format once: one that has this context's
        // already keeps it (Mesa refuses a second SetPixelFormat even with
        // the same format; gdi32 allows that one).
        if (l.getPixelFormat(hdc) != self.format and
            (l.describePixelFormat(hdc, self.format, @sizeOf(PIXELFORMATDESCRIPTOR), &pfd) == 0 or
                l.setPixelFormat(hdc, self.format, &pfd) == 0))
        {
            log.err("setting the window's pixel format failed err={}", .{GetLastError()});
            return error.PixelFormatFailed;
        }
        if (l.makeCurrent(hdc, self.hglrc) == 0) {
            log.err("wglMakeCurrent on the window failed err={}", .{GetLastError()});
            return error.MakeCurrentFailed;
        }
        return .{ .hwnd = hwnd, .hdc = hdc };
    }

    /// Make this context current on `window`'s DC again (after the window
    /// changed size: Wine recreates a GL child window's drawable then).
    pub fn rebind(self: Context, window: Window) Error!void {
        if (chosen.?.makeCurrent(window.hdc, self.hglrc) == 0) {
            log.err("wglMakeCurrent on the window failed err={}", .{GetLastError()});
            return error.MakeCurrentFailed;
        }
    }

    /// Make this context current on the calling thread.
    pub fn makeCurrent(self: Context) Error!void {
        if (chosen.?.makeCurrent(self.hdc, self.hglrc) == 0) {
            log.err("wglMakeCurrent failed err={}", .{GetLastError()});
            return error.MakeCurrentFailed;
        }
    }
};

/// Make no context current on the calling thread.
pub fn releaseCurrent() void {
    if (chosen) |l| _ = l.makeCurrent(null, null);
}

/// The GL function loader for GLAD. wglGetProcAddress only knows the
/// functions beyond OpenGL 1.1 and returns null (or, on some drivers,
/// a small integer) for the rest, which the chosen opengl32.dll exports
/// itself.
pub fn getProcAddress(name: [*c]const u8) callconv(.c) ?PROC {
    const n: [*:0]const u8 = @ptrCast(name);
    const l = current() orelse return null;
    if (l.getProcAddress(n)) |p| {
        switch (@intFromPtr(p)) {
            1, 2, 3, std.math.maxInt(usize) => {},
            else => return p,
        }
    }
    return GetProcAddress(l.module, n);
}
