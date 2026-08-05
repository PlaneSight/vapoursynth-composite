//! VapourSynth adapter.

#![allow(unsafe_code)]

use anyhow::{Error, anyhow, bail, ensure};
use vapoursynth::core::CoreRef;
use vapoursynth::format::{ColorFamily, PresetFormat, SampleType};
use vapoursynth::plugins::{Filter, FilterArgument, FrameContext, Metadata};
use vapoursynth::prelude::*;
use vapoursynth::video_info::{Property, Resolution, VideoInfo};
use vapoursynth::{export_vapoursynth_plugin, make_filter_function};

use crate::{
    DecodeMode, Decoder, Encoder, FrameIndex, GraySink, GraySource, Setup, Standard, YuvSink,
    YuvSource,
};

const PLUGIN_ID: &str = "com.planesight.composite";

#[derive(Clone, Copy, Debug)]
struct InputContract {
    resolution: Resolution,
}

fn validate_yuv444p16(node: &Node<'_>, standard: Standard) -> anyhow::Result<InputContract> {
    let info = node.info();
    let resolution = match info.resolution {
        Property::Constant(value) => value,
        Property::Variable => bail!("clip must have constant dimensions"),
    };
    ensure!(
        info.format.color_family() == ColorFamily::YUV
            && info.format.sample_type() == SampleType::Integer
            && info.format.bits_per_sample() == 16
            && info.format.sub_sampling_w() == 0
            && info.format.sub_sampling_h() == 0,
        "clip must be YUV444P16"
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

fn validate_gray16(node: &Node<'_>, standard: Standard) -> anyhow::Result<InputContract> {
    let info = node.info();
    let resolution = match info.resolution {
        Property::Constant(value) => value,
        Property::Variable => bail!("clip must have constant dimensions"),
    };
    ensure!(
        info.format.color_family() == ColorFamily::Gray
            && info.format.sample_type() == SampleType::Integer
            && info.format.bits_per_sample() == 16,
        "clip must be GRAY16"
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

fn row_offset(standard: Standard, frame: &FrameRef<'_>) -> anyhow::Result<usize> {
    let top_field_first = frame
        .props()
        .get_int("_FieldBased")
        .is_ok_and(|value| value == 2);
    Ok(standard
        .raster()
        .row_offset(standard, frame.height(0), top_field_first)?)
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

struct FrameYuvSink<'frame, 'core>(&'frame mut FrameRefMut<'core>);

impl YuvSink for FrameYuvSink<'_, '_> {
    fn dimensions(&self) -> (usize, usize) {
        (self.0.width(0), self.0.height(0))
    }

    fn write_row(&mut self, row: usize, y: &[u16], u: &[u16], v: &[u16]) {
        self.0.plane_row_mut::<u16>(0, row).copy_from_slice(y);
        self.0.plane_row_mut::<u16>(1, row).copy_from_slice(u);
        self.0.plane_row_mut::<u16>(2, row).copy_from_slice(v);
    }
}

struct PackedGray {
    width: usize,
    height: usize,
    samples: Vec<u16>,
}

impl PackedGray {
    fn new(width: usize, height: usize) -> anyhow::Result<Self> {
        let len = width
            .checked_mul(height)
            .ok_or_else(|| anyhow!("frame dimensions overflow"))?;
        Ok(Self {
            width,
            height,
            samples: vec![0; len],
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

fn new_frame<'core>(
    core: CoreRef<'core>,
    source: &FrameRef<'core>,
    preset: PresetFormat,
    resolution: Resolution,
) -> anyhow::Result<FrameRefMut<'core>> {
    let format = core
        .get_format(preset.into())
        .ok_or_else(|| anyhow!("VapourSynth format {preset:?} is unavailable"))?;
    // SAFETY: every active sample in every output plane is initialized by
    // Encoder::encode_source or Decoder::decode_source before the frame is
    // converted to FrameRef and returned across the host ABI.
    Ok(unsafe { FrameRefMut::new_uninitialized(core, Some(source), format, resolution) })
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
    resolution: Resolution,
    standard: Standard,
    decoder: Decoder,
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
        let offset = row_offset(self.standard, &source)?;
        let mut output = new_frame(core, &source, PresetFormat::YUV444P16, self.resolution)?;
        self.decoder.decode_source(
            FrameIndex(n),
            offset,
            &FrameGraySource(&source),
            &mut FrameYuvSink(&mut output),
        )?;
        Ok(output.into())
    }
}

struct RestoreFilter<'core> {
    source: Node<'core>,
    output: VideoInfo<'core>,
    resolution: Resolution,
    standard: Standard,
    encoder: Encoder,
    decoder: Decoder,
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
        let offset = row_offset(self.standard, &source)?;
        let mut composite = PackedGray::new(self.resolution.width, self.resolution.height)?;
        self.encoder.encode_source(
            FrameIndex(n),
            offset,
            &FrameYuvSource(&source),
            &mut composite,
        )?;
        let mut output = new_frame(core, &source, PresetFormat::YUV444P16, self.resolution)?;
        self.decoder.decode_source(
            FrameIndex(n),
            offset,
            &composite,
            &mut FrameYuvSink(&mut output),
        )?;
        Ok(output.into())
    }
}

fn output_info<'core>(
    core: CoreRef<'core>,
    input: VideoInfo<'core>,
    preset: PresetFormat,
) -> anyhow::Result<VideoInfo<'core>> {
    let format = core
        .get_format(preset.into())
        .ok_or_else(|| anyhow!("VapourSynth format {preset:?} is unavailable"))?;
    Ok(VideoInfo { format, ..input })
}

make_filter_function! {
    EncodeFunction, "Encode"
    fn create_encode<'core>(
        _api: API,
        core: CoreRef<'core>,
        clip: Node<'core>,
        standard: Option<&[u8]>,
        setup: Option<i64>,
        precomb: Option<i64>,
    ) -> Result<Option<Box<dyn Filter<'core> + 'core>>, Error> {
        let standard = parse_standard(standard)?;
        let contract = validate_yuv444p16(&clip, standard)?;
        let input = clip.info();
        let output = output_info(core, input, PresetFormat::Gray16)?;
        Ok(Some(Box::new(EncodeFilter {
            source: clip,
            output,
            resolution: contract.resolution,
            encoder: Encoder::new(standard, parse_setup(setup), precomb.unwrap_or(0) != 0),
        })))
    }
}

make_filter_function! {
    DecodeFunction, "Decode"
    fn create_decode<'core>(
        _api: API,
        core: CoreRef<'core>,
        clip: Node<'core>,
        standard: Option<&[u8]>,
        setup: Option<i64>,
        dimensions: Option<i64>,
    ) -> Result<Option<Box<dyn Filter<'core> + 'core>>, Error> {
        let standard = parse_standard(standard)?;
        let contract = validate_gray16(&clip, standard)?;
        let input = clip.info();
        let output = output_info(core, input, PresetFormat::YUV444P16)?;
        Ok(Some(Box::new(DecodeFilter {
            source: clip,
            output,
            resolution: contract.resolution,
            standard,
            decoder: Decoder::new(standard, parse_setup(setup), DecodeMode::from_dimensions(dimensions.unwrap_or(2))?),
        })))
    }
}

make_filter_function! {
    RestoreFunction, "Restore"
    fn create_restore<'core>(
        _api: API,
        core: CoreRef<'core>,
        clip: Node<'core>,
        standard: Option<&[u8]>,
        setup: Option<i64>,
        precomb: Option<i64>,
        dimensions: Option<i64>,
    ) -> Result<Option<Box<dyn Filter<'core> + 'core>>, Error> {
        let standard = parse_standard(standard)?;
        let contract = validate_yuv444p16(&clip, standard)?;
        let input = clip.info();
        let output = output_info(core, input, PresetFormat::YUV444P16)?;
        let setup = parse_setup(setup);
        Ok(Some(Box::new(RestoreFilter {
            source: clip,
            output,
            resolution: contract.resolution,
            standard,
            encoder: Encoder::new(standard, setup, precomb.unwrap_or(0) != 0),
            decoder: Decoder::new(standard, setup, DecodeMode::from_dimensions(dimensions.unwrap_or(2))?),
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
