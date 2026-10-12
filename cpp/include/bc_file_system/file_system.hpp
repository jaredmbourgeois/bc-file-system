#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/engine.hpp>
#include <bc_file_system/error.hpp>
#include <bc_file_system/location.hpp>
#include <bc_file_system/options.hpp>
#include <bc_file_system/result.hpp>
#include <bc_file_system/status.hpp>

namespace bc::file_system {

/// The answer of `try_lock`.
enum class LockOutcome : std::int32_t {
    acquired = 1,
    /// Another process or descriptor holds the lock; nothing was waited for.
    held_elsewhere = 2,
};

static_assert(static_cast<std::int32_t>(LockOutcome::acquired) == bc_file_system_lock_outcome_acquired);
static_assert(static_cast<std::int32_t>(LockOutcome::held_elsewhere) == bc_file_system_lock_outcome_held_elsewhere);

/// The outcome a value from the C boundary names, or empty when the value is outside the enum.
[[nodiscard]] constexpr auto lock_outcome_from(std::int32_t value) noexcept -> std::optional<LockOutcome> {
    if (value >= bc_file_system_lock_outcome_acquired && value <= bc_file_system_lock_outcome_held_elsewhere) {
        return static_cast<LockOutcome>(value);
    }
    return std::nullopt;
}

/// An advisory lock held on a file, let go when it goes out of scope. Move-only over the C handle; taken only
/// by `FileSystem::try_lock`.
class Lock {
public:
    Lock(Lock&& other) noexcept;
    auto operator=(Lock&& other) noexcept -> Lock&;
    Lock(const Lock&) = delete;
    auto operator=(const Lock&) = delete;
    ~Lock();

    /// The C handle. Never null while the object lives.
    [[nodiscard]] auto handle() const noexcept -> bc_file_system_lock* { return handle_; }

private:
    friend class FileSystem;

    explicit Lock(bc_file_system_lock* handle) noexcept;

    bc_file_system_lock* handle_;
};

/// The lines of a read buffer, each a view into it without its newline: split on `\n` only, the bytes
/// untouched (a `\r` stays where it was; the library never decides what text the bytes are). A final line
/// without a newline is a line; a trailing newline does not add an empty one; an empty buffer has no lines.
[[nodiscard]] auto lines_of(std::span<const std::byte> buffer) -> std::vector<std::string_view>;

/// The file system a piece of code is handed. Holds the engine it runs in; each method is one call into the
/// C core plus the answer's conversion, and nothing here is virtual: the double lives below the core, behind
/// the engine's observer, so there is nothing to substitute on this side.
///
/// Every operation states everything it needs: there is no default durability, no default mode and no
/// convenience overload. No operation throws; a failure is the `Error` in the result, with the full path
/// composed from the `Location` given.
class FileSystem {
public:
    /// `engine` must outlive the file system.
    explicit FileSystem(Engine& engine) noexcept;

    [[nodiscard]] auto engine() const noexcept -> Engine& { return engine_; }

    /// Opens the directory at `location`, remembering its text; something that is not a directory is
    /// `not_directory`; where the platform cannot enforce containment beneath it, `unsupported`.
    [[nodiscard]] auto open_directory(const Location& location, FinalSymlink final_symlink) noexcept
        -> Result<Directory>;

    /// What is at `location`, or an empty optional when nothing is: absence is a value here, not an error.
    [[nodiscard]] auto status(const Location& location, FinalSymlink final_symlink) noexcept
        -> Result<std::optional<Status>>;

    /// The bytes of a regular file, under the read limit. Anything that is not a regular file is
    /// `not_regular_file`. The buffer protocol is absorbed here: at most three attempts, then the error as
    /// received.
    [[nodiscard]] auto read_file(const Location& location, const ReadOptions& options) noexcept
        -> Result<std::vector<std::byte>>;

    /// `read_file`, with absence as an empty optional instead of `not_found`.
    [[nodiscard]] auto read_file_if_present(const Location& location, const ReadOptions& options) noexcept
        -> Result<std::optional<std::vector<std::byte>>>;

