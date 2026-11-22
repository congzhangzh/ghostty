//! The default shell on Windows, when the config sets no `command`:
//! PowerShell 7 (pwsh.exe) when it is installed, else Windows PowerShell
//! (powershell.exe), else cmd.exe.
//!
//! `choose` holds the order and is plain Zig, so it is tested on any host;
//! `probe` looks at the system and only runs on Windows.
const std = @import("std");
const builtin = @import("builtin");
const Allocator = std.mem.Allocator;
const path = @import("path.zig");

const log = std.log.scoped(.default_shell);

/// What `probe` found on this system: the full path of each candidate that
/// exists, null for one that doesn't.
pub const Found = struct {
    /// pwsh.exe on PATH. This covers the Microsoft Store install, whose
    /// app execution alias is in %LOCALAPPDATA%\Microsoft\WindowsApps.
    pwsh_on_path: ?[:0]const u8 = null,

    /// %ProgramFiles%\PowerShell\7\pwsh.exe, the MSI and winget install.
    pwsh_installed: ?[:0]const u8 = null,

    /// %SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe.
    windows_powershell: ?[:0]const u8 = null,

    /// Running under Wine: see `underWine`.
    wine: bool = false,
};

pub const Choice = struct {
    /// The full path of the shell to run, or null for cmd.exe, which the
    /// caller runs as the `cmd.exe` shell command (resolved via %COMSPEC%).
    path: ?[:0]const u8,

    /// Why this one, for the log.
    why: []const u8,
};

/// The order. Pure: everything it decides on is in `found`.
pub fn choose(found: Found) Choice {
    if (found.pwsh_on_path) |p| return .{ .path = p, .why = "PowerShell 7, pwsh.exe on PATH" };
    if (found.pwsh_installed) |p| return .{ .path = p, .why = "PowerShell 7, in its install folder" };
    if (found.windows_powershell) |p| {
        if (found.wine) return .{
            .path = null,
            .why = "no PowerShell 7; running under Wine, whose powershell.exe is a stub",
        };
        return .{ .path = p, .why = "Windows PowerShell; no PowerShell 7 found" };
    }
    return .{ .path = null, .why = "no PowerShell found" };
}

/// Look for the candidates on this system. Windows only. Paths are
/// allocated with `alloc` (an arena in the caller). Errors from looking (a
/// PATH entry that can't be read, an alias that can't be opened) count as
/// "not found": the default shell is a best effort, cmd.exe always works.
pub fn probe(
    io: std.Io,
    alloc: Allocator,
    environ_map: *const std.process.Environ.Map,
) Allocator.Error!Found {
    comptime std.debug.assert(builtin.target.os.tag == .windows);
    var found: Found = .{ .wine = underWine() };

    if (path.expand(io, alloc, environ_map, "pwsh.exe")) |p| {
        if (p) |v| found.pwsh_on_path = try alloc.dupeSentinel(u8, v, 0);
    } else |err| switch (err) {
        error.OutOfMemory => return error.OutOfMemory,
        else => log.info("looking for pwsh.exe on PATH failed: {}", .{err}),
    }

    if (environ_map.get("ProgramFiles")) |pf|
        found.pwsh_installed = try existing(io, alloc, &.{ pf, "PowerShell", "7", "pwsh.exe" });
    if (environ_map.get("SystemRoot")) |root|
        found.windows_powershell = try existing(io, alloc, &.{ root, "System32", "WindowsPowerShell", "v1.0", "powershell.exe" });

    return found;
}

/// The joined path if that file exists, else null.
fn existing(io: std.Io, alloc: Allocator, parts: []const []const u8) Allocator.Error!?[:0]const u8 {
    const p = try std.fs.path.joinZ(alloc, parts);
    std.Io.Dir.accessAbsolute(io, p, .{}) catch return null;
    return p;
}

/// True when this process runs under Wine: Wine's ntdll exports
/// wine_get_version, Windows' doesn't (the check Wine's own FAQ gives).
///
/// It exists because Wine's powershell.exe (Wine 11) is a stub that exits
/// at once, so a terminal that picked it would close as soon as it opened.
/// When Wine ships a working powershell.exe, delete this function and the
/// `wine` field, and Wine gets the same order as Windows.
fn underWine() bool {
    const ntdll = GetModuleHandleW(std.unicode.utf8ToUtf16LeStringLiteral("ntdll.dll")) orelse return false;
    return GetProcAddress(ntdll, "wine_get_version") != null;
}

const HMODULE = *opaque {};
extern "kernel32" fn GetModuleHandleW(name: ?[*:0]const u16) callconv(.winapi) ?HMODULE;
extern "kernel32" fn GetProcAddress(module: HMODULE, name: [*:0]const u8) callconv(.winapi) ?*anyopaque;

test "choose: PowerShell 7 on PATH first" {
    const c = choose(.{
        .pwsh_on_path = "C:\\Users\\a\\AppData\\Local\\Microsoft\\WindowsApps\\pwsh.exe",
        .pwsh_installed = "C:\\Program Files\\PowerShell\\7\\pwsh.exe",
        .windows_powershell = "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
    });
    try std.testing.expectEqualStrings("C:\\Users\\a\\AppData\\Local\\Microsoft\\WindowsApps\\pwsh.exe", c.path.?);
}

test "choose: PowerShell 7 in its install folder, then Windows PowerShell" {
    const pf = "C:\\Program Files\\PowerShell\\7\\pwsh.exe";
    const wp = "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    try std.testing.expectEqualStrings(pf, choose(.{ .pwsh_installed = pf, .windows_powershell = wp }).path.?);
    try std.testing.expectEqualStrings(wp, choose(.{ .windows_powershell = wp }).path.?);
}

test "choose: cmd.exe when there is no PowerShell" {
    try std.testing.expect(choose(.{}).path == null);
}

test "choose: under Wine, PowerShell 7 still wins but not Wine's powershell.exe" {
    const pf = "C:\\Program Files\\PowerShell\\7\\pwsh.exe";
    const wp = "C:\\windows\\system32\\WindowsPowerShell\\v1.0\\powershell.exe";
    try std.testing.expectEqualStrings(pf, choose(.{ .pwsh_installed = pf, .windows_powershell = wp, .wine = true }).path.?);
    try std.testing.expect(choose(.{ .windows_powershell = wp, .wine = true }).path == null);
}
