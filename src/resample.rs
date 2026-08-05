//! Shared BT.601 to composite-raster geometry and horizontal Spline36 plans.

use std::fmt;

use crate::model::Standard;

const SPLINE36_SUPPORT: f64 = 3.0;

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct ResampleGeometry {
    pub(crate) output_width: usize,
    pub(crate) src_left: f64,
    pub(crate) src_width: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum ResampleError {
    InvalidDimensions {
        input_width: usize,
        output_width: usize,
    },
    InvalidGeometry {
        src_left: f64,
        src_width: f64,
    },
}

impl fmt::Display for ResampleError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidDimensions {
                input_width,
                output_width,
            } => write!(
                formatter,
                "resample widths must be nonzero, got {input_width} to {output_width}"
            ),
            Self::InvalidGeometry {
                src_left,
                src_width,
            } => write!(
                formatter,
                "resample geometry must be finite with positive width, got left={src_left}, width={src_width}"
            ),
        }
    }
}

impl std::error::Error for ResampleError {}

#[derive(Clone, Copy, Debug)]
struct Tap {
    source: usize,
    weight: f32,
}

/// An immutable, allocation-free-per-row horizontal Spline36 resampler.
#[derive(Debug)]
pub(crate) struct HorizontalResampler {
    input_width: usize,
    output_width: usize,
    offsets: Box<[usize]>,
    taps: Box<[Tap]>,
}

/// An immutable, allocation-free-per-row horizontal bilinear resampler.
///
/// Diagnostic masks use this plan so their hard motion decisions remain a
/// clean coverage ramp instead of acquiring Spline36 ringing lobes.
#[derive(Debug)]
pub(crate) struct HorizontalBilinearResampler {
    input_width: usize,
    output_width: usize,
    samples: Box<[(usize, usize, f32)]>,
}

impl HorizontalBilinearResampler {
    pub(crate) fn new(
        input_width: usize,
        output_width: usize,
        src_left: f64,
        src_width: f64,
    ) -> Result<Self, ResampleError> {
        if input_width == 0 || output_width == 0 {
            return Err(ResampleError::InvalidDimensions {
                input_width,
                output_width,
            });
        }
        if !src_left.is_finite() || !src_width.is_finite() || src_width <= 0.0 {
            return Err(ResampleError::InvalidGeometry {
                src_left,
                src_width,
            });
        }
        let scale = src_width / output_width as f64;
        if !scale.is_finite() || scale <= 0.0 {
            return Err(ResampleError::InvalidGeometry {
                src_left,
                src_width,
            });
        }
        let mut samples = Vec::with_capacity(output_width);
        for output in 0..output_width {
            let center = src_left + (output as f64 + 0.5) * scale - 0.5;
            let left_unclamped = center.floor() as isize;
            let fraction = (center - left_unclamped as f64) as f32;
            let left = left_unclamped.clamp(0, input_width as isize - 1) as usize;
            let right = (left_unclamped + 1).clamp(0, input_width as isize - 1) as usize;
            samples.push((left, right, fraction));
        }
        Ok(Self {
            input_width,
            output_width,
            samples: samples.into_boxed_slice(),
        })
    }

    pub(crate) fn resample_row(&self, input: &[u16], output: &mut [u16]) {
        assert_eq!(input.len(), self.input_width);
        assert_eq!(output.len(), self.output_width);
        for (destination, &(left, right, fraction)) in output.iter_mut().zip(&*self.samples) {
            let value =
                f32::from(input[left]).mul_add(1.0 - fraction, f32::from(input[right]) * fraction);
            *destination = value.round().clamp(0.0, f32::from(u16::MAX)) as u16;
        }
    }
}

impl HorizontalResampler {
    pub(crate) fn new(
        input_width: usize,
        output_width: usize,
        src_left: f64,
        src_width: f64,
    ) -> Result<Self, ResampleError> {
        if input_width == 0 || output_width == 0 {
            return Err(ResampleError::InvalidDimensions {
                input_width,
                output_width,
            });
        }
        if !src_left.is_finite() || !src_width.is_finite() || src_width <= 0.0 {
            return Err(ResampleError::InvalidGeometry {
                src_left,
                src_width,
            });
        }

        let scale = src_width / output_width as f64;
        if !scale.is_finite() || scale <= 0.0 {
            return Err(ResampleError::InvalidGeometry {
                src_left,
                src_width,
            });
        }
        let filter_scale = scale.max(1.0);
        let support = SPLINE36_SUPPORT * filter_scale;
        let mut offsets = Vec::with_capacity(output_width + 1);
        let mut taps = Vec::new();

        for output in 0..output_width {
            offsets.push(taps.len());
            let center = src_left + (output as f64 + 0.5) * scale - 0.5;
            let first = (center - support).ceil() as isize;
            let last = (center + support).floor() as isize;
            let start = taps.len();
            let mut weight_sum = 0.0_f64;

            for source in first..=last {
                let weight = spline36((source as f64 - center) / filter_scale) / filter_scale;
                if weight.abs() <= f64::EPSILON {
                    continue;
                }
                let source = source.clamp(0, input_width as isize - 1) as usize;
                taps.push(Tap {
                    source,
                    weight: weight as f32,
                });
                weight_sum += weight;
            }

            if weight_sum.abs() <= f64::EPSILON {
                let source = center.round().clamp(0.0, input_width as f64 - 1.0) as usize;
                taps.push(Tap {
                    source,
                    weight: 1.0,
                });
            } else {
                let normalization = (1.0 / weight_sum) as f32;
                for tap in &mut taps[start..] {
                    tap.weight *= normalization;
                }
            }
        }
        offsets.push(taps.len());

        Ok(Self {
            input_width,
            output_width,
            offsets: offsets.into_boxed_slice(),
            taps: taps.into_boxed_slice(),
        })
    }

