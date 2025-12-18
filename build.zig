const std = @import("std");
const cefzig_pkg = @import("cefzig");
const cef_build = cefzig_pkg.build_helpers;

pub fn build(b: *std.Build) void {
    // CEF Windows builds are distributed for the MSVC ABI; building this project
    // with the default `windows-gnu` target will compile fine but fail to link due
    // to C++ ABI/name-mangling mismatches.
    const target = b.standardTargetOptions(.{
        .default_target = .{
            .cpu_arch = .x86_64,
            .os_tag = .windows,
            .abi = .msvc,
        },
    });
    const optimize = b.standardOptimizeOption(.{});
    if (target.result.os.tag != .windows) {
        std.log.err("chromedirect only supports Windows targets", .{});
        std.process.exit(1);
    }
    if (target.result.cpu.arch != .x86_64) {
        std.log.err("chromedirect currently expects a 64-bit target", .{});
        std.process.exit(1);
    }
    if (target.result.abi != .msvc) {
        std.log.err(
            "chromedirect requires an MSVC ABI target to link against the downloaded CEF build; try `zig build -Dtarget=x86_64-windows-msvc`",
            .{},
        );
        std.process.exit(1);
    }

    const exe_name = "chromedirect_demo";

    const cef_version = b.option(
        []const u8,
        "cef-version",
        "CEF version to download (e.g. 142.5.0+142.0.17).",
    ) orelse "142.5.0+142.0.17";

    const cef_base_url = b.option(
        []const u8,
        "cef-base-url",
        "Override the default Chromium Embedded Framework CDN URL.",
    ) orelse "https://cef-builds.spotifycdn.com";

    const use_windows_subsystem = b.option(
        bool,
        "subsystem-windows",
        "Link with the Windows subsystem (hides the console).",
    ) orelse true;

    const cefzig_dep = b.dependency("cefzig", .{});
    const openvr_dep = b.dependency("openvr", .{});

    // The CEF bootstrap is a host tool that runs during the build. Build it for
    // the native (host) target so it doesn't inherit the MSVC CRT constraints.
    // This still downloads the correct Windows CEF package (cefzig's target
    // mapping is OS/arch-based for the CEF artifacts).
    const bootstrap_target = b.resolveTargetQuery(.{});
    const prepared = cef_build.prepareCef(.{
        .b = b,
        .dependency = cefzig_dep,
        .target = bootstrap_target,
        .optimize = optimize,
        .cef_version = cef_version,
        .cef_base_url = cef_base_url,
        .bootstrap_name = "chromedirect-cef-bootstrap",
    });

    const exe = b.addExecutable(.{
        .name = exe_name,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    exe.step.dependOn(prepared.bootstrap_step);
    exe.linkLibC();
    // On `windows-msvc`, prefer the MSVC C++ runtime/headers over Zig's libc++
    // to avoid libcxxabi<->vcruntime typeinfo conflicts.
    if (target.result.abi == .msvc) {
        // Match Zig's default MSVC CRT choice (static /MT) to avoid
        // /FAILIFMISMATCH RuntimeLibrary errors.
        exe.linkSystemLibrary("libcpmt");
        exe.linkSystemLibrary("legacy_stdio_definitions");
    } else {
        exe.linkLibCpp();
    }
    exe.subsystem = if (use_windows_subsystem) .Windows else .Console;
    exe.stack_size = 0x800000;

    const common_flags = &[_][]const u8{
        "-std=c++17",
        "-DWIN32",
        "-D_WINDOWS",
        "-DUNICODE",
        "-D_UNICODE",
        "-DWINVER=0x0A00",
        "-D_WIN32_WINNT=0x0A00",
        "-DNTDDI_VERSION=0x0A00000E",
        "-DNOMINMAX",
        "-DWIN32_LEAN_AND_MEAN",
        "-D_HAS_EXCEPTIONS=0",
    };

    const sources = &[_][]const u8{
        "src/main.cpp",
        "src/Client.cpp",
        "src/CefAppHandlers.cpp",
        "src/D3DPresenter.cpp",
        "src/MinimalRenderHandler.cpp",
        "src/OpenVRPresenter.cpp",
    };

    inline for (sources) |src| {
        exe.addCSourceFile(.{ .file = b.path(src), .flags = common_flags });
    }

    exe.addIncludePath(b.path("src"));
    exe.addIncludePath(.{ .cwd_relative = prepared.cef_include_path });
    exe.addIncludePath(.{ .cwd_relative = prepared.cef_dir_path });
    exe.addIncludePath(openvr_dep.path("headers"));
    if (target.result.abi == .msvc) {
        addWindowsSdkIncludes(b, exe);
    }

    const cef_dll_wrapper = addCefDllWrapper(b, target, optimize, prepared, common_flags);
    exe.linkLibrary(cef_dll_wrapper);

    exe.addLibraryPath(.{ .cwd_relative = prepared.cef_dir_path });
    const openvr_arch_dir = switch (target.result.cpu.arch) {
        .x86_64 => "win64",
        .x86 => "win32",
        else => "win64",
    };
    exe.addLibraryPath(openvr_dep.path(b.fmt("lib/{s}", .{openvr_arch_dir})));

    // Avoid `-llibcef` resolving to `libcef.dll` (a DLL is not linkable);
    // link against the import library explicitly.
    exe.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ prepared.cef_dir_path, "libcef.lib" }) });
    exe.linkSystemLibrary("openvr_api");
    exe.linkSystemLibrary("d3d11");
    exe.linkSystemLibrary("dxgi");
    exe.linkSystemLibrary("user32");

    const cef_std_libs = &[_][]const u8{
        "comctl32",
        "crypt32",
        "delayimp",
        "gdi32",
        "rpcrt4",
        "shlwapi",
        "wintrust",
        "ws2_32",
    };
    inline for (cef_std_libs) |lib| {
        exe.linkSystemLibrary(lib);
    }

    b.installArtifact(exe);

    installCefRuntime(b, prepared);
    installOpenVrRuntime(b, openvr_arch_dir, openvr_dep);

    const run_step = b.step("run", "Run the chromedirect overlay demo");
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.cwd = .{ .cwd_relative = b.getInstallPath(.bin, "") };
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    run_step.dependOn(&run_cmd.step);
}

