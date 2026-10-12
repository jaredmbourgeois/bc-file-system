// The hand-run benchmark of the C++ face against std::filesystem, <fstream> and POSIX (design doc, "Benchmark").
//
// Each row times one operation on the library and on the standard forms a consumer uses today, and prints one
// Markdown table. A comparison row is the form that promises the least ("less: …") and, where that promises less
// than the library, a second form that hand-rolls the same guarantee ("same"); the library's live row carries
// its median over the "same" row's, which is how "never slower than std" is judged, row by row. The library's
// rows also carry the system calls one run made, read from an observed engine's step counts.
//
// This program is a composition root, and the whole of this file is the guard's legal reach site for it
// (AGENTS.md, "Inject, never reach"): `main` names the live values (the three engines, the clock, the two
// streams, the command line) and hands them down, and the comparison rows below it call std::filesystem,
// <fstream> and POSIX directly because those calls are what the benchmark measures. It writes only inside one
// directory it makes under the temporary root it is handed, and removes that directory when it ends. Never a CI
// job and never a leg of scripts/verify.sh: it is built by the `bench` preset and run by hand.
//
//   bc_file_system_benchmark <temporary-root> <warm-up> <iterations> <large-warm-up> <large-iterations>
//                            <small-bytes> <medium-bytes> <large-bytes> <entry-count> <chain-depth> <depth>...

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/engine.hpp>
#include <bc_file_system/error.hpp>
#include <bc_file_system/file_system.hpp>
#include <bc_file_system/location.hpp>
#include <bc_file_system/options.hpp>
#include <bc_file_system/result.hpp>
#include <bc_file_system/status.hpp>

#include "harness.hpp"

namespace bc::file_system::benchmark {

namespace {

// ---- What the command line states -----------------------------------------------------------------------------

struct Parameters {
    std::filesystem::path temporary_root;
    Repetitions repetitions;
    /// For the copies of the large file, each of which moves the whole file.
    Repetitions large_repetitions;
    std::size_t small_size;
    std::size_t medium_size;
    std::size_t large_size;
    std::size_t entry_count;
    std::size_t chain_depth;
    std::vector<std::size_t> status_depths;
};

constexpr std::string_view usage =
    "usage: bc_file_system_benchmark <temporary-root> <warm-up> <iterations> <large-warm-up> <large-iterations>\n"
    "                                <small-bytes> <medium-bytes> <large-bytes> <entry-count> <chain-depth> "
    "<depth>...\n"
    "  every count and size is a positive integer; <temporary-root> is an absolute path to an existing directory\n";

// The count the text spells, or nothing for anything but a positive decimal integer.
auto positive_count(std::string_view text) -> std::optional<std::size_t> {
    std::size_t value = 0;
    const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) {
        return std::nullopt;
    }
    return value;
}

auto positive_int(std::string_view text) -> std::optional<int> {
    const std::optional<std::size_t> count = positive_count(text);
    if (!count.has_value() || *count > 1'000'000) {
        return std::nullopt;
    }
    return static_cast<int>(*count);
}

// Every argument is required: a missing one is a usage error, never a value chosen here.
auto parameters_from(const std::vector<std::string_view>& arguments) -> std::optional<Parameters> {
    constexpr std::size_t fixed_arguments = 10;
    if (arguments.size() < fixed_arguments + 1) {
        return std::nullopt;
    }
    const std::optional<int> warm_up = positive_int(arguments[1]);
    const std::optional<int> iterations = positive_int(arguments[2]);
    const std::optional<int> large_warm_up = positive_int(arguments[3]);
    const std::optional<int> large_iterations = positive_int(arguments[4]);
    const std::optional<std::size_t> small_size = positive_count(arguments[5]);
    const std::optional<std::size_t> medium_size = positive_count(arguments[6]);
    const std::optional<std::size_t> large_size = positive_count(arguments[7]);
    const std::optional<std::size_t> entry_count = positive_count(arguments[8]);
    const std::optional<std::size_t> chain_depth = positive_count(arguments[9]);
    if (!warm_up || !iterations || !large_warm_up || !large_iterations || !small_size || !medium_size || !large_size ||
        !entry_count || !chain_depth) {
        return std::nullopt;
    }
    std::vector<std::size_t> status_depths;
    for (std::size_t index = fixed_arguments; index < arguments.size(); ++index) {
        const std::optional<std::size_t> depth = positive_count(arguments[index]);
        if (!depth.has_value()) {
            return std::nullopt;
        }
        status_depths.push_back(*depth);
    }
    const std::filesystem::path root{arguments[0]};
    if (!root.is_absolute()) {
        return std::nullopt;
    }
    return Parameters{.temporary_root = root,
                      .repetitions = Repetitions{.warm_up = *warm_up, .timed = *iterations},
                      .large_repetitions = Repetitions{.warm_up = *large_warm_up, .timed = *large_iterations},
                      .small_size = *small_size,
                      .medium_size = *medium_size,
                      .large_size = *large_size,
                      .entry_count = *entry_count,
                      .chain_depth = *chain_depth,
                      .status_depths = status_depths};
}

// ---- The engines and what is held open through each -----------------------------------------------------------

struct Engines {
    Engine live;
    Engine observed;
    Engine portable_only;
};

enum class EngineKind : std::uint8_t { live, observed, portable_only };

// One engine's face and the directories every `beneath` location of a row is relative to, each opened through
// that engine: `files` holds the files the reads, writes, copies, the listing, the chain and the lock act on;
// `tree` the chain of directories the status rows descend; `rename_a` and `rename_b` the two directories the
// renamed file moves between.
struct Side {
    Engine& engine;
    FileSystem files;
    Directory files_directory;
    Directory tree_directory;
    Directory rename_a_directory;
    Directory rename_b_directory;
};

struct Sides {
    Side live;
    Side observed;
    Side portable_only;

