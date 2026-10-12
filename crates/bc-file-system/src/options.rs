//! The required choices: every operation that can follow a final symlink, replace a destination, bound a read,
//! sync or set a mode is told which, at every call. None of these types has a `Default`: a choice nobody stated
//! is the defect the library exists to remove, and a field added to an options struct later fails to compile at
//! every place that builds one, which is the point.

use crate::error::{UnknownValue, variant_of};
use crate::ffi;

/// What to do when the last component of a path is a symlink.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FinalSymlink {
    /// The symlink itself is the entry: it is reported, removed or refused, never followed.
    Refuse = 1,
    /// The operation acts on what the symlink points at.
    Follow = 2,
}

/// What to do when a mutation's destination already holds something.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ExistingDestination {
    /// The destination is replaced: a file by a file, an empty directory by a directory.
    Replace = 1,
    /// `AlreadyExists`, and the destination is untouched.
    Refuse = 2,
}

/// How far a change must travel before the call returns, and how far it got. A call is told the durability it
/// needs and answers with the one it achieved: once a change is visible, a sync that fails lowers the answer to
/// `Written` rather than raising an error, because the change happened.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Durability {
    /// Handed to the operating system; outlives the process; a power loss can still lose it.
    Written = 1,
    /// On stable storage: the file and its directory synced (`F_FULLFSYNC` on Apple platforms, `fsync`
    /// elsewhere).
    Durable = 2,
}

impl Durability {
    /// Both durabilities, in the header's order.
    pub const ALL: [Self; 2] = [Self::Written, Self::Durable];
}

impl From<Durability> for i32 {
    fn from(durability: Durability) -> Self {
        durability as Self
    }
}

impl TryFrom<i32> for Durability {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(&Self::ALL, value, "bc_file_system_durability")
    }
}

/// How much of a file a read may take.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ReadLimit {
    /// Every byte, however many.
    WholeFile,
    /// At most this many bytes; a larger file is `TooLarge`, never a silent prefix.
    AtMost(u64),
}

impl ReadLimit {
    /// The limit as the core takes it.
    #[must_use]
    pub const fn to_c(self) -> ffi::bc_file_system_read_limit {
        match self {
            Self::WholeFile => ffi::bc_file_system_read_limit {
                kind: ffi::READ_LIMIT_KIND_WHOLE_FILE,
                at_most: 0,
            },
            Self::AtMost(bytes) => ffi::bc_file_system_read_limit {
                kind: ffi::READ_LIMIT_KIND_AT_MOST,
                at_most: bytes,
            },
        }
    }
}

/// What the mode of [`Permissions`] means for a file that replaces an existing one.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PermissionsPolicy {
    /// A replacing file gets the mode too.
    Exactly = 1,
    /// A replacing file keeps the mode the replaced file had, so an operator's chmod survives a rewrite.
    PreserveOr = 2,
}

/// The mode of anything an operation creates or replaces. There is no default mode: a caller builds one with
/// the named constructor of its policy.
///
/// The mode is the permission bits as chmod takes them (`0o644`, `0o600`, `0o755`); bits outside `0o7777` make
/// the operation fail with `InvalidArgument`. The mode is applied exactly: the umask is ambient state the
/// library never consults. What already exists and is not replaced is never touched.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Permissions {
    policy: PermissionsPolicy,
    mode: u32,
}

impl Permissions {
    /// A created file or directory gets `mode`, and so does a file that replaces an existing one.
    #[must_use]
    pub const fn exactly(mode: u32) -> Self {
        Self {
            policy: PermissionsPolicy::Exactly,
            mode,
        }
    }

    /// A file that replaces an existing one keeps the mode the replaced file had; anything newly created gets
    /// `mode`.
    #[must_use]
    pub const fn preserve_or(mode: u32) -> Self {
        Self {
            policy: PermissionsPolicy::PreserveOr,
            mode,
        }
    }

    /// The policy this value was built with.
    #[must_use]
    pub const fn policy(self) -> PermissionsPolicy {
        self.policy
    }

    /// The permission bits.
    #[must_use]
    pub const fn mode(self) -> u32 {
        self.mode
    }

    /// The permissions as the core takes them.
    #[must_use]
    pub const fn to_c(self) -> ffi::bc_file_system_permissions {
        ffi::bc_file_system_permissions {
            policy: self.policy as i32,
            mode: self.mode,
        }
    }
}

/// Every choice a write, a copy or a replace makes, each one required. Built once as a named value at each
/// composition root.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct WriteOptions {
    /// How far the write must travel before the call returns.
    pub durability: Durability,
    /// The mode of what is created or replaced.
    pub permissions: Permissions,
    /// Whether an existing destination is replaced or refused.
    pub existing_destination: ExistingDestination,
    /// Whether a final symlink at the destination is followed or refused.
    pub final_symlink: FinalSymlink,
}

impl WriteOptions {
    /// The options as the core takes them.
    #[must_use]
    pub const fn to_c(&self) -> ffi::bc_file_system_write_options {
        ffi::bc_file_system_write_options {
            durability: self.durability as i32,
            permissions: self.permissions.to_c(),
            existing_destination: self.existing_destination as i32,
            final_symlink: self.final_symlink as i32,
        }
    }
}

/// Every choice a read makes, each one required.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ReadOptions {
    /// How much of the file the read may take.
    pub limit: ReadLimit,
    /// Whether a final symlink is followed or refused.
    pub final_symlink: FinalSymlink,
}

impl ReadOptions {
    /// The options as the core takes them.
    #[must_use]
    pub const fn to_c(&self) -> ffi::bc_file_system_read_options {
        ffi::bc_file_system_read_options {
            limit: self.limit.to_c(),
            final_symlink: self.final_symlink as i32,
        }
    }
}
