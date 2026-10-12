#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <bc_file_system/bc_file_system.h>

namespace bc::file_system {

/// Why an operation failed: the core's fourteen kinds, each a decision a caller can act on, plus the C face's
/// grow-and-retry signal, which the face's own compositions absorb and hand on only after their bounded retry.
/// The values are the header's, pinned below, so a value crosses the boundary as itself.
enum class ErrorKind : std::int32_t {
    not_found = 1,
    not_regular_file = 2,
    not_directory = 3,
    permission_denied = 4,
    already_exists = 5,
    directory_not_empty = 6,
    symlink_refused = 7,
    too_large = 8,
    cross_device = 9,
    no_space = 10,
    resource_exhausted = 11,
    unsupported = 12,
    invalid_argument = 13,
    io_failed = 14,
    buffer_too_small = 15,
};

static_assert(static_cast<std::int32_t>(ErrorKind::not_found) == bc_file_system_error_kind_not_found);
static_assert(static_cast<std::int32_t>(ErrorKind::not_regular_file) == bc_file_system_error_kind_not_regular_file);
static_assert(static_cast<std::int32_t>(ErrorKind::not_directory) == bc_file_system_error_kind_not_directory);
static_assert(static_cast<std::int32_t>(ErrorKind::permission_denied) == bc_file_system_error_kind_permission_denied);
static_assert(static_cast<std::int32_t>(ErrorKind::already_exists) == bc_file_system_error_kind_already_exists);
static_assert(static_cast<std::int32_t>(ErrorKind::directory_not_empty) ==
              bc_file_system_error_kind_directory_not_empty);
static_assert(static_cast<std::int32_t>(ErrorKind::symlink_refused) == bc_file_system_error_kind_symlink_refused);
static_assert(static_cast<std::int32_t>(ErrorKind::too_large) == bc_file_system_error_kind_too_large);
static_assert(static_cast<std::int32_t>(ErrorKind::cross_device) == bc_file_system_error_kind_cross_device);
static_assert(static_cast<std::int32_t>(ErrorKind::no_space) == bc_file_system_error_kind_no_space);
static_assert(static_cast<std::int32_t>(ErrorKind::resource_exhausted) == bc_file_system_error_kind_resource_exhausted);
static_assert(static_cast<std::int32_t>(ErrorKind::unsupported) == bc_file_system_error_kind_unsupported);
static_assert(static_cast<std::int32_t>(ErrorKind::invalid_argument) == bc_file_system_error_kind_invalid_argument);
static_assert(static_cast<std::int32_t>(ErrorKind::io_failed) == bc_file_system_error_kind_io_failed);
static_assert(static_cast<std::int32_t>(ErrorKind::buffer_too_small) == bc_file_system_error_kind_buffer_too_small);

/// Which public function an error came from, in the header's order.
enum class Operation : std::int32_t {
    engine_create_live = 1,
    engine_create_observed_for_tests = 2,
    open_directory = 3,
    directory_opened_as = 4,
    observed_fault_add = 5,
    observed_faults_clear = 6,
    observed_step_count = 7,
    status = 8,
    read_file = 9,
    write_file = 10,
    append = 11,
    sync = 12,
    create_directory = 13,
    create_directories = 14,
    list_directory = 15,
    remove_file = 16,
    remove_directory = 17,
    remove_tree = 18,
    create_temporary_directory = 19,
    rename = 20,
    copy_file = 21,
    set_permissions = 22,
    create_symlink = 23,
    read_symlink = 24,
    try_lock = 25,
    condemn = 26,
    remove_condemned = 27,
    relative_path_from = 28,
    absolute_path_from = 29,
    path_join = 30,
    path_parent = 31,
    path_last_component = 32,
    path_next_component = 33,
    path_stem = 34,
    path_extension = 35,
    path_relate = 36,
    path_lexically_normalize = 37,
    path_resolve_against = 38,
    path_library_name = 39,
};

static_assert(static_cast<std::int32_t>(Operation::engine_create_live) == bc_file_system_operation_engine_create_live);
static_assert(static_cast<std::int32_t>(Operation::engine_create_observed_for_tests) ==
              bc_file_system_operation_engine_create_observed_for_tests);
static_assert(static_cast<std::int32_t>(Operation::open_directory) == bc_file_system_operation_open_directory);
static_assert(static_cast<std::int32_t>(Operation::directory_opened_as) ==
              bc_file_system_operation_directory_opened_as);
static_assert(static_cast<std::int32_t>(Operation::observed_fault_add) == bc_file_system_operation_observed_fault_add);
static_assert(static_cast<std::int32_t>(Operation::observed_faults_clear) ==
              bc_file_system_operation_observed_faults_clear);
static_assert(static_cast<std::int32_t>(Operation::observed_step_count) ==
              bc_file_system_operation_observed_step_count);
static_assert(static_cast<std::int32_t>(Operation::status) == bc_file_system_operation_status);
static_assert(static_cast<std::int32_t>(Operation::read_file) == bc_file_system_operation_read_file);
static_assert(static_cast<std::int32_t>(Operation::write_file) == bc_file_system_operation_write_file);
static_assert(static_cast<std::int32_t>(Operation::append) == bc_file_system_operation_append);
static_assert(static_cast<std::int32_t>(Operation::sync) == bc_file_system_operation_sync);
static_assert(static_cast<std::int32_t>(Operation::create_directory) == bc_file_system_operation_create_directory);
static_assert(static_cast<std::int32_t>(Operation::create_directories) == bc_file_system_operation_create_directories);
static_assert(static_cast<std::int32_t>(Operation::list_directory) == bc_file_system_operation_list_directory);
static_assert(static_cast<std::int32_t>(Operation::remove_file) == bc_file_system_operation_remove_file);
static_assert(static_cast<std::int32_t>(Operation::remove_directory) == bc_file_system_operation_remove_directory);
static_assert(static_cast<std::int32_t>(Operation::remove_tree) == bc_file_system_operation_remove_tree);
static_assert(static_cast<std::int32_t>(Operation::create_temporary_directory) ==
              bc_file_system_operation_create_temporary_directory);
static_assert(static_cast<std::int32_t>(Operation::rename) == bc_file_system_operation_rename);
static_assert(static_cast<std::int32_t>(Operation::copy_file) == bc_file_system_operation_copy_file);
static_assert(static_cast<std::int32_t>(Operation::set_permissions) == bc_file_system_operation_set_permissions);
static_assert(static_cast<std::int32_t>(Operation::create_symlink) == bc_file_system_operation_create_symlink);
static_assert(static_cast<std::int32_t>(Operation::read_symlink) == bc_file_system_operation_read_symlink);
static_assert(static_cast<std::int32_t>(Operation::try_lock) == bc_file_system_operation_try_lock);
static_assert(static_cast<std::int32_t>(Operation::condemn) == bc_file_system_operation_condemn);
static_assert(static_cast<std::int32_t>(Operation::remove_condemned) == bc_file_system_operation_remove_condemned);
static_assert(static_cast<std::int32_t>(Operation::relative_path_from) == bc_file_system_operation_relative_path_from);
static_assert(static_cast<std::int32_t>(Operation::absolute_path_from) == bc_file_system_operation_absolute_path_from);
static_assert(static_cast<std::int32_t>(Operation::path_join) == bc_file_system_operation_path_join);
static_assert(static_cast<std::int32_t>(Operation::path_parent) == bc_file_system_operation_path_parent);
static_assert(static_cast<std::int32_t>(Operation::path_last_component) ==
              bc_file_system_operation_path_last_component);
static_assert(static_cast<std::int32_t>(Operation::path_next_component) ==
              bc_file_system_operation_path_next_component);
static_assert(static_cast<std::int32_t>(Operation::path_stem) == bc_file_system_operation_path_stem);
static_assert(static_cast<std::int32_t>(Operation::path_extension) == bc_file_system_operation_path_extension);
static_assert(static_cast<std::int32_t>(Operation::path_relate) == bc_file_system_operation_path_relate);
static_assert(static_cast<std::int32_t>(Operation::path_lexically_normalize) ==
              bc_file_system_operation_path_lexically_normalize);
static_assert(static_cast<std::int32_t>(Operation::path_resolve_against) ==
              bc_file_system_operation_path_resolve_against);
static_assert(static_cast<std::int32_t>(Operation::path_library_name) == bc_file_system_operation_path_library_name);

/// The system call an operation was making when it failed, and the observer's vocabulary.
enum class Step : std::int32_t {
    open = 1,
    openat = 2,
    fstat = 3,
    fstatat = 4,
    read = 5,
    pread = 6,
    write = 7,
    fsync = 8,
    fcntl_fullfsync = 9,
    renameat = 10,
    rename_no_replace = 11,
    unlinkat = 12,
    mkdirat = 13,
    fdopendir = 14,
    readdir = 15,
    symlinkat = 16,
    readlinkat = 17,
    flock = 18,
    fchmod = 19,
    copy_range = 20,
    clone_file = 21,
    close = 22,
    identity_check = 23,
    /// The Linux fast path of a `beneath` location: openat2 over the whole relative path in one call. A
    /// scripted ENOSYS, EINVAL, ENOTSUP or EOPNOTSUPP on this step makes the engine walk the path instead.
    openat2 = 24,
    /// The Apple fast path of a `beneath` location: openat with O_NOFOLLOW_ANY over the whole relative path.
    openat_nofollow_any = 25,
    /// fchmodat: the mode set by name under a held directory (AT_SYMLINK_NOFOLLOW for the entry itself).
    fchmodat = 26,
    /// faccessat(X_OK): the search-permission check after a search-only open that checked nothing (Linux's O_PATH).
    faccessat = 27,
};

static_assert(static_cast<std::int32_t>(Step::open) == bc_file_system_step_open);
static_assert(static_cast<std::int32_t>(Step::openat) == bc_file_system_step_openat);
static_assert(static_cast<std::int32_t>(Step::fstat) == bc_file_system_step_fstat);
static_assert(static_cast<std::int32_t>(Step::fstatat) == bc_file_system_step_fstatat);
static_assert(static_cast<std::int32_t>(Step::read) == bc_file_system_step_read);
static_assert(static_cast<std::int32_t>(Step::pread) == bc_file_system_step_pread);
static_assert(static_cast<std::int32_t>(Step::write) == bc_file_system_step_write);
static_assert(static_cast<std::int32_t>(Step::fsync) == bc_file_system_step_fsync);
static_assert(static_cast<std::int32_t>(Step::fcntl_fullfsync) == bc_file_system_step_fcntl_fullfsync);
static_assert(static_cast<std::int32_t>(Step::renameat) == bc_file_system_step_renameat);
static_assert(static_cast<std::int32_t>(Step::rename_no_replace) == bc_file_system_step_rename_no_replace);
static_assert(static_cast<std::int32_t>(Step::unlinkat) == bc_file_system_step_unlinkat);
static_assert(static_cast<std::int32_t>(Step::mkdirat) == bc_file_system_step_mkdirat);
static_assert(static_cast<std::int32_t>(Step::fdopendir) == bc_file_system_step_fdopendir);
static_assert(static_cast<std::int32_t>(Step::readdir) == bc_file_system_step_readdir);
static_assert(static_cast<std::int32_t>(Step::symlinkat) == bc_file_system_step_symlinkat);
static_assert(static_cast<std::int32_t>(Step::readlinkat) == bc_file_system_step_readlinkat);
static_assert(static_cast<std::int32_t>(Step::flock) == bc_file_system_step_flock);
static_assert(static_cast<std::int32_t>(Step::fchmod) == bc_file_system_step_fchmod);
static_assert(static_cast<std::int32_t>(Step::copy_range) == bc_file_system_step_copy_range);
static_assert(static_cast<std::int32_t>(Step::clone_file) == bc_file_system_step_clone_file);
static_assert(static_cast<std::int32_t>(Step::close) == bc_file_system_step_close);
static_assert(static_cast<std::int32_t>(Step::identity_check) == bc_file_system_step_identity_check);
static_assert(static_cast<std::int32_t>(Step::openat2) == bc_file_system_step_openat2);
static_assert(static_cast<std::int32_t>(Step::openat_nofollow_any) == bc_file_system_step_openat_nofollow_any);
static_assert(static_cast<std::int32_t>(Step::fchmodat) == bc_file_system_step_fchmodat);
static_assert(static_cast<std::int32_t>(Step::faccessat) == bc_file_system_step_faccessat);

/// Which of the two locations of `rename` and `copy_file` an error belongs to: the source is the first, the
/// destination the second. The values are the header's.
enum class FailedLocation : std::int32_t {
    first = 1,
    second = 2,
};

/// Why an operation failed. A plain value: no inheritance, and no source location, because a library does not
/// decide its caller's diagnostics. The core reports enums and integers; the face composes `path` from the
/// `Location` it holds and renders text on request.
struct Error {
    ErrorKind kind;
    Operation operation;
    /// The system call that failed; empty for a refusal made before any system call.
    std::optional<Step> step;
    /// The errno of the system call that failed, or 0 when the failure was not a system call's.
    int system_code;
    /// The full path the failing step acted on: the location's text (for `beneath`, the directory's opened-as
    /// text, a slash and the relative path), then a slash and the entry's name when the step acted on an entry
    /// inside it. Empty for a call that took no location. For reporting, never for a retry.
    std::string path;
    /// True when the entry's name was longer than the core's bounded name field, so `path` ends early.
    bool name_truncated;
    /// Which end of a two-location operation the failure belongs to, and so which location `path` was composed
    /// from; empty for an operation over one location and for a refusal of the arguments, which blames neither.
    std::optional<FailedLocation> failed_location;

