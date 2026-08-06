//! Fixed-point PAL and NTSC composite encoder.

use crate::model::{FrameIndex, GraySink, Plane, PlaneMut, Setup, Standard, YuvSource};
use crate::subcarrier::{line_phase, sine_table};

const MAX_WIDTH: usize = 928;
const MAX_TAPS: usize = 13;
const LEVEL_MIN: i32 = 0x0100;
const LEVEL_MAX: i32 = 0xfeff;

const PAL_UV_TAPS: [i32; 13] = [
    4, 38, 270, 1_226, 3_619, 6_927, 8_600, 6_927, 3_619, 1_226, 270, 38, 4,
];
const NTSC_UV_TAPS: [i32; 9] = [69, 626, 2_959, 7_563, 10_334, 7_563, 2_959, 626, 69];

/// A reusable, immutable composite encoder.
///
/// The encoder allocates only its phase table during construction. Frame and
/// row processing use fixed-size stack scratch whose upper bound is the PAL
/// active raster width.
#[derive(Debug)]
pub struct Encoder {
    standard: Standard,
    setup: Setup,
    precomb: bool,
    sine: Box<[i16]>,
    levels: LevelMap,
}

#[derive(Clone, Copy, Debug)]
struct LevelMap {
    black: i32,
    luma_num: i32,
    luma_den: i32,
    ku: i32,
    kv: i32,
}

impl Encoder {
    /// Constructs an encoder with a fixed standard and setup policy.
    #[must_use]
    pub fn new(standard: Standard, setup: Setup, precomb: bool) -> Self {
        let levels = LevelMap::new(standard, setup);
        let denominator = standard.raster().phase_denominator as usize;
        Self {
            standard,
            setup,
            precomb,
            sine: sine_table(denominator),
            levels,
        }
    }

    /// Returns the configured standard.
    #[must_use]
    pub const fn standard(&self) -> Standard {
        self.standard
    }

    /// Returns whether NTSC setup is enabled.
    #[must_use]
    pub const fn setup(&self) -> Setup {
        self.setup
    }

    /// Encodes a YUV444P16 active picture into a GRAY16 composite raster.
    ///
    /// All planes must have the standard's 4fsc width and the same height.
    /// NTSC permits either 480 active rows placed within the 486-line raster
    /// or the full 486 rows; `row_offset` carries that placement.
    pub fn encode(
        &self,
        frame: FrameIndex,
        row_offset: usize,
        y: Plane<'_>,
        u: Plane<'_>,
        v: Plane<'_>,
        mut output: PlaneMut<'_>,
    ) -> Result<(), EncodeError> {
        let dimensions = y.dimensions();
        if u.dimensions() != dimensions
            || v.dimensions() != dimensions
            || output.dimensions() != dimensions
        {
            return Err(EncodeError::PlaneGeometry {
                expected_width: self.standard.raster().width,
                y: dimensions,
                u: u.dimensions(),
                v: v.dimensions(),
                output: output.dimensions(),
            });
        }
        struct Source<'a> {
            y: Plane<'a>,
            u: Plane<'a>,
            v: Plane<'a>,
        }
        impl YuvSource for Source<'_> {
            fn dimensions(&self) -> (usize, usize) {
                self.y.dimensions()
            }
            fn row(&self, plane: usize, row: usize) -> &[u16] {
                match plane {
                    0 => self.y.row(row),
                    1 => self.u.row(row),
                    2 => self.v.row(row),
                    _ => panic!("invalid YUV plane"),
                }
            }
        }
        self.encode_source(frame, row_offset, &Source { y, u, v }, &mut output)
    }

    /// Encodes any row-addressable YUV444P16 source.
    pub fn encode_source<S: YuvSource, D: GraySink>(
        &self,
        frame: FrameIndex,
        row_offset: usize,
        source: &S,
        output: &mut D,
    ) -> Result<(), EncodeError> {
        let expected_width = self.standard.raster().width;
        let dimensions = source.dimensions();
        if dimensions.0 != expected_width || output.dimensions() != dimensions {
            return Err(EncodeError::PlaneGeometry {
                expected_width,
                y: dimensions,
                u: dimensions,
                v: dimensions,
                output: output.dimensions(),
            });
        }
        if row_offset + dimensions.1 > self.standard.raster().full_height {
            return Err(EncodeError::RasterPlacement {
                row_offset,
                rows: dimensions.1,
                raster_height: self.standard.raster().full_height,
            });
        }

        let mut filtered_u = [0_u16; MAX_WIDTH];
        let mut filtered_v = [0_u16; MAX_WIDTH];

        for row in 0..dimensions.1 {
            let (source_u, source_v) = if self.precomb {
                let above = row.checked_sub(2).unwrap_or(row);
                let below = (row + 2).min(dimensions.1 - 1);
                precomb_row(
                    source.row(1, above),
                    source.row(1, row),
                    source.row(1, below),
                    &mut filtered_u[..dimensions.0],
                );
                precomb_row(
                    source.row(2, above),
                    source.row(2, row),
                    source.row(2, below),
                    &mut filtered_v[..dimensions.0],
                );
                (&filtered_u[..dimensions.0], &filtered_v[..dimensions.0])
            } else {
                (source.row(1, row), source.row(2, row))
            };

            let mut encoded = [0_u16; MAX_WIDTH];
            self.encode_row(
                frame,
                row + row_offset,
                source.row(0, row),
                source_u,
                source_v,
                &mut encoded[..dimensions.0],
            );
            output.write_row(row, &encoded[..dimensions.0]);
        }
        Ok(())
    }

    fn encode_row(
        &self,
        frame: FrameIndex,
        raster_row: usize,
        y: &[u16],
        u: &[u16],
        v: &[u16],
        output: &mut [u16],
    ) {
        let width = output.len();
        let taps = match self.standard {
            Standard::Pal => &PAL_UV_TAPS[..],
            Standard::Ntsc => &NTSC_UV_TAPS[..],
        };
        let mut filtered_u = [0_i32; MAX_WIDTH];
        let mut filtered_v = [0_i32; MAX_WIDTH];
        fir_chroma_row(u, taps, &mut filtered_u[..width]);
        fir_chroma_row(v, taps, &mut filtered_v[..width]);

        let carrier = line_phase(self.standard, frame, raster_row);
        let den = self.standard.raster().phase_denominator as usize;
        let sin = i32::from(self.sine[carrier.phase]);
        let cos = i32::from(self.sine[(carrier.phase + den / 4) % den]);
        let sine4 = [sin, cos, -sin, -cos];
        let cosine4 = [
            carrier.v_switch * cos,
            -carrier.v_switch * sin,
            -carrier.v_switch * cos,
            carrier.v_switch * sin,
        ];

        for (x, destination) in output.iter_mut().enumerate() {
            let luma = self.levels.black
                + round_div(
                    i64::from(i32::from(y[x]) - 4_096) * i64::from(self.levels.luma_num),
                    self.levels.luma_den,
                );
            let chroma_u = (filtered_u[x] * self.levels.ku + 16_384) >> 15;
            let chroma_v = (filtered_v[x] * self.levels.kv + 16_384) >> 15;
            let chroma = (chroma_u * sine4[x & 3] + chroma_v * cosine4[x & 3] + 16_384) >> 15;
            *destination = (luma + chroma).clamp(LEVEL_MIN, LEVEL_MAX) as u16;
        }
    }
}

