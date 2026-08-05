//! Windowed spectral chroma separation.
//!
//! The separators deliberately keep their FFT plans immutable and all tile
//! buffers in caller-owned scratch storage. A filter can therefore retain one
//! scratch arena per in-flight frame without allocating in its row or tile
//! kernels.

use std::sync::Arc;

use rustfft::num_complex::Complex32;
use rustfft::{Fft, FftPlanner};

use crate::model::GraySource;

pub(crate) const LUT_KNOTS: usize = 16;
pub(crate) const PAL_2D_BINS: usize = 80;
pub(crate) const PAL_3D_BINS: usize = 384;
pub(crate) const NTSC_3D_BINS: usize = 768;

const PAL_2D_WIDTH: usize = 32;
const PAL_2D_HEIGHT: usize = 16;
const THREE_D_WIDTH: usize = 16;
const THREE_D_DEPTH: usize = 8;
const LEVEL_BLACK: f32 = 16_384.0;

/// The per-bin policy used to retain chroma-like spectral energy.
#[derive(Clone, Debug)]
pub(crate) enum SeparationRule {
    /// Keep a pair only when its energy ratio meets the configured threshold.
    Threshold,
    /// Use a different squared pair-symmetry threshold for each bin.
    Thresholds(Arc<[f32]>),
    /// Reduce the stronger half of a pair to the weaker half's magnitude.
    AmplitudeLimit,
    /// Interpolate a trained soft gain for each pair-symmetry ratio.
    TrainedLut(Arc<[f32]>),
}

/// Immutable controls shared by the two-dimensional and temporal separators.
#[derive(Clone, Debug)]
pub(crate) struct SeparationSettings {
    threshold_squared: f32,
    evidence: f32,
    rule: SeparationRule,
}

impl SeparationSettings {
    pub(crate) fn new(threshold: f32, evidence: f32, rule: SeparationRule) -> Self {
        Self {
            threshold_squared: threshold * threshold,
            evidence,
            rule,
        }
    }

    fn gain(&self, bin: usize, low: f32, high: f32) -> f32 {
        match &self.rule {
            SeparationRule::Threshold => {
                if low < high * self.threshold_squared {
                    0.0
                } else {
                    1.0
                }
            }
            SeparationRule::Thresholds(values) => {
                if low < high * values[bin] {
                    0.0
                } else {
                    1.0
                }
            }
            SeparationRule::AmplitudeLimit => {
                if high <= f32::EPSILON {
                    1.0
                } else {
                    (low / high).sqrt()
                }
            }
            SeparationRule::TrainedLut(values) => {
                let ratio = if high <= f32::EPSILON { 1.0 } else { low / high };
                let position = ratio.clamp(0.0, 1.0) * (LUT_KNOTS - 1) as f32;
                let knot = position.floor() as usize;
                let lower = knot.min(LUT_KNOTS - 2);
                let fraction = position - lower as f32;
                let base = bin * LUT_KNOTS;
                values[base + lower] + (values[base + lower + 1] - values[base + lower]) * fraction
            }
        }
    }

    fn evidence_gain(&self, evidence: f32, spectral_energy: f32) -> f32 {
        if self.evidence <= f32::EPSILON {
            return 1.0;
        }
        let denominator = evidence + self.evidence * spectral_energy;
        if denominator <= f32::EPSILON {
            1.0
        } else {
            evidence / denominator
        }
    }
}

/// Reusable storage for one transform invocation.
#[derive(Debug)]
pub(crate) struct TransformScratch {
    spectrum: Vec<Complex32>,
    filtered: Vec<Complex32>,
    line: Vec<Complex32>,
}

impl TransformScratch {
    pub(crate) fn new() -> Self {
        let capacity = THREE_D_WIDTH * THREE_D_DEPTH * 32;
        Self {
            spectrum: vec![Complex32::new(0.0, 0.0); capacity],
            filtered: vec![Complex32::new(0.0, 0.0); capacity],
            line: vec![Complex32::new(0.0, 0.0); 32],
        }
    }
}

