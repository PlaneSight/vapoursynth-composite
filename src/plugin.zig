const std = @import("std");
const vapoursynth = @import("vapoursynth");
const core = @import("composite");
const lut = @import("lut_tables.zig");

const vs = vapoursynth.vapoursynth4;
const vsc = vapoursynth.vsconstants;
const vsh = vapoursynth.vshelper;
const ZAPI = vapoursynth.ZAPI;
const zon_version = std.SemanticVersion{ .major = 0, .minor = 2, .patch = 0 };
const allocator = std.heap.c_allocator;

const EncodeData = struct {
    node: *vs.Node,
    vi: vs.VideoInfo,
    encoder: core.encode.Encoder,
};

const DecodeData = struct {
    node: *vs.Node,
    original_node: ?*vs.Node,
    vi: vs.VideoInfo,
    decoder: *core.decode.Decoder,
    standard: core.constants.Standard,
    mask_kind: core.constants.MaskKind,
};

fn parseStandard(map: anytype) ?core.constants.Standard {
    const value = map.getDataZ("standard", 0) orelse return .pal;
    return core.constants.Standard.parse(value);
}

fn boolOption(map: anytype, comptime key: [:0]const u8, default: bool) bool {
    return (map.getInt(i32, key) orelse @intFromBool(default)) != 0;
}

fn copyIntOrFloat(source: anytype, destination: anytype, comptime key: [:0]const u8) void {
    if (source.getInt(i32, key)) |value| destination.setInt(key, value, .Replace) else if (source.getFloat(f64, key)) |value| destination.setFloat(key, value, .Replace);
}

fn sameFormat(vi: *const vs.VideoInfo, family: vs.ColorFamily, sample_type: vs.SampleType, bits: i32, subsampling_w: i32, subsampling_h: i32) bool {
    return vi.format.colorFamily == family and vi.format.sampleType == sample_type and vi.format.bitsPerSample == bits and vi.format.subSamplingW == subsampling_w and vi.format.subSamplingH == subsampling_h;
}

fn resizeNode(zapi: *const ZAPI, node: *vs.Node, filter_name: [:0]const u8, format: ?vs.PresetVideoFormat, width: i32, height: i32, src_left: ?f64, src_width: ?f64) ?*vs.Node {
    const resize = zapi.getPluginByID2(.Resize) orelse return null;
    var args = zapi.createZMap();
    defer args.free();
    _ = args.setNode("clip", node, .Replace);
    if (format) |value| args.setVideoFormat("format", value, .Replace);
    if (width > 0) args.setInt("width", width, .Replace);
    if (height > 0) args.setInt("height", height, .Replace);
    if (src_left) |value| args.setFloat("src_left", value, .Replace);
    if (src_width) |value| args.setFloat("src_width", value, .Replace);
    var result = args.invoke(resize, filter_name);
    defer result.free();
    if (result.getError() != null) return null;
    return result.getNode("clip");
}

fn canonicalize(zapi: *const ZAPI, node: *vs.Node, standard: core.constants.Standard, format: vs.PresetVideoFormat, family: vs.ColorFamily, width: i32, height: i32, src_left: ?f64, src_width: ?f64) ?*vs.Node {
    _ = standard;
    const vi = zapi.getVideoInfo(node);
    if (sameFormat(vi, family, .Integer, 16, 0, 0) and vi.width == width and vi.height == height) return node;
    const result = resizeNode(zapi, node, "Spline36", format, width, height, src_left, src_width) orelse {
        zapi.freeNode(node);
        return null;
    };
    zapi.freeNode(node);
    return result;
}

fn readPlane(zapi: *const ZAPI, frame: *const vs.Frame, plane: i32, height: usize) []const u16 {
    const stride = @as(usize, @intCast(@divExact(zapi.getStride(frame, plane), @sizeOf(u16))));
    const ptr = zapi.getReadPtr(frame, plane);
    return @as([*]const u16, @ptrCast(@alignCast(ptr)))[0 .. stride * height];
}

fn writePlane(zapi: *const ZAPI, frame: *vs.Frame, plane: i32, height: usize) []u16 {
    const stride = @as(usize, @intCast(@divExact(zapi.getStride(frame, plane), @sizeOf(u16))));
    const ptr = zapi.getWritePtr(frame, plane);
    return @as([*]u16, @ptrCast(@alignCast(ptr)))[0 .. stride * height];
}

