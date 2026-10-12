//! The error vocabulary every face shares, as Rust types: the fifteen kinds, the operations, the steps and the
//! error value that carries them, converted once from the core's struct with the full path composed from the
//! location the face holds.
//!
//! Each enum is closed and mirrors its C enum value for value (the explicit discriminants are the header's
//! numbers); a new value on the C side is a version bump for every face, so there is no catch-all variant. A C
//! integer becomes a variant through `TryFrom<i32>`, which refuses a value outside the enum with an
//! [`UnknownValue`]; `transmute` is not a conversion.

use std::ffi::{OsStr, OsString};
use std::fmt;
use std::ops::Deref;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};

use crate::ffi;

/// Why a call failed: fourteen decisions a caller can act on, never a passthrough of errno, plus the grow-and-
/// retry signal of the C face. [`ErrorDetails::system_code`] keeps the exact errno for the log.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ErrorKind {
    /// Nothing exists at the location.
    NotFound = 1,
    /// Something exists where a regular file is required, and it is not one.
    NotRegularFile = 2,
    /// Something exists where a directory is required, and it is not one.
    NotDirectory = 3,
    /// `EACCES`, `EPERM` and `EROFS`: a read-only volume refuses the same way a mode does.
    PermissionDenied = 4,
    /// The destination holds something the call was told not to replace.
    AlreadyExists = 5,
    /// A directory that must be empty to be removed or replaced is not.
    DirectoryNotEmpty = 6,
    /// A symlink sat where the call was told not to follow one.
    SymlinkRefused = 7,
    /// The file is larger than a read limit, than `usize` on this target, or than the process's file-size limit.
    TooLarge = 8,
    /// `EXDEV`: the two locations of a rename are on different devices.
    CrossDevice = 9,
    /// `ENOSPC` and `EDQUOT`: the disk or the quota is full. Never memory.
    NoSpace = 10,
    /// `ENOMEM`, `EMFILE`, `ENFILE`: memory or descriptors ran out.
    ResourceExhausted = 11,
    /// The platform cannot keep the guarantee the call asks for, and the library refuses rather than weakens it.
    Unsupported = 12,
    /// A contract violation; nothing was touched.
    InvalidArgument = 13,
    /// Any other system failure; the system code says which.
    IoFailed = 14,
    /// The caller's buffer cannot hold the answer; the C face's grow-and-retry signal, which the typed face
    /// absorbs: a consumer sees it only for an answer that grew on every attempt of the bounded grow loop.
    BufferTooSmall = 15,
}

impl ErrorKind {
    /// Every kind, in the header's order.
    pub const ALL: [Self; 15] = [
        Self::NotFound,
        Self::NotRegularFile,
        Self::NotDirectory,
        Self::PermissionDenied,
        Self::AlreadyExists,
        Self::DirectoryNotEmpty,
        Self::SymlinkRefused,
        Self::TooLarge,
        Self::CrossDevice,
        Self::NoSpace,
        Self::ResourceExhausted,
        Self::Unsupported,
        Self::InvalidArgument,
        Self::IoFailed,
        Self::BufferTooSmall,
    ];

    /// The kind's name as the header spells it, for messages and logs.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::NotFound => "not_found",
            Self::NotRegularFile => "not_regular_file",
            Self::NotDirectory => "not_directory",
            Self::PermissionDenied => "permission_denied",
            Self::AlreadyExists => "already_exists",
            Self::DirectoryNotEmpty => "directory_not_empty",
            Self::SymlinkRefused => "symlink_refused",
            Self::TooLarge => "too_large",
            Self::CrossDevice => "cross_device",
            Self::NoSpace => "no_space",
            Self::ResourceExhausted => "resource_exhausted",
            Self::Unsupported => "unsupported",
            Self::InvalidArgument => "invalid_argument",
            Self::IoFailed => "io_failed",
            Self::BufferTooSmall => "buffer_too_small",
        }
    }
}