/// A PAL field separator using 32 by 16 half-overlapped transform tiles.
pub(crate) struct Transform2d {
    settings: SeparationSettings,
    window: Box<[f32]>,
    forward_x: Arc<dyn Fft<f32>>,
    inverse_x: Arc<dyn Fft<f32>>,
    forward_y: Arc<dyn Fft<f32>>,
    inverse_y: Arc<dyn Fft<f32>>,
}

impl Transform2d {
    pub(crate) fn new(settings: SeparationSettings) -> Self {
        let mut planner = FftPlanner::<f32>::new();
        Self {
            settings,
            window: make_window_2d(PAL_2D_WIDTH, PAL_2D_HEIGHT),
            forward_x: planner.plan_fft_forward(PAL_2D_WIDTH),
            inverse_x: planner.plan_fft_inverse(PAL_2D_WIDTH),
            forward_y: planner.plan_fft_forward(PAL_2D_HEIGHT),
            inverse_y: planner.plan_fft_inverse(PAL_2D_HEIGHT),
        }
    }

    pub(crate) fn separate(
        &self,
        input: &dyn GraySource,
        chroma: &mut [f32],
        mut confidence: Option<&mut [f32]>,
        scratch: &mut TransformScratch,
    ) {
        let (width, height) = input.dimensions();
        debug_assert_eq!(chroma.len(), width * height);
        chroma.fill(0.0);
        let mut confidence = confidence;
        if let Some(values) = confidence.as_deref_mut() {
            values.fill(0.0);
        }

        for tile_y in (-8_isize..height as isize).step_by(PAL_2D_HEIGHT / 2) {
            for tile_x in (-16_isize..width as isize).step_by(PAL_2D_WIDTH / 2) {
                self.load_2d_tile(input, tile_x, tile_y, &mut scratch.spectrum);
                self.fft_2d(&mut scratch.spectrum, &mut scratch.line, false);
                let tile_confidence = self.filter_2d(&scratch.spectrum, &mut scratch.filtered);
                self.fft_2d(&mut scratch.filtered, &mut scratch.line, true);
                self.accumulate_2d_tile(
                    tile_x,
                    tile_y,
                    width,
                    height,
                    &scratch.filtered,
                    chroma,
                    confidence.as_deref_mut(),
                    tile_confidence,
                );
            }
        }
    }

    fn load_2d_tile(
        &self,
        input: &dyn GraySource,
        tile_x: isize,
        tile_y: isize,
        destination: &mut [Complex32],
    ) {
        let (width, height) = input.dimensions();
        for y in 0..PAL_2D_HEIGHT {
            let source_y = tile_y + y as isize;
            for x in 0..PAL_2D_WIDTH {
                let source_x = tile_x + x as isize;
                let sample = if source_x >= 0
                    && source_x < width as isize
                    && source_y >= 0
                    && source_y < height as isize
                {
                    input.row(source_y as usize)[source_x as usize] as f32
                } else {
                    LEVEL_BLACK
                };
                destination[y * PAL_2D_WIDTH + x] = Complex32::new(sample * self.window[y * PAL_2D_WIDTH + x], 0.0);
            }
        }
    }

    fn fft_2d(&self, tile: &mut [Complex32], line: &mut [Complex32], inverse: bool) {
        let x_plan = if inverse { &self.inverse_x } else { &self.forward_x };
        let y_plan = if inverse { &self.inverse_y } else { &self.forward_y };
        for row in tile[..PAL_2D_WIDTH * PAL_2D_HEIGHT].chunks_exact_mut(PAL_2D_WIDTH) {
            x_plan.process(row);
        }
        for x in 0..PAL_2D_WIDTH {
            for y in 0..PAL_2D_HEIGHT {
                line[y] = tile[y * PAL_2D_WIDTH + x];
            }
            y_plan.process(&mut line[..PAL_2D_HEIGHT]);
            for y in 0..PAL_2D_HEIGHT {
                tile[y * PAL_2D_WIDTH + x] = line[y];
            }
        }
    }

