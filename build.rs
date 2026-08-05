//! Builds the calibrated LUT data embedded by the Rust plugin.

use std::fmt::Write as _;
use std::fs;
use std::path::PathBuf;

struct Table<'a> {
    source_name: &'a str,
    rust_name: &'a str,
    length: usize,
}

fn parse_table<'a>(source: &'a str, table: &Table<'_>) -> Result<Vec<&'a str>, String> {
    let declaration = format!("const double {}[", table.source_name);
    let declaration_start = source
        .find(&declaration)
        .ok_or_else(|| format!("missing {}", table.source_name))?;
    let initializer_start = source[declaration_start..]
        .find('{')
        .map(|offset| declaration_start + offset + 1)
        .ok_or_else(|| format!("missing initializer for {}", table.source_name))?;
    let initializer_end = source[initializer_start..]
        .find("};")
        .map(|offset| initializer_start + offset)
        .ok_or_else(|| format!("unterminated initializer for {}", table.source_name))?;

    let mut values = Vec::with_capacity(table.length);
    for value in source[initializer_start..initializer_end]
        .split(',')
        .map(str::trim)
        .filter(|value| !value.is_empty())
    {
        value
            .parse::<f32>()
            .map_err(|_| format!("invalid {} value {value:?}", table.source_name))?;
        values.push(value);
    }
    if values.len() != table.length {
        return Err(format!(
            "{} length changed: expected {}, got {}; update the Rust contract deliberately",
            table.source_name,
            table.length,
            values.len()
        ));
    }
    Ok(values)
}

fn main() {
    if let Err(error) = run() {
        panic!("{error}");
    }
}

fn run() -> Result<(), String> {
    println!("cargo:rerun-if-changed=src/lut_tables.c");

    let source = fs::read_to_string("src/lut_tables.c").map_err(|error| error.to_string())?;
    let tables = [
        Table {
            source_name: "comp_lut_builtin_pal_2d",
            rust_name: "PAL_2D",
            length: 1_280,
        },
        Table {
            source_name: "comp_lut_builtin_pal_3d",
            rust_name: "PAL_3D",
            length: 6_144,
        },
        Table {
            source_name: "comp_lut_builtin_ntsc",
            rust_name: "NTSC_3D",
            length: 12_288,
        },
    ];

    let mut generated = String::from("// Generated from src/lut_tables.c by build.rs.\n");
    for table in &tables {
        writeln!(
            generated,
            "pub(crate) static {}: [f32; {}] = [",
            table.rust_name, table.length
        )
        .map_err(|error| error.to_string())?;
        for value in parse_table(&source, table)? {
            writeln!(generated, "    {value}_f32,").map_err(|error| error.to_string())?;
        }
        generated.push_str("];\n");
    }

    let output_directory =
        std::env::var_os("OUT_DIR").ok_or_else(|| "missing OUT_DIR".to_owned())?;
    let destination = PathBuf::from(output_directory).join("builtin_lut.rs");
    fs::write(destination, generated).map_err(|error| error.to_string())
}
