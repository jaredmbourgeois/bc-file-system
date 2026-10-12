//! The engine and every call into the core: the one module beside `ffi` that holds `unsafe`.
//!
//! [`Engine::live`] is the Rust face's live seam: naming it is itself a reach, legal only at a composition root
//! (AGENTS.md, "Inject, never reach"), which is why this file is the Rust entry of `REACH_IS_LEGAL_IN` in
//! `scripts/check_house_rules.py`. Each operation is one call into the core plus the conversion of its answer;
//! the value types live in modules of their own and cross through their `to_c` and `TryFrom`.
//!
//! Every `unsafe` block below relies on the header's pointer contract, stated once here and restated briefly at
//! each block: every pointer the core takes is a borrow for the call (the header's POINTERS paragraph); a
//! location borrows bytes and a directory that the borrow checker keeps alive for the call; each out-parameter
//! and the error struct is a live local the core writes only as the header says (an answer on success, the
//! error on failure); a buffer is a slice whose length is the capacity passed beside it; and the engine's handle
//! is live while the [`Engine`] is.
//!
//! The observer's door, [`testing_support`], is here too, because it is calls into the core; the companion crate
//! `bc-file-system-testing` wraps it as its public API.
#![expect(
    unsafe_code,
    reason = "the calls into the C core and the thread-safety proofs of its handles; nowhere else"
)]

use core::marker::PhantomData;
use core::ptr::NonNull;
use std::ffi::OsString;
use std::ops::Range;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard, PoisonError};

use crate::buffer::{
    FIRST_ENTRY_CAPACITY, FIRST_TEXT_CAPACITY, first_read_capacity, grown_answer, measured_answer,
};
use crate::error::{Error, Operation, Step, UnknownValue, path_of_bytes};
use crate::ffi;
use crate::location::{Directory, Location};
use crate::lock::{Lock, LockOutcome};
use crate::options::{
    Durability, ExistingDestination, FinalSymlink, Permissions, ReadOptions, WriteOptions,
};
use crate::status::{Entry, Status};

/// The context every operation runs in: the platform's fast paths, probed once at creation, and, on an engine
/// created for tests, the observer. Created at a composition root with [`Engine::live`], handed down by
/// reference, released when dropped. Every operation is a `&self` method: a live engine is shared, not lent.
///
/// `Send` and `Sync`: the header's THREADS paragraph promises that a live engine is immutable after creation and
/// may be used from any number of threads at once (its one mutable field, the staging counter, is atomic in the
/// core), and confines an engine created for tests to one thread at a time, which this face keeps by making
/// every call into such an engine take its turn first. The proofs are the two `unsafe impl`s below.
#[derive(Debug)]
pub struct Engine {
    handle: NonNull<ffi::bc_file_system_engine>,
    /// `Some` on an engine created for tests, `None` on a live one. A shared `&Engine` may cross threads, so the
    /// exclusive borrow of the observed engine's owner cannot by itself keep two threads out of the observer's
    /// step counts; this lock does, and a live engine pays one comparison per call for it.
    observed_turn: Option<Mutex<()>>,
}

// SAFETY: a live engine is immutable after creation but for an atomic counter (the header, THREADS); an engine
// created for tests is only ever called with its turn held (`Engine::one_call`), so moving either to another
// thread changes nothing the core relies on.
unsafe impl Send for Engine {}
// SAFETY: as above: concurrent live calls read the same probes and draw from the core's atomic counter; calls into
// an engine created for tests are serialised by `observed_turn`, as the header's "one thread at a time" requires.
unsafe impl Sync for Engine {}

// SAFETY: a directory handle is an open descriptor and its remembered text; the header asks only that one thread
// use it at a time, which moving it keeps (it is not `Sync`, so no two threads can share it).
unsafe impl Send for Directory<'_> {}
// SAFETY: a lock handle is the descriptor the lock is held by; closing it from another thread releases the lock as
// closing it here would, and it is not `Sync`, so one thread uses it at a time, as the header asks.
unsafe impl Send for Lock<'_> {}

/// What one call into the core runs with: the engine's handle and, on an engine created for tests, the turn that
/// keeps a second thread's call out until this one returns.
struct OneCall<'engine> {
    handle: *mut ffi::bc_file_system_engine,
    _turn: Option<MutexGuard<'engine, ()>>,
}

/// The core's one return convention: 0 is success, anything else is the kind the error struct holds.
fn checked(
    status: i32,
    c_error: &ffi::bc_file_system_error,
) -> Result<(), Box<ffi::bc_file_system_error>> {
    if status == 0 {
        return Ok(());
    }
    Err(Box::new(*c_error))
}

/// A failed operation's error, with its path composed from the location the operation was handed.
fn failure(
    c_error: &ffi::bc_file_system_error,
    called: Operation,
    location: &Location<'_>,
) -> Error {
    Error::from_c(c_error, called, &location.text())
}

/// The durability the core says it achieved, refused should it be outside the enum.
fn achieved(value: i32, called: Operation, location: &Location<'_>) -> Result<Durability, Error> {
    Durability::try_from(value)
        .map_err(|unknown| Error::answer_outside_enum(called, unknown, &location.text()))
}

/// One of the two factories, called with the operation it reports under.
fn create(
    factory: unsafe extern "C" fn(
        *mut *mut ffi::bc_file_system_engine,
        *mut ffi::bc_file_system_error,
    ) -> i32,
    called: Operation,
) -> Result<NonNull<ffi::bc_file_system_engine>, Error> {
    let mut handle: *mut ffi::bc_file_system_engine = core::ptr::null_mut();
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `handle` and `c_error` are live locals for the call, which is the header's whole contract for the
    // two pointers; the core writes the handle only on success and the error only on failure.
    let status = unsafe { factory(&raw mut handle, &raw mut c_error) };
    checked(status, &c_error).map_err(|c_error| Error::from_c(&c_error, called, Path::new("")))?;
    NonNull::new(handle).ok_or_else(|| Error::from_c(&c_error, called, Path::new("")))
}