    [[nodiscard]] auto of(EngineKind kind) -> Side& {
        if (kind == EngineKind::observed) {
            return observed;
        }
        if (kind == EngineKind::portable_only) {
            return portable_only;
        }
        return live;
    }
};

struct WorkPaths {
    std::filesystem::path work;
    std::filesystem::path files;
    std::filesystem::path tree;
    std::filesystem::path rename_a;
    std::filesystem::path rename_b;
};

auto directory_at(FileSystem& files, const std::filesystem::path& path) -> Result<Directory> {
    return files.open_directory(Location::at(path), FinalSymlink::refuse);
}

auto open_side(Engine& engine, const WorkPaths& paths) -> Result<Side> {
    FileSystem files{engine};
    Result<Directory> files_directory = directory_at(files, paths.files);
    if (!files_directory) {
        return files_directory.error();
    }
    Result<Directory> tree_directory = directory_at(files, paths.tree);
    if (!tree_directory) {
        return tree_directory.error();
    }
    Result<Directory> rename_a_directory = directory_at(files, paths.rename_a);
    if (!rename_a_directory) {
        return rename_a_directory.error();
    }
    Result<Directory> rename_b_directory = directory_at(files, paths.rename_b);
    if (!rename_b_directory) {
        return rename_b_directory.error();
    }
    return Side{.engine = engine,
                .files = files,
                .files_directory = std::move(files_directory).value(),
                .tree_directory = std::move(tree_directory).value(),
                .rename_a_directory = std::move(rename_a_directory).value(),
                .rename_b_directory = std::move(rename_b_directory).value()};
}

// A directory held open for the hand-rolled POSIX forms, which act on it with *at calls as the library acts on
// its own directories. Read-only, so a durable form can sync it directly; closed when it goes out of scope.
class HeldDirectory {
public:
    explicit HeldDirectory(int descriptor) noexcept : descriptor_(descriptor) {}
    HeldDirectory(const HeldDirectory&) = delete;
    auto operator=(const HeldDirectory&) -> HeldDirectory& = delete;
    HeldDirectory(HeldDirectory&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    auto operator=(HeldDirectory&&) -> HeldDirectory& = delete;
    ~HeldDirectory() {
        if (descriptor_ >= 0) {
            (void)close(descriptor_);
        }
    }

    [[nodiscard]] auto descriptor() const noexcept -> int { return descriptor_; }

private:
    int descriptor_;
};

struct HeldDirectories {
    HeldDirectory files;
    HeldDirectory tree;
    HeldDirectory rename_a;
    HeldDirectory rename_b;
};

// ---- Rows and groups -------------------------------------------------------------------------------------------

using SideAction = std::function<bool(Side&)>;

// A row of the library: the engine it is timed on, and the operations whose steps it reaches, counted on the
// engine that observes it (the observed engine for a live row, the row's own engine otherwise).
struct LibraryRow {
    std::string form;
    EngineKind timed_on;
    std::vector<Operation> operations;
    Action prepare;
    SideAction action;
};

// A row of the standard forms: what it does not promise that the library does ("less: …"), or "same".
struct ComparisonRow {
    std::string form;
    std::string guarantee;
    Action prepare;
    Action action;
};

// One operation in one case: the library's rows, the first of them the live engine's, and the comparison rows.
struct Group {
    std::string operation;
    std::string case_label;
    Repetitions repetitions;
    std::vector<LibraryRow> library_rows;
    std::vector<ComparisonRow> comparison_rows;
};

auto nothing_to_prepare() -> Action {
    return [] { return true; };
}

auto library_row(std::string form, EngineKind timed_on, std::vector<Operation> operations, Action prepare,
                 SideAction action) -> LibraryRow {
    return LibraryRow{.form = std::move(form),
                      .timed_on = timed_on,
                      .operations = std::move(operations),
                      .prepare = std::move(prepare),
                      .action = std::move(action)};
}

auto live_row(std::vector<Operation> operations, Action prepare, SideAction action) -> LibraryRow {
    return library_row("library, live engine", EngineKind::live, std::move(operations), std::move(prepare),
                       std::move(action));
}

// ---- Counting the system calls ---------------------------------------------------------------------------------

// Every step count of `operations` on `engine`, summed over the operations, one entry per step in the header's
// order. The steps are walked until the face no longer names one, so a step appended later is counted too.
auto step_counts(const Engine& engine, const std::vector<Operation>& operations)
    -> std::optional<std::vector<std::uint64_t>> {
    std::vector<std::uint64_t> counts;
    for (std::int32_t value = 1;; ++value) {
        const std::optional<Step> step = step_from(value);
        if (!step.has_value()) {
            return counts;
        }
        std::uint64_t total = 0;
        for (const Operation operation : operations) {
            const Result<std::uint64_t> count = engine.step_count(operation, *step);
            if (!count) {
                return std::nullopt;
            }
            total += *count;
        }
        counts.push_back(total);
    }
}

struct CallCount {
    std::string calls;
    std::string steps;
};

// The calls between two readings: every step but the identity comparison, which is a step of the observer's but
// calls nothing; the breakdown names it all the same, so a reader sees the bracket.
auto call_count(const std::vector<std::uint64_t>& before, const std::vector<std::uint64_t>& after) -> CallCount {
    std::uint64_t calls = 0;
    std::string steps;
    for (std::size_t index = 0; index < after.size(); ++index) {
        const std::uint64_t reached = after[index] - before[index];
        if (reached == 0) {
            continue;
        }
        const std::optional<Step> step = step_from(static_cast<std::int32_t>(index + 1));
        if (!step.has_value()) {
            continue;
        }
        if (*step != Step::identity_check) {
            calls += reached;
        }
        if (!steps.empty()) {
            steps += ", ";
        }
        steps += std::string(name_of(*step)) + " " + std::to_string(reached);
    }
    return CallCount{.calls = std::to_string(calls), .steps = steps};
}

auto counting_engine(EngineKind timed_on) -> EngineKind {
    if (timed_on == EngineKind::portable_only) {
        return EngineKind::portable_only;
    }
    return EngineKind::observed;
}

// One run of the row on the engine that observes it, between two readings of its step counts.
auto count_calls(Sides& sides, const LibraryRow& row) -> std::optional<CallCount> {
    Side& side = sides.of(counting_engine(row.timed_on));
    const std::optional<std::vector<std::uint64_t>> before = step_counts(side.engine, row.operations);
    if (!before.has_value() || !row.prepare() || !row.action(side)) {
        return std::nullopt;
    }
    const std::optional<std::vector<std::uint64_t>> after = step_counts(side.engine, row.operations);
    if (!after.has_value()) {
        return std::nullopt;
    }
    return call_count(*before, *after);
}

// Measures every row of `group` and appends its lines to `table`; answers the form that failed, if one did.
auto measure_group(Now now, Sides& sides, const Group& group, std::vector<TableRow>& table)
    -> std::optional<std::string> {
    std::vector<TableRow> lines;
    for (const LibraryRow& row : group.library_rows) {
        const std::optional<CallCount> counted = count_calls(sides, row);
        if (!counted.has_value()) {
            return row.form + " (counting its calls)";
        }
        Side& side = sides.of(row.timed_on);
        const SideAction& action = row.action;
        const std::optional<Summary> summary =
            measure(now, group.repetitions, row.prepare, [&action, &side] { return action(side); });
        if (!summary.has_value()) {
            return row.form;
        }
        lines.push_back(TableRow{.operation = group.operation,
                                 .case_label = group.case_label,
                                 .form = row.form,
                                 .guarantee = "library",
                                 .summary = *summary,
                                 .versus_same = "",
                                 .calls = counted->calls,
                                 .steps = counted->steps});
    }
    std::optional<Summary> same;
    for (const ComparisonRow& row : group.comparison_rows) {
        const std::optional<Summary> summary = measure(now, group.repetitions, row.prepare, row.action);
        if (!summary.has_value()) {
            return row.form;
        }
        if (row.guarantee == "same" && !same.has_value()) {
            same = summary;
        }
        lines.push_back(TableRow{.operation = group.operation,
                                 .case_label = group.case_label,
                                 .form = row.form,
                                 .guarantee = row.guarantee,
                                 .summary = *summary,
                                 .versus_same = "",
                                 .calls = "",
                                 .steps = ""});
    }
    if (same.has_value() && !lines.empty()) {
        lines.front().versus_same = ratio_text(lines.front().summary.median_nanoseconds, same->median_nanoseconds);
    }
    table.insert(table.end(), lines.begin(), lines.end());
    return std::nullopt;
}

// ---- The forms the library is compared with ---------------------------------------------------------------------

#ifdef __APPLE__
// What the library's `durable` is on Apple platforms: F_FULLFSYNC asks the drive to flush its own cache, which
// fsync leaves alone there, so a hand-rolled form with the same guarantee makes the same call.
auto full_sync(int descriptor) -> bool {
    return fcntl(descriptor, F_FULLFSYNC) == 0;
}

// How the library opens a directory it walks through: for search only, so a directory the process may enter but
// not list is a parent like any other. A hand-rolled walk with the same guarantee opens the same way. O_SEARCH is
// O_EXEC | O_DIRECTORY on Apple platforms, so it carries the directory check itself.
auto search_open_flags() -> int {
    return O_SEARCH | O_CLOEXEC;
}
#else
auto full_sync(int descriptor) -> bool {
    return fsync(descriptor) == 0;
}

auto search_open_flags() -> int {
    return O_PATH | O_DIRECTORY | O_CLOEXEC;
}
#endif

auto is_regular(const std::filesystem::path& path) -> bool {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    return !error && status.type() == std::filesystem::file_type::regular;
}

// The library's containment, hand-rolled: each directory component opened with `open_flags` against the one before
// it, never through a symlink, then the leaf stated without following it. The walk a platform without a one-call
// containment open pays, without the library's identity bracket.
auto walked_status(int held, std::size_t components, const char* leaf, int open_flags) -> bool {
    int parent = held;
    for (std::size_t component = 0; component < components; ++component) {
        const int child = openat(parent, "d", open_flags | O_NOFOLLOW);
        if (parent != held) {
            (void)close(parent);
        }
        if (child < 0) {
            return false;
        }
        parent = child;
    }
    struct stat info{};
    const bool stated = fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(info.st_mode);
    if (parent != held) {
        (void)close(parent);
    }
    return stated;
}

// The idiom most C++ reads a whole file with: open at the end to learn the size, then read that much.
auto fstream_read(const std::filesystem::path& path) -> bool {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    const std::streamsize size = stream.tellg();
    if (size < 0) {
        return false;
    }
    std::vector<char> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(bytes.data(), size);
    return stream.gcount() == size;
}

// The library's read, hand-rolled: the leaf opened without following it and without waiting on a FIFO, anything
// but a regular file refused by fstat, the measured size read, then one more read to see that the file did not
// grow.
auto posix_read(int held, const char* leaf) -> bool {
    const int descriptor = openat(held, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    struct stat info{};
    bool read_whole = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode);
    if (read_whole) {
        std::vector<std::byte> bytes(static_cast<std::size_t>(info.st_size));
        std::size_t offset = 0;
        while (read_whole && offset < bytes.size()) {
            const ssize_t count =
                pread(descriptor, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
            read_whole = count > 0;
            offset += read_whole ? static_cast<std::size_t>(count) : 0;
        }
        std::byte past_the_end{};
        read_whole = read_whole && pread(descriptor, &past_the_end, 1, static_cast<off_t>(offset)) == 0;
    }
    (void)close(descriptor);
    return read_whole;
}

auto write_every_byte(int descriptor, std::span<const std::byte> bytes) -> bool {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

// `<fstream>`'s write: the destination truncated and written in place.
auto fstream_write(const std::filesystem::path& path, std::span<const std::byte> bytes) -> bool {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    return !stream.fail();
}

// A write in place, the form POSIX code writes a file with: created exclusively under `refuse`, truncated under
// `replace`; under `durable` the file is synced, and so is its directory when the name is new.
auto in_place_write(int held, const char* leaf, std::span<const std::byte> bytes, Durability durability,
                    ExistingDestination existing_destination) -> bool {
    const int creation = existing_destination == ExistingDestination::refuse ? O_EXCL : O_TRUNC;
    const int descriptor = openat(held, leaf, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | creation, 0644);
    if (descriptor < 0) {
        return false;
    }
    bool written = write_every_byte(descriptor, bytes);
    if (written && durability == Durability::durable) {
        written = full_sync(descriptor);
    }
    written = close(descriptor) == 0 && written;
    if (written && durability == Durability::durable && existing_destination == ExistingDestination::refuse) {
        written = full_sync(held);
    }
    return written;
}

// What the library decides before it stages anything: the destination is absent or a regular file, never a
// symlink, a directory or a FIFO, which a rename into place would otherwise replace.
auto destination_is_replaceable(int held, const char* leaf) -> bool {
    struct stat info{};
    if (fstatat(held, leaf, &info, AT_SYMLINK_NOFOLLOW) == 0) {
        return S_ISREG(info.st_mode);
    }
    return errno == ENOENT;
}

// The library's staged write, hand-rolled: the destination judged, an exclusive temporary beside it, every byte,
// the mode, under `durable` a full sync, the close, then one rename into place (or, refusing to replace, a link
// that fails on an existing name and the unlink of the temporary), then under `durable` the directory's sync.
auto staged_write(int held, const char* leaf, const char* temporary, std::span<const std::byte> bytes,
                  Durability durability, ExistingDestination existing_destination) -> bool {
    if (!destination_is_replaceable(held, leaf)) {
        return false;
    }
    const int descriptor = openat(held, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    bool written = write_every_byte(descriptor, bytes) && fchmod(descriptor, 0644) == 0;
    if (written && durability == Durability::durable) {
        written = full_sync(descriptor);
    }
    written = close(descriptor) == 0 && written;
    if (written && existing_destination == ExistingDestination::replace) {
        written = renameat(held, temporary, held, leaf) == 0;
    } else if (written) {
        written = linkat(held, temporary, held, leaf, 0) == 0;
    }
    if (!written || existing_destination == ExistingDestination::refuse) {
        (void)unlinkat(held, temporary, 0);
    }
    if (written && durability == Durability::durable) {
        written = full_sync(held);
    }
    return written;
}

// The library's copy by std::filesystem: the destination judged as a staged write judges it, the bytes copied to
// a temporary beside it, the mode set exactly, then one rename into place.
auto staged_copy(const std::filesystem::path& source, const std::filesystem::path& destination,
                 const std::filesystem::path& temporary) -> bool {
    std::error_code error;
    const std::filesystem::file_type existing = std::filesystem::symlink_status(destination, error).type();
    if (existing != std::filesystem::file_type::regular && existing != std::filesystem::file_type::not_found) {
        return false;
    }
    error.clear();
    std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if (!error) {
        std::filesystem::permissions(temporary,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                         std::filesystem::perms::group_read | std::filesystem::perms::others_read,
                                     std::filesystem::perm_options::replace, error);
    }
    if (!error) {
        std::filesystem::rename(temporary, destination, error);
    }
    return !error;
}

auto removed(const std::filesystem::path& path) -> bool {
    std::error_code error;
    std::filesystem::remove(path, error);
    return !error;
}

// A directory's entries as the library answers them: each name with its type, in name order or as read.
enum class ListingOrder : std::uint8_t { as_read, by_name };

struct ListedEntry {
    std::string name;
    std::filesystem::file_type type;
};

// An entry's type from what the iterator already read (the directory's own record of it), as the library takes
// it: the predicates read the cached type, where `symlink_status()` would stat every entry for its permissions.
auto listed_type(const std::filesystem::directory_entry& entry, std::error_code& error) -> std::filesystem::file_type {
    if (entry.is_symlink(error)) {
        return std::filesystem::file_type::symlink;
    }
    if (entry.is_directory(error)) {
        return std::filesystem::file_type::directory;
    }
    if (entry.is_regular_file(error)) {
        return std::filesystem::file_type::regular;
    }
    return std::filesystem::file_type::unknown;
}

auto std_listing(const std::filesystem::path& path, ListingOrder order, std::size_t expected) -> bool {
    std::error_code error;
    std::vector<ListedEntry> entries;
    for (std::filesystem::directory_iterator iterator(path, error); !error && iterator != end(iterator);
         iterator.increment(error)) {
        entries.push_back(
            ListedEntry{.name = iterator->path().filename().native(), .type = listed_type(*iterator, error)});
    }
    if (order == ListingOrder::by_name) {
        std::ranges::sort(entries, {}, &ListedEntry::name);
    }
    return !error && entries.size() == expected;
}

// ---- The groups ------------------------------------------------------------------------------------------------

auto write_options(Durability durability, ExistingDestination existing_destination) -> WriteOptions {
    return WriteOptions{.durability = durability,
                        .permissions = Permissions::exactly(0644),
                        .existing_destination = existing_destination,
                        .final_symlink = FinalSymlink::refuse};
}

auto relative(std::string_view text) -> Result<RelativePath> {
    return RelativePath::from(text);
}

// The C struct of a location beneath `directory`, for the rows that call the core without the face: it borrows
// the bytes of `path`, which the row's action holds, so nothing is copied.
auto c_beneath(const Directory& directory, const RelativePath& path) -> bc_file_system_location {
    const std::string_view bytes = path.bytes();
    return bc_file_system_location_beneath(directory.handle(), reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                           bytes.size());
}

auto size_label(std::size_t bytes) -> std::string {
    constexpr std::size_t kibibyte = 1024;
    constexpr std::size_t mebibyte = kibibyte * 1024;
    if (bytes % mebibyte == 0) {
        return std::to_string(bytes / mebibyte) + " MiB";
    }
    if (bytes % kibibyte == 0) {
        return std::to_string(bytes / kibibyte) + " KiB";
    }
    return std::to_string(bytes) + " B";
}

// The relative path of the file at `depth` in the status tree: depth - 1 directories named `d`, then `f`.
auto tree_relative_text(std::size_t depth) -> std::string {
    std::string text;
    for (std::size_t level = 1; level < depth; ++level) {
        text += "d/";
    }
    return text + "f";
}

auto status_groups(const Parameters& parameters, const WorkPaths& paths, const HeldDirectories& held)
    -> Result<std::vector<Group>> {
    std::vector<Group> groups;
    const std::vector<Operation> status_operation{Operation::status};
    for (const std::size_t depth : parameters.status_depths) {
        const std::string text = tree_relative_text(depth);
        Result<RelativePath> relative_path = relative(text);
        if (!relative_path) {
            return relative_path.error();
        }
        const std::filesystem::path absolute = paths.tree / text;
        const SideAction at_status = [absolute](Side& side) {
            const Result<std::optional<Status>> answer =
                side.files.status(Location::at(absolute), FinalSymlink::refuse);
            return answer.has_value() && answer->has_value();
        };
        const SideAction beneath_status = [path = *relative_path](Side& side) {
            const Result<std::optional<Status>> answer =
                side.files.status(Location::beneath(side.tree_directory, path), FinalSymlink::refuse);
            return answer.has_value() && answer->has_value();
        };
        // The core alone, for what the face's location, result and conversion add over the same call.
        const SideAction core_status = [path = *relative_path](Side& side) {
            const bc_file_system_location location = c_beneath(side.tree_directory, path);
            std::int32_t present = 0;
            bc_file_system_status status;
            bc_file_system_error error;
            return bc_file_system_status_of(side.engine.handle(), &location, bc_file_system_final_symlink_refuse,
                                            &present, &status, &error) == 0 &&
                   present == 1;
        };
        const Action std_status = [absolute] { return is_regular(absolute); };
        groups.push_back(Group{.operation = "status_of",
                               .case_label = "at, depth " + std::to_string(depth),
                               .repetitions = parameters.repetitions,
                               .library_rows = {live_row(status_operation, nothing_to_prepare(), at_status),
                                                library_row("library, observed engine", EngineKind::observed,
                                                            status_operation, nothing_to_prepare(), at_status)},
                               .comparison_rows = {ComparisonRow{.form = "`std::filesystem::symlink_status`",
                                                                 .guarantee = "same",
                                                                 .prepare = nothing_to_prepare(),
                                                                 .action = std_status}}});
        const int tree = held.tree.descriptor();
        groups.push_back(Group{
            .operation = "status_of",
            .case_label = "beneath, depth " + std::to_string(depth),
            .repetitions = parameters.repetitions,
            .library_rows = {live_row(status_operation, nothing_to_prepare(), beneath_status),
                             library_row("library, observed engine", EngineKind::observed, status_operation,
                                         nothing_to_prepare(), beneath_status),
                             library_row("C core, live engine", EngineKind::live, status_operation,
                                         nothing_to_prepare(), core_status),
                             library_row("library, portable-only engine (the walk)", EngineKind::portable_only,
                                         status_operation, nothing_to_prepare(), beneath_status)},
            .comparison_rows = {ComparisonRow{.form = "`std::filesystem::symlink_status` of the joined path",
                                              .guarantee = "less: no containment",
                                              .prepare = nothing_to_prepare(),
                                              .action = std_status},
                                ComparisonRow{.form = "POSIX `openat` per component, then `fstatat`",
                                              .guarantee = "same",
                                              .prepare = nothing_to_prepare(),
                                              .action =
                                                  [tree, components = depth - 1] {
                                                      return walked_status(tree, components, "f", search_open_flags());
                                                  }},
                                // The same walk with each component opened for reading, the open the S6 spike
                                // measured: what the search-only open saves at every level.
                                ComparisonRow{.form = "POSIX `openat` per component opened read-only, then "
                                                      "`fstatat`",
                                              .guarantee = "same",
                                              .prepare = nothing_to_prepare(),
                                              .action = [tree, components = depth - 1] {
                                                  return walked_status(tree, components, "f",
                                                                       O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                                              }}}});
    }
    return groups;
}

auto source_name(std::size_t size) -> std::string {
    return "source-" + std::to_string(size);
}

auto read_groups(const Parameters& parameters, const WorkPaths& paths, const HeldDirectories& held)
    -> Result<std::vector<Group>> {
    std::vector<Group> groups;
    const std::vector<Operation> read_operation{Operation::read_file};
    const ReadOptions options{.limit = ReadLimit::whole_file(), .final_symlink = FinalSymlink::refuse};
    for (const std::size_t size : {parameters.small_size, parameters.medium_size}) {
        const std::string leaf = source_name(size);
        Result<RelativePath> relative_path = relative(leaf);
        if (!relative_path) {
            return relative_path.error();
        }
        const std::filesystem::path absolute = paths.files / leaf;
        // The core alone, into a buffer the caller sized to the file: what the face's grow loop and its fresh,
        // zeroed buffer add is the difference between this row and the live row.
        const auto core_buffer = std::make_shared<std::vector<std::uint8_t>>(size);
        const SideAction core_read = [path = *relative_path, options, core_buffer](Side& side) {
            const bc_file_system_location location = c_beneath(side.files_directory, path);
            const bc_file_system_read_options c_options = to_c(options);
            std::size_t length = 0;
            bc_file_system_error error;
            return bc_file_system_read_file(side.engine.handle(), &location, &c_options, core_buffer->data(),
                                            core_buffer->size(), &length, &error) == 0 &&
                   length == core_buffer->size();
        };
        const int files = held.files.descriptor();
        groups.push_back(Group{
            .operation = "read_file",
            .case_label = size_label(size),
            .repetitions = parameters.repetitions,
            .library_rows = {live_row(read_operation, nothing_to_prepare(),
                                      [path = *relative_path, options, size](Side& side) {
                                          const Result<std::vector<std::byte>> bytes = side.files.read_file(
                                              Location::beneath(side.files_directory, path), options);
                                          return bytes.has_value() && bytes->size() == size;
                                      }),
                             library_row("C core, the caller's buffer of the file's size", EngineKind::live,
                                         read_operation, nothing_to_prepare(), core_read)},
            .comparison_rows = {ComparisonRow{.form = "`std::ifstream`, sized by `tellg`",
                                              .guarantee = "less: follows a symlink, reads a FIFO",
                                              .prepare = nothing_to_prepare(),
                                              .action = [absolute] { return fstream_read(absolute); }},
                                ComparisonRow{.form = "POSIX `openat`, `fstat`, `pread`",
                                              .guarantee = "same",
                                              .prepare = nothing_to_prepare(),
                                              .action = [files, leaf] { return posix_read(files, leaf.c_str()); }}}});
    }
    return groups;
}

auto durability_label(Durability durability) -> std::string {
    return durability == Durability::durable ? "durable" : "written";
}

auto destination_label(ExistingDestination existing_destination) -> std::string {
    return existing_destination == ExistingDestination::refuse ? "refuse" : "replace";
}

auto write_groups(const Parameters& parameters, const WorkPaths& paths, const HeldDirectories& held,
                  const std::shared_ptr<const std::vector<std::byte>>& payload) -> Result<std::vector<Group>> {
    std::vector<Group> groups;
    const std::vector<Operation> write_operation{Operation::write_file};
    const int files = held.files.descriptor();
    for (const std::size_t size : {parameters.small_size, parameters.medium_size}) {
        for (const Durability durability : {Durability::written, Durability::durable}) {
            for (const ExistingDestination existing : {ExistingDestination::replace, ExistingDestination::refuse}) {
                const std::string leaf = "write-" + std::to_string(size) + "-" + durability_label(durability) + "-" +
                                         destination_label(existing);
                const std::string temporary = "." + leaf + ".staged";
                Result<RelativePath> relative_path = relative(leaf);
                if (!relative_path) {
                    return relative_path.error();
                }
                const std::filesystem::path absolute = paths.files / leaf;
                // Under `refuse` the destination must be absent before every run, and is removed untimed.
                const Action prepare = existing == ExistingDestination::refuse
                                           ? Action{[absolute] { return removed(absolute); }}
                                           : nothing_to_prepare();
                const std::span<const std::byte> bytes(payload->data(), size);
                const SideAction library_write = [path = *relative_path, payload, bytes,
                                                  options = write_options(durability, existing)](Side& side) {
                    return side.files.write_file(Location::beneath(side.files_directory, path), bytes, options)
                        .has_value();
                };
                std::vector<LibraryRow> library_rows{live_row(write_operation, prepare, library_write)};
                if (size == parameters.small_size && durability == Durability::written &&
                    existing == ExistingDestination::replace) {
                    library_rows.push_back(library_row("library, observed engine", EngineKind::observed,
                                                       write_operation, prepare, library_write));
                }
                ComparisonRow in_place{.form = "POSIX `openat` in place, `write`" +
                                               std::string(durability == Durability::durable ? ", `F_FULLFSYNC`" : ""),
                                       .guarantee = "less: not atomic",
                                       .prepare = prepare,
                                       .action = [files, leaf, payload, bytes, durability, existing] {
                                           return in_place_write(files, leaf.c_str(), bytes, durability, existing);
                                       }};
                if (durability == Durability::written && existing == ExistingDestination::replace) {
                    in_place =
                        ComparisonRow{.form = "`std::ofstream`",
                                      .guarantee = "less: not atomic",
                                      .prepare = prepare,
                                      .action = [absolute, payload, bytes] { return fstream_write(absolute, bytes); }};
                }
                const std::string publish =
                    existing == ExistingDestination::refuse ? "`linkat`, `unlinkat`" : "`renameat`";
                groups.push_back(Group{
                    .operation = "write_file",
                    .case_label =
                        size_label(size) + ", " + durability_label(durability) + ", " + destination_label(existing),
                    .repetitions = parameters.repetitions,
                    .library_rows = std::move(library_rows),
                    .comparison_rows = {
                        in_place,
                        ComparisonRow{
                            .form = "POSIX staged: `fstatat`, `openat` exclusive, `write`, `fchmod`, " +
                                    std::string(durability == Durability::durable ? "`F_FULLFSYNC`, " : "") + publish +
                                    (durability == Durability::durable ? ", `F_FULLFSYNC` of the directory" : ""),
                            .guarantee = "same",
                            .prepare = prepare,
                            .action = [files, leaf, temporary, payload, bytes, durability, existing] {
                                return staged_write(files, leaf.c_str(), temporary.c_str(), bytes, durability,
                                                    existing);
                            }}}});
            }
        }
    }
    return groups;
}

auto copy_groups(const Parameters& parameters, const WorkPaths& paths) -> Result<std::vector<Group>> {
    std::vector<Group> groups;
    const std::vector<Operation> copy_operation{Operation::copy_file};
    const WriteOptions options = write_options(Durability::written, ExistingDestination::replace);
    for (const std::size_t size : {parameters.medium_size, parameters.large_size}) {
        const std::string source_leaf = source_name(size);
        const std::string destination_leaf = "copy-" + std::to_string(size);
        Result<RelativePath> source_path = relative(source_leaf);
        Result<RelativePath> destination_path = relative(destination_leaf);
        if (!source_path) {
            return source_path.error();
        }
        if (!destination_path) {
            return destination_path.error();
        }
        const std::filesystem::path source = paths.files / source_leaf;
        const std::filesystem::path destination = paths.files / destination_leaf;
        const std::filesystem::path temporary = paths.files / ("." + destination_leaf + ".staged");
        const SideAction library_copy = [source_path = *source_path, destination_path = *destination_path,
                                         options](Side& side) {
            return side.files
                .copy_file(Location::beneath(side.files_directory, source_path),
                           Location::beneath(side.files_directory, destination_path), options)
                .has_value();
        };
        groups.push_back(Group{
            .operation = "copy_file",
            .case_label = size_label(size) + ", written, replace",
            .repetitions = size == parameters.large_size ? parameters.large_repetitions : parameters.repetitions,
            .library_rows = {live_row(copy_operation, nothing_to_prepare(), library_copy),
                             library_row("library, portable-only engine (the read-and-write loop)",
                                         EngineKind::portable_only, copy_operation, nothing_to_prepare(),
                                         library_copy)},
            .comparison_rows = {ComparisonRow{.form = "`std::filesystem::copy_file`, overwrite_existing",
                                              .guarantee = "less: not atomic",
                                              .prepare = nothing_to_prepare(),
                                              .action =
                                                  [source, destination] {
                                                      std::error_code error;
                                                      std::filesystem::copy_file(
                                                          source, destination,
                                                          std::filesystem::copy_options::overwrite_existing, error);
                                                      return !error;
                                                  }},
                                ComparisonRow{.form = "`std::filesystem::symlink_status` of the destination, "
                                                      "`copy_file` to a temporary, `permissions`, `rename`",
                                              .guarantee = "same",
                                              .prepare = nothing_to_prepare(),
                                              .action = [source, destination, temporary] {
                                                  return staged_copy(source, destination, temporary);
                                              }}}});
    }
    return groups;
}

// Where the renamed file is now: every rename row moves it from one of the two directories to the other, so
// the destination is always absent and the rows can run under `refuse` as well as `replace`.
struct RenameState {
    bool in_a;
};

auto rename_groups(const Parameters& parameters, const WorkPaths& paths, const HeldDirectories& held)
    -> Result<std::vector<Group>> {
    std::vector<Group> groups;
    const std::vector<Operation> rename_operation{Operation::rename};
    Result<RelativePath> moving = relative("moving");
    if (!moving) {
        return moving.error();
    }
    const auto state = std::make_shared<RenameState>(RenameState{.in_a = true});
    const std::filesystem::path in_a = paths.rename_a / "moving";
    const std::filesystem::path in_b = paths.rename_b / "moving";
    const int held_a = held.rename_a.descriptor();
    const int held_b = held.rename_b.descriptor();
    // Both directories' syncs after a move, the destination's first, as the library orders them.
    const auto sync_both = [state, held_a, held_b] {
        return state->in_a ? full_sync(held_a) && full_sync(held_b) : full_sync(held_b) && full_sync(held_a);
    };
    for (const ExistingDestination existing : {ExistingDestination::replace, ExistingDestination::refuse}) {
        for (const Durability durability : {Durability::written, Durability::durable}) {
            const SideAction library_rename = [state, path = *moving, existing, durability](Side& side) {
                const Directory& from = state->in_a ? side.rename_a_directory : side.rename_b_directory;
                const Directory& to = state->in_a ? side.rename_b_directory : side.rename_a_directory;
                const bool moved =
                    side.files.rename(Location::beneath(from, path), Location::beneath(to, path), existing, durability)
                        .has_value();
                state->in_a = moved ? !state->in_a : state->in_a;
                return moved;
            };
            const Action std_rename = [state, in_a, in_b] {
                std::error_code error;
                std::filesystem::rename(state->in_a ? in_a : in_b, state->in_a ? in_b : in_a, error);
                state->in_a = !error ? !state->in_a : state->in_a;
                return !error;
            };
            const Action link_and_remove = [state, in_a, in_b] {
                std::error_code error;
                std::filesystem::create_hard_link(state->in_a ? in_a : in_b, state->in_a ? in_b : in_a, error);
                if (!error) {
                    std::filesystem::remove(state->in_a ? in_a : in_b, error);
                }
                state->in_a = !error ? !state->in_a : state->in_a;
                return !error;
            };
            std::vector<ComparisonRow> comparison_rows;
            const bool durable = durability == Durability::durable;
            const bool refusing = existing == ExistingDestination::refuse;
            // The std form that promises the least is a plain rename in every case: it neither refuses an
            // existing name nor syncs anything.
            if (durable || refusing) {
                comparison_rows.push_back(
                    ComparisonRow{.form = "`std::filesystem::rename`",
                                  .guarantee = refusing ? "less: replaces" : "less: syncs nothing",
                                  .prepare = nothing_to_prepare(),
                                  .action = std_rename});
            }
            const Action same_move = refusing ? link_and_remove : std_rename;
            const std::string same_form =
                refusing ? "`std::filesystem::create_hard_link`, `remove`" : "`std::filesystem::rename`";
            comparison_rows.push_back(
                ComparisonRow{.form = same_form + (durable ? ", `F_FULLFSYNC` of both directories" : ""),
                              .guarantee = "same",
                              .prepare = nothing_to_prepare(),
                              .action = durable ? Action{[same_move, sync_both] {
                                  // The move flips the state, so the sync reads the directories the other way round:
                                  // the one the file is in now is the destination it was moved to.
                                  if (!same_move()) {
                                      return false;
                                  }
                                  return sync_both();
                              }}
                                                : same_move});
            groups.push_back(Group{.operation = "rename",
                                   .case_label = "a file between two directories, " + durability_label(durability) +
                                                 ", " + destination_label(existing),
                                   .repetitions = parameters.repetitions,
                                   .library_rows = {live_row(rename_operation, nothing_to_prepare(), library_rename)},
                                   .comparison_rows = std::move(comparison_rows)});
        }
    }
    return groups;
}

auto list_group(const Parameters& parameters, const WorkPaths& paths) -> Result<Group> {
    const std::vector<Operation> list_operation{Operation::list_directory};
    Result<RelativePath> listing = relative("listing");
    if (!listing) {
        return listing.error();
    }
    const std::size_t expected = parameters.entry_count;
    const auto core_entries = std::make_shared<std::vector<bc_file_system_entry>>(expected);
    // The core alone, into an array the caller sized to the listing; see the read rows.
    const SideAction core_list = [path = *listing, core_entries](Side& side) {
        const bc_file_system_location location = c_beneath(side.files_directory, path);
        std::size_t count = 0;
        bc_file_system_error error;
        return bc_file_system_list_directory(side.engine.handle(), &location, bc_file_system_final_symlink_refuse,
                                             core_entries->data(), core_entries->size(), &count, &error) == 0 &&
               count == core_entries->size();
    };
    const std::filesystem::path absolute = paths.files / "listing";
    return Group{
        .operation = "list_directory",
        .case_label = std::to_string(expected) + " entries",
        .repetitions = parameters.repetitions,
        .library_rows = {live_row(list_operation, nothing_to_prepare(),
                                  [path = *listing, expected](Side& side) {
                                      const Result<std::vector<Entry>> entries = side.files.list_directory(
                                          Location::beneath(side.files_directory, path), FinalSymlink::refuse);
                                      return entries.has_value() && entries->size() == expected;
                                  }),
                         library_row("C core, the caller's array of the listing's size", EngineKind::live,
                                     list_operation, nothing_to_prepare(), core_list)},
        .comparison_rows = {
            ComparisonRow{
                .form = "`std::filesystem::directory_iterator`",
                .guarantee = "less: unsorted",
                .prepare = nothing_to_prepare(),
                .action = [absolute, expected] { return std_listing(absolute, ListingOrder::as_read, expected); }},
            ComparisonRow{
                .form = "`std::filesystem::directory_iterator`, then sorted by name",
                .guarantee = "same",
                .prepare = nothing_to_prepare(),
                .action = [absolute, expected] { return std_listing(absolute, ListingOrder::by_name, expected); }}}};
}

auto chain_group(const Parameters& parameters, const WorkPaths& paths) -> Result<Group> {
    const std::vector<Operation> chain_operations{Operation::create_directories, Operation::remove_tree};
    std::string deepest_text = "chain";
    std::vector<std::filesystem::path> levels{paths.files / "chain"};
    for (std::size_t level = 1; level < parameters.chain_depth; ++level) {
        deepest_text += "/d";
        levels.push_back(paths.files / deepest_text);
    }
    Result<RelativePath> deepest = relative(deepest_text);
    Result<RelativePath> top = relative("chain");
    if (!deepest) {
        return deepest.error();
    }
    if (!top) {
        return top.error();
    }
    const std::filesystem::path top_path = levels.front();
    const std::filesystem::path deepest_path = levels.back();
    const auto std_chain = [top_path, deepest_path] {
        std::error_code error;
        std::filesystem::create_directories(deepest_path, error);
        const bool created = !error;
        std::filesystem::remove_all(top_path, error);
        return created && !error;
    };
    return Group{
        .operation = "create_directories + remove_tree",
        .case_label = std::to_string(parameters.chain_depth) + "-level chain",
        .repetitions = parameters.repetitions,
        .library_rows = {live_row(chain_operations, nothing_to_prepare(),
                                  [deepest = *deepest, top = *top](Side& side) {
                                      const bool created =
                                          side.files
                                              .create_directories(Location::beneath(side.files_directory, deepest),
                                                                  Permissions::exactly(0700))
                                              .has_value();
                                      const Result<bool> removed_tree =
                                          side.files.remove_tree(Location::beneath(side.files_directory, top));
                                      return created && removed_tree.has_value() && *removed_tree;
                                  })},
        .comparison_rows = {
            ComparisonRow{.form = "`std::filesystem::create_directories`, `remove_all`",
                          .guarantee = "less: modes from the umask",
                          .prepare = nothing_to_prepare(),
                          .action = std_chain},
            ComparisonRow{.form = "`std::filesystem::create_directories`, `permissions` per level, `remove_all`",
                          .guarantee = "same",
                          .prepare = nothing_to_prepare(),
                          .action = [levels, top_path] {
                              std::error_code error;
                              std::filesystem::create_directories(levels.back(), error);
                              for (const std::filesystem::path& level : levels) {
                                  if (!error) {
                                      std::filesystem::permissions(level, std::filesystem::perms::owner_all,
                                                                   std::filesystem::perm_options::replace, error);
                                  }
                              }
                              const bool created = !error;
                              std::filesystem::remove_all(top_path, error);
                              return created && !error;
                          }}}};
}

auto lock_group(const Parameters& parameters, const HeldDirectories& held) -> Result<Group> {
    const std::vector<Operation> lock_operation{Operation::try_lock};
    Result<RelativePath> target = relative("lock-target");
    if (!target) {
        return target.error();
    }
    const int files = held.files.descriptor();
    return Group{.operation = "try_lock",
                 .case_label = "acquired, then released",
                 .repetitions = parameters.repetitions,
                 .library_rows = {live_row(lock_operation, nothing_to_prepare(),
                                           [path = *target](Side& side) {
                                               const Result<std::optional<Lock>> lock =
                                                   side.files.try_lock(Location::beneath(side.files_directory, path));
                                               return lock.has_value() && lock->has_value();
                                           })},
                 .comparison_rows = {ComparisonRow{.form = "POSIX `openat`, `fstat`, `flock`, `close`",
                                                   .guarantee = "same",
                                                   .prepare = nothing_to_prepare(),
                                                   .action = [files] {
                                                       const int descriptor =
                                                           openat(files, "lock-target",
                                                                  O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
                                                       if (descriptor < 0) {
                                                           return false;
                                                       }
                                                       struct stat info{};
                                                       const bool locked = fstat(descriptor, &info) == 0 &&
                                                                           S_ISREG(info.st_mode) &&
                                                                           flock(descriptor, LOCK_EX | LOCK_NB) == 0;
                                                       (void)close(descriptor);
                                                       return locked;
                                                   }}}};
}

// ---- The work directory -----------------------------------------------------------------------------------------

// The tree every row acts on, made with the live engine before anything is timed: the status tree with a file
// at each measured depth, the three sources, the listing, the lock target and the renamed file.
auto plant_work(FileSystem& files, const Parameters& parameters, const WorkPaths& paths,
                std::span<const std::byte> payload) -> Result<void> {
    const Permissions directory_mode = Permissions::exactly(0700);
    for (const std::filesystem::path& directory :
         {paths.files, paths.tree, paths.work / "rename", paths.rename_a, paths.rename_b, paths.files / "listing"}) {
        Result<void> created = files.create_directory(Location::at(directory), directory_mode);
        if (!created) {
            return created;
        }
    }
    const std::size_t deepest = *std::ranges::max_element(parameters.status_depths);
    if (deepest > 1) {
        std::string chain = tree_relative_text(deepest);
        chain.resize(chain.size() - 2); // the chain of directories alone, without "/f"
        Result<void> created = files.create_directories(Location::at(paths.tree / chain), directory_mode);
        if (!created) {
            return created;
        }
    }
    const WriteOptions options = write_options(Durability::written, ExistingDestination::replace);
    const std::span<const std::byte> small = payload.first(parameters.small_size);
    for (const std::size_t depth : parameters.status_depths) {
        const Result<Durability> written =
            files.write_file(Location::at(paths.tree / tree_relative_text(depth)), small, options);
        if (!written) {
            return written.error();
        }
    }
    for (const std::size_t size : {parameters.small_size, parameters.medium_size, parameters.large_size}) {
        const Result<Durability> written =
            files.write_file(Location::at(paths.files / source_name(size)), payload.first(size), options);
        if (!written) {
            return written.error();
        }
    }
    std::vector<std::filesystem::path> others{paths.files / "lock-target", paths.rename_a / "moving"};
    for (std::size_t entry = 0; entry < parameters.entry_count; ++entry) {
        others.push_back(paths.files / "listing" / ("entry-" + std::to_string(entry)));
    }
    for (const std::filesystem::path& other : others) {
        const Result<Durability> written = files.write_file(Location::at(other), small, options);
        if (!written) {
            return written.error();
        }
    }
    return Result<void>{};
}

auto open_held(const std::filesystem::path& path) -> HeldDirectory {
    return HeldDirectory{open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
}

// Every group, in the order the table prints them.
auto every_group(const Parameters& parameters, const WorkPaths& paths, const HeldDirectories& held,
                 const std::shared_ptr<const std::vector<std::byte>>& payload) -> Result<std::vector<Group>> {
    std::vector<Group> groups;
    for (Result<std::vector<Group>> part :
         {status_groups(parameters, paths, held), read_groups(parameters, paths, held),
          write_groups(parameters, paths, held, payload), copy_groups(parameters, paths),
          rename_groups(parameters, paths, held)}) {
        if (!part) {
            return part.error();
        }
        std::ranges::move(part.value(), std::back_inserter(groups));
    }
    for (Result<Group> single :
         {list_group(parameters, paths), chain_group(parameters, paths), lock_group(parameters, held)}) {
        if (!single) {
            return single.error();
        }
        groups.push_back(std::move(single).value());
    }
    return groups;
}

auto measure_work(const Parameters& parameters, Engines& engines, const WorkPaths& paths, Now now, std::ostream& output,
                  std::ostream& errors) -> int {
    const auto payload = std::make_shared<const std::vector<std::byte>>(parameters.large_size, std::byte{0x5a});
    FileSystem live_files{engines.live};
    const Result<void> planted = plant_work(live_files, parameters, paths, *payload);
    if (!planted) {
        errors << "the work tree could not be planted: " << planted.error().message() << "\n";
        return 1;
    }
    Result<Side> live = open_side(engines.live, paths);
    Result<Side> observed = open_side(engines.observed, paths);
    Result<Side> portable_only = open_side(engines.portable_only, paths);
    for (const Result<Side>* side : {&live, &observed, &portable_only}) {
        if (!*side) {
            errors << "a directory of the work tree did not open: " << side->error().message() << "\n";
            return 1;
        }
    }
    Sides sides{.live = std::move(live).value(),
                .observed = std::move(observed).value(),
                .portable_only = std::move(portable_only).value()};
    const HeldDirectories held{.files = open_held(paths.files),
                               .tree = open_held(paths.tree),
                               .rename_a = open_held(paths.rename_a),
                               .rename_b = open_held(paths.rename_b)};
    for (const HeldDirectory* directory : {&held.files, &held.tree, &held.rename_a, &held.rename_b}) {
        if (directory->descriptor() < 0) {
            errors << "a directory of the work tree did not open for the POSIX rows\n";
            return 1;
        }
    }
    const Result<std::vector<Group>> groups = every_group(parameters, paths, held, payload);
    if (!groups) {
        errors << "the rows could not be built: " << groups.error().message() << "\n";
        return 1;
    }
    std::vector<TableRow> table;
    for (const Group& group : *groups) {
        const std::optional<std::string> failed = measure_group(now, sides, group, table);
        if (failed.has_value()) {
            errors << group.operation << ", " << group.case_label << ": " << *failed << " failed\n";
            return 1;
        }
    }
    const std::size_t work_depth = static_cast<std::size_t>(std::distance(paths.work.begin(), paths.work.end())) - 1;
    output << "bc-file-system benchmark, C++ face: " << parameters.repetitions.timed << " timed runs after "
           << parameters.repetitions.warm_up << " warm-up runs (" << size_label(parameters.large_size)
           << " copies: " << parameters.large_repetitions.timed << " after " << parameters.large_repetitions.warm_up
           << "); the work directory is " << work_depth << " components below `/`; times in µs.\n\n";
    print_table(output, table);
    return 0;
}

auto run(const Parameters& parameters, Engines& engines, Now now, std::ostream& output, std::ostream& errors) -> int {
    FileSystem live_files{engines.live};
    const Result<std::string> work_name =
        live_files.create_temporary_directory(Location::at(parameters.temporary_root), Permissions::exactly(0700));
    if (!work_name) {
        errors << "no work directory under the temporary root: " << work_name.error().message() << "\n";
        return 1;
    }
    const std::filesystem::path work = parameters.temporary_root / *work_name;
    const WorkPaths paths{.work = work,
                          .files = work / "files",
                          .tree = work / "tree",
                          .rename_a = work / "rename" / "a",
                          .rename_b = work / "rename" / "b"};
    const int status = measure_work(parameters, engines, paths, now, output, errors);
    const Result<bool> removed_work = live_files.remove_tree(Location::at(work));
    if (!removed_work) {
        errors << "the work directory was not removed: " << removed_work.error().message() << "\n";
        return 1;
    }
    return status;
}

} // namespace

} // namespace bc::file_system::benchmark

// The composition root: the command line, the three engines, the clock and the two streams are named here and
// nowhere below.
auto main(int argument_count, char** argument_values) -> int {
    namespace benchmark = bc::file_system::benchmark;
    using bc::file_system::Engine;
    using bc::file_system::Result;
    const std::vector<std::string_view> arguments(argument_values + 1, argument_values + argument_count);
    const std::optional<benchmark::Parameters> parameters = benchmark::parameters_from(arguments);
    if (!parameters.has_value()) {
        std::cerr << benchmark::usage;
        return 2;
    }
    Result<Engine> live = Engine::live();
    Result<Engine> observed = Engine::observed_for_tests();
    Result<Engine> portable_only = Engine::portable_only_for_tests();
    if (!live || !observed || !portable_only) {
        std::cerr << "an engine could not be created\n";
        return 1;
    }
    benchmark::Engines engines{.live = std::move(live).value(),
                               .observed = std::move(observed).value(),
                               .portable_only = std::move(portable_only).value()};
    const benchmark::Now now = [] { return std::chrono::steady_clock::now(); };
    return benchmark::run(*parameters, engines, now, std::cout, std::cerr);
}
