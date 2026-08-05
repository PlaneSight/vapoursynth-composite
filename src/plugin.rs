//! VapourSynth adapter for the signal core.
//!
//! The safe `vapoursynth-rs` filter API deliberately keeps host ownership
//! outside the numerical core. Temporal dependencies are requested manually
//! here, while the decoder itself receives a checked source window.

#![allow(unsafe_code)]

use std::sync::Mutex;

use anyhow::{Error, anyhow, bail, ensure};
use vapoursynth::core::CoreRef;
use vapoursynth::format::{ColorFamily, PresetFormat, SampleType};
use vapoursynth::map::ValueIter;
use vapoursynth::plugins::{Filter, FilterArgument, FrameContext, Metadata};
use vapoursynth::prelude::*;
use vapoursynth::video_info::{Property, Resolution, VideoInfo};
use vapoursynth::{export_vapoursynth_plugin, make_filter_function};

#[path = "resample.rs"]
mod resample;

use self::resample::{
    HorizontalBilinearResampler, HorizontalResampler, ResampleGeometry, decode_edge_columns,
    decode_geometry, encode_geometry,
};
use crate::decode::{
    DecodeReport, DecodeScratch, DecoderOptions, EqualizerMode, MaskKind, NtscTemporalMode,
};
use crate::{
    DecodeMode, Decoder, Encoder, FrameIndex, GraySink, GraySource, Setup, Standard, YuvSink,
    YuvSource,
};

const PLUGIN_ID: &str = "com.planesight.composite";
const MAX_TEMPORAL_WINDOW: usize = 9;
const MAX_RASTER_WIDTH: usize = 928;
static ZERO_RASTER_ROW: [u16; MAX_RASTER_WIDTH] = [0; MAX_RASTER_WIDTH];

#[derive(Clone, Copy, Debug)]
struct InputContract {
    resolution: Resolution,
}

struct OutputGeometry {
    resolution: Resolution,
    picture_resampler: Option<HorizontalResampler>,
    mask_resampler: Option<HorizontalBilinearResampler>,
    edge_columns: (usize, usize),
}

fn constant_resolution(node: &Node<'_>) -> anyhow::Result<Resolution> {
    match node.info().resolution {
        Property::Constant(resolution) => Ok(resolution),
        Property::Variable => bail!("clip must have constant dimensions"),
    }
}

fn validate_yuv_input(node: &Node<'_>, standard: Standard) -> anyhow::Result<InputContract> {
    let info = node.info();
    let resolution = constant_resolution(node)?;
    ensure!(
        info.format.color_family() == ColorFamily::YUV,
        "clip must be YUV"
    );
    standard
        .raster()
        .row_offset(standard, resolution.height, false)?;
    Ok(InputContract { resolution })
}

fn validate_yuv444p16_raster(node: &Node<'_>, standard: Standard) -> anyhow::Result<InputContract> {
    let info = node.info();
    let contract = validate_yuv_input(node, standard)?;
    ensure!(
        info.format.sample_type() == SampleType::Integer
            && info.format.bits_per_sample() == 16
            && info.format.sub_sampling_w() == 0
            && info.format.sub_sampling_h() == 0,
        "internal raster must be YUV444P16"
    );
    ensure!(
        contract.resolution.width == standard.raster().width,
        "internal raster width must be {} for {standard:?}",
        standard.raster().width
    );
    Ok(contract)
}

fn validate_gray16_raster(node: &Node<'_>, standard: Standard) -> anyhow::Result<InputContract> {
    let info = node.info();
    let resolution = constant_resolution(node)?;
    ensure!(
        info.format.color_family() == ColorFamily::Gray
            && info.format.sample_type() == SampleType::Integer
            && info.format.bits_per_sample() == 16,
        "clip must be GRAY16 composite"
    );
    ensure!(
        resolution.width == standard.raster().width,
        "clip width must be {} for {standard:?}",
        standard.raster().width
    );
    standard
        .raster()
        .row_offset(standard, resolution.height, false)?;
    Ok(InputContract { resolution })
}

fn parse_standard(value: Option<&[u8]>) -> anyhow::Result<Standard> {
    Ok(Standard::parse(value.unwrap_or(b"pal"))?)
}

fn parse_setup(value: Option<i64>) -> Setup {
    if value.unwrap_or(0) == 0 {
        Setup::None
    } else {
        Setup::Ire7_5
    }
}

fn parse_output_width(value: Option<i64>, default: usize) -> anyhow::Result<usize> {
    let width = value.unwrap_or(default as i64);
    ensure!(
        width == 0 || (16..=8_192).contains(&width),
        "width must be 0 (raw raster) or between 16 and 8192"
    );
    Ok(width as usize)
}

fn parse_mask(value: Option<&[u8]>) -> anyhow::Result<Option<MaskKind>> {
    match value {
        None => Ok(None),
        Some(b"motion") => Ok(Some(MaskKind::Motion)),
        Some(b"confidence") => Ok(Some(MaskKind::Confidence)),
        Some(_) => bail!("mask must be \"motion\" or \"confidence\""),
    }
}

fn collect_float_arguments(values: Option<impl Iterator<Item = f64>>) -> Option<Vec<f32>> {
    values.map(|values| values.map(|value| value as f32).collect())
}

