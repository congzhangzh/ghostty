//! Graphics API wrapper for OpenGL.
pub const OpenGL = @This();

const std = @import("std");
const builtin = @import("builtin");
const Allocator = std.mem.Allocator;
const global = @import("../global.zig");
const gl = @import("opengl");
const egl = gl.egl;
const shadertoy = @import("shadertoy.zig");
const apprt = @import("../apprt.zig");
const font = @import("../font/main.zig");
const configpkg = @import("../config.zig");
const rendererpkg = @import("../renderer.zig");
const Renderer = rendererpkg.GenericRenderer(OpenGL);
const Dmabuf = @import("Dmabuf.zig");
pub const Device = @import("opengl/Device.zig");

pub const GraphicsAPI = OpenGL;
pub const Target = @import("opengl/Target.zig");
pub const Frame = @import("opengl/Frame.zig");
pub const RenderPass = @import("opengl/RenderPass.zig");
pub const Pipeline = @import("opengl/Pipeline.zig");
const bufferpkg = @import("opengl/buffer.zig");
pub const Buffer = bufferpkg.Buffer;
pub const Sampler = @import("opengl/Sampler.zig");
pub const Texture = @import("opengl/Texture.zig");
pub const shaders = @import("opengl/shaders.zig");

pub const custom_shader_target: shadertoy.Target = .glsl;
// The fragCoord for OpenGL shaders is +Y = up.
pub const custom_shader_y_is_down = false;

/// Triple-buffering gives the GPU room to pipeline renders without
/// having to wait on the apprt consuming previous frames.
pub const swap_chain_count = 3;

const log = std.log.scoped(.opengl);

/// We require at least OpenGL 4.3
pub const MIN_VERSION_MAJOR = 4;
pub const MIN_VERSION_MINOR = 3;

alloc: std.mem.Allocator,

/// Alpha blending mode
blending: configpkg.Config.AlphaBlending,

/// The shared render device.
device: *const Device,

/// This surface's OpenGL context: an EGL context, or on Windows a WGL
/// context with the hidden window that provides its DC.
context: Context,

/// Windows, GHOSTTY_PRESENT=swap: the surface's own window, which the
/// context draws into and swaps, so a frame never comes back through the
/// CPU (no glReadPixels, no copies in the host). Null: frames are read
/// back and the host presents them (the default).
swap_window: SwapWindow = if (Device.api == .wgl) null else {},

/// The size of the last frame swapped into `swap_window`: a new size
/// rebinds the context to the window first.
swap_size: if (Device.api == .wgl) [2]usize else void = if (Device.api == .wgl) .{ 0, 0 } else {},

const SwapWindow = if (Device.api == .wgl) ?gl.wgl.Window else void;

/// GHOSTTY_PRESENT=swap asks for the swap present path (Windows).
fn swapWanted() bool {
    const v = std.c.getenv("GHOSTTY_PRESENT") orelse return false;
    return std.mem.eql(u8, std.mem.span(v), "swap");
}

const Context = switch (Device.api) {
    .egl => *gl.egl.Context,
    .wgl => gl.wgl.Context,
};

pub fn init(
    alloc: Allocator,
    device: *const Device,
    opts: rendererpkg.Options,
) !OpenGL {
    if (comptime Device.api == .wgl) {
        // Created on the main thread, made current on the render
        // thread in `threadEnter`; nothing is current after this.
        const context = gl.wgl.Context.create(
            MIN_VERSION_MAJOR,
            MIN_VERSION_MINOR,
            builtin.mode == .debug,
        ) catch |err| {
            log.warn("failed to create WGL context err={}", .{err});
            return err;
        };
        return .{
            .alloc = alloc,
            .blending = opts.config.blending,
            .device = device,
            .context = context,
        };
    }

    const display = device.display;

    // Bind to OpenGL API explicitly before we create the context,
    // as otherwise it might fallback to OpenGL ES
    try egl.bindApi(egl.c.EGL_OPENGL_API);

    // Create our context. Contexts are surface-scoped because a
    // context can only be current on one thread at a time.
    const context = egl.Context.create(display, device.config, null, &.{
        egl.c.EGL_CONTEXT_MAJOR_VERSION,       MIN_VERSION_MAJOR,
        egl.c.EGL_CONTEXT_MINOR_VERSION,       MIN_VERSION_MINOR,
        egl.c.EGL_CONTEXT_OPENGL_PROFILE_MASK, egl.c.EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    }) catch |err| {
        log.warn("failed to create EGL context err={}", .{err});
        return err;
    };
    errdefer context.destroy(display) catch {};

    display.makeCurrent(null, null, context) catch |err| {
        log.warn("failed to make EGL context current err={}", .{err});
        return err;
    };

    // Release current so that the main thread
    // doesn't hold onto the GL context forever.
    defer display.releaseCurrent();

    return .{
        .alloc = alloc,
        .blending = opts.config.blending,
        .device = device,
        .context = context,
    };
}