fn installCefRuntime(b: *std.Build, prepared: cef_build.PrepareResult) void {
    const cef_root = prepared.cef_dir_path;
    const runtime_files = &[_][]const u8{
        "chrome_elf.dll",
        "d3dcompiler_47.dll",
        "dxcompiler.dll",
        "dxil.dll",
        "icudtl.dat",
        "libcef.dll",
        "libEGL.dll",
        "libGLESv2.dll",
        "v8_context_snapshot.bin",
        "vk_swiftshader.dll",
        "vk_swiftshader_icd.json",
        "vulkan-1.dll",
        "chrome_100_percent.pak",
        "chrome_200_percent.pak",
        "resources.pak",
    };

    for (runtime_files) |rel_path| {
        const abs = b.pathJoin(&.{ cef_root, rel_path });
        installFileIfExists(b, prepared.bootstrap_step, abs, std.fs.path.basename(rel_path));
    }

    const locales_path = b.pathJoin(&.{ cef_root, "locales" });
    if (pathExists(locales_path)) {
        const install_locales = b.addInstallDirectory(.{
            .source_dir = .{ .cwd_relative = locales_path },
            .install_dir = .bin,
            .install_subdir = "locales",
        });
        install_locales.step.dependOn(prepared.bootstrap_step);
        b.getInstallStep().dependOn(&install_locales.step);
    }
}

fn installFileIfExists(
    b: *std.Build,
    dependency_step: *std.Build.Step,
    absolute_source: []const u8,
    dest_relative: []const u8,
) void {
    if (pathExists(absolute_source)) {
        const copy = b.addInstallBinFile(.{ .cwd_relative = absolute_source }, dest_relative);
        copy.step.dependOn(dependency_step);
        b.getInstallStep().dependOn(&copy.step);
    }
}

fn installOpenVrRuntime(
    b: *std.Build,
    arch_dir: []const u8,
    openvr_dep: *std.Build.Dependency,
) void {
    const dll = openvr_dep.path(b.fmt("bin/{s}/openvr_api.dll", .{arch_dir}));
    const copy = b.addInstallBinFile(dll, "openvr_api.dll");
    b.getInstallStep().dependOn(&copy.step);
}

fn pathExists(p: []const u8) bool {
    const flags = std.fs.File.OpenFlags{};
    if (std.fs.path.isAbsolute(p)) {
        std.fs.accessAbsolute(p, flags) catch |err| switch (err) {
            error.FileNotFound => return false,
            else => std.debug.panic("access({s}) failed: {s}", .{ p, @errorName(err) }),
        };
        return true;
    }
    std.fs.cwd().access(p, flags) catch |err| switch (err) {
        error.FileNotFound => return false,
        else => std.debug.panic("access({s}) failed: {s}", .{ p, @errorName(err) }),
    };
    return true;
}

