//! The hand-run benchmark of the Rust face against `std::fs` (design doc, "Benchmark").
//!
//! Each row times one operation on the crate and on the `std::fs` forms a consumer uses today, and the program
//! prints one Markdown table. A comparison row is the form that promises the least ("less: …") and, where that
//! promises less than the crate, a second form that hand-rolls the same guarantee ("same"); the crate's live row
//! carries its median over the "same" row's, which is how "the crate must never be slower than std" is judged,
//! row by row. The crate's rows also carry the system calls one run made, read from an observed engine's step
//! counts. `std::fs` cannot open beneath a held directory, so the `beneath` rows have no "same" row here; the
//! C++ program's POSIX rows are that comparison for the one core both faces call.
//!
//! This program is a composition root, and the whole of this file is the guard's legal reach site for it
//! (AGENTS.md, "Inject, never reach"): `main` names the live values (the three engines, the clock, the two
//! streams, the command line) and hands them down, and the comparison rows call `std::fs` directly because those
//! calls are what the benchmark measures. It writes only inside one directory it makes under the temporary root
//! it is handed, and removes that directory when it ends. It is the `std_comparison` bench target of
//! `bc-file-system` (release optimisation, the observed engine from the dev-dependency), run by hand, never by
//! CI or `scripts/verify.sh`:
//!
//! ```text
//! caffeinate -i cargo bench --manifest-path crates/Cargo.toml -p bc-file-system --bench std_comparison -- \
//!     <temporary-root> <warm-up> <iterations> <large-warm-up> <large-iterations> \
//!     <small-bytes> <medium-bytes> <large-bytes> <entry-count> <chain-depth> <depth>...
//! ```

use std::cell::Cell;
use std::ffi::OsString;
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::rc::Rc;
use std::time::Instant;

use bc_file_system::{
    AbsolutePath, Directory, Durability, Engine, Error, ExistingDestination, FinalSymlink,
    Location, LockOutcome, Operation, Permissions, ReadLimit, ReadOptions, RelativePath, Step,
    WriteOptions,
};
use bc_file_system_testing::ObservedEngine;

// ---- What the command line states -----------------------------------------------------------------------------

/// How many times a row's action runs: first untimed, to warm the caches and the allocator, then timed.
#[derive(Clone, Copy)]
struct Repetitions {
    warm_up: usize,
    timed: usize,
}

struct Parameters {
    temporary_root: PathBuf,
    repetitions: Repetitions,
    /// For the copies of the large file, each of which moves the whole file.
    large_repetitions: Repetitions,
    small_size: usize,
    medium_size: usize,
    large_size: usize,
    entry_count: usize,
    chain_depth: usize,
    status_depths: Vec<usize>,
}

const USAGE: &str = "usage: std_comparison <temporary-root> <warm-up> <iterations> <large-warm-up> \
<large-iterations> <small-bytes> <medium-bytes> <large-bytes> <entry-count> <chain-depth> <depth>...\n  every \
count and size is a positive integer; <temporary-root> is an absolute path to an existing directory\n";

/// The count the text spells, or nothing for anything but a positive decimal integer.
fn positive_count(text: &OsString) -> Option<usize> {
    let count: usize = text.to_str()?.parse().ok()?;
    (count > 0).then_some(count)
}

/// Every argument is required: a missing one is a usage error, never a value chosen here.
fn parameters_from(arguments: &[OsString]) -> Option<Parameters> {
    const FIXED_ARGUMENTS: usize = 10;
    if arguments.len() < FIXED_ARGUMENTS + 1 {
        return None;
    }
    let count = |index: usize| positive_count(&arguments[index]);
    let temporary_root = PathBuf::from(&arguments[0]);
    if !temporary_root.is_absolute() {
        return None;
    }
    let status_depths = arguments[FIXED_ARGUMENTS..]
        .iter()
        .map(positive_count)
        .collect::<Option<Vec<usize>>>()?;
    Some(Parameters {
        temporary_root,
        repetitions: Repetitions {
            warm_up: count(1)?,
            timed: count(2)?,
        },
        large_repetitions: Repetitions {
            warm_up: count(3)?,
            timed: count(4)?,
        },
        small_size: count(5)?,
        medium_size: count(6)?,
        large_size: count(7)?,
        entry_count: count(8)?,
        chain_depth: count(9)?,
        status_depths,
    })
}

// ---- Timing and the table ---------------------------------------------------------------------------------------

/// The middle sample and the 90th-percentile sample of one row's timed runs, in nanoseconds.
#[derive(Clone, Copy)]
struct Summary {
    median_nanoseconds: u128,
    p90_nanoseconds: u128,
}

/// Runs `prepare` and then `action`, `warm_up` times, then `timed` times with only `action` between two readings
/// of `now`. Answers the summary of the timed samples, or nothing as soon as `prepare` or `action` fails, because
/// the numbers of a failing row would time the failure.
fn measure(
    now: fn() -> Instant,
    repetitions: Repetitions,
    prepare: &mut dyn FnMut() -> bool,
    action: &mut dyn FnMut() -> bool,
) -> Option<Summary> {
    for _ in 0..repetitions.warm_up {
        if !prepare() || !action() {
            return None;
        }
    }
    let mut samples = Vec::with_capacity(repetitions.timed);
    for _ in 0..repetitions.timed {
        if !prepare() {
            return None;
        }
        let started = now();
        let succeeded = action();
        let finished = now();
        if !succeeded {
            return None;
        }
        samples.push(finished.duration_since(started).as_nanos());
    }
    samples.sort_unstable();
    // Both answers are samples that were taken, never interpolated: the median is the middle one (the upper of
    // the two middle ones for an even count), and the 90th percentile is the nearest rank, ceil(0.9 × count).
    let p90_rank = (samples.len() * 9).div_ceil(10);
    Some(Summary {
        median_nanoseconds: *samples.get(samples.len() / 2)?,
        p90_nanoseconds: *samples.get(p90_rank.checked_sub(1)?)?,
    })
}

/// `hundredths` as "<whole>.<two digits>", the shape both columns of numbers share.
fn two_decimals(hundredths: u128) -> String {
    format!("{}.{:02}", hundredths / 100, hundredths % 100)
}

