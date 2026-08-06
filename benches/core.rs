use std::hint::black_box;
use std::thread;
use std::time::Instant;

use vapoursynth_composite::{
    DecodeMode, DecodeScratch, Decoder, Encoder, FrameIndex, Plane, PlaneMut, Setup, Standard,
};

type BenchResult<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;

const ITERATIONS: usize = 3;
const WARMUP: usize = 1;

fn main() -> BenchResult<()> {
    println!("vapoursynth-composite whole-frame benchmarks");
    benchmark_encode(Standard::Ntsc)?;
    benchmark_decode(Standard::Ntsc, DecodeMode::SpatialComb)?;
    benchmark_decode(Standard::Pal, DecodeMode::SpatialComb)?;
    benchmark_temporal_decode()?;
    benchmark_parallel_decode()?;
    Ok(())
}

fn benchmark_encode(standard: Standard) -> BenchResult<()> {
    let (width, height) = dimensions(standard);
    let (y, u, v) = test_yuv(width, height);
    let encoder = Encoder::new(standard, Setup::None, false);
    let mut composite = vec![0_u16; width * height];

    measure(
        &format!("encode {:?}", standard),
        || {
            encoder.encode(
                FrameIndex(0),
                0,
                Plane::new(&y, width, height, width)?,
                Plane::new(&u, width, height, width)?,
                Plane::new(&v, width, height, width)?,
                PlaneMut::new(&mut composite, width, height, width)?,
            )?;
            Ok(checksum(&composite))
        },
    )
}

fn benchmark_decode(standard: Standard, mode: DecodeMode) -> BenchResult<()> {
    let (width, height) = dimensions(standard);
    let composite = encoded_frame(standard)?;
    let decoder = Decoder::new(standard, Setup::None, mode);
    let mut scratch = DecodeScratch::new(width, height)?;
    let mut y = vec![0_u16; width * height];
    let mut u = vec![0_u16; width * height];
    let mut v = vec![0_u16; width * height];

    measure(
        &format!("decode {:?} {:?}", standard, mode),
        || {
            let composite_plane = Plane::new(&composite, width, height, width)?;
            decoder.decode_source_with_scratch(
                FrameIndex(0),
                0,
                &composite_plane,
                &mut PackedYuv {
                    y: &mut y,
                    u: &mut u,
                    v: &mut v,
                    width,
                    height,
                },
                &mut scratch,
            )?;
            Ok(checksum(&y) ^ checksum(&u).rotate_left(11) ^ checksum(&v).rotate_left(23))
        },
    )
}

fn benchmark_temporal_decode() -> BenchResult<()> {
    let standard = Standard::Ntsc;
    let (width, height) = dimensions(standard);
    let composite = encoded_frame(standard)?;
    let frames: Vec<_> = (0..9).map(|_| composite.clone()).collect();
    let decoder = Decoder::new(standard, Setup::None, DecodeMode::TemporalTransform);
    let sources: Vec<_> = frames
        .iter()
        .map(|frame| Plane::new(frame, width, height, width))
        .collect::<Result<_, _>>()?;
    let source_refs: Vec<&dyn vapoursynth_composite::GraySource> = sources
        .iter()
        .map(|source| source as &dyn vapoursynth_composite::GraySource)
        .collect();
    let indices: Vec<_> = (0..9).map(FrameIndex).collect();
    let mut scratch = DecodeScratch::new(width, height)?;
    let mut y = vec![0_u16; width * height];
    let mut u = vec![0_u16; width * height];
    let mut v = vec![0_u16; width * height];

    measure("decode ntsc temporal", || {
        decoder.decode_window_with_scratch(
            0,
            &source_refs,
            &indices,
            &mut PackedYuv {
                y: &mut y,
                u: &mut u,
                v: &mut v,
                width,
                height,
            },
            &mut scratch,
        )?;
        Ok(checksum(&y) ^ checksum(&u).rotate_left(11) ^ checksum(&v).rotate_left(23))
    })
}