fn strideSamples(zapi: *const ZAPI, frame: *const vs.Frame, plane: i32) usize {
    return @intCast(@divExact(zapi.getStride(frame, plane), @sizeOf(u16)));
}

fn rowOffset(zapi: *const ZAPI, frame: *const vs.Frame, standard: core.constants.Standard, height: usize) i32 {
    if (standard != .ntsc or height != 480) return 0;
    const props = ZAPI.ZFrame(*const vs.Frame).init(zapi, frame).getPropertiesRO();
    return if (props.getFieldBased() == .TOP) 5 else 4;
}

fn encodeGetFrame(n: c_int, reason: vs.ActivationReason, instance_data: ?*anyopaque, frame_data: ?*?*anyopaque, frame_ctx: ?*vs.FrameContext, core_ptr: ?*vs.Core, vsapi: ?*const vs.API) callconv(.c) ?*const vs.Frame {
    _ = frame_data;
    const data: *EncodeData = @ptrCast(@alignCast(instance_data));
    const zapi = ZAPI.init(vsapi, core_ptr, frame_ctx);
    if (reason == .Initial) {
        zapi.requestFrameFilter(n, data.node);
        return null;
    }
    if (reason != .AllFramesReady) return null;

    const source = zapi.initZFrame(data.node, n);
    defer source.deinit();
    var destination = zapi.initZFrameFromVi(&data.vi, source.frame);
    const height: usize = @intCast(data.vi.height);
    const source_y = source.getReadSlice2(u16, 0);
    const source_u = source.getReadSlice2(u16, 1);
    const source_v = source.getReadSlice2(u16, 2);
    const destination_plane = destination.getWriteSlice2(u16, 0);
    data.encoder.encodeFrame(
        n,
        height,
        rowOffset(&zapi, source.frame, data.encoder.standard, height),
        destination_plane,
        destination.getStride2(u16, 0),
        source_y,
        source.getStride2(u16, 0),
        source_u,
        source.getStride2(u16, 1),
        source_v,
        source.getStride2(u16, 2),
    );
    return destination.frame;
}

fn encodeFree(instance_data: ?*anyopaque, core_ptr: ?*vs.Core, vsapi: ?*const vs.API) callconv(.c) void {
    const data: *EncodeData = @ptrCast(@alignCast(instance_data));
    const zapi = ZAPI.init(vsapi, core_ptr, null);
    zapi.freeNode(data.node);
    allocator.destroy(data);
}

