//! A test's own directory, and the one file of the Rust face's tests where a reach is legal (AGENTS.md, "Inject,
//! never reach", legal site 3: a test may own a directory it created). It asks the system where temporary files
//! go (`std::env::temp_dir`) and the process's id, once each, to make a name nobody else holds; and it sets a
//! case up and checks its outcome through the standard library's file system, so that what a test asserts was
//! measured without the library under test.

use std::ffi::OsString;
use std::fs;
use std::io;
use std::os::unix::fs::{PermissionsExt, symlink};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

/// How many directories this process has asked for, so two tests running at once never ask for one name.
static DIRECTORIES_ASKED_FOR: AtomicU64 = AtomicU64::new(0);

/// How many names one creation tries. A name is taken only by a directory that a killed run of an earlier
/// process with the same id left behind, so a few tries are plenty, and the bound keeps a broken temporary
/// directory from looping.
const NAMES_TRIED: u32 = 64;

/// A directory a test created, removed with everything in it when dropped. Every path it answers is inside it,
/// and every raw helper acts inside it.
#[derive(Debug)]
pub struct TemporaryDirectory {
    /// Absolute, with no symlink in it (macOS reaches its temporary directories through one), so the paths a test
    /// builds from it compare equal to what the system reports.
    path: PathBuf,
}

impl TemporaryDirectory {
    /// A fresh, empty directory under the system's temporary directory, named for `tag` (one component naming the
    /// test), the process and a number this process has not used: `bc-file-system-<tag>-<pid>-<number>`. The
    /// directory is made exclusively, so a name a killed run left behind is passed over, never reused.
    ///
    /// # Errors
    ///
    /// The system's error when the temporary directory cannot be resolved or a directory cannot be made in it.
    pub fn create(tag: &str) -> io::Result<Self> {
        let system_temporary = fs::canonicalize(std::env::temp_dir())?;
        let process = std::process::id();
        let mut names_tried = 0;
        loop {
            names_tried += 1;
            let number = DIRECTORIES_ASKED_FOR.fetch_add(1, Ordering::Relaxed);
            let path = system_temporary.join(format!("bc-file-system-{tag}-{process}-{number}"));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(Self { path }),
                Err(error)
                    if error.kind() == io::ErrorKind::AlreadyExists
                        && names_tried < NAMES_TRIED => {}
                Err(error) => return Err(error),
            }
        }
    }

    /// The directory's absolute path.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The absolute path of `relative` inside the directory.
    #[must_use]
    pub fn path_of(&self, relative: &str) -> PathBuf {
        self.path.join(relative)
    }

    /// Creates or replaces the file at `relative` with `bytes`, with exactly `mode`.
    ///
    /// # Errors
    ///
    /// The system's error when the file cannot be written or its mode set.
    pub fn write_raw(&self, relative: &str, bytes: &[u8], mode: u32) -> io::Result<()> {
        let path = self.path_of(relative);
        fs::write(&path, bytes)?;
        fs::set_permissions(&path, fs::Permissions::from_mode(mode))
    }

    /// The bytes of the file at `relative`.
    ///
    /// # Errors
    ///
    /// The system's error when the file cannot be read.
    pub fn read_raw(&self, relative: &str) -> io::Result<Vec<u8>> {
        fs::read(self.path_of(relative))
    }

    /// Creates one directory at `relative`, with exactly `mode`.
    ///
    /// # Errors
    ///
    /// The system's error when the directory cannot be made or its mode set.
    pub fn make_directory_raw(&self, relative: &str, mode: u32) -> io::Result<()> {
        let path = self.path_of(relative);
        fs::create_dir(&path)?;
        fs::set_permissions(&path, fs::Permissions::from_mode(mode))
    }

    /// Creates a symlink at `relative` whose text is `target`, as spelled.
    ///
    /// # Errors
    ///
    /// The system's error when the link cannot be made.
    pub fn make_symlink_raw(&self, relative: &str, target: &str) -> io::Result<()> {
        symlink(target, self.path_of(relative))
    }

    /// The permission bits of the entry at `relative` itself (a symlink is not followed).
    ///
    /// # Errors
    ///
    /// The system's error when the entry cannot be looked at.
    pub fn mode_raw(&self, relative: &str) -> io::Result<u32> {
        Ok(fs::symlink_metadata(self.path_of(relative))?
            .permissions()
            .mode()
            & 0o7777)
    }

    /// Whether anything is at `relative` (a symlink counts, whatever it points at).
    #[must_use]
    pub fn exists_raw(&self, relative: &str) -> bool {
        fs::symlink_metadata(self.path_of(relative)).is_ok()
    }

    /// Whether the entry at `relative` is itself a symlink.
    #[must_use]
    pub fn is_symlink_raw(&self, relative: &str) -> bool {
        fs::symlink_metadata(self.path_of(relative))
            .is_ok_and(|metadata| metadata.file_type().is_symlink())
    }

    /// Every name directly inside the directory at `relative` (`""` for the test's own), hidden ones included,
    /// sorted.
    ///
    /// # Errors
    ///
    /// The system's error when the directory cannot be listed.
    pub fn names_raw(&self, relative: &str) -> io::Result<Vec<OsString>> {
        let mut names = fs::read_dir(self.path_of(relative))?
            .map(|entry| entry.map(|entry| entry.file_name()))
            .collect::<io::Result<Vec<OsString>>>()?;
        names.sort();
        Ok(names)
    }
}