#[allow(clippy::too_many_arguments)]
fn parse_decoder_options(
    standard: Standard,
    setup: Setup,
    dimensions: Option<i64>,
    threshold: Option<f64>,
    eq: Option<i64>,
    thresholds: Option<impl Iterator<Item = f64>>,
    transform: Option<i64>,
    level: Option<i64>,
    lut: Option<impl Iterator<Item = f64>>,
    evidence: Option<f64>,
    cti: Option<i64>,
    mask: Option<&[u8]>,
) -> anyhow::Result<(Decoder, Option<MaskKind>)> {
    let mode = DecodeMode::from_dimensions(dimensions.unwrap_or(3))?;
    let transform = transform.unwrap_or(
        if standard == Standard::Ntsc && mode == DecodeMode::TemporalTransform {
            2
        } else {
            0
        },
    );
    ensure!(
        (0..=2).contains(&transform),
        "transform must be 0 (comb), 1 (transform), or 2 (hybrid)"
    );
    ensure!(
        transform == 0 || standard == Standard::Ntsc,
        "transform applies to NTSC; PAL always uses the spectral transform"
    );
    ensure!(
        transform == 0 || mode == DecodeMode::TemporalTransform,
        "transform needs dimensions=3"
    );
    let ntsc_temporal = match transform {
        0 => NtscTemporalMode::Comb,
        1 => NtscTemporalMode::Transform,
        2 => NtscTemporalMode::Hybrid,
        _ => unreachable!("transform range was validated"),
    };
    let has_transform = match standard {
        Standard::Pal => mode != DecodeMode::Notch,
        Standard::Ntsc => mode == DecodeMode::TemporalTransform && transform != 0,
    };
    let equalizer = match eq.unwrap_or(if has_transform { 2 } else { 1 }) {
        0 => EqualizerMode::Off,
        1 => EqualizerMode::Fixed,
        2 => EqualizerMode::LeakAware,
        _ => bail!("eq must be 0 (off), 1 (fixed), or 2 (leak-aware)"),
    };
    ensure!(
        equalizer != EqualizerMode::LeakAware || has_transform,
        "eq=2 needs a transform separation"
    );
    let cti = cti.unwrap_or(0) != 0;
    ensure!(
        !cti || mode != DecodeMode::Notch,
        "cti needs dimensions 2 or 3"
    );
    let amplitude_limit = level.unwrap_or(0) != 0;
    ensure!(
        !amplitude_limit || has_transform,
        "level=1 needs a transform separation"
    );

    let threshold_values = collect_float_arguments(thresholds);
    let lut_values = collect_float_arguments(lut);
    ensure!(
        !(amplitude_limit && threshold_values.is_some()),
        "thresholds need threshold mode (level=0)"
    );
    ensure!(
        !(amplitude_limit && lut_values.is_some()),
        "lut and level are mutually exclusive"
    );
    ensure!(
        !(threshold_values.is_some() && lut_values.is_some()),
        "lut and thresholds are mutually exclusive"
    );

    let use_builtin_lut = has_transform
        && threshold.is_none()
        && threshold_values.is_none()
        && lut_values.is_none()
        && !amplitude_limit;
    let mut options = DecoderOptions::new(mode)
        .with_ntsc_temporal_mode(ntsc_temporal)
        .with_equalizer(equalizer)
        .with_cti(cti)
        .with_evidence(evidence.unwrap_or(0.0) as f32)
        .with_builtin_lut(use_builtin_lut);
    if let Some(value) = threshold {
        options = options.with_threshold(value as f32);
    }
    if let Some(values) = threshold_values {
        options = options.with_thresholds(values);
    }
    if amplitude_limit {
        options = options.with_amplitude_limit(true);
    }
    if let Some(values) = lut_values {
        options = options.with_lut(values);
    }
    let mask = parse_mask(mask)?;
    if matches!(mask, Some(MaskKind::Motion)) {
        ensure!(
            standard == Standard::Ntsc
                && mode == DecodeMode::TemporalTransform
                && ntsc_temporal == NtscTemporalMode::Hybrid,
            "mask=\"motion\" needs NTSC dimensions=3 transform=2"
        );
    }
    if matches!(mask, Some(MaskKind::Confidence)) {
        ensure!(
            equalizer == EqualizerMode::LeakAware,
            "mask=\"confidence\" needs eq=2"
        );
    }
    Ok((Decoder::with_options(standard, setup, options)?, mask))
}

fn row_offset(standard: Standard, frame: &FrameRef<'_>) -> anyhow::Result<usize> {
    let top_field_first = frame
        .props()
        .get_int("_FieldBased")
        .is_ok_and(|value| value == 2);
    Ok(standard
        .raster()
        .row_offset(standard, frame.height(0), top_field_first)?)
}

