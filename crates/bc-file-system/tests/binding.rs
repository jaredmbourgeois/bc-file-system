//! The Rust binding suite: the face's contract with the C header, apart from what the operations do to the file
//! system (the C++ contract suite's job). Each operation reaches the core once through the typed face and answers
//! from the file system; the mapping of every enum is total in both directions; each value type makes one round
//! trip across the ABI; the error struct is converted with its path composed from the location the face holds,
//! from the end a two-location error names; and the face's compositions (`read_file_if_present`, the grow loop
//! over a real file and a real directory, `lines`) do what the design doc says.
//!
//! Every case runs on an observed engine (a test never names the live one) in a temporary directory it owns,
//! and checks what it asserts through that directory's raw helpers, never through the code under test.

use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};

use bc_file_system::{
    AbsolutePath, Durability, Engine, Entry, EntryType, Error, ErrorDetails, ErrorKind,
    ExistingDestination, FailedLocation, FinalSymlink, Location, LockOutcome, Operation,
    Permissions, PermissionsPolicy, ReadLimit, ReadOptions, RelativePath, Status, Step,
    UnknownValue, WriteOptions, ffi, lines,
};
use bc_file_system_testing::{Fault, FaultAction, ObservedEngine, TemporaryDirectory};

// The options every case states, built once here so a field added later fails in one place.
const WHOLE_FILE_REFUSING_SYMLINK: ReadOptions = ReadOptions {
    limit: ReadLimit::WholeFile,
    final_symlink: FinalSymlink::Refuse,
};
const WRITTEN_EXACTLY_0644: WriteOptions = WriteOptions {
    durability: Durability::Written,
    permissions: Permissions::exactly(0o644),
    existing_destination: ExistingDestination::Replace,
    final_symlink: FinalSymlink::Refuse,
};
const DURABLE_PRESERVING_OR_0600: WriteOptions = WriteOptions {
    durability: Durability::Durable,
    permissions: Permissions::preserve_or(0o600),
    existing_destination: ExistingDestination::Refuse,
    final_symlink: FinalSymlink::Follow,
};

// The errno values the cases name, the same number on Linux and on Apple platforms (ENAMETOOLONG, for one, is
// not, so no case names it).
const ENOENT: i32 = 2;
const EIO: i32 = 5;
const EACCES: i32 = 13;
const EEXIST: i32 = 17;
const ENOSPC: i32 = 28;

/// A case's own directory and its observed engine.
struct Case {
    directory: TemporaryDirectory,
    observed: ObservedEngine,
}

impl Case {
    fn named(tag: &str) -> Self {
        Self {
            directory: TemporaryDirectory::create(tag).expect("a temporary directory"),
            observed: ObservedEngine::observed_for_tests().expect("an observed engine"),
        }
    }

    fn engine(&self) -> &Engine {
        self.observed.engine()
    }

    /// The absolute path of `relative` inside the case's directory, as the library's value.
    fn at(&self, relative: &str) -> AbsolutePath {
        AbsolutePath::from_path(&self.directory.path_of(relative)).expect("an absolute path")
    }

    /// The case's directory itself, as the library's value (a trailing slash is no valid spelling).
    fn root(&self) -> AbsolutePath {
        AbsolutePath::from_path(self.directory.path()).expect("an absolute path")
    }
}

fn relative(text: &str) -> RelativePath {
    RelativePath::from_bytes(text.as_bytes()).expect("a relative path")
}

// ---------------------------------------------------------------------------------------------------------------
// Every operation reaches C, through the face
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn open_directory_reaches_c_and_the_core_remembers_the_text_it_was_opened_with() {
    let case = Case::named("open-directory");
    case.directory
        .make_directory_raw("folder", 0o755)
        .expect("setup");
    let folder = case.at("folder");

    let directory = case
        .engine()
        .open_directory(&Location::At(&folder), FinalSymlink::Refuse)
        .expect("the directory opens");

    assert_eq!(
        directory.opened_as().expect("the text is read back"),
        case.directory.path_of("folder")
    );
}

#[test]
fn status_reaches_c() {
    let case = Case::named("status");
    case.directory
        .write_raw("file", b"seven b", 0o640)
        .expect("setup");
    let file = case.at("file");

    let status = case
        .engine()
        .status(&Location::At(&file), FinalSymlink::Refuse)
        .expect("the file is described")
        .expect("the file is present");

    assert_eq!(status.entry_type, EntryType::RegularFile);
    assert_eq!(status.size, 7);
    assert_eq!(status.mode, 0o640);
}

#[test]
fn read_file_reaches_c() {
    let case = Case::named("read-file");
    case.directory
        .write_raw("file", b"content", 0o644)
        .expect("setup");
    let file = case.at("file");

    let bytes = case
        .engine()
        .read_file(&Location::At(&file), &WHOLE_FILE_REFUSING_SYMLINK)
        .expect("the file is read");

    assert_eq!(bytes, b"content");
}

#[test]
fn write_file_reaches_c_and_answers_the_durability_achieved() {
    let case = Case::named("write-file");
    let file = case.at("file");

    let achieved = case
        .engine()
        .write_file(&Location::At(&file), b"content", &WRITTEN_EXACTLY_0644)
        .expect("the file is written");

    assert_eq!(achieved, Durability::Written);
    assert_eq!(
        case.directory.read_raw("file").expect("raw read"),
        b"content"
    );
    assert_eq!(case.directory.mode_raw("file").expect("raw mode"), 0o644);
}

#[test]
fn append_reaches_c() {
    let case = Case::named("append");
    let log = case.at("log");

    let achieved = case
        .engine()
        .append(
            &Location::At(&log),
            b"entry\n",
            Durability::Written,
            Permissions::exactly(0o644),
        )
        .expect("the bytes are appended");

    assert_eq!(achieved, Durability::Written);
    assert_eq!(
        case.directory.read_raw("log").expect("raw read"),
        b"entry\n"
    );
}

