//! The C ABI of `<bc_file_system/bc_file_system.h>`, spelled by hand: the opaque handles, the `#[repr(C)]`
//! mirrors of every struct that crosses, every function the header declares, and the layout pins.
//!
//! Hand-written rather than generated because the header is a common subset written for three importers, and a
//! few dozen declarations are cheaper to mirror than a generator pin is to keep honest. The names are the
//! header's, so a reader holds the two side by side. Each struct is pinned by its size and the offset of its
//! last field, as literals, against the numbers the core asserts in `core/src/bc_file_system.c`: a field added
//! on either side fails this crate's build. The contract of every pointer is the header's comment at that
//! parameter; the live module restates it at each call.
#![expect(
    unsafe_code,
    reason = "the extern \"C\" block that spells the ABI; the one declaration site of every C function"
)]

use core::marker::{PhantomData, PhantomPinned};
use core::mem::offset_of;

/// The context every operation runs in: created by one of the two factories, released once. Opaque: only the
/// core knows its shape.
#[repr(C)]
#[derive(Debug)]
pub struct bc_file_system_engine {
    _opaque: [u8; 0],
    _unsend: PhantomData<(*mut u8, PhantomPinned)>,
}

/// An open directory that remembers the text it was opened with. Opaque.
#[repr(C)]
#[derive(Debug)]
pub struct bc_file_system_directory {
    _opaque: [u8; 0],
    _unsend: PhantomData<(*mut u8, PhantomPinned)>,
}

/// An advisory lock held on a file. Opaque.
#[repr(C)]
#[derive(Debug)]
pub struct bc_file_system_lock {
    _opaque: [u8; 0],
    _unsend: PhantomData<(*mut u8, PhantomPinned)>,
}

/// Why a call failed: enums and integers only, caller-allocated, zeroed on success, filled on failure.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_error {
    /// A `bc_file_system_error_kind`; 0 is success.
    pub kind: i32,
    /// The `bc_file_system_operation` that failed.
    pub operation: i32,
    /// The `bc_file_system_step` that failed, or 0 for a refusal before any system call.
    pub step: i32,
    /// The errno, copied on the line after the failing call, or 0.
    pub system_code: i32,
    /// How many bytes of `name` are meaningful.
    pub name_length: u32,
    /// 1 when the entry's name did not fit in `name`.
    pub name_truncated: u8,
    /// The entry the step acted on, relative to the location; empty when it was the location itself.
    pub name: [u8; 256],
    /// Which location of an operation over two (`rename`, `copy_file`) the failure belongs to: 1 the first (the
    /// source), 2 the second (the destination); 0 for an operation over one location, and for a contract
    /// violation, which blames neither.
    pub failed_location: i32,
}

impl bc_file_system_error {
    /// The struct as the core expects it before a call: every field zero, the shape of success.
    #[must_use]
    pub const fn zeroed() -> Self {
        Self {
            kind: 0,
            operation: 0,
            step: 0,
            system_code: 0,
            name_length: 0,
            name_truncated: 0,
            name: [0; 256],
            failed_location: 0,
        }
    }
}

/// Where an operation acts. Built with [`bc_file_system_location_at`] or [`bc_file_system_location_beneath`].
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_location {
    /// A `bc_file_system_location_kind`.
    pub kind: i32,
    /// The directory for `beneath`; null for `at`.
    pub directory: *const bc_file_system_directory,
    /// `length` bytes, no NUL inside; absolute for `at`, relative for `beneath`; borrowed for the call.
    pub path: *const u8,
    /// How many bytes `path` points at.
    pub length: usize,
}

/// How much of a file a read may take.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_read_limit {
    /// A `bc_file_system_read_limit_kind`.
    pub kind: i32,
    /// The bound under `at_most`; ignored under `whole_file`.
    pub at_most: u64,
}

/// The mode of anything a call creates or replaces, and what it means for a replaced file.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_permissions {
    /// A `bc_file_system_permissions_policy`.
    pub policy: i32,
    /// The permission bits as chmod takes them (0644); bits outside 07777 are `invalid_argument`.
    pub mode: u32,
}

/// Every choice a write makes, each one required: a zero field is `invalid_argument`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_write_options {
    /// A `bc_file_system_durability`.
    pub durability: i32,
    /// The mode and its policy.
    pub permissions: bc_file_system_permissions,
    /// A `bc_file_system_existing_destination`.
    pub existing_destination: i32,
    /// A `bc_file_system_final_symlink`.
    pub final_symlink: i32,
}

/// Every choice a read makes, each one required.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_read_options {
    /// The read limit.
    pub limit: bc_file_system_read_limit,
    /// A `bc_file_system_final_symlink`.
    pub final_symlink: i32,
}