/// Nanoseconds as microseconds with two decimals; a hundredth of a microsecond is ten nanoseconds, and adding
/// five rounds to the nearest one.
fn microseconds_text(nanoseconds: u128) -> String {
    two_decimals((nanoseconds + 5) / 10)
}

fn ratio_text(numerator: u128, denominator: u128) -> String {
    if denominator == 0 {
        return String::new();
    }
    format!(
        "{}×",
        two_decimals((numerator * 100 + denominator / 2) / denominator)
    )
}

/// One line of the printed table; the comparison rows leave the calls and the steps empty.
struct TableRow {
    operation: String,
    case_label: String,
    form: String,
    /// "library" for the crate's own rows; "same" or "less: <what it does not promise>" for a comparison row.
    guarantee: String,
    summary: Summary,
    /// The crate's live row only: its median over the median of the comparison row with the same guarantee.
    versus_same: String,
    calls: String,
    steps: String,
}

fn print_table(output: &mut dyn Write, rows: &[TableRow]) -> io::Result<()> {
    writeln!(
        output,
        "| operation | case | form | guarantee | median µs | p90 µs | vs same | calls | steps |"
    )?;
    writeln!(output, "|---|---|---|---|---:|---:|---:|---:|---|")?;
    for row in rows {
        writeln!(
            output,
            "| {} | {} | {} | {} | {} | {} | {} | {} | {} |",
            row.operation,
            row.case_label,
            row.form,
            row.guarantee,
            microseconds_text(row.summary.median_nanoseconds),
            microseconds_text(row.summary.p90_nanoseconds),
            row.versus_same,
            row.calls,
            row.steps
        )?;
    }
    Ok(())
}

// ---- The engines and what is held open through each -----------------------------------------------------------

struct Engines {
    live: Engine,
    observed: ObservedEngine,
    portable_only: ObservedEngine,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum EngineKind {
    Live,
    Observed,
    PortableOnly,
}

/// One engine and the directories every `beneath` location of a row is relative to, each opened through it:
/// `files` holds what the reads, writes, copies, the listing, the chain and the lock act on; `tree` the chain the
/// status rows descend; `rename_a` and `rename_b` the two directories the renamed file moves between.
struct Side<'engine> {
    engine: &'engine Engine,
    files: Directory<'engine>,
    tree: Directory<'engine>,
    rename_a: Directory<'engine>,
    rename_b: Directory<'engine>,
}

struct Sides<'engine> {
    live: Side<'engine>,
    observed: Side<'engine>,
    portable_only: Side<'engine>,
    engines: &'engine Engines,
}

impl Sides<'_> {
    fn of(&self, kind: EngineKind) -> &Side<'_> {
        match kind {
            EngineKind::Live => &self.live,
            EngineKind::Observed => &self.observed,
            EngineKind::PortableOnly => &self.portable_only,
        }
    }

    /// The engine whose step counts say what a row timed on `kind` did: the observed engine for a live row, the
    /// row's own engine otherwise.
    fn counted(&self, kind: EngineKind) -> (&Side<'_>, &ObservedEngine) {
        match kind {
            EngineKind::Live | EngineKind::Observed => (&self.observed, &self.engines.observed),
            EngineKind::PortableOnly => (&self.portable_only, &self.engines.portable_only),
        }
    }
}

struct WorkPaths {
    work: PathBuf,
    files: PathBuf,
    tree: PathBuf,
    rename_a: PathBuf,
    rename_b: PathBuf,
}

fn open_side<'engine>(engine: &'engine Engine, paths: &WorkPaths) -> Result<Side<'engine>, Error> {
    let open = |path: &Path| -> Result<Directory<'engine>, Error> {
        let absolute = AbsolutePath::from_path(path)?;
        engine.open_directory(&Location::At(&absolute), FinalSymlink::Refuse)
    };
    Ok(Side {
        engine,
        files: open(&paths.files)?,
        tree: open(&paths.tree)?,
        rename_a: open(&paths.rename_a)?,
        rename_b: open(&paths.rename_b)?,
    })
}

/// The directories the `std::fs` rows sync, held open as the library holds its own.
struct HeldDirectories {
    files: Rc<File>,
    rename_a: Rc<File>,
    rename_b: Rc<File>,
}

// ---- Rows and groups --------------------------------------------------------------------------------------------