fn make_output_geometry(
    standard: Standard,
    width: usize,
    height: usize,
) -> anyhow::Result<OutputGeometry> {
    if width == 0 {
        return Ok(OutputGeometry {
            resolution: Resolution {
                width: standard.raster().width,
                height,
            },
            picture_resampler: None,
            mask_resampler: None,
            edge_columns: (0, 0),
        });
    }
    let geometry = decode_geometry(standard, width)?;
    Ok(OutputGeometry {
        resolution: Resolution { width, height },
        picture_resampler: Some(HorizontalResampler::new(
            standard.raster().width,
            width,
            geometry.src_left,
            geometry.src_width,
        )?),
        mask_resampler: Some(HorizontalBilinearResampler::new(
            standard.raster().width,
            width,
            geometry.src_left,
            geometry.src_width,
        )?),
        edge_columns: decode_edge_columns(standard, width)?,
    })
}

fn invoke_spline36<'core>(
    api: API,
    core: CoreRef<'core>,
    source: &Node<'core>,
    resolution: Resolution,
    geometry: Option<ResampleGeometry>,
) -> anyhow::Result<Node<'core>> {
    let resize = core
        .get_plugin_by_id("com.vapoursynth.resize")?
        .ok_or_else(|| anyhow!("resize plugin not found"))?;
    let mut arguments = OwnedMap::new(api);
    arguments.set_node("clip", source)?;
    arguments.set_int("format", i64::from(PresetFormat::YUV444P16 as i32))?;
    arguments.set_int("width", resolution.width as i64)?;
    arguments.set_int("height", resolution.height as i64)?;
    if let Some(geometry) = geometry {
        arguments.set_float("src_left", geometry.src_left)?;
        arguments.set_float("src_width", geometry.src_width)?;
    }
    let result = resize.invoke("Spline36", &arguments)?;
    if let Some(error) = result.error() {
        bail!("Spline36 conversion failed: {error}");
    }
    Ok(result.get_video_node("clip")?)
}

fn resample_to_raster<'core>(
    api: API,
    core: CoreRef<'core>,
    source: &Node<'core>,
    standard: Standard,
    input: InputContract,
) -> anyhow::Result<Node<'core>> {
    let geometry = encode_geometry(standard, input.resolution.width);
    invoke_spline36(
        api,
        core,
        source,
        Resolution {
            width: geometry.output_width,
            height: input.resolution.height,
        },
        Some(geometry),
    )
}

fn output_info<'core>(
    core: CoreRef<'core>,
    input: VideoInfo<'core>,
    preset: PresetFormat,
    resolution: Resolution,
) -> anyhow::Result<VideoInfo<'core>> {
    let format = core
        .get_format(preset.into())
        .ok_or_else(|| anyhow!("VapourSynth format {preset:?} is unavailable"))?;
    Ok(VideoInfo {
        format,
        resolution: Property::Constant(resolution),
        ..input
    })
}

fn new_frame<'core>(
    core: CoreRef<'core>,
    source: &FrameRef<'core>,
    preset: PresetFormat,
    resolution: Resolution,
) -> anyhow::Result<FrameRefMut<'core>> {
    let format = core
        .get_format(preset.into())
        .ok_or_else(|| anyhow!("VapourSynth format {preset:?} is unavailable"))?;
    // SAFETY: every active sample in every output plane is initialized before
    // the frame crosses the host ABI. The only exception is the property-only
    // mask frame, whose single plane is likewise written in full.
    Ok(unsafe { FrameRefMut::new_uninitialized(core, Some(source), format, resolution) })
}

struct FrameYuvSource<'frame, 'core>(&'frame FrameRef<'core>);

impl YuvSource for FrameYuvSource<'_, '_> {
    fn dimensions(&self) -> (usize, usize) {
        (self.0.width(0), self.0.height(0))
    }

    fn row(&self, plane: usize, row: usize) -> &[u16] {
        self.0.plane_row::<u16>(plane, row)
    }
}

struct FrameGraySource<'frame, 'core>(&'frame FrameRef<'core>);

impl GraySource for FrameGraySource<'_, '_> {
    fn dimensions(&self) -> (usize, usize) {
        (self.0.width(0), self.0.height(0))
    }

    fn row(&self, row: usize) -> &[u16] {
        self.0.plane_row::<u16>(0, row)
    }
}

struct FrameGraySink<'frame, 'core>(&'frame mut FrameRefMut<'core>);

impl GraySink for FrameGraySink<'_, '_> {
    fn dimensions(&self) -> (usize, usize) {
        (self.0.width(0), self.0.height(0))
    }

    fn write_row(&mut self, row: usize, values: &[u16]) {
        self.0.plane_row_mut::<u16>(0, row).copy_from_slice(values);
    }
}

struct ZeroGraySource {
    width: usize,
    height: usize,
}

impl GraySource for ZeroGraySource {
    fn dimensions(&self) -> (usize, usize) {
        (self.width, self.height)
    }

    fn row(&self, _row: usize) -> &[u16] {
        &ZERO_RASTER_ROW[..self.width]
    }
}

struct PackedGray {
    width: usize,
    height: usize,
    samples: Vec<u16>,
}

impl PackedGray {
    fn new(width: usize, height: usize) -> anyhow::Result<Self> {
        let length = width
            .checked_mul(height)
            .ok_or_else(|| anyhow!("frame dimensions overflow"))?;
        Ok(Self {
            width,
            height,
            samples: vec![0; length],
        })
    }
}

impl GraySource for PackedGray {
    fn dimensions(&self) -> (usize, usize) {
        (self.width, self.height)
    }

