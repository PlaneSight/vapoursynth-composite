//! Domain types and validated planar views.

use std::fmt;

/// PAL or NTSC composite encoding.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Standard {
    /// 625-line PAL at 4× the colour-subcarrier frequency.
    Pal,
    /// 525-line NTSC at 4× the colour-subcarrier frequency.
    Ntsc,
}

impl Standard {
    /// Parses the VapourSynth argument spelling.
    pub fn parse(value: &[u8]) -> Result<Self, ParseStandardError> {
        match value {
            b"pal" => Ok(Self::Pal),
            b"ntsc" => Ok(Self::Ntsc),
            _ => Err(ParseStandardError),
        }
    }

    /// Returns the active 4fsc raster used by this standard.
    #[must_use]
    pub const fn raster(self) -> Raster {
        match self {
            Self::Pal => Raster {
                width: 928,
                full_height: 576,
                phase_denominator: 2_500,
            },
            Self::Ntsc => Raster {
                width: 758,
                full_height: 486,
                phase_denominator: 720,
            },
        }
    }
}

/// Error returned when a standard name is not `pal` or `ntsc`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ParseStandardError;

impl fmt::Display for ParseStandardError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("standard must be \"pal\" or \"ntsc\"")
    }
}

impl std::error::Error for ParseStandardError {}

/// The active picture geometry at 4fsc.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Raster {
    /// Samples per active line.
    pub width: usize,
    /// Full active-picture line count.
    pub full_height: usize,
    /// Units per complete subcarrier cycle.
    pub phase_denominator: i32,
}

impl Raster {
    /// Validates an input height and returns its raster row offset.
    pub fn row_offset(
        self,
        standard: Standard,
        height: usize,
        top_field_first: bool,
    ) -> Result<usize, GeometryError> {
        match (standard, height) {
            (Standard::Pal, 576) | (Standard::Ntsc, 486) => Ok(0),
            (Standard::Ntsc, 480) => Ok(if top_field_first { 5 } else { 4 }),
            (Standard::Pal, actual) => Err(GeometryError {
                standard,
                expected: "576",
                actual,
            }),
            (Standard::Ntsc, actual) => Err(GeometryError {
                standard,
                expected: "480 or 486",
                actual,
            }),
        }
    }
}

/// Invalid vertical active-picture geometry.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct GeometryError {
    standard: Standard,
    expected: &'static str,
    actual: usize,
}

impl fmt::Display for GeometryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{} input must have {} lines, got {}",
            match self.standard {
                Standard::Pal => "PAL",
                Standard::Ntsc => "NTSC",
            },
            self.expected,
            self.actual
        )
    }
}

impl std::error::Error for GeometryError {}

/// NTSC pedestal policy.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Setup {
    /// No 7.5 IRE pedestal.
    #[default]
    None,
    /// Apply the 7.5 IRE NTSC pedestal.
    Ire7_5,
}

/// A frame number in the colour-frame sequence.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FrameIndex(pub usize);

/// Read access to one YUV444P16 frame.
pub trait YuvSource {
    /// Active width and height shared by all three planes.
    fn dimensions(&self) -> (usize, usize);

    /// Returns one active row from plane 0 (Y), 1 (U), or 2 (V).
    fn row(&self, plane: usize, row: usize) -> &[u16];
}

/// Write access to a GRAY16 frame.
pub trait GraySink {
    /// Active width and height.
    fn dimensions(&self) -> (usize, usize);

    /// Copies a complete active row into the destination frame.
    fn write_row(&mut self, row: usize, values: &[u16]);
}

/// Read access to a GRAY16 frame.
pub trait GraySource {
    /// Active width and height.
    fn dimensions(&self) -> (usize, usize);

    /// Returns one active row.
    fn row(&self, row: usize) -> &[u16];
}

/// Write access to one YUV444P16 frame.
pub trait YuvSink {
    /// Active width and height shared by all three planes.
    fn dimensions(&self) -> (usize, usize);

    /// Copies one complete Y, U, and V row into the destination frame.
    fn write_row(&mut self, row: usize, y: &[u16], u: &[u16], v: &[u16]);
}

/// Read-only packed plane with an explicit row stride in samples.
#[derive(Clone, Copy, Debug)]
pub struct Plane<'a> {
    data: &'a [u16],
    width: usize,
    height: usize,
    stride: usize,
}

