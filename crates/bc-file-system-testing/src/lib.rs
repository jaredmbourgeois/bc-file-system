//! What a consumer's tests of `bc-file-system` need and production never sees: an [`ObservedEngine`], the engine
//! with the observer installed, whose scripted [`Fault`]s fail or stop a system call exactly where a test says
//! and whose step counts say what an operation did; and a [`TemporaryDirectory`] for the test to own.
//!
//! A consumer names this crate under `[dev-dependencies]` only. The test double for a file system is the real
//! file system, in a temporary directory, behind the engine's observer: the operations a test runs are
//! `bc-file-system`'s own, on [`ObservedEngine::engine`], so nothing here is a second implementation to keep
//! honest.
//!
//! This crate holds no `unsafe` (its lint table forbids it): it reaches the core only through the safe,
//! documentation-hidden door `bc_file_system::testing_support`, which it wraps as its public API.
#![deny(missing_docs)]

mod observed;
mod temporary_directory;

pub use observed::{Fault, FaultAction, ObservedEngine};
pub use temporary_directory::TemporaryDirectory;
