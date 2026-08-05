const std = @import("std");
const constants = @import("constants.zig");
const encode = @import("encode.zig");
const fir = @import("fir.zig");
const subcarrier = @import("subcarrier.zig");
const transform2d = @import("transform2d.zig");
const transform3d = @import("transform3d.zig");

const max_width = constants.Standard.pal.activeWidth();
const padded_width = max_width + 2 * constants.decode_filter_size;

pub const FrameView = struct {
    data: []const u16,
    stride: usize,
    frame_number: i32,
};

pub const FrameOutput = struct {
    y: []u16,
    y_stride: usize,
    u: []u16,
    u_stride: usize,
    v: []u16,
    v_stride: usize,
    rows: usize,
    row_offset: i32 = 0,
    original_y: ?[]const u16 = null,
    original_y_stride: usize = 0,
    mask: ?[]u16 = null,
    mask_stride: usize = 0,
    mask_kind: constants.MaskKind = .none,
    metrics: ?*constants.Metrics = null,
};

pub const Config = struct {
    standard: constants.Standard,
    setup: bool = false,
    dimensions: u8 = 2,
    eq: u8 = 0,
    refine: u16 = 0,
    use_transform: u8 = 0,
    level: bool = false,
    threshold: f64 = 0.4,
    evidence: f64 = 0.0,
    cti: bool = false,
    scratch_count: usize = 4,
};

const ConfidenceAccumulator = struct {
    sum: f64 = 0.0,
    sumsq: f64 = 0.0,
    count: usize = 0,

    fn add(self: *ConfidenceAccumulator, value: f32) void {
        const v = @as(f64, @floatCast(value));
        self.sum += v;
        self.sumsq += v * v;
        self.count += 1;
    }
};

const Scratch = struct {
    chroma_f: ?[]f32 = null,
    chroma: ?[]i16 = null,
    confidence: ?[]f32 = null,
    chroma_comb: ?[]i16 = null,
    mask: ?[]u8 = null,
    temporal: ?[]f32 = null,
    refine: ?[]u16 = null,

    fn deinit(self: *Scratch, allocator: std.mem.Allocator) void {
        if (self.chroma_f) |value| allocator.free(value);
        if (self.chroma) |value| allocator.free(value);
        if (self.confidence) |value| allocator.free(value);
        if (self.chroma_comb) |value| allocator.free(value);
        if (self.mask) |value| allocator.free(value);
        if (self.temporal) |value| allocator.free(value);
        if (self.refine) |value| allocator.free(value);
        self.* = .{};
    }
};