fn decodeGetFrame(n: c_int, reason: vs.ActivationReason, instance_data: ?*anyopaque, frame_data: ?*?*anyopaque, frame_ctx: ?*vs.FrameContext, core_ptr: ?*vs.Core, vsapi: ?*const vs.API) callconv(.c) ?*const vs.Frame {
    _ = frame_data;
    const data: *DecodeData = @ptrCast(@alignCast(instance_data));
    const zapi = ZAPI.init(vsapi, core_ptr, frame_ctx);
    const look = data.decoder.look();
    const last = data.vi.numFrames - 1;
    if (reason == .Initial) {
        var previous: c_int = -1;
        for (0..2 * look + 1) |index| {
            const candidate = @as(c_int, @intCast(@min(@max(@as(i32, n) - @as(i32, @intCast(look)) + @as(i32, @intCast(index)), 0), @as(i32, last))));
            if (candidate != previous) zapi.requestFrameFilter(candidate, data.node);
            previous = candidate;
        }
        if (data.original_node) |original| zapi.requestFrameFilter(n, original);
        return null;
    }
    if (reason != .AllFramesReady) return null;

    var source_frames: [2 * core.constants.Transform3d.look + 1]*const vs.Frame = undefined;
    var views: [2 * core.constants.Transform3d.look + 1]core.decode.FrameView = undefined;
    for (0..2 * look + 1) |index| {
        const candidate = @as(c_int, @intCast(@min(@max(@as(i32, n) - @as(i32, @intCast(look)) + @as(i32, @intCast(index)), 0), @as(i32, last))));
        const frame = zapi.getFrameFilter(candidate, data.node).?;
        source_frames[index] = frame;
        views[index] = .{
            .data = readPlane(&zapi, frame, 0, @intCast(data.vi.height)),
            .stride = strideSamples(&zapi, frame, 0),
            .frame_number = candidate,
        };
    }
    defer for (0..2 * look + 1) |index| zapi.freeFrame(source_frames[index]);

    var destination = zapi.initZFrameFromVi(&data.vi, source_frames[look]);
    var mask_frame: ?ZAPI.ZFrame(*vs.Frame) = null;
    var mask_slice: ?[]u16 = null;
    var mask_stride: usize = 0;
    if (data.mask_kind != .none) {
        var mask_vi = data.vi;
        _ = zapi.queryVideoFormat(&mask_vi.format, .Gray, .Integer, 16, 0, 0);
        mask_frame = zapi.initZFrameFromVi(&mask_vi, source_frames[look]);
        mask_stride = mask_frame.?.getStride2(u16, 0);
        mask_slice = mask_frame.?.getWriteSlice2(u16, 0);
        @memset(mask_slice.?, 0);
    }

    var metrics = core.constants.Metrics{};
    var original_frame: ?*const vs.Frame = null;
    var original_plane: ?[]const u16 = null;
    var original_stride: usize = 0;
    if (data.original_node) |original_node| {
        original_frame = zapi.getFrameFilter(n, original_node).?;
        original_plane = readPlane(&zapi, original_frame.?, 0, @intCast(data.vi.height));
        original_stride = strideSamples(&zapi, original_frame.?, 0);
    }
    defer if (original_frame) |frame| zapi.freeFrame(frame);

    const output = core.decode.FrameOutput{
        .y = writePlane(&zapi, destination.frame, 0, @intCast(data.vi.height)),
        .y_stride = strideSamples(&zapi, destination.frame, 0),
        .u = writePlane(&zapi, destination.frame, 1, @intCast(data.vi.height)),
        .u_stride = strideSamples(&zapi, destination.frame, 1),
        .v = writePlane(&zapi, destination.frame, 2, @intCast(data.vi.height)),
        .v_stride = strideSamples(&zapi, destination.frame, 2),
        .rows = @intCast(data.vi.height),
        .row_offset = rowOffset(&zapi, source_frames[look], data.standard, @intCast(data.vi.height)),
        .original_y = original_plane,
        .original_y_stride = original_stride,
        .mask = mask_slice,
        .mask_stride = mask_stride,
        .mask_kind = data.mask_kind,
        .metrics = &metrics,
    };
    data.decoder.decodeFrame(n, @intCast(data.vi.numFrames), views[0 .. 2 * look + 1], look, output);

    var properties = destination.getPropertiesRW();
    if (metrics.separation_confidence_mean >= 0.0) {
        properties.setFloat("CompositeSeparationConfidenceMean", metrics.separation_confidence_mean, .Replace);
        properties.setFloat("CompositeSeparationConfidenceStdDev", metrics.separation_confidence_stddev, .Replace);
    }
    if (metrics.motion_fraction >= 0.0) properties.setFloat("CompositeMotionFraction", metrics.motion_fraction, .Replace);
    if (metrics.refine_residual >= 0.0) properties.setFloat("CompositeRefineResidual", metrics.refine_residual, .Replace);
    if (metrics.refine_correction >= 0.0) properties.setFloat("CompositeRefineCorrection", metrics.refine_correction, .Replace);
    if (mask_frame) |mask| {
        _ = properties.consumeFrame("CompositeMask", mask.frame, .Replace);
    }
    return destination.frame;
}

fn decodeFree(instance_data: ?*anyopaque, core_ptr: ?*vs.Core, vsapi: ?*const vs.API) callconv(.c) void {
    const data: *DecodeData = @ptrCast(@alignCast(instance_data));
    const zapi = ZAPI.init(vsapi, core_ptr, null);
    zapi.freeNode(data.node);
    if (data.original_node) |node| zapi.freeNode(node);
    data.decoder.deinit();
    allocator.destroy(data.decoder);
    allocator.destroy(data);
}