pub fn deinit(self: *OpenGL) void {
    switch (comptime Device.api) {
        .egl => {
            self.device.display.releaseCurrent();
            self.context.destroy(self.device.display) catch {};
        },
        .wgl => {
            gl.wgl.releaseCurrent();
            self.context.destroy();
        },
    }
    self.* = undefined;
}

/// 32-bit windows cross-compilation breaks with `.c` for some reason, so...
const gl_debug_proc_callconv =
    @typeInfo(
        @typeInfo(
            @typeInfo(
                gl.c.GLDEBUGPROC,
            ).optional.child,
        ).pointer.child,
    ).@"fn".attrs.@"callconv";

fn glDebugMessageCallback(
    src: gl.c.GLenum,
    typ: gl.c.GLenum,
    id: gl.c.GLuint,
    severity: gl.c.GLenum,
    len: gl.c.GLsizei,
    msg: [*c]const gl.c.GLchar,
    user_param: ?*const anyopaque,
) callconv(gl_debug_proc_callconv) void {
    _ = user_param;

    const src_str: []const u8 = switch (src) {
        gl.c.GL_DEBUG_SOURCE_API => "OpenGL API",
        gl.c.GL_DEBUG_SOURCE_WINDOW_SYSTEM => "Window System",
        gl.c.GL_DEBUG_SOURCE_SHADER_COMPILER => "Shader Compiler",
        gl.c.GL_DEBUG_SOURCE_THIRD_PARTY => "Third Party",
        gl.c.GL_DEBUG_SOURCE_APPLICATION => "User",
        gl.c.GL_DEBUG_SOURCE_OTHER => "Other",
        else => "Unknown",
    };

    const typ_str: []const u8 = switch (typ) {
        gl.c.GL_DEBUG_TYPE_ERROR => "Error",
        gl.c.GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR => "Deprecated Behavior",
        gl.c.GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR => "Undefined Behavior",
        gl.c.GL_DEBUG_TYPE_PORTABILITY => "Portability Issue",
        gl.c.GL_DEBUG_TYPE_PERFORMANCE => "Performance Issue",
        gl.c.GL_DEBUG_TYPE_MARKER => "Marker",
        gl.c.GL_DEBUG_TYPE_PUSH_GROUP => "Group Push",
        gl.c.GL_DEBUG_TYPE_POP_GROUP => "Group Pop",
        gl.c.GL_DEBUG_TYPE_OTHER => "Other",
        else => "Unknown",
    };

    const msg_str = msg[0..@intCast(len)];

    (switch (severity) {
        gl.c.GL_DEBUG_SEVERITY_HIGH => log.err(
            "[{d}] ({s}: {s}) {s}",
            .{ id, src_str, typ_str, msg_str },
        ),
        gl.c.GL_DEBUG_SEVERITY_MEDIUM => log.warn(
            "[{d}] ({s}: {s}) {s}",
            .{ id, src_str, typ_str, msg_str },
        ),
        gl.c.GL_DEBUG_SEVERITY_LOW => log.info(
            "[{d}] ({s}: {s}) {s}",
            .{ id, src_str, typ_str, msg_str },
        ),
        gl.c.GL_DEBUG_SEVERITY_NOTIFICATION => log.debug(
            "[{d}] ({s}: {s}) {s}",
            .{ id, src_str, typ_str, msg_str },
        ),
        else => log.warn(
            "UNKNOWN SEVERITY [{d}] ({s}: {s}) {s}",
            .{ id, src_str, typ_str, msg_str },
        ),
    });
}