/// Which public function an error came from, in the header's order. 28 and 29 are the path module's two
/// validators, named for the face constructors that call them; 30 onward are the rest of the path module.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[expect(
    missing_docs,
    reason = "each variant is the public function of the same name; the header documents it"
)]
pub enum Operation {
    EngineCreateLive = 1,
    EngineCreateObservedForTests = 2,
    OpenDirectory = 3,
    DirectoryOpenedAs = 4,
    ObservedFaultAdd = 5,
    ObservedFaultsClear = 6,
    ObservedStepCount = 7,
    Status = 8,
    ReadFile = 9,
    WriteFile = 10,
    Append = 11,
    Sync = 12,
    CreateDirectory = 13,
    CreateDirectories = 14,
    ListDirectory = 15,
    RemoveFile = 16,
    RemoveDirectory = 17,
    RemoveTree = 18,
    CreateTemporaryDirectory = 19,
    Rename = 20,
    CopyFile = 21,
    SetPermissions = 22,
    CreateSymlink = 23,
    ReadSymlink = 24,
    TryLock = 25,
    Condemn = 26,
    RemoveCondemned = 27,
    RelativePathFrom = 28,
    AbsolutePathFrom = 29,
    PathJoin = 30,
    PathParent = 31,
    PathLastComponent = 32,
    PathNextComponent = 33,
    PathStem = 34,
    PathExtension = 35,
    PathRelate = 36,
    PathLexicallyNormalize = 37,
    PathResolveAgainst = 38,
    PathLibraryName = 39,
    EngineCreatePortableOnlyForTests = 40,
}

impl Operation {
    /// Every operation, in the header's order.
    pub const ALL: [Self; 40] = [
        Self::EngineCreateLive,
        Self::EngineCreateObservedForTests,
        Self::OpenDirectory,
        Self::DirectoryOpenedAs,
        Self::ObservedFaultAdd,
        Self::ObservedFaultsClear,
        Self::ObservedStepCount,
        Self::Status,
        Self::ReadFile,
        Self::WriteFile,
        Self::Append,
        Self::Sync,
        Self::CreateDirectory,
        Self::CreateDirectories,
        Self::ListDirectory,
        Self::RemoveFile,
        Self::RemoveDirectory,
        Self::RemoveTree,
        Self::CreateTemporaryDirectory,
        Self::Rename,
        Self::CopyFile,
        Self::SetPermissions,
        Self::CreateSymlink,
        Self::ReadSymlink,
        Self::TryLock,
        Self::Condemn,
        Self::RemoveCondemned,
        Self::RelativePathFrom,
        Self::AbsolutePathFrom,
        Self::PathJoin,
        Self::PathParent,
        Self::PathLastComponent,
        Self::PathNextComponent,
        Self::PathStem,
        Self::PathExtension,
        Self::PathRelate,
        Self::PathLexicallyNormalize,
        Self::PathResolveAgainst,
        Self::PathLibraryName,
        Self::EngineCreatePortableOnlyForTests,
    ];