impl Engine {
    /// The live engine of a composition root, with the platform's fast paths probed once.
    ///
    /// # Errors
    ///
    /// `ResourceExhausted` when the engine cannot be allocated; the probes themselves cannot fail.
    pub fn live() -> Result<Self, Error> {
        let handle = create(
            ffi::bc_file_system_engine_create_live,
            Operation::EngineCreateLive,
        )?;
        Ok(Self {
            handle,
            observed_turn: None,
        })
    }

    /// The handle for one call, with the observer's turn taken first on an engine created for tests. A lock
    /// poisoned by a panic elsewhere guards nothing (it holds no data), so its turn is taken all the same.
    fn one_call(&self) -> OneCall<'_> {
        OneCall {
            handle: self.handle.as_ptr(),
            _turn: self
                .observed_turn
                .as_ref()
                .map(|turn| turn.lock().unwrap_or_else(PoisonError::into_inner)),
        }
    }

    /// Opens the directory at `location` and remembers the text it was opened with: the path for `At`, the
    /// parent's remembered text, a slash and the relative path for `Beneath`. The root, `/`, opens.
    ///
    /// # Errors
    ///
    /// `NotDirectory` for something that is not a directory; `NotFound` for nothing; `SymlinkRefused` for a
    /// final symlink under `Refuse`; `Unsupported` where the platform cannot enforce containment beneath a
    /// directory (Emscripten), for every location beneath one.
    pub fn open_directory(
        &self,
        location: &Location<'_>,
        final_symlink: FinalSymlink,
    ) -> Result<Directory<'_>, Error> {
        let c_location = location.to_c();
        let mut handle: *mut ffi::bc_file_system_directory = core::ptr::null_mut();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `c_location` borrows what `location` borrows; `handle` receives the new
        // directory on success; `c_error` is filled on failure.
        let status = unsafe {
            ffi::bc_file_system_directory_open(
                call.handle,
                &raw const c_location,
                final_symlink as i32,
                &raw mut handle,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::OpenDirectory, location))?;
        let handle = NonNull::new(handle)
            .ok_or_else(|| failure(&c_error, Operation::OpenDirectory, location))?;
        Ok(Directory {
            handle,
            opened_as_text: location.text().into_os_string().into_vec(),
            engine: PhantomData,
        })
    }

    /// What is at `location`, or `None` when nothing is: absence (a missing entry, or a missing or
    /// non-directory component on the way) is an answer, not an error.
    ///
    /// # Errors
    ///
    /// `SymlinkRefused` for a symlink on the way beneath a directory; `PermissionDenied` for a directory that
    /// cannot be searched; `IoFailed` for any other system failure.
    pub fn status(
        &self,
        location: &Location<'_>,
        final_symlink: FinalSymlink,
    ) -> Result<Option<Status>, Error> {
        let c_location = location.to_c();
        let mut present: i32 = 0;
        let mut c_status = ffi::bc_file_system_status::zeroed();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `present` and `c_status` are written on success, `c_error` on failure.
        let status = unsafe {
            ffi::bc_file_system_status_of(
                call.handle,
                &raw const c_location,
                final_symlink as i32,
                &raw mut present,
                &raw mut c_status,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::Status, location))?;
        if present == 0 {
            return Ok(None);
        }
        Status::try_from(&c_status).map(Some).map_err(|unknown| {
            Error::answer_outside_enum(Operation::Status, unknown, &location.text())
        })
    }

    /// The bytes of the regular file at `location`, under the read limit. The buffer protocol is absorbed
    /// here: at most three attempts, then the error as received.
    ///
    /// # Errors
    ///
    /// `NotRegularFile` for anything that is not a regular file; `TooLarge` for a file past the limit or past
    /// `usize`; `NotFound`, `SymlinkRefused`, `PermissionDenied`, `IoFailed`; `BufferTooSmall` only for a file
    /// that grew on every one of the three attempts.
    pub fn read_file(
        &self,
        location: &Location<'_>,
        options: &ReadOptions,
    ) -> Result<Vec<u8>, Error> {
        let c_location = location.to_c();
        let c_options = options.to_c();
        let call = self.one_call();
        grown_answer(
            first_read_capacity(options.limit),
            0_u8,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut ffi::bc_file_system_error| {
                // SAFETY: the module's contract: `buffer` is `buffer.len()` writable bytes the loop owns;
                // `length` is written on success and on `BufferTooSmall`, `c_error` on failure.
                unsafe {
                    ffi::bc_file_system_read_file(
                        call.handle,
                        &raw const c_location,
                        &raw const c_options,
                        buffer.as_mut_ptr(),
                        buffer.len(),
                        length,
                        c_error,
                    )
                }
            },
        )
        .map_err(|c_error| failure(&c_error, Operation::ReadFile, location))
    }

    /// Replaces or creates the regular file at `location` with `bytes`, atomically: a reader sees the old
    /// content or the new, never a mixture. The directory must exist. Answers the durability achieved.
    ///
    /// # Errors
    ///
    /// `AlreadyExists` under `Refuse`; `NotFound` for a missing directory; `NotRegularFile`, `SymlinkRefused`,
    /// `PermissionDenied`, `NoSpace`, `IoFailed`; `InvalidArgument` for a mode outside `0o7777`.
    pub fn write_file(
        &self,
        location: &Location<'_>,
        bytes: &[u8],
        options: &WriteOptions,
    ) -> Result<Durability, Error> {
        let c_location = location.to_c();
        let c_options = options.to_c();
        let mut durability_achieved: i32 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `bytes` is `bytes.len()` readable bytes; `durability_achieved` is
        // written on success, `c_error` on failure.
        let status = unsafe {
            ffi::bc_file_system_write_file(
                call.handle,
                &raw const c_location,
                bytes.as_ptr(),
                bytes.len(),
                &raw const c_options,
                &raw mut durability_achieved,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::WriteFile, location))?;
        achieved(durability_achieved, Operation::WriteFile, location)
    }

    /// Adds `bytes` to the end of the regular file at `location`, creating it with `permissions` when it is
    /// absent. Not atomic: after an error some leading part of the bytes may be in the file. Answers the
    /// durability achieved.
    ///
    /// # Errors
    ///
    /// `NotRegularFile`, `NotFound`, `SymlinkRefused`, `PermissionDenied`, `NoSpace`, `IoFailed`.
    pub fn append(
        &self,
        location: &Location<'_>,
        bytes: &[u8],
        durability: Durability,
        permissions: Permissions,
    ) -> Result<Durability, Error> {
        let c_location = location.to_c();
        let c_permissions = permissions.to_c();
        let mut durability_achieved: i32 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `bytes` is `bytes.len()` readable bytes; `c_permissions` is a live
        // local; `durability_achieved` is written on success, `c_error` on failure.
        let status = unsafe {
            ffi::bc_file_system_append(
                call.handle,
                &raw const c_location,
                bytes.as_ptr(),
                bytes.len(),
                durability as i32,
                &raw const c_permissions,
                &raw mut durability_achieved,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::Append, location))?;
        achieved(durability_achieved, Operation::Append, location)
    }

    /// Pushes the file or directory at `location` to stable storage. A volume that offers no sync answers
    /// `Written`.
    ///
    /// # Errors
    ///
    /// `NotFound`, `SymlinkRefused`, `PermissionDenied`; `IoFailed` for a sync of its own that failed.
    pub fn sync(&self, location: &Location<'_>) -> Result<Durability, Error> {
        let c_location = location.to_c();
        let mut durability_achieved: i32 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `durability_achieved` is written on success, `c_error` on failure.
        let status = unsafe {
            ffi::bc_file_system_sync(
                call.handle,
                &raw const c_location,
                &raw mut durability_achieved,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::Sync, location))?;
        achieved(durability_achieved, Operation::Sync, location)
    }

    /// Creates one directory with exactly `permissions`' mode.
    ///
    /// # Errors
    ///
    /// `AlreadyExists` for one already there; `NotFound` for a missing parent; `PermissionDenied`,
    /// `NoSpace`, `IoFailed`.
    pub fn create_directory(
        &self,
        location: &Location<'_>,
        permissions: Permissions,
    ) -> Result<(), Error> {
        let c_location = location.to_c();
        let c_permissions = permissions.to_c();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `c_permissions` is a live local; `c_error` is filled on failure.
        let status = unsafe {
            ffi::bc_file_system_create_directory(
                call.handle,
                &raw const c_location,
                &raw const c_permissions,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::CreateDirectory, location))
    }

    /// Creates the directory and every missing one on the way, each with exactly `permissions`' mode; one
    /// already there is left as it is.
    ///
    /// # Errors
    ///
    /// `NotDirectory` for something on the way that is not a directory; `PermissionDenied`, `NoSpace`,
    /// `IoFailed`.
    pub fn create_directories(
        &self,
        location: &Location<'_>,
        permissions: Permissions,
    ) -> Result<(), Error> {
        let c_location = location.to_c();
        let c_permissions = permissions.to_c();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `c_permissions` is a live local; `c_error` is filled on failure.
        let status = unsafe {
            ffi::bc_file_system_create_directories(
                call.handle,
                &raw const c_location,
                &raw const c_permissions,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::CreateDirectories, location))
    }

    /// The entries of the directory at `location`, sorted by name bytes so a listing is the same on every run;
    /// complete or an error. A symlink inside is an entry, never followed.
    ///
    /// # Errors
    ///
    /// `NotDirectory`, `NotFound`, `SymlinkRefused`, `PermissionDenied`, `IoFailed`; `BufferTooSmall` only for
    /// a directory that grew on every one of the three attempts.
    pub fn list_directory(
        &self,
        location: &Location<'_>,
        final_symlink: FinalSymlink,
    ) -> Result<Vec<Entry>, Error> {
        let c_location = location.to_c();
        let call = self.one_call();
        let c_entries = grown_answer(
            FIRST_ENTRY_CAPACITY,
            ffi::bc_file_system_entry::zeroed(),
            |entries: &mut [ffi::bc_file_system_entry],
             count: &mut usize,
             c_error: &mut ffi::bc_file_system_error| {
                // SAFETY: the module's contract: `entries` is `entries.len()` writable entries the loop owns;
                // `count` is written on success and on `BufferTooSmall`, `c_error` on failure.
                unsafe {
                    ffi::bc_file_system_list_directory(
                        call.handle,
                        &raw const c_location,
                        final_symlink as i32,
                        entries.as_mut_ptr(),
                        entries.len(),
                        count,
                        c_error,
                    )
                }
            },
        )
        .map_err(|c_error| failure(&c_error, Operation::ListDirectory, location))?;
        c_entries
            .iter()
            .map(Entry::try_from)
            .collect::<Result<Vec<Entry>, UnknownValue>>()
            .map_err(|unknown| {
                Error::answer_outside_enum(Operation::ListDirectory, unknown, &location.text())
            })
    }

    /// Removes the file, symlink or other non-directory at `location`; answers whether there was one.
    ///
    /// # Errors
    ///
    /// `NotRegularFile` for a directory; `PermissionDenied`, `IoFailed`.
    pub fn remove_file(&self, location: &Location<'_>) -> Result<bool, Error> {
        self.remove(
            location,
            ffi::bc_file_system_remove_file,
            Operation::RemoveFile,
        )
    }

    /// Removes the empty directory at `location`; answers whether there was one.
    ///
    /// # Errors
    ///
    /// `DirectoryNotEmpty`; `NotDirectory` for something that is not a directory; `PermissionDenied`,
    /// `IoFailed`.
    pub fn remove_directory(&self, location: &Location<'_>) -> Result<bool, Error> {
        self.remove(
            location,
            ffi::bc_file_system_remove_directory,
            Operation::RemoveDirectory,
        )
    }

    /// Removes `location` and everything below it, never following a symlink, never the root of the file system,
    /// never across a device; answers whether there was anything. After an error part of the tree may be gone.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for the root; `CrossDevice` for a tree that crosses onto another device;
    /// `PermissionDenied`, `IoFailed`.
    pub fn remove_tree(&self, location: &Location<'_>) -> Result<bool, Error> {
        self.remove(
            location,
            ffi::bc_file_system_remove_tree,
            Operation::RemoveTree,
        )
    }

    /// The three removals share one shape: a location in, whether something was there out.
    fn remove(
        &self,
        location: &Location<'_>,
        removal: unsafe extern "C" fn(
            *mut ffi::bc_file_system_engine,
            *const ffi::bc_file_system_location,
            *mut i32,
            *mut ffi::bc_file_system_error,
        ) -> i32,
        called: Operation,
    ) -> Result<bool, Error> {
        let c_location = location.to_c();
        let mut existed: i32 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `existed` is written on success, `c_error` on failure.
        let status = unsafe {
            removal(
                call.handle,
                &raw const c_location,
                &raw mut existed,
                &raw mut c_error,
            )
        };
        checked(status, &c_error).map_err(|c_error| failure(&c_error, called, location))?;
        Ok(existed != 0)
    }

    /// Creates a directory with a unique name inside `parent`, with `permissions`, and answers that name, one
    /// component.
    ///
    /// # Errors
    ///
    /// `NotFound`, `NotDirectory`, `PermissionDenied`, `NoSpace`, `IoFailed`.
    pub fn create_temporary_directory(
        &self,
        parent: &Location<'_>,
        permissions: Permissions,
    ) -> Result<OsString, Error> {
        let c_parent = parent.to_c();
        let c_permissions = permissions.to_c();
        let call = self.one_call();
        let name = grown_answer(
            FIRST_TEXT_CAPACITY,
            0_u8,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut ffi::bc_file_system_error| {
                // SAFETY: the module's contract: `c_permissions` is a live local; `buffer` is `buffer.len()`
                // writable bytes; `length` is written on success and on `BufferTooSmall`, `c_error` on failure.
                unsafe {
                    ffi::bc_file_system_create_temporary_directory(
                        call.handle,
                        &raw const c_parent,
                        &raw const c_permissions,
                        buffer.as_mut_ptr(),
                        buffer.len(),
                        length,
                        c_error,
                    )
                }
            },
        )
        .map_err(|c_error| failure(&c_error, Operation::CreateTemporaryDirectory, parent))?;
        Ok(OsString::from_vec(name))
    }

    /// Renames `source` to `destination` in one step: under `Replace` a file replaces a file and a directory an
    /// empty directory; under `Refuse` an existing destination is `AlreadyExists` and untouched. Answers the
    /// durability achieved. An error names the end it belongs to ([`crate::ErrorDetails::failed_location`]), and
    /// its path is composed from that end.
    ///
    /// # Errors
    ///
    /// `NotFound` for a missing source; `AlreadyExists`, `DirectoryNotEmpty`, `CrossDevice`, `NotDirectory`,
    /// `NotRegularFile`, `PermissionDenied`, `IoFailed`; `InvalidArgument` for the root at either end.
    pub fn rename(
        &self,
        source: &Location<'_>,
        destination: &Location<'_>,
        existing_destination: ExistingDestination,
        durability: Durability,
    ) -> Result<Durability, Error> {
        let c_source = source.to_c();
        let c_destination = destination.to_c();
        let mut durability_achieved: i32 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract, for both locations: `durability_achieved` is written on success,
        // `c_error` on failure.
        let status = unsafe {
            ffi::bc_file_system_rename(
                call.handle,
                &raw const c_source,
                &raw const c_destination,
                existing_destination as i32,
                durability as i32,
                &raw mut durability_achieved,
                &raw mut c_error,
            )
        };
        checked(status, &c_error).map_err(|c_error| {
            Error::from_c_two_locations(
                &c_error,
                Operation::Rename,
                &source.text(),
                &destination.text(),
            )
        })?;
        achieved(durability_achieved, Operation::Rename, destination)
    }

    /// Copies the regular file at `source` to `destination` with the guarantees of [`Engine::write_file`]:
    /// atomic at the destination, the platform's clone where it has one, the copy loop otherwise. An error names
    /// the end it belongs to, as [`Engine::rename`]'s does.
    ///
    /// # Errors
    ///
    /// `NotFound` for a missing source; `NotRegularFile`, `AlreadyExists`, `SymlinkRefused`,
    /// `PermissionDenied`, `NoSpace`, `IoFailed`; `InvalidArgument` for the same file at both ends.
    pub fn copy_file(
        &self,
        source: &Location<'_>,
        destination: &Location<'_>,
        options: &WriteOptions,
    ) -> Result<Durability, Error> {
        let c_source = source.to_c();
        let c_destination = destination.to_c();
        let c_options = options.to_c();
        let mut durability_achieved: i32 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract, for both locations: `c_options` is a live local;
        // `durability_achieved` is written on success, `c_error` on failure.
        let status = unsafe {
            ffi::bc_file_system_copy_file(
                call.handle,
                &raw const c_source,
                &raw const c_destination,
                &raw const c_options,
                &raw mut durability_achieved,
                &raw mut c_error,
            )
        };
        checked(status, &c_error).map_err(|c_error| {
            Error::from_c_two_locations(
                &c_error,
                Operation::CopyFile,
                &source.text(),
                &destination.text(),
            )
        })?;
        achieved(durability_achieved, Operation::CopyFile, destination)
    }

    /// Sets the permission bits of the entry at `location` exactly to `mode`.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for bits outside `0o7777`; `NotFound`, `SymlinkRefused`, `PermissionDenied`,
    /// `IoFailed`.
    pub fn set_permissions(
        &self,
        location: &Location<'_>,
        mode: u32,
        final_symlink: FinalSymlink,
    ) -> Result<(), Error> {
        let c_location = location.to_c();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `c_error` is filled on failure.
        let status = unsafe {
            ffi::bc_file_system_set_permissions(
                call.handle,
                &raw const c_location,
                mode,
                final_symlink as i32,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::SetPermissions, location))
    }

    /// Creates a symlink at `link` whose text is `target`, passed to the system unchanged: never validated,
    /// never resolved.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for an empty target, a NUL inside it, or one the system could not hold (`PATH_MAX` or
    /// longer); `AlreadyExists`; `NotFound`, `PermissionDenied`, `IoFailed`.
    pub fn create_symlink(&self, target: &Path, link: &Location<'_>) -> Result<(), Error> {
        let target_bytes = target.as_os_str().as_bytes();
        let c_link = link.to_c();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `target_bytes` is `target_bytes.len()` readable bytes; `c_error` is
        // filled on failure.
        let status = unsafe {
            ffi::bc_file_system_create_symlink(
                call.handle,
                target_bytes.as_ptr(),
                target_bytes.len(),
                &raw const c_link,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::CreateSymlink, link))
    }

    /// The text of the symlink at `location`, whole.
    ///
    /// # Errors
    ///
    /// `IoFailed` with `EINVAL` for an entry that is not a symlink, as the system answers it; `NotFound`,
    /// `PermissionDenied`.
    pub fn read_symlink(&self, location: &Location<'_>) -> Result<PathBuf, Error> {
        let c_location = location.to_c();
        let call = self.one_call();
        let text = grown_answer(
            FIRST_TEXT_CAPACITY,
            0_u8,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut ffi::bc_file_system_error| {
                // SAFETY: the module's contract: `buffer` is `buffer.len()` writable bytes; `length` is written
                // on success and on `BufferTooSmall`, `c_error` on failure.
                unsafe {
                    ffi::bc_file_system_read_symlink(
                        call.handle,
                        &raw const c_location,
                        buffer.as_mut_ptr(),
                        buffer.len(),
                        length,
                        c_error,
                    )
                }
            },
        )
        .map_err(|c_error| failure(&c_error, Operation::ReadSymlink, location))?;
        Ok(PathBuf::from(OsString::from_vec(text)))
    }

    /// Tries to take an advisory exclusive lock on the file or directory at `location` without waiting. The
    /// lock opens the entry itself, read-only and never through a symlink, and never creates it.
    ///
    /// # Errors
    ///
    /// `NotFound` for nothing there; `SymlinkRefused` for a symlink; `NotRegularFile` for anything but a file
    /// or a directory; `Unsupported` where the platform's `flock` is a stub (Emscripten).
    pub fn try_lock(&self, location: &Location<'_>) -> Result<LockOutcome<'_>, Error> {
        let c_location = location.to_c();
        let mut handle: *mut ffi::bc_file_system_lock = core::ptr::null_mut();
        let mut outcome: i32 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `handle` receives the lock under `acquired` and null under
        // `held_elsewhere`, `outcome` is written on success, `c_error` on failure.
        let status = unsafe {
            ffi::bc_file_system_try_lock(
                call.handle,
                &raw const c_location,
                &raw mut handle,
                &raw mut outcome,
                &raw mut c_error,
            )
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::TryLock, location))?;
        let unknown = UnknownValue {
            enumeration: "bc_file_system_lock_outcome",
            value: outcome,
        };
        match (outcome, NonNull::new(handle)) {
            (ffi::LOCK_OUTCOME_ACQUIRED, Some(handle)) => Ok(LockOutcome::Acquired(Lock {
                handle,
                engine: PhantomData,
            })),
            (ffi::LOCK_OUTCOME_HELD_ELSEWHERE, None) => Ok(LockOutcome::HeldElsewhere),
            // An outcome outside the enum, or one that disagrees with the handle, which the core never answers.
            _ => Err(Error::answer_outside_enum(
                Operation::TryLock,
                unknown,
                &location.text(),
            )),
        }
    }

    /// Renames the entry at `location` (a file, a symlink or a directory tree) to `.bc-condemned-<name>` beside
    /// it, in one step that replaces nothing, for a later sweep, and answers the condemned name (one component).
    /// A condemned name a previous condemn left behind is never an error: the name is tried again with a token
    /// of the engine's appended, so the answer may be longer than the first name.
    ///
    /// # Errors
    ///
    /// `NotFound` for nothing there; `InvalidArgument` for the root; `Unsupported` where the platform has no
    /// no-replace rename; `PermissionDenied`, `IoFailed`.
    pub fn condemn(&self, location: &Location<'_>) -> Result<OsString, Error> {
        let c_location = location.to_c();
        let call = self.one_call();
        let name = grown_answer(
            FIRST_TEXT_CAPACITY,
            0_u8,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut ffi::bc_file_system_error| {
                // SAFETY: the module's contract: `buffer` is `buffer.len()` writable bytes; `length` is written
                // on success and on `BufferTooSmall`, `c_error` on failure.
                unsafe {
                    ffi::bc_file_system_condemn(
                        call.handle,
                        &raw const c_location,
                        buffer.as_mut_ptr(),
                        buffer.len(),
                        length,
                        c_error,
                    )
                }
            },
        )
        .map_err(|c_error| failure(&c_error, Operation::Condemn, location))?;
        Ok(OsString::from_vec(name))
    }

    /// Removes every `.bc-condemned-` entry directly inside `parent`, as [`Engine::remove_tree`] would, and
    /// every staged file a killed write or copy left there (its name carries `.bc-tmp-`).
    ///
    /// # Errors
    ///
    /// `NotFound` for no directory there; `NotDirectory` for something else; the first entry the sweep cannot
    /// remove ends it, named in the error's path, and what was removed before it stays removed.
    pub fn remove_condemned(&self, parent: &Location<'_>) -> Result<(), Error> {
        let c_parent = parent.to_c();
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = self.one_call();
        // SAFETY: the module's contract: `c_error` is filled on failure.
        let status = unsafe {
            ffi::bc_file_system_remove_condemned(call.handle, &raw const c_parent, &raw mut c_error)
        };
        checked(status, &c_error)
            .map_err(|c_error| failure(&c_error, Operation::RemoveCondemned, parent))
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        // SAFETY: the handle came from a factory and is released exactly once, here; every directory and lock
        // opened through it borrowed it and so has been released already, as the header requires.
        unsafe { ffi::bc_file_system_engine_release(self.handle.as_ptr()) }
    }
}

