//! Generated trained gain tables retained as Rust data.
//!
//! The values are generated at build time from the preserved calibration
//! source. They are never linked into the Rust plugin as C code.

use std::sync::Arc;

include!(concat!(env!("OUT_DIR"), "/builtin_lut.rs"));

pub(crate) fn pal_2d() -> Arc<[f32]> {
    Arc::from(&PAL_2D[..])
}

pub(crate) fn pal_3d() -> Arc<[f32]> {
    Arc::from(&PAL_3D[..])
}

pub(crate) fn ntsc_3d() -> Arc<[f32]> {
    Arc::from(&NTSC_3D[..])
}