fn benchmark_parallel_decode() -> BenchResult<()> {
    let standard = Standard::Ntsc;
    let (width, height) = dimensions(standard);
    let composite = encoded_frame(standard)?;
    let decoder = Decoder::new(standard, Setup::None, DecodeMode::SpatialComb);

    measure("decode ntsc spatial x4", || {
        let results = thread::scope(|scope| {
            let mut handles = Vec::with_capacity(4);
            for _ in 0..4 {
                handles.push(scope.spawn(|| -> BenchResult<u64> {
                    let mut scratch = DecodeScratch::new(width, height)?;
                    let mut y = vec![0_u16; width * height];
                    let mut u = vec![0_u16; width * height];
                    let mut v = vec![0_u16; width * height];
                    let composite_plane = Plane::new(&composite, width, height, width)?;
                    decoder.decode_source_with_scratch(
                        FrameIndex(0),
                        0,
                        &composite_plane,
                        &mut PackedYuv {
                            y: &mut y,
                            u: &mut u,
                            v: &mut v,
                            width,
                            height,
                        },
                        &mut scratch,
                    )?;
                    Ok(checksum(&y) ^ checksum(&u).rotate_left(11) ^ checksum(&v).rotate_left(23))
                }));
            }
            handles
                .into_iter()
                .map(|handle| match handle.join() {
                    Ok(result) => result,
                    Err(_) => Err(std::io::Error::other("benchmark worker panicked").into()),
                })
                .collect::<BenchResult<Vec<_>>>()
        })?;
        Ok(results
            .into_iter()
            .fold(0_u64, |value, checksum| value ^ checksum.rotate_left(7)))
    })
}

fn encoded_frame(standard: Standard) -> BenchResult<Vec<u16>> {
    let (width, height) = dimensions(standard);
    let (y, u, v) = test_yuv(width, height);
    let encoder = Encoder::new(standard, Setup::None, false);
    let mut composite = vec![0_u16; width * height];
    encoder.encode(
        FrameIndex(0),
        0,
        Plane::new(&y, width, height, width)?,
        Plane::new(&u, width, height, width)?,
        Plane::new(&v, width, height, width)?,
        PlaneMut::new(&mut composite, width, height, width)?,
    )?;
    Ok(composite)
}

fn dimensions(standard: Standard) -> (usize, usize) {
    (standard.raster().width, standard.raster().full_height)
}

fn test_yuv(width: usize, height: usize) -> (Vec<u16>, Vec<u16>, Vec<u16>) {
    let length = width * height;
    let y = (0..length)
        .map(|index| 24_000_u16 + (index % 5_000) as u16)
        .collect();
    let u = (0..length)
        .map(|index| 28_000_u16 + (index % 3_000) as u16)
        .collect();
    let v = (0..length)
        .map(|index| 36_000_u16 - (index % 3_000) as u16)
        .collect();
    (y, u, v)
}

fn checksum(values: &[u16]) -> u64 {
    values
        .iter()
        .step_by(97)
        .fold(0_u64, |sum, &value| sum.wrapping_add(u64::from(value)))
}

fn measure<F>(label: &str, mut operation: F) -> BenchResult<()>
where
    F: FnMut() -> BenchResult<u64>,
{
    for _ in 0..WARMUP {
        black_box(operation()?);
    }
    let start = Instant::now();
    let mut result = 0_u64;
    for _ in 0..ITERATIONS {
        result ^= black_box(operation()?);
    }
    let elapsed = start.elapsed();
    println!(
        "{label}: {:.3} ms/frame checksum={result}",
        elapsed.as_secs_f64() * 1_000.0 / ITERATIONS as f64
    );
    Ok(())
}

struct PackedYuv<'a> {
    y: &'a mut [u16],
    u: &'a mut [u16],
    v: &'a mut [u16],
    width: usize,
    height: usize,
}

impl vapoursynth_composite::YuvSink for PackedYuv<'_> {
    fn dimensions(&self) -> (usize, usize) {
        (self.width, self.height)
    }

    fn write_row(&mut self, row: usize, y: &[u16], u: &[u16], v: &[u16]) {
        let start = row * self.width;
        self.y[start..start + self.width].copy_from_slice(y);
        self.u[start..start + self.width].copy_from_slice(u);
        self.v[start..start + self.width].copy_from_slice(v);
    }
}