fn addWindowsSdkIncludes(b: *std.Build, exe: *std.Build.Step.Compile) void {
    const allocator = b.allocator;
    const candidates = &[_][]const u8{
        "C:\\Program Files (x86)\\Windows Kits\\10\\Include",
        "C:\\Program Files\\Windows Kits\\10\\Include",
    };

    var root: ?[]const u8 = null;
    for (candidates) |cand| {
        if (pathExists(cand)) {
            root = cand;
            break;
        }
    }
    const include_root = root orelse return;

    var dir = std.fs.openDirAbsolute(include_root, .{ .iterate = true }) catch return;
    defer dir.close();

    var best_name: ?[]u8 = null;
    defer if (best_name) |s| allocator.free(s);
    var best_ver: [4]u32 = .{ 0, 0, 0, 0 };

    var it = dir.iterate();
    while (it.next() catch null) |entry| {
        if (entry.kind != .directory) continue;
        const parsed = parseSdkVersion(entry.name) orelse continue;
        if (versionGreater(parsed, best_ver)) {
            if (best_name) |old| allocator.free(old);
            best_name = allocator.dupe(u8, entry.name) catch @panic("oom");
            best_ver = parsed;
        }
    }

    const ver_name = best_name orelse return;
    const base = b.pathJoin(&.{ include_root, ver_name });
    const subdirs = &[_][]const u8{ "shared", "um", "ucrt", "winrt" };
    inline for (subdirs) |sub| {
        const p = b.pathJoin(&.{ base, sub });
        if (pathExists(p)) exe.addSystemIncludePath(.{ .cwd_relative = p });
    }
}

fn parseSdkVersion(name: []const u8) ?[4]u32 {
    var parts: [4]u32 = .{ 0, 0, 0, 0 };
    var i: usize = 0;
    var it = std.mem.splitScalar(u8, name, '.');
    while (it.next()) |piece| {
        if (i >= parts.len) return null;
        parts[i] = std.fmt.parseInt(u32, piece, 10) catch return null;
        i += 1;
    }
    return if (i == parts.len) parts else null;
}

fn versionGreater(a: [4]u32, b: [4]u32) bool {
    inline for (0..4) |idx| {
        if (a[idx] > b[idx]) return true;
        if (a[idx] < b[idx]) return false;
    }
    return false;
}

fn addCefDllWrapper(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    prepared: cef_build.PrepareResult,
    common_flags: []const []const u8,
) *std.Build.Step.Compile {
    const lib = b.addLibrary(.{
        .name = "cef_dll_wrapper",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    lib.step.dependOn(prepared.bootstrap_step);
    lib.linkLibC();
    if (target.result.abi == .msvc) {
        lib.linkSystemLibrary("libcpmt");
        lib.linkSystemLibrary("legacy_stdio_definitions");
        addWindowsSdkIncludes(b, lib);
    } else {
        lib.linkLibCpp();
    }

    lib.addIncludePath(.{ .cwd_relative = prepared.cef_include_path });
    lib.addIncludePath(.{ .cwd_relative = prepared.cef_dir_path });
    lib.addIncludePath(.{ .cwd_relative = b.pathJoin(&.{ prepared.cef_dir_path, "libcef_dll" }) });

    var wrapper_flags_list: std.ArrayList([]const u8) = .empty;
    wrapper_flags_list.appendSlice(b.allocator, common_flags) catch @panic("oom");
    wrapper_flags_list.append(b.allocator, "-DWRAPPING_CEF_SHARED") catch @panic("oom");
    const wrapper_flags = wrapper_flags_list.toOwnedSlice(b.allocator) catch @panic("oom");
    const dll_root = b.pathJoin(&.{ prepared.cef_dir_path, "libcef_dll" });
    var dir = std.fs.cwd().openDir(dll_root, .{ .iterate = true }) catch |err| {
        std.debug.panic("openDir({s}) failed: {s}", .{ dll_root, @errorName(err) });
    };
    defer dir.close();

    var walker = dir.walk(b.allocator) catch @panic("oom");
    defer walker.deinit();
    while (walker.next() catch null) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.path, ".cc")) continue;
        if (std.mem.indexOf(u8, entry.path, "cpptoc\\test\\") != null) continue;
        if (std.mem.indexOf(u8, entry.path, "cpptoc/test/") != null) continue;
        const abs = b.pathJoin(&.{ dll_root, entry.path });
        lib.addCSourceFile(.{ .file = .{ .cwd_relative = abs }, .flags = wrapper_flags });
    }

    return lib;
}