    fn row(&self, row: usize) -> &[u16] {
        let start = row * self.width;
        &self.samples[start..start + self.width]
    }
}

impl GraySink for PackedGray {
    fn dimensions(&self) -> (usize, usize) {
        (self.width, self.height)
    }

    fn write_row(&mut self, row: usize, values: &[u16]) {
        let start = row * self.width;
        self.samples[start..start + self.width].copy_from_slice(values);
    }
}

struct PackedYuv {
    width: usize,
    height: usize,
    planes: [Vec<u16>; 3],
}

impl PackedYuv {
    fn new(width: usize, height: usize) -> anyhow::Result<Self> {
        let length = width
            .checked_mul(height)
            .ok_or_else(|| anyhow!("frame dimensions overflow"))?;
        Ok(Self {
            width,
            height,
            planes: std::array::from_fn(|_| vec![0; length]),
        })
    }

    fn plane_row_mut(&mut self, plane: usize, row: usize) -> &mut [u16] {
        let start = row * self.width;
        &mut self.planes[plane][start..start + self.width]
    }
}

impl YuvSource for PackedYuv {
    fn dimensions(&self) -> (usize, usize) {
        (self.width, self.height)
    }

    fn row(&self, plane: usize, row: usize) -> &[u16] {
        let start = row * self.width;
        &self.planes[plane][start..start + self.width]
    }
}

impl YuvSink for PackedYuv {
    fn dimensions(&self) -> (usize, usize) {
        (self.width, self.height)
    }

    fn write_row(&mut self, row: usize, y: &[u16], u: &[u16], v: &[u16]) {
        self.plane_row_mut(0, row).copy_from_slice(y);
        self.plane_row_mut(1, row).copy_from_slice(u);
        self.plane_row_mut(2, row).copy_from_slice(v);
    }
}

fn write_yuv(
    source: &impl YuvSource,
    output: &mut FrameRefMut<'_>,
    resampler: Option<&HorizontalResampler>,
) {
    let (_, height) = source.dimensions();
    for plane in 0..3 {
        for row in 0..height {
            let destination = output.plane_row_mut::<u16>(plane, row);
            if let Some(resampler) = resampler {
                resampler.resample_row(source.row(plane, row), destination);
            } else {
                destination.copy_from_slice(source.row(plane, row));
            }
        }
    }
}

fn write_gray(
    source: &impl GraySource,
    output: &mut FrameRefMut<'_>,
    resampler: Option<&HorizontalBilinearResampler>,
) {
    let (_, height) = source.dimensions();
    for row in 0..height {
        let destination = output.plane_row_mut::<u16>(0, row);
        if let Some(resampler) = resampler {
            resampler.resample_row(source.row(row), destination);
        } else {
            destination.copy_from_slice(source.row(row));
        }
    }
}

fn splice_edges(output: &mut FrameRefMut<'_>, source: &impl YuvSource, left: usize, right: usize) {
    if left == 0 && right == 0 {
        return;
    }
    let (width, height) = source.dimensions();
    for plane in 0..3 {
        for row in 0..height {
            let source_row = source.row(plane, row);
            let destination = output.plane_row_mut::<u16>(plane, row);
            if left > 0 {
                destination[..left].copy_from_slice(&source_row[..left]);
            }
            if right > 0 {
                destination[width - right..].copy_from_slice(&source_row[width - right..]);
            }
        }
    }
}

fn set_output_properties(
    output: &mut FrameRefMut<'_>,
    report: DecodeReport,
    refinement: Option<(f64, f64)>,
    mask: Option<&FrameRef<'_>>,
) -> anyhow::Result<()> {
    let mut properties = output.props_mut();
    if let (Some(mean), Some(std_dev)) = (report.confidence_mean, report.confidence_std_dev) {
        properties.set_float("CompositeSeparationConfidenceMean", mean)?;
        properties.set_float("CompositeSeparationConfidenceStdDev", std_dev)?;
    }
    if let Some(motion) = report.motion_fraction {
        properties.set_float("CompositeMotionFraction", motion)?;
    }
    if let Some((residual, correction)) = refinement {
        properties.set_float("CompositeRefineResidual", residual)?;
        properties.set_float("CompositeRefineCorrection", correction)?;
    }
    if let Some(mask) = mask {
        properties.set_frame("CompositeMask", mask)?;
    }
    Ok(())
}

fn window_frame_index(n: usize, look: usize, slot: usize, frame_count: usize) -> Option<usize> {
    if slot < look {
        n.checked_sub(look - slot)
    } else {
        n.checked_add(slot - look)
            .filter(|&index| index < frame_count)
    }
}

fn request_window(
    source: &Node<'_>,
    context: FrameContext,
    n: usize,
    look: usize,
    frame_count: usize,
) {
    let count = look * 2 + 1;
    let mut requested = [usize::MAX; MAX_TEMPORAL_WINDOW];
    for slot in 0..count {
        let Some(index) = window_frame_index(n, look, slot, frame_count) else {
            continue;
        };
        if requested[..slot].contains(&index) {
            continue;
        }
        source.request_frame_filter(context, index);
        requested[slot] = index;
    }
}