#[test]
fn sync_reaches_c() {
    let case = Case::named("sync");
    case.directory
        .write_raw("file", b"content", 0o644)
        .expect("setup");
    let file = case.at("file");

    let achieved = case
        .engine()
        .sync(&Location::At(&file))
        .expect("the file is synced");

    assert_eq!(achieved, Durability::Durable);
}

#[test]
fn create_directory_reaches_c() {
    let case = Case::named("create-directory");
    let folder = case.at("folder");

    case.engine()
        .create_directory(&Location::At(&folder), Permissions::exactly(0o755))
        .expect("the directory is created");

    assert_eq!(case.directory.mode_raw("folder").expect("raw mode"), 0o755);
}

#[test]
fn create_directories_reaches_c() {
    let case = Case::named("create-directories");
    let chain = case.at("a/b/c");

    case.engine()
        .create_directories(&Location::At(&chain), Permissions::exactly(0o755))
        .expect("the chain is created");

    assert_eq!(case.directory.mode_raw("a/b/c").expect("raw mode"), 0o755);
}

#[test]
fn list_directory_reaches_c() {
    let case = Case::named("list-directory");
    case.directory
        .make_directory_raw("folder", 0o755)
        .expect("setup");
    case.directory
        .write_raw("folder/file", b"content", 0o644)
        .expect("setup");
    let folder = case.at("folder");

    let entries = case
        .engine()
        .list_directory(&Location::At(&folder), FinalSymlink::Refuse)
        .expect("the directory is listed");

    assert_eq!(
        entries,
        [Entry {
            entry_type: EntryType::RegularFile,
            name: OsString::from("file"),
        }]
    );
}

#[test]
fn remove_file_reaches_c_and_says_there_was_one() {
    let case = Case::named("remove-file");
    case.directory
        .write_raw("file", b"content", 0o644)
        .expect("setup");
    let file = case.at("file");

    let removed = case
        .engine()
        .remove_file(&Location::At(&file))
        .expect("the file is removed");
    let removed_again = case
        .engine()
        .remove_file(&Location::At(&file))
        .expect("absence is an answer");

    assert!(removed);
    assert!(!removed_again);
    assert!(!case.directory.exists_raw("file"));
}

#[test]
fn remove_directory_reaches_c() {
    let case = Case::named("remove-directory");
    case.directory
        .make_directory_raw("folder", 0o755)
        .expect("setup");
    let folder = case.at("folder");

    let removed = case
        .engine()
        .remove_directory(&Location::At(&folder))
        .expect("the directory is removed");

    assert!(removed);
    assert!(!case.directory.exists_raw("folder"));
}

#[test]
fn remove_tree_reaches_c() {
    let case = Case::named("remove-tree");
    case.directory
        .make_directory_raw("tree", 0o755)
        .expect("setup");
    case.directory
        .make_directory_raw("tree/sub", 0o755)
        .expect("setup");
    case.directory
        .write_raw("tree/sub/file", b"content", 0o644)
        .expect("setup");
    let tree = case.at("tree");

    let removed = case
        .engine()
        .remove_tree(&Location::At(&tree))
        .expect("the tree is removed");

    assert!(removed);
    assert!(!case.directory.exists_raw("tree"));
}

#[test]
fn create_temporary_directory_reaches_c_and_answers_the_name_it_made() {
    let case = Case::named("create-temporary-directory");
    case.directory
        .make_directory_raw("parent", 0o755)
        .expect("setup");
    let parent = case.at("parent");

    let name = case
        .engine()
        .create_temporary_directory(&Location::At(&parent), Permissions::exactly(0o700))
        .expect("a temporary directory is made");

    let made = format!("parent/{}", name.to_string_lossy());
    assert!(!name.is_empty());
    assert_eq!(case.directory.mode_raw(&made).expect("raw mode"), 0o700);
}

// A rename is real, and the error of a two-location operation names the end it belongs to: the face reads the
// core's `failed_location` and composes the path from the source or the destination accordingly.
#[test]
fn rename_reaches_c_and_its_error_names_the_end_that_failed() {
    let case = Case::named("rename");
    case.directory
        .write_raw("from", b"content", 0o644)
        .expect("setup");
    let from = case.at("from");
    let to = case.at("to");
    let missing_to = case.at("missing/to");
    let rename = |source: &AbsolutePath, destination: &AbsolutePath| {
        case.engine().rename(
            &Location::At(source),
            &Location::At(destination),
            ExistingDestination::Replace,
            Durability::Written,
        )
    };

    let moved = rename(&from, &to).expect("the file is renamed");
    let of_nothing = rename(&from, &to).expect_err("the source is gone");
    let into_nothing =
        rename(&to, &missing_to).expect_err("the destination's directory is missing");

    assert_eq!(moved, Durability::Written);
    assert_eq!(case.directory.read_raw("to").expect("raw read"), b"content");
    assert!(!case.directory.exists_raw("from"));
    assert_eq!(of_nothing.kind, ErrorKind::NotFound);
    assert_eq!(of_nothing.operation, Operation::Rename);
    assert_eq!(of_nothing.failed_location, Some(FailedLocation::First));
    assert_eq!(of_nothing.path, case.directory.path_of("from"));
    assert_eq!(into_nothing.kind, ErrorKind::NotFound);
    assert_eq!(into_nothing.failed_location, Some(FailedLocation::Second));
    assert_eq!(into_nothing.path, case.directory.path_of("missing/to"));
}