type Prepare = Box<dyn FnMut() -> bool>;
type SideAction = Box<dyn FnMut(&Side<'_>) -> bool>;
type StdAction = Box<dyn FnMut() -> bool>;

/// A row of the crate: the engine it is timed on and the operations whose steps it reaches.
struct LibraryRow {
    form: String,
    timed_on: EngineKind,
    operations: Vec<Operation>,
    prepare: Prepare,
    action: SideAction,
}

/// A row of `std::fs`: what it does not promise that the crate does ("less: …"), or "same".
struct ComparisonRow {
    form: String,
    guarantee: String,
    prepare: Prepare,
    action: StdAction,
}

/// One operation in one case: the crate's rows, the first of them the live engine's, and the comparison rows.
struct Group {
    operation: String,
    case_label: String,
    repetitions: Repetitions,
    library_rows: Vec<LibraryRow>,
    comparison_rows: Vec<ComparisonRow>,
}

fn nothing_to_prepare() -> Prepare {
    Box::new(|| true)
}

fn library_row(
    form: &str,
    timed_on: EngineKind,
    operations: &[Operation],
    prepare: Prepare,
    action: SideAction,
) -> LibraryRow {
    LibraryRow {
        form: form.to_owned(),
        timed_on,
        operations: operations.to_vec(),
        prepare,
        action,
    }
}

fn comparison_row(
    form: &str,
    guarantee: &str,
    prepare: Prepare,
    action: StdAction,
) -> ComparisonRow {
    ComparisonRow {
        form: form.to_owned(),
        guarantee: guarantee.to_owned(),
        prepare,
        action,
    }
}

const LIVE: &str = "library, live engine";
const OBSERVED: &str = "library, observed engine";

// ---- Counting the system calls ---------------------------------------------------------------------------------

/// Every step count of `operations`, summed over the operations, one entry per step in the header's order.
fn step_counts(observer: &ObservedEngine, operations: &[Operation]) -> Option<Vec<u64>> {
    Step::ALL
        .iter()
        .map(|&step| {
            operations
                .iter()
                .map(|&operation| observer.step_count(operation, step).ok())
                .sum::<Option<u64>>()
        })
        .collect()
}

/// The calls between two readings: every step but the identity comparison, which is a step of the observer's
/// but calls nothing; the breakdown names it all the same, so a reader sees the bracket.
fn call_count(before: &[u64], after: &[u64]) -> (String, String) {
    let mut calls = 0;
    let mut steps = Vec::new();
    for ((step, before), after) in Step::ALL.iter().zip(before).zip(after) {
        let reached = after - before;
        if reached == 0 {
            continue;
        }
        if *step != Step::IdentityCheck {
            calls += reached;
        }
        steps.push(format!("{} {reached}", step.name()));
    }
    (calls.to_string(), steps.join(", "))
}

/// One run of the row on the engine that observes it, between two readings of its step counts.
fn count_calls(sides: &Sides<'_>, row: &mut LibraryRow) -> Option<(String, String)> {
    let (side, observer) = sides.counted(row.timed_on);
    let before = step_counts(observer, &row.operations)?;
    if !(row.prepare)() || !(row.action)(side) {
        return None;
    }
    let after = step_counts(observer, &row.operations)?;
    Some(call_count(&before, &after))
}

/// Measures every row of `group` and appends its lines to `table`; answers the form that failed, if one did.
fn measure_group(
    now: fn() -> Instant,
    sides: &Sides<'_>,
    group: &mut Group,
    table: &mut Vec<TableRow>,
) -> Result<(), String> {
    let mut lines = Vec::new();
    for row in &mut group.library_rows {
        let (calls, steps) =
            count_calls(sides, row).ok_or_else(|| format!("{} (counting its calls)", row.form))?;
        let side = sides.of(row.timed_on);
        let form = row.form.clone();
        let LibraryRow {
            prepare, action, ..
        } = row;
        let summary = measure(now, group.repetitions, prepare, &mut || action(side))
            .ok_or_else(|| form.clone())?;
        lines.push(TableRow {
            operation: group.operation.clone(),
            case_label: group.case_label.clone(),
            form,
            guarantee: "library".to_owned(),
            summary,
            versus_same: String::new(),
            calls,
            steps,
        });
    }
    let mut same = None;
    for row in &mut group.comparison_rows {
        let summary = measure(now, group.repetitions, &mut row.prepare, &mut row.action)
            .ok_or_else(|| row.form.clone())?;
        if row.guarantee == "same" && same.is_none() {
            same = Some(summary);
        }
        lines.push(TableRow {
            operation: group.operation.clone(),
            case_label: group.case_label.clone(),
            form: row.form.clone(),
            guarantee: row.guarantee.clone(),
            summary,
            versus_same: String::new(),
            calls: String::new(),
            steps: String::new(),
        });
    }
    if let (Some(same), Some(first)) = (same, lines.first_mut()) {
        first.versus_same = ratio_text(first.summary.median_nanoseconds, same.median_nanoseconds);
    }
    table.extend(lines);
    Ok(())
}

// ---- The forms the crate is compared with -----------------------------------------------------------------------

fn is_regular(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|metadata| metadata.file_type().is_file())
}

/// What the library decides before it stages anything: the destination is absent or a regular file, never a
/// symlink, a directory or a FIFO, which a rename into place would otherwise replace.
fn destination_is_replaceable(destination: &Path) -> bool {
    match fs::symlink_metadata(destination) {
        Ok(metadata) => metadata.file_type().is_file(),
        Err(error) => error.kind() == io::ErrorKind::NotFound,
    }
}

/// A write in place: `File::create` truncates under `replace`, `File::create_new` refuses under `refuse`; under
/// `durable` the file is synced, and so is its directory when the name is new.
fn in_place_write(
    destination: &Path,
    bytes: &[u8],
    options: WriteOptions,
    directory: &File,
) -> io::Result<()> {
    let mut file = match options.existing_destination {
        ExistingDestination::Replace => File::create(destination)?,
        ExistingDestination::Refuse => File::create_new(destination)?,
    };
    file.write_all(bytes)?;
    if options.durability == Durability::Durable {
        file.sync_all()?;
    }
    drop(file);
    if options.durability == Durability::Durable
        && options.existing_destination == ExistingDestination::Refuse
    {
        directory.sync_all()?;
    }
    Ok(())
}

/// The library's staged write by `std::fs`: the destination judged, an exclusive temporary beside it, every
/// byte, the mode, under `durable` a full sync (`sync_all` is `F_FULLFSYNC` on Apple platforms), the close, then
/// one rename into place (or, refusing to replace, a hard link that fails on an existing name and the removal of
/// the temporary), then under `durable` the directory's sync.
fn staged_write(
    destination: &Path,
    temporary: &Path,
    bytes: &[u8],
    options: WriteOptions,
    directory: &File,
) -> io::Result<()> {
    if !destination_is_replaceable(destination) {
        return Err(io::Error::other("the destination is not a regular file"));
    }
    let mut file = File::create_new(temporary)?;
    file.write_all(bytes)?;
    file.set_permissions(fs::Permissions::from_mode(0o644))?;
    if options.durability == Durability::Durable {
        file.sync_all()?;
    }
    drop(file);
    match options.existing_destination {
        ExistingDestination::Replace => fs::rename(temporary, destination)?,
        ExistingDestination::Refuse => {
            let linked = fs::hard_link(temporary, destination);
            fs::remove_file(temporary)?;
            linked?;
        }
    }
    if options.durability == Durability::Durable {
        directory.sync_all()?;
    }
    Ok(())
}

/// The library's read by `std::fs`: the file opened, anything but a regular file refused by its metadata (one
/// `fstat`), then read to its end into a buffer of the measured size.
fn read_regular_file(path: &Path) -> io::Result<Vec<u8>> {
    let mut file = File::open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file() {
        return Err(io::Error::other("not a regular file"));
    }
    let mut bytes = Vec::with_capacity(usize::try_from(metadata.len()).map_err(io::Error::other)?);
    file.read_to_end(&mut bytes)?;
    Ok(bytes)
}

