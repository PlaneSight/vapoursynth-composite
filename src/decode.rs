//! Allocation-free spatial composite decoder.

use crate::encode::{level_parameters, round_div};
use crate::model::{FrameIndex, GraySource, Plane, PlaneMut, Setup, Standard, YuvSink};
use crate::subcarrier::{line_phase, sine_table};

const MAX_WIDTH: usize = 928;
const COLOR_TAPS: [i32; 17] = [
    73, 317, 200, -682, -1_597, -503, 3_726, 9_094, 11_579, 9_094, 3_726, -503, -1_597, -682, 200,
    317, 73,
];

/// Chroma-separation strategy.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum DecodeMode {
    /// A one-line notch decoder. Useful as a degradation reference.
    Notch,
    /// Adaptive same-field line comb. This is the production spatial path.
    #[default]
    SpatialComb,
}

impl DecodeMode {
    /// Converts the public `dimensions` argument into a supported mode.
    pub fn from_dimensions(value: i64) -> Result<Self, DecodeConfigError> {
        match value {
            1 => Ok(Self::Notch),
            2 => Ok(Self::SpatialComb),
            _ => Err(DecodeConfigError::UnsupportedDimensions(value)),
        }
    }
}

/// Immutable spatial decoder.
#[derive(Debug)]
pub struct Decoder {
    standard: Standard,
    mode: DecodeMode,
    sine: Box<[i16]>,
    black: i32,
    luma_num: i32,
    luma_den: i32,
    ku: i32,
    kv: i32,
    comb_range: f32,
}

struct DemodulatedRow<'a> {
    y: &'a mut [u16],
    u: &'a mut [u16],
    v: &'a mut [u16],
}

impl Decoder {
    /// Constructs a decoder.
    #[must_use]
    pub fn new(standard: Standard, setup: Setup, mode: DecodeMode) -> Self {
        let (black, luma_num, luma_den, ku, kv) = level_parameters(standard, setup);
        let comb_range = 45.0 * (f32::from(0xc800_u16) - black as f32) / 100.0;
        Self {
            standard,
            mode,
            sine: sine_table(standard.raster().phase_denominator as usize),
            black,
            luma_num,
            luma_den,
            ku,
            kv,
            comb_range,
        }
    }

