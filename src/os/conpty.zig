//! Which ConPTY Ghostty uses on Windows: Microsoft's newer one, side-loaded from the
//! conpty.dll beside ghostty.exe (which starts the OpenConsole.exe beside it), else the
//! in-box one in kernel32.
//!
//! Why: the in-box console of current Windows (conhost 10.0.26100, also on Windows 11
//! build 26300) re-renders what a program writes and drops what it doesn't model: mouse
//! modes, bracketed paste, focus events, the kitty keyboard protocol, cursor shapes,
//! kitty graphics and sixel, the title, OSC 52, OSC 7. The side-loaded ConPTY (Windows
//! Terminal 1.22+, NuGet Microsoft.Windows.Console.ConPTY) passes VT through unchanged:
//! measured 24 of 25 sequences against the in-box 4 of 25.
//!
//! GHOSTTY_CONPTY=inbox turns the side-load off.
const std = @import("std");
const builtin = @import("builtin");
const windows = @import("windows.zig");
const global = @import("../global.zig");

const log = std.log.scoped(.conpty);

const HRESULT = windows.HRESULT;
const HANDLE = windows.HANDLE;
const HPCON = windows.HPCON;
const COORD = windows.COORD;
const DWORD = windows.DWORD;

/// PSEUDOCONSOLE_GLYPH_WIDTH_GRAPHEMES from conpty.h: measure text in grapheme clusters, as
/// Ghostty does. Only the side-loaded ConPTY knows it.
const GLYPH_WIDTH_GRAPHEMES: DWORD = 0x08;

pub const Impl = struct {
    create: *const fn (COORD, HANDLE, HANDLE, DWORD, *HPCON) callconv(.winapi) HRESULT,
    resize: *const fn (HPCON, COORD) callconv(.winapi) HRESULT,
    close: *const fn (HPCON) callconv(.winapi) void,
    /// Flags this implementation is created with.
    flags: DWORD,
    side_loaded: bool,
};

pub const inbox: Impl = .{
    .create = &windows.exp.kernel32.CreatePseudoConsole,
    .resize = &windows.exp.kernel32.ResizePseudoConsole,
    .close = &windows.exp.kernel32.ClosePseudoConsole,
    .flags = 0,
    .side_loaded = false,
};

var side_loaded: Impl = undefined;
var chosen: ?*const Impl = null;
var mutex: std.Io.Mutex = .init;

/// The ConPTY to create the next pseudoconsole with, chosen once per process.
pub fn get() *const Impl {
    mutex.lockUncancelable(global.io());
    defer mutex.unlock(global.io());
    if (chosen) |c| return c;
    chosen = choose();
    return chosen.?;
}

/// Called when the side-loaded ConPTY failed to create a pseudoconsole (Wine's console
/// driver lacks what OpenConsole.exe needs, for one): use the in-box one from now on.
pub fn fallBack(hr: HRESULT) *const Impl {
    mutex.lockUncancelable(global.io());
    defer mutex.unlock(global.io());
    log.warn("side-loaded ConPTY failed to create a pseudoconsole (HRESULT 0x{x:0>8}), using the in-box one", .{@as(u32, @bitCast(hr))});
    chosen = &inbox;
    return &inbox;
}

fn choose() *const Impl {
    if (global.environ().getWindows(std.unicode.utf8ToUtf16LeStringLiteral("GHOSTTY_CONPTY"))) |v| {
        if (std.mem.eql(u16, v, std.unicode.utf8ToUtf16LeStringLiteral("inbox"))) {
            log.info("using the in-box ConPTY (GHOSTTY_CONPTY=inbox)", .{});
            return &inbox;
        }
    }

    // The folder ghostty.exe is in, as a wide string.
    var exe: [windows.MAX_PATH * 2:0]u16 = undefined;
    const n = GetModuleFileNameW(null, &exe, exe.len);
    if (n == 0 or n >= exe.len) return noSideLoad("can't get ghostty.exe's path");
    const sep = std.mem.lastIndexOfScalar(u16, exe[0..n], '\\') orelse return noSideLoad("ghostty.exe's path has no folder");
    const dir = exe[0 .. sep + 1];

    var dll_buf: [windows.MAX_PATH * 2 + 32:0]u16 = undefined;
    const dll = join(&dll_buf, dir, "conpty.dll") orelse return noSideLoad("path too long");
    var oc_buf: [windows.MAX_PATH * 2 + 32:0]u16 = undefined;
    const open_console = join(&oc_buf, dir, "OpenConsole.exe") orelse return noSideLoad("path too long");

    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES)
        return noSideLoad("no conpty.dll beside ghostty.exe");
    // Without it conpty.dll quietly starts the in-box console instead; say so.
    if (GetFileAttributesW(open_console) == INVALID_FILE_ATTRIBUTES)
        return noSideLoad("conpty.dll is there but OpenConsole.exe is not");

    // By full path, and its own folder for its dependencies: never a DLL search.
    const module = LoadLibraryExW(dll, null, LOAD_WITH_ALTERED_SEARCH_PATH) orelse {
        log.warn("can't load conpty.dll (error {d}), using the in-box ConPTY", .{@backingInt(windows.GetLastError())});
        return &inbox;
    };
    const create = GetProcAddress(module, "ConptyCreatePseudoConsole");
    const resize = GetProcAddress(module, "ConptyResizePseudoConsole");
    const close = GetProcAddress(module, "ConptyClosePseudoConsole");
    if (create == null or resize == null or close == null) {
        log.warn("conpty.dll lacks the Conpty* functions, using the in-box ConPTY", .{});
        return &inbox;
    }
    side_loaded = .{
        .create = @ptrCast(create.?),
        .resize = @ptrCast(resize.?),
        .close = @ptrCast(close.?),
        .flags = GLYPH_WIDTH_GRAPHEMES,
        .side_loaded = true,
    };

    var path_utf8: [windows.MAX_PATH * 6]u8 = undefined;
    const path_len = std.unicode.wtf16LeToWtf8(&path_utf8, dll);
    var ver_buf: [32]u8 = undefined;
    log.info("using the side-loaded ConPTY {s} (file version {s})", .{ path_utf8[0..path_len], fileVersion(dll, &ver_buf) });
    return &side_loaded;
}