pub const Decoder = struct {
    pub const Error = error{
        InvalidConfiguration,
        OutOfMemory,
    };

    allocator: std.mem.Allocator,
    standard: constants.Standard,
    dimensions: u8,
    use_transform: u8,
    refine: u16,
    eq: u8,
    cti: bool,
    width: usize,
    height: usize,
    denominator: usize,
    ku: i32,
    kv: i32,
    level_black: i32,
    luma_numerator: i32,
    luma_denominator: i32,
    comb_range: i32,
    sine_q15: [2500]i16,
    colorlp_q15: [constants.color_lowpass_taps]i32,
    equalizer_q15: [constants.equalizer_taps]i32,
    narrow_q15: [constants.narrow_taps]i32,
    cfilt_q16: [constants.decode_filter_size + 1][4]i32,
    encoder: encode.Encoder,
    transform_2d: ?transform2d.Transform2d,
    transform_3d: ?transform3d.Transform3d,
    scratch: []Scratch,
    scratch_initialized: bool,
    used_scratch: std.atomic.Value(u64),

    pub fn init(allocator: std.mem.Allocator, config: Config) Error!Decoder {
        if (config.scratch_count == 0 or config.scratch_count > 64) return error.InvalidConfiguration;
        if (config.dimensions < 1 or config.dimensions > 3) return error.InvalidConfiguration;
        if (config.cti and config.dimensions < 2) return error.InvalidConfiguration;
        if (config.evidence < 0.0) return error.InvalidConfiguration;
        if (config.evidence > 0.0 and (config.standard != .pal or config.dimensions < 2)) return error.InvalidConfiguration;
        if (config.use_transform > 2) return error.InvalidConfiguration;
        if (config.use_transform != 0 and (config.standard != .ntsc or config.dimensions != 3)) return error.InvalidConfiguration;
        if (config.level and (config.dimensions < 2 or (config.standard == .ntsc and config.use_transform == 0))) return error.InvalidConfiguration;
        if (config.eq > 2) return error.InvalidConfiguration;
        if (config.eq == 2 and !(config.standard == .pal and config.dimensions >= 2 or config.use_transform != 0)) return error.InvalidConfiguration;
        if (config.threshold <= 0.0 or config.threshold > 1.0) return error.InvalidConfiguration;

        const encoder = encode.Encoder.init(config.standard, config.setup, false) catch return error.InvalidConfiguration;
        var result = Decoder{
            .allocator = allocator,
            .standard = config.standard,
            .dimensions = config.dimensions,
            .use_transform = config.use_transform,
            .refine = config.refine,
            .eq = config.eq,
            .cti = config.cti,
            .width = encoder.width,
            .height = config.standard.activeHeight(),
            .denominator = encoder.denominator,
            .ku = encoder.ku,
            .kv = encoder.kv,
            .level_black = encoder.level_black,
            .luma_numerator = encoder.luma_denominator,
            .luma_denominator = encoder.luma_numerator,
            .comb_range = 0,
            .sine_q15 = encoder.sine_q15,
            .colorlp_q15 = .{ 73, 317, 200, -682, -1597, -503, 3726, 9094, 11579, 9094, 3726, -503, -1597, -682, 200, 317, 73 },
            .equalizer_q15 = [_]i32{0} ** constants.equalizer_taps,
            .narrow_q15 = [_]i32{0} ** constants.narrow_taps,
            .cfilt_q16 = [_][4]i32{.{ 0, 0, 0, 0 }} ** (constants.decode_filter_size + 1),
            .encoder = encoder,
            .transform_2d = null,
            .transform_3d = null,
            .scratch = undefined,
            .scratch_initialized = false,
            .used_scratch = std.atomic.Value(u64).init(0),
        };
        errdefer result.deinit();

        if (config.standard == .ntsc) {
            result.comb_range = @intFromFloat(@round(45.0 * @as(f64, @floatFromInt(0xC800 - result.level_black)) / 100.0));
        }

        if (config.standard == .pal and config.dimensions == 2) {
            result.transform_2d = transform2d.Transform2d.init(config.threshold, config.level, config.evidence) catch return error.InvalidConfiguration;
        } else if (config.standard == .pal and config.dimensions == 3 or config.standard == .ntsc and config.use_transform != 0) {
            result.transform_3d = transform3d.Transform3d.init(config.standard, config.threshold, config.level, if (config.standard == .pal) config.evidence else 0.0) catch return error.InvalidConfiguration;
        }

        if (config.eq == 2) result.designNarrow();
        if (config.standard == .pal and config.dimensions > 1) {
            result.designPalChromaFilter();
            if (config.eq != 0) {
                const profile = result.palEqualizerProfile();
                result.designEqualizer(result.encoder.uv_taps, profile[0..]);
            }
        } else if (config.eq != 0) {
            const profile = result.colorlpProfile();
            result.designEqualizer(result.encoder.uv_taps, profile[0..]);
        }

        result.scratch = try allocator.alloc(Scratch, config.scratch_count);
        result.scratch_initialized = true;
        for (result.scratch) |*scratch| scratch.* = .{};
        const plane = result.width * result.height;
        for (result.scratch) |*scratch| {
            scratch.chroma_f = try allocator.alloc(f32, plane);
            scratch.chroma = try allocator.alloc(i16, plane);
            if (config.eq == 2) scratch.confidence = try allocator.alloc(f32, plane);
            if (config.standard == .ntsc and config.dimensions > 1) {
                const temporal_planes: usize = if (config.dimensions == 3) 6 else 1;
                scratch.temporal = try allocator.alloc(f32, plane * temporal_planes);
            }
            if (config.use_transform == 2) {
                scratch.chroma_comb = try allocator.alloc(i16, plane);
                scratch.mask = try allocator.alloc(u8, plane);
            }
            if (config.refine > 0) scratch.refine = try allocator.alloc(u16, plane * 6);
        }
        return result;
    }

    pub fn deinit(self: *Decoder) void {
        if (!self.scratch_initialized) return;
        for (self.scratch) |*scratch| scratch.deinit(self.allocator);
        self.allocator.free(self.scratch);
        self.scratch = undefined;
        self.scratch_initialized = false;
    }

    pub fn look(self: *const Decoder) usize {
        if (self.dimensions != 3) return 0;
        return if (self.standard == .pal or self.use_transform != 0) constants.Transform3d.look else 1;
    }

    pub fn setThresholds(self: *Decoder, values: []const f64) Error!void {
        if (self.standard != .pal and self.use_transform == 0) return error.InvalidConfiguration;
        if (self.dimensions == 2) {
            if (self.transform_2d) |*transform| {
                transform.setThresholds(values) catch return error.InvalidConfiguration;
                return;
            }
        }
        if (self.dimensions == 3) {
            if (self.transform_3d) |*transform| {
                transform.setThresholds(values) catch return error.InvalidConfiguration;
                return;
            }
        }
        return error.InvalidConfiguration;
    }

    pub fn setLut(self: *Decoder, values: []const f64) Error!void {
        if (self.standard != .pal and self.use_transform == 0) return error.InvalidConfiguration;
        if (self.dimensions == 2) {
            if (self.transform_2d) |*transform| {
                transform.setLut(values) catch return error.InvalidConfiguration;
                return;
            }
        }
        if (self.dimensions == 3) {
            if (self.transform_3d) |*transform| {
                transform.setLut(values) catch return error.InvalidConfiguration;
                return;
            }
        }
        return error.InvalidConfiguration;
    }

    pub fn decodeFrame(self: *Decoder, frame: i32, clip_frames: usize, views: []const FrameView, look_index: usize, output: FrameOutput) void {
        std.debug.assert(look_index < views.len);
        std.debug.assert(output.rows <= self.height);
        if (output.metrics) |metrics| metrics.* = .{};
        const scratch_index = self.acquireScratch();
        defer self.releaseScratch(scratch_index);
        const scratch = &self.scratch[scratch_index];
        const composite = views[look_index];
        const want_confidence_mask = output.mask != null and output.mask_kind == .confidence;
        var confidence_accumulator = ConfidenceAccumulator{};
        const collect_confidence = self.eq == 2 and (output.metrics != null or want_confidence_mask);

        if (self.dimensions == 1) {
            self.crudeDecodeFrame(frame, output.rows, output.row_offset, composite, output);
        } else if (self.standard == .pal) {
            const field_rows = output.rows / 2;
            if (self.dimensions == 2) {
                const separated = scratch.chroma_f.?;
                const confidence = scratch.confidence;
                for (0..2) |field| {
                    const base = field * composite.stride;
                    self.transform_2d.?.field(
                        composite.data[base..],
                        2 * composite.stride,
                        self.width,
                        field_rows,
                        separated[field * self.width ..],
                        2 * self.width,
                        if (confidence) |map| map[field * self.width ..] else null,
                        2 * self.width,
                    );
                }
                for (0..self.width * output.rows) |i| scratch.chroma.?[i] = constants.clampI16(@intFromFloat(@round(separated[i])));
            } else {
                self.decodeTransform3d(frame, clip_frames, views, look_index, 0, field_rows, scratch);
            }
            for (0..2) |field| {
                self.palDecodeField(frame, field, composite, scratch, output, if (scratch.confidence) |map| map[field * self.width ..] else null, if (collect_confidence) &confidence_accumulator else null);
            }
        } else {
            if (self.use_transform != 0) {
                const parity = @mod(output.row_offset + 1, 2);
                self.decodeTransform3d(frame, clip_frames, views, look_index, parity, output.rows / 2, scratch);
                if (self.use_transform == 2) self.decodeHybrid(output.rows, views, look_index, composite, scratch, output.row_offset);
            } else if (self.dimensions == 2) {
                self.ntscComb1d(output.rows, composite, scratch.chroma_f.?[0..]);
                self.ntscComb2d(output.rows, scratch.chroma_f.?[0..], scratch.temporal.?[0..]);
                for (0..self.width * output.rows) |i| scratch.chroma.?[i] = constants.clampI16(@intFromFloat(@round(scratch.temporal.?[i])));
            } else {
                self.ntscComb3d(output.rows, views, look_index, scratch, output.row_offset);
            }
            for (0..output.rows) |row| {
                self.ntscDemodLine(frame, @as(usize, @intCast(@as(i32, @intCast(row)) + output.row_offset)), row, composite.data[row * composite.stride ..], scratch.chroma.?[row * self.width ..], if (scratch.confidence) |map| map[row * self.width ..] else null, if (collect_confidence) &confidence_accumulator else null, if (self.use_transform == 2) scratch.mask.?[row * self.width ..] else null, output);
            }
            if (output.mask != null and output.mask_kind == .motion and self.use_transform == 2) self.emitMotionMask(output, scratch, &confidence_accumulator);
        }

        if (self.refine > 0 and output.original_y != null) self.refineLuma(frame, composite, scratch, output);
        if (output.metrics) |metrics| {
            if (confidence_accumulator.count > 0) {
                const mean = confidence_accumulator.sum / @as(f64, @floatFromInt(confidence_accumulator.count));
                var variance = confidence_accumulator.sumsq / @as(f64, @floatFromInt(confidence_accumulator.count)) - mean * mean;
                if (variance < 0.0) variance = 0.0;
                metrics.separation_confidence_mean = mean;
                metrics.separation_confidence_stddev = @sqrt(variance);
            }
        }
    }

    fn acquireScratch(self: *Decoder) usize {
        while (true) {
            const current = self.used_scratch.load(.acquire);
            for (0..self.scratch.len) |index| {
                const bit = @as(u64, 1) << @intCast(index);
                if (current & bit != 0) continue;
                if (self.used_scratch.cmpxchgWeak(current, current | bit, .acquire, .monotonic) == null) return index;
            }
            std.atomic.spinLoopHint();
            _ = std.Thread.yield() catch {};
        }
    }

    fn releaseScratch(self: *Decoder, index: usize) void {
        const bit = @as(u64, 1) << @intCast(index);
        _ = self.used_scratch.fetchAnd(~bit, .release);
    }

    fn designNarrow(self: *Decoder) void {
        const sample_rate: f64 = if (self.standard == .pal) 4.0 * 4433618.75 else 4.0 * 3579545.0;
        const cutoff = 600000.0 / sample_rate;
        const centre = constants.narrow_taps / 2;
        var taps: [constants.narrow_taps]f64 = undefined;
        var sum: f64 = 0.0;
        for (0..constants.narrow_taps) |index| {
            const k = @as(f64, @floatFromInt(index)) - @as(f64, @floatFromInt(centre));
            const sinc = if (k == 0.0) 2.0 * cutoff else @sin(2.0 * std.math.pi * cutoff * k) / (std.math.pi * k);
            taps[index] = sinc * (0.5 + 0.5 * @cos(std.math.pi * k / @as(f64, @floatFromInt(centre + 1))));
            sum += taps[index];
        }
        var total: i32 = 0;
        for (taps, 0..) |tap, index| {
            self.narrow_q15[index] = @intFromFloat(@round(tap / sum * 32768.0));
            total += self.narrow_q15[index];
        }
        self.narrow_q15[centre] += 32768 - total;
    }

    fn designPalChromaFilter(self: *Decoder) void {
        const fs_hz = 4.0 * 4433618.75;
        const bandwidth = 1100000.0 / 0.93;
        const ca = 0.5 * fs_hz / bandwidth;
        var divisor: f64 = 0.0;
        var filter: [constants.decode_filter_size + 1][4]f64 = undefined;
        for (0..constants.decode_filter_size + 1) |f| {
            const fd = @as(f64, @floatFromInt(f));
            const fc = @min(ca, fd);
            const ff = @min(ca, @sqrt(fd * fd + 4.0));
            const fff = @min(ca, @sqrt(fd * fd + 16.0));
            const ffff = @min(ca, @sqrt(fd * fd + 36.0));
            const div: f64 = if (f == 0) 2.0 else 1.0;
            filter[f][0] = (1.0 + @cos(std.math.pi * fc / ca)) / div;
            filter[f][2] = (1.0 + @cos(std.math.pi * ff / ca)) / div;
            filter[f][1] = (1.0 + @cos(std.math.pi * fff / ca)) / div;
            filter[f][3] = (1.0 + @cos(std.math.pi * ffff / ca)) / div;
            divisor += 2.0 * (filter[f][0] + 2.0 * filter[f][2] + 2.0 * filter[f][1] + 2.0 * filter[f][3]);
        }
        for (filter, 0..) |row, f| {
            for (row, 0..) |value, k| {
                self.cfilt_q16[f][k] = @intFromFloat(@round(value / divisor * 65536.0));
            }
        }
    }

    fn designEqualizer(self: *Decoder, encoder_taps: []const i16, decoder_taps: []const f64) void {
        const frequency_samples = 256;
        const centre = constants.equalizer_taps / 2;
        var inverse: [frequency_samples / 2 + 1]f64 = undefined;
        for (0..frequency_samples / 2 + 1) |sample| {
            const omega = 2.0 * std.math.pi * @as(f64, @floatFromInt(sample)) / frequency_samples;
            var decoder_response: f64 = 0.0;
            for (decoder_taps, 0..) |tap, index| decoder_response += tap * @cos(omega * (@as(f64, @floatFromInt(index)) - @as(f64, @floatFromInt(decoder_taps.len - 1)) / 2.0));
            var encoder_response: f64 = 0.0;
            for (encoder_taps, 0..) |tap, index| encoder_response += @as(f64, @floatFromInt(tap)) / 32768.0 * @cos(omega * (@as(f64, @floatFromInt(index)) - @as(f64, @floatFromInt(encoder_taps.len - 1)) / 2.0));
            const cascade = encoder_response * decoder_response;
            inverse[sample] = if (cascade > 0.25) @min(4.0, 1.0 / cascade) else 4.0;
        }

        var taps: [constants.equalizer_taps]f64 = undefined;
        var dc: f64 = 0.0;
        for (0..constants.equalizer_taps) |index| {
            const offset = @as(f64, @floatFromInt(index)) - @as(f64, @floatFromInt(centre));
            var sum = inverse[0];
            for (1..frequency_samples / 2) |sample| sum += 2.0 * inverse[sample] * @cos(2.0 * std.math.pi * @as(f64, @floatFromInt(sample)) * offset / frequency_samples);
            sum += inverse[frequency_samples / 2] * @cos(std.math.pi * offset);
            const hann = 0.5 + 0.5 * @cos(std.math.pi * offset / @as(f64, @floatFromInt(centre + 1)));
            taps[index] = sum / frequency_samples * hann;
            dc += taps[index];
        }
        for (taps, 0..) |tap, index| self.equalizer_q15[index] = @intFromFloat(@round(tap * (inverse[0] / dc) * 32768.0));
    }

    fn palEqualizerProfile(self: *const Decoder) [2 * constants.decode_filter_size + 1]f64 {
        const divisor = self.palFilterDivisor();
        var profile: [2 * constants.decode_filter_size + 1]f64 = undefined;
        for (0..2 * constants.decode_filter_size + 1) |offset_index| {
            const offset = @as(isize, @intCast(offset_index)) - @as(isize, @intCast(constants.decode_filter_size));
            const absolute = @as(usize, @intCast(if (offset < 0) -offset else offset));
            const value = self.cfilt_q16[absolute][0] + 2 * self.cfilt_q16[absolute][1] + 2 * self.cfilt_q16[absolute][2] + 2 * self.cfilt_q16[absolute][3];
            const centre_gain: f64 = if (offset == 0) 2.0 else 1.0;
            profile[@as(usize, @intCast(offset + constants.decode_filter_size))] = @as(f64, @floatFromInt(value)) / divisor * centre_gain;
        }
        return profile;
    }

    fn palFilterDivisor(self: *const Decoder) f64 {
        _ = self;
        const fs_hz = 4.0 * 4433618.75;
        const bandwidth = 1100000.0 / 0.93;
        const ca = 0.5 * fs_hz / bandwidth;
        var divisor: f64 = 0.0;
        for (0..constants.decode_filter_size + 1) |f| {
            const fd = @as(f64, @floatFromInt(f));
            const fc = @min(ca, fd);
            const ff = @min(ca, @sqrt(fd * fd + 4.0));
            const fff = @min(ca, @sqrt(fd * fd + 16.0));
            const ffff = @min(ca, @sqrt(fd * fd + 36.0));
            const div: f64 = if (f == 0) 2.0 else 1.0;
            const c0 = (1.0 + @cos(std.math.pi * fc / ca)) / div;
            const c2 = (1.0 + @cos(std.math.pi * ff / ca)) / div;
            const c1 = (1.0 + @cos(std.math.pi * fff / ca)) / div;
            const c3 = (1.0 + @cos(std.math.pi * ffff / ca)) / div;
            divisor += 2.0 * (c0 + 2.0 * c2 + 2.0 * c1 + 2.0 * c3);
        }
        return divisor;
    }

    fn colorlpProfile(self: *const Decoder) [constants.color_lowpass_taps]f64 {
        _ = self;
        const taps = [_]i32{ 73, 317, 200, -682, -1597, -503, 3726, 9094, 11579, 9094, 3726, -503, -1597, -682, 200, 317, 73 };
        var profile: [constants.color_lowpass_taps]f64 = undefined;
        for (0..profile.len) |index| profile[index] = @as(f64, @floatFromInt(taps[index])) / 32768.0;
        return profile;
    }

    fn decodeTransform3d(self: *const Decoder, frame: i32, clip_frames: usize, views: []const FrameView, look_index: usize, parity: i32, field_rows: usize, scratch: *const Scratch) void {
        const depth_half = constants.Transform3d.tile_depth / 2;
        const frame_field = frame * 2;
        const high_origin = @divTrunc(frame_field + 1, @as(i32, @intCast(depth_half))) * @as(i32, @intCast(depth_half));
        const first_field = high_origin - @as(i32, @intCast(depth_half));
        const field_count = constants.Transform3d.tile_depth + depth_half;
        var fields: [field_count]transform3d.FieldView = undefined;

        for (0..field_count) |index| {
            const absolute_field = first_field + @as(i32, @intCast(index));
            if (absolute_field < 0 or absolute_field >= @as(i32, @intCast(2 * clip_frames))) {
                fields[index] = .{ .data = null, .stride = 0 };
                continue;
            }
            var view_index = @divTrunc(absolute_field, 2) - (frame - @as(i32, @intCast(look_index)));
            view_index = @max(0, @min(view_index, @as(i32, @intCast(views.len - 1))));
            const view = views[@intCast(view_index)];
            const field_offset: usize = if (self.standard == .pal) @intCast(@mod(absolute_field, 2)) else @intCast(@mod(absolute_field ^ parity, 2));
            fields[index] = .{
                .data = view.data[field_offset * view.stride ..],
                .stride = 2 * view.stride,
            };
        }

        const separated = scratch.chroma_f.?;
        const confidence = scratch.confidence;
        self.transform_3d.?.frame(
            fields[0..],
            first_field,
            frame,
            parity,
            self.width,
            field_rows,
            .{ separated[0..], separated[self.width..] },
            2 * self.width,
            if (confidence) |map| .{ map[0..], map[self.width..] } else null,
            2 * self.width,
        );
        const chroma = scratch.chroma.?;
        for (0..self.width * field_rows * 2) |index| chroma[index] = roundFloatToI16(separated[index]);
    }

    fn palDecodeField(self: *const Decoder, frame: i32, field: usize, composite: FrameView, scratch: *const Scratch, output: FrameOutput, confidence: ?[]const f32, accumulator: ?*ConfidenceAccumulator) void {
        const field_rows = output.rows / 2;
        const chroma = scratch.chroma.?[field * self.width ..];
        const zero = [_]i16{0} ** max_width;
        var m: [4][padded_width]i32 = undefined;
        var n: [4][padded_width]i32 = undefined;
        var u_row: [max_width]i32 = undefined;
        var v_row: [max_width]i32 = undefined;
        var u_eq: [max_width]i32 = undefined;
        var v_eq: [max_width]i32 = undefined;
        var u_narrow: [max_width]i32 = undefined;
        var v_narrow: [max_width]i32 = undefined;

        for (0..field_rows) |field_row| {
            const in0 = chroma[field_row * 2 * self.width ..];
            const in1 = if (field_row > 0) chroma[(field_row - 1) * 2 * self.width ..] else zero[0..];
            const in2 = if (field_row + 1 < field_rows) chroma[(field_row + 1) * 2 * self.width ..] else zero[0..];
            const in3 = if (field_row >= 2) chroma[(field_row - 2) * 2 * self.width ..] else zero[0..];
            const in4 = if (field_row + 2 < field_rows) chroma[(field_row + 2) * 2 * self.width ..] else zero[0..];
            const in5 = if (field_row >= 3) chroma[(field_row - 3) * 2 * self.width ..] else zero[0..];
            const in6 = if (field_row + 3 < field_rows) chroma[(field_row + 3) * 2 * self.width ..] else zero[0..];
            for (&m) |*row| @memset(row, @as(i32, 0));
            for (&n) |*row| @memset(row, @as(i32, 0));
            for (0..self.width) |x| {
                const sine_sign: i32 = switch (x & 3) {
                    1 => 1,
                    3 => -1,
                    else => 0,
                };
                const cosine_sign: i32 = switch (x & 3) {
                    0 => 1,
                    2 => -1,
                    else => 0,
                };
                const index = constants.decode_filter_size + x;
                m[0][index] = @as(i32, in0[x]) * sine_sign;
                m[2][index] = (@as(i32, in1[x]) - @as(i32, in2[x])) * sine_sign;
                m[1][index] = (-@as(i32, in3[x]) - @as(i32, in4[x])) * sine_sign;
                m[3][index] = (-@as(i32, in5[x]) + @as(i32, in6[x])) * sine_sign;
                n[0][index] = @as(i32, in0[x]) * cosine_sign;
                n[2][index] = (@as(i32, in1[x]) - @as(i32, in2[x])) * cosine_sign;
                n[1][index] = (-@as(i32, in3[x]) - @as(i32, in4[x])) * cosine_sign;
                n[3][index] = (-@as(i32, in5[x]) + @as(i32, in6[x])) * cosine_sign;
            }

            const raster_row = @as(i32, @intCast(field_row * 2 + field)) + output.row_offset;
            const phase = subcarrier.line(self.standard, frame, raster_row);
            const bp = -@as(i32, self.sine_q15[(phase.phase + self.denominator / 4) % self.denominator]);
            const bq = -@as(i32, self.sine_q15[phase.phase]);
            palDemodRow(self, m[0..], n[0..], u_row[0..], v_row[0..], bp, bq, phase.v_switch, self.width);

            const comp_row = composite.data[(field_row * 2 + field) * composite.stride ..];
            const out_row = field_row * 2 + field;
            const y_out = output.y[out_row * output.y_stride ..];
            const u_out = output.u[out_row * output.u_stride ..];
            const v_out = output.v[out_row * output.v_stride ..];
            for (0..self.width) |x| {
                const luma = @as(i32, comp_row[x]) - @as(i32, in0[x]);
                y_out[x] = constants.clampU16(4096 + constants.roundDivSigned(@as(i64, luma - self.level_black) * self.luma_numerator, self.luma_denominator));
            }

            var u_final = u_row[0..self.width];
            var v_final = v_row[0..self.width];
            if (self.eq != 0) {
                eqRow(self, u_row[0..], u_eq[0..], self.width);
                eqRow(self, v_row[0..], v_eq[0..], self.width);
                if (self.eq == 2 and confidence != null) {
                    narrowRow(self, u_row[0..], u_narrow[0..], self.width);
                    narrowRow(self, v_row[0..], v_narrow[0..], self.width);
                    const confidence_row = confidence.?[field_row * 2 * self.width ..];
                    for (0..self.width) |x| {
                        const c = confidence_row[x];
                        if (accumulator) |acc| acc.add(c);
                        const confidence_clamped = std.math.clamp(c, 0.0, 1.0);
                        var weight: i32 = @intFromFloat(@round(confidence_clamped * confidence_clamped * confidence_clamped * confidence_clamped * 32768.0));
                        weight = std.math.clamp(weight, 0, 32768);
                        u_eq[x] = u_narrow[x] + @as(i32, @intCast(@divTrunc(@as(i64, weight) * (u_eq[x] - u_narrow[x]), 1 << 15)));
                        v_eq[x] = v_narrow[x] + @as(i32, @intCast(@divTrunc(@as(i64, weight) * (v_eq[x] - v_narrow[x]), 1 << 15)));
                        if (output.mask) |mask| {
                            if (output.mask_kind == .confidence) mask[out_row * output.mask_stride + x] = @intFromFloat(@round((1.0 - confidence_clamped) * 65535.0));
                        }
                    }
                }
                u_final = u_eq[0..self.width];
                v_final = v_eq[0..self.width];
            }

            if (self.cti) ctiRow(y_out[0..], u_final, v_final, self.width);
            for (0..self.width) |x| {
                u_out[x] = constants.clampU16(32768 + constants.roundDivSigned(@as(i64, u_final[x]) * 32768, self.ku));
                v_out[x] = constants.clampU16(32768 + constants.roundDivSigned(@as(i64, v_final[x]) * 32768, self.kv));
            }
        }
    }

    fn crudeDecodeFrame(self: *const Decoder, frame: i32, rows: usize, row_offset: i32, composite: FrameView, output: FrameOutput) void {
        var estimate: [max_width]i32 = undefined;
        var m: [padded_width]i32 = [_]i32{0} ** padded_width;
        var n: [padded_width]i32 = [_]i32{0} ** padded_width;
        var p: [max_width]i32 = undefined;
        var q: [max_width]i32 = undefined;
        var u: [max_width]i32 = undefined;
        var v: [max_width]i32 = undefined;
        var u_eq: [max_width]i32 = undefined;
        var v_eq: [max_width]i32 = undefined;

        for (0..rows) |row| {
            const line = composite.data[row * composite.stride ..];
            estimate[0] = 0;
            estimate[1] = 0;
            estimate[self.width - 2] = 0;
            estimate[self.width - 1] = 0;
            for (2..self.width - 2) |x| estimate[x] = @divTrunc(2 * @as(i32, line[x]) - @as(i32, line[x - 2]) - @as(i32, line[x + 2]), 4);
            @memset(&m, @as(i32, 0));
            @memset(&n, @as(i32, 0));
            for (0..self.width) |x| {
                const sine_sign: i32 = switch (x & 3) {
                    1 => 1,
                    3 => -1,
                    else => 0,
                };
                const cosine_sign: i32 = switch (x & 3) {
                    0 => 1,
                    2 => -1,
                    else => 0,
                };
                m[constants.color_lowpass_taps / 2 + x] = estimate[x] * sine_sign;
                n[constants.color_lowpass_taps / 2 + x] = estimate[x] * cosine_sign;
            }
            const phase = subcarrier.line(self.standard, frame, row_offset + @as(i32, @intCast(row)));
            const sine = @as(i32, self.sine_q15[phase.phase]);
            const cosine = @as(i32, self.sine_q15[(phase.phase + self.denominator / 4) % self.denominator]);
            firRowStaged(self.colorlp_q15[0..], m[0..], p[0..], self.width);
            firRowStaged(self.colorlp_q15[0..], n[0..], q[0..], self.width);
            const y_out = output.y[row * output.y_stride ..];
            const u_out = output.u[row * output.u_stride ..];
            const v_out = output.v[row * output.v_stride ..];
            for (0..self.width) |x| {
                u[x] = @intCast((@as(i64, p[x]) * cosine + @as(i64, q[x]) * sine + 8192) >> 14);
                v[x] = phase.v_switch * @as(i32, @intCast((@as(i64, q[x]) * cosine - @as(i64, p[x]) * sine + 8192) >> 14));
                const luma = @as(i32, line[x]) - estimate[x];
                y_out[x] = constants.clampU16(4096 + constants.roundDivSigned(@as(i64, luma - self.level_black) * self.luma_numerator, self.luma_denominator));
            }
            var u_final = u[0..self.width];
            var v_final = v[0..self.width];
            if (self.eq != 0) {
                eqRow(self, u[0..], u_eq[0..], self.width);
                eqRow(self, v[0..], v_eq[0..], self.width);
                u_final = u_eq[0..self.width];
                v_final = v_eq[0..self.width];
            }
            for (0..self.width) |x| {
                u_out[x] = constants.clampU16(32768 + constants.roundDivSigned(@as(i64, u_final[x]) * 32768, self.ku));
                v_out[x] = constants.clampU16(32768 + constants.roundDivSigned(@as(i64, v_final[x]) * 32768, self.kv));
            }
        }
    }

    fn ntscComb1d(self: *const Decoder, rows: usize, composite: FrameView, output: []f32) void {
        for (0..rows) |row| {
            const line = composite.data[row * composite.stride ..];
            const destination = output[row * self.width ..];
            destination[0] = 0.0;
            destination[1] = 0.0;
            destination[self.width - 2] = 0.0;
            destination[self.width - 1] = 0.0;
            for (2..self.width - 2) |x| destination[x] = (2.0 * @as(f32, @floatFromInt(line[x])) - @as(f32, @floatFromInt(line[x - 2])) - @as(f32, @floatFromInt(line[x + 2]))) / 4.0;
        }
    }

    fn ntscComb2d(self: *const Decoder, rows: usize, first: []const f32, second: []f32) void {
        var zero = [_]f32{0.0} ** max_width;
        for (0..rows) |row| {
            const current = first[row * self.width ..];
            const previous = if (row >= 2) first[(row - 2) * self.width ..] else zero[0..];
            const next = if (row + 2 < rows) first[(row + 2) * self.width ..] else zero[0..];
            const destination = second[row * self.width ..];
            destination[0] = 0.0;
            for (1..self.width) |x| {
                var previous_score = @abs(@abs(current[x]) - @abs(previous[x])) + @abs(@abs(current[x - 1]) - @abs(previous[x - 1])) - (@abs(current[x]) + @abs(previous[x - 1])) * 0.10;
                var next_score = @abs(@abs(current[x]) - @abs(next[x])) + @abs(@abs(current[x - 1]) - @abs(next[x - 1])) - (@abs(current[x]) + @abs(next[x - 1])) * 0.10;
                previous_score = std.math.clamp(1.0 - previous_score / @as(f32, @floatFromInt(self.comb_range)), 0.0, 1.0);
                next_score = std.math.clamp(1.0 - next_score / @as(f32, @floatFromInt(self.comb_range)), 0.0, 1.0);
                var scale: f32 = 1.0;
                if (next_score > 0.0 or previous_score > 0.0) {
                    if (next_score > 3.0 * previous_score) previous_score = 0.0 else if (previous_score > 3.0 * next_score) next_score = 0.0;
                    scale = @max(1.0, 2.0 / (next_score + previous_score));
                } else if (@abs(@abs(previous[x]) - @abs(next[x])) - @abs((next[x] + previous[x]) * 0.2) <= 0.0) {
                    previous_score = 1.0;
                    next_score = 1.0;
                }
                destination[x] = ((current[x] - previous[x]) * previous_score * scale + (current[x] - next[x]) * next_score * scale) / 4.0;
            }
        }
    }

    fn ntscDemodLine(self: *const Decoder, frame: i32, raster_row: usize, output_row: usize, composite: []const u16, chroma: []const i16, confidence: ?[]const f32, accumulator: ?*ConfidenceAccumulator, motion_mask: ?[]const u8, output: FrameOutput) void {
        var m: [padded_width]i32 = [_]i32{0} ** padded_width;
        var n: [padded_width]i32 = [_]i32{0} ** padded_width;
        var p: [max_width]i32 = undefined;
        var q: [max_width]i32 = undefined;
        var u: [max_width]i32 = undefined;
        var v: [max_width]i32 = undefined;
        var u_eq: [max_width]i32 = undefined;
        var v_eq: [max_width]i32 = undefined;
        var u_narrow: [max_width]i32 = undefined;
        var v_narrow: [max_width]i32 = undefined;
        const half = constants.color_lowpass_taps / 2;
        for (0..self.width) |x| {
            const sine_sign: i32 = switch (x & 3) {
                1 => 1,
                3 => -1,
                else => 0,
            };
            const cosine_sign: i32 = switch (x & 3) {
                0 => 1,
                2 => -1,
                else => 0,
            };
            m[half + x] = @as(i32, chroma[x]) * sine_sign;
            n[half + x] = @as(i32, chroma[x]) * cosine_sign;
        }
        const phase = subcarrier.line(self.standard, frame, @intCast(raster_row));
        const sine = @as(i32, self.sine_q15[phase.phase]);
        const cosine = @as(i32, self.sine_q15[(phase.phase + self.denominator / 4) % self.denominator]);
        const bp = -cosine;
        const bq = -sine;
        const s4 = [_]i32{ sine, cosine, -sine, -cosine };
        const c4 = [_]i32{ cosine, -sine, -cosine, sine };
        firRowStaged(self.colorlp_q15[0..], m[0..], p[0..], self.width);
        firRowStaged(self.colorlp_q15[0..], n[0..], q[0..], self.width);
        for (0..self.width) |x| {
            u[x] = @intCast((-(@as(i64, p[x]) * bp + @as(i64, q[x]) * bq + 8192)) >> 14);
            v[x] = @intCast((-(@as(i64, q[x]) * bp - @as(i64, p[x]) * bq + 8192)) >> 14);
        }

        const y_out = output.y[output_row * output.y_stride ..];
        const u_out = output.u[output_row * output.u_stride ..];
        const v_out = output.v[output_row * output.v_stride ..];
        var luma: [max_width]i32 = undefined;
        if (self.use_transform == 1) {
            for (0..self.width) |x| luma[x] = @as(i32, composite[x]) - @as(i32, chroma[x]);
        } else {
            for (0..self.width) |x| {
                const reconstructed: i32 = @intCast((@as(i64, u[x]) * s4[x & 3] + @as(i64, v[x]) * c4[x & 3] + 16384) >> 15);
                if (self.use_transform == 2 and motion_mask != null) {
                    const difference = reconstructed - @as(i32, chroma[x]);
                    luma[x] = @as(i32, composite[x]) - @as(i32, chroma[x]) - if (motion_mask.?[x] != 0) difference else 0;
                } else {
                    luma[x] = @as(i32, composite[x]) - reconstructed;
                }
            }
        }
        for (0..self.width) |x| y_out[x] = constants.clampU16(4096 + constants.roundDivSigned(@as(i64, luma[x] - self.level_black) * self.luma_numerator, self.luma_denominator));

        var u_final = u[0..self.width];
        var v_final = v[0..self.width];
        if (self.eq != 0) {
            eqRow(self, u[0..], u_eq[0..], self.width);
            eqRow(self, v[0..], v_eq[0..], self.width);
            if (self.eq == 2 and confidence != null) {
                narrowRow(self, u[0..], u_narrow[0..], self.width);
                narrowRow(self, v[0..], v_narrow[0..], self.width);
                for (0..self.width) |x| {
                    const c = confidence.?[x];
                    if (accumulator) |acc| acc.add(c);
                    const clamped = std.math.clamp(c, 0.0, 1.0);
                    var weight: i32 = @intFromFloat(@round(clamped * clamped * clamped * clamped * 32768.0));
                    weight = std.math.clamp(weight, 0, 32768);
                    u_eq[x] = u_narrow[x] + @as(i32, @intCast(@divTrunc(@as(i64, weight) * (u_eq[x] - u_narrow[x]), 1 << 15)));
                    v_eq[x] = v_narrow[x] + @as(i32, @intCast(@divTrunc(@as(i64, weight) * (v_eq[x] - v_narrow[x]), 1 << 15)));
                    if (output.mask) |mask| {
                        if (output.mask_kind == .confidence) mask[output_row * output.mask_stride + x] = @intFromFloat(@round((1.0 - clamped) * 65535.0));
                    }
                }
            }
            u_final = u_eq[0..self.width];
            v_final = v_eq[0..self.width];
        }
        if (self.cti) ctiRow(y_out[0..], u_final, v_final, self.width);
        for (0..self.width) |x| {
            u_out[x] = constants.clampU16(32768 + constants.roundDivSigned(@as(i64, u_final[x]) * 32768, self.ku));
            v_out[x] = constants.clampU16(32768 + constants.roundDivSigned(@as(i64, v_final[x]) * 32768, self.kv));
        }
    }

    fn ntscComb3d(self: *const Decoder, rows: usize, views: []const FrameView, look_index: usize, scratch: *const Scratch, row_offset: i32) void {
        const plane = self.width * self.height;
        const temporal = scratch.temporal.?;
        var c1: [3][]const f32 = undefined;
        var c2: [3][]const f32 = undefined;
        for (0..3) |index| {
            const first = temporal[index * plane ..];
            const second = temporal[(3 + index) * plane ..];
            self.ntscComb1d(rows, views[look_index - 1 + index], first);
            self.ntscComb2d(rows, first, second);
            c1[index] = first;
            c2[index] = second;
        }
        ntscSplit3d(self, rows, row_offset, views[look_index - 1 .. look_index + 2], c1, c2, scratch.chroma.?);
    }

    fn decodeHybrid(self: *const Decoder, rows: usize, views: []const FrameView, look_index: usize, composite: FrameView, scratch: *const Scratch, row_offset: i32) void {
        self.ntscComb3d(rows, views, look_index, scratch, row_offset);
        const chroma = scratch.chroma.?;
        const comb = scratch.chroma_comb.?;
        const mask = scratch.mask.?;
        const previous = views[look_index - 2];
        const next = views[look_index + 2];
        var differences: [max_width]i32 = undefined;
        const margin = @divTrunc(self.comb_range, 16);
        for (0..rows) |row| {
            const current = composite.data[row * composite.stride ..];
            const previous_row = previous.data[row * previous.stride ..];
            const next_row = next.data[row * next.stride ..];
            for (0..self.width) |x| {
                const a = @abs(@as(i32, current[x]) - @as(i32, previous_row[x]));
                const b = @abs(@as(i32, current[x]) - @as(i32, next_row[x]));
                differences[x] = @intCast(@max(a, b));
            }
            for (2..self.width - 2) |x| {
                var maximum = differences[x - 2];
                maximum = @max(maximum, differences[x - 1]);
                maximum = @max(maximum, differences[x]);
                maximum = @max(maximum, differences[x + 1]);
                maximum = @max(maximum, differences[x + 2]);
                mask[row * self.width + x] = @intFromBool(maximum < margin);
            }
            for (0..4) |edge| {
                const x = if (edge < 2) edge else self.width - 4 + edge;
                const low = if (x < 2) 0 else x - 2;
                const high = if (x >= self.width - 2) self.width - 1 else x + 2;
                var maximum: i32 = 0;
                for (low..high + 1) |index| maximum = @max(maximum, differences[index]);
                mask[row * self.width + x] = @intFromBool(maximum < margin);
            }
            for (0..self.width) |x| {
                const index = row * self.width + x;
                if (mask[index] != 0) chroma[index] = comb[index];
            }
        }
    }

    fn refineLuma(self: *const Decoder, frame: i32, composite: FrameView, scratch: *const Scratch, output: FrameOutput) void {
        const original = output.original_y.?;
        const refine = scratch.refine.?;
        const plane = self.width * self.height;
        const estimate_y = refine[0..plane];
        const estimate_u = refine[plane .. 2 * plane];
        const estimate_v = refine[2 * plane .. 3 * plane];
        const recomposite = refine[3 * plane .. 4 * plane];
        const crude_y = refine[4 * plane .. 5 * plane];
        const crude_u = refine[5 * plane .. 6 * plane];

        for (0..output.rows) |row| {
            @memcpy(estimate_y[row * self.width ..][0..self.width], output.y[row * output.y_stride ..][0..self.width]);
            @memcpy(estimate_u[row * self.width ..][0..self.width], output.u[row * output.u_stride ..][0..self.width]);
            @memcpy(estimate_v[row * self.width ..][0..self.width], output.v[row * output.v_stride ..][0..self.width]);
        }
        var residual_sum: f64 = 0.0;
        for (0..self.refine) |iteration| {
            residual_sum = 0.0;
            for (0..output.rows) |row| {
                const phase = subcarrier.line(self.standard, frame, output.row_offset + @as(i32, @intCast(row)));
                self.encoder.encodeLine(recomposite[row * self.width ..], estimate_y[row * self.width ..], estimate_u[row * self.width ..], estimate_v[row * self.width ..], phase);
            }
            const crude_output = FrameOutput{
                .y = crude_y,
                .y_stride = self.width,
                .u = crude_u,
                .u_stride = self.width,
                .v = crude_u,
                .v_stride = self.width,
                .rows = output.rows,
                .row_offset = output.row_offset,
            };
            self.crudeDecodeFrame(frame, output.rows, output.row_offset, .{ .data = recomposite, .stride = self.width, .frame_number = frame }, crude_output);
            for (0..output.rows) |row| {
                const original_row = original[row * output.original_y_stride ..];
                for (0..self.width) |x| {
                    const difference = @as(i32, original_row[x]) - @as(i32, crude_y[row * self.width + x]);
                    if (iteration + 1 == self.refine) residual_sum += @as(f64, @floatFromInt(@abs(difference)));
                    estimate_y[row * self.width + x] = constants.clampU16(@as(i32, estimate_y[row * self.width + x]) + difference);
                }
            }
        }
        var correction_sum: f64 = 0.0;
        for (0..output.rows) |row| {
            const destination = output.y[row * output.y_stride ..];
            for (0..self.width) |x| {
                correction_sum += @as(f64, @floatFromInt(@abs(@as(i32, estimate_y[row * self.width + x]) - @as(i32, destination[x]))));
                destination[x] = estimate_y[row * self.width + x];
            }
        }
        if (output.metrics) |metrics| {
            const count = @as(f64, @floatFromInt(output.rows * self.width));
            metrics.refine_residual = residual_sum / count;
            metrics.refine_correction = correction_sum / count;
        }
        _ = composite;
    }

    fn emitMotionMask(self: *const Decoder, output: FrameOutput, scratch: *const Scratch, accumulator: *ConfidenceAccumulator) void {
        _ = accumulator;
        const mask = scratch.mask.?;
        var motion: usize = 0;
        for (0..output.rows) |row| {
            const mask_row = mask[row * self.width ..];
            for (0..self.width) |x| {
                const is_motion = mask_row[x] == 0;
                if (is_motion) motion += 1;
                if (output.mask) |destination| {
                    if (output.mask_kind == .motion) destination[row * output.mask_stride + x] = if (is_motion) 65535 else 0;
                }
            }
        }
        if (output.metrics) |metrics| metrics.motion_fraction = @as(f64, @floatFromInt(motion)) / @as(f64, @floatFromInt(output.rows * self.width));
    }
};