impl Directory<'_> {
    /// The text the directory was opened with, read from the core (the face keeps its own copy for composing
    /// error paths; this call is the binding's).
    ///
    /// # Errors
    ///
    /// None in practice: the text is the one the directory was opened with; the signature is the buffer
    /// protocol's.
    pub fn opened_as(&self) -> Result<PathBuf, Error> {
        let text = grown_answer(
            FIRST_TEXT_CAPACITY,
            0_u8,
            |buffer: &mut [u8], length: &mut usize, c_error: &mut ffi::bc_file_system_error| {
                // SAFETY: the directory's handle is live while `self` is; `buffer` is `buffer.len()` writable
                // bytes; `length` is written on success and on `BufferTooSmall`, `c_error` on failure.
                unsafe {
                    ffi::bc_file_system_directory_opened_as(
                        self.handle.as_ptr(),
                        buffer.as_mut_ptr(),
                        buffer.len(),
                        length,
                        c_error,
                    )
                }
            },
        )
        .map_err(|c_error| {
            Error::from_c(
                &c_error,
                Operation::DirectoryOpenedAs,
                path_of_bytes(&self.opened_as_text),
            )
        })?;
        Ok(PathBuf::from(OsString::from_vec(text)))
    }
}

impl Drop for Directory<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from `bc_file_system_directory_open` and is released exactly once, here; every
        // location beneath it borrowed it and so is gone.
        unsafe { ffi::bc_file_system_directory_release(self.handle.as_ptr()) }
    }
}

