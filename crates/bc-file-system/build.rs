//! Compiles the C core (`core/src/*.c`) into this crate with the `cc` crate.
//!
//! This is the build's composition root: the one place that reads the environment (cargo's `CARGO_MANIFEST_DIR`)
//! and the directory of C sources. The source list is every `.c` under `core/src`, read from the directory, so it
//! is the list `CMake` and `SwiftPM` compile by construction; `scripts/check_build_parity.py` holds the standard
//! spelled below (`gnu2x`) equal to theirs. The flags are the standard and warnings alone: hardening and
//! sanitizers are a consumer's profile (the core's own `CMake` posture in its CI), never something a `cc` build
//! imposes on every embedder, and no `-D` is passed, because the one translation unit that needs a feature-test
//! macro defines it at its top under `__linux__`.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

/// The published consumer-host floor: a host below it fails on the first C23 feature with a diagnostic that
/// names no cause, so the failure is turned into one that does.
const COMPILER_FLOOR: &str = "GCC 14, clang 19, Apple clang 21 (Xcode 27), NDK r28 or later, Emscripten 2025 or later (C23 in the GNU dialect with <stdckdint.h>)";

/// Every `.c` file directly under `core/src`, sorted so the compile order is the same on every machine.
fn core_sources(core_src: &Path) -> Vec<PathBuf> {
    let entries = fs::read_dir(core_src).unwrap_or_else(|error| {
        panic!(
            "cannot read the C sources at {}: {error}",
            core_src.display()
        )
    });
    let mut sources: Vec<PathBuf> = entries
        .map(|entry| {
            entry.unwrap_or_else(|error| {
                panic!("cannot read an entry of {}: {error}", core_src.display())
            })
        })
        .map(|entry| entry.path())
        .filter(|path| path.extension().is_some_and(|extension| extension == "c"))
        .collect();
    sources.sort();
    assert!(
        !sources.is_empty(),
        "no .c file under {}",
        core_src.display()
    );
    sources
}

fn main() {
    let manifest_directory = PathBuf::from(
        env::var_os("CARGO_MANIFEST_DIR")
            .expect("cargo sets CARGO_MANIFEST_DIR for every build script"),
    );
    let core = manifest_directory.join("..").join("..").join("core");
    let core_src = core.join("src");
    let core_include = core.join("include");

    let mut build = cc::Build::new();
    build.std("gnu2x").warnings(true).include(&core_include);
    for source in core_sources(&core_src) {
        build.file(source);
    }
    // The whole core directory: a header edit must rebuild as surely as a source edit.
    println!("cargo:rerun-if-changed={}", core.display());

    if let Err(error) = build.try_compile("bc_file_system") {
        let compiler = build.get_compiler();
        panic!(
            "the bc-file-system C core did not compile with {}: {error}\nthe published consumer-host floor is {COMPILER_FLOOR}; set CC to a compiler at or above it",
            compiler.path().display()
        );
    }
}