/// Prepares the provided GL context, loading it with glad.
fn prepareContext(getProcAddress: anytype) !void {
    const version = try gl.glad.load(getProcAddress);
    const major = gl.glad.versionMajor(@intCast(version));
    const minor = gl.glad.versionMinor(@intCast(version));
    errdefer gl.glad.unload();
    log.info("loaded OpenGL {}.{}", .{ major, minor });

    // Need to check version before trying to enable it
    if (major < MIN_VERSION_MAJOR or
        (major == MIN_VERSION_MAJOR and minor < MIN_VERSION_MINOR))
    {
        log.warn(
            "OpenGL version is too old. Ghostty requires OpenGL {d}.{d}",
            .{ MIN_VERSION_MAJOR, MIN_VERSION_MINOR },
        );
        return error.OpenGLOutdated;
    }

    // Enable debug output for the context.
    try gl.enable(gl.c.GL_DEBUG_OUTPUT);

    // Register our debug message callback with the OpenGL context.
    gl.glad.context.DebugMessageCallback.?(glDebugMessageCallback, null);

    // NVIDIA reports every read of the frame back into memory, which the
    // CPU frame path does once a frame (on Windows), as performance warning
    // 131154 "Pixel transfer is synchronized with 3D rendering", and it
    // calls the callback from its own worker thread. That is a log line per
    // frame; and when stderr is a pipe nobody drains (Windows PowerShell's
    // `2>` for a GUI program), the write blocks that thread, the driver's
    // other threads spin waiting on it, and the window stops responding.
    // The readback is known (zero-copy presentation replaces it), so mute
    // this one message.
    const muted = [_]gl.c.GLuint{131154};
    gl.glad.context.DebugMessageControl.?(
        gl.c.GL_DEBUG_SOURCE_API,
        gl.c.GL_DEBUG_TYPE_PERFORMANCE,
        gl.c.GL_DONT_CARE,
        muted.len,
        &muted,
        gl.c.GL_FALSE,
    );

    // Enable SRGB framebuffer for linear blending support.
    try gl.enable(gl.c.GL_FRAMEBUFFER_SRGB);
}

/// Callback called by renderer.Thread when it begins. Called on the render
/// thread. The EGL context was created at `init` time on the main thread;
/// here we (re)bind it to this thread and load the thread-local glad
/// function pointers so all subsequent GL work on this thread is valid.
pub fn threadEnter(self: *OpenGL, surface: *apprt.Surface) !void {
    switch (comptime Device.api) {
        .egl => {
            try self.device.display.makeCurrent(null, null, self.context);
            // Load our function pointers for this thread's threadlocal.
            try prepareContext(&gl.egl.getProcAddress);
        },
        .wgl => {
            const hwnd: ?gl.wgl.HWND = if (comptime @hasField(@TypeOf(surface.*), "platform"))
                @ptrCast(surface.platform.windows.hwnd)
            else
                null;
            if (swapWanted() and hwnd != null) {
                // Draw into the surface's own window; if it cannot be,
                // the frames are read back as before.
                if (self.context.attachWindow(hwnd.?)) |window| {
                    self.swap_window = window;
                    log.info("present: SwapBuffers on the surface's window", .{});
                } else |err| {
                    log.warn("present: no swap on the window err={}, reading frames back", .{err});
                    try self.context.makeCurrent();
                }
            } else {
                try self.context.makeCurrent();
            }
            try prepareContext(&gl.wgl.getProcAddress);
        },
    }
}

/// Callback called by renderer.Thread when it exits. Called on the render
/// thread; unbinds the context from this thread so it can be destroyed on
/// the main thread.
pub fn threadExit(self: *OpenGL) void {
    switch (comptime Device.api) {
        .egl => self.device.display.releaseCurrent(),
        .wgl => gl.wgl.releaseCurrent(),
    }
    gl.glad.unload();
}

/// Get the current size of the runtime surface.
pub fn surfaceSize(self: *const OpenGL) !struct { width: u32, height: u32 } {
    _ = self;
    var viewport: [4]gl.c.GLint = undefined;
    gl.glad.context.GetIntegerv.?(gl.c.GL_VIEWPORT, &viewport);
    return .{
        .width = @intCast(viewport[2]),
        .height = @intCast(viewport[3]),
    };
}

/// Set the GL viewport to cover the given size in device pixels.
///
/// This used to be automatically called by the GtkGLArea upon resizing,
/// but now we need to do this manually.
pub fn setViewport(self: *const OpenGL, width: u32, height: u32) void {
    _ = self;
    gl.viewport(0, 0, @intCast(width), @intCast(height)) catch |err| {
        log.warn("failed to set OpenGL viewport err={}", .{err});
    };
}

/// Actions taken before doing anything in `drawFrame`.
///
/// Right now there's nothing we need to do for OpenGL.
pub fn drawFrameStart(self: *OpenGL) void {
    _ = self;
}

