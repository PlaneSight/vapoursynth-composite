const constants = @import("constants.zig");

const Rho = struct {
    sample_ratio: f64,
    active_start: f64,
    anchor_601: f64,
};

fn anchors(standard: constants.Standard) Rho {
    return switch (standard) {
        .pal => .{
            .sample_ratio = 540000.0 / 709379.0,
            .active_start = 182.0,
            .anchor_601 = 132.0,
        },
        .ntsc => .{
            .sample_ratio = 33.0 / 35.0,
            .active_start = 130.0 + 57.0 / 90.0,
            .anchor_601 = 122.0,
        },
    };
}

pub const EncodeResample = struct {
    target_width: usize,
    source_left: f64,
    source_width: f64,
};

pub fn encodeResample(standard: constants.Standard, input_width: usize) EncodeResample {
    const a = anchors(standard);
    const scale = @as(f64, @floatFromInt(input_width)) / 720.0;
    const target_width = standard.activeWidth();
    return .{
        .target_width = target_width,
        .source_left = scale * ((a.active_start - 0.5) * a.sample_ratio - a.anchor_601) + 0.5,
        .source_width = scale * @as(f64, @floatFromInt(target_width)) * a.sample_ratio,
    };
}

pub const DecodeResample = struct {
    source_left: f64,
    source_width: f64,
};

pub fn decodeResample(standard: constants.Standard, output_width: usize) DecodeResample {
    const a = anchors(standard);
    const output = @as(f64, @floatFromInt(output_width));
    return .{
        .source_left = constants.edge_pad + a.anchor_601 / a.sample_ratio - (a.active_start - 0.5) - 0.5 * (720.0 / output) / a.sample_ratio,
        .source_width = 720.0 / a.sample_ratio,
    };
}

pub const EdgeColumns = struct { left: usize, right: usize };

pub fn decodeEdgeColumns(standard: constants.Standard, output_width: usize, raster_width: usize) EdgeColumns {
    const a = anchors(standard);
    const output = @as(f64, @floatFromInt(output_width));
    const source_left = a.anchor_601 / a.sample_ratio - (a.active_start - 0.5) - 0.5 * (720.0 / output) / a.sample_ratio;
    const step = (720.0 / a.sample_ratio) / output;

    var left: usize = 0;
    while (left < output_width and source_left + (@as(f64, @floatFromInt(left)) + 0.5) * step < -0.5) : (left += 1) {}

    var right: usize = 0;
    while (right < output_width and source_left + (@as(f64, @floatFromInt(output_width - right)) - 0.5) * step > @as(f64, @floatFromInt(raster_width)) - 0.5) : (right += 1) {}
    return .{ .left = left, .right = right };
}

test "resampling anchors retain the documented active widths" {
    const pal = encodeResample(.pal, 720);
    const ntsc = encodeResample(.ntsc, 720);
    try @import("std").testing.expectEqual(@as(usize, 928), pal.target_width);
    try @import("std").testing.expectEqual(@as(usize, 758), ntsc.target_width);
    try @import("std").testing.expect(pal.source_width > 0.0);
    try @import("std").testing.expect(ntsc.source_width > 0.0);
}