/// The library's copy by `std::fs`: the destination judged as a staged write judges it, the bytes copied to a
/// temporary beside it, the mode set exactly, then one rename into place.
fn staged_copy(source: &Path, destination: &Path, temporary: &Path) -> io::Result<()> {
    if !destination_is_replaceable(destination) {
        return Err(io::Error::other("the destination is not a regular file"));
    }
    fs::copy(source, temporary)?;
    fs::set_permissions(temporary, fs::Permissions::from_mode(0o644))?;
    fs::rename(temporary, destination)
}

/// A directory's entries as the library answers them: each name with its type, in name order or as read.
#[derive(Clone, Copy, PartialEq, Eq)]
enum ListingOrder {
    AsRead,
    ByName,
}

fn std_listing(path: &Path, order: ListingOrder, expected: usize) -> bool {
    let Ok(reader) = fs::read_dir(path) else {
        return false;
    };
    let mut entries = Vec::new();
    for entry in reader {
        let Ok(entry) = entry else {
            return false;
        };
        let Ok(file_type) = entry.file_type() else {
            return false;
        };
        entries.push((entry.file_name(), file_type));
    }
    if order == ListingOrder::ByName {
        entries.sort_by(|first, second| first.0.cmp(&second.0));
    }
    entries.len() == expected
}

// ---- The groups ------------------------------------------------------------------------------------------------

fn write_options(
    durability: Durability,
    existing_destination: ExistingDestination,
) -> WriteOptions {
    WriteOptions {
        durability,
        permissions: Permissions::exactly(0o644),
        existing_destination,
        final_symlink: FinalSymlink::Refuse,
    }
}

fn relative(text: &str) -> Result<RelativePath, Error> {
    RelativePath::from_path(Path::new(text))
}

fn size_label(bytes: usize) -> String {
    const KIBIBYTE: usize = 1024;
    const MEBIBYTE: usize = KIBIBYTE * 1024;
    if bytes % MEBIBYTE == 0 {
        format!("{} MiB", bytes / MEBIBYTE)
    } else if bytes % KIBIBYTE == 0 {
        format!("{} KiB", bytes / KIBIBYTE)
    } else {
        format!("{bytes} B")
    }
}

/// The relative path of the file at `depth` in the status tree: depth - 1 directories named `d`, then `f`.
fn tree_relative_text(depth: usize) -> String {
    let mut text = "d/".repeat(depth - 1);
    text.push('f');
    text
}

fn status_at(absolute: &AbsolutePath) -> SideAction {
    let absolute = absolute.clone();
    Box::new(move |side: &Side<'_>| {
        side.engine
            .status(&Location::At(&absolute), FinalSymlink::Refuse)
            .is_ok_and(|status| status.is_some())
    })
}

fn status_beneath(path: &RelativePath) -> SideAction {
    let path = path.clone();
    Box::new(move |side: &Side<'_>| {
        side.engine
            .status(&Location::Beneath(&side.tree, &path), FinalSymlink::Refuse)
            .is_ok_and(|status| status.is_some())
    })
}

fn std_status(absolute: &Path) -> StdAction {
    let absolute = absolute.to_path_buf();
    Box::new(move || is_regular(&absolute))
}

fn status_groups(parameters: &Parameters, paths: &WorkPaths) -> Result<Vec<Group>, Error> {
    let status = [Operation::Status];
    let mut groups = Vec::new();
    for &depth in &parameters.status_depths {
        let text = tree_relative_text(depth);
        let path = relative(&text)?;
        let joined = paths.tree.join(&text);
        let absolute = AbsolutePath::from_path(&joined)?;
        groups.push(Group {
            operation: "status_of".to_owned(),
            case_label: format!("at, depth {depth}"),
            repetitions: parameters.repetitions,
            library_rows: vec![
                library_row(
                    LIVE,
                    EngineKind::Live,
                    &status,
                    nothing_to_prepare(),
                    status_at(&absolute),
                ),
                library_row(
                    OBSERVED,
                    EngineKind::Observed,
                    &status,
                    nothing_to_prepare(),
                    status_at(&absolute),
                ),
            ],
            comparison_rows: vec![comparison_row(
                "`fs::symlink_metadata`",
                "same",
                nothing_to_prepare(),
                std_status(&joined),
            )],
        });
        groups.push(Group {
            operation: "status_of".to_owned(),
            case_label: format!("beneath, depth {depth}"),
            repetitions: parameters.repetitions,
            library_rows: vec![
                library_row(
                    LIVE,
                    EngineKind::Live,
                    &status,
                    nothing_to_prepare(),
                    status_beneath(&path),
                ),
                library_row(
                    OBSERVED,
                    EngineKind::Observed,
                    &status,
                    nothing_to_prepare(),
                    status_beneath(&path),
                ),
                library_row(
                    "library, portable-only engine (the walk)",
                    EngineKind::PortableOnly,
                    &status,
                    nothing_to_prepare(),
                    status_beneath(&path),
                ),
            ],
            comparison_rows: vec![comparison_row(
                "`fs::symlink_metadata` of the joined path",
                "less: no containment",
                nothing_to_prepare(),
                std_status(&joined),
            )],
        });
    }
    Ok(groups)
}

fn source_name(size: usize) -> String {
    format!("source-{size}")
}

fn read_groups(parameters: &Parameters, paths: &WorkPaths) -> Result<Vec<Group>, Error> {
    let options = ReadOptions {
        limit: ReadLimit::WholeFile,
        final_symlink: FinalSymlink::Refuse,
    };
    let mut groups = Vec::new();
    for size in [parameters.small_size, parameters.medium_size] {
        let path = relative(&source_name(size))?;
        let absolute = paths.files.join(source_name(size));
        let plain = absolute.clone();
        let library_read: SideAction = Box::new(move |side: &Side<'_>| {
            side.engine
                .read_file(&Location::Beneath(&side.files, &path), &options)
                .is_ok_and(|bytes| bytes.len() == size)
        });
        groups.push(Group {
            operation: "read_file".to_owned(),
            case_label: size_label(size),
            repetitions: parameters.repetitions,
            library_rows: vec![library_row(
                LIVE,
                EngineKind::Live,
                &[Operation::ReadFile],
                nothing_to_prepare(),
                library_read,
            )],
            comparison_rows: vec![
                comparison_row(
                    "`fs::read`",
                    "less: follows a symlink, reads a FIFO",
                    nothing_to_prepare(),
                    Box::new(move || fs::read(&plain).is_ok_and(|bytes| bytes.len() == size)),
                ),
                comparison_row(
                    "`File::open`, `metadata`, `read_to_end`",
                    "same",
                    nothing_to_prepare(),
                    Box::new(move || {
                        read_regular_file(&absolute).is_ok_and(|bytes| bytes.len() == size)
                    }),
                ),
            ],
        });
    }
    Ok(groups)
}

