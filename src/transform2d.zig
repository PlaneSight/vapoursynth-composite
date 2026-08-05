const std = @import("std");
const constants = @import("constants.zig");
const fft = @import("fft.zig");

pub const Transform2d = struct {
    pub const Error = error{InvalidThreshold};
    const width = constants.Transform2d.tile_width;
    const height = constants.Transform2d.tile_height;
    const bins = constants.Transform2d.threshold_bins;

    level: bool,
    use_lut: bool,
    evidence: f32,
    window: [height][width]f32,
    threshold_squared: [bins]f32,
    lut: [bins][constants.lut_knots]f32,

    pub fn init(threshold: f64, level: bool, evidence: f64) Error!Transform2d {
        if (!(threshold > 0.0 and threshold <= 1.0) or evidence < 0.0) return error.InvalidThreshold;
        var result = Transform2d{
            .level = level,
            .use_lut = false,
            .evidence = @floatCast(evidence),
            .window = undefined,
            .threshold_squared = undefined,
            .lut = undefined,
        };
        const threshold_sq: f32 = @floatCast(threshold * threshold);
        for (&result.threshold_squared) |*value| value.* = threshold_sq;
        for (0..height) |y| {
            const wy = raisedCosine(y, height);
            for (0..width) |x| result.window[y][x] = wy * raisedCosine(x, width);
        }
        return result;
    }

    pub fn setThresholds(self: *Transform2d, values: []const f64) Error!void {
        if (values.len != bins) return error.InvalidThreshold;
        for (values, 0..) |value, i| {
            if (!(value > 0.0 and value <= 1.0)) return error.InvalidThreshold;
            self.threshold_squared[i] = @floatCast(value * value);
        }
        self.use_lut = false;
    }

    pub fn setLut(self: *Transform2d, values: []const f64) Error!void {
        if (values.len != bins * constants.lut_knots) return error.InvalidThreshold;
        for (0..bins) |bin| {
            for (0..constants.lut_knots) |knot| {
                const value = values[bin * constants.lut_knots + knot];
                if (!(value >= 0.0 and value <= 1.0)) return error.InvalidThreshold;
                self.lut[bin][knot] = @floatCast(value);
            }
        }
        self.use_lut = true;
    }

    pub fn field(self: *const Transform2d, composite: []const u16, composite_stride: usize, raster_width: usize, rows: usize, chroma: []f32, chroma_stride: usize, confidence: ?[]f32, confidence_stride: usize) void {
        std.debug.assert(raster_width <= constants.Standard.pal.activeWidth());
        std.debug.assert(chroma.len >= chroma_stride * rows);
        for (0..rows) |row| {
            @memset(chroma[row * chroma_stride ..][0..raster_width], 0.0);
            if (confidence) |map| @memset(map[row * confidence_stride ..][0..raster_width], 0.0);
        }

        var real: [width * height]f32 = undefined;
        var input: [width * height]fft.Complex = undefined;
        var output: [width * height]fft.Complex = undefined;
        var tile_y: isize = -@as(isize, @intCast(height / 2));
        while (tile_y < @as(isize, @intCast(rows))) : (tile_y += @as(isize, @intCast(height / 2))) {
            const start_y: usize = if (tile_y < 0) @intCast(-tile_y) else 0;
            const remaining_y = @as(isize, @intCast(rows)) - tile_y;
            const end_y: usize = if (remaining_y < @as(isize, @intCast(height))) @intCast(remaining_y) else height;
            var tile_x: isize = -@as(isize, @intCast(width / 2));
            while (tile_x < @as(isize, @intCast(raster_width))) : (tile_x += @as(isize, @intCast(width / 2))) {
                const start_x: usize = if (tile_x < 0) @intCast(-tile_x) else 0;
                const remaining_x = @as(isize, @intCast(raster_width)) - tile_x;
                const end_x: usize = if (remaining_x < @as(isize, @intCast(width))) @intCast(remaining_x) else width;

                for (0..height) |y| {
                    const valid_row = y >= start_y and y < end_y;
                    const source_row: []const u16 = if (valid_row) composite[@as(usize, @intCast(tile_y + @as(isize, @intCast(y)))) * composite_stride ..] else &[_]u16{};
                    const x0 = if (valid_row) start_x else width;
                    const x1 = if (valid_row) end_x else width;
                    const window_row = self.window[y];
                    const destination = real[y * width ..][0..width];
                    for (0..x0) |x| destination[x] = 16384.0 * window_row[x];
                    for (x0..x1) |x| destination[x] = @as(f32, @floatFromInt(source_row[@as(usize, @intCast(tile_x + @as(isize, @intCast(x))))])) * window_row[x];
                    for (x1..width) |x| destination[x] = 16384.0 * window_row[x];
                }

                fft.forward2d(&real, &input, width, height);
                const tile_confidence = self.applyFilter(&input, &output);
                fft.inverse2d(&output, &real, width, height);
                for (start_y..end_y) |y| {
                    const destination = chroma[@as(usize, @intCast(tile_y + @as(isize, @intCast(y)))) * chroma_stride ..];
                    for (start_x..end_x) |x| destination[@as(usize, @intCast(tile_x + @as(isize, @intCast(x))))] += real[y * width + x];
                    if (confidence) |map| {
                        const confidence_row = map[@as(usize, @intCast(tile_y + @as(isize, @intCast(y)))) * confidence_stride ..];
                        for (start_x..end_x) |x| confidence_row[@as(usize, @intCast(tile_x + @as(isize, @intCast(x))))] += tile_confidence * self.window[y][x];
                    }
                }
            }
        }
    }

    fn applyFilter(self: *const Transform2d, input: []const fft.Complex, output: []fft.Complex) f32 {
        @memset(output, .{});
        var confidence_numerator: f32 = 0.0;
        var confidence_denominator: f32 = 0.0;
        var bin: usize = 0;
        for (0..height) |y| {
            const reflected_y = (height / 2 + height - y) % height;
            for (width / 8..width / 4 + 1) |x| {
                const reflected_x = width / 2 - x;
                const source = input[y * width + x];
                const reflected = input[reflected_y * width + reflected_x];
                if (x == reflected_x and y == reflected_y) {
                    output[y * width + x] = source;
                    const energy = source.magnitudeSquared();
                    confidence_numerator += energy;
                    confidence_denominator += energy;
                    bin += 1;
                    continue;
                }

                const input_energy = source.magnitudeSquared();
                const reflected_energy = reflected.magnitudeSquared();
                const low = @min(input_energy, reflected_energy);
                const high = @max(input_energy, reflected_energy);
                const ratio = if (high > 0.0) low / high else 1.0;
                var evidence_gain: f32 = 1.0;
                if (self.evidence > 0.0) {
                    const lf_y1 = (height / 4 + height - y) % height;
                    const lf_y2 = (3 * (height / 4) + height - y) % height;
                    const lf_x = width / 4 - x;
                    const e1 = input[lf_y1 * width + lf_x].magnitudeSquared();
                    const e2 = input[lf_y2 * width + lf_x].magnitudeSquared();
                    const evidence_energy = @max(e1, e2);
                    const denominator = evidence_energy + self.evidence * high;
                    if (denominator > 0.0) evidence_gain = evidence_energy / denominator;
                }

                if (self.use_lut) {
                    const gain = lutGain(self.lut[bin][0..], low, high) * evidence_gain;
                    output[y * width + x] = source.scale(gain);
                    output[reflected_y * width + reflected_x] = reflected.scale(gain);
                    const energy = gain * gain * (input_energy + reflected_energy);
                    confidence_numerator += energy * ratio;
                    confidence_denominator += energy;
                } else if (self.level) {
                    var source_gain = evidence_gain;
                    var reflected_gain = evidence_gain;
                    if (input_energy > reflected_energy and input_energy > 0.0) source_gain *= @sqrt(reflected_energy / input_energy);
                    if (reflected_energy > input_energy and reflected_energy > 0.0) reflected_gain *= @sqrt(input_energy / reflected_energy);
                    output[y * width + x] = source.scale(source_gain);
                    output[reflected_y * width + reflected_x] = reflected.scale(reflected_gain);
                    const energy = 2.0 * low * evidence_gain * evidence_gain;
                    confidence_numerator += energy * ratio;
                    confidence_denominator += energy;
                } else if (!(input_energy < reflected_energy * self.threshold_squared[bin] or reflected_energy < input_energy * self.threshold_squared[bin])) {
                    output[y * width + x] = source.scale(evidence_gain);
                    output[reflected_y * width + reflected_x] = reflected.scale(evidence_gain);
                    const energy = (input_energy + reflected_energy) * evidence_gain * evidence_gain;
                    confidence_numerator += energy * ratio;
                    confidence_denominator += energy;
                }
                bin += 1;
            }
        }
        return if (confidence_denominator > 0.0) confidence_numerator / confidence_denominator else 0.0;
    }
};

