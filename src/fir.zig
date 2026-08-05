const std = @import("std");

pub fn rowQ15(out: []i32, input: []const i32, coefficients: []const i32) void {
    std.debug.assert(out.len + coefficients.len - 1 <= input.len);
    for (out, 0..) |*destination, x| {
        var accumulator: i64 = 0;
        for (coefficients, 0..) |coefficient, tap| {
            accumulator += @as(i64, coefficient) * input[x + tap];
        }
        destination.* = @intCast((accumulator + 16384) >> 15);
    }
}

test "q15 FIR rounds positive and negative accumulators symmetrically" {
    var output: [3]i32 = undefined;
    const input = [_]i32{ 100, 200, -100, -200, 50 };
    const coefficients = [_]i32{ 16384, 16384 };
    rowQ15(&output, &input, &coefficients);
    try std.testing.expectEqual(@as(i32, 150), output[0]);
    try std.testing.expectEqual(@as(i32, 50), output[1]);
    try std.testing.expectEqual(@as(i32, -150), output[2]);
}
