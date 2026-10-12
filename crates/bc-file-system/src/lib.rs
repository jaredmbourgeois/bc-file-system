//! The Rust face of bc-file-system: a file system that code is handed instead of one it reaches for.
//!
//! The crate compiles the C core from this repository (`core/src/*.c`) and exposes it through typed Rust: an
//! [`Engine`] created once at a composition root with [`Engine::live`], the error vocabulary ([`Error`],
//! [`ErrorKind`], [`Operation`], [`Step`]) every face shares, and, for tests, an [`ObservedEngine`] whose scripted
//! [`Fault`]s fail or stop a system call exactly where a test says. The typed operations grow at stage
//! `rust-crates`; this stage carries the engine, the errors and the one operation the ABI smoke drives.
//!
//! The raw declarations are the [`ffi`] module, public so a reader can hold a Rust declaration against the C
//! header line by line; `unsafe` lives there and in the live module and nowhere else, under
//! `unsafe_code = "deny"` with an `#[expect]` at each of the two.
//!
//! # The consumer-host floor
//!
//! A consumer's `cargo build` compiles the C core with the consumer's own C compiler, so the published
//! consumer-host floor is a property of this crate: GCC 14, clang 19, Apple clang 21 (Xcode 27), NDK r28 or
//! later, Emscripten 2025 or later (C23 in the GNU dialect with `<stdckdint.h>`). The build script names the
//! floor and the compiler it found when a host falls below it. The Rust floor is the declared `rust-version`.
#![deny(missing_docs)]

mod error;
pub mod ffi;
mod live;

pub use error::{Error, ErrorKind, Operation, Step, UnknownValue};
pub use live::{Engine, EntryType, Fault, FaultAction, FinalSymlink, ObservedEngine, Status};