/// Actions taken after `drawFrame` is done.
///
/// Right now there's nothing we need to do for OpenGL.
pub fn drawFrameEnd(self: *OpenGL) void {
    _ = self;
}

pub fn initShaders(
    self: *const OpenGL,
    alloc: Allocator,
    custom_shaders: []const [:0]const u8,
) !shaders.Shaders {
    _ = alloc;
    return try shaders.Shaders.init(
        self.alloc,
        custom_shaders,
    );
}

/// Initialize a new render target which can be presented by this API.
pub fn initTarget(self: *const OpenGL, width: usize, height: usize) !Target {
    _ = self;
    return Target.init(.{
        .width = width,
        .height = height,
    });
}

/// Export a rendered target. Caller takes ownership
/// of the frame and is responsible for freeing it.
///
/// This runs on the render thread.
pub fn present(
    self: *OpenGL,
    target: Target,
    presentation_health: rendererpkg.Health,
) !ExportedFrame {
    // We only export DMABUFs when the apprt can present them.
    // Otherwise, use CPU buffers. Windows has no DMABUFs at all.
    if ((comptime Device.api == .egl) and presentation_health == .healthy) {
        if (target.exportDmabuf(self.device.display, self.context)) |dmabuf| {
            return .{ .dmabuf = dmabuf };
        } else |_| {
            log.warn("failed to export DMABUF, falling back to CPU presentation", .{});
        }
    }

    // The swap path: the frame goes to the window's back buffer (the
    // target's bytes are sRGB-encoded already, so the blit copies them as
    // they are) and is swapped; the host gets only its size and the cost.
    if (comptime Device.api == .wgl) if (self.swap_window) |window| {
        const io = global.io();
        const t0: std.Io.Timestamp = .now(io, .awake);
        // A new size: make the context current on the window again. On
        // Windows that is cheap; under Wine a resized GL child window gets
        // a new drawable, and a context left on the old one swaps into
        // nothing (suspected: the window stopped updating in 1 of 3 runs).
        if (self.swap_size[0] != target.width or self.swap_size[1] != target.height) {
            self.context.rebind(window) catch |err| log.warn("present: rebind err={}", .{err});
            self.swap_size = .{ target.width, target.height };
        }
        const read = try target.framebuffer.bind(.read);
        defer read.unbind();
        const draw = try gl.Framebuffer.bind(.{ .id = 0 }, .draw);
        defer draw.unbind();
        const srgb = gl.glad.context.IsEnabled.?(gl.c.GL_FRAMEBUFFER_SRGB) != 0;
        if (srgb) gl.glad.context.Disable.?(gl.c.GL_FRAMEBUFFER_SRGB);
        const w: gl.c.GLint = @intCast(target.width);
        const h: gl.c.GLint = @intCast(target.height);
        try gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, .{ .color_buffer_bit = true }, .nearest);
        if (srgb) gl.glad.context.Enable.?(gl.c.GL_FRAMEBUFFER_SRGB);
        if (!window.swap()) log.warn("SwapBuffers failed", .{});
        const t1: std.Io.Timestamp = .now(io, .awake);
        return .{ .memory = .{
            .width = @intCast(target.width),
            .height = @intCast(target.height),
            .pixels = try self.alloc.alloc(u8, 0),
            .alloc = self.alloc,
            .readback_ns = @intCast(@max(0, t0.durationTo(t1).nanoseconds)),
        } };
    };

    // What the readback costs, for the host's stats (Windows): first the
    // wait for the GPU to finish the frame (glReadPixels would wait the
    // same, unseen), then the copy into CPU memory.
    const io = global.io();
    const t0: std.Io.Timestamp = .now(io, .awake);
    if (comptime Device.api == .wgl) gl.finish();
    const t1: std.Io.Timestamp = .now(io, .awake);
    const pixels = try target.readPixelsAlloc(self.alloc);
    const t2: std.Io.Timestamp = .now(io, .awake);
    return .{ .memory = .{
        .width = @intCast(target.width),
        .height = @intCast(target.height),
        .pixels = pixels,
        .alloc = self.alloc,
        .gpu_wait_ns = @intCast(@max(0, t0.durationTo(t1).nanoseconds)),
        .readback_ns = @intCast(@max(0, t1.durationTo(t2).nanoseconds)),
    } };
}

