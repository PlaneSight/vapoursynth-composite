const std = @import("std");
const constants = @import("constants.zig");
const fft = @import("fft.zig");

pub const FieldView = struct {
    data: ?[]const u16,
    stride: usize,
};

pub const Transform3d = struct {
    pub const Error = error{InvalidConfiguration};
    const width = constants.Transform3d.tile_width;
    const height = constants.Transform3d.tile_height;
    const pal_height = constants.Transform3d.pal_tile_height;
    const depth = constants.Transform3d.tile_depth;
    const bins = constants.Transform3d.threshold_bins;

    standard: constants.Standard,
    level: bool,
    use_lut: bool,
    evidence: f32,
    window: [depth][height][width]f32,
    threshold_squared: [bins]f32,
    lut: [bins][constants.lut_knots]f32,

    pub fn init(standard: constants.Standard, threshold: f64, level: bool, evidence: f64) Error!Transform3d {
        if (!(threshold > 0.0 and threshold <= 1.0) or evidence < 0.0) return error.InvalidConfiguration;
        var result = Transform3d{
            .standard = standard,
            .level = level,
            .use_lut = false,
            .evidence = @floatCast(evidence),
            .window = undefined,
            .threshold_squared = undefined,
            .lut = undefined,
        };
        for (&result.window) |*plane| for (plane) |*row| @memset(row, 0.0);
        const tile_height = if (standard == .pal) pal_height else height;
        const threshold_sq: f32 = @floatCast(threshold * threshold);
        for (&result.threshold_squared) |*value| value.* = threshold_sq;
        for (0..depth) |z| {
            const wz = raisedCosine(z, depth);
            for (0..tile_height) |y| {
                const wy = raisedCosine(y, tile_height);
                for (0..width) |x| result.window[z][y][x] = wz * wy * raisedCosine(x, width);
            }
        }
        return result;
    }

    pub fn setThresholds(self: *Transform3d, values: []const f64) Error!void {
        const expected = if (self.standard == .pal) constants.Transform3d.pal_threshold_bins else constants.Transform3d.threshold_bins;
        if (values.len != expected) return error.InvalidConfiguration;
        for (values, 0..) |value, i| {
            if (!(value > 0.0 and value <= 1.0)) return error.InvalidConfiguration;
            self.threshold_squared[i] = @floatCast(value * value);
        }
        self.use_lut = false;
    }

    pub fn setLut(self: *Transform3d, values: []const f64) Error!void {
        const bins_for_standard = if (self.standard == .pal) constants.Transform3d.pal_threshold_bins else constants.Transform3d.threshold_bins;
        if (values.len != bins_for_standard * constants.lut_knots) return error.InvalidConfiguration;
        for (0..bins_for_standard) |bin| {
            for (0..constants.lut_knots) |knot| {
                const value = values[bin * constants.lut_knots + knot];
                if (!(value >= 0.0 and value <= 1.0)) return error.InvalidConfiguration;
                self.lut[bin][knot] = @floatCast(value);
            }
        }
        self.use_lut = true;
    }

    /// Separates the two fields of one output frame. The field window is
    /// borrowed from the caller and is indexed by its absolute field number.
    /// The function deliberately recomputes the two covering temporal slabs;
    /// this is the correctness-first reference path. The decoder may add a
    /// bounded immutable slab cache around this operation later.
    pub fn frame(self: *const Transform3d, fields: []const FieldView, first_field: i32, output_frame: i32, parity: i32, raster_width: usize, field_rows: usize, output: [2][]f32, output_stride: usize, confidence: ?[2][]f32, confidence_stride: usize) void {
        std.debug.assert(raster_width > 0 and raster_width <= constants.Standard.pal.activeWidth());
        for (output) |plane| {
            for (0..field_rows) |row| @memset(plane[row * output_stride ..][0..raster_width], 0.0);
        }
        if (confidence) |maps| {
            for (maps) |plane| {
                for (0..field_rows) |row| @memset(plane[row * confidence_stride ..][0..raster_width], 0.0);
            }
        }

        const frame_field = output_frame * 2;
        const half_depth: i32 = @intCast(depth / 2);
        const high_origin = @divTrunc(frame_field + 1 + half_depth - 1, half_depth) * half_depth;
        const origins = [_]i32{ high_origin - @as(i32, @intCast(depth / 2)), high_origin };
        for (origins) |origin| self.buildOrigin(fields, first_field, origin, frame_field, parity, raster_width, field_rows, output, output_stride, confidence, confidence_stride);
    }

    fn buildOrigin(self: *const Transform3d, fields: []const FieldView, first_field: i32, origin: i32, output_field: i32, parity: i32, raster_width: usize, field_rows: usize, output: [2][]f32, output_stride: usize, confidence: ?[2][]f32, confidence_stride: usize) void {
        const tile_height = if (self.standard == .pal) pal_height else height;
        const real_len = depth * tile_height * width;
        var real: [depth * height * width]f32 = undefined;
        var input: [depth * height * width]fft.Complex = undefined;
        var filtered: [depth * height * width]fft.Complex = undefined;
        var tile_x: isize = -@as(isize, @intCast(width / 2));
        while (tile_x < @as(isize, @intCast(raster_width))) : (tile_x += @as(isize, @intCast(width / 2))) {
            const start_x: usize = if (tile_x < 0) @intCast(-tile_x) else 0;
            const remaining_x = @as(isize, @intCast(raster_width)) - tile_x;
            const end_x: usize = if (remaining_x < @as(isize, @intCast(width))) @intCast(remaining_x) else width;

            if (self.standard == .pal) {
                var tile_row: isize = -@as(isize, @intCast(pal_height / 2));
                while (tile_row < @as(isize, @intCast(field_rows + depth / 2))) : (tile_row += @as(isize, @intCast(pal_height / 2))) {
                    self.fillPalTile(fields, first_field, origin, tile_row, start_x, end_x, tile_x, raster_width, field_rows, &real);
                    fft.forward3d(real[0..real_len], input[0..real_len], width, pal_height, depth);
                    const tile_confidence = if (self.use_lut) self.applyLut(&input, &filtered, pal_height) else self.applyPal(&input, &filtered);
                    fft.inverse3d(&filtered, real[0..real_len], width, pal_height, depth);
                    self.overlapAddPal(origin, output_field, tile_row, start_x, end_x, tile_x, field_rows, output, output_stride, confidence, confidence_stride, tile_confidence, &real);
                }
            } else {
                var tile_row: isize = -@as(isize, @intCast(height / 2));
                const frame_lines = field_rows * 2;
                while (tile_row < @as(isize, @intCast(frame_lines))) : (tile_row += @as(isize, @intCast(height / 2))) {
                    self.fillNtscTile(fields, first_field, origin, tile_row, parity, start_x, end_x, tile_x, raster_width, field_rows, &real);
                    fft.forward3d(real[0..real_len], input[0..real_len], width, height, depth);
                    const tile_confidence = if (self.use_lut) self.applyLut(&input, &filtered, height) else self.applyNtsc(&input, &filtered);
                    fft.inverse3d(&filtered, real[0..real_len], width, height, depth);
                    self.overlapAddNtsc(origin, output_field, tile_row, parity, start_x, end_x, tile_x, field_rows, output, output_stride, confidence, confidence_stride, tile_confidence, &real);
                }
            }
        }
    }

    fn findField(fields: []const FieldView, first_field: i32, field: i32) ?FieldView {
        if (field < first_field or field >= first_field + @as(i32, @intCast(fields.len))) return null;
        return fields[@intCast(field - first_field)];
    }

    fn fillPalTile(self: *const Transform3d, fields: []const FieldView, first_field: i32, origin: i32, tile_row: isize, start_x: usize, end_x: usize, tile_x: isize, raster_width: usize, field_rows: usize, real: []f32) void {
        _ = raster_width;
        for (0..depth) |z| {
            const absolute_field = origin + @as(i32, @intCast(z));
            const view = findField(fields, first_field, absolute_field);
            const shift = (z + 1) / 2;
            for (0..pal_height) |y| {
                const source_row = tile_row + @as(isize, @intCast(y)) - @as(isize, @intCast(shift));
                const valid = view != null and view.?.data != null and source_row >= 0 and source_row < @as(isize, @intCast(field_rows));
                const destination = real[(z * pal_height + y) * width ..][0..width];
                const window_row = self.window[z][y];
                const x0 = if (valid) start_x else width;
                const x1 = if (valid) end_x else width;
                for (0..x0) |x| destination[x] = 16384.0 * window_row[x];
                if (valid) {
                    const row = view.?.data.?[@as(usize, @intCast(source_row)) * view.?.stride ..];
                    for (x0..x1) |x| destination[x] = @as(f32, @floatFromInt(row[@as(usize, @intCast(tile_x + @as(isize, @intCast(x))))])) * window_row[x];
                }
                for (x1..width) |x| destination[x] = 16384.0 * window_row[x];
            }
        }
    }

    fn fillNtscTile(self: *const Transform3d, fields: []const FieldView, first_field: i32, origin: i32, tile_row: isize, parity: i32, start_x: usize, end_x: usize, tile_x: isize, raster_width: usize, field_rows: usize, real: []f32) void {
        _ = raster_width;
        for (0..depth) |z| {
            const absolute_field = origin + @as(i32, @intCast(z));
            const view = findField(fields, first_field, absolute_field);
            for (0..height) |y| {
                const frame_line = tile_row + @as(isize, @intCast(y));
                const valid = view != null and view.?.data != null and frame_line >= 0 and frame_line < @as(isize, @intCast(field_rows * 2)) and @mod(@as(i32, @intCast(frame_line)) + parity, 2) == @mod(absolute_field, 2);
                const destination = real[(z * height + y) * width ..][0..width];
                const window_row = self.window[z][y];
                const x0 = if (valid) start_x else width;
                const x1 = if (valid) end_x else width;
                for (0..x0) |x| destination[x] = 16384.0 * window_row[x];
                if (valid) {
                    const row = view.?.data.?[@as(usize, @intCast(@divTrunc(frame_line, 2))) * view.?.stride ..];
                    for (x0..x1) |x| destination[x] = @as(f32, @floatFromInt(row[@as(usize, @intCast(tile_x + @as(isize, @intCast(x))))])) * window_row[x];
                }
                for (x1..width) |x| destination[x] = 16384.0 * window_row[x];
            }
        }
    }

    fn overlapAddPal(self: *const Transform3d, origin: i32, output_field: i32, tile_row: isize, start_x: usize, end_x: usize, tile_x: isize, field_rows: usize, output: [2][]f32, output_stride: usize, confidence: ?[2][]f32, confidence_stride: usize, tile_confidence: f32, real: []const f32) void {
        for (0..depth) |z| {
            const absolute_field = origin + @as(i32, @intCast(z));
            const slot = absolute_field - output_field;
            if (slot < 0 or slot > 1) continue;
            const shift = (z + 1) / 2;
            for (0..pal_height) |y| {
                const row = tile_row + @as(isize, @intCast(y)) - @as(isize, @intCast(shift));
                if (row < 0 or row >= @as(isize, @intCast(field_rows))) continue;
                const destination = output[@intCast(slot)][@as(usize, @intCast(row)) * output_stride ..];
                for (start_x..end_x) |x| {
                    const column = @as(usize, @intCast(tile_x + @as(isize, @intCast(x))));
                    destination[column] += real[(z * pal_height + y) * width + x];
                    if (confidence) |maps| maps[@intCast(slot)][@as(usize, @intCast(row)) * confidence_stride + column] += tile_confidence * self.window[z][y][x];
                }
            }
        }
    }

    fn overlapAddNtsc(self: *const Transform3d, origin: i32, output_field: i32, tile_row: isize, parity: i32, start_x: usize, end_x: usize, tile_x: isize, field_rows: usize, output: [2][]f32, output_stride: usize, confidence: ?[2][]f32, confidence_stride: usize, tile_confidence: f32, real: []const f32) void {
        for (0..depth) |z| {
            const absolute_field = origin + @as(i32, @intCast(z));
            const slot = absolute_field - output_field;
            if (slot < 0 or slot > 1) continue;
            for (0..height) |y| {
                const frame_line = tile_row + @as(isize, @intCast(y));
                if (frame_line < 0 or frame_line >= @as(isize, @intCast(field_rows * 2))) continue;
                if (@mod(@as(i32, @intCast(frame_line)) + parity, 2) != @mod(absolute_field, 2)) continue;
                const row = @as(usize, @intCast(@divTrunc(frame_line, 2)));
                const destination = output[@intCast(slot)][row * output_stride ..];
                for (start_x..end_x) |x| {
                    const column = @as(usize, @intCast(tile_x + @as(isize, @intCast(x))));
                    destination[column] += real[(z * height + y) * width + x];
                    if (confidence) |maps| maps[@intCast(slot)][row * confidence_stride + column] += tile_confidence * self.window[z][y][x];
                }
            }
        }
    }

    fn applyPal(self: *const Transform3d, input: []const fft.Complex, output: []fft.Complex) f32 {
        @memset(output, .{});
        var numerator: f32 = 0.0;
        var denominator: f32 = 0.0;
        var bin: usize = 0;
        for (0..depth) |z| {
            const reflected_z = (depth - z) % depth;
            for (0..pal_height) |y| {
                const reflected_y = (pal_height / 2 + pal_height - y) % pal_height;
                for (width / 8..width / 4 + 1) |x| {
                    const reflected_x = width / 2 - x;
                    const source = input[(z * pal_height + y) * width + x];
                    const reflected = input[(reflected_z * pal_height + reflected_y) * width + reflected_x];
                    const source_energy = source.magnitudeSquared();
                    if (x == reflected_x and y == reflected_y and z == reflected_z) {
                        output[(z * pal_height + y) * width + x] = source;
                        numerator += source_energy;
                        denominator += source_energy;
                        bin += 1;
                        continue;
                    }
                    const reflected_energy = reflected.magnitudeSquared();
                    const low = @min(source_energy, reflected_energy);
                    const high = @max(source_energy, reflected_energy);
                    const ratio = if (high > 0.0) low / high else 1.0;
                    var evidence_gain: f32 = 1.0;
                    if (self.evidence > 0.0) {
                        var evidence_energy: f32 = 0.0;
                        for ([_]usize{ 12, 4 }) |carrier_y| {
                            const lf_y = (carrier_y + pal_height - y) % pal_height;
                            const lf_x = width / 4 - x;
                            evidence_energy = @max(evidence_energy, input[((depth - z) % depth * pal_height + lf_y) * width + lf_x].magnitudeSquared());
                        }
                        const evidence_denominator = evidence_energy + self.evidence * high;
                        if (evidence_denominator > 0.0) evidence_gain = evidence_energy / evidence_denominator;
                    }
                    if (self.level) {
                        var source_gain = evidence_gain;
                        var reflected_gain = evidence_gain;
                        if (source_energy > reflected_energy and source_energy > 0.0) source_gain *= @sqrt(reflected_energy / source_energy);
                        if (reflected_energy > source_energy and reflected_energy > 0.0) reflected_gain *= @sqrt(source_energy / reflected_energy);
                        output[(z * pal_height + y) * width + x] = source.scale(source_gain);
                        output[(reflected_z * pal_height + reflected_y) * width + reflected_x] = reflected.scale(reflected_gain);
                        const energy = 2.0 * low * evidence_gain * evidence_gain;
                        numerator += energy * ratio;
                        denominator += energy;
                    } else if (!(source_energy < reflected_energy * self.threshold_squared[bin] or reflected_energy < source_energy * self.threshold_squared[bin])) {
                        output[(z * pal_height + y) * width + x] = source.scale(evidence_gain);
                        output[(reflected_z * pal_height + reflected_y) * width + reflected_x] = reflected.scale(evidence_gain);
                        const energy = (source_energy + reflected_energy) * evidence_gain * evidence_gain;
                        numerator += energy * ratio;
                        denominator += energy;
                    }
                    bin += 1;
                }
            }
        }
        return if (denominator > 0.0) numerator / denominator else 0.0;
    }

    fn applyNtsc(self: *const Transform3d, input: []const fft.Complex, output: []fft.Complex) f32 {
        @memset(output, .{});
        var numerator: f32 = 0.0;
        var denominator: f32 = 0.0;
        var bin: usize = 0;
        for (0..depth) |z| {
            const reflected_z = (depth / 2 + depth - z) % depth;
            const low_z = (z + depth - depth / 4) % depth;
            const low_z_reflected = (depth - low_z) % depth;
            const kz0 = @as(f32, @floatFromInt(z)) / @as(f32, @floatFromInt(depth));
            for (0..height) |y| {
                const reflected_y = (height / 2 + height - y) % height;
                const low_y = (y + height - height / 4) % height;
                const low_y_reflected = (height - low_y) % height;
                const ky0 = @as(f32, @floatFromInt(y)) / @as(f32, @floatFromInt(height));
                var ky = ky0;
                var kz = kz0;
                if (kz0 + ky0 < 0.5) {
                    kz += 0.5;
                    ky += 0.5;
                } else if (kz0 + ky0 > 1.5) {
                    kz -= 0.5;
                    ky -= 0.5;
                } else if (kz0 - ky0 > 0.5) {
                    kz -= 0.5;
                    ky += 0.5;
                } else if (ky0 - kz0 > 0.5) {
                    kz += 0.5;
                    ky -= 0.5;
                }
                if (kz + ky > 1.0) {
                    kz = 1.0 - kz;
                    ky = 1.0 - ky;
                }
                for (width / 8..width / 4 + 1) |x| {
                    const reflected_x = width / 2 - x;
                    const low_x = @as(isize, @intCast(x)) - @as(isize, @intCast(width / 4));
                    const source_index = (z * height + y) * width + x;
                    const reflected_index = (reflected_z * height + reflected_y) * width + reflected_x;
                    const source = input[source_index];
                    const reflected = input[reflected_index];
                    if (x == reflected_x) {
                        const carrier = (y == height / 4 and z == depth / 4) or (y == 3 * (height / 4) and z == 3 * (depth / 4));
                        const discard = ((y == 0 or y == height / 2) and (z == 0 or z == depth / 2)) or (y == height / 4 and z == 3 * (depth / 4)) or (y == 3 * (height / 4) and z == depth / 4);
                        if (carrier) {
                            output[source_index] = source;
                            const energy = source.magnitudeSquared();
                            numerator += energy;
                            denominator += energy;
                            bin += 1;
                            continue;
                        }
                        if (discard) {
                            bin += 1;
                            continue;
                        }
                    }

                    const source_energy = source.magnitudeSquared();
                    const reflected_energy = reflected.magnitudeSquared();
                    const low = @min(source_energy, reflected_energy);
                    const high = @max(source_energy, reflected_energy);
                    const ratio = if (high > 0.0) low / high else 1.0;
                    const low_index_a: usize = if (low_x >= 0) (low_z * height + low_y) * width + @as(usize, @intCast(low_x)) else (low_z_reflected * height + low_y_reflected) * width + @as(usize, @intCast(-low_x));
                    const low_index_b: usize = if (low_x >= 0) (low_z_reflected * height + low_y_reflected) * width + (width / 2 - @as(usize, @intCast(low_x))) else (low_z * height + low_y) * width + @as(usize, @intCast(@as(isize, @intCast(width / 2)) + low_x));
                    const luma_energy = @max(input[low_index_a].magnitudeSquared(), input[low_index_b].magnitudeSquared());

                    if (self.level) {
                        if (high > 10.0 * luma_energy) {
                            bin += 1;
                            continue;
                        }
                        var source_gain: f32 = 1.0;
                        var reflected_gain: f32 = 1.0;
                        if (source_energy > 10.0 * reflected_energy and source_energy > 0.0) source_gain = @sqrt(reflected_energy / source_energy);
                        if (reflected_energy > 10.0 * source_energy and reflected_energy > 0.0) reflected_gain = @sqrt(source_energy / reflected_energy);
                        output[source_index] = source.scale(source_gain);
                        output[reflected_index] = reflected.scale(reflected_gain);
                        const energy = source_gain * source_gain * source_energy + reflected_gain * reflected_gain * reflected_energy;
                        numerator += energy * ratio;
                        denominator += energy;
                    } else {
                        const kx = @as(f32, @floatFromInt(x)) / @as(f32, @floatFromInt(width));
                        const luma_distance = distanceSquared(kz - 0.5, ky - 0.5, kx);
                        const chroma_distance = distanceSquared(kz - 0.25, ky - 0.25, kx - 0.25);
                        var threshold = std.math.pow(f32, chroma_distance / (luma_distance + chroma_distance), 10.0 * self.threshold_squared[bin]);
                        if (luma_energy < high * threshold) threshold = 0.5 * (1.0 + threshold);
                        if (!(source_energy < reflected_energy * threshold or reflected_energy < source_energy * threshold)) {
                            output[source_index] = source;
                            output[reflected_index] = reflected;
                            numerator += (source_energy + reflected_energy) * ratio;
                            denominator += source_energy + reflected_energy;
                        }
                    }
                    bin += 1;
                }
            }
        }
        return if (denominator > 0.0) numerator / denominator else 0.0;
    }

    fn applyLut(self: *const Transform3d, input: []const fft.Complex, output: []fft.Complex, tile_height: usize) f32 {
        @memset(output, .{});
        var numerator: f32 = 0.0;
        var denominator: f32 = 0.0;
        const bin_count = depth * tile_height * (width / 4 - width / 8 + 1);
        var bin: usize = 0;
        for (0..depth) |z| {
            const reflected_z = if (self.standard == .pal) (depth - z) % depth else (depth / 2 + depth - z) % depth;
            for (0..tile_height) |y| {
                const reflected_y = (tile_height / 2 + tile_height - y) % tile_height;
                for (width / 8..width / 4 + 1) |x| {
                    const reflected_x = width / 2 - x;
                    const source_index = (z * tile_height + y) * width + x;
                    const reflected_index = (reflected_z * tile_height + reflected_y) * width + reflected_x;
                    const source = input[source_index];
                    const reflected = input[reflected_index];
                    const source_energy = source.magnitudeSquared();
                    const reflected_energy = reflected.magnitudeSquared();
                    const low = @min(source_energy, reflected_energy);
                    const high = @max(source_energy, reflected_energy);
                    const ratio = if (high > 0.0) low / high else 1.0;
                    const gain = lutGain(self.lut[bin][0..], low, high);
                    output[source_index] = source.scale(gain);
                    output[reflected_index] = reflected.scale(gain);
                    const energy = gain * gain * (source_energy + reflected_energy);
                    numerator += energy * ratio;
                    denominator += energy;
                    bin += 1;
                }
            }
        }
        _ = bin_count;
        return if (denominator > 0.0) numerator / denominator else 0.0;
    }
};

