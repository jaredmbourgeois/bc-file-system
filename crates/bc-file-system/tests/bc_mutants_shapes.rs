//! bc-mutants' file operations (`crates/bc-mutants-run/src/fs.rs` there, read-only here), composed from this
//! crate's API in the shape the swap will give `fs.rs` (bc-plumb's `mutation-ledger-drains-itself`, stage
//! `adopt-bc-file-system`), with that file's four tests copied as they are. Passing them is the exit criterion
//! of stage `rust-crates`: the crate meets a consumer's own contract, and no file in bc-mutants changes.
//!
//! The four test bodies changed in three ways and no other: their imports; their temporary directory
//! (`bc-file-system-testing`'s, for bc-mutants' testkit); and one argument, the engine, first at every call,
//! because a function is handed the file system it acts through (inject, never reach). Each test owns an
//! observed engine, as every test of this repository does. Their error spellings did not change: `RunError`
//! below is bc-mutants' own, cut to the three variants these functions produce, and the library's answers map
//! onto it in one place, [`RunError::from_library`] and [`ExclusiveLock::try_take`]:
//!
//! | bc-mutants                | bc-file-system                                                          |
//! |---------------------------|-------------------------------------------------------------------------|
//! | `RunError::Request`       | `ErrorKind::InvalidArgument` (and a path that names no file)            |
//! | `RunError::Locked{path}`  | `LockOutcome::HeldElsewhere`: an answer, which the composition turns into the consumer's error |
//! | `RunError::Io`            | every other `ErrorKind`, the library's `Error` as its source            |
//!
//! Where bc-mutants' shape and the library's rules differ, the composition bridges, and the function says how.
//! A value the library requires and std chose silently is marked `swept-default:` (AGENTS.md, "No default
//! arguments"): inherited, not yet decided by bc-mutants.
//!
//! This file is a legal reach site (AGENTS.md, "Inject, never reach", site 3): the copied test bodies set up and
//! check their cases with `std::fs`, inside the temporary directory they own.

use std::path::{Path, PathBuf};

use bc_file_system::path;
use bc_file_system::{
    AbsolutePath, Durability, Engine, EntryType, Error, ErrorDetails, ErrorKind,
    ExistingDestination, FinalSymlink, Location, Lock, LockOutcome, Operation, Permissions,
    ReadLimit, ReadOptions, RelativePath, WriteOptions,
};

/// bc-mutants' `RunError`, cut to the variants the seven functions produce; the swap keeps bc-mutants' own
/// enum, whose `Io::source` becomes the library's [`Error`] instead of `std::io::Error`.
#[derive(Debug)]
enum RunError {
    /// A file-system operation failed.
    Io {
        /// What the runner was doing.
        operation: &'static str,
        /// The path involved.
        path: PathBuf,
        /// The failure, as the library reported it.
        source: Error,
    },
    /// Another process holds the lock.
    Locked {
        /// The lock file.
        path: PathBuf,
    },
    /// What a person asked for names what cannot be.
    Request(String),
}

impl std::fmt::Display for RunError {
    /// bc-mutants' own wording for the three variants.
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io {
                operation,
                path,
                source,
            } => write!(formatter, "{operation} {} failed: {source}", path.display()),
            Self::Locked { path } => write!(
                formatter,
                "another process holds {}: one writer at a time",
                path.display()
            ),
            Self::Request(why) => write!(formatter, "{why}"),
        }
    }
}

impl RunError {
    /// The mapping of the library's errors onto bc-mutants' own: a contract violation the library refused is the
    /// request's fault; every other kind is a failure of the file system at `path`. Shaped as bc-mutants'
    /// `RunError::io(operation, path)`, for `map_err`.
    fn from_library(operation: &'static str, path: &Path) -> impl FnOnce(Error) -> Self {
        let path = path.to_path_buf();
        move |source| {
            if source.kind == ErrorKind::InvalidArgument {
                Self::Request(format!("{operation} {}: {source}", path.display()))
            } else {
                Self::Io {
                    operation,
                    path,
                    source,
                }
            }
        }
    }
}