fn durability_label(durability: Durability) -> &'static str {
    match durability {
        Durability::Written => "written",
        Durability::Durable => "durable",
    }
}

fn destination_label(existing_destination: ExistingDestination) -> &'static str {
    match existing_destination {
        ExistingDestination::Replace => "replace",
        ExistingDestination::Refuse => "refuse",
    }
}

/// Under `refuse` the destination must be absent before every run, and is removed untimed.
fn prepare_destination(destination: &Path, options: WriteOptions) -> Prepare {
    if options.existing_destination == ExistingDestination::Replace {
        return nothing_to_prepare();
    }
    let destination = destination.to_path_buf();
    Box::new(move || match fs::remove_file(&destination) {
        Ok(()) => true,
        Err(error) => error.kind() == io::ErrorKind::NotFound,
    })
}

fn write_group(
    parameters: &Parameters,
    paths: &WorkPaths,
    held: &HeldDirectories,
    payload: &Rc<Vec<u8>>,
    size: usize,
    options: WriteOptions,
) -> Result<Group, Error> {
    let leaf = format!(
        "write-{size}-{}-{}",
        durability_label(options.durability),
        destination_label(options.existing_destination)
    );
    let path = relative(&leaf)?;
    let destination = paths.files.join(&leaf);
    let temporary = paths.files.join(format!(".{leaf}.staged"));
    let library_payload = Rc::clone(payload);
    let library_write = move || -> SideAction {
        let (path, payload) = (path.clone(), Rc::clone(&library_payload));
        Box::new(move |side: &Side<'_>| {
            side.engine
                .write_file(
                    &Location::Beneath(&side.files, &path),
                    &payload[..size],
                    &options,
                )
                .is_ok()
        })
    };
    let write = [Operation::WriteFile];
    let mut library_rows = vec![library_row(
        LIVE,
        EngineKind::Live,
        &write,
        prepare_destination(&destination, options),
        library_write(),
    )];
    if size == parameters.small_size
        && options == write_options(Durability::Written, ExistingDestination::Replace)
    {
        library_rows.push(library_row(
            OBSERVED,
            EngineKind::Observed,
            &write,
            prepare_destination(&destination, options),
            library_write(),
        ));
    }
    Ok(Group {
        operation: "write_file".to_owned(),
        case_label: format!(
            "{}, {}, {}",
            size_label(size),
            durability_label(options.durability),
            destination_label(options.existing_destination)
        ),
        repetitions: parameters.repetitions,
        library_rows,
        comparison_rows: write_comparison_rows(
            &destination,
            &temporary,
            payload,
            size,
            options,
            &held.files,
        ),
    })
}

/// The two `std::fs` rows of a write: in place, which promises the least, and staged, which hand-rolls the
/// library's guarantee.
fn write_comparison_rows(
    destination: &Path,
    temporary: &Path,
    payload: &Rc<Vec<u8>>,
    size: usize,
    options: WriteOptions,
    directory: &Rc<File>,
) -> Vec<ComparisonRow> {
    let durable = options.durability == Durability::Durable;
    let in_place_form =
        if options == write_options(Durability::Written, ExistingDestination::Replace) {
            "`fs::write`".to_owned()
        } else {
            let create = match options.existing_destination {
                ExistingDestination::Replace => "`File::create`",
                ExistingDestination::Refuse => "`File::create_new`",
            };
            format!(
                "{create}, `write_all`{}",
                if durable { ", `sync_all`" } else { "" }
            )
        };
    let publish = match options.existing_destination {
        ExistingDestination::Replace => "`fs::rename`",
        ExistingDestination::Refuse => "`fs::hard_link`, `fs::remove_file`",
    };
    let staged_form = format!(
        "staged: `symlink_metadata`, `File::create_new`, `write_all`, `set_permissions`, {}{publish}{}",
        if durable { "`sync_all`, " } else { "" },
        if durable {
            ", `sync_all` of the directory"
        } else {
            ""
        }
    );
    let (in_place_payload, in_place_destination, in_place_directory) = (
        Rc::clone(payload),
        destination.to_path_buf(),
        Rc::clone(directory),
    );
    let (staged_payload, staged_destination, staged_temporary, staged_directory) = (
        Rc::clone(payload),
        destination.to_path_buf(),
        temporary.to_path_buf(),
        Rc::clone(directory),
    );
    vec![
        comparison_row(
            &in_place_form,
            "less: not atomic",
            prepare_destination(destination, options),
            Box::new(move || {
                in_place_write(
                    &in_place_destination,
                    &in_place_payload[..size],
                    options,
                    &in_place_directory,
                )
                .is_ok()
            }),
        ),
        comparison_row(
            &staged_form,
            "same",
            prepare_destination(destination, options),
            Box::new(move || {
                staged_write(
                    &staged_destination,
                    &staged_temporary,
                    &staged_payload[..size],
                    options,
                    &staged_directory,
                )
                .is_ok()
            }),
        ),
    ]
}

fn write_groups(
    parameters: &Parameters,
    paths: &WorkPaths,
    held: &HeldDirectories,
    payload: &Rc<Vec<u8>>,
) -> Result<Vec<Group>, Error> {
    let mut groups = Vec::new();
    for size in [parameters.small_size, parameters.medium_size] {
        for durability in [Durability::Written, Durability::Durable] {
            for existing in [ExistingDestination::Replace, ExistingDestination::Refuse] {
                let options = write_options(durability, existing);
                groups.push(write_group(
                    parameters, paths, held, payload, size, options,
                )?);
            }
        }
    }
    Ok(groups)
}