fn extractMask(zapi: *const ZAPI, node: *vs.Node) ?struct { picture: *vs.Node, mask: *vs.Node } {
    const standard = zapi.getPluginByID2(.Std) orelse return null;
    var args = zapi.createZMap();
    _ = args.setNode("clip", node, .Replace);
    args.setData("prop", "CompositeMask", .Utf8, .Replace);
    var ret = args.invoke(standard, "PropToClip");
    args.free();
    if (ret.getError() != null) {
        ret.free();
        return null;
    }
    const mask = ret.getNode("clip") orelse {
        ret.free();
        return null;
    };
    ret.free();

    var strip_args = zapi.createZMap();
    _ = strip_args.setNode("clip", node, .Replace);
    strip_args.setData("props", "CompositeMask", .Utf8, .Replace);
    var stripped = strip_args.invoke(standard, "RemoveFrameProps");
    strip_args.free();
    if (stripped.getError() != null) {
        stripped.free();
        zapi.freeNode(mask);
        return null;
    }
    const picture = stripped.getNode("clip") orelse {
        stripped.free();
        zapi.freeNode(mask);
        return null;
    };
    stripped.free();
    return .{ .picture = picture, .mask = mask };
}

fn publishResampled(zapi: *const ZAPI, out: anytype, picture: *vs.Node, mask: ?*vs.Node, width: i32, height: i32) void {
    var output_picture = picture;
    if (width > 0 and (zapi.getVideoInfo(output_picture).width != width or zapi.getVideoInfo(output_picture).height != height)) {
        const resized = resizeNode(zapi, output_picture, "Spline36", null, width, height, null, null);
        if (resized == null) {
            zapi.freeNode(output_picture);
            if (mask) |value| zapi.freeNode(value);
            out.setError("composite: output resize failed");
            return;
        }
        zapi.freeNode(output_picture);
        output_picture = resized.?;
    }
    _ = out.consumeNode("clip", output_picture, .Replace);
    if (mask) |value| {
        var output_mask = value;
        if (width > 0 and (zapi.getVideoInfo(output_mask).width != width or zapi.getVideoInfo(output_mask).height != height)) {
            const resized = resizeNode(zapi, output_mask, "Bilinear", null, width, height, null, null);
            if (resized == null) {
                zapi.freeNode(output_mask);
                out.setError("composite: mask resize failed");
                return;
            }
            zapi.freeNode(output_mask);
            output_mask = resized.?;
        }
        _ = out.consumeNode("clip", output_mask, .Append);
    }
}

fn encodeCreate(in: ?*const vs.Map, out: ?*vs.Map, user_data: ?*anyopaque, core_ptr: ?*vs.Core, vsapi: *const vs.API) callconv(.c) void {
    _ = user_data;
    const zapi = ZAPI.init(vsapi, core_ptr, null);
    const map_in = zapi.initZMap(in);
    const map_out = zapi.initZMap(out);
    const input = map_in.getNodeVi2("clip") orelse {
        map_out.setError("Encode: clip is required");
        return;
    };
    var standard = parseStandard(map_in) orelse {
        map_out.setError("Encode: standard must be pal or ntsc");
        zapi.freeNode(input.node);
        return;
    };
    const setup = boolOption(map_in, "setup", false);
    const precomb = boolOption(map_in, "precomb", false);
    if (!vsh.isConstantVideoFormat(input.vi) or input.vi.format.colorFamily != .YUV) {
        map_out.setError("Encode: clip must be a constant-format YUV clip");
        zapi.freeNode(input.node);
        return;
    }
    const target_width: i32 = @intCast(standard.activeWidth());
    if (standard == .pal and input.vi.height != 576 or standard == .ntsc and input.vi.height != 480 and input.vi.height != 486) {
        map_out.setError("Encode: PAL requires 576 lines; NTSC requires 480 or 486 lines");
        zapi.freeNode(input.node);
        return;
    }
    const resample = core.geometry.encodeResample(standard, @intCast(input.vi.width));
    const working = canonicalize(&zapi, input.node, standard, .YUV444P16, .YUV, target_width, input.vi.height, resample.source_left, resample.source_width) orelse {
        map_out.setError("Encode: input resampling failed");
        return;
    };
    const encoder = core.encode.Encoder.init(standard, setup, precomb) catch {
        zapi.freeNode(working);
        map_out.setError("Encode: unsupported standard");
        return;
    };
    var output_vi = input.vi.*;
    _ = zapi.queryVideoFormat(&output_vi.format, .Gray, .Integer, 16, 0, 0);
    output_vi.width = target_width;
    const data = allocator.create(EncodeData) catch {
        zapi.freeNode(working);
        map_out.setError("Encode: out of memory");
        return;
    };
    data.* = .{ .node = working, .vi = output_vi, .encoder = encoder };
    const dep = [_]vs.FilterDependency{.{ .source = data.node, .requestPattern = .StrictSpatial }};
    zapi.createVideoFilter(out, "Encode", &data.vi, encodeGetFrame, encodeFree, .Parallel, &dep, data);
}