fn raisedCosine(element: usize, limit: usize) f32 {
    const angle = 2.0 * std.math.pi * (@as(f64, @floatFromInt(element)) + 0.5) / @as(f64, @floatFromInt(limit));
    return @floatCast(0.5 - 0.5 * @cos(angle));
}

fn distanceSquared(a: f32, b: f32, c: f32) f32 {
    return a * a + b * b + c * c;
}

fn lutGain(row: []const f32, low: f32, high: f32) f32 {
    const ratio = if (high > 0.0) low / high else 1.0;
    const position = ratio * @as(f32, @floatFromInt(constants.lut_knots - 1));
    var index: usize = @intFromFloat(position);
    if (index > constants.lut_knots - 2) index = constants.lut_knots - 2;
    return row[index] + (row[index + 1] - row[index]) * (position - @as(f32, @floatFromInt(index)));
}

test "3D transform uses the standard-specific threshold count" {
    var pal = try Transform3d.init(.pal, 0.4, false, 0.0);
    var ntsc = try Transform3d.init(.ntsc, 0.4, false, 0.0);
    var pal_thresholds: [constants.Transform3d.pal_threshold_bins]f64 = [_]f64{0.4} ** constants.Transform3d.pal_threshold_bins;
    var ntsc_thresholds: [constants.Transform3d.threshold_bins]f64 = [_]f64{0.4} ** constants.Transform3d.threshold_bins;
    try pal.setThresholds(&pal_thresholds);
    try ntsc.setThresholds(&ntsc_thresholds);
}