    pub(crate) fn resample_row(&self, input: &[u16], output: &mut [u16]) {
        assert_eq!(input.len(), self.input_width);
        assert_eq!(output.len(), self.output_width);
        for (index, destination) in output.iter_mut().enumerate() {
            let mut value = 0.0_f64;
            for tap in &self.taps[self.offsets[index]..self.offsets[index + 1]] {
                value += f64::from(input[tap.source]) * f64::from(tap.weight);
            }
            *destination = value.round().clamp(0.0, f64::from(u16::MAX)) as u16;
        }
    }
}

pub(crate) fn encode_geometry(standard: Standard, input_width: usize) -> ResampleGeometry {
    let (rho, active_start, anchor_601) = anchors(standard);
    let scale = input_width as f64 / 720.0;
    ResampleGeometry {
        output_width: standard.raster().width,
        src_left: scale * ((active_start - 0.5) * rho - anchor_601) + 0.5,
        src_width: scale * (standard.raster().width as f64 * rho),
    }
}

pub(crate) fn decode_geometry(
    standard: Standard,
    output_width: usize,
) -> Result<ResampleGeometry, ResampleError> {
    if output_width == 0 {
        return Err(ResampleError::InvalidDimensions {
            input_width: standard.raster().width,
            output_width,
        });
    }
    let (rho, active_start, anchor_601) = anchors(standard);
    Ok(ResampleGeometry {
        output_width,
        src_left: anchor_601 / rho
            - (active_start - 0.5)
            - 0.5 * (720.0 / output_width as f64) / rho,
        src_width: 720.0 / rho,
    })
}

pub(crate) fn decode_edge_columns(
    standard: Standard,
    output_width: usize,
) -> Result<(usize, usize), ResampleError> {
    let geometry = decode_geometry(standard, output_width)?;
    let step = geometry.src_width / output_width as f64;
    let raster_width = standard.raster().width as f64;
    let mut left = 0;
    let mut right = 0;
    while left < output_width && geometry.src_left + (left as f64 + 0.5) * step < -0.5 {
        left += 1;
    }
    while right < output_width
        && geometry.src_left + (output_width as f64 - right as f64 - 0.5) * step
            > raster_width - 0.5
    {
        right += 1;
    }
    Ok((left, right))
}

fn anchors(standard: Standard) -> (f64, f64, f64) {
    match standard {
        Standard::Pal => (540_000.0 / 709_379.0, 182.0, 132.0),
        Standard::Ntsc => (33.0 / 35.0, 130.0 + 57.0 / 90.0, 122.0),
    }
}

fn spline36(value: f64) -> f64 {
    let value = value.abs();
    if value < 1.0 {
        polynomial(value, 1.0, -3.0 / 209.0, -453.0 / 209.0, 13.0 / 11.0)
    } else if value < 2.0 {
        polynomial(value - 1.0, 0.0, -156.0 / 209.0, 270.0 / 209.0, -6.0 / 11.0)
    } else if value < 3.0 {
        polynomial(value - 2.0, 0.0, 26.0 / 209.0, -45.0 / 209.0, 1.0 / 11.0)
    } else {
        0.0
    }
}

fn polynomial(value: f64, c0: f64, c1: f64, c2: f64, c3: f64) -> f64 {
    ((c3 * value + c2) * value + c1) * value + c0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spline36_is_interpolating() {
        assert!((spline36(0.0) - 1.0).abs() < f64::EPSILON);
        assert!(spline36(1.0).abs() < f64::EPSILON);
        assert!(spline36(2.0).abs() < f64::EPSILON);
        assert!(spline36(3.0).abs() < f64::EPSILON);
    }

    #[test]
    fn identity_plan_preserves_every_sample() {
        let plan = match HorizontalResampler::new(7, 7, 0.0, 7.0) {
            Ok(plan) => plan,
            Err(error) => panic!("{error}"),
        };
        let input = [0_u16, 1, 10, 257, 32_768, 65_534, 65_535];
        let mut output = [0_u16; 7];
        plan.resample_row(&input, &mut output);
        assert_eq!(input, output);
    }

    #[test]
    fn edge_clamping_preserves_a_flat_row() {
        let plan = match HorizontalResampler::new(5, 11, -1.5, 8.0) {
            Ok(plan) => plan,
            Err(error) => panic!("{error}"),
        };
        let input = [42_000_u16; 5];
        let mut output = [0_u16; 11];
        plan.resample_row(&input, &mut output);
        assert_eq!(output, [42_000_u16; 11]);
    }

    #[test]
    fn geometry_matches_the_broadcast_rasters() {
        assert_eq!(encode_geometry(Standard::Pal, 720).output_width, 928);
        assert_eq!(encode_geometry(Standard::Ntsc, 720).output_width, 758);
        assert!(decode_geometry(Standard::Pal, 720).is_ok());
        assert!(decode_geometry(Standard::Ntsc, 720).is_ok());
    }
}