    /// "<operation>: <path>: <the system's message, or the kind's name>"; the path segment is left out when
    /// the call took no location.
    [[nodiscard]] auto message() const -> std::string;

    [[nodiscard]] friend auto operator==(const Error&, const Error&) -> bool = default;
};

/// The name as the enum spells it ("not_found", "read_file", "fcntl_fullfsync"), for log lines and tests.
[[nodiscard]] auto name_of(ErrorKind kind) noexcept -> std::string_view;
[[nodiscard]] auto name_of(Operation operation) noexcept -> std::string_view;
[[nodiscard]] auto name_of(Step step) noexcept -> std::string_view;

/// The enum a value from the C boundary names, or empty when the value is outside the enum: the one place a
/// foreign integer is refused, so every switch above it is exhaustive.
[[nodiscard]] auto error_kind_from(std::int32_t value) noexcept -> std::optional<ErrorKind>;
[[nodiscard]] auto operation_from(std::int32_t value) noexcept -> std::optional<Operation>;
[[nodiscard]] auto step_from(std::int32_t value) noexcept -> std::optional<Step>;
/// Empty for 0 (an operation over one location) and for a value outside the enum.
[[nodiscard]] auto failed_location_from(std::int32_t value) noexcept -> std::optional<FailedLocation>;

/// The face's one conversion of a C error, with the full path composed from `location_text` (empty for a call
/// that took no location) and the entry name the core reported. The core names the operation itself; `called`
/// is the operation this face invoked and stands in only if the core's value were outside the enum, which the
/// pins above make impossible for a core and a face built together.
[[nodiscard]] auto error_from(const bc_file_system_error& c_error, Operation called, std::string_view location_text)
    -> Error;

/// The same conversion for an operation over two locations (`rename`, `copy_file`): the path is composed from
/// the location the core's `failed_location` names, the first (the source) or the second (the destination), and
/// from the second when it names neither, which only a refusal of the arguments does.
[[nodiscard]] auto error_from_two_locations(const bc_file_system_error& c_error, Operation called,
                                            std::string_view first_location_text, std::string_view second_location_text)
    -> Error;

} // namespace bc::file_system