fn firRowStaged(coefficients: []const i32, input: []const i32, output: []i32, width: usize) void {
    const taps = coefficients.len;
    var staged: [padded_width + constants.equalizer_taps - 1]i32 = [_]i32{0} ** (padded_width + constants.equalizer_taps - 1);
    @memcpy(staged[taps / 2 ..][0..width], input[0..width]);
    fir.rowQ15(output[0..width], staged[0 .. width + taps - 1], coefficients);
}

fn eqRow(decoder: *const Decoder, input: []const i32, output: []i32, width: usize) void {
    firRowStaged(decoder.equalizer_q15[0..], input, output, width);
}

fn narrowRow(decoder: *const Decoder, input: []const i32, output: []i32, width: usize) void {
    firRowStaged(decoder.narrow_q15[0..], input, output, width);
}

fn roundFloatToI16(value: f32) i16 {
    return constants.clampI16(@intFromFloat(@round(value)));
}

fn palDemodRow(decoder: *const Decoder, m: []const [padded_width]i32, n: []const [padded_width]i32, u: []i32, v: []i32, bp: i32, bq: i32, v_switch: i32, width: usize) void {
    for (0..width) |x| {
        var pu: i64 = 0;
        var qu: i64 = 0;
        var pv: i64 = 0;
        var qv: i64 = 0;
        for (0..constants.decode_filter_size + 1) |band| {
            const left = constants.decode_filter_size + x - band;
            const right = constants.decode_filter_size + x + band;
            const coeff = decoder.cfilt_q16[band];
            const m0 = m[0][right] + m[0][left];
            const n0 = n[0][right] + n[0][left];
            const m1 = m[1][right] + m[1][left];
            const n1 = n[1][right] + n[1][left];
            const m2 = m[2][right] + m[2][left];
            const n2 = n[2][right] + n[2][left];
            const m3 = m[3][right] + m[3][left];
            const n3 = n[3][right] + n[3][left];
            pu += @as(i64, m0) * coeff[0] + @as(i64, m1) * coeff[1] + @as(i64, n2) * coeff[2] + @as(i64, n3) * coeff[3];
            qu += @as(i64, n0) * coeff[0] + @as(i64, n1) * coeff[1] - @as(i64, m2) * coeff[2] - @as(i64, m3) * coeff[3];
            pv += @as(i64, m0) * coeff[0] + @as(i64, m1) * coeff[1] - @as(i64, n2) * coeff[2] - @as(i64, n3) * coeff[3];
            qv += @as(i64, n0) * coeff[0] + @as(i64, n1) * coeff[1] + @as(i64, m2) * coeff[2] + @as(i64, m3) * coeff[3];
        }
        const pu0 = (pu + 32768) >> 16;
        const qu0 = (qu + 32768) >> 16;
        const pv0 = (pv + 32768) >> 16;
        const qv0 = (qv + 32768) >> 16;
        const u_numerator = pu0 * bp + qu0 * bq + 8192;
        const v_numerator = qv0 * bp - pv0 * bq + 8192;
        u[x] = @intCast((-u_numerator) >> 14);
        v[x] = v_switch * @as(i32, @intCast((-v_numerator) >> 14));
    }
}