    /// Decodes one composite frame into YUV444P16.
    pub fn decode(
        &self,
        frame: FrameIndex,
        row_offset: usize,
        composite: Plane<'_>,
        y: PlaneMut<'_>,
        u: PlaneMut<'_>,
        v: PlaneMut<'_>,
    ) -> Result<(), DecodeError> {
        struct Sink<'a> {
            y: PlaneMut<'a>,
            u: PlaneMut<'a>,
            v: PlaneMut<'a>,
        }
        impl YuvSink for Sink<'_> {
            fn dimensions(&self) -> (usize, usize) {
                self.y.dimensions()
            }
            fn write_row(&mut self, row: usize, y: &[u16], u: &[u16], v: &[u16]) {
                self.y.row_mut(row).copy_from_slice(y);
                self.u.row_mut(row).copy_from_slice(u);
                self.v.row_mut(row).copy_from_slice(v);
            }
        }
        self.decode_source(frame, row_offset, &composite, &mut Sink { y, u, v })
    }

    /// Decodes any row-addressable GRAY16 source.
    pub fn decode_source<S: GraySource, D: YuvSink>(
        &self,
        frame: FrameIndex,
        row_offset: usize,
        composite: &S,
        output: &mut D,
    ) -> Result<(), DecodeError> {
        let dimensions = composite.dimensions();
        let expected_width = self.standard.raster().width;
        if dimensions.0 != expected_width || output.dimensions() != dimensions {
            return Err(DecodeError::PlaneGeometry {
                expected_width,
                composite: dimensions,
                y: output.dimensions(),
                u: output.dimensions(),
                v: output.dimensions(),
            });
        }
        if row_offset + dimensions.1 > self.standard.raster().full_height {
            return Err(DecodeError::RasterPlacement {
                row_offset,
                rows: dimensions.1,
                raster_height: self.standard.raster().full_height,
            });
        }

        let mut current = [0_f32; MAX_WIDTH];
        let mut previous = [0_f32; MAX_WIDTH];
        let mut next = [0_f32; MAX_WIDTH];
        let mut separated = [0_i32; MAX_WIDTH];
        for row in 0..dimensions.1 {
            bandpass(composite.row(row), &mut current[..dimensions.0]);
            match self.mode {
                DecodeMode::Notch => {
                    for (destination, value) in
                        separated.iter_mut().zip(&current).take(dimensions.0)
                    {
                        *destination = value
                            .round()
                            .clamp(f32::from(i16::MIN), f32::from(i16::MAX))
                            as i32;
                    }
                }
                DecodeMode::SpatialComb => {
                    if let Some(above) = row.checked_sub(2) {
                        bandpass(composite.row(above), &mut previous[..dimensions.0]);
                    } else {
                        previous[..dimensions.0].fill(0.0);
                    }
                    if row + 2 < dimensions.1 {
                        bandpass(composite.row(row + 2), &mut next[..dimensions.0]);
                    } else {
                        next[..dimensions.0].fill(0.0);
                    }
                    spatial_comb(
                        &current[..dimensions.0],
                        &previous[..dimensions.0],
                        &next[..dimensions.0],
                        self.comb_range,
                        &mut separated[..dimensions.0],
                    );
                }
            }

            let mut y = [0_u16; MAX_WIDTH];
            let mut u = [0_u16; MAX_WIDTH];
            let mut v = [0_u16; MAX_WIDTH];
            self.demodulate_row(
                frame,
                row + row_offset,
                composite.row(row),
                &separated[..dimensions.0],
                DemodulatedRow {
                    y: &mut y[..dimensions.0],
                    u: &mut u[..dimensions.0],
                    v: &mut v[..dimensions.0],
                },
            );
            output.write_row(
                row,
                &y[..dimensions.0],
                &u[..dimensions.0],
                &v[..dimensions.0],
            );
        }
        Ok(())
    }

    fn demodulate_row(
        &self,
        frame: FrameIndex,
        raster_row: usize,
        composite: &[u16],
        chroma: &[i32],
        output: DemodulatedRow<'_>,
    ) {
        let width = composite.len();
        let mut product_sine = [0_i32; MAX_WIDTH];
        let mut product_cosine = [0_i32; MAX_WIDTH];
        for x in 0..width {
            product_sine[x] = match x & 3 {
                1 => chroma[x],
                3 => -chroma[x],
                _ => 0,
            };
            product_cosine[x] = match x & 3 {
                0 => chroma[x],
                2 => -chroma[x],
                _ => 0,
            };
        }

        let mut p = [0_i32; MAX_WIDTH];
        let mut q = [0_i32; MAX_WIDTH];
        fir_row(&product_sine[..width], &COLOR_TAPS, &mut p[..width]);
        fir_row(&product_cosine[..width], &COLOR_TAPS, &mut q[..width]);

        let carrier = line_phase(self.standard, frame, raster_row);
        let denominator = self.standard.raster().phase_denominator as usize;
        let sin = i32::from(self.sine[carrier.phase]);
        let cos = i32::from(self.sine[(carrier.phase + denominator / 4) % denominator]);
        let sine4 = [sin, cos, -sin, -cos];
        let cosine4 = [
            carrier.v_switch * cos,
            -carrier.v_switch * sin,
            -carrier.v_switch * cos,
            carrier.v_switch * sin,
        ];

        for x in 0..width {
            let demod_u =
                -((i64::from(p[x]) * i64::from(-cos) + i64::from(q[x]) * i64::from(-sin) + 8_192)
                    >> 14) as i32;
            let demod_v = carrier.v_switch
                * (-((i64::from(q[x]) * i64::from(-cos) - i64::from(p[x]) * i64::from(-sin)
                    + 8_192)
                    >> 14) as i32);

            let resynthesized = (demod_u * sine4[x & 3] + demod_v * cosine4[x & 3] + 16_384) >> 15;
            let luma_level = i32::from(composite[x])
                - if self.mode == DecodeMode::Notch {
                    chroma[x]
                } else {
                    resynthesized
                };
            output.y[x] = clamp_u16(
                4_096
                    + round_div(
                        i64::from(luma_level - self.black) * i64::from(self.luma_num),
                        self.luma_den,
                    ),
            );
            output.u[x] = clamp_u16(32_768 + round_div(i64::from(demod_u) * 32_768, self.ku));
            output.v[x] = clamp_u16(32_768 + round_div(i64::from(demod_v) * 32_768, self.kv));
        }
    }
}

fn bandpass(input: &[u16], output: &mut [f32]) {
    debug_assert_eq!(input.len(), output.len());
    output.fill(0.0);
    if input.len() < 5 {
        return;
    }
    for x in 2..input.len() - 2 {
        output[x] =
            (2.0 * f32::from(input[x]) - f32::from(input[x - 2]) - f32::from(input[x + 2])) * 0.25;
    }
}