impl Drop for Lock<'_> {
    fn drop(&mut self) {
        // SAFETY: the handle came from `bc_file_system_try_lock` and is released exactly once, here, which closes
        // the descriptor and so lets the lock go.
        unsafe { ffi::bc_file_system_lock_release(self.handle.as_ptr()) }
    }
}

// ---- The path module's calls ------------------------------------------------------------------------------------
//
// The path module takes bytes and answers prefixes, ranges and new text; no engine. Each function below is one
// call plus the reading of its out-parameters; `path.rs` turns the answers into values. A refusal names, as its
// path, the bytes the core refused: the loose argument's where a function takes one, the first path's otherwise
// (the C++ face's reading).

/// The refusal of one path function, with `text` as the error's path.
fn refused(c_error: &ffi::bc_file_system_error, called: Operation, text: &[u8]) -> Error {
    Error::from_c(c_error, called, path_of_bytes(text))
}

/// Whether `path` is a relative path by the header's rules.
pub(crate) fn relative_path_validate(path: &[u8]) -> Result<(), Error> {
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `path` is `path.len()` readable bytes for the call; `c_error` is filled on failure.
    let status = unsafe {
        ffi::bc_file_system_relative_path_validate(path.as_ptr(), path.len(), &raw mut c_error)
    };
    checked(status, &c_error)
        .map_err(|c_error| refused(&c_error, Operation::RelativePathFrom, path))
}

