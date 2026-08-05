const std = @import("std");

test {
    _ = @import("composite");
}

test "decoder accepts the supported PAL and NTSC paths" {
    const composite = @import("composite");
    var pal = try composite.decode.Decoder.init(std.testing.allocator, .{
        .standard = .pal,
        .dimensions = 2,
        .eq = 0,
        .scratch_count = 1,
    });
    defer pal.deinit();
    var ntsc = try composite.decode.Decoder.init(std.testing.allocator, .{
        .standard = .ntsc,
        .dimensions = 3,
        .use_transform = 2,
        .eq = 0,
        .scratch_count = 1,
    });
    defer ntsc.deinit();
    try std.testing.expectEqual(@as(usize, 0), pal.look());
    try std.testing.expectEqual(@as(usize, 3), ntsc.look());
}

test "crude decoder round-trips a neutral PAL raster level" {
    const composite = @import("composite");
    const width = composite.constants.Standard.pal.activeWidth();
    const height = composite.constants.Standard.pal.activeHeight();
    const plane = width * height;
    const comp = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(comp);
    const y = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(y);
    const u = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(u);
    const v = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(v);
    @memset(comp, 0x4000);
    @memset(y, 0);
    @memset(u, 0);
    @memset(v, 0);

    var decoder = try composite.decode.Decoder.init(std.testing.allocator, .{
        .standard = .pal,
        .dimensions = 1,
        .eq = 0,
        .scratch_count = 1,
    });
    defer decoder.deinit();
    const views = [_]composite.decode.FrameView{.{ .data = comp, .stride = width, .frame_number = 0 }};
    decoder.decodeFrame(0, 1, &views, 0, .{
        .y = y,
        .y_stride = width,
        .u = u,
        .u_stride = width,
        .v = v,
        .v_stride = width,
        .rows = height,
    });
    try std.testing.expectEqual(@as(u16, 4096), y[width * (height / 2) + width / 2]);
    try std.testing.expectEqual(@as(u16, 32768), u[width * (height / 2) + width / 2]);
    try std.testing.expectEqual(@as(u16, 32768), v[width * (height / 2) + width / 2]);
}

test "PAL 3D decoder completes a bounded short frame" {
    const composite = @import("composite");
    const width = composite.constants.Standard.pal.activeWidth();
    const rows = 32;
    const plane = width * rows;
    const comp = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(comp);
    const y = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(y);
    const u = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(u);
    const v = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(v);
    @memset(comp, 0x4000);
    @memset(y, 0);
    @memset(u, 0);
    @memset(v, 0);

    var decoder = try composite.decode.Decoder.init(std.testing.allocator, .{
        .standard = .pal,
        .dimensions = 3,
        .eq = 0,
        .scratch_count = 1,
    });
    defer decoder.deinit();
    var views: [7]composite.decode.FrameView = undefined;
    for (&views) |*view| view.* = .{ .data = comp, .stride = width, .frame_number = 0 };
    decoder.decodeFrame(0, 1, views[0..], 3, .{
        .y = y,
        .y_stride = width,
        .u = u,
        .u_stride = width,
        .v = v,
        .v_stride = width,
        .rows = rows,
    });
    try std.testing.expect(y[width * (rows / 2) + width / 2] > 0);
    try std.testing.expect(u[width * (rows / 2) + width / 2] > 0);
    try std.testing.expect(v[width * (rows / 2) + width / 2] > 0);
}

test "NTSC hybrid decoder completes a bounded short frame" {
    const composite = @import("composite");
    const width = composite.constants.Standard.ntsc.activeWidth();
    const rows = 32;
    const plane = width * rows;
    const comp = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(comp);
    const y = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(y);
    const u = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(u);
    const v = try std.testing.allocator.alloc(u16, plane);
    defer std.testing.allocator.free(v);
    @memset(comp, 0x4000);
    @memset(y, 0);
    @memset(u, 0);
    @memset(v, 0);

    var decoder = try composite.decode.Decoder.init(std.testing.allocator, .{
        .standard = .ntsc,
        .dimensions = 3,
        .use_transform = 2,
        .eq = 0,
        .scratch_count = 1,
    });
    defer decoder.deinit();
    var views: [7]composite.decode.FrameView = undefined;
    for (&views) |*view| view.* = .{ .data = comp, .stride = width, .frame_number = 0 };
    decoder.decodeFrame(0, 1, views[0..], 3, .{
        .y = y,
        .y_stride = width,
        .u = u,
        .u_stride = width,
        .v = v,
        .v_stride = width,
        .rows = rows,
    });
    try std.testing.expect(y[width * (rows / 2) + width / 2] > 0);
    try std.testing.expect(u[width * (rows / 2) + width / 2] > 0);
    try std.testing.expect(v[width * (rows / 2) + width / 2] > 0);
}
