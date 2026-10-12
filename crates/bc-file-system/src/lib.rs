//! The Rust face of bc-file-system: a file system that code is handed instead of one it reaches for.
//!
//! The crate compiles the C core from this repository (`core/src/*.c`) and exposes it through typed Rust. An
//! [`Engine`] is created once at a composition root with [`Engine::live`] and handed down by reference; every
//! operation is a `&self` method on it, one call into the core plus the conversion of its answer, taking a
//! [`Location`] (an [`AbsolutePath`], or a [`RelativePath`] beneath an open [`Directory`] with containment
//! enforced by the kernel) and stating every choice it makes ([`WriteOptions`], [`ReadOptions`], [`Permissions`],
//! [`Durability`], [`FinalSymlink`], [`ExistingDestination`]; none has a default). A failure is an [`Error`]: the
//! kind to act on, the operation and the step that failed, the errno, and the full path composed from the
//! location. The [`path`] module holds the lexical rules of path text, and [`lines`] and
//! [`Engine::read_file_if_present`] are the compositions every face carries.
//!
//! What a consumer's tests need and production never sees (the observed engine whose scripted faults fail or stop
//! a system call where a test says, and a temporary directory) is the companion crate `bc-file-system-testing`.
//!
//! The raw declarations are the [`ffi`] module, public so a reader can hold a Rust declaration against the C
//! header line by line; `unsafe` lives there and in the live module and nowhere else, under
//! `unsafe_code = "deny"` with an `#[expect]` at each of the two.
//!
//! The crate is experimental until a production consumer runs on it.
//!
//! # The consumer-host floor
//!
//! A consumer's `cargo build` compiles the C core with the consumer's own C compiler, so the published
//! consumer-host floor is a property of this crate: GCC 14, clang 19, Apple clang 21 (Xcode 27), NDK r28 or
//! later, Emscripten 2025 or later (C23 in the GNU dialect with `<stdckdint.h>`). The build script names the
//! floor and the compiler it found when a host falls below it. The Rust floor is the declared `rust-version`. The
//! core is POSIX, so the crate builds for the Unix family only.
#![deny(missing_docs)]

mod buffer;
mod compositions;
mod error;
pub mod ffi;
mod live;
mod location;
mod lock;
mod options;
pub mod path;
mod status;

pub use compositions::{Lines, lines};
pub use error::{Error, ErrorDetails, ErrorKind, FailedLocation, Operation, Step, UnknownValue};
pub use live::Engine;
#[doc(hidden)]
pub use live::testing_support;
pub use location::{Directory, Location};
pub use lock::{Lock, LockOutcome};
pub use options::{
    Durability, ExistingDestination, FinalSymlink, Permissions, PermissionsPolicy, ReadLimit,
    ReadOptions, WriteOptions,
};
pub use path::{AbsolutePath, RelativePath};
pub use status::{Entry, EntryType, Status};