fn decodeCreate(in: ?*const vs.Map, out: ?*vs.Map, user_data: ?*anyopaque, core_ptr: ?*vs.Core, vsapi: *const vs.API) callconv(.c) void {
    _ = user_data;
    const zapi = ZAPI.init(vsapi, core_ptr, null);
    const map_in = zapi.initZMap(in);
    const map_out = zapi.initZMap(out);
    const input = map_in.getNodeVi2("clip") orelse {
        map_out.setError("Decode: clip is required");
        return;
    };
    const standard = parseStandard(map_in) orelse {
        map_out.setError("Decode: standard must be pal or ntsc");
        zapi.freeNode(input.node);
        return;
    };
    const requested_width = map_in.getInt(i32, "width") orelse input.vi.width;
    if (requested_width != 0 and (requested_width < 16 or requested_width > 8192)) {
        map_out.setError("Decode: width must be 0 or between 16 and 8192");
        zapi.freeNode(input.node);
        return;
    }
    if (!vsh.isConstantVideoFormat(input.vi) or !sameFormat(input.vi, .Gray, .Integer, 16, 0, 0)) {
        map_out.setError("Decode: clip must be a constant GRAY16 composite");
        zapi.freeNode(input.node);
        return;
    }
    const active_width: i32 = @intCast(standard.activeWidth());
    if (input.vi.width != active_width or (standard == .pal and input.vi.height != 576) or (standard == .ntsc and input.vi.height != 480 and input.vi.height != 486)) {
        map_out.setError("Decode: composite dimensions do not match the selected standard");
        zapi.freeNode(input.node);
        return;
    }

    var config = core.decode.Config{ .standard = standard };
    config.setup = boolOption(map_in, "setup", false);
    config.threshold = map_in.getFloat(f64, "threshold") orelse 0.4;
    config.dimensions = @intCast(map_in.getInt(i32, "dimensions") orelse 3);
    const transform_value: i32 = map_in.getInt(i32, "transform") orelse if (standard == .ntsc and config.dimensions == 3) @as(i32, 2) else @as(i32, 0);
    const default_eq: i32 = if (standard == .pal or transform_value != 0) 2 else 1;
    config.eq = @intCast(map_in.getInt(i32, "eq") orelse default_eq);
    config.refine = @intCast(map_in.getInt(i32, "refine") orelse 0);
    config.use_transform = @intCast(transform_value);
    config.level = boolOption(map_in, "level", false);
    config.evidence = map_in.getFloat(f64, "evidence") orelse 0.0;
    config.cti = boolOption(map_in, "cti", false);
    var info: vs.CoreInfo = .{};
    zapi.getCoreInfo(core_ptr, &info);
    config.scratch_count = @intCast(@max(info.numThreads, 1));
    const decoder = allocator.create(core.decode.Decoder) catch {
        zapi.freeNode(input.node);
        map_out.setError("Decode: out of memory");
        return;
    };
    decoder.* = core.decode.Decoder.init(allocator, config) catch {
        allocator.destroy(decoder);
        zapi.freeNode(input.node);
        map_out.setError("Decode: invalid decoder configuration");
        return;
    };
    if (map_in.getFloatArray("thresholds")) |values| decoder.setThresholds(values) catch {
        decoder.deinit();
        allocator.destroy(decoder);
        zapi.freeNode(input.node);
        map_out.setError("Decode: invalid thresholds");
        return;
    };
    if (map_in.getFloatArray("lut")) |values| decoder.setLut(values) catch {
        decoder.deinit();
        allocator.destroy(decoder);
        zapi.freeNode(input.node);
        map_out.setError("Decode: invalid LUT");
        return;
    };
    const has_transform = standard == .pal and config.dimensions >= 2 or config.use_transform != 0;
    const threshold_explicit = map_in.getFloat(f64, "threshold") != null;
    const level_explicit = map_in.getInt(i32, "level") != null;
    if (has_transform and !threshold_explicit and !level_explicit and map_in.getFloatArray("thresholds") == null and map_in.getFloatArray("lut") == null) {
        const values: []const f64 = if (standard == .pal and config.dimensions == 2)
            lut.builtin_pal_2d[0..]
        else if (standard == .pal)
            lut.builtin_pal_3d[0..]
        else
            lut.builtin_ntsc[0..];
        decoder.setLut(values) catch {
            decoder.deinit();
            allocator.destroy(decoder);
            zapi.freeNode(input.node);
            map_out.setError("Decode: built-in LUT does not match the selected path");
            return;
        };
    }

    var original_node: ?*vs.Node = null;
    if (config.refine > 0) {
        const original = map_in.getNode("orig") orelse {
            decoder.deinit();
            allocator.destroy(decoder);
            zapi.freeNode(input.node);
            map_out.setError("Decode: refine requires an orig YUV clip");
            return;
        };
        original_node = canonicalize(&zapi, original, standard, .YUV444P16, .YUV, active_width, input.vi.height, null, null) orelse {
            decoder.deinit();
            allocator.destroy(decoder);
            zapi.freeNode(input.node);
            map_out.setError("Decode: refinement source conversion failed");
            return;
        };
    }

    var output_vi = input.vi.*;
    _ = zapi.queryVideoFormat(&output_vi.format, .YUV, .Integer, 16, 0, 0);
    output_vi.width = active_width;
    const data = allocator.create(DecodeData) catch {
        decoder.deinit();
        allocator.destroy(decoder);
        zapi.freeNode(input.node);
        if (original_node) |node| zapi.freeNode(node);
        map_out.setError("Decode: out of memory");
        return;
    };
    const mask_text = map_in.getData("mask", 0);
    var mask_kind: core.constants.MaskKind = .none;
    if (mask_text) |text_value| {
        if (std.mem.eql(u8, text_value, "motion")) mask_kind = .motion else if (std.mem.eql(u8, text_value, "confidence")) mask_kind = .confidence else {
            allocator.destroy(data);
            decoder.deinit();
            allocator.destroy(decoder);
            zapi.freeNode(input.node);
            if (original_node) |node| zapi.freeNode(node);
            map_out.setError("Decode: mask must be motion or confidence");
            return;
        }
    }
    const mask_supported = switch (mask_kind) {
        .none => true,
        .motion => standard == .ntsc and config.dimensions == 3 and config.use_transform == 2,
        .confidence => config.eq == 2,
    };
    if (!mask_supported) {
        allocator.destroy(data);
        decoder.deinit();
        allocator.destroy(decoder);
        zapi.freeNode(input.node);
        if (original_node) |node| zapi.freeNode(node);
        map_out.setError("Decode: requested mask is not available for this decoder path");
        return;
    }
    data.* = .{ .node = input.node, .original_node = original_node, .vi = output_vi, .decoder = decoder, .standard = standard, .mask_kind = mask_kind };
    const dep = [_]vs.FilterDependency{.{ .source = data.node, .requestPattern = if (config.dimensions == 3) .General else .StrictSpatial }};
    const decoded = zapi.createVideoFilter2("Decode", &data.vi, decodeGetFrame, decodeFree, .Parallel, &dep, data) orelse {
        decodeFree(data, core_ptr, vsapi);
        map_out.setError("Decode: failed to create filter");
        return;
    };
    var picture = decoded;
    var mask: ?*vs.Node = null;
    if (mask_kind != .none) {
        const extracted = extractMask(&zapi, picture) orelse {
            zapi.freeNode(picture);
            map_out.setError("Decode: mask extraction failed");
            return;
        };
        zapi.freeNode(picture);
        picture = extracted.picture;
        mask = extracted.mask;
    }
    const output_height = input.vi.height;
    publishResampled(&zapi, map_out, picture, mask, requested_width, output_height);
}