    fn filter_2d(&self, input: &[Complex32], output: &mut [Complex32]) -> f32 {
        let length = PAL_2D_WIDTH * PAL_2D_HEIGHT;
        output[..length].fill(Complex32::new(0.0, 0.0));
        let mut confidence_energy = 0.0_f32;
        let mut total_energy = 0.0_f32;
        let mut bin = 0;
        for y in 0..PAL_2D_HEIGHT {
            let y_ref = (PAL_2D_HEIGHT / 2 + PAL_2D_HEIGHT - y) % PAL_2D_HEIGHT;
            for x in PAL_2D_WIDTH / 8..=PAL_2D_WIDTH / 4 {
                let x_ref = PAL_2D_WIDTH / 2 - x;
                let index = y * PAL_2D_WIDTH + x;
                let reflected = y_ref * PAL_2D_WIDTH + x_ref;
                if index == reflected {
                    output[index] = input[index];
                    let energy = magnitude_squared(input[index]);
                    confidence_energy += energy;
                    total_energy += energy;
                    bin += 1;
                    continue;
                }

                let source_energy = magnitude_squared(input[index]);
                let reflected_energy = magnitude_squared(input[reflected]);
                let low = source_energy.min(reflected_energy);
                let high = source_energy.max(reflected_energy);
                let ratio = if high <= f32::EPSILON { 1.0 } else { low / high };
                let low_frequency_x = PAL_2D_WIDTH / 4 - x;
                let first_y = (PAL_2D_HEIGHT / 4 + PAL_2D_HEIGHT - y) % PAL_2D_HEIGHT;
                let second_y = (3 * PAL_2D_HEIGHT / 4 + PAL_2D_HEIGHT - y) % PAL_2D_HEIGHT;
                let evidence = magnitude_squared(input[first_y * PAL_2D_WIDTH + low_frequency_x])
                    .max(magnitude_squared(input[second_y * PAL_2D_WIDTH + low_frequency_x]));
                let evidence_gain = self.settings.evidence_gain(evidence, high);
                let gain = self.settings.gain(bin, low, high) * evidence_gain;
                output[index] = input[index] * gain;
                output[reflected] = input[reflected] * gain;
                let kept = gain * gain * (source_energy + reflected_energy);
                confidence_energy += kept * ratio;
                total_energy += kept;
                bin += 1;
            }
        }
        debug_assert_eq!(bin, PAL_2D_BINS);
        if total_energy <= f32::EPSILON {
            0.0
        } else {
            confidence_energy / total_energy
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn accumulate_2d_tile(
        &self,
        tile_x: isize,
        tile_y: isize,
        width: usize,
        height: usize,
        tile: &[Complex32],
        output: &mut [f32],
        mut confidence: Option<&mut [f32]>,
        tile_confidence: f32,
    ) {
        let normalization = (PAL_2D_WIDTH * PAL_2D_HEIGHT) as f32;
        for y in 0..PAL_2D_HEIGHT {
            let output_y = tile_y + y as isize;
            if output_y < 0 || output_y >= height as isize {
                continue;
            }
            for x in 0..PAL_2D_WIDTH {
                let output_x = tile_x + x as isize;
                if output_x < 0 || output_x >= width as isize {
                    continue;
                }
                let output_index = output_y as usize * width + output_x as usize;
                let tile_index = y * PAL_2D_WIDTH + x;
                output[output_index] += tile[tile_index].re / normalization;
                if let Some(values) = confidence.as_deref_mut() {
                    values[output_index] += tile_confidence * self.window[tile_index];
                }
            }
        }
    }
}

/// A temporal separator using 16-wide, eight-frame spectral tiles.
///
/// Two half-overlapped temporal tiles contribute to the center frame. The
/// nine-frame caller window supplies the four neighbours needed on either
/// side without ever fabricating a temporal sample at the clip boundary.
pub(crate) struct Transform3d {
    settings: SeparationSettings,
    y_size: usize,
    window: Box<[f32]>,
    forward_x: Arc<dyn Fft<f32>>,
    inverse_x: Arc<dyn Fft<f32>>,
    forward_y: Arc<dyn Fft<f32>>,
    inverse_y: Arc<dyn Fft<f32>>,
    forward_z: Arc<dyn Fft<f32>>,
    inverse_z: Arc<dyn Fft<f32>>,
    standard: TemporalStandard,
}

/// The lattice used by a temporal separator.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum TemporalStandard {
    /// PAL's displaced-field lattice, with sixteen vertical samples per tile.
    Pal,
    /// NTSC's frame-line lattice, with thirty-two vertical samples per tile.
    Ntsc,
}

impl Transform3d {
    pub(crate) fn new(settings: SeparationSettings, standard: TemporalStandard) -> Self {
        let y_size = match standard {
            TemporalStandard::Pal => 16,
            TemporalStandard::Ntsc => 32,
        };
        let mut planner = FftPlanner::<f32>::new();
        Self {
            settings,
            y_size,
            window: make_window_3d(THREE_D_WIDTH, y_size, THREE_D_DEPTH),
            forward_x: planner.plan_fft_forward(THREE_D_WIDTH),
            inverse_x: planner.plan_fft_inverse(THREE_D_WIDTH),
            forward_y: planner.plan_fft_forward(y_size),
            inverse_y: planner.plan_fft_inverse(y_size),
            forward_z: planner.plan_fft_forward(THREE_D_DEPTH),
            inverse_z: planner.plan_fft_inverse(THREE_D_DEPTH),
            standard,
        }
    }

