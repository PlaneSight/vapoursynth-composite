const std = @import("std");
const constants = @import("constants.zig");
const fir = @import("fir.zig");
const subcarrier = @import("subcarrier.zig");

const uv_taps_pal = [_]i16{ 4, 38, 270, 1226, 3619, 6927, 8600, 6927, 3619, 1226, 270, 38, 4 };
const uv_taps_ntsc = [_]i16{ 69, 626, 2959, 7563, 10334, 7563, 2959, 626, 69 };

const kb = 0.49211104112248356308804691718185;
const kr = 0.87728321993817866838972487283129;

pub const Encoder = struct {
    pub const Error = error{UnsupportedStandard};

    standard: constants.Standard,
    width: usize,
    denominator: usize,
    precombine: bool,
    uv_taps: []const i16,
    uv_taps_q15: [constants.uv_max_taps]i32,
    ku: i32,
    kv: i32,
    level_black: i32,
    luma_numerator: i32,
    luma_denominator: i32,
    sine_q15: [2500]i16,

    pub fn init(standard: constants.Standard, setup: bool, precombine: bool) Error!Encoder {
        var result = Encoder{
            .standard = standard,
            .width = standard.activeWidth(),
            .denominator = standard.subcarrierDenominator(),
            .precombine = precombine,
            .uv_taps = undefined,
            .uv_taps_q15 = [_]i32{0} ** constants.uv_max_taps,
            .ku = 0,
            .kv = 0,
            .level_black = 0,
            .luma_numerator = 0,
            .luma_denominator = 0,
            .sine_q15 = [_]i16{0} ** 2500,
        };

        const level_white: i32 = switch (standard) {
            .pal => blk: {
                result.uv_taps = uv_taps_pal[0..];
                result.level_black = 0x4000;
                break :blk 0xD300;
            },
            .ntsc => blk: {
                result.uv_taps = uv_taps_ntsc[0..];
                result.level_black = if (setup) 0x4680 else 0x3C00;
                break :blk 0xC800;
            },
        };

        result.luma_numerator = @divTrunc(level_white - result.level_black, 256);
        result.luma_denominator = 219;
        if (result.luma_numerator * 256 != level_white - result.level_black) {
            result.luma_numerator = @divTrunc(level_white - result.level_black, 128);
            result.luma_denominator = 438;
        }

        const span = @as(f64, @floatFromInt(level_white - result.level_black));
        result.ku = @intFromFloat(@round((1.0 - 0.114) * kb / (112.0 * 256.0) * span * 32768.0));
        result.kv = @intFromFloat(@round((1.0 - 0.299) * kr / (112.0 * 256.0) * span * 32768.0));
        subcarrier.fillSineQ15(result.sine_q15[0..result.denominator]);
        for (result.uv_taps, 0..) |tap, i| result.uv_taps_q15[i] = tap;
        return result;
    }

    pub fn encodeLine(self: *const Encoder, destination: []u16, y: []const u16, u: []const u16, v: []const u16, phase: subcarrier.Line) void {
        std.debug.assert(destination.len >= self.width);
        std.debug.assert(y.len >= self.width and u.len >= self.width and v.len >= self.width);

        var u_staged: [constants.alignedWidth(928) + constants.uv_max_taps - 1]i32 = undefined;
        var v_staged: [constants.alignedWidth(928) + constants.uv_max_taps - 1]i32 = undefined;
        var u_filtered: [928]i32 = undefined;
        var v_filtered: [928]i32 = undefined;
        var luma: [928]i32 = undefined;
        const tap_count = self.uv_taps.len;
        const half = tap_count / 2;
        @memset(u_staged[0 .. constants.alignedWidth(928) + constants.uv_max_taps - 1], 0);
        @memset(v_staged[0 .. constants.alignedWidth(928) + constants.uv_max_taps - 1], 0);
        for (0..self.width) |x| {
            u_staged[x + half] = @as(i32, u[x]) - 32768;
            v_staged[x + half] = @as(i32, v[x]) - 32768;
        }
        fir.rowQ15(u_filtered[0..self.width], u_staged[0 .. self.width + tap_count - 1], self.uv_taps_q15[0..tap_count]);
        fir.rowQ15(v_filtered[0..self.width], v_staged[0 .. self.width + tap_count - 1], self.uv_taps_q15[0..tap_count]);

        const sine = @as(i32, self.sine_q15[phase.phase]);
        const cosine = @as(i32, self.sine_q15[(phase.phase + self.denominator / 4) % self.denominator]);
        const sine_pattern = [_]i32{ sine, cosine, -sine, -cosine };
        const cosine_pattern = [_]i32{ phase.v_switch * cosine, -phase.v_switch * sine, -phase.v_switch * cosine, phase.v_switch * sine };

        for (0..self.width) |x| {
            const luma_input = @as(i32, y[x]) - 4096;
            luma[x] = self.level_black + constants.roundDivSigned(@as(i64, luma_input) * self.luma_numerator, self.luma_denominator);
            const chroma_u = @divTrunc(u_filtered[x] * self.ku + 16384, 1 << 15);
            const chroma_v = @divTrunc(v_filtered[x] * self.kv + 16384, 1 << 15);
            const chroma = @divTrunc(chroma_u * sine_pattern[x & 3] + chroma_v * cosine_pattern[x & 3] + 16384, 1 << 15);
            destination[x] = constants.clampLevel(luma[x] + chroma);
        }
    }

    pub fn encodeFrame(self: *const Encoder, frame: i32, rows: usize, row_offset: i32, destination: []u16, destination_stride: usize, y: []const u16, y_stride: usize, u: []const u16, u_stride: usize, v: []const u16, v_stride: usize) void {
        std.debug.assert(destination.len >= destination_stride * rows);
        var filtered_u: [928]u16 = undefined;
        var filtered_v: [928]u16 = undefined;
        for (0..rows) |row| {
            const phase = subcarrier.line(self.standard, frame, row_offset + @as(i32, @intCast(row)));
            const dst = destination[row * destination_stride ..];
            const src_y = y[row * y_stride ..];
            if (!self.precombine) {
                self.encodeLine(dst, src_y, u[row * u_stride ..], v[row * v_stride ..], phase);
                continue;
            }

            const above = if (row >= 2) row - 2 else row;
            const below = if (row + 2 < rows) row + 2 else row;
            for (0..self.width) |x| {
                filtered_u[x] = @intCast((@as(u32, u[above * u_stride + x]) + 2 * @as(u32, u[row * u_stride + x]) + @as(u32, u[below * u_stride + x]) + 2) >> 2);
                filtered_v[x] = @intCast((@as(u32, v[above * v_stride + x]) + 2 * @as(u32, v[row * v_stride + x]) + @as(u32, v[below * v_stride + x]) + 2) >> 2);
            }
            self.encodeLine(dst, src_y, filtered_u[0..], filtered_v[0..], phase);
        }
    }
};

test "PAL encoder preserves neutral chroma around black" {
    var encoder = try Encoder.init(.pal, false, false);
    var y: [928]u16 = [_]u16{4096} ** 928;
    var u: [928]u16 = [_]u16{32768} ** 928;
    var v: [928]u16 = [_]u16{32768} ** 928;
    var output: [928]u16 = undefined;
    encoder.encodeLine(&output, &y, &u, &v, subcarrier.line(.pal, 0, 0));
    for (output) |sample| try std.testing.expectEqual(@as(u16, 0x4000), sample);
}