/// `path` as the library's absolute path value; text that is no absolute path is the request's fault.
fn absolute(operation: &'static str, path: &Path) -> Result<AbsolutePath, RunError> {
    AbsolutePath::from_path(path).map_err(RunError::from_library(operation, path))
}

/// Writes `bytes` to `path` through a temporary file beside it and a rename, syncing the file and then its
/// directory, so a reader sees the old bytes or the new ones and never half of either.
///
/// Bridges: bc-mutants creates missing parents (`create_dir_all`) and the library never creates a parent
/// implicitly, so `create_directories` comes first; bc-mutants syncs the file and its directory, which is
/// `Durability::Durable` on `write_file` (a directory sync that fails lowers the answer rather than failing,
/// as bc-mutants' best-effort directory sync does); the library stages its own temporary name
/// (`.<name>.bc-tmp-<pid>-<token>`, swept by `remove_condemned`), not `.<name>.writing`.
fn write_durably(engine: &Engine, path: &Path, bytes: &[u8]) -> Result<(), RunError> {
    let target = absolute("write", path)?;
    let Some(parent) = path::parent(&target).map_err(RunError::from_library("write", path))? else {
        return Err(RunError::Request(format!(
            "{} names no file in a directory",
            path.display()
        )));
    };
    engine
        .create_directories(
            &Location::At(&parent),
            // swept-default: permissions — std's `create_dir_all` under the usual umask (022).
            Permissions::exactly(0o755),
        )
        .map_err(RunError::from_library("create", parent.as_path()))?;
    let options = WriteOptions {
        durability: Durability::Durable,
        // swept-default: permissions — std's `File::create` under the usual umask (022); a replace resets it,
        // as bc-mutants' rename over the old file did.
        permissions: Permissions::exactly(0o644),
        existing_destination: ExistingDestination::Replace,
        // swept-default: final_symlink — bc-mutants' rename replaced a symlink at `path` with the file, which the
        // library offers neither way; refusing is the conservative reading.
        final_symlink: FinalSymlink::Refuse,
    };
    // The durability achieved is not reported: bc-mutants' directory sync was best effort too.
    engine
        .write_file(&Location::At(&target), bytes, &options)
        .map_err(RunError::from_library("write", path))?;
    Ok(())
}

/// An exclusive advisory lock on a file, held until this is dropped. It never outlives the process, so a killed
/// holder leaves nothing to clean. It borrows the engine, which outlives every lock taken through it.
#[derive(Debug)]
struct ExclusiveLock<'engine> {
    _lock: Lock<'engine>,
}

impl<'engine> ExclusiveLock<'engine> {
    /// Takes the lock at `path`, creating the file, without waiting.
    ///
    /// Bridges: bc-mutants opens with `create(true)` and the library's `try_lock` never creates, so an empty file
    /// is written first under `ExistingDestination::Refuse` (one already there is `AlreadyExists`, which is fine:
    /// a replace would put a new file under a lock someone holds); `HeldElsewhere` is an answer of the library,
    /// which this function turns into bc-mutants' `Locked`.
    fn try_take(engine: &'engine Engine, path: &Path) -> Result<Self, RunError> {
        let lock_file = absolute("open", path)?;
        let location = Location::At(&lock_file);
        let create_only = WriteOptions {
            durability: Durability::Written,
            // swept-default: permissions — std's `OpenOptions::create` under the usual umask (022).
            permissions: Permissions::exactly(0o644),
            existing_destination: ExistingDestination::Refuse,
            final_symlink: FinalSymlink::Refuse,
        };
        match engine.write_file(&location, b"", &create_only) {
            Ok(_achieved) => {}
            Err(error) if error.kind == ErrorKind::AlreadyExists => {}
            Err(error) => return Err(RunError::from_library("open", path)(error)),
        }
        match engine
            .try_lock(&location)
            .map_err(RunError::from_library("lock", path))?
        {
            LockOutcome::Acquired(lock) => Ok(Self { _lock: lock }),
            LockOutcome::HeldElsewhere => Err(RunError::Locked {
                path: path.to_path_buf(),
            }),
        }
    }
}