    /// The operation's name as the header spells it.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::EngineCreateLive => "engine_create_live",
            Self::EngineCreateObservedForTests => "engine_create_observed_for_tests",
            Self::OpenDirectory => "open_directory",
            Self::DirectoryOpenedAs => "directory_opened_as",
            Self::ObservedFaultAdd => "observed_fault_add",
            Self::ObservedFaultsClear => "observed_faults_clear",
            Self::ObservedStepCount => "observed_step_count",
            Self::Status => "status",
            Self::ReadFile => "read_file",
            Self::WriteFile => "write_file",
            Self::Append => "append",
            Self::Sync => "sync",
            Self::CreateDirectory => "create_directory",
            Self::CreateDirectories => "create_directories",
            Self::ListDirectory => "list_directory",
            Self::RemoveFile => "remove_file",
            Self::RemoveDirectory => "remove_directory",
            Self::RemoveTree => "remove_tree",
            Self::CreateTemporaryDirectory => "create_temporary_directory",
            Self::Rename => "rename",
            Self::CopyFile => "copy_file",
            Self::SetPermissions => "set_permissions",
            Self::CreateSymlink => "create_symlink",
            Self::ReadSymlink => "read_symlink",
            Self::TryLock => "try_lock",
            Self::Condemn => "condemn",
            Self::RemoveCondemned => "remove_condemned",
            Self::RelativePathFrom => "relative_path_from",
            Self::AbsolutePathFrom => "absolute_path_from",
            Self::PathJoin => "path_join",
            Self::PathParent => "path_parent",
            Self::PathLastComponent => "path_last_component",
            Self::PathNextComponent => "path_next_component",
            Self::PathStem => "path_stem",
            Self::PathExtension => "path_extension",
            Self::PathRelate => "path_relate",
            Self::PathLexicallyNormalize => "path_lexically_normalize",
            Self::PathResolveAgainst => "path_resolve_against",
            Self::PathLibraryName => "path_library_name",
            Self::EngineCreatePortableOnlyForTests => "engine_create_portable_only_for_tests",
        }
    }
}

/// The system call an operation was making when it failed, and the observer's vocabulary: one variant per
/// system call the engine makes.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[expect(
    missing_docs,
    reason = "each variant is the system call of the same name; the header documents it"
)]
pub enum Step {
    Open = 1,
    Openat = 2,
    Fstat = 3,
    Fstatat = 4,
    Read = 5,
    Pread = 6,
    Write = 7,
    Fsync = 8,
    /// `fcntl(F_FULLFSYNC)`: the durable sync on Apple platforms, where `fsync` stops at the drive's cache.
    FcntlFullfsync = 9,
    Renameat = 10,
    /// The platform's no-replace rename: `renameatx_np(RENAME_EXCL)`, `renameat2(RENAME_NOREPLACE)`.
    RenameNoReplace = 11,
    Unlinkat = 12,
    Mkdirat = 13,
    Fdopendir = 14,
    Readdir = 15,
    Symlinkat = 16,
    Readlinkat = 17,
    Flock = 18,
    Fchmod = 19,
    /// `copy_file_range`, or the read-and-write loop where the kernel offers no copy.
    CopyRange = 20,
    /// `clonefileat`, `fclonefileat`, `FICLONE`.
    CloneFile = 21,
    Close = 22,
    /// The `st_dev`/`st_ino` comparison that catches a directory swapped in between a stat and an open.
    IdentityCheck = 23,
    /// The Linux fast path of a `beneath` location: `openat2` over the whole relative path in one call. A
    /// scripted `ENOSYS`, `EINVAL`, `ENOTSUP` or `EOPNOTSUPP` on this step makes the engine walk the path instead.
    Openat2 = 24,
    /// The Apple fast path of a `beneath` location: `openat` with `O_NOFOLLOW_ANY` over the whole relative path.
    OpenatNofollowAny = 25,
    /// `fchmodat`: the mode set by name under a held directory (`AT_SYMLINK_NOFOLLOW` for the entry itself).
    Fchmodat = 26,
    /// `faccessat(X_OK)`: the search-permission check after a search-only open that checked nothing (Linux's `O_PATH`).
    Faccessat = 27,
}

impl Step {
    /// Every step, in the header's order.
    pub const ALL: [Self; 27] = [
        Self::Open,
        Self::Openat,
        Self::Fstat,
        Self::Fstatat,
        Self::Read,
        Self::Pread,
        Self::Write,
        Self::Fsync,
        Self::FcntlFullfsync,
        Self::Renameat,
        Self::RenameNoReplace,
        Self::Unlinkat,
        Self::Mkdirat,
        Self::Fdopendir,
        Self::Readdir,
        Self::Symlinkat,
        Self::Readlinkat,
        Self::Flock,
        Self::Fchmod,
        Self::CopyRange,
        Self::CloneFile,
        Self::Close,
        Self::IdentityCheck,
        Self::Openat2,
        Self::OpenatNofollowAny,
        Self::Fchmodat,
        Self::Faccessat,
    ];