/// Whether `path` is an absolute path by the header's rules.
pub(crate) fn absolute_path_validate(path: &[u8]) -> Result<(), Error> {
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `path` is `path.len()` readable bytes for the call; `c_error` is filled on failure.
    let status = unsafe {
        ffi::bc_file_system_absolute_path_validate(path.as_ptr(), path.len(), &raw mut c_error)
    };
    checked(status, &c_error)
        .map_err(|c_error| refused(&c_error, Operation::AbsolutePathFrom, path))
}

/// `base`, a slash and `relative`, by two calls.
pub(crate) fn path_join(base: &[u8], relative: &[u8]) -> Result<Vec<u8>, Error> {
    measured_answer(
        |buffer: &mut [u8], length: &mut usize, c_error: &mut ffi::bc_file_system_error| {
            // SAFETY: `base` and `relative` are readable for their lengths; `buffer` is `buffer.len()` writable
            // bytes; `length` is written on success and on `BufferTooSmall`, `c_error` on failure.
            unsafe {
                ffi::bc_file_system_path_join(
                    base.as_ptr(),
                    base.len(),
                    relative.as_ptr(),
                    relative.len(),
                    buffer.as_mut_ptr(),
                    buffer.len(),
                    length,
                    c_error,
                )
            }
        },
    )
    .map_err(|c_error| refused(&c_error, Operation::PathJoin, base))
}