fn ctiRow(y: []const u16, u: []i32, v: []i32, width: usize) void {
    const edge_threshold: i32 = 3000;
    const step_threshold: i32 = 3000;
    const distance: usize = 8;
    var x: usize = distance + 3;
    while (x < width - distance - 3) {
        const gradient = @as(i32, y[x + 1]) - @as(i32, y[x - 1]);
        if (gradient < edge_threshold and gradient > -edge_threshold) {
            x += 1;
            continue;
        }
        var end = x;
        while (end + 1 < width - distance - 3) {
            const next_gradient = @as(i32, y[end + 2]) - @as(i32, y[end]);
            if (next_gradient < edge_threshold and next_gradient > -edge_threshold) break;
            end += 1;
        }
        const low = x - distance;
        const high = end + distance;
        if (high >= width - 3) break;
        const ya = @divTrunc(@as(i32, y[low - 2]) + @as(i32, y[low - 1]) + @as(i32, y[low]), 3);
        const yb = @divTrunc(@as(i32, y[high]) + @as(i32, y[high + 1]) + @as(i32, y[high + 2]), 3);
        const yd = yb - ya;
        if (yd > 4096 or yd < -4096) {
            for ([_][]i32{ u, v }) |chroma| {
                const a = @divTrunc(chroma[low - 2] + chroma[low - 1] + chroma[low], 3);
                const b = @divTrunc(chroma[high] + chroma[high + 1] + chroma[high + 2], 3);
                if (b - a > step_threshold or a - b > step_threshold) {
                    for (low..high + 1) |index| {
                        var normalized = @divTrunc(@as(i64, @as(i32, y[index]) - ya) * 32768, yd);
                        normalized = std.math.clamp(normalized, 0, 32768);
                        chroma[index] = a + @as(i32, @intCast(@divTrunc(@as(i64, b - a) * normalized, 1 << 15)));
                    }
                }
            }
        }
        x = high + 1;
    }
}