#[test]
fn copy_file_reaches_c_and_its_error_names_the_end_that_failed() {
    let case = Case::named("copy-file");
    case.directory
        .write_raw("from", b"content", 0o644)
        .expect("setup");
    let from = case.at("from");
    let to = case.at("to");
    let absent = case.at("absent");
    let missing_to = case.at("missing/to");
    let copy = |source: &AbsolutePath, destination: &AbsolutePath| {
        case.engine().copy_file(
            &Location::At(source),
            &Location::At(destination),
            &WRITTEN_EXACTLY_0644,
        )
    };

    let copied = copy(&from, &to).expect("the file is copied");
    let of_nothing = copy(&absent, &to).expect_err("there is no source");
    let into_nothing =
        copy(&from, &missing_to).expect_err("the destination's directory is missing");

    assert_eq!(copied, Durability::Written);
    assert_eq!(case.directory.read_raw("to").expect("raw read"), b"content");
    assert_eq!(
        case.directory.read_raw("from").expect("raw read"),
        b"content"
    );
    assert_eq!(of_nothing.kind, ErrorKind::NotFound);
    assert_eq!(of_nothing.operation, Operation::CopyFile);
    assert_eq!(of_nothing.failed_location, Some(FailedLocation::First));
    assert_eq!(of_nothing.path, case.directory.path_of("absent"));
    assert_eq!(into_nothing.failed_location, Some(FailedLocation::Second));
    assert_eq!(into_nothing.path, case.directory.path_of("missing/to"));
}

#[test]
fn set_permissions_reaches_c() {
    let case = Case::named("set-permissions");
    case.directory
        .write_raw("file", b"content", 0o644)
        .expect("setup");
    let file = case.at("file");

    case.engine()
        .set_permissions(&Location::At(&file), 0o600, FinalSymlink::Refuse)
        .expect("the mode is set");

    assert_eq!(case.directory.mode_raw("file").expect("raw mode"), 0o600);
}

#[test]
fn create_symlink_reaches_c() {
    let case = Case::named("create-symlink");
    let link = case.at("link");

    case.engine()
        .create_symlink(Path::new("target"), &Location::At(&link))
        .expect("the link is created");

    assert!(case.directory.is_symlink_raw("link"));
}

#[test]
fn read_symlink_reaches_c() {
    let case = Case::named("read-symlink");
    case.directory
        .make_symlink_raw("link", "target")
        .expect("setup");
    let link = case.at("link");

    let text = case
        .engine()
        .read_symlink(&Location::At(&link))
        .expect("the link is read");

    assert_eq!(text, Path::new("target"));
}

// The lock is real, and its outcome crosses both ways: the handle comes back under `Acquired`, a second ask
// answers `HeldElsewhere` (two locks in one process contend), and dropping the lock lets it go.
#[test]
fn try_lock_reaches_c_and_its_two_outcomes_cross() {
    let case = Case::named("try-lock");
    case.directory
        .write_raw("file.lock", b"", 0o644)
        .expect("setup");
    let lock_file = case.at("file.lock");
    let location = Location::At(&lock_file);

    let held = case
        .engine()
        .try_lock(&location)
        .expect("the lock is asked for");
    let refused = case
        .engine()
        .try_lock(&location)
        .expect("the lock is asked for again");
    assert!(matches!(held, LockOutcome::Acquired(_)));
    assert!(matches!(refused, LockOutcome::HeldElsewhere));
    drop(held);
    let again = case
        .engine()
        .try_lock(&location)
        .expect("the lock is asked for a third time");

    assert!(matches!(again, LockOutcome::Acquired(_)));
}

#[test]
fn condemn_reaches_c_and_answers_the_condemned_name() {
    let case = Case::named("condemn");
    case.directory
        .make_directory_raw("tree", 0o755)
        .expect("setup");
    let tree = case.at("tree");

    let name = case
        .engine()
        .condemn(&Location::At(&tree))
        .expect("the tree is condemned");

    assert_eq!(name, OsStr::new(".bc-condemned-tree"));
    assert!(case.directory.exists_raw(".bc-condemned-tree"));
    assert!(!case.directory.exists_raw("tree"));
}

#[test]
fn remove_condemned_reaches_c() {
    let case = Case::named("remove-condemned");
    case.directory
        .make_directory_raw("parent", 0o755)
        .expect("setup");
    case.directory
        .make_directory_raw("parent/.bc-condemned-tree", 0o755)
        .expect("setup");
    case.directory
        .write_raw("parent/kept", b"kept", 0o644)
        .expect("setup");
    let parent = case.at("parent");

    case.engine()
        .remove_condemned(&Location::At(&parent))
        .expect("the sweep runs");

    assert_eq!(
        case.directory.names_raw("parent").expect("raw names"),
        [OsString::from("kept")]
    );
}

// The observer's three calls reach C through the companion crate: on an observed engine each succeeds, and a
// fresh engine has counted nothing.
#[test]
fn add_fault_clear_faults_and_step_count_reach_c() {
    let observed = ObservedEngine::observed_for_tests().expect("an observed engine");
    let fault = Fault {
        operation: Operation::WriteFile,
        step: Step::Write,
        occurrence: 1,
        action: FaultAction::Fail {
            system_code: ENOSPC,
        },
    };

    observed.add_fault(fault).expect("the fault is scripted");
    observed.clear_faults().expect("the faults are cleared");
    let counted = observed
        .step_count(Operation::WriteFile, Step::Write)
        .expect("the count is read");

    assert_eq!(counted, 0);
}

