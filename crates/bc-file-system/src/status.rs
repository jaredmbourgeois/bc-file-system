//! What the file system records about an entry: its [`Status`], and the [`Entry`] a directory listing answers.
//! Each crosses the boundary in one function per direction, so the field order lives in one place, and an entry
//! type outside the enum is refused once, here.

use std::ffi::OsString;
use std::os::unix::ffi::OsStringExt;

use crate::error::{UnknownValue, variant_of};
use crate::ffi;

/// What an entry is, judged without following a symlink.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EntryType {
    /// A regular file.
    RegularFile = 1,
    /// A directory.
    Directory = 2,
    /// A symlink, itself: reported, never followed.
    Symlink = 3,
    /// A socket, a FIFO, a device.
    Other = 4,
}

impl EntryType {
    /// Every entry type, in the header's order.
    pub const ALL: [Self; 4] = [
        Self::RegularFile,
        Self::Directory,
        Self::Symlink,
        Self::Other,
    ];
}

impl From<EntryType> for i32 {
    fn from(entry_type: EntryType) -> Self {
        entry_type as Self
    }
}

impl TryFrom<i32> for EntryType {
    type Error = UnknownValue;

    fn try_from(value: i32) -> Result<Self, UnknownValue> {
        variant_of(&Self::ALL, value, "bc_file_system_entry_type")
    }
}

/// What the file system records about one entry.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Status {
    /// What the entry is.
    pub entry_type: EntryType,
    /// A regular file's size in bytes; 0 for every other type.
    pub size: i64,
    /// The permission bits (`0o644`), without the file-type bits.
    pub mode: u32,
    /// `st_dev`: with `inode`, the entry's identity.
    pub device: u64,
    /// `st_ino`.
    pub inode: u64,
    /// `st_nlink`.
    pub link_count: u64,
}

impl Status {
    /// The status as the core lays it out.
    #[must_use]
    pub const fn to_c(&self) -> ffi::bc_file_system_status {
        ffi::bc_file_system_status {
            entry_type: self.entry_type as i32,
            size: self.size,
            mode: self.mode,
            device: self.device,
            inode: self.inode,
            link_count: self.link_count,
        }
    }
}

impl TryFrom<&ffi::bc_file_system_status> for Status {
    type Error = UnknownValue;

    /// The core's status as the typed value; an entry type outside the enum is refused.
    fn try_from(c_status: &ffi::bc_file_system_status) -> Result<Self, UnknownValue> {
        Ok(Self {
            entry_type: EntryType::try_from(c_status.entry_type)?,
            size: c_status.size,
            mode: c_status.mode,
            device: c_status.device,
            inode: c_status.inode,
            link_count: c_status.link_count,
        })
    }
}

/// One entry of a directory listing: its name, one component, and what it is.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Entry {
    /// What the entry is.
    pub entry_type: EntryType,
    /// The entry's name, one component, as the bytes the system gave.
    pub name: OsString,
}

impl TryFrom<&ffi::bc_file_system_entry> for Entry {
    type Error = UnknownValue;

    /// The core's entry as the typed value. An entry type outside the enum is refused, and so is a name length
    /// the fixed field cannot hold, which the core never answers.
    fn try_from(c_entry: &ffi::bc_file_system_entry) -> Result<Self, UnknownValue> {
        let entry_type = EntryType::try_from(c_entry.entry_type)?;
        let name_length = usize::try_from(c_entry.name_length).unwrap_or(usize::MAX);
        let Some(name) = c_entry.name.get(..name_length) else {
            return Err(UnknownValue {
                enumeration: "bc_file_system_entry.name_length (at most 256)",
                value: i32::try_from(c_entry.name_length).unwrap_or(i32::MAX),
            });
        };
        Ok(Self {
            entry_type,
            name: OsString::from_vec(name.to_vec()),
        })
    }
}
