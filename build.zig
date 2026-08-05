const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const vapoursynth = b.dependency("vapoursynth", .{
        .target = target,
        .optimize = optimize,
    });

    const core = b.addModule("composite", .{
        .root_source_file = b.path("src/core.zig"),
        .target = target,
        .optimize = optimize,
    });

    const plugin = b.createModule(.{
        .root_source_file = b.path("src/plugin.zig"),
        .target = target,
        .optimize = optimize,
        .imports = &.{
            .{ .name = "composite", .module = core },
            .{ .name = "vapoursynth", .module = vapoursynth.module("vapoursynth") },
        },
    });

    const library = b.addLibrary(.{
        .name = "composite",
        .linkage = .dynamic,
        .root_module = plugin,
    });
    library.root_module.link_libc = true;
    b.installArtifact(library);

    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/tests.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{.{ .name = "composite", .module = core }},
        }),
    });
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run deterministic codec and geometry tests");
    test_step.dependOn(&run_tests.step);

    const check = b.step("check", "Compile the plugin without installing it");
    check.dependOn(&library.step);
}
