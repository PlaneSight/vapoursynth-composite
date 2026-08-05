//! Exact PAL and NTSC colour-frame phase generation.

use crate::{FrameIndex, Standard};

/// Carrier phase and PAL V-switch for one active raster line.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct LinePhase {
    pub phase: usize,
    pub v_switch: i32,
}

#[must_use]
pub(crate) fn line_phase(standard: Standard, frame: FrameIndex, row: usize) -> LinePhase {
    match standard {
        Standard::Pal => pal_line_phase(frame.0, row),
        Standard::Ntsc => ntsc_line_phase(frame.0, row),
    }
}

fn pal_line_phase(frame: usize, row: usize) -> LinePhase {
    let frame_line = 44 + row;
    let field_id = (frame % 4) * 2 + (frame_line & 1);
    let previous_lines = (field_id / 2) * 625 + (field_id % 2) * 313 + frame_line / 2;
    LinePhase {
        phase: (182 * 625 + (previous_lines % 2_500) * 1_879) % 2_500,
        v_switch: if previous_lines & 1 == 0 { 1 } else { -1 },
    }
}

fn ntsc_line_phase(frame: usize, row: usize) -> LinePhase {
    let frame_line = 39 + row;
    let field_id = (frame % 2) * 2 + (frame_line & 1);
    let previous_lines = (field_id / 2) * 525 + (field_id % 2) * 263 + frame_line / 2;
    LinePhase {
        phase: (130 * 180 + 654 + (previous_lines % 720) * 360) % 720,
        v_switch: 1,
    }
}

pub(crate) fn sine_table(denominator: usize) -> Box<[i16]> {
    (0..denominator)
        .map(|sample| {
            let angle = std::f64::consts::TAU * sample as f64 / denominator as f64;
            (angle.sin() * 32_768.0).round().clamp(-32_768.0, 32_767.0) as i16
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn colour_sequences_repeat_exactly() {
        for row in [0, 1, 287, 575] {
            assert_eq!(
                line_phase(Standard::Pal, FrameIndex(0), row),
                line_phase(Standard::Pal, FrameIndex(4), row)
            );
        }
        for row in [0, 1, 243, 485] {
            assert_eq!(
                line_phase(Standard::Ntsc, FrameIndex(0), row),
                line_phase(Standard::Ntsc, FrameIndex(2), row)
            );
        }
    }

    #[test]
    fn q15_table_has_exact_quadrature_points() {
        let table = sine_table(720);
        assert_eq!(table[0], 0);
        assert_eq!(table[180], 32_767);
        assert_eq!(table[360], 0);
        assert_eq!(table[540], -32_768);
    }
}