/// What the file system records about one entry.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_status {
    /// A `bc_file_system_entry_type`.
    pub entry_type: i32,
    /// A regular file's size in bytes; 0 for every other type.
    pub size: i64,
    /// The permission bits (0644), without the file-type bits.
    pub mode: u32,
    /// `st_dev`: with `inode`, the entry's identity.
    pub device: u64,
    /// `st_ino`.
    pub inode: u64,
    /// `st_nlink`.
    pub link_count: u64,
}

impl bc_file_system_status {
    /// The struct before a call: every field zero, which no successful answer leaves (an entry type is never 0).
    #[must_use]
    pub const fn zeroed() -> Self {
        Self {
            entry_type: 0,
            size: 0,
            mode: 0,
            device: 0,
            inode: 0,
            link_count: 0,
        }
    }
}

/// One entry of a directory listing.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_entry {
    /// A `bc_file_system_entry_type`.
    pub entry_type: i32,
    /// How many bytes of `name` are meaningful.
    pub name_length: u32,
    /// The entry's name, one component, not NUL-terminated.
    pub name: [u8; 256],
}

impl bc_file_system_entry {
    /// The struct before a call: every field zero, which no stored entry leaves (an entry type is never 0).
    #[must_use]
    pub const fn zeroed() -> Self {
        Self {
            entry_type: 0,
            name_length: 0,
            name: [0; 256],
        }
    }
}

/// One scripted fault for an engine created for tests: the `occurrence`-th time `operation` reaches `step`,
/// the step fails with `system_code` (`fail`) or the operation returns from there without cleanup (`stop`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct bc_file_system_fault {
    /// A `bc_file_system_operation`.
    pub operation: i32,
    /// A `bc_file_system_step`.
    pub step: i32,
    /// Counted from 1 across the engine's lifetime; 0 is `invalid_argument`.
    pub occurrence: u32,
    /// A `bc_file_system_fault_action`.
    pub action: i32,
    /// The errno to answer under `fail` (at least 1); must be 0 under `stop`.
    pub system_code: i32,
}

/// The `at` location kind, as the header numbers it.
pub const LOCATION_KIND_AT: i32 = 1;
/// The `beneath` location kind.
pub const LOCATION_KIND_BENEATH: i32 = 2;

// The values of the header's enums that the typed face reads or writes without a `#[repr(i32)]` enum of its own:
// a read limit and a fault carry data beside their kind, a lock outcome carries the lock, and the library kind
// `none` is an absent answer rather than a kind.

/// `bc_file_system_read_limit_kind_whole_file`.
pub const READ_LIMIT_KIND_WHOLE_FILE: i32 = 1;
/// `bc_file_system_read_limit_kind_at_most`.
pub const READ_LIMIT_KIND_AT_MOST: i32 = 2;
/// `bc_file_system_lock_outcome_acquired`.
pub const LOCK_OUTCOME_ACQUIRED: i32 = 1;
/// `bc_file_system_lock_outcome_held_elsewhere`.
pub const LOCK_OUTCOME_HELD_ELSEWHERE: i32 = 2;
/// `bc_file_system_fault_action_fail`.
pub const FAULT_ACTION_FAIL: i32 = 1;
/// `bc_file_system_fault_action_stop`.
pub const FAULT_ACTION_STOP: i32 = 2;
/// `bc_file_system_library_kind_none`: the component spells no library file.
pub const LIBRARY_KIND_NONE: i32 = 1;

/// The header's `static inline` constructor of an `at` location, respelled: an absolute path the system
/// resolves. `path` is `length` bytes, borrowed for the call that takes the location.
#[must_use]
pub const fn bc_file_system_location_at(path: *const u8, length: usize) -> bc_file_system_location {
    bc_file_system_location {
        kind: LOCATION_KIND_AT,
        directory: core::ptr::null(),
        path,
        length,
    }
}

/// The header's `static inline` constructor of a `beneath` location, respelled: a relative path resolved
/// beneath `directory`, which must outlive the location.
#[must_use]
pub const fn bc_file_system_location_beneath(
    directory: *const bc_file_system_directory,
    path: *const u8,
    length: usize,
) -> bc_file_system_location {
    bc_file_system_location {
        kind: LOCATION_KIND_BENEATH,
        directory,
        path,
        length,
    }
}

// The layout pins: the same literals the core asserts, so a field added on either side fails this build. The
// location holds two pointers, so its numbers depend on the pointer width, and both shipped widths are pinned.
const _: () = assert!(size_of::<bc_file_system_error>() == 284);
const _: () = assert!(align_of::<bc_file_system_error>() == 4);
const _: () = assert!(offset_of!(bc_file_system_error, name) == 21);
const _: () = assert!(offset_of!(bc_file_system_error, failed_location) == 280);
const _: () =
    assert!(size_of::<bc_file_system_location>() == if size_of::<usize>() == 8 { 32 } else { 16 });