    /// The step's name as the header spells it.
    #[must_use]
    pub const fn name(self) -> &'static str {
        match self {
            Self::Open => "open",
            Self::Openat => "openat",
            Self::Fstat => "fstat",
            Self::Fstatat => "fstatat",
            Self::Read => "read",
            Self::Pread => "pread",
            Self::Write => "write",
            Self::Fsync => "fsync",
            Self::FcntlFullfsync => "fcntl_fullfsync",
            Self::Renameat => "renameat",
            Self::RenameNoReplace => "rename_no_replace",
            Self::Unlinkat => "unlinkat",
            Self::Mkdirat => "mkdirat",
            Self::Fdopendir => "fdopendir",
            Self::Readdir => "readdir",
            Self::Symlinkat => "symlinkat",
            Self::Readlinkat => "readlinkat",
            Self::Flock => "flock",
            Self::Fchmod => "fchmod",
            Self::CopyRange => "copy_range",
            Self::CloneFile => "clone_file",
            Self::Close => "close",
            Self::IdentityCheck => "identity_check",
            Self::Openat2 => "openat2",
            Self::OpenatNofollowAny => "openat_nofollow_any",
            Self::Fchmodat => "fchmodat",
            Self::Faccessat => "faccessat",
        }
    }
}

/// Which of the two locations of `rename` and `copy_file` an error belongs to: the source is the first, the
/// destination the second. The discriminants are the header's; 0 (an operation over one location) is `None` on
/// the typed error, not a variant.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FailedLocation {
    /// The first location: the source.
    First = 1,
    /// The second location: the destination.
    Second = 2,
}

impl FailedLocation {
    /// Both locations, in the header's order.
    pub const ALL: [Self; 2] = [Self::First, Self::Second];
}

/// An integer the core answered that is outside the enum, or the range, this face knows. It cannot happen for
/// a core and a face built from one checkout, which this crate's build script guarantees; it is reported rather
/// than guessed at because a guess would be a silent default.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct UnknownValue {
    /// The enum (or the field) the value was meant for, as the header names it.
    pub enumeration: &'static str,
    /// The integer as received.
    pub value: i32,
}

impl fmt::Display for UnknownValue {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{} is no {}", self.value, self.enumeration)
    }
}

impl std::error::Error for UnknownValue {}

/// The one shape of every conversion from a C integer: find the variant whose integer is the value, else refuse
/// it by name.
pub(crate) fn variant_of<T: Copy + Into<i32>>(
    all: &[T],
    value: i32,
    enumeration: &'static str,
) -> Result<T, UnknownValue> {
    all.iter()
        .copied()
        .find(|variant| (*variant).into() == value)
        .ok_or(UnknownValue { enumeration, value })
}

impl From<ErrorKind> for i32 {
    fn from(kind: ErrorKind) -> Self {
        kind as Self
    }
}

impl From<Operation> for i32 {
    fn from(operation: Operation) -> Self {
        operation as Self
    }
}

impl From<Step> for i32 {
    fn from(step: Step) -> Self {
        step as Self
    }
}

impl From<FailedLocation> for i32 {
    fn from(location: FailedLocation) -> Self {
        location as Self
    }
}

impl TryFrom<i32> for ErrorKind {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(&Self::ALL, value, "bc_file_system_error_kind")
    }
}

impl TryFrom<i32> for Operation {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(&Self::ALL, value, "bc_file_system_operation")
    }
}

impl TryFrom<i32> for Step {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(&Self::ALL, value, "bc_file_system_step")
    }
}

impl TryFrom<i32> for FailedLocation {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(&Self::ALL, value, "bc_file_system_error.failed_location")
    }
}