fn ntscLinePhase(frame: i32, raster_row: i32) i32 {
    const frame_line = 39 + raster_row;
    const field_id = @mod(frame, 2) * 2 + @mod(frame_line, 2);
    const previous_lines = @divTrunc(field_id, 2) * 525 + @mod(field_id, 2) * 263 + @divTrunc(frame_line, 2);
    return @mod(previous_lines, 2);
}

fn ntscSplit3d(decoder: *const Decoder, rows: usize, row_offset: i32, views: []const FrameView, c1: [3][]const f32, c2: [3][]const f32, output: []i16) void {
    const width = decoder.width;
    const inverse_scale = @as(f64, @floatFromInt(decoder.comb_range)) / 45.0;
    const line_bonus: f64 = -2.0;
    const field_bonus: f64 = -4.0;
    const frame_bonus: f64 = -6.0;
    const weights = [_]f64{ 0.5, 1.0, 0.5 };
    const candidate_rows = [_]i32{ 0, 0, -2, 2, -1, 1, 0, 0 };
    const candidate_offsets = [_]i32{ -2, 2, 0, 0, 0, 0, 0, 0 };
    const candidate_bonuses = [_]f64{ 0.0, 0.0, line_bonus, line_bonus, field_bonus, field_bonus, frame_bonus, frame_bonus };
    for (0..rows) |row| {
        const current_phase = ntscLinePhase(views[1].frame_number, @as(i32, @intCast(row)) + row_offset);
        var candidate_frames = [_]usize{ 1, 1, 1, 1, 1, 1, 0, 2 };
        if (current_phase == ntscLinePhase(views[1].frame_number, @as(i32, @intCast(row)) + row_offset - 1)) candidate_frames[4] = 0 else candidate_frames[5] = 2;
        const current_c1 = c1[1][row * width ..];
        const current_c2 = c2[1][row * width ..];
        const reference = views[1].data[row * views[1].stride ..];
        const destination = output[row * width ..];
        for (0..3) |x| destination[x] = roundFloatToI16(current_c2[x]);
        for (3..width - 3) |x| {
            var best_penalty: f64 = 0.0;
            var best_sample: f32 = 0.0;
            var best_index: i32 = -1;
            for (0..8) |candidate_index| {
                const candidate_row = @as(i32, @intCast(row)) + candidate_rows[candidate_index];
                const inside = candidate_row >= 0 and candidate_row < @as(i32, @intCast(rows));
                const sample_row: usize = @intCast(if (inside) candidate_row else @as(i32, @intCast(row)));
                const candidate_frame = candidate_frames[candidate_index];
                const channel = @as(i32, @intCast(x)) + candidate_offsets[candidate_index];
                const candidate_channel: usize = @intCast(channel);
                var penalty: f64 = 1000.0;
                var sample: f32 = 0.0;
                if (inside) {
                    sample = c1[candidate_frame][sample_row * width + candidate_channel];
                    const candidate_phase = ntscLinePhase(views[candidate_frame].frame_number, @as(i32, @intCast(sample_row)) + row_offset);
                    const phase_ok = @mod(2 + 2 * current_phase - 2 * candidate_phase - candidate_offsets[candidate_index] + 8, 4) == 0;
                    if (phase_ok) {
                        var luma_penalty: f64 = 0.0;
                        var chroma_penalty: f64 = 0.0;
                        const candidate_composite = views[candidate_frame].data[sample_row * views[candidate_frame].stride ..];
                        const candidate_c2 = c2[candidate_frame][sample_row * width ..];
                        for ([_]i32{ -1, 0, 1 }) |offset| {
                            const current_x: usize = @intCast(@as(i32, @intCast(x)) + offset);
                            const candidate_x: usize = @intCast(channel + offset);
                            const reference_chroma = @as(f64, @floatCast(current_c2[current_x]));
                            const candidate_chroma = @as(f64, @floatCast(candidate_c2[candidate_x]));
                            luma_penalty += @abs((@as(f64, @floatFromInt(reference[current_x])) - reference_chroma) - (@as(f64, @floatFromInt(candidate_composite[candidate_x])) - candidate_chroma));
                            chroma_penalty += @abs(reference_chroma + candidate_chroma) * weights[@intCast(offset + 1)];
                        }
                        penalty = luma_penalty / 3.0 / inverse_scale + (chroma_penalty / 2.0 / inverse_scale) * 0.28 + candidate_bonuses[candidate_index];
                    }
                }
                if (best_index < 0 or penalty < best_penalty) {
                    best_index = @intCast(candidate_index);
                    best_penalty = penalty;
                    best_sample = sample;
                }
            }
            const value = if (best_index < 4) current_c2[x] else (current_c1[x] - best_sample) / 2.0;
            destination[x] = roundFloatToI16(value);
        }
        for (width - 3..width) |x| destination[x] = roundFloatToI16(current_c2[x]);
    }
}