/// When the library answers that nothing was at `path`, whether that is because something on the way is not a
/// directory. The library reads that as absence (as `lstat` reads `ENOTDIR`, the rule `status` keeps), while std,
/// which bc-mutants' `fs.rs` was written over, answers an error ("a tree under a file cannot be"). The nearest
/// ancestor that is present decides.
fn blocked_by_a_file_on_the_way(
    engine: &Engine,
    target: &AbsolutePath,
) -> Result<Option<AbsolutePath>, Error> {
    let mut ancestor = path::parent(target)?;
    while let Some(candidate) = ancestor {
        match engine.status(&Location::At(&candidate), FinalSymlink::Follow)? {
            Some(status) if status.entry_type == EntryType::Directory => return Ok(None),
            Some(_) => return Ok(Some(candidate)),
            None => ancestor = path::parent(&candidate)?,
        }
    }
    Ok(None)
}

/// The library's absence, kept as absence only when nothing on the way is a file (see
/// [`blocked_by_a_file_on_the_way`]); otherwise bc-mutants' error.
fn absence_or_blocked(
    engine: &Engine,
    target: &AbsolutePath,
    called: Operation,
    path: &Path,
) -> Result<(), RunError> {
    match blocked_by_a_file_on_the_way(engine, target)
        .map_err(RunError::from_library("remove", path))?
    {
        None => Ok(()),
        Some(file) => Err(RunError::Io {
            operation: "remove",
            path: path.to_path_buf(),
            source: Error::from(ErrorDetails {
                kind: ErrorKind::NotDirectory,
                operation: called,
                step: None,
                system_code: 0,
                path: file.as_path().to_path_buf(),
                name_truncated: false,
                failed_location: None,
                unknown_value: None,
            }),
        }),
    }
}

/// Removes the file at `path`; one that is not there is already removed.
///
/// Bridge: absence behind a file on the way is an error, as std answers it (see
/// [`blocked_by_a_file_on_the_way`]).
fn remove_file_if_present(engine: &Engine, path: &Path) -> Result<(), RunError> {
    let target = absolute("remove", path)?;
    let existed = engine
        .remove_file(&Location::At(&target))
        .map_err(RunError::from_library("remove", path))?;
    if existed {
        return Ok(());
    }
    absence_or_blocked(engine, &target, Operation::RemoveFile, path)
}

/// Removes the directory tree at `path`; one that is not there is already removed.
///
/// Bridge: absence behind a file on the way is an error, as std answers it (see
/// [`blocked_by_a_file_on_the_way`]).
fn remove_tree_if_present(engine: &Engine, path: &Path) -> Result<(), RunError> {
    let target = absolute("remove", path)?;
    let existed = engine
        .remove_tree(&Location::At(&target))
        .map_err(RunError::from_library("remove", path))?;
    if existed {
        return Ok(());
    }
    absence_or_blocked(engine, &target, Operation::RemoveTree, path)
}

/// The bytes of `path`, or `None` when nothing is there. Anything else that stops the read is an error, never
/// an empty file. The library's composition, with std's choices stated.
fn read_if_present(engine: &Engine, path: &Path) -> Result<Option<Vec<u8>>, RunError> {
    let target = absolute("read", path)?;
    let options = ReadOptions {
        // swept-default: limit — std's `read` takes the whole file.
        limit: ReadLimit::WholeFile,
        // swept-default: final_symlink — std's `read` follows a symlink.
        final_symlink: FinalSymlink::Follow,
    };
    engine
        .read_file_if_present(&Location::At(&target), &options)
        .map_err(RunError::from_library("read", path))
}

