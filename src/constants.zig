const std = @import("std");

pub const Standard = enum {
    pal,
    ntsc,

    pub fn parse(value: []const u8) ?Standard {
        if (std.mem.eql(u8, value, "pal")) return .pal;
        if (std.mem.eql(u8, value, "ntsc")) return .ntsc;
        return null;
    }

    pub fn name(self: Standard) []const u8 {
        return switch (self) {
            .pal => "pal",
            .ntsc => "ntsc",
        };
    }

    pub fn activeWidth(self: Standard) usize {
        return switch (self) {
            .pal => 928,
            .ntsc => 758,
        };
    }

    pub fn activeHeight(self: Standard) usize {
        return switch (self) {
            .pal => 576,
            .ntsc => 486,
        };
    }

    pub fn subcarrierDenominator(self: Standard) usize {
        return switch (self) {
            .pal => 2500,
            .ntsc => 720,
        };
    }
};

pub const edge_pad: usize = 24;
pub const level_min: i32 = 0x0100;
pub const level_max: i32 = 0xFEFF;
pub const uv_max_taps: usize = 13;

pub const Transform2d = struct {
    pub const tile_width: usize = 32;
    pub const tile_height: usize = 16;
    pub const complex_width: usize = tile_width / 2 + 1;
    pub const threshold_bins: usize = tile_height * (tile_width / 4 - tile_width / 8 + 1);
};

pub const Transform3d = struct {
    pub const tile_width: usize = 16;
    pub const tile_height: usize = 32;
    pub const pal_tile_height: usize = tile_height / 2;
    pub const tile_depth: usize = 8;
    pub const complex_width: usize = tile_width / 2 + 1;
    pub const threshold_bins: usize = tile_depth * tile_height * (tile_width / 4 - tile_width / 8 + 1);
    pub const pal_threshold_bins: usize = threshold_bins / 2;
    pub const look: usize = 3;
};

pub const lut_knots: usize = 16;
pub const decode_filter_size: usize = 7;
pub const equalizer_taps: usize = 31;
pub const narrow_taps: usize = 13;
pub const color_lowpass_taps: usize = 17;

pub const MaskKind = enum {
    none,
    motion,
    confidence,
};

pub const Metrics = struct {
    separation_confidence_mean: f64 = -1.0,
    separation_confidence_stddev: f64 = -1.0,
    motion_fraction: f64 = -1.0,
    refine_residual: f64 = -1.0,
    refine_correction: f64 = -1.0,
};

pub fn clampU16(value: i32) u16 {
    if (value < 0) return 0;
    if (value > std.math.maxInt(u16)) return std.math.maxInt(u16);
    return @intCast(value);
}

pub fn clampI16(value: i32) i16 {
    if (value < std.math.minInt(i16)) return std.math.minInt(i16);
    if (value > std.math.maxInt(i16)) return std.math.maxInt(i16);
    return @intCast(value);
}

pub fn clampLevel(value: i32) u16 {
    if (value < level_min) return @intCast(level_min);
    if (value > level_max) return @intCast(level_max);
    return @intCast(value);
}

pub fn roundDivSigned(numerator: i64, denominator: i32) i32 {
    std.debug.assert(denominator > 0);
    const den: i64 = denominator;
    const adjusted = if (numerator >= 0) numerator + @divTrunc(den, 2) else numerator - @divTrunc(den, 2);
    return @intCast(@divTrunc(adjusted, den));
}

pub fn alignedWidth(width: usize) usize {
    return (width + 15) & ~@as(usize, 15);
}

test "domain constants match the composite raster contract" {
    try std.testing.expectEqual(@as(usize, 928), Standard.pal.activeWidth());
    try std.testing.expectEqual(@as(usize, 758), Standard.ntsc.activeWidth());
    try std.testing.expectEqual(@as(usize, 80), Transform2d.threshold_bins);
    try std.testing.expectEqual(@as(usize, 384), Transform3d.pal_threshold_bins);
    try std.testing.expectEqual(@as(usize, 768), Transform3d.threshold_bins);
}