/// How many leading bytes of `path` spell its parent, or `None` for the root and a one-component relative path.
pub(crate) fn path_parent(path: &[u8]) -> Result<Option<usize>, Error> {
    let mut present: i32 = 0;
    let mut parent_length: usize = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `path` is readable for its length; `present` and `parent_length` are written on success, `c_error`
    // on failure.
    let status = unsafe {
        ffi::bc_file_system_path_parent(
            path.as_ptr(),
            path.len(),
            &raw mut present,
            &raw mut parent_length,
            &raw mut c_error,
        )
    };
    checked(status, &c_error).map_err(|c_error| refused(&c_error, Operation::PathParent, path))?;
    Ok((present != 0).then_some(parent_length))
}

/// Where the last component sits inside `path`.
pub(crate) fn path_last_component(path: &[u8]) -> Result<Range<usize>, Error> {
    let mut offset: usize = 0;
    let mut component_length: usize = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `path` is readable for its length; `offset` and `component_length` are written on success,
    // `c_error` on failure.
    let status = unsafe {
        ffi::bc_file_system_path_last_component(
            path.as_ptr(),
            path.len(),
            &raw mut offset,
            &raw mut component_length,
            &raw mut c_error,
        )
    };
    checked(status, &c_error)
        .map_err(|c_error| refused(&c_error, Operation::PathLastComponent, path))?;
    Ok(offset..offset.saturating_add(component_length))
}