fn noSideLoad(why: []const u8) *const Impl {
    log.info("using the in-box ConPTY ({s})", .{why});
    return &inbox;
}

fn join(buf: [:0]u16, dir: []const u16, comptime name: []const u8) ?[:0]const u16 {
    const w = std.unicode.utf8ToUtf16LeStringLiteral(name);
    if (dir.len + w.len >= buf.len) return null;
    @memcpy(buf[0..dir.len], dir);
    @memcpy(buf[dir.len..][0..w.len], w);
    buf[dir.len + w.len] = 0;
    return buf[0 .. dir.len + w.len :0];
}

/// "a.b.c.d" from the file's VS_FIXEDFILEINFO, or "unknown". version.dll is loaded from
/// System32 on demand, so ghostty.exe needs no import of it.
fn fileVersion(path: [:0]const u16, buf: []u8) []const u8 {
    const unknown = "unknown";
    const version = LoadLibraryExW(std.unicode.utf8ToUtf16LeStringLiteral("version.dll"), null, LOAD_LIBRARY_SEARCH_SYSTEM32) orelse return unknown;
    defer _ = FreeLibrary(version);
    const size_fn: *const fn ([*:0]const u16, ?*DWORD) callconv(.winapi) DWORD = @ptrCast(GetProcAddress(version, "GetFileVersionInfoSizeW") orelse return unknown);
    const info_fn: *const fn ([*:0]const u16, DWORD, DWORD, *anyopaque) callconv(.winapi) windows.BOOL = @ptrCast(GetProcAddress(version, "GetFileVersionInfoW") orelse return unknown);
    const query_fn: *const fn (*const anyopaque, [*:0]const u16, *?*anyopaque, *u32) callconv(.winapi) windows.BOOL = @ptrCast(GetProcAddress(version, "VerQueryValueW") orelse return unknown);

    var data: [8192]u8 align(8) = undefined;
    const size = size_fn(path, null);
    if (size == 0 or size > data.len) return unknown;
    if (info_fn(path, 0, size, &data) == windows.FALSE) return unknown;
    var fixed: ?*anyopaque = null;
    var len: u32 = 0;
    if (query_fn(&data, std.unicode.utf8ToUtf16LeStringLiteral("\\"), &fixed, &len) == windows.FALSE or fixed == null or len < 16) return unknown;
    // VS_FIXEDFILEINFO: dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS, ...
    const words: [*]align(1) const u32 = @ptrCast(fixed.?);
    const ms = words[2];
    const ls = words[3];
    return std.mem.print(buf, "{d}.{d}.{d}.{d}", .{ ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff }) catch unknown;
}

const HMODULE = *opaque {};
const INVALID_FILE_ATTRIBUTES: DWORD = 0xffffffff;
const LOAD_WITH_ALTERED_SEARCH_PATH: DWORD = 0x00000008;
const LOAD_LIBRARY_SEARCH_SYSTEM32: DWORD = 0x00000800;
extern "kernel32" fn GetModuleFileNameW(module: ?HMODULE, name: [*]u16, size: DWORD) callconv(.winapi) DWORD;
extern "kernel32" fn GetFileAttributesW(name: [*:0]const u16) callconv(.winapi) DWORD;
extern "kernel32" fn LoadLibraryExW(name: [*:0]const u16, file: ?HANDLE, flags: DWORD) callconv(.winapi) ?HMODULE;
extern "kernel32" fn FreeLibrary(module: HMODULE) callconv(.winapi) windows.BOOL;
/// What GetProcAddress returns, as Windows declares it (FARPROC): a function pointer, so a
/// @ptrCast to the real function type keeps its alignment. From ?*anyopaque (alignment 1) the
/// cast would raise it to a function's, 4 on aarch64, which Zig rejects there.
const FARPROC = *const fn () callconv(.winapi) isize;
extern "kernel32" fn GetProcAddress(module: HMODULE, name: [*:0]const u8) callconv(.winapi) ?FARPROC;