fn restoreCreate(in: ?*const vs.Map, out: ?*vs.Map, user_data: ?*anyopaque, core_ptr: ?*vs.Core, vsapi: *const vs.API) callconv(.c) void {
    _ = user_data;
    const zapi = ZAPI.init(vsapi, core_ptr, null);
    const map_in = zapi.initZMap(in);
    const map_out = zapi.initZMap(out);
    const source = map_in.getNodeVi2("clip") orelse {
        map_out.setError("Restore: clip is required");
        return;
    };
    const plugin = zapi.getPluginByID("com.ifb.composite") orelse {
        zapi.freeNode(source.node);
        map_out.setError("Restore: plugin registry lookup failed");
        return;
    };
    const original_node = zapi.addNodeRef(source.node).?;
    const source_width = source.vi.width;
    const output_width = map_in.getInt(i32, "width") orelse source_width;
    var encode_args = zapi.createZMap();
    _ = encode_args.setNode("clip", source.node, .Replace);
    if (map_in.getDataZ("standard", 0)) |value| encode_args.setData("standard", value, .Utf8, .Replace);
    encode_args.setInt("setup", map_in.getInt(i32, "setup") orelse 0, .Replace);
    encode_args.setInt("precomb", map_in.getInt(i32, "precomb") orelse 0, .Replace);
    var encoded = encode_args.invoke(plugin, "Encode");
    encode_args.free();
    zapi.freeNode(source.node);
    if (encoded.getError() != null) {
        map_out.setError("Restore: Encode stage failed");
        zapi.freeNode(original_node);
        encoded.free();
        return;
    }
    const encoded_node = encoded.getNode("clip") orelse {
        zapi.freeNode(original_node);
        encoded.free();
        map_out.setError("Restore: Encode stage returned no clip");
        return;
    };
    encoded.free();

    var decode_args = zapi.createZMap();
    _ = decode_args.setNode("clip", encoded_node, .Replace);
    decode_args.setInt("width", output_width, .Replace);
    if (map_in.getDataZ("standard", 0)) |value| decode_args.setData("standard", value, .Utf8, .Replace);
    copyIntOrFloat(map_in, decode_args, "threshold");
    copyIntOrFloat(map_in, decode_args, "setup");
    copyIntOrFloat(map_in, decode_args, "dimensions");
    copyIntOrFloat(map_in, decode_args, "eq");
    copyIntOrFloat(map_in, decode_args, "refine");
    copyIntOrFloat(map_in, decode_args, "transform");
    copyIntOrFloat(map_in, decode_args, "level");
    copyIntOrFloat(map_in, decode_args, "evidence");
    copyIntOrFloat(map_in, decode_args, "cti");
    if (map_in.getFloatArray("thresholds")) |values| decode_args.setFloatArray("thresholds", values);
    if (map_in.getFloatArray("lut")) |values| decode_args.setFloatArray("lut", values);
    if (map_in.getDataZ("mask", 0)) |value| decode_args.setData("mask", value, .Utf8, .Replace);
    _ = decode_args.setNode("orig", original_node, .Replace);
    var decoded = decode_args.invoke(plugin, "Decode");
    decode_args.free();
    zapi.freeNode(encoded_node);
    zapi.freeNode(original_node);
    if (decoded.getError() != null) {
        map_out.setError("Restore: Decode stage failed");
        decoded.free();
        return;
    }
    const count = decoded.numElements("clip") orelse 0;
    for (0..count) |index| {
        var err: vs.MapPropertyError = undefined;
        const node = zapi.mapGetNode(decoded.map, "clip", @intCast(index), &err);
        if (err == .Success) _ = map_out.consumeNode("clip", node, .Append);
    }
    decoded.free();
}