impl Drop for TemporaryDirectory {
    fn drop(&mut self) {
        // Access is given back first, so a directory a test locked (mode 0) can be emptied. Removal is best effort:
        // a drop cannot report, and a directory left behind costs disk, never a later test's correctness, since
        // every name is new.
        restore_access(&self.path);
        let _removed = fs::remove_dir_all(&self.path);
    }
}

/// Gives the owner back full access to every directory at and below `path`, never following a symlink.
fn restore_access(path: &Path) {
    let Ok(metadata) = fs::symlink_metadata(path) else {
        return;
    };
    if !metadata.is_dir() {
        return;
    }
    let _restored = fs::set_permissions(path, fs::Permissions::from_mode(0o700));
    let Ok(entries) = fs::read_dir(path) else {
        return;
    };
    for entry in entries.flatten() {
        restore_access(&entry.path());
    }
}

#[cfg(test)]
mod tests {
    use super::TemporaryDirectory;

    #[test]
    fn each_directory_is_new_empty_and_free_of_symlinks() {
        let first = TemporaryDirectory::create("same").expect("a directory");
        let second = TemporaryDirectory::create("same").expect("another directory");

        assert_ne!(first.path(), second.path());
        assert!(first.names_raw("").expect("listed").is_empty());
        assert_eq!(
            std::fs::canonicalize(first.path()).expect("resolved"),
            first.path()
        );
    }

    #[test]
    fn a_directory_goes_when_dropped_even_with_a_locked_directory_inside() {
        let directory = TemporaryDirectory::create("dropped").expect("a directory");
        directory
            .make_directory_raw("locked", 0o755)
            .expect("setup");
        directory
            .write_raw("locked/file", b"held", 0o644)
            .expect("setup");
        directory
            .make_symlink_raw("locked/link", "/")
            .expect("setup");
        std::fs::set_permissions(
            directory.path_of("locked"),
            std::os::unix::fs::PermissionsExt::from_mode(0o000),
        )
        .expect("setup");
        let path = directory.path().to_path_buf();

        drop(directory);

        assert!(!path.exists());
    }

    #[test]
    fn the_raw_helpers_set_up_and_read_back_what_they_were_given() {
        let directory = TemporaryDirectory::create("raw").expect("a directory");

        directory
            .write_raw("file", b"bytes", 0o640)
            .expect("written");
        directory.make_directory_raw("folder", 0o750).expect("made");
        directory.make_symlink_raw("link", "file").expect("linked");

        assert_eq!(directory.read_raw("file").expect("read"), b"bytes");
        assert_eq!(directory.mode_raw("file").expect("mode"), 0o640);
        assert_eq!(directory.mode_raw("folder").expect("mode"), 0o750);
        assert!(directory.is_symlink_raw("link"));
        assert!(!directory.is_symlink_raw("file"));
        assert!(directory.exists_raw("folder"));
        assert!(!directory.exists_raw("absent"));
        assert_eq!(
            directory.names_raw("").expect("listed"),
            ["file", "folder", "link"]
        );
    }
}