fn copy_groups(parameters: &Parameters, paths: &WorkPaths) -> Result<Vec<Group>, Error> {
    let options = write_options(Durability::Written, ExistingDestination::Replace);
    let copy = [Operation::CopyFile];
    let mut groups = Vec::new();
    for size in [parameters.medium_size, parameters.large_size] {
        let source_path = relative(&source_name(size))?;
        let destination_path = relative(&format!("copy-{size}"))?;
        let library_copy = || -> SideAction {
            let (source, destination) = (source_path.clone(), destination_path.clone());
            Box::new(move |side: &Side<'_>| {
                side.engine
                    .copy_file(
                        &Location::Beneath(&side.files, &source),
                        &Location::Beneath(&side.files, &destination),
                        &options,
                    )
                    .is_ok()
            })
        };
        let source = paths.files.join(source_name(size));
        let destination = paths.files.join(format!("copy-{size}"));
        let temporary = paths.files.join(format!(".copy-{size}.staged"));
        let (plain_source, plain_destination) = (source.clone(), destination.clone());
        groups.push(Group {
            operation: "copy_file".to_owned(),
            case_label: format!("{}, written, replace", size_label(size)),
            repetitions: if size == parameters.large_size {
                parameters.large_repetitions
            } else {
                parameters.repetitions
            },
            library_rows: vec![
                library_row(
                    LIVE,
                    EngineKind::Live,
                    &copy,
                    nothing_to_prepare(),
                    library_copy(),
                ),
                library_row(
                    "library, portable-only engine (the read-and-write loop)",
                    EngineKind::PortableOnly,
                    &copy,
                    nothing_to_prepare(),
                    library_copy(),
                ),
            ],
            comparison_rows: vec![
                comparison_row(
                    "`fs::copy`",
                    "less: not atomic",
                    nothing_to_prepare(),
                    Box::new(move || fs::copy(&plain_source, &plain_destination).is_ok()),
                ),
                comparison_row(
                    "`symlink_metadata` of the destination, `fs::copy` to a temporary, \
                     `set_permissions`, `fs::rename`",
                    "same",
                    nothing_to_prepare(),
                    Box::new(move || staged_copy(&source, &destination, &temporary).is_ok()),
                ),
            ],
        });
    }
    Ok(groups)
}

/// Where the renamed file is now: every rename row moves it from one of the two directories to the other, so
/// the destination is always absent and the rows can run under `refuse` as well as `replace`.
#[derive(Clone)]
struct RenameEnds {
    in_a: Rc<Cell<bool>>,
    path_a: PathBuf,
    path_b: PathBuf,
    directory_a: Rc<File>,
    directory_b: Rc<File>,
}

impl RenameEnds {
    /// The file's path now and the path it moves to.
    fn current_and_next(&self) -> (&Path, &Path) {
        if self.in_a.get() {
            (&self.path_a, &self.path_b)
        } else {
            (&self.path_b, &self.path_a)
        }
    }

    /// Both directories' syncs after a move, the destination's first, as the library orders them: the move has
    /// flipped `in_a`, so the directory the file is in now is the one it was moved to.
    fn sync_both(&self) -> bool {
        let (destination, source) = if self.in_a.get() {
            (&self.directory_a, &self.directory_b)
        } else {
            (&self.directory_b, &self.directory_a)
        };
        destination.sync_all().is_ok() && source.sync_all().is_ok()
    }

    fn moved(&self, succeeded: bool) -> bool {
        if succeeded {
            self.in_a.set(!self.in_a.get());
        }
        succeeded
    }
}

fn library_rename(
    ends: &RenameEnds,
    existing: ExistingDestination,
    durability: Durability,
) -> Result<SideAction, Error> {
    let (ends, path) = (ends.clone(), relative("moving")?);
    Ok(Box::new(move |side: &Side<'_>| {
        let (from, to) = if ends.in_a.get() {
            (&side.rename_a, &side.rename_b)
        } else {
            (&side.rename_b, &side.rename_a)
        };
        let moved = side.engine.rename(
            &Location::Beneath(from, &path),
            &Location::Beneath(to, &path),
            existing,
            durability,
        );
        ends.moved(moved.is_ok())
    }))
}

/// The `std::fs` move: `fs::rename`, which replaces, or a hard link and the removal of the old name, which
/// refuses an existing destination; under `durable` both directories synced after it.
fn std_rename(
    ends: &RenameEnds,
    existing: ExistingDestination,
    durability: Durability,
) -> StdAction {
    let ends = ends.clone();
    Box::new(move || {
        let (from, to) = ends.current_and_next();
        let succeeded = match existing {
            ExistingDestination::Replace => fs::rename(from, to).is_ok(),
            ExistingDestination::Refuse => {
                fs::hard_link(from, to).is_ok() && fs::remove_file(from).is_ok()
            }
        };
        ends.moved(succeeded) && (durability == Durability::Written || ends.sync_both())
    })
}

fn rename_groups(
    parameters: &Parameters,
    paths: &WorkPaths,
    held: &HeldDirectories,
) -> Result<Vec<Group>, Error> {
    let ends = RenameEnds {
        in_a: Rc::new(Cell::new(true)),
        path_a: paths.rename_a.join("moving"),
        path_b: paths.rename_b.join("moving"),
        directory_a: Rc::clone(&held.rename_a),
        directory_b: Rc::clone(&held.rename_b),
    };
    let mut groups = Vec::new();
    for existing in [ExistingDestination::Replace, ExistingDestination::Refuse] {
        for durability in [Durability::Written, Durability::Durable] {
            let mut comparison_rows = Vec::new();
            let durable = durability == Durability::Durable;
            let refusing = existing == ExistingDestination::Refuse;
            // The form that promises the least is a plain `fs::rename` in every case: it neither refuses an
            // existing name nor syncs anything.
            if durable || refusing {
                comparison_rows.push(comparison_row(
                    "`fs::rename`",
                    if refusing {
                        "less: replaces"
                    } else {
                        "less: syncs nothing"
                    },
                    nothing_to_prepare(),
                    std_rename(&ends, ExistingDestination::Replace, Durability::Written),
                ));
            }
            let same_form = if refusing {
                "`fs::hard_link`, `fs::remove_file`"
            } else {
                "`fs::rename`"
            };
            comparison_rows.push(comparison_row(
                &format!(
                    "{same_form}{}",
                    if durable {
                        ", `sync_all` of both directories"
                    } else {
                        ""
                    }
                ),
                "same",
                nothing_to_prepare(),
                std_rename(&ends, existing, durability),
            ));
            groups.push(Group {
                operation: "rename".to_owned(),
                case_label: format!(
                    "a file between two directories, {}, {}",
                    durability_label(durability),
                    destination_label(existing)
                ),
                repetitions: parameters.repetitions,
                library_rows: vec![library_row(
                    LIVE,
                    EngineKind::Live,
                    &[Operation::Rename],
                    nothing_to_prepare(),
                    library_rename(&ends, existing, durability)?,
                )],
                comparison_rows,
            });
        }
    }
    Ok(groups)
}