    /// Separates the center frame of a nine-frame temporal window.
    ///
    /// The caller provides four clamped neighbors on each side. Keeping the
    /// window explicit makes the scheduler contract visible and prevents an
    /// accidental single-frame fallback for the three-dimensional mode.
    pub(crate) fn separate_center(
        &self,
        frames: &[&dyn GraySource],
        chroma: &mut [f32],
        mut confidence: Option<&mut [f32]>,
        scratch: &mut TransformScratch,
    ) {
        debug_assert_eq!(frames.len(), 9);
        let (width, height) = frames[4].dimensions();
        debug_assert_eq!(chroma.len(), width * height);
        chroma.fill(0.0);
        let mut confidence = confidence;
        if let Some(values) = confidence.as_deref_mut() {
            values.fill(0.0);
        }

        // An eight-sample FFT has its two center positions at z=3 and z=4.
        // Place the output frame at both positions in two adjacent tiles and
        // average their half-overlapped reconstructions. This keeps the
        // temporal hanning window normalized at the output frame.
        for (frame_offset, output_z) in [(0_usize, 4_usize), (1_usize, 3_usize)] {
            let center_window = self.window[self.index(output_z, 0, 0)]
                / horizontal_vertical_window(self.y_size, 0, 0);
            debug_assert!(center_window > 0.0);
            for tile_y in (-(self.y_size as isize / 2)..height as isize).step_by(self.y_size / 2) {
                for tile_x in (-8_isize..width as isize).step_by(THREE_D_WIDTH / 2) {
                    self.load_3d_tile(frames, frame_offset, tile_x, tile_y, &mut scratch.spectrum);
                    self.fft_3d(&mut scratch.spectrum, &mut scratch.line, false);
                    let tile_confidence = self.filter_3d(&scratch.spectrum, &mut scratch.filtered);
                    self.fft_3d(&mut scratch.filtered, &mut scratch.line, true);
                    self.accumulate_3d_plane(
                        tile_x,
                        tile_y,
                        width,
                        height,
                        output_z,
                        &scratch.filtered,
                        chroma,
                        confidence.as_deref_mut(),
                        tile_confidence,
                        center_window,
                    );
                }
            }
        }
    }

    fn index(&self, z: usize, y: usize, x: usize) -> usize {
        (z * self.y_size + y) * THREE_D_WIDTH + x
    }

    fn load_3d_tile(
        &self,
        frames: &[&dyn GraySource],
        frame_offset: usize,
        tile_x: isize,
        tile_y: isize,
        destination: &mut [Complex32],
    ) {
        let (width, height) = frames[4].dimensions();
        for z in 0..THREE_D_DEPTH {
            let source = frames[frame_offset + z];
            for y in 0..self.y_size {
                let source_y = tile_y + y as isize;
                for x in 0..THREE_D_WIDTH {
                    let source_x = tile_x + x as isize;
                    let sample = if source_x >= 0
                        && source_x < width as isize
                        && source_y >= 0
                        && source_y < height as isize
                    {
                        source.row(source_y as usize)[source_x as usize] as f32
                    } else {
                        LEVEL_BLACK
                    };
                    let index = self.index(z, y, x);
                    destination[index] = Complex32::new(sample * self.window[index], 0.0);
                }
            }
        }
    }