fn fetch_window<'core>(
    source: &Node<'core>,
    context: FrameContext,
    n: usize,
    look: usize,
    frame_count: usize,
) -> anyhow::Result<[Option<FrameRef<'core>>; MAX_TEMPORAL_WINDOW]> {
    let count = look * 2 + 1;
    let mut frames: [Option<FrameRef<'core>>; MAX_TEMPORAL_WINDOW] = std::array::from_fn(|_| None);
    for (slot, frame) in frames.iter_mut().enumerate().take(count) {
        let Some(index) = window_frame_index(n, look, slot, frame_count) else {
            continue;
        };
        *frame = Some(
            source
                .get_frame_filter(context, index)
                .ok_or_else(|| anyhow!("source frame {index} was not returned"))?,
        );
    }
    Ok(frames)
}

struct DecodeWork {
    scratch: DecodeScratch,
    decoded: PackedYuv,
    mask: PackedGray,
}

impl DecodeWork {
    fn new(width: usize, height: usize) -> anyhow::Result<Self> {
        Ok(Self {
            scratch: DecodeScratch::new(width, height)?,
            decoded: PackedYuv::new(width, height)?,
            mask: PackedGray::new(width, height)?,
        })
    }
}

fn decode_requested_window<'core>(
    decoder: &Decoder,
    standard: Standard,
    source: &Node<'core>,
    context: FrameContext,
    n: usize,
    scratch: &mut DecodeScratch,
    output: &mut PackedYuv,
) -> Result<(DecodeReport, FrameRef<'core>), Error> {
    let look = decoder.look();
    let frame_count = source.info().num_frames;
    let mut frames = fetch_window(source, context, n, look, frame_count)?;
    let offset = row_offset(
        standard,
        frames[look]
            .as_ref()
            .ok_or_else(|| anyhow!("center source frame {n} was not returned"))?,
    )?;
    let count = look * 2 + 1;
    let indices: [FrameIndex; MAX_TEMPORAL_WINDOW] = std::array::from_fn(|slot| {
        FrameIndex(window_frame_index(n, look, slot, frame_count).unwrap_or(n))
    });
    let report = if look == 0 {
        let current = frames[look]
            .as_ref()
            .ok_or_else(|| anyhow!("center source frame {n} was not returned"))?;
        let source = FrameGraySource(current);
        let window: [&dyn GraySource; 1] = [&source];
        decoder.decode_window_with_scratch(offset, &window, &indices[..1], output, scratch)?
    } else {
        let current = frames[look]
            .as_ref()
            .ok_or_else(|| anyhow!("center source frame {n} was not returned"))?;
        let zero = ZeroGraySource {
            width: current.width(0),
            height: current.height(0),
        };
        let sources: [Option<FrameGraySource<'_, 'core>>; MAX_TEMPORAL_WINDOW] =
            std::array::from_fn(|slot| frames[slot].as_ref().map(FrameGraySource));
        let mut window: [&dyn GraySource; MAX_TEMPORAL_WINDOW] =
            std::array::from_fn(|_| &zero as &dyn GraySource);
        for slot in 0..count {
            if let Some(source) = sources[slot].as_ref() {
                window[slot] = source;
            }
        }
        decoder.decode_window_with_scratch(
            offset,
            &window[..count],
            &indices[..count],
            output,
            scratch,
        )?
    };
    let current = frames[look]
        .take()
        .ok_or_else(|| anyhow!("center source frame {n} was not returned"))?;
    Ok((report, current))
}

struct EncodeFilter<'core> {
    source: Node<'core>,
    output: VideoInfo<'core>,
    resolution: Resolution,
    encoder: Encoder,
}

impl<'core> Filter<'core> for EncodeFilter<'core> {
    fn video_info(&self, _api: API, _core: CoreRef<'core>) -> Vec<VideoInfo<'core>> {
        vec![self.output]
    }

    fn get_frame_initial(
        &self,
        _api: API,
        _core: CoreRef<'core>,
        context: FrameContext,
        n: usize,
    ) -> Result<Option<FrameRef<'core>>, Error> {
        self.source.request_frame_filter(context, n);
        Ok(None)
    }

    fn get_frame(
        &self,
        _api: API,
        core: CoreRef<'core>,
        context: FrameContext,
        n: usize,
    ) -> Result<FrameRef<'core>, Error> {
        let source = self
            .source
            .get_frame_filter(context, n)
            .ok_or_else(|| anyhow!("source frame {n} was not returned"))?;
        let offset = row_offset(self.encoder.standard(), &source)?;
        let mut output = new_frame(core, &source, PresetFormat::Gray16, self.resolution)?;
        self.encoder.encode_source(
            FrameIndex(n),
            offset,
            &FrameYuvSource(&source),
            &mut FrameGraySink(&mut output),
        )?;
        Ok(output.into())
    }
}

struct DecodeFilter<'core> {
    source: Node<'core>,
    output: VideoInfo<'core>,
    standard: Standard,
    decoder: Decoder,
    picture_resampler: Option<HorizontalResampler>,
    mask_resampler: Option<HorizontalBilinearResampler>,
    mask_kind: Option<MaskKind>,
    work: Mutex<DecodeWork>,
}