/// What one failure was, as the core reported it: the kind to act on, the operation and the step that failed,
/// the errno for the log, and the full path the step acted on. [`Error`] holds it behind one pointer; its fields
/// read through the error (`error.kind`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ErrorDetails {
    /// The decision a caller can act on.
    pub kind: ErrorKind,
    /// The public function that failed.
    pub operation: Operation,
    /// The system call that failed, or `None` for a refusal made before any system call.
    pub step: Option<Step>,
    /// The errno, copied on the line after the failing call, or 0.
    pub system_code: i32,
    /// The full path the failing step acted on: the location's text (for a location beneath a directory, the
    /// directory's opened-as text, a slash and the relative path), then a slash and the entry's name when the
    /// step acted on an entry inside it; the name alone for a call that took no location. For reporting, never
    /// for a retry.
    pub path: PathBuf,
    /// Whether the entry's name was cut short by the core's fixed field, so `path` ends early.
    pub name_truncated: bool,
    /// Which end of a two-location operation (`rename`, `copy_file`) the failure belongs to, and so which
    /// location `path` was composed from; `None` for an operation over one location and for a refusal of the
    /// arguments, which blames neither.
    pub failed_location: Option<FailedLocation>,
    /// Set when the core answered an integer outside an enum this face knows: the field it stands in for was
    /// filled with a documented stand-in (`InvalidArgument` for the kind, the operation that was called for the
    /// operation, `None` for the step and the failed location) and the raw value is kept here so the mismatch
    /// is visible, never silent.
    pub unknown_value: Option<UnknownValue>,
}

/// Why a call failed. One pointer wide: every operation's `Result` pays for the size of its error, so the
/// fields live behind a `Box` and a `Result<(), Error>` is as small as a pointer (the size is pinned below).
/// The fields read as if they were the error's own (`error.kind`, `error.path`), through `Deref`; the text a
/// consumer reads is [`fmt::Display`]'s, rendered here with the host's own errno text, since the core never
/// formats.
#[derive(Clone, PartialEq, Eq)]
pub struct Error {
    details: Box<ErrorDetails>,
}

// The measured decision (AGENTS.md, "Rust"): unboxed, the fields made every `Result<(), Error>` 72 bytes wide on a
// 64-bit target; boxed, it is one pointer. A field inlined back into `Error` fails this line, which no test would
// notice.
const _: () = assert!(size_of::<Error>() == size_of::<usize>());

impl From<ErrorDetails> for Error {
    fn from(details: ErrorDetails) -> Self {
        Self {
            details: Box::new(details),
        }
    }
}

impl Deref for Error {
    type Target = ErrorDetails;

    fn deref(&self) -> &ErrorDetails {
        &self.details
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.details.fmt(formatter)
    }
}

impl Error {
    /// The core's error struct as the typed value, converted once at the boundary, with the path composed from
    /// `location_text` (empty for a call that took no location) and the entry name the core reported. `called`
    /// is the operation the face invoked, the stand-in should the core's `operation` be outside the enum (it
    /// never is for a core and a face from one checkout; the face still never substitutes its own value
    /// silently, see [`ErrorDetails::unknown_value`]).
    #[must_use]
    pub fn from_c(
        c_error: &ffi::bc_file_system_error,
        called: Operation,
        location_text: &Path,
    ) -> Self {
        let mut unknown_value = None;
        let kind = ErrorKind::try_from(c_error.kind).unwrap_or_else(|unknown| {
            unknown_value = Some(unknown);
            ErrorKind::InvalidArgument
        });
        let operation = Operation::try_from(c_error.operation).unwrap_or_else(|unknown| {
            unknown_value.get_or_insert(unknown);
            called
        });
        let step = if c_error.step == 0 {
            None
        } else {
            Step::try_from(c_error.step).map_or_else(
                |unknown| {
                    unknown_value.get_or_insert(unknown);
                    None
                },
                Some,
            )
        };
        let failed_location = if c_error.failed_location == 0 {
            None
        } else {
            FailedLocation::try_from(c_error.failed_location).map_or_else(
                |unknown| {
                    unknown_value.get_or_insert(unknown);
                    None
                },
                Some,
            )
        };
        Self::from(ErrorDetails {
            kind,
            operation,
            step,
            system_code: c_error.system_code,
            path: composed_path(location_text, c_error),
            name_truncated: c_error.name_truncated != 0,
            failed_location,
            unknown_value,
        })
    }