    fn fft_3d(&self, tile: &mut [Complex32], line: &mut [Complex32], inverse: bool) {
        let x_plan = if inverse { &self.inverse_x } else { &self.forward_x };
        let y_plan = if inverse { &self.inverse_y } else { &self.forward_y };
        let z_plan = if inverse { &self.inverse_z } else { &self.forward_z };
        let length = THREE_D_DEPTH * self.y_size * THREE_D_WIDTH;
        let tile = &mut tile[..length];
        for slab in tile.chunks_exact_mut(self.y_size * THREE_D_WIDTH) {
            for row in slab.chunks_exact_mut(THREE_D_WIDTH) {
                x_plan.process(row);
            }
        }
        for z in 0..THREE_D_DEPTH {
            for x in 0..THREE_D_WIDTH {
                for y in 0..self.y_size {
                    line[y] = tile[self.index(z, y, x)];
                }
                y_plan.process(&mut line[..self.y_size]);
                for y in 0..self.y_size {
                    tile[self.index(z, y, x)] = line[y];
                }
            }
        }
        for y in 0..self.y_size {
            for x in 0..THREE_D_WIDTH {
                for z in 0..THREE_D_DEPTH {
                    line[z] = tile[self.index(z, y, x)];
                }
                z_plan.process(&mut line[..THREE_D_DEPTH]);
                for z in 0..THREE_D_DEPTH {
                    tile[self.index(z, y, x)] = line[z];
                }
            }
        }
    }

    fn filter_3d(&self, input: &[Complex32], output: &mut [Complex32]) -> f32 {
        let length = THREE_D_DEPTH * self.y_size * THREE_D_WIDTH;
        output[..length].fill(Complex32::new(0.0, 0.0));
        let mut confidence_energy = 0.0_f32;
        let mut total_energy = 0.0_f32;
        let mut bin = 0;
        for z in 0..THREE_D_DEPTH {
            let z_ref = match self.standard {
                TemporalStandard::Pal => (THREE_D_DEPTH - z) % THREE_D_DEPTH,
                TemporalStandard::Ntsc => (THREE_D_DEPTH / 2 + THREE_D_DEPTH - z) % THREE_D_DEPTH,
            };
            for y in 0..self.y_size {
                let y_ref = (self.y_size / 2 + self.y_size - y) % self.y_size;
                for x in THREE_D_WIDTH / 8..=THREE_D_WIDTH / 4 {
                    let x_ref = THREE_D_WIDTH / 2 - x;
                    let index = self.index(z, y, x);
                    let reflected = self.index(z_ref, y_ref, x_ref);
                    if index == reflected {
                        if self.is_carrier(z, y, x) {
                            output[index] = input[index];
                            let energy = magnitude_squared(input[index]);
                            confidence_energy += energy;
                            total_energy += energy;
                        }
                        bin += 1;
                        continue;
                    }

                    let source_energy = magnitude_squared(input[index]);
                    let reflected_energy = magnitude_squared(input[reflected]);
                    let low = source_energy.min(reflected_energy);
                    let high = source_energy.max(reflected_energy);
                    let ratio = if high <= f32::EPSILON { 1.0 } else { low / high };
                    let evidence = self.low_frequency_evidence(input, z, y, x);
                    let evidence_gain = self.settings.evidence_gain(evidence, high);
                    let gain = self.settings.gain(bin, low, high) * evidence_gain;
                    output[index] = input[index] * gain;
                    output[reflected] = input[reflected] * gain;
                    let kept = gain * gain * (source_energy + reflected_energy);
                    confidence_energy += kept * ratio;
                    total_energy += kept;
                    bin += 1;
                }
            }
        }
        debug_assert_eq!(
            bin,
            match self.standard {
                TemporalStandard::Pal => PAL_3D_BINS,
                TemporalStandard::Ntsc => NTSC_3D_BINS,
            }
        );
        if total_energy <= f32::EPSILON {
            0.0
        } else {
            confidence_energy / total_energy
        }
    }