fn raisedCosine(element: usize, limit: usize) f32 {
    const angle = 2.0 * std.math.pi * (@as(f64, @floatFromInt(element)) + 0.5) / @as(f64, @floatFromInt(limit));
    return @floatCast(0.5 - 0.5 * @cos(angle));
}

fn lutGain(row: []const f32, low: f32, high: f32) f32 {
    const ratio = if (high > 0.0) low / high else 1.0;
    const position = ratio * @as(f32, @floatFromInt(constants.lut_knots - 1));
    var index: usize = @intFromFloat(position);
    if (index > constants.lut_knots - 2) index = constants.lut_knots - 2;
    return row[index] + (row[index + 1] - row[index]) * (position - @as(f32, @floatFromInt(index)));
}

test "2D transform rejects invalid thresholds" {
    try std.testing.expectError(error.InvalidThreshold, Transform2d.init(0.0, false, 0.0));
    try std.testing.expectError(error.InvalidThreshold, Transform2d.init(1.1, false, 0.0));
    try std.testing.expectError(error.InvalidThreshold, Transform2d.init(0.4, false, -1.0));
}

test "2D transform has unity window centre" {
    const transform = try Transform2d.init(0.4, false, 0.0);
    try std.testing.expect(transform.window[8][16] > 0.99);
}