impl<'a> Plane<'a> {
    /// Constructs a checked planar view.
    pub fn new(
        data: &'a [u16],
        width: usize,
        height: usize,
        stride: usize,
    ) -> Result<Self, PlaneError> {
        validate_plane(data.len(), width, height, stride)?;
        Ok(Self {
            data,
            width,
            height,
            stride,
        })
    }

    /// Returns one active row without its padding.
    #[must_use]
    pub fn row(self, row: usize) -> &'a [u16] {
        assert!(row < self.height, "row was validated by the caller");
        let start = row * self.stride;
        &self.data[start..start + self.width]
    }

    /// Returns the active dimensions.
    #[must_use]
    pub const fn dimensions(self) -> (usize, usize) {
        (self.width, self.height)
    }
}

impl GraySource for Plane<'_> {
    fn dimensions(&self) -> (usize, usize) {
        (*self).dimensions()
    }

    fn row(&self, row: usize) -> &[u16] {
        (*self).row(row)
    }
}

/// Mutable packed plane with an explicit row stride in samples.
#[derive(Debug)]
pub struct PlaneMut<'a> {
    data: &'a mut [u16],
    width: usize,
    height: usize,
    stride: usize,
}

impl<'a> PlaneMut<'a> {
    /// Constructs a checked mutable planar view.
    pub fn new(
        data: &'a mut [u16],
        width: usize,
        height: usize,
        stride: usize,
    ) -> Result<Self, PlaneError> {
        validate_plane(data.len(), width, height, stride)?;
        Ok(Self {
            data,
            width,
            height,
            stride,
        })
    }

    /// Returns one active row without its padding.
    #[must_use]
    pub fn row_mut(&mut self, row: usize) -> &mut [u16] {
        assert!(row < self.height, "row was validated by the caller");
        let start = row * self.stride;
        &mut self.data[start..start + self.width]
    }

    /// Returns the active dimensions.
    #[must_use]
    pub const fn dimensions(&self) -> (usize, usize) {
        (self.width, self.height)
    }
}

impl GraySink for PlaneMut<'_> {
    fn dimensions(&self) -> (usize, usize) {
        self.dimensions()
    }

    fn write_row(&mut self, row: usize, values: &[u16]) {
        self.row_mut(row).copy_from_slice(values);
    }
}

fn validate_plane(
    len: usize,
    width: usize,
    height: usize,
    stride: usize,
) -> Result<(), PlaneError> {
    if width == 0 || height == 0 || stride < width {
        return Err(PlaneError::InvalidLayout {
            width,
            height,
            stride,
        });
    }
    let required = stride
        .checked_mul(height.saturating_sub(1))
        .and_then(|prefix| prefix.checked_add(width))
        .ok_or(PlaneError::SizeOverflow)?;
    if len < required {
        return Err(PlaneError::TooShort {
            required,
            actual: len,
        });
    }
    Ok(())
}

/// Invalid plane storage or layout.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlaneError {
    /// Width, height, or stride does not describe a plane.
    InvalidLayout {
        /// Active samples in each row.
        width: usize,
        /// Active row count.
        height: usize,
        /// Distance between adjacent row starts, in samples.
        stride: usize,
    },
    /// The layout calculation overflowed `usize`.
    SizeOverflow,
    /// The provided storage cannot hold the declared plane.
    TooShort {
        /// Minimum sample count required by the declared layout.
        required: usize,
        /// Available sample count.
        actual: usize,
    },
}

impl fmt::Display for PlaneError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match *self {
            Self::InvalidLayout {
                width,
                height,
                stride,
            } => {
                write!(f, "invalid plane layout: {width}x{height}, stride {stride}")
            }
            Self::SizeOverflow => f.write_str("plane layout overflows addressable memory"),
            Self::TooShort { required, actual } => {
                write!(f, "plane storage needs {required} samples, got {actual}")
            }
        }
    }
}

impl std::error::Error for PlaneError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_strided_plane_extent() {
        let data = [0_u16; 11];
        assert!(Plane::new(&data, 3, 3, 4).is_ok());
        assert_eq!(
            Plane::new(&data[..10], 3, 3, 4).err(),
            Some(PlaneError::TooShort {
                required: 11,
                actual: 10
            })
        );
    }

    #[test]
    fn models_ntsc_480_field_alignment() {
        let raster = Standard::Ntsc.raster();
        assert_eq!(raster.row_offset(Standard::Ntsc, 480, false), Ok(4));
        assert_eq!(raster.row_offset(Standard::Ntsc, 480, true), Ok(5));
    }
}