fn list_group(parameters: &Parameters, paths: &WorkPaths) -> Result<Group, Error> {
    let path = relative("listing")?;
    let expected = parameters.entry_count;
    let (as_read, by_name) = (paths.files.join("listing"), paths.files.join("listing"));
    Ok(Group {
        operation: "list_directory".to_owned(),
        case_label: format!("{expected} entries"),
        repetitions: parameters.repetitions,
        library_rows: vec![library_row(
            LIVE,
            EngineKind::Live,
            &[Operation::ListDirectory],
            nothing_to_prepare(),
            Box::new(move |side: &Side<'_>| {
                side.engine
                    .list_directory(&Location::Beneath(&side.files, &path), FinalSymlink::Refuse)
                    .is_ok_and(|entries| entries.len() == expected)
            }),
        )],
        comparison_rows: vec![
            comparison_row(
                "`fs::read_dir`",
                "less: unsorted",
                nothing_to_prepare(),
                Box::new(move || std_listing(&as_read, ListingOrder::AsRead, expected)),
            ),
            comparison_row(
                "`fs::read_dir`, then sorted by name",
                "same",
                nothing_to_prepare(),
                Box::new(move || std_listing(&by_name, ListingOrder::ByName, expected)),
            ),
        ],
    })
}

fn chain_group(parameters: &Parameters, paths: &WorkPaths) -> Result<Group, Error> {
    let mut deepest_text = "chain".to_owned();
    let mut levels = vec![paths.files.join("chain")];
    for _ in 1..parameters.chain_depth {
        deepest_text.push_str("/d");
        levels.push(paths.files.join(&deepest_text));
    }
    let (deepest, top) = (relative(&deepest_text)?, relative("chain")?);
    let (top_path, deepest_path) = (paths.files.join("chain"), paths.files.join(&deepest_text));
    let (plain_top, plain_deepest) = (top_path.clone(), deepest_path.clone());
    Ok(Group {
        operation: "create_directories + remove_tree".to_owned(),
        case_label: format!("{}-level chain", parameters.chain_depth),
        repetitions: parameters.repetitions,
        library_rows: vec![library_row(
            LIVE,
            EngineKind::Live,
            &[Operation::CreateDirectories, Operation::RemoveTree],
            nothing_to_prepare(),
            Box::new(move |side: &Side<'_>| {
                let created = side
                    .engine
                    .create_directories(
                        &Location::Beneath(&side.files, &deepest),
                        Permissions::exactly(0o700),
                    )
                    .is_ok();
                let removed = side
                    .engine
                    .remove_tree(&Location::Beneath(&side.files, &top));
                created && removed.is_ok_and(|existed| existed)
            }),
        )],
        comparison_rows: vec![
            comparison_row(
                "`fs::create_dir_all`, `fs::remove_dir_all`",
                "less: modes from the umask",
                nothing_to_prepare(),
                Box::new(move || {
                    fs::create_dir_all(&plain_deepest).is_ok()
                        && fs::remove_dir_all(&plain_top).is_ok()
                }),
            ),
            comparison_row(
                "`fs::create_dir_all`, `fs::set_permissions` per level, `fs::remove_dir_all`",
                "same",
                nothing_to_prepare(),
                Box::new(move || {
                    let created = fs::create_dir_all(&deepest_path).is_ok()
                        && levels.iter().all(|level| {
                            fs::set_permissions(level, fs::Permissions::from_mode(0o700)).is_ok()
                        });
                    fs::remove_dir_all(&top_path).is_ok() && created
                }),
            ),
        ],
    })
}

fn lock_group(parameters: &Parameters, paths: &WorkPaths) -> Result<Group, Error> {
    let path = relative("lock-target")?;
    let target = paths.files.join("lock-target");
    Ok(Group {
        operation: "try_lock".to_owned(),
        case_label: "acquired, then released".to_owned(),
        repetitions: parameters.repetitions,
        library_rows: vec![library_row(
            LIVE,
            EngineKind::Live,
            &[Operation::TryLock],
            nothing_to_prepare(),
            Box::new(move |side: &Side<'_>| {
                matches!(
                    side.engine.try_lock(&Location::Beneath(&side.files, &path)),
                    Ok(LockOutcome::Acquired(_))
                )
            }),
        )],
        comparison_rows: vec![comparison_row(
            "`File::open`, `metadata`, `File::try_lock`, drop",
            "same",
            nothing_to_prepare(),
            Box::new(move || {
                // The library locks a regular file or a directory and refuses anything else.
                File::open(&target).is_ok_and(|file| {
                    file.metadata()
                        .is_ok_and(|metadata| metadata.is_file() || metadata.is_dir())
                        && file.try_lock().is_ok()
                })
            }),
        )],
    })
}

// ---- The work directory -----------------------------------------------------------------------------------------

/// `path` as an `At` location's path; every path here is the absolute root joined with relative names.
fn absolute(path: &Path) -> Result<AbsolutePath, Error> {
    AbsolutePath::from_path(path)
}

