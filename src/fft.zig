const std = @import("std");

pub const Complex = struct {
    re: f32 = 0.0,
    im: f32 = 0.0,

    fn add(self: Complex, other: Complex) Complex {
        return .{ .re = self.re + other.re, .im = self.im + other.im };
    }

    fn sub(self: Complex, other: Complex) Complex {
        return .{ .re = self.re - other.re, .im = self.im - other.im };
    }

    fn mul(self: Complex, other: Complex) Complex {
        return .{
            .re = self.re * other.re - self.im * other.im,
            .im = self.re * other.im + self.im * other.re,
        };
    }

    fn conjugate(self: Complex) Complex {
        return .{ .re = self.re, .im = -self.im };
    }

    pub fn magnitudeSquared(self: Complex) f32 {
        return self.re * self.re + self.im * self.im;
    }

    pub fn scale(self: Complex, factor: f32) Complex {
        return .{ .re = self.re * factor, .im = self.im * factor };
    }
};

fn reverseBits(value: usize, bits: usize) usize {
    var result: usize = 0;
    var bit: usize = 0;
    while (bit < bits) : (bit += 1) {
        result = (result << 1) | ((value >> @intCast(bit)) & 1);
    }
    return result;
}

fn fft1d(values: []Complex, inverse: bool) void {
    std.debug.assert(values.len > 0 and (values.len & (values.len - 1)) == 0);

    var bits: usize = 0;
    var length = values.len;
    while (length > 1) : (length >>= 1) bits += 1;
    for (0..values.len) |i| {
        const reversed = reverseBits(i, bits);
        if (reversed > i) std.mem.swap(Complex, &values[i], &values[reversed]);
    }

    var block_size: usize = 2;
    while (block_size <= values.len) : (block_size <<= 1) {
        const half = block_size / 2;
        const sign: f32 = if (inverse) 1.0 else -1.0;
        const angle = sign * 2.0 * std.math.pi / @as(f32, @floatFromInt(block_size));
        const rotation = Complex{ .re = @floatCast(@cos(@as(f64, angle))), .im = @floatCast(@sin(@as(f64, angle))) };

        var block: usize = 0;
        while (block < values.len) : (block += block_size) {
            var twiddle = Complex{ .re = 1.0, .im = 0.0 };
            for (0..half) |offset| {
                const left_index = block + offset;
                const right_index = left_index + half;
                const even = values[left_index];
                const odd = values[right_index].mul(twiddle);
                values[left_index] = even.add(odd);
                values[right_index] = even.sub(odd);
                twiddle = twiddle.mul(rotation);
            }
        }
    }
}

pub fn forward2d(real: []const f32, spectrum: []Complex, width: usize, height: usize) void {
    std.debug.assert(real.len >= width * height);
    std.debug.assert(spectrum.len >= width * height);
    for (0..height) |y| {
        for (0..width) |x| spectrum[y * width + x] = .{ .re = real[y * width + x], .im = 0.0 };
        fft1d(spectrum[y * width ..][0..width], false);
    }

    var column: [32]Complex = undefined;
    std.debug.assert(height <= column.len);
    for (0..width) |x| {
        for (0..height) |y| column[y] = spectrum[y * width + x];
        fft1d(column[0..height], false);
        for (0..height) |y| spectrum[y * width + x] = column[y];
    }
}

pub fn inverse2d(spectrum: []const Complex, real: []f32, width: usize, height: usize) void {
    std.debug.assert(real.len >= width * height);
    std.debug.assert(spectrum.len >= width * height);
    var working: [32 * 16]Complex = undefined;
    std.debug.assert(width * height <= working.len);
    @memcpy(working[0 .. width * height], spectrum[0 .. width * height]);
    completeRealSpectrum2d(working[0 .. width * height], width, height);

    for (0..width) |x| {
        var column: [32]Complex = undefined;
        for (0..height) |y| column[y] = working[y * width + x];
        fft1d(column[0..height], true);
        for (0..height) |y| working[y * width + x] = column[y];
    }
    for (0..height) |y| {
        fft1d(working[y * width ..][0..width], true);
        for (0..width) |x| real[y * width + x] = working[y * width + x].re / @as(f32, @floatFromInt(width * height));
    }
}