// The portable-only factory reaches C once through the companion crate, and the engine kind crosses with it: its
// copy takes no fast path, so neither the clone's step nor the in-kernel copy's is reached and the bytes pass
// through the read-and-write loop.
#[test]
fn the_portable_only_factory_reaches_c_and_its_copy_takes_no_fast_path() {
    let directory = TemporaryDirectory::create("portable-only").expect("a temporary directory");
    let portable = ObservedEngine::portable_only_for_tests().expect("a portable-only engine");
    directory
        .write_raw("from", b"content", 0o644)
        .expect("setup");
    let from = AbsolutePath::from_path(&directory.path_of("from")).expect("an absolute path");
    let to = AbsolutePath::from_path(&directory.path_of("to")).expect("an absolute path");

    let copied = portable
        .engine()
        .copy_file(
            &Location::At(&from),
            &Location::At(&to),
            &WRITTEN_EXACTLY_0644,
        )
        .expect("the file is copied");

    assert_eq!(copied, Durability::Written);
    assert_eq!(directory.read_raw("to").expect("raw read"), b"content");
    let count_of = |step| {
        portable
            .step_count(Operation::CopyFile, step)
            .expect("the count is read")
    };
    assert_eq!(count_of(Step::CloneFile), 0);
    assert_eq!(count_of(Step::CopyRange), 0);
    assert!(count_of(Step::Pread) > 0);
}

// An engine created for tests is used from one thread at a time (the header); the face keeps that promise for a
// shared `&Engine` by taking the observer's turn on every call, so two threads' calls are serialised and every
// step is counted.
#[test]
fn an_observed_engine_shared_between_threads_counts_every_step() {
    let case = Case::named("two-threads");
    let root = case.root();
    let calls_per_thread: u64 = 50;

    std::thread::scope(|scope| {
        for _ in 0..2 {
            scope.spawn(|| {
                for _ in 0..calls_per_thread {
                    case.engine()
                        .status(&Location::At(&root), FinalSymlink::Refuse)
                        .expect("the directory is described");
                }
            });
        }
    });
    let root_opens = case
        .observed
        .step_count(Operation::Status, Step::Open)
        .expect("the count is read");

    assert_eq!(
        root_opens,
        2 * calls_per_thread,
        "each status opens its parent by path once"
    );
}

// A fault crosses into the core and the failure it scripts comes back typed: the kind the errno maps to, the step
// it was scripted at, the errno itself, and the path composed from the location.
#[test]
fn a_scripted_fault_fails_its_step_and_the_failure_comes_back_typed() {
    let case = Case::named("scripted-fault");
    let file = case.at("file");
    case.observed
        .add_fault(Fault {
            operation: Operation::WriteFile,
            step: Step::Write,
            occurrence: 1,
            action: FaultAction::Fail {
                system_code: ENOSPC,
            },
        })
        .expect("the fault is scripted");

    let failed = case
        .engine()
        .write_file(&Location::At(&file), b"content", &WRITTEN_EXACTLY_0644)
        .expect_err("the scripted write fails");
    let writes = case
        .observed
        .step_count(Operation::WriteFile, Step::Write)
        .expect("the count is read");

    assert_eq!(failed.kind, ErrorKind::NoSpace);
    assert_eq!(failed.operation, Operation::WriteFile);
    assert_eq!(failed.step, Some(Step::Write));
    assert_eq!(failed.system_code, ENOSPC);
    assert!(
        failed.path.starts_with(case.directory.path_of("file")),
        "{failed}"
    );
    assert_eq!(writes, 1);
    assert!(
        !case.directory.exists_raw("file"),
        "the destination is untouched"
    );
}

// The library's reading of a path whose way is blocked by a file: absence, for the removals as for `status`
// (`lstat`'s `ENOTDIR` read as "nothing there"). std answers an error instead, which is why the vendored
// bc-mutants compositions bridge it; this case pins the rule they bridge.
#[test]
fn a_path_under_a_file_is_absent_to_the_removals_and_to_status() {
    let case = Case::named("under-a-file");
    case.directory
        .write_raw("file", b"held", 0o644)
        .expect("setup");
    let under = case.at("file/tree");

    let tree_removed = case
        .engine()
        .remove_tree(&Location::At(&under))
        .expect("absence is an answer");
    let file_removed = case
        .engine()
        .remove_file(&Location::At(&under))
        .expect("absence is an answer");
    let status = case
        .engine()
        .status(&Location::At(&under), FinalSymlink::Refuse)
        .expect("absence is an answer");

    assert!(!tree_removed);
    assert!(!file_removed);
    assert_eq!(status, None);
}

// A location beneath a directory reaches the core with the directory's handle and its relative bytes, and an
// error beneath it names the directory's remembered text, a slash and the relative path.
#[test]
fn a_location_beneath_a_directory_reaches_c_and_names_its_path_from_the_directory() {
    let case = Case::named("beneath");
    case.directory
        .make_directory_raw("folder", 0o755)
        .expect("setup");
    let folder = case.at("folder");
    let directory = case
        .engine()
        .open_directory(&Location::At(&folder), FinalSymlink::Refuse)
        .expect("the directory opens");
    let note = relative("note");
    let absent = relative("absent");

    case.engine()
        .write_file(
            &Location::Beneath(&directory, &note),
            b"beneath",
            &WRITTEN_EXACTLY_0644,
        )
        .expect("the file is written beneath the directory");
    let missing = case
        .engine()
        .read_file(
            &Location::Beneath(&directory, &absent),
            &WHOLE_FILE_REFUSING_SYMLINK,
        )
        .expect_err("nothing is there");

    assert_eq!(
        case.directory.read_raw("folder/note").expect("raw read"),
        b"beneath"
    );
    assert_eq!(missing.kind, ErrorKind::NotFound);
    assert_eq!(missing.path, case.directory.path_of("folder/absent"));
}

// ---------------------------------------------------------------------------------------------------------------
// The face's compositions
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn read_file_if_present_answers_none_for_nothing_there() {
    let case = Case::named("if-present-absent");
    let absent = case.at("absent");

    let bytes = case
        .engine()
        .read_file_if_present(&Location::At(&absent), &WHOLE_FILE_REFUSING_SYMLINK)
        .expect("absence is an answer");

    assert_eq!(bytes, None);
}