export fn VapourSynthPluginInit2(plugin: *vs.Plugin, vspapi: *const vs.PLUGINAPI) void {
    ZAPI.Plugin.config("com.ifb.composite", "composite", "PAL/NTSC composite video encoder/decoder (Zig)", zon_version, plugin, vspapi);
    ZAPI.Plugin.function("Encode", "clip:vnode;standard:data:opt;setup:int:opt;precomb:int:opt;", "clip:vnode;", encodeCreate, plugin, vspapi);
    ZAPI.Plugin.function("Decode", "clip:vnode;standard:data:opt;width:int:opt;threshold:float:opt;setup:int:opt;dimensions:int:opt;eq:int:opt;thresholds:float[]:opt;transform:int:opt;level:int:opt;lut:float[]:opt;evidence:float:opt;cti:int:opt;mask:data:opt;refine:int:opt;orig:vnode:opt;", "clip:vnode[];", decodeCreate, plugin, vspapi);
    ZAPI.Plugin.function("Restore", "clip:vnode;standard:data:opt;width:int:opt;threshold:float:opt;setup:int:opt;dimensions:int:opt;eq:int:opt;refine:int:opt;thresholds:float[]:opt;precomb:int:opt;transform:int:opt;level:int:opt;lut:float[]:opt;evidence:float:opt;cti:int:opt;mask:data:opt;", "clip:vnode[];", restoreCreate, plugin, vspapi);
}