fn completeRealSpectrum2d(spectrum: []Complex, width: usize, height: usize) void {
    const half = width / 2;
    for (0..height) |y| {
        const reflected_y = (height - y) % height;
        var x: usize = 1;
        while (x < half) : (x += 1) {
            spectrum[reflected_y * width + width - x] = spectrum[y * width + x].conjugate();
        }
    }
}

pub fn forward3d(real: []const f32, spectrum: []Complex, width: usize, height: usize, depth: usize) void {
    const plane = width * height;
    const volume = plane * depth;
    std.debug.assert(real.len >= volume and spectrum.len >= volume);
    for (0..depth) |z| {
        for (0..height) |y| {
            const row = (z * height + y) * width;
            for (0..width) |x| spectrum[row + x] = .{ .re = real[row + x], .im = 0.0 };
            fft1d(spectrum[row..][0..width], false);
        }
    }

    var line: [32]Complex = undefined;
    std.debug.assert(height <= line.len);
    for (0..depth) |z| {
        for (0..width) |x| {
            for (0..height) |y| line[y] = spectrum[(z * height + y) * width + x];
            fft1d(line[0..height], false);
            for (0..height) |y| spectrum[(z * height + y) * width + x] = line[y];
        }
    }

    std.debug.assert(depth <= 8);
    for (0..height) |y| {
        for (0..width) |x| {
            for (0..depth) |z| line[z] = spectrum[(z * height + y) * width + x];
            fft1d(line[0..depth], false);
            for (0..depth) |z| spectrum[(z * height + y) * width + x] = line[z];
        }
    }
}

pub fn inverse3d(spectrum: []const Complex, real: []f32, width: usize, height: usize, depth: usize) void {
    const volume = width * height * depth;
    std.debug.assert(real.len >= volume and spectrum.len >= volume);
    var working: [16 * 32 * 8]Complex = undefined;
    std.debug.assert(volume <= working.len);
    @memcpy(working[0..volume], spectrum[0..volume]);
    completeRealSpectrum3d(working[0..volume], width, height, depth);

    var line: [32]Complex = undefined;
    for (0..height) |y| {
        for (0..width) |x| {
            for (0..depth) |z| line[z] = working[(z * height + y) * width + x];
            fft1d(line[0..depth], true);
            for (0..depth) |z| working[(z * height + y) * width + x] = line[z];
        }
    }
    for (0..depth) |z| {
        for (0..width) |x| {
            for (0..height) |y| line[y] = working[(z * height + y) * width + x];
            fft1d(line[0..height], true);
            for (0..height) |y| working[(z * height + y) * width + x] = line[y];
        }
    }
    for (0..depth) |z| {
        for (0..height) |y| {
            const row = (z * height + y) * width;
            fft1d(working[row..][0..width], true);
            for (0..width) |x| real[row + x] = working[row + x].re / @as(f32, @floatFromInt(volume));
        }
    }
}

fn completeRealSpectrum3d(spectrum: []Complex, width: usize, height: usize, depth: usize) void {
    const half = width / 2;
    for (0..depth) |z| {
        for (0..height) |y| {
            const reflected_z = (depth - z) % depth;
            const reflected_y = (height - y) % height;
            var x: usize = 1;
            while (x < half) : (x += 1) {
                spectrum[(reflected_z * height + reflected_y) * width + width - x] = spectrum[(z * height + y) * width + x].conjugate();
            }
        }
    }
}

test "2D FFT round trips real tiles" {
    var input: [32 * 16]f32 = undefined;
    var output: [32 * 16]f32 = undefined;
    var spectrum: [32 * 16]Complex = undefined;
    for (&input, 0..) |*value, i| value.* = @floatFromInt(@as(i32, @intCast(i % 19)) - 9);
    forward2d(&input, &spectrum, 32, 16);
    inverse2d(&spectrum, &output, 32, 16);
    for (input, output) |expected, actual| try std.testing.expectApproxEqAbs(expected, actual, 0.001);
}

test "3D FFT round trips real tiles" {
    var input: [16 * 32 * 8]f32 = undefined;
    var output: [16 * 32 * 8]f32 = undefined;
    var spectrum: [16 * 32 * 8]Complex = undefined;
    for (&input, 0..) |*value, i| value.* = @floatFromInt(@as(i32, @intCast(i % 11)) - 5);
    forward3d(&input, &spectrum, 16, 32, 8);
    inverse3d(&spectrum, &output, 16, 32, 8);
    for (input, output) |expected, actual| try std.testing.expectApproxEqAbs(expected, actual, 0.001);
}