// Absence becomes `None`; every other error passes through as received.
#[test]
fn read_file_if_present_passes_an_error_other_than_not_found_through() {
    let case = Case::named("if-present-error");
    case.directory
        .make_directory_raw("folder", 0o755)
        .expect("setup");
    let folder = case.at("folder");

    let failed = case
        .engine()
        .read_file_if_present(&Location::At(&folder), &WHOLE_FILE_REFUSING_SYMLINK)
        .expect_err("a directory is not read as a file");

    assert_eq!(failed.kind, ErrorKind::NotRegularFile);
    assert_eq!(failed.operation, Operation::ReadFile);
    assert_eq!(failed.path, case.directory.path_of("folder"));
}

#[test]
fn read_file_grows_its_buffer_to_a_file_larger_than_the_first_capacity() {
    // The face offers 1 MiB first; the core names the capacity it needs and the face asks once more.
    let case = Case::named("grow-read");
    let large = vec![b'g'; (1 << 20) + 100 * 1024];
    case.directory
        .write_raw("large.bin", &large, 0o644)
        .expect("setup");
    let file = case.at("large.bin");

    let bytes = case
        .engine()
        .read_file(&Location::At(&file), &WHOLE_FILE_REFUSING_SYMLINK)
        .expect("the large file is read");

    assert_eq!(bytes, large);
}

// The face's buffer is never initialised before the core writes it, so an answer shorter than the buffer it was
// read into must hold the file's own bytes and nothing of what the buffer held before: a large read first leaves
// its bytes where the allocator may hand the next buffer out again, and the short file read next is answered as
// exactly its bytes, its length its own and its capacity nowhere near the first offer's, under either limit.
#[test]
fn read_file_of_a_file_shorter_than_the_first_offer_answers_only_its_own_bytes() {
    let case = Case::named("short-read");
    let large = vec![b'L'; 900 * 1024];
    case.directory
        .write_raw("large.bin", &large, 0o644)
        .expect("setup");
    case.directory
        .write_raw("short.txt", b"seven b", 0o644)
        .expect("setup");
    let large_file = case.at("large.bin");
    let short_file = case.at("short.txt");
    let at_most_a_mebibyte = ReadOptions {
        limit: ReadLimit::AtMost(1 << 20),
        final_symlink: FinalSymlink::Refuse,
    };

    let first = case
        .engine()
        .read_file(&Location::At(&large_file), &WHOLE_FILE_REFUSING_SYMLINK)
        .expect("the large file is read");
    let whole = case
        .engine()
        .read_file(&Location::At(&short_file), &WHOLE_FILE_REFUSING_SYMLINK)
        .expect("the short file is read");
    let bounded = case
        .engine()
        .read_file(&Location::At(&short_file), &at_most_a_mebibyte)
        .expect("the short file is read under a bound");

    assert_eq!(first.len(), large.len());
    assert_eq!(whole, b"seven b");
    assert!(
        whole.capacity() < 1024,
        "the answer was shrunk to its own size"
    );
    assert_eq!(bounded, b"seven b");
    assert!(
        bounded.capacity() < 1024,
        "the answer was shrunk to its own size"
    );
}

#[test]
fn list_directory_grows_its_array_past_the_first_capacity() {
    // The face offers room for 1,024 entries first.
    let case = Case::named("grow-list");
    case.directory
        .make_directory_raw("many", 0o755)
        .expect("setup");
    let mut expected = Vec::new();
    for number in 1000..2100 {
        let name = format!("entry-{number}");
        case.directory
            .write_raw(&format!("many/{name}"), b"", 0o644)
            .expect("setup");
        expected.push(OsString::from(name));
    }
    let many = case.at("many");

    let entries = case
        .engine()
        .list_directory(&Location::At(&many), FinalSymlink::Refuse)
        .expect("the directory is listed");

    let listed: Vec<OsString> = entries.into_iter().map(|entry| entry.name).collect();
    assert_eq!(listed, expected);
}

#[test]
fn lines_split_on_newline_only_and_leave_every_other_byte_alone() {
    let found: Vec<&[u8]> = lines(b"one\ntwo\r\nthree").collect();

    assert_eq!(found, [&b"one"[..], b"two\r", b"three"]);
}

#[test]
fn lines_with_a_trailing_newline_add_no_empty_line() {
    let with_newline: Vec<&[u8]> = lines(b"one\ntwo\n").collect();
    let without_newline: Vec<&[u8]> = lines(b"one\ntwo").collect();

    assert_eq!(with_newline, [&b"one"[..], b"two"]);
    assert_eq!(without_newline, with_newline);
}

#[test]
fn lines_keep_an_empty_line_in_the_middle() {
    let found: Vec<&[u8]> = lines(b"one\n\nthree").collect();

    assert_eq!(found, [&b"one"[..], b"", b"three"]);
}

#[test]
fn lines_of_an_empty_buffer_are_none() {
    assert_eq!(lines(b"").count(), 0);
}

#[test]
fn each_line_is_a_view_into_the_buffer() {
    let buffer = b"one\ntwo".to_vec();

    let found: Vec<&[u8]> = lines(&buffer).collect();

    assert_eq!(found.len(), 2);
    assert!(core::ptr::eq(found[0].as_ptr(), buffer.as_ptr()));
    assert!(core::ptr::eq(found[1].as_ptr(), buffer[4..].as_ptr()));
}

// ---------------------------------------------------------------------------------------------------------------
// The error mapping is total: each C value names one variant, and each variant has its header name
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn every_error_kind_has_its_name_and_its_c_value() {
    let names = [
        "not_found",
        "not_regular_file",
        "not_directory",
        "permission_denied",
        "already_exists",
        "directory_not_empty",
        "symlink_refused",
        "too_large",
        "cross_device",
        "no_space",
        "resource_exhausted",
        "unsupported",
        "invalid_argument",
        "io_failed",
        "buffer_too_small",
    ];

    for (value, name) in (1..).zip(names) {
        let kind = ErrorKind::try_from(value).expect("a kind for every value");
        assert_eq!(kind.name(), name);
        assert_eq!(i32::from(kind), value);
    }
    assert_eq!(ErrorKind::ALL.len(), names.len());
    assert!(ErrorKind::try_from(0).is_err());
    assert!(ErrorKind::try_from(16).is_err());
}