/// A finished frame exported for presentation by the apprt.
pub const ExportedFrame = union(enum) {
    dmabuf: Dmabuf,
    memory: Memory,

    /// RGBA8 pixel data with premultiplied alpha, tightly packed
    /// (`width * 4` bytes per row), in CPU memory.
    pub const Memory = struct {
        width: u32,
        height: u32,
        pixels: []u8,
        alloc: Allocator,
        /// On the render thread: waiting for the GPU to finish this
        /// frame, then reading it back (glReadPixels).
        gpu_wait_ns: u64 = 0,
        readback_ns: u64 = 0,

        pub fn deinit(self: Memory) void {
            self.alloc.free(self.pixels);
        }
    };

    pub fn deinit(self: ExportedFrame) void {
        switch (self) {
            .dmabuf => |v| v.deinit(),
            .memory => |v| v.deinit(),
        }
    }
};

/// Returns the options to use when constructing buffers.
pub inline fn bufferOptions(self: OpenGL) bufferpkg.Options {
    _ = self;
    return .{
        .target = .array,
        .usage = .dynamic_draw,
    };
}

pub const instanceBufferOptions = bufferOptions;
pub const uniformBufferOptions = bufferOptions;
pub const fgBufferOptions = bufferOptions;
pub const bgBufferOptions = bufferOptions;
pub const imageBufferOptions = bufferOptions;
pub const bgImageBufferOptions = bufferOptions;

/// Returns the options to use when constructing textures.
pub inline fn textureOptions(self: OpenGL) Texture.Options {
    _ = self;
    return .{
        .format = .rgba,
        .internal_format = .srgba,
        .target = .@"2d",
        .min_filter = .linear,
        .mag_filter = .linear,
        .wrap_s = .clamp_to_edge,
        .wrap_t = .clamp_to_edge,
    };
}

/// Returns the options to use when constructing samplers.
pub inline fn samplerOptions(self: OpenGL) Sampler.Options {
    _ = self;
    return .{
        .min_filter = .linear,
        .mag_filter = .linear,
        .wrap_s = .clamp_to_edge,
        .wrap_t = .clamp_to_edge,
    };
}

/// Pixel format for image texture options.
pub const ImageTextureFormat = enum {
    /// 1 byte per pixel grayscale.
    gray,
    /// 4 bytes per pixel RGBA.
    rgba,
    /// 4 bytes per pixel BGRA.
    bgra,

    fn toPixelFormat(self: ImageTextureFormat) gl.Texture.Format {
        return switch (self) {
            .gray => .red,
            .rgba => .rgba,
            .bgra => .bgra,
        };
    }
};

/// Returns the options to use when constructing textures for images.
pub inline fn imageTextureOptions(
    self: OpenGL,
    format: ImageTextureFormat,
    srgb: bool,
) Texture.Options {
    _ = self;
    return .{
        .format = format.toPixelFormat(),
        .internal_format = if (srgb) .srgba else .rgba,
        .target = .@"2d",
        // TODO: Generate mipmaps for image textures and use
        //       linear_mipmap_linear filtering so that they
        //       look good even when scaled way down.
        .min_filter = .linear,
        .mag_filter = .linear,
        // TODO: Separate out background image options, use
        //       repeating coordinate modes so we don't have
        //       to do the modulus in the shader.
        .wrap_s = .clamp_to_edge,
        .wrap_t = .clamp_to_edge,
    };
}

/// Initializes a Texture suitable for the provided font atlas.
pub fn initAtlasTexture(
    self: *const OpenGL,
    atlas: *const font.Atlas,
) Texture.Error!Texture {
    _ = self;
    const format: gl.Texture.Format, const internal_format: gl.Texture.InternalFormat =
        switch (atlas.format) {
            .grayscale => .{ .red, .red },
            .bgra => .{ .bgra, .srgba },
            else => @panic("unsupported atlas format for OpenGL texture"),
        };

    return try Texture.init(
        .{
            .format = format,
            .internal_format = internal_format,
            .target = .rectangle,
            .min_filter = .nearest,
            .mag_filter = .nearest,
            .wrap_s = .clamp_to_edge,
            .wrap_t = .clamp_to_edge,
        },
        atlas.size,
        atlas.size,
        null,
    );
}

/// Begin a frame.
pub inline fn beginFrame(
    self: *const OpenGL,
    /// Once the frame has been completed, the `frameCompleted` method
    /// on the renderer is called with the health status of the frame.
    renderer: *Renderer,
    /// The target is presented via the provided renderer's API when completed.
    target: *Target,
) !Frame {
    _ = self;
    return try Frame.begin(.{}, renderer, target);
}
