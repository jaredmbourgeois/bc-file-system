#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The tests' own way to the file system, and the only code in the tests that does not go through the library.
// A test sets a case up with these and checks the outcome with these, so that what it asserts was measured
// independently of the code under test. Everything here acts inside a directory the test created under the
// root the build handed it (the generated test_paths.hpp); nothing here asks the system where temporary files
// go.
//
// A helper that cannot do its job records a test failure and returns an empty value.

namespace bc::file_system_test {

/// A directory a test created under the root it was handed, removed again when the test ends.
class TemporaryDirectory {
public:
    /// Creates a fresh, uniquely named directory inside `root`. Empty when it could not be created.
    [[nodiscard]] static auto create_under(std::string_view root) -> std::optional<TemporaryDirectory>;

    /// Removes the directory and everything in it, first giving back access to anything a test locked.
    ~TemporaryDirectory();

    TemporaryDirectory(TemporaryDirectory&& other) noexcept;
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    auto operator=(const TemporaryDirectory&) -> TemporaryDirectory& = delete;
    auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

    [[nodiscard]] auto path() const noexcept -> const std::filesystem::path&;

private:
    explicit TemporaryDirectory(std::filesystem::path path) noexcept;

    std::filesystem::path path_;
};

/// What the system records as an entry's identity: the device and the inode number, which the library reports
/// in a Status and compares in its own identity check.
struct Identity {
    std::uint64_t device;
    std::uint64_t inode;

    [[nodiscard]] friend auto operator==(const Identity&, const Identity&) -> bool = default;
};

/// Creates or replaces the file at `path` with `text`, with exactly `mode`.
auto write_raw(const std::filesystem::path& path, std::string_view text, std::uint32_t mode) -> void;

/// The file's content.
[[nodiscard]] auto read_raw(const std::filesystem::path& path) -> std::string;

/// Creates one directory with exactly `mode`.
auto make_directory_raw(const std::filesystem::path& path, std::uint32_t mode) -> void;

/// Creates the symlink `link` pointing at `target`.
auto make_symlink_raw(const std::filesystem::path& target, const std::filesystem::path& link) -> void;

auto set_mode_raw(const std::filesystem::path& path, std::uint32_t mode) -> void;

/// The permission bits of `path` itself (a symlink is not followed).
[[nodiscard]] auto mode_raw(const std::filesystem::path& path) -> std::uint32_t;

/// The identity of `path` itself (a symlink is not followed).
[[nodiscard]] auto identity_raw(const std::filesystem::path& path) -> Identity;

/// Whether anything is at `path` (a symlink counts, whatever it points at).
[[nodiscard]] auto exists_raw(const std::filesystem::path& path) -> bool;

/// Every name directly inside `directory`, hidden ones included, sorted.
[[nodiscard]] auto names_raw(const std::filesystem::path& directory) -> std::vector<std::string>;

/// False when the process may read what its permission bits forbid, as the superuser may. A test of a
/// permission failure skips itself there, because no such failure can happen. `scratch` is a directory the
/// test owns; the probe file is created and removed inside it.
[[nodiscard]] auto permissions_are_enforced(const std::filesystem::path& scratch) -> bool;

/// While one of these lives, no file this process writes may grow past `byte_limit`: the system refuses the
/// write that would cross it, which is how a test meets a real "too large" failure without filling a disk. The
/// previous limit comes back when it is destroyed.
class FileSizeLimit {
public:
    explicit FileSizeLimit(std::uint64_t byte_limit);
    ~FileSizeLimit();

    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit(FileSizeLimit&&) = delete;
    auto operator=(const FileSizeLimit&) -> FileSizeLimit& = delete;
    auto operator=(FileSizeLimit&&) -> FileSizeLimit& = delete;

private:
    std::uint64_t previous_limit_;
    bool applied_;
};

/// The text as the bytes the library takes.
[[nodiscard]] auto bytes_of(std::string_view text) -> std::vector<std::byte>;

/// The bytes the library returned, as text.
[[nodiscard]] auto text_of(std::span<const std::byte> bytes) -> std::string;

} // namespace bc::file_system_test