#[test]
fn every_operation_has_its_name_and_its_c_value() {
    let names = [
        "engine_create_live",
        "engine_create_observed_for_tests",
        "open_directory",
        "directory_opened_as",
        "observed_fault_add",
        "observed_faults_clear",
        "observed_step_count",
        "status",
        "read_file",
        "write_file",
        "append",
        "sync",
        "create_directory",
        "create_directories",
        "list_directory",
        "remove_file",
        "remove_directory",
        "remove_tree",
        "create_temporary_directory",
        "rename",
        "copy_file",
        "set_permissions",
        "create_symlink",
        "read_symlink",
        "try_lock",
        "condemn",
        "remove_condemned",
        "relative_path_from",
        "absolute_path_from",
        "path_join",
        "path_parent",
        "path_last_component",
        "path_next_component",
        "path_stem",
        "path_extension",
        "path_relate",
        "path_lexically_normalize",
        "path_resolve_against",
        "path_library_name",
        "engine_create_portable_only_for_tests",
    ];

    for (value, name) in (1..).zip(names) {
        let operation = Operation::try_from(value).expect("an operation for every value");
        assert_eq!(operation.name(), name);
        assert_eq!(i32::from(operation), value);
    }
    assert_eq!(Operation::ALL.len(), names.len());
    assert!(Operation::try_from(0).is_err());
    assert!(Operation::try_from(41).is_err());
}

#[test]
fn every_step_has_its_name_and_its_c_value() {
    let names = [
        "open",
        "openat",
        "fstat",
        "fstatat",
        "read",
        "pread",
        "write",
        "fsync",
        "fcntl_fullfsync",
        "renameat",
        "rename_no_replace",
        "unlinkat",
        "mkdirat",
        "fdopendir",
        "readdir",
        "symlinkat",
        "readlinkat",
        "flock",
        "fchmod",
        "copy_range",
        "clone_file",
        "close",
        "identity_check",
        "openat2",
        "openat_nofollow_any",
        "fchmodat",
        "faccessat",
    ];

    for (value, name) in (1..).zip(names) {
        let step = Step::try_from(value).expect("a step for every value");
        assert_eq!(step.name(), name);
        assert_eq!(i32::from(step), value);
    }
    assert_eq!(Step::ALL.len(), names.len());
    assert!(Step::try_from(0).is_err());
    assert!(Step::try_from(28).is_err());
}

#[test]
fn the_failed_location_is_one_or_two_and_nothing_else() {
    assert_eq!(
        FailedLocation::ALL,
        [FailedLocation::First, FailedLocation::Second]
    );
    assert_eq!(i32::from(FailedLocation::First), 1);
    assert_eq!(i32::from(FailedLocation::Second), 2);
    assert!(FailedLocation::try_from(0).is_err(), "0 blames neither end");
    assert!(FailedLocation::try_from(3).is_err());
    assert!(FailedLocation::try_from(-1).is_err());
}

#[test]
fn every_small_enum_is_pinned_to_the_header_and_refuses_a_value_outside_it() {
    assert_eq!(FinalSymlink::Refuse as i32, 1);
    assert_eq!(FinalSymlink::Follow as i32, 2);
    assert_eq!(ExistingDestination::Replace as i32, 1);
    assert_eq!(ExistingDestination::Refuse as i32, 2);
    assert_eq!(PermissionsPolicy::Exactly as i32, 1);
    assert_eq!(PermissionsPolicy::PreserveOr as i32, 2);
    assert_eq!(Durability::try_from(1), Ok(Durability::Written));
    assert_eq!(Durability::try_from(2), Ok(Durability::Durable));
    assert!(Durability::try_from(0).is_err());
    assert!(Durability::try_from(3).is_err());
    assert_eq!(EntryType::try_from(4), Ok(EntryType::Other));
    assert!(EntryType::try_from(0).is_err());
    assert!(EntryType::try_from(5).is_err());
    assert_eq!((ffi::LOCATION_KIND_AT, ffi::LOCATION_KIND_BENEATH), (1, 2));
    assert_eq!(
        (
            ffi::READ_LIMIT_KIND_WHOLE_FILE,
            ffi::READ_LIMIT_KIND_AT_MOST
        ),
        (1, 2)
    );
    assert_eq!(
        (ffi::LOCK_OUTCOME_ACQUIRED, ffi::LOCK_OUTCOME_HELD_ELSEWHERE),
        (1, 2)
    );
    assert_eq!((ffi::FAULT_ACTION_FAIL, ffi::FAULT_ACTION_STOP), (1, 2));
    assert_eq!(ffi::LIBRARY_KIND_NONE, 1);
}

// ---------------------------------------------------------------------------------------------------------------
// The error struct crosses once, in `Error::from_c`, and renders in `Display`
// ---------------------------------------------------------------------------------------------------------------

fn a_c_error(
    kind: ErrorKind,
    operation: Operation,
    step: i32,
    system_code: i32,
    name: &[u8],
) -> ffi::bc_file_system_error {
    let mut c_error = ffi::bc_file_system_error::zeroed();
    c_error.kind = i32::from(kind);
    c_error.operation = i32::from(operation);
    c_error.step = step;
    c_error.system_code = system_code;
    c_error.name[..name.len()].copy_from_slice(name);
    c_error.name_length = u32::try_from(name.len()).expect("a name fits the field");
    c_error
}

