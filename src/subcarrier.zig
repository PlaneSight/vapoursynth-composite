const std = @import("std");
const constants = @import("constants.zig");

pub const Line = struct {
    phase: usize,
    v_switch: i32,
};

pub fn fillSineQ15(table: []i16) void {
    const denominator = table.len;
    std.debug.assert(denominator > 0);
    for (table, 0..) |*value, k| {
        const radians = 2.0 * std.math.pi * @as(f64, @floatFromInt(k)) / @as(f64, @floatFromInt(denominator));
        const scaled = @round(@sin(radians) * 32768.0);
        const integer: i32 = @intFromFloat(scaled);
        value.* = @intCast(std.math.clamp(integer, -32768, 32767));
    }
}

pub fn line(standard: constants.Standard, frame: i32, row: i32) Line {
    return switch (standard) {
        .pal => {
            const frame_line = 44 + row;
            const field_id = @mod(frame, 4) * 2 + @mod(frame_line, 2);
            const previous_lines = @divTrunc(field_id, 2) * 625 + @mod(field_id, 2) * 313 + @divTrunc(frame_line, 2);
            const phase = @mod(182 * 625 + @mod(previous_lines, 2500) * 1879, 2500);
            return .{ .phase = @intCast(phase), .v_switch = if (@mod(previous_lines, 2) != 0) -1 else 1 };
        },
        .ntsc => {
            const frame_line = 39 + row;
            const field_id = @mod(frame, 2) * 2 + @mod(frame_line, 2);
            const previous_lines = @divTrunc(field_id, 2) * 525 + @mod(field_id, 2) * 263 + @divTrunc(frame_line, 2);
            const phase = @mod(130 * 180 + 654 + @mod(previous_lines, 720) * 360, 720);
            return .{ .phase = @intCast(phase), .v_switch = 1 };
        },
    };
}

test "subcarrier phase bookkeeping is periodic" {
    const pal0 = line(.pal, 0, 0);
    const pal4 = line(.pal, 4, 0);
    try std.testing.expectEqual(pal0.phase, pal4.phase);
    try std.testing.expectEqual(pal0.v_switch, pal4.v_switch);

    const ntsc0 = line(.ntsc, 0, 0);
    const ntsc2 = line(.ntsc, 2, 0);
    try std.testing.expectEqual(ntsc0.phase, ntsc2.phase);
}

test "sine table clamps positive unity" {
    var table: [720]i16 = undefined;
    fillSineQ15(&table);
    try std.testing.expectEqual(@as(i16, 32767), table[180]);
    try std.testing.expectEqual(@as(i16, -32768), table[540]);
}