    fn is_carrier(&self, z: usize, y: usize, x: usize) -> bool {
        if x != THREE_D_WIDTH / 4 {
            return false;
        }
        match self.standard {
            TemporalStandard::Pal => z == THREE_D_DEPTH / 2 && (y == self.y_size / 4 || y == 3 * self.y_size / 4),
            TemporalStandard::Ntsc => {
                (z == THREE_D_DEPTH / 4 && y == self.y_size / 4)
                    || (z == 3 * THREE_D_DEPTH / 4 && y == 3 * self.y_size / 4)
            }
        }
    }

    fn low_frequency_evidence(&self, input: &[Complex32], z: usize, y: usize, x: usize) -> f32 {
        if self.standard != TemporalStandard::Pal {
            return 0.0;
        }
        let x_low = THREE_D_WIDTH / 4 - x;
        let z_low = (THREE_D_DEPTH / 2 + THREE_D_DEPTH - z) % THREE_D_DEPTH;
        let first_y = (self.y_size / 4 + self.y_size - y) % self.y_size;
        let second_y = (3 * self.y_size / 4 + self.y_size - y) % self.y_size;
        magnitude_squared(input[self.index(z_low, first_y, x_low)])
            .max(magnitude_squared(input[self.index(z_low, second_y, x_low)]))
    }

    #[allow(clippy::too_many_arguments)]
    fn accumulate_3d_plane(
        &self,
        tile_x: isize,
        tile_y: isize,
        width: usize,
        height: usize,
        output_z: usize,
        tile: &[Complex32],
        output: &mut [f32],
        mut confidence: Option<&mut [f32]>,
        tile_confidence: f32,
        center_window: f32,
    ) {
        let normalization = (THREE_D_WIDTH * self.y_size * THREE_D_DEPTH) as f32 * center_window;
        for y in 0..self.y_size {
            let output_y = tile_y + y as isize;
            if output_y < 0 || output_y >= height as isize {
                continue;
            }
            for x in 0..THREE_D_WIDTH {
                let output_x = tile_x + x as isize;
                if output_x < 0 || output_x >= width as isize {
                    continue;
                }
                let output_index = output_y as usize * width + output_x as usize;
                let tile_index = self.index(output_z, y, x);
                output[output_index] += tile[tile_index].re / (2.0 * normalization);
                if let Some(values) = confidence.as_deref_mut() {
                    values[output_index] +=
                        0.5 * tile_confidence * horizontal_vertical_window(self.y_size, y, x);
                }
            }
        }
    }
}

fn magnitude_squared(value: Complex32) -> f32 {
    value.re * value.re + value.im * value.im
}

fn make_window_2d(width: usize, height: usize) -> Box<[f32]> {
    let mut values = vec![0.0; width * height];
    for y in 0..height {
        for x in 0..width {
            values[y * width + x] = hann(y, height) * hann(x, width);
        }
    }
    values.into_boxed_slice()
}

fn make_window_3d(width: usize, height: usize, depth: usize) -> Box<[f32]> {
    let mut values = vec![0.0; width * height * depth];
    for z in 0..depth {
        for y in 0..height {
            for x in 0..width {
                values[(z * height + y) * width + x] = hann(z, depth) * hann(y, height) * hann(x, width);
            }
        }
    }
    values.into_boxed_slice()
}

fn horizontal_vertical_window(height: usize, y: usize, x: usize) -> f32 {
    hann(y, height) * hann(x, THREE_D_WIDTH)
}

fn hann(index: usize, length: usize) -> f32 {
    let angle = std::f32::consts::TAU * (index as f32 + 0.5) / length as f32;
    0.5 - 0.5 * angle.cos()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lut_interpolation_preserves_endpoint_gain() {
        let lut = Arc::<[f32]>::from(vec![0.25; PAL_2D_BINS * LUT_KNOTS]);
        let settings = SeparationSettings::new(0.4, 0.0, SeparationRule::TrainedLut(lut));
        assert!((settings.gain(0, 0.0, 1.0) - 0.25).abs() < f32::EPSILON);
        assert!((settings.gain(0, 1.0, 1.0) - 0.25).abs() < f32::EPSILON);
    }

    #[test]
    fn temporal_window_has_nonzero_center() {
        let window = make_window_3d(THREE_D_WIDTH, 16, THREE_D_DEPTH);
        assert!(window[(4 * 16) * THREE_D_WIDTH] > 0.0);
    }
}