#[test]
fn the_path_is_composed_from_the_location_and_the_entry_name() {
    let c_error = a_c_error(
        ErrorKind::NotFound,
        Operation::ReadFile,
        i32::from(Step::Openat),
        ENOENT,
        b"child",
    );

    let error = Error::from_c(&c_error, Operation::ReadFile, Path::new("/a/b"));

    assert_eq!(error.kind, ErrorKind::NotFound);
    assert_eq!(error.operation, Operation::ReadFile);
    assert_eq!(error.step, Some(Step::Openat));
    assert_eq!(error.system_code, ENOENT);
    assert_eq!(error.path, Path::new("/a/b/child"));
    assert!(!error.name_truncated);
}

#[test]
fn the_path_is_the_location_alone_when_the_step_acted_on_it() {
    let c_error = a_c_error(
        ErrorKind::PermissionDenied,
        Operation::Status,
        i32::from(Step::Fstatat),
        EACCES,
        b"",
    );

    let error = Error::from_c(&c_error, Operation::Status, Path::new("/a/b"));

    assert_eq!(error.path, Path::new("/a/b"));
}

#[test]
fn the_path_is_the_name_alone_when_the_call_took_no_location() {
    let c_error = a_c_error(
        ErrorKind::IoFailed,
        Operation::EngineCreateLive,
        0,
        EIO,
        b"probe",
    );

    let error = Error::from_c(&c_error, Operation::EngineCreateLive, Path::new(""));

    assert_eq!(error.path, Path::new("probe"));
    assert_eq!(error.step, None);
}

// The two-location conversion composes the path from the end the core names: the first, the second, or the
// second when it names neither.
#[test]
fn the_failed_location_chooses_the_path_of_a_two_location_error() {
    let mut at_the_source = a_c_error(
        ErrorKind::NotFound,
        Operation::Rename,
        i32::from(Step::Fstatat),
        ENOENT,
        b"",
    );
    at_the_source.failed_location = 1;
    let mut at_the_destination = a_c_error(
        ErrorKind::AlreadyExists,
        Operation::Rename,
        i32::from(Step::RenameNoReplace),
        EEXIST,
        b"",
    );
    at_the_destination.failed_location = 2;
    let at_neither = a_c_error(ErrorKind::InvalidArgument, Operation::Rename, 0, 0, b"");
    let from = Path::new("/from");
    let to = Path::new("/to");

    let source = Error::from_c_two_locations(&at_the_source, Operation::Rename, from, to);
    let destination = Error::from_c_two_locations(&at_the_destination, Operation::Rename, from, to);
    let neither = Error::from_c_two_locations(&at_neither, Operation::Rename, from, to);
    let one_location = Error::from_c(&at_neither, Operation::Rename, Path::new("/one"));

    assert_eq!(source.failed_location, Some(FailedLocation::First));
    assert_eq!(source.path, from);
    assert_eq!(destination.failed_location, Some(FailedLocation::Second));
    assert_eq!(destination.path, to);
    assert_eq!(neither.failed_location, None);
    assert_eq!(neither.path, to);
    assert_eq!(one_location.failed_location, None);
    assert_eq!(one_location.path, Path::new("/one"));
}

// The core's name field is bounded; a longer name arrives cut short and flagged, never silently.
#[test]
fn a_name_longer_than_the_field_arrives_cut_short_and_flagged() {
    let long_name = [b'n'; 256];
    let mut c_error = a_c_error(
        ErrorKind::NotFound,
        Operation::Status,
        i32::from(Step::Fstatat),
        ENOENT,
        &long_name,
    );
    c_error.name_truncated = 1;

    let error = Error::from_c(&c_error, Operation::Status, Path::new("/a"));

    let mut expected = b"/a/".to_vec();
    expected.extend_from_slice(&long_name);
    assert_eq!(error.path.as_os_str().len(), expected.len());
    assert!(error.name_truncated);
    assert!(error.to_string().contains("…: not_found"), "{error}");
}

#[test]
fn the_message_names_the_operation_the_path_the_kind_and_the_step() {
    let failed = Error::from(ErrorDetails {
        kind: ErrorKind::NotFound,
        operation: Operation::ReadFile,
        step: Some(Step::Openat),
        system_code: ENOENT,
        path: PathBuf::from("/etc/config.toml"),
        name_truncated: false,
        failed_location: None,
        unknown_value: None,
    });
    let refused = Error::from(ErrorDetails {
        kind: ErrorKind::InvalidArgument,
        operation: Operation::WriteFile,
        step: None,
        system_code: 0,
        path: PathBuf::from("relative.txt"),
        name_truncated: false,
        failed_location: None,
        unknown_value: None,
    });
    let without_location = Error::from(ErrorDetails {
        kind: ErrorKind::ResourceExhausted,
        operation: Operation::EngineCreateLive,
        step: None,
        system_code: 0,
        path: PathBuf::new(),
        name_truncated: false,
        failed_location: None,
        unknown_value: None,
    });

    let system_text = std::io::Error::from_raw_os_error(ENOENT).to_string();
    assert_eq!(
        failed.to_string(),
        format!("read_file: /etc/config.toml: not_found at openat: {system_text}")
    );
    assert_eq!(
        refused.to_string(),
        "write_file: relative.txt: invalid_argument"
    );
    assert_eq!(
        without_location.to_string(),
        "engine_create_live: resource_exhausted"
    );
}

// ---------------------------------------------------------------------------------------------------------------
// One ABI round trip per value type
// ---------------------------------------------------------------------------------------------------------------

#[test]
fn a_read_limit_crosses_as_its_kind_and_its_bound() {
    let whole = ReadLimit::WholeFile.to_c();
    let bounded = ReadLimit::AtMost(512).to_c();

    assert_eq!(
        (whole.kind, whole.at_most),
        (ffi::READ_LIMIT_KIND_WHOLE_FILE, 0)
    );
    assert_eq!(
        (bounded.kind, bounded.at_most),
        (ffi::READ_LIMIT_KIND_AT_MOST, 512)
    );
}

