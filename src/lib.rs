//! PAL and NTSC composite-video encoding and restoration.
//!
//! The signal core is independent of VapourSynth. The plugin adapter is kept
//! in [`plugin`] so frame ownership and the host ABI cannot leak into the
//! numerical code.

#![cfg_attr(not(test), deny(clippy::unwrap_used, clippy::expect_used))]

mod decode;
mod encode;
mod model;
mod plugin;
mod subcarrier;

pub use decode::{
    DecodeConfigError, DecodeError, DecodeMode, DecodeReport, DecodeScratch, Decoder,
    DecoderOptions, EqualizerMode, MaskKind, NtscTemporalMode,
};
pub use encode::{EncodeError, Encoder};
pub use model::{
    FrameIndex, GeometryError, GraySink, GraySource, ParseStandardError, Plane, PlaneError,
    PlaneMut, Raster, Setup, Standard, YuvSink, YuvSource,
};
