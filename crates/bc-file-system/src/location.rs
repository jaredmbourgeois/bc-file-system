//! Where an operation acts: a [`Location`], one sum type so every operation has one signature, and the
//! [`Directory`] a location beneath borrows.

use core::marker::PhantomData;
use core::ptr::NonNull;
use std::ffi::OsString;
use std::os::unix::ffi::OsStringExt;
use std::path::PathBuf;

use crate::ffi;
use crate::live::Engine;
use crate::path::{AbsolutePath, RelativePath};

/// An open directory the kernel enforces containment beneath, which remembers the text it was opened with so
/// that an error beneath it reads as that text, a slash and the relative path. Opened only by
/// [`Engine::open_directory`]; it borrows the engine, which the header requires to outlive every directory opened
/// through it; closed when dropped. Used from one thread at a time, as the header says: it may move to another
/// thread (`Send`) but not be shared between two (`!Sync`).
#[derive(Debug)]
pub struct Directory<'engine> {
    /// The core's handle; released by the `Drop` in the live module, which holds every call into the core.
    pub(crate) handle: NonNull<ffi::bc_file_system_directory>,
    /// The text this face composed for the location it opened, the same bytes the core remembers, kept so that
    /// composing an error's path costs no call.
    pub(crate) opened_as_text: Vec<u8>,
    pub(crate) engine: PhantomData<&'engine Engine>,
}

/// Where an operation acts: an absolute path the system resolves, or a relative path beneath a [`Directory`]
/// with containment enforced by the kernel. A location borrows everything it names, so it is made at the call
/// and lives no longer than the directory and the path.
#[derive(Clone, Copy, Debug)]
pub enum Location<'a> {
    /// An absolute path the system resolves, following symlinks on the way.
    At(&'a AbsolutePath),
    /// A relative path resolved beneath an open directory: no component may be a symlink, and nothing above the
    /// directory is reachable.
    Beneath(&'a Directory<'a>, &'a RelativePath),
}

impl Location<'_> {
    /// The C struct, borrowing this location's bytes and directory: valid only while what the location borrows
    /// lives, which the borrow checker holds for the call that takes it.
    #[must_use]
    pub fn to_c(&self) -> ffi::bc_file_system_location {
        match *self {
            Self::At(path) => {
                let bytes = path.as_bytes();
                ffi::bc_file_system_location_at(bytes.as_ptr(), bytes.len())
            }
            Self::Beneath(directory, relative) => {
                let bytes = relative.as_bytes();
                ffi::bc_file_system_location_beneath(
                    directory.handle.as_ptr().cast_const(),
                    bytes.as_ptr(),
                    bytes.len(),
                )
            }
        }
    }

    /// The text this location stands for, as an error reports it: the absolute path, or the directory's
    /// opened-as text, a slash and the relative path.
    #[must_use]
    pub fn text(&self) -> PathBuf {
        match *self {
            Self::At(path) => path.as_path().to_path_buf(),
            Self::Beneath(directory, relative) => {
                let mut text = directory.opened_as_text.clone();
                text.push(b'/');
                text.extend_from_slice(relative.as_bytes());
                PathBuf::from(OsString::from_vec(text))
            }
        }
    }
}
