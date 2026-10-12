//! The advisory lock [`Engine::try_lock`](crate::Engine::try_lock) takes, and its two-way answer.

use core::marker::PhantomData;
use core::ptr::NonNull;

use crate::ffi;
use crate::live::Engine;

/// An advisory exclusive lock on a file or a directory, let go when dropped. What a lock is: the platform's
/// `flock`, advisory (a process that does not ask sees nothing), held by the open-file description this value
/// owns rather than by the process, so two locks in one process contend like two processes do. It borrows the
/// engine, which the header requires to outlive every lock taken through it. Used from one thread at a time: it
/// may move to another thread (`Send`) but not be shared between two (`!Sync`).
#[derive(Debug)]
pub struct Lock<'engine> {
    /// The core's handle; released by the `Drop` in the live module, which holds every call into the core.
    pub(crate) handle: NonNull<ffi::bc_file_system_lock>,
    pub(crate) engine: PhantomData<&'engine Engine>,
}

/// The answer of [`Engine::try_lock`](crate::Engine::try_lock): the lock, or the fact that someone else holds it.
/// Held elsewhere is an answer, not an error: nothing was waited for, and a caller decides what it means.
#[derive(Debug)]
#[must_use = "dropping an acquired lock releases it at once"]
pub enum LockOutcome<'engine> {
    /// The lock is this caller's until the value is dropped.
    Acquired(Lock<'engine>),
    /// Another process or descriptor holds the lock.
    HeldElsewhere,
}