/// The tree every row acts on, made with the live engine before anything is timed: the status tree with a file
/// at each measured depth, the three sources, the listing, the lock target and the renamed file.
fn plant_work(
    engine: &Engine,
    parameters: &Parameters,
    paths: &WorkPaths,
    payload: &[u8],
) -> Result<(), Error> {
    let mode = Permissions::exactly(0o700);
    let listing = paths.files.join("listing");
    let rename = paths.work.join("rename");
    for directory in [
        &paths.files,
        &paths.tree,
        &rename,
        &paths.rename_a,
        &paths.rename_b,
        &listing,
    ] {
        engine.create_directory(&Location::At(&absolute(directory)?), mode)?;
    }
    // The chain of directories the deepest status file sits at the bottom of; a depth of 1 needs none.
    if let Some(deepest) = parameters
        .status_depths
        .iter()
        .copied()
        .max()
        .filter(|&depth| depth > 1)
    {
        let chain = "d/".repeat(deepest - 1);
        let chain = paths.tree.join(chain.trim_end_matches('/'));
        engine.create_directories(&Location::At(&absolute(&chain)?), mode)?;
    }
    let options = write_options(Durability::Written, ExistingDestination::Replace);
    let small = &payload[..parameters.small_size];
    let mut files: Vec<(PathBuf, &[u8])> = parameters
        .status_depths
        .iter()
        .map(|&depth| (paths.tree.join(tree_relative_text(depth)), small))
        .collect();
    for size in [
        parameters.small_size,
        parameters.medium_size,
        parameters.large_size,
    ] {
        files.push((paths.files.join(source_name(size)), &payload[..size]));
    }
    files.push((paths.files.join("lock-target"), small));
    files.push((paths.rename_a.join("moving"), small));
    for entry in 0..parameters.entry_count {
        files.push((listing.join(format!("entry-{entry}")), small));
    }
    for (path, bytes) in files {
        engine.write_file(&Location::At(&absolute(&path)?), bytes, &options)?;
    }
    Ok(())
}

fn every_group(
    parameters: &Parameters,
    paths: &WorkPaths,
    held: &HeldDirectories,
    payload: &Rc<Vec<u8>>,
) -> Result<Vec<Group>, Error> {
    let mut groups = status_groups(parameters, paths)?;
    groups.extend(read_groups(parameters, paths)?);
    groups.extend(write_groups(parameters, paths, held, payload)?);
    groups.extend(copy_groups(parameters, paths)?);
    groups.extend(rename_groups(parameters, paths, held)?);
    groups.push(list_group(parameters, paths)?);
    groups.push(chain_group(parameters, paths)?);
    groups.push(lock_group(parameters, paths)?);
    Ok(groups)
}

fn held_directory(path: &Path) -> Result<Rc<File>, String> {
    File::open(path)
        .map(Rc::new)
        .map_err(|error| format!("{}: {error}", path.display()))
}

fn measure_work(
    parameters: &Parameters,
    engines: &Engines,
    paths: &WorkPaths,
    now: fn() -> Instant,
) -> Result<Vec<TableRow>, String> {
    let payload = Rc::new(vec![0x5a_u8; parameters.large_size]);
    plant_work(&engines.live, parameters, paths, &payload)
        .map_err(|error| format!("the work tree could not be planted: {error}"))?;
    let open = |engine| {
        open_side(engine, paths).map_err(|error| format!("a directory did not open: {error}"))
    };
    let sides = Sides {
        live: open(&engines.live)?,
        observed: open(engines.observed.engine())?,
        portable_only: open(engines.portable_only.engine())?,
        engines,
    };
    let held = HeldDirectories {
        files: held_directory(&paths.files)?,
        rename_a: held_directory(&paths.rename_a)?,
        rename_b: held_directory(&paths.rename_b)?,
    };
    let mut groups = every_group(parameters, paths, &held, &payload)
        .map_err(|error| format!("the rows could not be built: {error}"))?;
    let mut table = Vec::new();
    for group in &mut groups {
        measure_group(now, &sides, group, &mut table)
            .map_err(|form| format!("{}, {}: {form} failed", group.operation, group.case_label))?;
    }
    Ok(table)
}

fn run(
    parameters: &Parameters,
    engines: &Engines,
    now: fn() -> Instant,
    output: &mut dyn Write,
) -> Result<(), String> {
    let root = absolute(&parameters.temporary_root).map_err(|error| error.to_string())?;
    let name = engines
        .live
        .create_temporary_directory(&Location::At(&root), Permissions::exactly(0o700))
        .map_err(|error| format!("no work directory under the temporary root: {error}"))?;
    let work = parameters.temporary_root.join(name);
    let paths = WorkPaths {
        files: work.join("files"),
        tree: work.join("tree"),
        rename_a: work.join("rename").join("a"),
        rename_b: work.join("rename").join("b"),
        work,
    };
    let measured = measure_work(parameters, engines, &paths, now);
    let removed = absolute(&paths.work)
        .and_then(|work| engines.live.remove_tree(&Location::At(&work)))
        .map_err(|error| format!("the work directory was not removed: {error}"));
    let table = measured?;
    removed?;
    let depth = paths.work.components().count() - 1;
    let print = |output: &mut dyn Write| -> io::Result<()> {
        writeln!(
            output,
            "bc-file-system benchmark, Rust face: {} timed runs after {} warm-up runs ({} copies: {} after {}); \
             the work directory is {depth} components below `/`; times in µs.\n",
            parameters.repetitions.timed,
            parameters.repetitions.warm_up,
            size_label(parameters.large_size),
            parameters.large_repetitions.timed,
            parameters.large_repetitions.warm_up
        )?;
        print_table(output, &table)
    };
    print(output).map_err(|error| format!("the table could not be written: {error}"))
}

/// The composition root: the command line, the three engines, the clock and the two streams are named here and
/// nowhere below.
fn main() -> ExitCode {
    let mut arguments: Vec<OsString> = std::env::args_os().skip(1).collect();
    // `cargo bench` appends `--bench` to the arguments of every benchmark it runs: cargo's flag, not ours.
    if arguments.last().is_some_and(|last| last == "--bench") {
        arguments.pop();
    }
    let mut errors = io::stderr().lock();
    let Some(parameters) = parameters_from(&arguments) else {
        // A usage error whether or not the stream takes the usage: the exit code says so either way.
        drop(errors.write_all(USAGE.as_bytes()));
        return ExitCode::from(2);
    };
    let engines = match (
        Engine::live(),
        ObservedEngine::observed_for_tests(),
        ObservedEngine::portable_only_for_tests(),
    ) {
        (Ok(live), Ok(observed), Ok(portable_only)) => Engines {
            live,
            observed,
            portable_only,
        },
        _ => return ExitCode::FAILURE,
    };
    let now: fn() -> Instant = Instant::now;
    match run(&parameters, &engines, now, &mut io::stdout().lock()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            // The run failed whether or not the stream takes the message: the exit code says so either way.
            drop(writeln!(errors, "{message}"));
            ExitCode::FAILURE
        }
    }
}