impl LevelMap {
    fn new(standard: Standard, setup: Setup) -> Self {
        let (black, white) = match standard {
            Standard::Pal => (0x4000, 0xd300),
            Standard::Ntsc => (
                if setup == Setup::Ire7_5 {
                    0x4680
                } else {
                    0x3c00
                },
                0xc800,
            ),
        };
        let mut luma_num = (white - black) / 256;
        let mut luma_den = 219;
        if luma_num * 256 != white - black {
            luma_num = (white - black) / 128;
            luma_den = 438;
        }
        let span = f64::from(white - black);
        let ku = (((1.0 - 0.114) * 0.492_111_041_122_483_56 / (112.0 * 256.0)) * span * 32_768.0)
            .round() as i32;
        let kv = (((1.0 - 0.299) * 0.877_283_219_938_178_7 / (112.0 * 256.0)) * span * 32_768.0)
            .round() as i32;
        Self {
            black,
            luma_num,
            luma_den,
            ku,
            kv,
        }
    }
}

fn precomb_row(above: &[u16], current: &[u16], below: &[u16], output: &mut [u16]) {
    for (((destination, &a), &c), &b) in output.iter_mut().zip(above).zip(current).zip(below) {
        *destination = ((u32::from(a) + 2 * u32::from(c) + u32::from(b) + 2) >> 2) as u16;
    }
}

fn fir_chroma_row(input: &[u16], taps: &[i32], output: &mut [i32]) {
    debug_assert_eq!(input.len(), output.len());
    debug_assert!(taps.len() <= MAX_TAPS);
    let radius = taps.len() / 2;
    let length = input.len();
    let interior_start = radius.min(length);
    let interior_end = length.saturating_sub(radius);

    if interior_start >= interior_end {
        for x in 0..length {
            output[x] = fir_chroma_edge(input, taps, radius, x);
        }
        return;
    }

    for x in 0..interior_start {
        output[x] = fir_chroma_edge(input, taps, radius, x);
    }
    for x in interior_start..interior_end {
        output[x] = fir_chroma_interior(input, taps, radius, x);
    }
    for x in interior_end..length {
        output[x] = fir_chroma_edge(input, taps, radius, x);
    }
}

fn fir_chroma_edge(input: &[u16], taps: &[i32], radius: usize, x: usize) -> i32 {
    let first_tap = radius.saturating_sub(x);
    let last_tap = (input.len() + radius - x).min(taps.len());
    let mut accumulator = 0_i64;
    for (tap_index, &tap) in taps[first_tap..last_tap].iter().enumerate() {
        let sample = input[x + first_tap + tap_index - radius];
        accumulator += i64::from(tap) * i64::from(i32::from(sample) - 32_768);
    }
    ((accumulator + 16_384) >> 15) as i32
}