/// Where the next component sits inside `path`, advancing `cursor` (0 before the first call), or `None` at the
/// end.
pub(crate) fn path_next_component(
    path: &[u8],
    cursor: &mut usize,
) -> Result<Option<Range<usize>>, Error> {
    let mut offset: usize = 0;
    let mut component_length: usize = 0;
    let mut present: i32 = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `path` is readable for its length; `cursor` is the caller's live counter, read and advanced by the
    // core; `offset`, `component_length` and `present` are written on success, `c_error` on failure.
    let status = unsafe {
        ffi::bc_file_system_path_next_component(
            path.as_ptr(),
            path.len(),
            cursor,
            &raw mut offset,
            &raw mut component_length,
            &raw mut present,
            &raw mut c_error,
        )
    };
    checked(status, &c_error)
        .map_err(|c_error| refused(&c_error, Operation::PathNextComponent, path))?;
    Ok((present != 0).then_some(offset..offset.saturating_add(component_length)))
}

/// How many leading bytes of `component` its stem is.
pub(crate) fn path_stem(component: &[u8]) -> Result<usize, Error> {
    let mut stem_length: usize = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `component` is readable for its length; `stem_length` is written on success, `c_error` on failure.
    let status = unsafe {
        ffi::bc_file_system_path_stem(
            component.as_ptr(),
            component.len(),
            &raw mut stem_length,
            &raw mut c_error,
        )
    };
    checked(status, &c_error)
        .map_err(|c_error| refused(&c_error, Operation::PathStem, component))?;
    Ok(stem_length)
}

/// Where the extension sits inside `component`, or `None` when it has none.
pub(crate) fn path_extension(component: &[u8]) -> Result<Option<Range<usize>>, Error> {
    let mut offset: usize = 0;
    let mut extension_length: usize = 0;
    let mut present: i32 = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `component` is readable for its length; `offset`, `extension_length` and `present` are written on
    // success, `c_error` on failure.
    let status = unsafe {
        ffi::bc_file_system_path_extension(
            component.as_ptr(),
            component.len(),
            &raw mut offset,
            &raw mut extension_length,
            &raw mut present,
            &raw mut c_error,
        )
    };
    checked(status, &c_error)
        .map_err(|c_error| refused(&c_error, Operation::PathExtension, component))?;
    Ok((present != 0).then_some(offset..offset.saturating_add(extension_length)))
}

/// Where `path` stands with respect to `other`, as the core's `bc_file_system_path_relation` value.
pub(crate) fn path_relate(path: &[u8], other: &[u8]) -> Result<i32, Error> {
    let mut relation: i32 = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `path` and `other` are readable for their lengths; `relation` is written on success, `c_error` on
    // failure.
    let status = unsafe {
        ffi::bc_file_system_path_relate(
            path.as_ptr(),
            path.len(),
            other.as_ptr(),
            other.len(),
            &raw mut relation,
            &raw mut c_error,
        )
    };
    checked(status, &c_error).map_err(|c_error| refused(&c_error, Operation::PathRelate, path))?;
    Ok(relation)
}

/// Loose text reduced to one spelling, by two calls.
pub(crate) fn path_lexically_normalize(text: &[u8]) -> Result<Vec<u8>, Error> {
    measured_answer(
        |buffer: &mut [u8], normalized: &mut usize, c_error: &mut ffi::bc_file_system_error| {
            // SAFETY: `text` is readable for its length; `buffer` is `buffer.len()` writable bytes;
            // `normalized` is written on success and on `BufferTooSmall`, `c_error` on failure.
            unsafe {
                ffi::bc_file_system_path_lexically_normalize(
                    text.as_ptr(),
                    text.len(),
                    buffer.as_mut_ptr(),
                    buffer.len(),
                    normalized,
                    c_error,
                )
            }
        },
    )
    .map_err(|c_error| refused(&c_error, Operation::PathLexicallyNormalize, text))
}