const _: () = assert!(
    offset_of!(bc_file_system_location, length) == if size_of::<usize>() == 8 { 24 } else { 12 }
);
const _: () = assert!(size_of::<bc_file_system_read_limit>() == 16);
const _: () = assert!(offset_of!(bc_file_system_read_limit, at_most) == 8);
const _: () = assert!(size_of::<bc_file_system_permissions>() == 8);
const _: () = assert!(offset_of!(bc_file_system_permissions, mode) == 4);
const _: () = assert!(size_of::<bc_file_system_write_options>() == 20);
const _: () = assert!(offset_of!(bc_file_system_write_options, final_symlink) == 16);
const _: () = assert!(size_of::<bc_file_system_read_options>() == 24);
const _: () = assert!(offset_of!(bc_file_system_read_options, final_symlink) == 16);
const _: () = assert!(size_of::<bc_file_system_status>() == 48);
const _: () = assert!(offset_of!(bc_file_system_status, link_count) == 40);
const _: () = assert!(size_of::<bc_file_system_entry>() == 264);
const _: () = assert!(offset_of!(bc_file_system_entry, name) == 8);
const _: () = assert!(size_of::<bc_file_system_fault>() == 20);
const _: () = assert!(offset_of!(bc_file_system_fault, system_code) == 16);