/// Drops the directories `names` under `root` as one unit: each is condemned (renamed to a condemned name) first,
/// and only then is anything removed. A kill at any point leaves each directory whole under its own name or
/// under a condemned one, and the next drop finishes what a killed one condemned.
///
/// Bridges: bc-mutants renames a nested name (`build/out`) to one condemned name at the root
/// (`.bc-condemned-build%2Fout`), and the library's `condemn` renames an entry beside itself
/// (`build/.bc-condemned-out`), so each name is condemned where it is and every distinct parent is swept with
/// `remove_condemned` afterwards (the root's sweep alone would miss a nested one). `condemn` has no durability of
/// its own, so each parent is synced after the renames, as bc-mutants synced the root (best effort, both). A
/// condemned name a killed drop left is no obstacle to the library (it retries with a token), so the first sweep
/// is kept for bc-mutants' order, not for the rename.
fn drop_directories(engine: &Engine, root: &Path, names: &[String]) -> Result<(), RunError> {
    // What a killed drop condemned goes first.
    finish_condemned(engine, root)?;
    let root_path = absolute("rename", root)?;
    let mut parents: Vec<AbsolutePath> = Vec::new();
    for name in names {
        let relative = RelativePath::from_bytes(name.as_bytes())
            .map_err(RunError::from_library("rename", &root.join(name)))?;
        let live = path::join(&root_path, &relative)
            .map_err(RunError::from_library("rename", &root.join(name)))?;
        match engine.condemn(&Location::At(&live)) {
            Ok(_condemned_name) => {}
            Err(error) if error.kind == ErrorKind::NotFound => {}
            Err(error) => return Err(RunError::from_library("rename", live.as_path())(error)),
        }
        // A path joined under the root always has a parent: the root, or a directory beneath it.
        if let Some(parent) =
            path::parent(&live).map_err(RunError::from_library("rename", live.as_path()))?
            && !parents.contains(&parent)
        {
            parents.push(parent);
        }
    }
    // The renames are what a killed drop leaves behind: make them durable before anything is removed. Best
    // effort, as bc-mutants' was: a failed sync leaves the renames done and only less durable.
    for parent in &parents {
        let _synced = engine.sync(&Location::At(parent));
    }
    for parent in &parents {
        finish_condemned(engine, parent.as_path())?;
    }
    finish_condemned(engine, root)
}

/// Removes every directory under `root` that an earlier drop condemned; a root that is not there is done.
///
/// Bridge: the library's sweep also clears the staged files a killed write left in `root` (their names carry
/// `.bc-tmp-`), which bc-mutants' sweep did not look for.
fn finish_condemned(engine: &Engine, root: &Path) -> Result<(), RunError> {
    let parent = absolute("list", root)?;
    match engine.remove_condemned(&Location::At(&parent)) {
        Ok(()) => Ok(()),
        Err(error) if error.kind == ErrorKind::NotFound => Ok(()),
        Err(error) => Err(RunError::from_library("list", root)(error)),
    }
}

// ---------------------------------------------------------------------------------------------------------------
// bc-mutants' four tests, copied as they are (see the file comment for the three changes)
// ---------------------------------------------------------------------------------------------------------------

mod tests {
    use bc_file_system_testing::{ObservedEngine, TemporaryDirectory};

    use super::RunError;
    use super::{
        ExclusiveLock, drop_directories, finish_condemned, read_if_present, remove_file_if_present,
        remove_tree_if_present, write_durably,
    };