fn spatial_comb(current: &[f32], previous: &[f32], next: &[f32], range: f32, output: &mut [i32]) {
    debug_assert_eq!(current.len(), previous.len());
    debug_assert_eq!(current.len(), next.len());
    debug_assert_eq!(current.len(), output.len());
    output[0] = 0;
    for x in 1..current.len() {
        let mut weight_previous = (current[x].abs() - previous[x].abs()).abs()
            + (current[x - 1].abs() - previous[x - 1].abs()).abs()
            - (current[x].abs() + previous[x - 1].abs()) * 0.10;
        let mut weight_next = (current[x].abs() - next[x].abs()).abs()
            + (current[x - 1].abs() - next[x - 1].abs()).abs()
            - (current[x].abs() + next[x - 1].abs()) * 0.10;

        weight_previous = (1.0 - weight_previous / range).clamp(0.0, 1.0);
        weight_next = (1.0 - weight_next / range).clamp(0.0, 1.0);
        let scale = if weight_previous > 0.0 || weight_next > 0.0 {
            if weight_next > 3.0 * weight_previous {
                weight_previous = 0.0;
            } else if weight_previous > 3.0 * weight_next {
                weight_next = 0.0;
            }
            (2.0 / (weight_next + weight_previous)).max(1.0)
        } else if (previous[x].abs() - next[x].abs()).abs() <= ((next[x] + previous[x]) * 0.2).abs()
        {
            weight_previous = 1.0;
            weight_next = 1.0;
            1.0
        } else {
            1.0
        };
        let value = ((current[x] - previous[x]) * weight_previous * scale
            + (current[x] - next[x]) * weight_next * scale)
            * 0.25;
        output[x] = value
            .round()
            .clamp(f32::from(i16::MIN), f32::from(i16::MAX)) as i32;
    }
}

fn fir_row(input: &[i32], taps: &[i32], output: &mut [i32]) {
    debug_assert_eq!(input.len(), output.len());
    let radius = taps.len() / 2;
    for (x, destination) in output.iter_mut().enumerate() {
        let mut accumulator = 0_i64;
        for (tap_index, &tap) in taps.iter().enumerate() {
            let source = x + tap_index;
            if source < radius || source - radius >= input.len() {
                continue;
            }
            accumulator += i64::from(tap) * i64::from(input[source - radius]);
        }
        *destination = ((accumulator + 16_384) >> 15) as i32;
    }
}

fn clamp_u16(value: i32) -> u16 {
    value.clamp(0, i32::from(u16::MAX)) as u16
}

/// Invalid decoder configuration.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DecodeConfigError {
    /// Only spatial dimensions are present in this first Rust milestone.
    UnsupportedDimensions(i64),
}

impl std::fmt::Display for DecodeConfigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::UnsupportedDimensions(value) => {
                write!(f, "dimensions must be 1 or 2, got {value}")
            }
        }
    }
}

impl std::error::Error for DecodeConfigError {}

/// Invalid decoder plane geometry or raster placement.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DecodeError {
    /// The planes disagree or do not use the standard's active width.
    PlaneGeometry {
        /// Required composite raster width for the selected standard.
        expected_width: usize,
        /// Actual composite-plane `(width, height)`.
        composite: (usize, usize),
        /// Actual luma-plane `(width, height)`.
        y: (usize, usize),
        /// Actual U-plane `(width, height)`.
        u: (usize, usize),
        /// Actual V-plane `(width, height)`.
        v: (usize, usize),
    },
    /// Active rows extend beyond the broadcast raster.
    RasterPlacement {
        /// First active row within the complete raster.
        row_offset: usize,
        /// Number of active rows in the input frame.
        rows: usize,
        /// Complete active-picture height for the selected standard.
        raster_height: usize,
    },
}

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::PlaneGeometry {
                expected_width,
                composite,
                y,
                u,
                v,
            } => write!(
                f,
                "decoder needs matching {expected_width}-sample planes; composite={composite:?}, Y={y:?}, U={u:?}, V={v:?}"
            ),
            Self::RasterPlacement {
                row_offset,
                rows,
                raster_height,
            } => write!(
                f,
                "rows {row_offset}..{} exceed the {raster_height}-line raster",
                row_offset + rows
            ),
        }
    }
}

impl std::error::Error for DecodeError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bandpass_rejects_flat_luma() {
        let input = [20_000_u16; 32];
        let mut output = [1.0_f32; 32];
        bandpass(&input, &mut output);
        assert!(output.iter().all(|&value| value == 0.0));
    }

    #[test]
    fn spatial_comb_is_bounded_for_empty_neighbors() {
        let current = [1_000.0_f32; 32];
        let empty = [0.0_f32; 32];
        let mut output = [0_i32; 32];
        spatial_comb(&current, &empty, &empty, 10_000.0, &mut output);
        assert!(output.iter().all(|&value| value.abs() <= 1_000));
    }
}