/// `relative` resolved against `base`, by two calls; a refusal names `relative`, the loose argument.
pub(crate) fn path_resolve_against(base: &[u8], relative: &[u8]) -> Result<Vec<u8>, Error> {
    measured_answer(
        |buffer: &mut [u8], length: &mut usize, c_error: &mut ffi::bc_file_system_error| {
            // SAFETY: `base` and `relative` are readable for their lengths; `buffer` is `buffer.len()` writable
            // bytes; `length` is written on success and on `BufferTooSmall`, `c_error` on failure.
            unsafe {
                ffi::bc_file_system_path_resolve_against(
                    base.as_ptr(),
                    base.len(),
                    relative.as_ptr(),
                    relative.len(),
                    buffer.as_mut_ptr(),
                    buffer.len(),
                    length,
                    c_error,
                )
            }
        },
    )
    .map_err(|c_error| refused(&c_error, Operation::PathResolveAgainst, relative))
}

/// The library kind `component` spells, as the core's `bc_file_system_library_kind` value, and where the
/// library's own name sits inside it (meaningful unless the kind is `none`).
pub(crate) fn path_library_name(component: &[u8]) -> Result<(i32, Range<usize>), Error> {
    let mut kind: i32 = 0;
    let mut stem_offset: usize = 0;
    let mut stem_length: usize = 0;
    let mut c_error = ffi::bc_file_system_error::zeroed();
    // SAFETY: `component` is readable for its length; `kind`, `stem_offset` and `stem_length` are written on
    // success (the last two only for a library), `c_error` on failure.
    let status = unsafe {
        ffi::bc_file_system_path_library_name(
            component.as_ptr(),
            component.len(),
            &raw mut kind,
            &raw mut stem_offset,
            &raw mut stem_length,
            &raw mut c_error,
        )
    };
    checked(status, &c_error)
        .map_err(|c_error| refused(&c_error, Operation::PathLibraryName, component))?;
    Ok((kind, stem_offset..stem_offset.saturating_add(stem_length)))
}

/// The observer's door, for `bc-file-system-testing` alone: the two factories that install the observer and the
/// three calls that script and read it, each a safe function over an [`Engine`]. Hidden from this crate's
/// documentation (a production consumer has no use for it) and named for tests, so the house-rules guard's
/// `test-only-factory` leg confines every caller in this repository to test code. A consumer's tests reach it
/// through `bc-file-system-testing`'s `ObservedEngine`.
pub mod testing_support {
    use super::{Engine, Error, Mutex, Operation, Path, Step, checked, create, ffi};

    /// The engine with the observer installed: every step is counted, and the faults scripted with
    /// [`add_fault`] take effect at the step they name. Every call into it takes its turn, so it may be shared
    /// between threads and is still used by one at a time, as the header requires.
    ///
    /// # Errors
    ///
    /// `ResourceExhausted` when the engine cannot be allocated.
    pub fn observed_for_tests() -> Result<Engine, Error> {
        let handle = create(
            ffi::bc_file_system_engine_create_observed_for_tests,
            Operation::EngineCreateObservedForTests,
        )?;
        Ok(Engine {
            handle,
            observed_turn: Some(Mutex::new(())),
        })
    }

    /// The observed engine with every fast path refused: every operation takes the path every platform has (the
    /// walk, the read-and-write loop, fsync), and one whose promise a fast path alone keeps (the no-replace rename
    /// of a `Refuse` and of a condemn) answers `Unsupported`, as a platform without that path does. Every call into
    /// it takes its turn, as on [`observed_for_tests`].
    ///
    /// # Errors
    ///
    /// `ResourceExhausted` when the engine cannot be allocated.
    pub fn portable_only_for_tests() -> Result<Engine, Error> {
        let handle = create(
            ffi::bc_file_system_engine_create_portable_only_for_tests,
            Operation::EngineCreatePortableOnlyForTests,
        )?;
        Ok(Engine {
            handle,
            observed_turn: Some(Mutex::new(())),
        })
    }

    /// Scripts one fault, copied by the core. The slot holds a bounded number.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for a live engine or a fault the core refuses (an occurrence of 0, a `fail` without
    /// its errno); `ResourceExhausted` when the slot is full.
    pub fn add_fault(engine: &Engine, fault: &ffi::bc_file_system_fault) -> Result<(), Error> {
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = engine.one_call();
        // SAFETY: the engine's handle is live while `engine` is; `fault` is a live borrow the core copies;
        // `c_error` is filled on failure.
        let status =
            unsafe { ffi::bc_file_system_observed_fault_add(call.handle, fault, &raw mut c_error) };
        checked(status, &c_error)
            .map_err(|c_error| Error::from_c(&c_error, Operation::ObservedFaultAdd, Path::new("")))
    }

    /// Forgets every scripted fault; the step counts are kept.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for a live engine.
    pub fn clear_faults(engine: &Engine) -> Result<(), Error> {
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = engine.one_call();
        // SAFETY: the engine's handle is live while `engine` is; `c_error` is filled on failure.
        let status =
            unsafe { ffi::bc_file_system_observed_faults_clear(call.handle, &raw mut c_error) };
        checked(status, &c_error).map_err(|c_error| {
            Error::from_c(&c_error, Operation::ObservedFaultsClear, Path::new(""))
        })
    }

    /// How many times `operation` has reached `step` in the engine's lifetime.
    ///
    /// # Errors
    ///
    /// `InvalidArgument` for a live engine.
    pub fn step_count(engine: &Engine, operation: Operation, step: Step) -> Result<u64, Error> {
        let mut count: u64 = 0;
        let mut c_error = ffi::bc_file_system_error::zeroed();
        let call = engine.one_call();
        // SAFETY: the engine's handle is live while `engine` is; `count` is written on success, `c_error` on
        // failure.
        let status = unsafe {
            ffi::bc_file_system_observed_step_count(
                call.handle,
                operation as i32,
                step as i32,
                &raw mut count,
                &raw mut c_error,
            )
        };
        checked(status, &c_error).map_err(|c_error| {
            Error::from_c(&c_error, Operation::ObservedStepCount, Path::new(""))
        })?;
        Ok(count)
    }
}