impl<'core> Filter<'core> for DecodeFilter<'core> {
    fn video_info(&self, _api: API, _core: CoreRef<'core>) -> Vec<VideoInfo<'core>> {
        vec![self.output]
    }

    fn get_frame_initial(
        &self,
        _api: API,
        _core: CoreRef<'core>,
        context: FrameContext,
        n: usize,
    ) -> Result<Option<FrameRef<'core>>, Error> {
        request_window(
            &self.source,
            context,
            n,
            self.decoder.look(),
            self.source.info().num_frames,
        );
        Ok(None)
    }

    fn get_frame(
        &self,
        _api: API,
        core: CoreRef<'core>,
        context: FrameContext,
        n: usize,
    ) -> Result<FrameRef<'core>, Error> {
        let mut work = self
            .work
            .lock()
            .map_err(|_| anyhow!("decode workspace lock was poisoned"))?;
        let (report, source) = {
            let DecodeWork {
                scratch, decoded, ..
            } = &mut *work;
            decode_requested_window(
                &self.decoder,
                self.standard,
                &self.source,
                context,
                n,
                scratch,
                decoded,
            )?
        };
        let resolution = match self.output.resolution {
            Property::Constant(resolution) => resolution,
            Property::Variable => unreachable!("filter output dimensions are constant"),
        };
        let mut output = new_frame(core, &source, PresetFormat::YUV444P16, resolution)?;
        write_yuv(&work.decoded, &mut output, self.picture_resampler.as_ref());
        let mask = if let Some(kind) = self.mask_kind {
            let DecodeWork { scratch, mask, .. } = &mut *work;
            scratch.write_mask(kind, mask)?;
            let mut frame = new_frame(core, &source, PresetFormat::Gray16, resolution)?;
            write_gray(&work.mask, &mut frame, self.mask_resampler.as_ref());
            Some(FrameRef::from(frame))
        } else {
            None
        };
        set_output_properties(&mut output, report, None, mask.as_ref())?;
        Ok(output.into())
    }
}

struct RestoreWork {
    scratch: DecodeScratch,
    decoded: PackedYuv,
    composites: Vec<PackedGray>,
    recomposite: PackedGray,
    crude: PackedYuv,
    crude_scratch: DecodeScratch,
    initial_luma: Vec<u16>,
    mask: PackedGray,
}

impl RestoreWork {
    fn new(width: usize, height: usize) -> anyhow::Result<Self> {
        let length = width
            .checked_mul(height)
            .ok_or_else(|| anyhow!("frame dimensions overflow"))?;
        let mut composites = Vec::with_capacity(MAX_TEMPORAL_WINDOW);
        for _ in 0..MAX_TEMPORAL_WINDOW {
            composites.push(PackedGray::new(width, height)?);
        }
        Ok(Self {
            scratch: DecodeScratch::new(width, height)?,
            decoded: PackedYuv::new(width, height)?,
            composites,
            recomposite: PackedGray::new(width, height)?,
            crude: PackedYuv::new(width, height)?,
            crude_scratch: DecodeScratch::new(width, height)?,
            initial_luma: vec![0; length],
            mask: PackedGray::new(width, height)?,
        })
    }
}

#[allow(clippy::too_many_arguments)]
fn refine_luma(
    encoder: &Encoder,
    decoder: &Decoder,
    work: &mut RestoreWork,
    frame: FrameIndex,
    row_offset: usize,
    original: &impl YuvSource,
    iterations: usize,
) -> Result<(f64, f64), Error> {
    work.initial_luma.copy_from_slice(&work.decoded.planes[0]);
    let (width, height) = original.dimensions();
    let mut residual_sum = 0_i64;
    for iteration in 0..iterations {
        encoder.encode_source(frame, row_offset, &work.decoded, &mut work.recomposite)?;
        decoder.decode_source_with_scratch(
            frame,
            row_offset,
            &work.recomposite,
            &mut work.crude,
            &mut work.crude_scratch,
        )?;
        residual_sum = 0;
        for row in 0..height {
            let original_row = original.row(0, row);
            let decoded_row = work.decoded.plane_row_mut(0, row);
            let crude_row = work.crude.row(0, row);
            for column in 0..width {
                let residual = i32::from(original_row[column]) - i32::from(crude_row[column]);
                if iteration + 1 == iterations {
                    residual_sum += i64::from(residual.abs());
                }
                decoded_row[column] = (i32::from(decoded_row[column]) + residual)
                    .clamp(0, i32::from(u16::MAX)) as u16;
            }
        }
    }
    let correction_sum = work.decoded.planes[0]
        .iter()
        .zip(&work.initial_luma)
        .map(|(&decoded, &initial)| i64::from((i32::from(decoded) - i32::from(initial)).abs()))
        .sum::<i64>();
    let samples = (width * height) as f64;
    Ok((
        residual_sum as f64 / samples,
        correction_sum as f64 / samples,
    ))
}