    /// The same conversion for an operation over two locations (`rename`, `copy_file`): the path is composed
    /// from the location the core's `failed_location` names, the first (the source) or the second (the
    /// destination), and from the second when it names neither, which only a refusal of the arguments does.
    #[must_use]
    pub fn from_c_two_locations(
        c_error: &ffi::bc_file_system_error,
        called: Operation,
        first_location_text: &Path,
        second_location_text: &Path,
    ) -> Self {
        let first_failed = c_error.failed_location == i32::from(FailedLocation::First);
        let location_text = if first_failed {
            first_location_text
        } else {
            second_location_text
        };
        Self::from_c(c_error, called, location_text)
    }

    /// An answer of the core's that is outside the enum (or the range) this face knows, reported as an error
    /// under the stand-ins `from_c` uses, with the raw value kept.
    pub(crate) fn answer_outside_enum(
        called: Operation,
        unknown: UnknownValue,
        location_text: &Path,
    ) -> Self {
        Self::from(ErrorDetails {
            kind: ErrorKind::InvalidArgument,
            operation: called,
            step: None,
            system_code: 0,
            path: location_text.to_path_buf(),
            name_truncated: false,
            failed_location: None,
            unknown_value: Some(unknown),
        })
    }
}

/// The location's text, then a slash and the entry's name when the core named one; the name alone when there
/// is no location. The core bounds `name_length` by the field; the minimum keeps the face honest should it ever
/// not.
fn composed_path(location_text: &Path, c_error: &ffi::bc_file_system_error) -> PathBuf {
    let name_length = usize::try_from(c_error.name_length)
        .unwrap_or(usize::MAX)
        .min(c_error.name.len());
    let name = &c_error.name[..name_length];
    let mut path = location_text.as_os_str().as_bytes().to_vec();
    if !name.is_empty() {
        if !path.is_empty() {
            path.push(b'/');
        }
        path.extend_from_slice(name);
    }
    PathBuf::from(OsString::from_vec(path))
}

impl fmt::Display for Error {
    /// `<operation>: <path>: <kind>`, then ` at <step>` when a system call failed, then the host's text for the
    /// errno (`std::io::Error`'s rendering, which is thread-safe where `strerror` is not), then which end of a
    /// two-location operation failed, then the unknown value when there was one. The path segment is left out
    /// when the call took no location, and ends in `…` when the core cut the entry's name short.
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}: ", self.operation.name())?;
        if !self.path.as_os_str().is_empty() {
            let suffix = if self.name_truncated { "…" } else { "" };
            write!(formatter, "{}{suffix}: ", self.path.display())?;
        }
        write!(formatter, "{}", self.kind.name())?;
        if let Some(step) = self.step {
            write!(formatter, " at {}", step.name())?;
        }
        if self.system_code != 0 {
            write!(
                formatter,
                ": {}",
                std::io::Error::from_raw_os_error(self.system_code)
            )?;
        }
        match self.failed_location {
            Some(FailedLocation::First) => write!(formatter, " (at the source)")?,
            Some(FailedLocation::Second) => write!(formatter, " (at the destination)")?,
            None => {}
        }
        if let Some(unknown) = self.unknown_value {
            write!(formatter, " [the core answered {unknown}]")?;
        }
        Ok(())
    }
}

impl std::error::Error for Error {}

/// Bytes as a path, without a copy: on the Unix family every path is bytes.
pub(crate) fn path_of_bytes(bytes: &[u8]) -> &Path {
    Path::new(OsStr::from_bytes(bytes))
}
