#include <bc_file_system/error.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace bc::file_system {

auto name_of(ErrorKind kind) noexcept -> std::string_view {
    switch (kind) {
    case ErrorKind::not_found:
        return "not_found";
    case ErrorKind::not_regular_file:
        return "not_regular_file";
    case ErrorKind::not_directory:
        return "not_directory";
    case ErrorKind::permission_denied:
        return "permission_denied";
    case ErrorKind::already_exists:
        return "already_exists";
    case ErrorKind::directory_not_empty:
        return "directory_not_empty";
    case ErrorKind::symlink_refused:
        return "symlink_refused";
    case ErrorKind::too_large:
        return "too_large";
    case ErrorKind::cross_device:
        return "cross_device";
    case ErrorKind::no_space:
        return "no_space";
    case ErrorKind::resource_exhausted:
        return "resource_exhausted";
    case ErrorKind::unsupported:
        return "unsupported";
    case ErrorKind::invalid_argument:
        return "invalid_argument";
    case ErrorKind::io_failed:
        return "io_failed";
    case ErrorKind::buffer_too_small:
        return "buffer_too_small";
    }
    // Reached only by a value cast from outside the enum.
    return "unknown";
}

auto name_of(Operation operation) noexcept -> std::string_view {
    switch (operation) {
    case Operation::engine_create_live:
        return "engine_create_live";
    case Operation::engine_create_observed_for_tests:
        return "engine_create_observed_for_tests";
    case Operation::open_directory:
        return "open_directory";
    case Operation::directory_opened_as:
        return "directory_opened_as";
    case Operation::observed_fault_add:
        return "observed_fault_add";
    case Operation::observed_faults_clear:
        return "observed_faults_clear";
    case Operation::observed_step_count:
        return "observed_step_count";
    case Operation::status:
        return "status";
    case Operation::read_file:
        return "read_file";
    case Operation::write_file:
        return "write_file";
    case Operation::append:
        return "append";
    case Operation::sync:
        return "sync";
    case Operation::create_directory:
        return "create_directory";
    case Operation::create_directories:
        return "create_directories";
    case Operation::list_directory:
        return "list_directory";
    case Operation::remove_file:
        return "remove_file";
    case Operation::remove_directory:
        return "remove_directory";
    case Operation::remove_tree:
        return "remove_tree";
    case Operation::create_temporary_directory:
        return "create_temporary_directory";
    case Operation::rename:
        return "rename";
    case Operation::copy_file:
        return "copy_file";
    case Operation::set_permissions:
        return "set_permissions";
    case Operation::create_symlink:
        return "create_symlink";
    case Operation::read_symlink:
        return "read_symlink";
    case Operation::try_lock:
        return "try_lock";
    case Operation::condemn:
        return "condemn";
    case Operation::remove_condemned:
        return "remove_condemned";
    case Operation::relative_path_from:
        return "relative_path_from";
    case Operation::absolute_path_from:
        return "absolute_path_from";
    }
    return "unknown";
}

auto name_of(Step step) noexcept -> std::string_view {
    switch (step) {
    case Step::open:
        return "open";
    case Step::openat:
        return "openat";
    case Step::fstat:
        return "fstat";
    case Step::fstatat:
        return "fstatat";
    case Step::read:
        return "read";
    case Step::pread:
        return "pread";
    case Step::write:
        return "write";
    case Step::fsync:
        return "fsync";
    case Step::fcntl_fullfsync:
        return "fcntl_fullfsync";
    case Step::renameat:
        return "renameat";
    case Step::rename_no_replace:
        return "rename_no_replace";
    case Step::unlinkat:
        return "unlinkat";
    case Step::mkdirat:
        return "mkdirat";
    case Step::fdopendir:
        return "fdopendir";
    case Step::readdir:
        return "readdir";
    case Step::symlinkat:
        return "symlinkat";
    case Step::readlinkat:
        return "readlinkat";
    case Step::flock:
        return "flock";
    case Step::fchmod:
        return "fchmod";
    case Step::copy_range:
        return "copy_range";
    case Step::clone_file:
        return "clone_file";
    case Step::close:
        return "close";
    case Step::identity_check:
        return "identity_check";
    }
    return "unknown";
}

// Every enum in the header runs from 1 to its last value without a gap, so membership is a range check against
// the pinned first and last enumerators.
auto error_kind_from(std::int32_t value) noexcept -> std::optional<ErrorKind> {
    if (value >= bc_file_system_error_kind_not_found && value <= bc_file_system_error_kind_buffer_too_small) {
        return static_cast<ErrorKind>(value);
    }
    return std::nullopt;
}

auto operation_from(std::int32_t value) noexcept -> std::optional<Operation> {
    if (value >= bc_file_system_operation_engine_create_live && value <= bc_file_system_operation_absolute_path_from) {
        return static_cast<Operation>(value);
    }
    return std::nullopt;
}

auto step_from(std::int32_t value) noexcept -> std::optional<Step> {
    if (value >= bc_file_system_step_open && value <= bc_file_system_step_identity_check) {
        return static_cast<Step>(value);
    }
    return std::nullopt;
}

auto error_from(const bc_file_system_error& c_error, Operation called, std::string_view location_text) -> Error {
    // The core writes only values of its own enums, and the pins hold the two sides equal; an unknown kind is
    // still answered honestly as "a system failure we could not classify" rather than ending the process.
    const std::optional<ErrorKind> kind = error_kind_from(c_error.kind);
    const std::optional<Operation> operation = operation_from(c_error.operation);

    std::string path(location_text);
    if (c_error.name_length > 0) {
        const std::string_view name(reinterpret_cast<const char*>(c_error.name),
                                    c_error.name_length < sizeof(c_error.name) ? c_error.name_length
                                                                               : sizeof(c_error.name));
        if (!path.empty()) {
            path += '/';
        }
        path += name;
    }

    return Error{.kind = kind.has_value() ? *kind : ErrorKind::io_failed,
                 .operation = operation.has_value() ? *operation : called,
                 .step = step_from(c_error.step),
                 .system_code = c_error.system_code,
                 .path = std::move(path),
                 .name_truncated = c_error.name_truncated != 0};
}

auto Error::message() const -> std::string {
    std::string text(name_of(operation));
    text += ": ";
    if (!path.empty()) {
        text += path;
        text += ": ";
    }
    if (system_code != 0) {
        // The system's own wording for the errno, through the standard library rather than strerror, which is
        // not safe to call from two threads at once.
        text += std::system_category().message(system_code);
    } else {
        text += name_of(kind);
    }
    return text;
}

} // namespace bc::file_system