struct RestoreFilter<'core> {
    source: Node<'core>,
    edge_source: Option<Node<'core>>,
    output: VideoInfo<'core>,
    standard: Standard,
    encoder: Encoder,
    refine_encoder: Encoder,
    decoder: Decoder,
    crude_decoder: Decoder,
    refine: usize,
    picture_resampler: Option<HorizontalResampler>,
    mask_resampler: Option<HorizontalBilinearResampler>,
    edge_columns: (usize, usize),
    mask_kind: Option<MaskKind>,
    work: Mutex<RestoreWork>,
}

impl<'core> Filter<'core> for RestoreFilter<'core> {
    fn video_info(&self, _api: API, _core: CoreRef<'core>) -> Vec<VideoInfo<'core>> {
        vec![self.output]
    }

    fn get_frame_initial(
        &self,
        _api: API,
        _core: CoreRef<'core>,
        context: FrameContext,
        n: usize,
    ) -> Result<Option<FrameRef<'core>>, Error> {
        request_window(
            &self.source,
            context,
            n,
            self.decoder.look(),
            self.source.info().num_frames,
        );
        if let Some(edge_source) = self.edge_source.as_ref() {
            edge_source.request_frame_filter(context, n);
        }
        Ok(None)
    }

    fn get_frame(
        &self,
        _api: API,
        core: CoreRef<'core>,
        context: FrameContext,
        n: usize,
    ) -> Result<FrameRef<'core>, Error> {
        let look = self.decoder.look();
        let frame_count = self.source.info().num_frames;
        let count = look * 2 + 1;
        let frames = fetch_window(&self.source, context, n, look, frame_count)?;
        let current = frames[look]
            .as_ref()
            .ok_or_else(|| anyhow!("center source frame {n} was not returned"))?;
        let current_offset = row_offset(self.standard, current)?;
        let indices: [FrameIndex; MAX_TEMPORAL_WINDOW] = std::array::from_fn(|slot| {
            FrameIndex(window_frame_index(n, look, slot, frame_count).unwrap_or(n))
        });
        let mut work = self
            .work
            .lock()
            .map_err(|_| anyhow!("restore workspace lock was poisoned"))?;
        for slot in 0..count {
            if let Some(source) = frames[slot].as_ref() {
                let offset = row_offset(self.standard, source)?;
                self.encoder.encode_source(
                    indices[slot],
                    offset,
                    &FrameYuvSource(source),
                    &mut work.composites[slot],
                )?;
            } else {
                work.composites[slot].samples.fill(0);
            }
        }
        let report = {
            let RestoreWork {
                composites,
                decoded,
                scratch,
                ..
            } = &mut *work;
            let window: [&dyn GraySource; MAX_TEMPORAL_WINDOW] =
                std::array::from_fn(|slot| &composites[slot] as &dyn GraySource);
            self.decoder.decode_window_with_scratch(
                current_offset,
                &window[..count],
                &indices[..count],
                decoded,
                scratch,
            )?
        };
        let refinement = if self.refine > 0 {
            Some(refine_luma(
                &self.refine_encoder,
                &self.crude_decoder,
                &mut work,
                FrameIndex(n),
                current_offset,
                &FrameYuvSource(current),
                self.refine,
            )?)
        } else {
            None
        };
        let resolution = match self.output.resolution {
            Property::Constant(resolution) => resolution,
            Property::Variable => unreachable!("filter output dimensions are constant"),
        };
        let mut output = new_frame(core, current, PresetFormat::YUV444P16, resolution)?;
        write_yuv(&work.decoded, &mut output, self.picture_resampler.as_ref());
        if let Some(edge_source) = self.edge_source.as_ref() {
            let edge = edge_source
                .get_frame_filter(context, n)
                .ok_or_else(|| anyhow!("edge source frame {n} was not returned"))?;
            splice_edges(
                &mut output,
                &FrameYuvSource(&edge),
                self.edge_columns.0,
                self.edge_columns.1,
            );
        }
        let mask = if let Some(kind) = self.mask_kind {
            let RestoreWork { scratch, mask, .. } = &mut *work;
            scratch.write_mask(kind, mask)?;
            let mut frame = new_frame(core, current, PresetFormat::Gray16, resolution)?;
            write_gray(&work.mask, &mut frame, self.mask_resampler.as_ref());
            Some(FrameRef::from(frame))
        } else {
            None
        };
        set_output_properties(&mut output, report, refinement, mask.as_ref())?;
        Ok(output.into())
    }
}

make_filter_function! {
    EncodeFunction, "Encode"
    fn create_encode<'core>(
        api: API,
        core: CoreRef<'core>,
        clip: Node<'core>,
        standard: Option<&[u8]>,
        setup: Option<i64>,
        precomb: Option<i64>,
    ) -> Result<Option<Box<dyn Filter<'core> + 'core>>, Error> {
        let standard = parse_standard(standard)?;
        let input = validate_yuv_input(&clip, standard)?;
        let source = resample_to_raster(api, core, &clip, standard, input)?;
        let contract = validate_yuv444p16_raster(&source, standard)?;
        let output = output_info(core, source.info(), PresetFormat::Gray16, contract.resolution)?;
        Ok(Some(Box::new(EncodeFilter {
            source,
            output,
            resolution: contract.resolution,
            encoder: Encoder::new(standard, parse_setup(setup), precomb.unwrap_or(0) != 0),
        })))
    }
}