    /// Replaces or creates the regular file at `location` with `bytes`, atomically: a reader sees the old
    /// content or the new, never a mixture. The directory must exist. Returns the durability achieved.
    [[nodiscard]] auto write_file(const Location& location, std::span<const std::byte> bytes,
                                  const WriteOptions& options) noexcept -> Result<Durability>;

    /// Adds `bytes` to the end of the file at `location`, creating it with `permissions` when it is absent.
    /// Not atomic: after an error, some leading part of the bytes may be in the file.
    [[nodiscard]] auto append(const Location& location, std::span<const std::byte> bytes, Durability durability,
                              Permissions permissions) noexcept -> Result<Durability>;

    /// Pushes the file or directory at `location` to stable storage; a volume with no sync answers `written`.
    [[nodiscard]] auto sync(const Location& location) noexcept -> Result<Durability>;

    /// Creates one directory; one already there is `already_exists`; a missing parent is `not_found`.
    [[nodiscard]] auto create_directory(const Location& location, Permissions permissions) noexcept -> Result<void>;

    /// Creates the directory and every missing one on the way; one already there is left as it is.
    [[nodiscard]] auto create_directories(const Location& location, Permissions permissions) noexcept -> Result<void>;

    /// The entries of a directory, sorted by name so a listing is the same on every run; complete or an error.
    [[nodiscard]] auto list_directory(const Location& location, FinalSymlink final_symlink) noexcept
        -> Result<std::vector<Entry>>;

    /// Removes the non-directory at `location`; returns whether there was one. A directory is
    /// `not_regular_file`.
    [[nodiscard]] auto remove_file(const Location& location) noexcept -> Result<bool>;

    /// Removes the empty directory at `location`; returns whether there was one.
    [[nodiscard]] auto remove_directory(const Location& location) noexcept -> Result<bool>;

    /// Removes `location` and everything below it, never following a symlink, never the root of the file
    /// system, never across a device; returns whether there was anything.
    [[nodiscard]] auto remove_tree(const Location& location) noexcept -> Result<bool>;

    /// Creates a directory with a unique name inside `parent` and returns that name, one component.
    [[nodiscard]] auto create_temporary_directory(const Location& parent, Permissions permissions) noexcept
        -> Result<std::string>;

    /// Renames `source` to `destination` in one step; see the header for what `replace` replaces. Returns the
    /// durability achieved. An error names `destination`.
    [[nodiscard]] auto rename(const Location& source, const Location& destination,
                              ExistingDestination existing_destination, Durability durability) noexcept
        -> Result<Durability>;

    /// Copies the regular file at `source` to `destination` with the guarantees of `write_file`. An error names
    /// `destination`.
    [[nodiscard]] auto copy_file(const Location& source, const Location& destination,
                                 const WriteOptions& options) noexcept -> Result<Durability>;

    /// Sets the permission bits of the entry at `location` exactly to `mode`.
    [[nodiscard]] auto set_permissions(const Location& location, std::uint32_t mode,
                                       FinalSymlink final_symlink) noexcept -> Result<void>;

    /// Creates a symlink at `link` whose text is `target`, passed to the system unchanged.
    [[nodiscard]] auto create_symlink(std::string_view target, const Location& link) noexcept -> Result<void>;

    /// The text of the symlink at `location`.
    [[nodiscard]] auto read_symlink(const Location& location) noexcept -> Result<std::string>;

    /// Tries to take an advisory exclusive lock on the file at `location` without waiting: the lock when it
    /// was acquired, an empty optional when it is held elsewhere.
    [[nodiscard]] auto try_lock(const Location& location) noexcept -> Result<std::optional<Lock>>;

    /// Renames the tree at `location` to `.bc-condemned-<name>` beside it, for a later sweep, and returns the
    /// condemned name.
    [[nodiscard]] auto condemn(const Location& location) noexcept -> Result<std::string>;

    /// Removes every `.bc-condemned-` tree directly inside `parent`.
    [[nodiscard]] auto remove_condemned(const Location& parent) noexcept -> Result<void>;

private:
    Engine& engine_;
};

} // namespace bc::file_system