    #[test]
    fn a_durable_write_replaces_the_whole_file_and_leaves_no_temporary_behind() {
        let observed = ObservedEngine::observed_for_tests().unwrap();
        let engine = observed.engine();
        let dir = TemporaryDirectory::create("durable").unwrap();
        let path = dir.path().join("deep/state.json");
        write_durably(engine, &path, b"one").unwrap();
        write_durably(engine, &path, b"two").unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), b"two");
        let names: Vec<String> = std::fs::read_dir(dir.path().join("deep"))
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names, ["state.json"]);
        assert!(matches!(
            write_durably(engine, std::path::Path::new("/"), b"x"),
            Err(RunError::Request(_))
        ));
    }

    #[test]
    fn a_second_holder_is_told_the_lock_is_held_and_gets_it_once_released() {
        let observed = ObservedEngine::observed_for_tests().unwrap();
        let engine = observed.engine();
        let dir = TemporaryDirectory::create("lock").unwrap();
        let path = dir.path().join("lock");
        let first = ExclusiveLock::try_take(engine, &path).unwrap();
        assert!(matches!(
            ExclusiveLock::try_take(engine, &path),
            Err(RunError::Locked { path: held }) if held == path
        ));
        drop(first);
        assert!(ExclusiveLock::try_take(engine, &path).is_ok());
        assert!(matches!(
            ExclusiveLock::try_take(engine, &dir.path().join("absent/lock")),
            Err(RunError::Io { .. })
        ));
    }

    #[test]
    fn removing_what_is_not_there_is_done_and_a_read_tells_absent_from_cannot() {
        let observed = ObservedEngine::observed_for_tests().unwrap();
        let engine = observed.engine();
        let dir = TemporaryDirectory::create("absent").unwrap();
        let file = dir.path().join("file");
        remove_file_if_present(engine, &file).unwrap();
        remove_tree_if_present(engine, &dir.path().join("tree")).unwrap();
        assert_eq!(read_if_present(engine, &file).unwrap(), None);
        std::fs::write(&file, b"held").unwrap();
        assert_eq!(
            read_if_present(engine, &file).unwrap().as_deref(),
            Some(&b"held"[..])
        );
        remove_file_if_present(engine, &file).unwrap();
        assert!(!file.exists());
        // A directory where a file is read cannot be read: an error, never empty.
        assert!(matches!(
            read_if_present(engine, dir.path()),
            Err(RunError::Io { .. })
        ));
        // And a directory is not a file to remove.
        std::fs::create_dir(dir.path().join("a-directory")).unwrap();
        assert!(remove_file_if_present(engine, &dir.path().join("a-directory")).is_err());
        // Only what is not there is already removed: a tree under a file cannot be.
        std::fs::write(&file, b"held").unwrap();
        assert!(matches!(
            remove_tree_if_present(engine, &file.join("tree")),
            Err(RunError::Io { .. })
        ));
    }

    #[test]
    fn a_drop_takes_every_directory_named_and_finishes_what_a_killed_one_condemned() {
        let observed = ObservedEngine::observed_for_tests().unwrap();
        let engine = observed.engine();
        let dir = TemporaryDirectory::create("drop").unwrap();
        let root = dir.path();
        std::fs::create_dir_all(root.join("target/debug")).unwrap();
        std::fs::write(root.join("target/debug/binary"), b"x").unwrap();
        std::fs::create_dir_all(root.join("build/out")).unwrap();
        std::fs::write(root.join("kept.txt"), b"kept").unwrap();
        // A killed drop left a condemned directory behind.
        std::fs::create_dir_all(root.join(".bc-condemned-old/inner")).unwrap();
        // And one where `target` goes, which a rename onto it would meet.
        std::fs::create_dir_all(root.join(".bc-condemned-target/stale")).unwrap();
        std::fs::write(root.join(".bc-condemned-target/stale/binary"), b"old").unwrap();

        drop_directories(engine, root, &["target".to_owned(), "build/out".to_owned()]).unwrap();
        let mut left: Vec<String> = std::fs::read_dir(root)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        left.sort();
        assert_eq!(left, ["build", "kept.txt"]);
        assert_eq!(std::fs::read_dir(root.join("build")).unwrap().count(), 0);

        // Nothing named is there: still done, and nothing else is touched.
        drop_directories(engine, root, &["target".to_owned()]).unwrap();
        finish_condemned(engine, &root.join("absent")).unwrap();
        assert!(root.join("kept.txt").is_file());
        // A root that cannot be listed is an error, never a drop finished.
        assert!(matches!(
            finish_condemned(engine, &root.join("kept.txt")),
            Err(RunError::Io { .. })
        ));
    }
}