#[test]
fn permissions_cross_as_their_policy_and_their_mode() {
    let exactly = Permissions::exactly(0o640);
    let preserving = Permissions::preserve_or(0o600);

    assert_eq!(
        (exactly.policy(), exactly.mode()),
        (PermissionsPolicy::Exactly, 0o640)
    );
    assert_eq!((exactly.to_c().policy, exactly.to_c().mode), (1, 0o640));
    assert_eq!(
        (preserving.to_c().policy, preserving.to_c().mode),
        (2, 0o600)
    );
}

#[test]
fn write_options_cross_as_four_fields() {
    let c_options = DURABLE_PRESERVING_OR_0600.to_c();

    assert_eq!(c_options.durability, Durability::Durable as i32);
    assert_eq!(
        c_options.permissions.policy,
        PermissionsPolicy::PreserveOr as i32
    );
    assert_eq!(c_options.permissions.mode, 0o600);
    assert_eq!(
        c_options.existing_destination,
        ExistingDestination::Refuse as i32
    );
    assert_eq!(c_options.final_symlink, FinalSymlink::Follow as i32);
}

#[test]
fn read_options_cross_as_their_limit_and_their_final_symlink() {
    let c_options = ReadOptions {
        limit: ReadLimit::AtMost(9),
        final_symlink: FinalSymlink::Follow,
    }
    .to_c();

    assert_eq!(c_options.limit.kind, ffi::READ_LIMIT_KIND_AT_MOST);
    assert_eq!(c_options.limit.at_most, 9);
    assert_eq!(c_options.final_symlink, FinalSymlink::Follow as i32);
}

#[test]
fn a_status_crosses_and_comes_back_equal_and_one_outside_the_enum_is_refused() {
    let status = Status {
        entry_type: EntryType::RegularFile,
        size: 1234,
        mode: 0o640,
        device: 16_777_220,
        inode: 98_765_432_101,
        link_count: 2,
    };
    let mut outside = status.to_c();
    outside.entry_type = 0;

    let c_status = status.to_c();
    let back = Status::try_from(&c_status);

    assert_eq!(c_status.entry_type, 1);
    assert_eq!(c_status.size, 1234);
    assert_eq!(c_status.link_count, 2);
    assert_eq!(back, Ok(status));
    assert_eq!(
        Status::try_from(&outside),
        Err(UnknownValue {
            enumeration: "bc_file_system_entry_type",
            value: 0
        })
    );
}

#[test]
fn an_entry_crosses_as_its_type_and_its_name_bytes_and_a_bad_one_is_refused() {
    let mut c_entry = ffi::bc_file_system_entry::zeroed();
    c_entry.entry_type = EntryType::Symlink as i32;
    c_entry.name[..9].copy_from_slice(b"notes.txt");
    c_entry.name_length = 9;
    let mut bad_type = c_entry;
    bad_type.entry_type = 7;
    let mut too_long = c_entry;
    too_long.name_length = 257;

    assert_eq!(
        Entry::try_from(&c_entry),
        Ok(Entry {
            entry_type: EntryType::Symlink,
            name: OsString::from("notes.txt"),
        })
    );
    assert!(Entry::try_from(&bad_type).is_err());
    assert!(Entry::try_from(&too_long).is_err());
}

#[test]
fn a_fault_crosses_and_comes_back_equal_and_one_outside_its_enum_is_refused() {
    let fault = Fault {
        operation: Operation::WriteFile,
        step: Step::FcntlFullfsync,
        occurrence: 2,
        action: FaultAction::Fail { system_code: EIO },
    };
    let stop = Fault {
        operation: Operation::Rename,
        step: Step::Renameat,
        occurrence: 1,
        action: FaultAction::Stop,
    };
    let mut outside = fault.to_c();
    outside.action = 0;

    let c_fault = fault.to_c();

    assert_eq!(
        (
            c_fault.operation,
            c_fault.step,
            c_fault.occurrence,
            c_fault.action,
            c_fault.system_code
        ),
        (10, 9, 2, ffi::FAULT_ACTION_FAIL, EIO)
    );
    assert_eq!(Fault::from_c(&c_fault), Ok(fault));
    assert_eq!(Fault::from_c(&stop.to_c()), Ok(stop));
    assert_eq!(stop.to_c().system_code, 0);
    assert!(Fault::from_c(&outside).is_err());
}

#[test]
fn a_location_at_crosses_as_its_kind_and_its_borrowed_bytes() {
    let path = AbsolutePath::from_bytes(b"/etc/config.toml").expect("an absolute path");
    let location = Location::At(&path);

    let c_location = location.to_c();

    assert_eq!(c_location.kind, ffi::LOCATION_KIND_AT);
    assert!(c_location.directory.is_null());
    assert!(core::ptr::eq(c_location.path, path.as_bytes().as_ptr()));
    assert_eq!(c_location.length, path.as_bytes().len());
    assert_eq!(location.text(), Path::new("/etc/config.toml"));
}

#[test]
fn a_location_beneath_crosses_with_its_directory_and_its_borrowed_bytes() {
    let case = Case::named("location-beneath");
    let root = case.root();
    let directory = case
        .engine()
        .open_directory(&Location::At(&root), FinalSymlink::Refuse)
        .expect("the directory opens");
    let note = relative("notes/today.txt");
    let location = Location::Beneath(&directory, &note);

    let c_location = location.to_c();

    assert_eq!(c_location.kind, ffi::LOCATION_KIND_BENEATH);
    assert!(!c_location.directory.is_null());
    assert!(core::ptr::eq(c_location.path, note.as_bytes().as_ptr()));
    assert_eq!(c_location.length, note.as_bytes().len());
    assert_eq!(
        location.text(),
        case.directory.path().join("notes/today.txt")
    );
}
