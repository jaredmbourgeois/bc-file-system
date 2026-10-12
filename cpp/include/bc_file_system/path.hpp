#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/location.hpp>
#include <bc_file_system/result.hpp>

namespace bc::file_system {

/// Where one path stands with respect to another, lexically (`relate`). Advisory: a lexical answer knows
/// nothing of symlinks or the disk; `Location::beneath` is what guards an access. The values are the header's.
enum class PathRelation : std::int32_t {
    /// The path names something strictly beneath the other.
    inside = 1,
    same = 2,
    /// Neither beneath the other nor the other itself; the other being beneath the path is `outside` too.
    outside = 3,
    /// One path is absolute and the other relative, or one of them climbs out of its own start with `..`.
    undecidable = 4,
};

static_assert(static_cast<std::int32_t>(PathRelation::inside) == bc_file_system_path_relation_inside);
static_assert(static_cast<std::int32_t>(PathRelation::same) == bc_file_system_path_relation_same);
static_assert(static_cast<std::int32_t>(PathRelation::outside) == bc_file_system_path_relation_outside);
static_assert(static_cast<std::int32_t>(PathRelation::undecidable) == bc_file_system_path_relation_undecidable);

/// What a component spells when it spells a library file (`library_name`). `none` is the core's answer for a
/// component that is not one; the face reports that as an empty optional and never builds a `LibraryName`
/// with it.
enum class LibraryKind : std::int32_t {
    none = 1,
    /// `libX.so`, `libX.so.1.2`, `libX.dylib`, `X.dll`.
    shared_library = 2,
    /// `libX.a`.
    static_library = 3,
    /// `X.framework`.
    framework = 4,
};

static_assert(static_cast<std::int32_t>(LibraryKind::none) == bc_file_system_library_kind_none);
static_assert(static_cast<std::int32_t>(LibraryKind::shared_library) == bc_file_system_library_kind_shared);
static_assert(static_cast<std::int32_t>(LibraryKind::static_library) == bc_file_system_library_kind_static);
static_assert(static_cast<std::int32_t>(LibraryKind::framework) == bc_file_system_library_kind_framework);

/// The name as the enum spells it, for log lines and tests.
[[nodiscard]] auto name_of(PathRelation relation) noexcept -> std::string_view;
[[nodiscard]] auto name_of(LibraryKind kind) noexcept -> std::string_view;

/// The enum a value from the C boundary names, or empty when the value is outside the enum.
[[nodiscard]] auto path_relation_from(std::int32_t value) noexcept -> std::optional<PathRelation>;
[[nodiscard]] auto library_kind_from(std::int32_t value) noexcept -> std::optional<LibraryKind>;

/// A component recognised as a library file: its kind, never `none`, and the library's own name, `z` in
/// `libz.so.1`.
struct LibraryName {
    LibraryKind kind;
    std::string stem;

    [[nodiscard]] friend auto operator==(const LibraryName&, const LibraryName&) -> bool = default;
};

// The path module through the C++ value types. Each function is one call into the core (the typed overloads
// pass the value's bytes), and the rules are the public header's, under "Path text": nothing here consults the
// disk, the working directory or the environment. A refusal is an Error carrying the operation the core named,
// no step, no system code, and as its `path` the bytes the core refused: the loose argument's where a function
// takes one, the first path's otherwise. A typed value was validated when it was made, so only the loose
// arguments (`std::string_view`) are ever refused.

/// `base`, a slash and `relative`; the root already ends in its slash. The answer is a path of `base`'s kind.
[[nodiscard]] auto join(const AbsolutePath& base, const RelativePath& relative) noexcept -> Result<AbsolutePath>;
[[nodiscard]] auto join(const RelativePath& base, const RelativePath& relative) noexcept -> Result<RelativePath>;

/// The parent, a prefix of the path: `/` for `/a`, `a/b` for `a/b/c`. The root and a one-component relative
/// path have none.
[[nodiscard]] auto parent(const AbsolutePath& path) noexcept -> Result<std::optional<AbsolutePath>>;
[[nodiscard]] auto parent(const RelativePath& path) noexcept -> Result<std::optional<RelativePath>>;

/// The last component's bytes. The root has no component and is refused with `invalid_argument`.
[[nodiscard]] auto last_component(const AbsolutePath& path) noexcept -> Result<std::string>;
[[nodiscard]] auto last_component(const RelativePath& path) noexcept -> Result<std::string>;

/// Every component's bytes, in order; none for the root.
[[nodiscard]] auto components(const AbsolutePath& path) noexcept -> Result<std::vector<std::string>>;
[[nodiscard]] auto components(const RelativePath& path) noexcept -> Result<std::vector<std::string>>;

/// The bytes of one component before its extension, or all of them when it has none.
[[nodiscard]] auto stem(std::string_view component) noexcept -> Result<std::string>;

/// The bytes after a component's last dot, when that dot is neither its first byte (`.bashrc` has none) nor
/// its last (`archive.` has none); the dot itself is excluded.
[[nodiscard]] auto extension(std::string_view component) noexcept -> Result<std::optional<std::string>>;

/// Where `path` stands with respect to `other`, both normalised first. The loose form takes any text the
/// module normalises; two kinds, or a path that still begins with `..`, are `undecidable`.
[[nodiscard]] auto relate(const AbsolutePath& path, const AbsolutePath& other) noexcept -> Result<PathRelation>;
[[nodiscard]] auto relate(const RelativePath& path, const RelativePath& other) noexcept -> Result<PathRelation>;
[[nodiscard]] auto relate(std::string_view path, std::string_view other) noexcept -> Result<PathRelation>;

/// Loose text reduced to one spelling: doubled slashes collapse, `.` components go, `a/..` folds away, a
/// trailing slash goes, `/..` is `/`, a leading `..` of a relative path stays, and a relative path with nothing
/// left is `.`. Idempotent; the disk is never consulted.
[[nodiscard]] auto lexically_normalize(std::string_view text) noexcept -> Result<std::string>;

/// `relative`, loose text with no leading slash, joined to `base` and normalised: how a composition root turns
/// a command-line argument into an absolute path against the working directory it was handed.
[[nodiscard]] auto resolve_against(const AbsolutePath& base, std::string_view relative) noexcept
    -> Result<AbsolutePath>;

/// The library a component names, when it spells one the way a platform does (`libX.so`, `libX.so.1.2`,
/// `libX.dylib`, `libX.a`, `X.framework`, `X.dll`, byte for byte), or empty.
[[nodiscard]] auto library_name(std::string_view component) noexcept -> Result<std::optional<LibraryName>>;

} // namespace bc::file_system