fn fir_chroma_interior(input: &[u16], taps: &[i32], radius: usize, x: usize) -> i32 {
    let samples = &input[x - radius..x + radius + 1];
    let (tap_chunks, tap_tail) = taps.as_chunks::<4>();
    let (sample_chunks, sample_tail) = samples.as_chunks::<4>();
    let mut accumulator = 0_i64;
    for (tap_chunk, sample_chunk) in tap_chunks.iter().zip(sample_chunks) {
        accumulator += i64::from(tap_chunk[0])
            * i64::from(i32::from(sample_chunk[0]) - 32_768);
        accumulator += i64::from(tap_chunk[1])
            * i64::from(i32::from(sample_chunk[1]) - 32_768);
        accumulator += i64::from(tap_chunk[2])
            * i64::from(i32::from(sample_chunk[2]) - 32_768);
        accumulator += i64::from(tap_chunk[3])
            * i64::from(i32::from(sample_chunk[3]) - 32_768);
    }
    for (&tap, &sample) in tap_tail.iter().zip(sample_tail) {
        accumulator += i64::from(tap) * i64::from(i32::from(sample) - 32_768);
    }
    ((accumulator + 16_384) >> 15) as i32
}

pub(crate) fn round_div(numerator: i64, denominator: i32) -> i32 {
    let half = i64::from(denominator / 2);
    ((if numerator >= 0 {
        numerator + half
    } else {
        numerator - half
    }) / i64::from(denominator)) as i32
}

pub(crate) fn level_parameters(standard: Standard, setup: Setup) -> (i32, i32, i32, i32, i32) {
    let encode = LevelMap::new(standard, setup);
    let decode_luma_num = encode.luma_den;
    let decode_luma_den = encode.luma_num;
    (
        encode.black,
        decode_luma_num,
        decode_luma_den,
        encode.ku,
        encode.kv,
    )
}

/// Invalid encoder plane geometry or raster placement.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EncodeError {
    /// The planes disagree or do not use the standard's active width.
    PlaneGeometry {
        /// Required composite raster width for the selected standard.
        expected_width: usize,
        /// Actual luma-plane \`(width, height)\`.
        y: (usize, usize),
        /// Actual U-plane \`(width, height)\`.
        u: (usize, usize),
        /// Actual V-plane \`(width, height)\`.
        v: (usize, usize),
        /// Actual output-plane \`(width, height)\`.
        output: (usize, usize),
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

impl std::fmt::Display for EncodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::PlaneGeometry {
                expected_width,
                y,
                u,
                v,
                output,
            } => write!(
                f,
                "encoder needs matching {expected_width}-sample planes; Y={y:?}, U={u:?}, V={v:?}, output={output:?}"
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

impl std::error::Error for EncodeError {}

#[cfg(test)]
mod tests {
    use super::*;

    fn fir_chroma_row_reference(input: &[u16], taps: &[i32], output: &mut [i32]) {
        for (x, destination) in output.iter_mut().enumerate() {
            let mut accumulator = 0_i64;
            for (tap_index, &tap) in taps.iter().enumerate() {
                let source = x + tap_index;
                if source < taps.len() / 2 || source - taps.len() / 2 >= input.len() {
                    continue;
                }
                accumulator += i64::from(tap)
                    * i64::from(i32::from(input[source - taps.len() / 2]) - 32_768);
            }
            *destination = ((accumulator + 16_384) >> 15) as i32;
        }
    }

    #[test]
    fn chroma_filters_preserve_a_flat_signal() {
        for taps in [&PAL_UV_TAPS[..], &NTSC_UV_TAPS[..]] {
            let input = [40_000_u16; 64];
            let mut output = [0_i32; 64];
            fir_chroma_row(&input, taps, &mut output);
            for value in &output[taps.len()..64 - taps.len()] {
                assert_eq!(*value, 7_232);
            }
        }
    }

    #[test]
    fn split_chroma_filter_matches_reference_at_all_boundaries() {
        for taps in [&PAL_UV_TAPS[..], &NTSC_UV_TAPS[..]] {
            for length in 0..=40 {
                let input: Vec<_> = (0..length)
                    .map(|index| (index * 4_123 + 7_000) as u16)
                    .collect();
                let mut expected = vec![0_i32; length];
                let mut actual = vec![0_i32; length];
                fir_chroma_row_reference(&input, taps, &mut expected);
                fir_chroma_row(&input, taps, &mut actual);
                assert_eq!(actual, expected, "length={length}, taps={}", taps.len());
            }
        }
    }

    #[test]
    fn round_division_is_symmetric() {
        for value in 0..1000 {
            assert_eq!(round_div(value, 219), -round_div(-value, 219));
        }
    }
}