// Every function the header declares, in the header's order, under the one return convention: `i32` is the
// error kind or 0, and `error` is the caller's struct, zeroed on success and filled on failure. Each pointer's
// contract is the header's comment at that parameter.
unsafe extern "C" {
    /// The engine of a composition root. `engine` and `error` must not be null.
    pub fn bc_file_system_engine_create_live(
        engine: *mut *mut bc_file_system_engine,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The same engine with the observer installed; test code only.
    pub fn bc_file_system_engine_create_observed_for_tests(
        engine: *mut *mut bc_file_system_engine,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Releases an engine; `engine` may be null.
    pub fn bc_file_system_engine_release(engine: *mut bc_file_system_engine);
    /// Scripts one fault into an observed engine; `fault` is copied.
    pub fn bc_file_system_observed_fault_add(
        engine: *mut bc_file_system_engine,
        fault: *const bc_file_system_fault,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Forgets every scripted fault; the step counts are kept.
    pub fn bc_file_system_observed_faults_clear(
        engine: *mut bc_file_system_engine,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// How many times `operation` has reached `step`; `count` must not be null.
    pub fn bc_file_system_observed_step_count(
        engine: *const bc_file_system_engine,
        operation: i32,
        step: i32,
        count: *mut u64,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Opens the directory at `location`; `directory` receives the handle.
    pub fn bc_file_system_directory_open(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        final_symlink: i32,
        directory: *mut *mut bc_file_system_directory,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The text the directory was opened with, under the buffer protocol.
    pub fn bc_file_system_directory_opened_as(
        directory: *const bc_file_system_directory,
        buffer: *mut u8,
        capacity: usize,
        length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Closes the directory and frees the handle; `directory` may be null.
    pub fn bc_file_system_directory_release(directory: *mut bc_file_system_directory);
    /// Releases the lock and frees the handle; `lock` may be null.
    pub fn bc_file_system_lock_release(lock: *mut bc_file_system_lock);
    /// What is at `location`; absence is `*present == 0` with `status` untouched.
    pub fn bc_file_system_status_of(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        final_symlink: i32,
        present: *mut i32,
        status: *mut bc_file_system_status,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The bytes of a regular file, under the buffer protocol and the read limit.
    pub fn bc_file_system_read_file(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        options: *const bc_file_system_read_options,
        buffer: *mut u8,
        capacity: usize,
        length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Replaces or creates the regular file at `location` atomically.
    pub fn bc_file_system_write_file(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        bytes: *const u8,
        byte_count: usize,
        options: *const bc_file_system_write_options,
        durability_achieved: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Adds bytes to the end of the regular file at `location`.
    pub fn bc_file_system_append(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        bytes: *const u8,
        byte_count: usize,
        durability: i32,
        permissions: *const bc_file_system_permissions,
        durability_achieved: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Pushes the file or directory at `location` to stable storage.
    pub fn bc_file_system_sync(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        durability_achieved: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Creates one directory.
    pub fn bc_file_system_create_directory(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        permissions: *const bc_file_system_permissions,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Creates the directory and every missing one on the way.
    pub fn bc_file_system_create_directories(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        permissions: *const bc_file_system_permissions,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The entries of a directory, sorted by name bytes, into the caller's array.
    pub fn bc_file_system_list_directory(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        final_symlink: i32,
        entries: *mut bc_file_system_entry,
        capacity: usize,
        count: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Removes the non-directory at `location`; `*existed` says whether there was one.
    pub fn bc_file_system_remove_file(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        existed: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Removes the empty directory at `location`.
    pub fn bc_file_system_remove_directory(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        existed: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Removes `location` and everything below it.
    pub fn bc_file_system_remove_tree(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        existed: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Creates a uniquely named directory inside `parent`; the name under the buffer protocol.
    pub fn bc_file_system_create_temporary_directory(
        engine: *mut bc_file_system_engine,
        parent: *const bc_file_system_location,
        permissions: *const bc_file_system_permissions,
        buffer: *mut u8,
        capacity: usize,
        length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Renames `source` to `destination` in one step.
    pub fn bc_file_system_rename(
        engine: *mut bc_file_system_engine,
        source: *const bc_file_system_location,
        destination: *const bc_file_system_location,
        existing_destination: i32,
        durability: i32,
        durability_achieved: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Copies the regular file at `source` to `destination`.
    pub fn bc_file_system_copy_file(
        engine: *mut bc_file_system_engine,
        source: *const bc_file_system_location,
        destination: *const bc_file_system_location,
        options: *const bc_file_system_write_options,
        durability_achieved: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Sets the permission bits of the entry at `location` exactly to `mode`.
    pub fn bc_file_system_set_permissions(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        mode: u32,
        final_symlink: i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Creates a symlink at `location` whose text is `target`.
    pub fn bc_file_system_create_symlink(
        engine: *mut bc_file_system_engine,
        target: *const u8,
        target_length: usize,
        location: *const bc_file_system_location,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The text of the symlink at `location`, under the buffer protocol.
    pub fn bc_file_system_read_symlink(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        buffer: *mut u8,
        capacity: usize,
        length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Tries to take an advisory exclusive lock on the file at `location` without waiting.
    pub fn bc_file_system_try_lock(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        lock: *mut *mut bc_file_system_lock,
        outcome: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Renames the tree at `location` to a condemned name beside it; the name under the buffer protocol.
    pub fn bc_file_system_condemn(
        engine: *mut bc_file_system_engine,
        location: *const bc_file_system_location,
        buffer: *mut u8,
        capacity: usize,
        length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Removes every condemned entry directly inside `parent`.
    pub fn bc_file_system_remove_condemned(
        engine: *mut bc_file_system_engine,
        parent: *const bc_file_system_location,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Whether `path` is a relative path by the header's rules.
    pub fn bc_file_system_relative_path_validate(
        path: *const u8,
        length: usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Whether `path` is an absolute path by the header's rules.
    pub fn bc_file_system_absolute_path_validate(
        path: *const u8,
        length: usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// `base`, a slash and `relative`, under the buffer protocol.
    pub fn bc_file_system_path_join(
        base: *const u8,
        base_length: usize,
        relative: *const u8,
        relative_length: usize,
        buffer: *mut u8,
        capacity: usize,
        length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The parent of a valid path, as a prefix length; absence is `*present == 0`.
    pub fn bc_file_system_path_parent(
        path: *const u8,
        length: usize,
        present: *mut i32,
        parent_length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The last component of a valid path, located inside it.
    pub fn bc_file_system_path_last_component(
        path: *const u8,
        length: usize,
        offset: *mut usize,
        component_length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The components of a valid path, one per call, driven by `cursor`.
    pub fn bc_file_system_path_next_component(
        path: *const u8,
        length: usize,
        cursor: *mut usize,
        offset: *mut usize,
        component_length: *mut usize,
        present: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The stem of one component, as a prefix length.
    pub fn bc_file_system_path_stem(
        component: *const u8,
        length: usize,
        stem_length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// The extension of one component, located inside it; absence is `*present == 0`.
    pub fn bc_file_system_path_extension(
        component: *const u8,
        length: usize,
        offset: *mut usize,
        extension_length: *mut usize,
        present: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Where `path` stands with respect to `other`, lexically; a `bc_file_system_path_relation`.
    pub fn bc_file_system_path_relate(
        path: *const u8,
        length: usize,
        other: *const u8,
        other_length: usize,
        relation: *mut i32,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Loose text reduced to one spelling, under the buffer protocol.
    pub fn bc_file_system_path_lexically_normalize(
        path: *const u8,
        length: usize,
        buffer: *mut u8,
        capacity: usize,
        normalized: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// `relative` resolved against the valid absolute path `absolute_base`, under the buffer protocol.
    pub fn bc_file_system_path_resolve_against(
        absolute_base: *const u8,
        base_length: usize,
        relative: *const u8,
        relative_length: usize,
        buffer: *mut u8,
        capacity: usize,
        length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
    /// Whether one component spells a library file, and the library's own name inside it.
    pub fn bc_file_system_path_library_name(
        component: *const u8,
        length: usize,
        kind: *mut i32,
        stem_offset: *mut usize,
        stem_length: *mut usize,
        error: *mut bc_file_system_error,
    ) -> i32;
}
