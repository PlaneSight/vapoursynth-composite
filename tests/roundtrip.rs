//! End-to-end encode/decode tests.

use vapoursynth_composite::{
    DecodeMode, Decoder, Encoder, FrameIndex, Plane, PlaneMut, Setup, Standard,
};

fn flat_roundtrip(
    standard: Standard,
    y_value: u16,
    u_value: u16,
    v_value: u16,
) -> Result<(), Box<dyn std::error::Error>> {
    let width = standard.raster().width;
    let height = 8;
    let y = vec![y_value; width * height];
    let u = vec![u_value; width * height];
    let v = vec![v_value; width * height];
    let mut composite = vec![0_u16; width * height];

    let encoder = Encoder::new(standard, Setup::None, false);
    encoder.encode(
        FrameIndex(0),
        0,
        Plane::new(&y, width, height, width)?,
        Plane::new(&u, width, height, width)?,
        Plane::new(&v, width, height, width)?,
        PlaneMut::new(&mut composite, width, height, width)?,
    )?;

    assert!(
        composite
            .iter()
            .all(|&sample| (0x0100..=0xfeff).contains(&sample))
    );

    let mut decoded_y = vec![0_u16; width * height];
    let mut decoded_u = vec![0_u16; width * height];
    let mut decoded_v = vec![0_u16; width * height];
    let decoder = Decoder::new(standard, Setup::None, DecodeMode::Notch);
    decoder.decode(
        FrameIndex(0),
        0,
        Plane::new(&composite, width, height, width)?,
        PlaneMut::new(&mut decoded_y, width, height, width)?,
        PlaneMut::new(&mut decoded_u, width, height, width)?,
        PlaneMut::new(&mut decoded_v, width, height, width)?,
    )?;

    // The FIR wings intentionally see blank chroma. Check only the stable
    // interior, and use an error bound rather than asserting an implementation
    // snapshot of the fixed-point rounding.
    for row in 2..height - 2 {
        for column in 24..width - 24 {
            let index = row * width + column;
            assert!(decoded_y[index].abs_diff(y_value) <= 512);
            assert!(decoded_u[index].abs_diff(u_value) <= 1_024);
            assert!(decoded_v[index].abs_diff(v_value) <= 1_024);
        }
    }

    Ok(())
}

#[test]
fn pal_neutral_gray_roundtrips() -> Result<(), Box<dyn std::error::Error>> {
    flat_roundtrip(Standard::Pal, 32_768, 32_768, 32_768)
}

#[test]
fn ntsc_neutral_gray_roundtrips() -> Result<(), Box<dyn std::error::Error>> {
    flat_roundtrip(Standard::Ntsc, 32_768, 32_768, 32_768)
}