// Decode deliberately mirrors the public VapourSynth contract, whose distinct
// named arguments exceed Clippy's generic function-parameter heuristic.
#[allow(clippy::too_many_arguments)]
make_filter_function! {
    DecodeFunction, "Decode"
    fn create_decode<'core>(
        _api: API,
        core: CoreRef<'core>,
        clip: Node<'core>,
        standard: Option<&[u8]>,
        width: Option<i64>,
        threshold: Option<f64>,
        setup: Option<i64>,
        dimensions: Option<i64>,
        eq: Option<i64>,
        thresholds: Option<ValueIter<'_, 'core, f64>>,
        transform: Option<i64>,
        level: Option<i64>,
        lut: Option<ValueIter<'_, 'core, f64>>,
        evidence: Option<f64>,
        cti: Option<i64>,
        mask: Option<&[u8]>,
    ) -> Result<Option<Box<dyn Filter<'core> + 'core>>, Error> {
        let standard = parse_standard(standard)?;
        let contract = validate_gray16_raster(&clip, standard)?;
        let output_geometry = make_output_geometry(
            standard,
            parse_output_width(width, 720)?,
            contract.resolution.height,
        )?;
        let (decoder, mask_kind) = parse_decoder_options(
            standard,
            parse_setup(setup),
            dimensions,
            threshold,
            eq,
            thresholds,
            transform,
            level,
            lut,
            evidence,
            cti,
            mask,
        )?;
        let output = output_info(
            core,
            clip.info(),
            PresetFormat::YUV444P16,
            output_geometry.resolution,
        )?;
        Ok(Some(Box::new(DecodeFilter {
            source: clip,
            output,
            standard,
            decoder,
            picture_resampler: output_geometry.picture_resampler,
            mask_resampler: output_geometry.mask_resampler,
            mask_kind,
            work: Mutex::new(DecodeWork::new(
                contract.resolution.width,
                contract.resolution.height,
            )?),
        })))
    }
}

// Restore deliberately mirrors the public VapourSynth contract, whose distinct
// named arguments exceed Clippy's generic function-parameter heuristic.
#[allow(clippy::too_many_arguments)]
make_filter_function! {
    RestoreFunction, "Restore"
    fn create_restore<'core>(
        api: API,
        core: CoreRef<'core>,
        clip: Node<'core>,
        standard: Option<&[u8]>,
        width: Option<i64>,
        threshold: Option<f64>,
        setup: Option<i64>,
        dimensions: Option<i64>,
        eq: Option<i64>,
        refine: Option<i64>,
        thresholds: Option<ValueIter<'_, 'core, f64>>,
        precomb: Option<i64>,
        transform: Option<i64>,
        level: Option<i64>,
        lut: Option<ValueIter<'_, 'core, f64>>,
        evidence: Option<f64>,
        cti: Option<i64>,
        mask: Option<&[u8]>,
    ) -> Result<Option<Box<dyn Filter<'core> + 'core>>, Error> {
        let standard = parse_standard(standard)?;
        let input = validate_yuv_input(&clip, standard)?;
        let output_width = parse_output_width(width, input.resolution.width)?;
        let output_geometry = make_output_geometry(standard, output_width, input.resolution.height)?;
        let setup = parse_setup(setup);
        let (decoder, mask_kind) = parse_decoder_options(
            standard,
            setup,
            dimensions,
            threshold,
            eq,
            thresholds,
            transform,
            level,
            lut,
            evidence,
            cti,
            mask,
        )?;
        let refine = refine.unwrap_or(1);
        ensure!((0..=16).contains(&refine), "refine must be between 0 and 16");
        let source = resample_to_raster(api, core, &clip, standard, input)?;
        let contract = validate_yuv444p16_raster(&source, standard)?;
        let edge_source = if output_width == 0 {
            None
        } else {
            Some(invoke_spline36(
                api,
                core,
                &clip,
                output_geometry.resolution,
                None,
            )?)
        };
        let crude_decoder = Decoder::with_options(
            standard,
            setup,
            DecoderOptions::new(DecodeMode::Notch)
                .with_equalizer(EqualizerMode::Off)
                .with_builtin_lut(false),
        )?;
        let output = output_info(
            core,
            source.info(),
            PresetFormat::YUV444P16,
            output_geometry.resolution,
        )?;
        Ok(Some(Box::new(RestoreFilter {
            source,
            edge_source,
            output,
            standard,
            encoder: Encoder::new(standard, setup, precomb.unwrap_or(0) != 0),
            refine_encoder: Encoder::new(standard, setup, false),
            decoder,
            crude_decoder,
            refine: refine as usize,
            picture_resampler: output_geometry.picture_resampler,
            mask_resampler: output_geometry.mask_resampler,
            edge_columns: output_geometry.edge_columns,
            mask_kind,
            work: Mutex::new(RestoreWork::new(
                contract.resolution.width,
                contract.resolution.height,
            )?),
        })))
    }
}

export_vapoursynth_plugin! {
    Metadata {
        identifier: PLUGIN_ID,
        namespace: "composite",
        name: "PlaneSight Composite Video",
        read_only: true,
    },
    [EncodeFunction::new(), DecodeFunction::new(), RestoreFunction::new()]
}
