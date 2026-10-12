#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/result.hpp>

namespace bc::file_system {

class FileSystem;

/// Path text relative to a directory, as bytes, one spelling per path so equality is a byte compare.
///
/// Validated once at construction by the path module in C (`bc_file_system_relative_path_validate`): at least
/// one component, no NUL byte, no leading slash, no empty component (`a//b`, a trailing slash), no `.` and no
/// `..` component, no component over 255 bytes. Nothing on disk is consulted. The free functions over path
/// values are in `<bc_file_system/path.hpp>`.
class RelativePath {
public:
    [[nodiscard]] static auto from(std::string_view bytes) -> Result<RelativePath>;

    [[nodiscard]] auto bytes() const noexcept -> std::string_view { return bytes_; }

    [[nodiscard]] friend auto operator==(const RelativePath&, const RelativePath&) -> bool = default;

private:
    explicit RelativePath(std::string bytes) noexcept : bytes_(std::move(bytes)) {}

    std::string bytes_;
};

/// Absolute path text, as bytes. `..` is allowed and reported by the path module, never resolved here.
///
/// Validated once at construction by the path module in C (`bc_file_system_absolute_path_validate`): a leading
/// slash, `/` alone the root, otherwise the relative rules with `..` allowed.
class AbsolutePath {
public:
    [[nodiscard]] static auto from(std::string_view bytes) -> Result<AbsolutePath>;

    [[nodiscard]] auto bytes() const noexcept -> std::string_view { return bytes_; }

    [[nodiscard]] friend auto operator==(const AbsolutePath&, const AbsolutePath&) -> bool = default;

private:
    explicit AbsolutePath(std::string bytes) noexcept : bytes_(std::move(bytes)) {}

    std::string bytes_;
};

/// An open directory the kernel enforces containment beneath, which remembers the text it was opened with so
/// that an error beneath it reads as that text, a slash and the relative path. Move-only over the C handle;
/// released when it goes out of scope. Opened only by `FileSystem::open_directory`.
class Directory {
public:
    Directory(Directory&& other) noexcept;
    auto operator=(Directory&& other) noexcept -> Directory&;
    Directory(const Directory&) = delete;
    auto operator=(const Directory&) = delete;
    ~Directory();

    /// The text the directory was opened with, read from the core (the face keeps a copy for composing error
    /// paths; this call is the binding's).
    [[nodiscard]] auto opened_as() const -> Result<std::string>;

    /// The C handle, for the face's calls. Never null while the object lives.
    [[nodiscard]] auto handle() const noexcept -> bc_file_system_directory* { return handle_; }

private:
    friend class FileSystem;
    friend class Location; // reads the opened-as copy to compose a `beneath` location's text

    /// Adopts a handle the core created. `opened_as_text` is what the face composed for the location it opened,
    /// the same bytes the core remembers.
    Directory(bc_file_system_directory* handle, std::string opened_as_text) noexcept;

    bc_file_system_directory* handle_;
    std::string opened_as_text_;
};

enum class LocationKind : std::int32_t {
    at = 1,
    beneath = 2,
};

static_assert(static_cast<std::int32_t>(LocationKind::at) == bc_file_system_location_kind_at);
static_assert(static_cast<std::int32_t>(LocationKind::beneath) == bc_file_system_location_kind_beneath);

/// Where an operation acts: an absolute path the system resolves, or a relative path beneath a `Directory`
/// with containment enforced by the kernel. One sum type, so every operation has one signature.
class Location {
public:
    /// An absolute path the system resolves. The path's bytes go to the core unchanged; a relative path is
    /// refused there, by the operation, with `invalid_argument`.
    [[nodiscard]] static auto at(const std::filesystem::path& path) -> Location;
    [[nodiscard]] static auto at(const AbsolutePath& path) -> Location;

    /// A relative path beneath `directory`, which must outlive the location.
    [[nodiscard]] static auto beneath(const Directory& directory, const RelativePath& relative) -> Location;

    [[nodiscard]] auto kind() const noexcept -> LocationKind { return kind_; }

    /// The text this location stands for, as a face reports it: the absolute bytes, or the directory's
    /// opened-as text, a slash and the relative bytes.
    [[nodiscard]] auto text() const -> std::string;

    /// The C struct, borrowing this location's bytes: valid only while this object lives.
    [[nodiscard]] auto to_c() const noexcept -> bc_file_system_location;

private:
    Location(LocationKind kind, const Directory* directory, std::string bytes) noexcept;

    LocationKind kind_;
    const Directory* directory_; /// the `beneath` directory; null for `at`
    std::string bytes_;          /// absolute for `at`, relative for `beneath`
};

} // namespace bc::file_system
