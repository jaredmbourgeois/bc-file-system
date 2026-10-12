#include <bc_file_system/path.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/error.hpp>
#include <bc_file_system/location.hpp>
#include <bc_file_system/result.hpp>

// The path module's C++ face: each function is one call into the core plus the conversion of its answer into a
// value type, a string or an enum. The face holds no rule about path text; a typed answer (an AbsolutePath, a
// RelativePath) is made through the value type's own constructor, which validates it in C once more, so the
// face never holds bytes the core did not accept.

namespace bc::file_system {

namespace {

auto bytes_of(std::string_view text) noexcept -> const std::uint8_t* {
    return reinterpret_cast<const std::uint8_t*>(text.data());
}

// The core answered with a value outside its own enum, which a core and a face built together cannot do (the
// pins hold them equal). Reported as a failure rather than trusted or aborted on.
auto answer_outside_enum(Operation called, std::string_view text) -> Error {
    return Error{.kind = ErrorKind::io_failed,
                 .operation = called,
                 .step = std::nullopt,
                 .system_code = 0,
                 .path = std::string(text),
                 .name_truncated = false};
}

// The buffer protocol for a pure function: the size first (a capacity of 0), then exactly that much. A path
// answer cannot change between the two calls, so there is no grow loop here. `call(buffer, capacity, &length,
// &c_error)` is the core's function with everything but the buffer bound; `refused_text` is what a refusal
// names as its path.
template <class Call>
auto answered_text(Operation called, std::string_view refused_text, const Call& call) -> Result<std::string> {
    std::size_t needed = 0;
    bc_file_system_error c_error;
    const std::int32_t sized = call(nullptr, std::size_t{0}, &needed, &c_error);
    if (sized == 0) {
        return std::string{};
    }
    if (sized != bc_file_system_error_kind_buffer_too_small) {
        return error_from(c_error, called, refused_text);
    }
    std::string answer(needed, '\0');
    std::size_t length = 0;
    const std::int32_t kind = call(reinterpret_cast<std::uint8_t*>(answer.data()), answer.size(), &length, &c_error);
    if (kind != 0) {
        return error_from(c_error, called, refused_text);
    }
    answer.resize(length);
    return answer;
}

auto join_text(std::string_view base, std::string_view relative) -> Result<std::string> {
    return answered_text(
        Operation::path_join, base,
        [&](std::uint8_t* buffer, std::size_t capacity, std::size_t* length, bc_file_system_error* c_error) {
            return bc_file_system_path_join(bytes_of(base), base.size(), bytes_of(relative), relative.size(), buffer,
                                            capacity, length, c_error);
        });
}

// The parent as a prefix of `path`, or empty when there is none.
auto parent_text(std::string_view path) -> Result<std::optional<std::string_view>> {
    std::int32_t present = 0;
    std::size_t parent_length = 0;
    bc_file_system_error c_error;
    if (bc_file_system_path_parent(bytes_of(path), path.size(), &present, &parent_length, &c_error) != 0) {
        return error_from(c_error, Operation::path_parent, path);
    }
    if (present == 0) {
        return std::optional<std::string_view>{};
    }
    return std::optional<std::string_view>{path.substr(0, parent_length)};
}

auto last_component_text(std::string_view path) -> Result<std::string> {
    std::size_t offset = 0;
    std::size_t component_length = 0;
    bc_file_system_error c_error;
    if (bc_file_system_path_last_component(bytes_of(path), path.size(), &offset, &component_length, &c_error) != 0) {
        return error_from(c_error, Operation::path_last_component, path);
    }
    return std::string(path.substr(offset, component_length));
}

auto components_text(std::string_view path) -> Result<std::vector<std::string>> {
    std::vector<std::string> found;
    std::size_t cursor = 0;
    for (;;) {
        std::size_t offset = 0;
        std::size_t component_length = 0;
        std::int32_t present = 0;
        bc_file_system_error c_error;
        if (bc_file_system_path_next_component(bytes_of(path), path.size(), &cursor, &offset, &component_length,
                                               &present, &c_error) != 0) {
            return error_from(c_error, Operation::path_next_component, path);
        }
        if (present == 0) {
            return found;
        }
        found.emplace_back(path.substr(offset, component_length));
    }
}

} // namespace

// ---- The enums -----------------------------------------------------------------------------------------------

auto name_of(PathRelation relation) noexcept -> std::string_view {
    switch (relation) {
    case PathRelation::inside:
        return "inside";
    case PathRelation::same:
        return "same";
    case PathRelation::outside:
        return "outside";
    case PathRelation::undecidable:
        return "undecidable";
    }
    return "unknown";
}

auto name_of(LibraryKind kind) noexcept -> std::string_view {
    switch (kind) {
    case LibraryKind::none:
        return "none";
    case LibraryKind::shared_library:
        return "shared_library";
    case LibraryKind::static_library:
        return "static_library";
    case LibraryKind::framework:
        return "framework";
    }
    return "unknown";
}

auto path_relation_from(std::int32_t value) noexcept -> std::optional<PathRelation> {
    if (value >= bc_file_system_path_relation_inside && value <= bc_file_system_path_relation_undecidable) {
        return static_cast<PathRelation>(value);
    }
    return std::nullopt;
}

auto library_kind_from(std::int32_t value) noexcept -> std::optional<LibraryKind> {
    if (value >= bc_file_system_library_kind_none && value <= bc_file_system_library_kind_framework) {
        return static_cast<LibraryKind>(value);
    }
    return std::nullopt;
}

// ---- The functions -------------------------------------------------------------------------------------------

auto join(const AbsolutePath& base, const RelativePath& relative) noexcept -> Result<AbsolutePath> {
    const Result<std::string> joined = join_text(base.bytes(), relative.bytes());
    if (!joined.has_value()) {
        return joined.error();
    }
    return AbsolutePath::from(*joined);
}

auto join(const RelativePath& base, const RelativePath& relative) noexcept -> Result<RelativePath> {
    const Result<std::string> joined = join_text(base.bytes(), relative.bytes());
    if (!joined.has_value()) {
        return joined.error();
    }
    return RelativePath::from(*joined);
}

auto parent(const AbsolutePath& path) noexcept -> Result<std::optional<AbsolutePath>> {
    const Result<std::optional<std::string_view>> prefix = parent_text(path.bytes());
    if (!prefix.has_value()) {
        return prefix.error();
    }
    if (!prefix->has_value()) {
        return std::optional<AbsolutePath>{};
    }
    Result<AbsolutePath> typed = AbsolutePath::from(**prefix);
    if (!typed.has_value()) {
        return typed.error();
    }
    return std::optional<AbsolutePath>{std::move(*typed)};
}

auto parent(const RelativePath& path) noexcept -> Result<std::optional<RelativePath>> {
    const Result<std::optional<std::string_view>> prefix = parent_text(path.bytes());
    if (!prefix.has_value()) {
        return prefix.error();
    }
    if (!prefix->has_value()) {
        return std::optional<RelativePath>{};
    }
    Result<RelativePath> typed = RelativePath::from(**prefix);
    if (!typed.has_value()) {
        return typed.error();
    }
    return std::optional<RelativePath>{std::move(*typed)};
}

auto last_component(const AbsolutePath& path) noexcept -> Result<std::string> {
    return last_component_text(path.bytes());
}

auto last_component(const RelativePath& path) noexcept -> Result<std::string> {
    return last_component_text(path.bytes());
}

auto components(const AbsolutePath& path) noexcept -> Result<std::vector<std::string>> {
    return components_text(path.bytes());
}

auto components(const RelativePath& path) noexcept -> Result<std::vector<std::string>> {
    return components_text(path.bytes());
}

auto stem(std::string_view component) noexcept -> Result<std::string> {
    std::size_t stem_length = 0;
    bc_file_system_error c_error;
    if (bc_file_system_path_stem(bytes_of(component), component.size(), &stem_length, &c_error) != 0) {
        return error_from(c_error, Operation::path_stem, component);
    }
    return std::string(component.substr(0, stem_length));
}

auto extension(std::string_view component) noexcept -> Result<std::optional<std::string>> {
    std::size_t offset = 0;
    std::size_t extension_length = 0;
    std::int32_t present = 0;
    bc_file_system_error c_error;
    if (bc_file_system_path_extension(bytes_of(component), component.size(), &offset, &extension_length, &present,
                                      &c_error) != 0) {
        return error_from(c_error, Operation::path_extension, component);
    }
    if (present == 0) {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{std::string(component.substr(offset, extension_length))};
}

auto relate(const AbsolutePath& path, const AbsolutePath& other) noexcept -> Result<PathRelation> {
    return relate(path.bytes(), other.bytes());
}

auto relate(const RelativePath& path, const RelativePath& other) noexcept -> Result<PathRelation> {
    return relate(path.bytes(), other.bytes());
}

auto relate(std::string_view path, std::string_view other) noexcept -> Result<PathRelation> {
    std::int32_t relation = 0;
    bc_file_system_error c_error;
    if (bc_file_system_path_relate(bytes_of(path), path.size(), bytes_of(other), other.size(), &relation, &c_error) !=
        0) {
        return error_from(c_error, Operation::path_relate, path);
    }
    const std::optional<PathRelation> answer = path_relation_from(relation);
    if (!answer.has_value()) {
        return answer_outside_enum(Operation::path_relate, path);
    }
    return *answer;
}

auto lexically_normalize(std::string_view text) noexcept -> Result<std::string> {
    return answered_text(
        Operation::path_lexically_normalize, text,
        [&](std::uint8_t* buffer, std::size_t capacity, std::size_t* length, bc_file_system_error* c_error) {
            return bc_file_system_path_lexically_normalize(bytes_of(text), text.size(), buffer, capacity, length,
                                                           c_error);
        });
}

auto resolve_against(const AbsolutePath& base, std::string_view relative) noexcept -> Result<AbsolutePath> {
    const std::string_view base_bytes = base.bytes();
    const Result<std::string> resolved = answered_text(
        Operation::path_resolve_against, relative,
        [&](std::uint8_t* buffer, std::size_t capacity, std::size_t* length, bc_file_system_error* c_error) {
            return bc_file_system_path_resolve_against(bytes_of(base_bytes), base_bytes.size(), bytes_of(relative),
                                                       relative.size(), buffer, capacity, length, c_error);
        });
    if (!resolved.has_value()) {
        return resolved.error();
    }
    return AbsolutePath::from(*resolved);
}

auto library_name(std::string_view component) noexcept -> Result<std::optional<LibraryName>> {
    std::int32_t kind = 0;
    std::size_t stem_offset = 0;
    std::size_t stem_length = 0;
    bc_file_system_error c_error;
    if (bc_file_system_path_library_name(bytes_of(component), component.size(), &kind, &stem_offset, &stem_length,
                                         &c_error) != 0) {
        return error_from(c_error, Operation::path_library_name, component);
    }
    const std::optional<LibraryKind> library_kind = library_kind_from(kind);
    if (!library_kind.has_value()) {
        return answer_outside_enum(Operation::path_library_name, component);
    }
    if (*library_kind == LibraryKind::none) {
        return std::optional<LibraryName>{};
    }
    return std::optional<LibraryName>{
        LibraryName{.kind = *library_kind, .stem = std::string(component.substr(stem_offset, stem_length))}};
}

} // namespace bc::file_system
