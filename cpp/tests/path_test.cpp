#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/error.hpp>
#include <bc_file_system/location.hpp>
#include <bc_file_system/path.hpp>
#include <bc_file_system/result.hpp>

// The path module's C++ binding suite: every free function of <bc_file_system/path.hpp> reaches C once and
// hands its answer back as a value type, a string or an enum; a refusal arrives as an Error with the operation
// the core named and the refused bytes as its path; the two enums map totally; and the edge table the C-only
// test (core/tests/path_test.c) holds is run again here, through the face. The rules themselves are proven in
// C; what these cases hold is that the face carries each answer across unchanged.

namespace bc::file_system {

// gtest prints an enum class as raw bytes; the face's names read better in a failure.
inline auto PrintTo(ErrorKind kind, std::ostream* out) -> void {
    *out << name_of(kind);
}
inline auto PrintTo(Operation operation, std::ostream* out) -> void {
    *out << name_of(operation);
}
inline auto PrintTo(PathRelation relation, std::ostream* out) -> void {
    *out << name_of(relation);
}
inline auto PrintTo(LibraryKind kind, std::ostream* out) -> void {
    *out << name_of(kind);
}

} // namespace bc::file_system

namespace {

using bc::file_system::AbsolutePath;
using bc::file_system::Error;
using bc::file_system::ErrorKind;
using bc::file_system::LibraryKind;
using bc::file_system::LibraryName;
using bc::file_system::Operation;
using bc::file_system::PathRelation;
using bc::file_system::RelativePath;
using bc::file_system::Result;

auto absolute(std::string_view bytes) -> AbsolutePath {
    Result<AbsolutePath> path = AbsolutePath::from(bytes);
    EXPECT_TRUE(path.has_value()) << bytes;
    return std::move(path).value();
}

auto relative(std::string_view bytes) -> RelativePath {
    Result<RelativePath> path = RelativePath::from(bytes);
    EXPECT_TRUE(path.has_value()) << bytes;
    return std::move(path).value();
}

// The bytes an optional path holds, or none: an absent answer compares as empty text rather than being
// dereferenced.
auto bytes_or_empty(const std::optional<AbsolutePath>& path) -> std::string_view {
    if (path.has_value()) {
        return path->bytes();
    }
    return {};
}

auto bytes_or_empty(const std::optional<RelativePath>& path) -> std::string_view {
    if (path.has_value()) {
        return path->bytes();
    }
    return {};
}

// What every refusal of the module looks like through the face.
auto expect_refusal(const Error& error, Operation operation, std::string_view refused_bytes) -> void {
    EXPECT_EQ(error.kind, ErrorKind::invalid_argument);
    EXPECT_EQ(error.operation, operation);
    EXPECT_FALSE(error.step.has_value());
    EXPECT_EQ(error.system_code, 0);
    EXPECT_EQ(error.path, refused_bytes);
    EXPECT_FALSE(error.name_truncated);
}

// -------------------------------------------------------------------------------------------------------------
// Each function reaches C and hands its answer back
// -------------------------------------------------------------------------------------------------------------

TEST(PathBindingTest, JoinOfAnAbsoluteBaseIsAbsolute) {
    const Result<AbsolutePath> joined = bc::file_system::join(absolute("/a"), relative("b/c"));
    const Result<AbsolutePath> under_root = bc::file_system::join(absolute("/"), relative("a"));

    ASSERT_TRUE(joined.has_value());
    EXPECT_EQ(joined->bytes(), "/a/b/c");
    ASSERT_TRUE(under_root.has_value());
    EXPECT_EQ(under_root->bytes(), "/a");
}

TEST(PathBindingTest, JoinOfARelativeBaseIsRelative) {
    const Result<RelativePath> joined = bc::file_system::join(relative("a"), relative("b"));

    ASSERT_TRUE(joined.has_value());
    EXPECT_EQ(joined->bytes(), "a/b");
}

TEST(PathBindingTest, ParentIsAPrefixOrAbsent) {
    const Result<std::optional<AbsolutePath>> of_file = bc::file_system::parent(absolute("/a/b"));
    const Result<std::optional<AbsolutePath>> of_top = bc::file_system::parent(absolute("/a"));
    const Result<std::optional<AbsolutePath>> of_root = bc::file_system::parent(absolute("/"));
    const Result<std::optional<RelativePath>> of_nested = bc::file_system::parent(relative("a/b/c"));
    const Result<std::optional<RelativePath>> of_one = bc::file_system::parent(relative("a"));

    ASSERT_TRUE(of_file.has_value());
    EXPECT_EQ(bytes_or_empty(*of_file), "/a");
    ASSERT_TRUE(of_top.has_value());
    EXPECT_EQ(bytes_or_empty(*of_top), "/");
    ASSERT_TRUE(of_root.has_value());
    EXPECT_FALSE(of_root->has_value());
    ASSERT_TRUE(of_nested.has_value());
    EXPECT_EQ(bytes_or_empty(*of_nested), "a/b");
    ASSERT_TRUE(of_one.has_value());
    EXPECT_FALSE(of_one->has_value());
}

TEST(PathBindingTest, LastComponentOfAPathAndTheRootsRefusal) {
    const Result<std::string> of_absolute = bc::file_system::last_component(absolute("/a/b.txt"));
    const Result<std::string> of_relative = bc::file_system::last_component(relative("a"));
    const Result<std::string> of_root = bc::file_system::last_component(absolute("/"));

    ASSERT_TRUE(of_absolute.has_value());
    EXPECT_EQ(*of_absolute, "b.txt");
    ASSERT_TRUE(of_relative.has_value());
    EXPECT_EQ(*of_relative, "a");
    ASSERT_FALSE(of_root.has_value());
    expect_refusal(of_root.error(), Operation::path_last_component, "/");
}

TEST(PathBindingTest, ComponentsInOrderAndNoneForTheRoot) {
    const Result<std::vector<std::string>> of_absolute = bc::file_system::components(absolute("/a/../bb"));
    const Result<std::vector<std::string>> of_relative = bc::file_system::components(relative("x/y"));
    const Result<std::vector<std::string>> of_root = bc::file_system::components(absolute("/"));

    ASSERT_TRUE(of_absolute.has_value());
    EXPECT_EQ(*of_absolute, (std::vector<std::string>{"a", "..", "bb"}));
    ASSERT_TRUE(of_relative.has_value());
    EXPECT_EQ(*of_relative, (std::vector<std::string>{"x", "y"}));
    ASSERT_TRUE(of_root.has_value());
    EXPECT_TRUE(of_root->empty());
}

TEST(PathBindingTest, StemAndExtensionOfOneComponent) {
    const Result<std::string> stem = bc::file_system::stem("archive.tar.gz");
    const Result<std::optional<std::string>> extension = bc::file_system::extension("archive.tar.gz");
    const Result<std::optional<std::string>> none = bc::file_system::extension(".bashrc");

    ASSERT_TRUE(stem.has_value());
    EXPECT_EQ(*stem, "archive.tar");
    ASSERT_TRUE(extension.has_value());
    EXPECT_EQ(*extension, std::optional<std::string>{"gz"});
    ASSERT_TRUE(none.has_value());
    EXPECT_FALSE(none->has_value());
}

TEST(PathBindingTest, RelateThroughEachOverload) {
    const Result<PathRelation> typed_absolute = bc::file_system::relate(absolute("/a/b"), absolute("/a"));
    const Result<PathRelation> typed_relative = bc::file_system::relate(relative("a"), relative("a/b"));
    const Result<PathRelation> loose = bc::file_system::relate("a/../b", "b");
    const Result<PathRelation> mixed = bc::file_system::relate("/a", "a");

    ASSERT_TRUE(typed_absolute.has_value());
    EXPECT_EQ(*typed_absolute, PathRelation::inside);
    ASSERT_TRUE(typed_relative.has_value());
    EXPECT_EQ(*typed_relative, PathRelation::outside);
    ASSERT_TRUE(loose.has_value());
    EXPECT_EQ(*loose, PathRelation::same);
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(*mixed, PathRelation::undecidable);
}

TEST(PathBindingTest, LexicallyNormalizeAnswersTheWholeText) {
    const Result<std::string> normalized = bc::file_system::lexically_normalize("/a/./b//../c/");

    ASSERT_TRUE(normalized.has_value());
    EXPECT_EQ(*normalized, "/a/c");
}

TEST(PathBindingTest, ResolveAgainstAnswersAnAbsolutePath) {
    const Result<AbsolutePath> resolved = bc::file_system::resolve_against(absolute("/home/me"), "docs/../notes.txt");
    const Result<AbsolutePath> clamped = bc::file_system::resolve_against(absolute("/a"), "../../b");

    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->bytes(), "/home/me/notes.txt");
    ASSERT_TRUE(clamped.has_value());
    EXPECT_EQ(clamped->bytes(), "/b");
}

TEST(PathBindingTest, LibraryNameIsAKindAndAStemOrEmpty) {
    const Result<std::optional<LibraryName>> shared = bc::file_system::library_name("libz.so.1");
    const Result<std::optional<LibraryName>> plain = bc::file_system::library_name("notes.txt");

    ASSERT_TRUE(shared.has_value());
    EXPECT_EQ(*shared, (std::optional<LibraryName>{LibraryName{.kind = LibraryKind::shared_library, .stem = "z"}}));
    ASSERT_TRUE(plain.has_value());
    EXPECT_FALSE(plain->has_value());
}

// -------------------------------------------------------------------------------------------------------------
// The error mapping
// -------------------------------------------------------------------------------------------------------------

TEST(PathErrorTest, ARefusalNamesTheOperationAndTheRefusedBytes) {
    const Result<std::string> stem = bc::file_system::stem("a/b");
    const Result<std::optional<std::string>> extension = bc::file_system::extension("");
    const Result<PathRelation> relation = bc::file_system::relate("", "a");
    const Result<std::string> normalized = bc::file_system::lexically_normalize("");
    const Result<AbsolutePath> resolved = bc::file_system::resolve_against(absolute("/a"), "/b");
    const Result<std::optional<LibraryName>> library = bc::file_system::library_name("a/b");

    ASSERT_FALSE(stem.has_value());
    expect_refusal(stem.error(), Operation::path_stem, "a/b");
    ASSERT_FALSE(extension.has_value());
    expect_refusal(extension.error(), Operation::path_extension, "");
    ASSERT_FALSE(relation.has_value());
    expect_refusal(relation.error(), Operation::path_relate, "");
    ASSERT_FALSE(normalized.has_value());
    expect_refusal(normalized.error(), Operation::path_lexically_normalize, "");
    ASSERT_FALSE(resolved.has_value());
    expect_refusal(resolved.error(), Operation::path_resolve_against, "/b");
    ASSERT_FALSE(library.has_value());
    expect_refusal(library.error(), Operation::path_library_name, "a/b");
}

TEST(PathErrorTest, ARefusalsMessageReadsLikeEveryOtherErrors) {
    const Result<std::string> stem = bc::file_system::stem("a/b");

    ASSERT_FALSE(stem.has_value());
    EXPECT_EQ(stem.error().message(), "path_stem: a/b: invalid_argument");
}

TEST(PathErrorTest, ANulByteInsideLooseTextIsRefused) {
    std::string text = "a";
    text.push_back('\0');
    text += "b";

    const Result<std::string> normalized = bc::file_system::lexically_normalize(text);

    ASSERT_FALSE(normalized.has_value());
    EXPECT_EQ(normalized.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(normalized.error().path, text);
}

TEST(PathErrorTest, EveryPathRelationHasItsNameAndItsCValue) {
    const std::vector<std::string_view> names = {"inside", "same", "outside", "undecidable"};

    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto value = static_cast<std::int32_t>(index + 1);
        const std::optional<PathRelation> relation = bc::file_system::path_relation_from(value);
        if (!relation.has_value()) {
            ADD_FAILURE() << "no PathRelation for " << value;
            continue;
        }
        EXPECT_EQ(static_cast<std::int32_t>(*relation), value);
        EXPECT_EQ(name_of(*relation), names[index]);
    }
    EXPECT_FALSE(bc::file_system::path_relation_from(0).has_value());
    EXPECT_FALSE(bc::file_system::path_relation_from(5).has_value());
}

TEST(PathErrorTest, EveryLibraryKindHasItsNameAndItsCValue) {
    const std::vector<std::string_view> names = {"none", "shared_library", "static_library", "framework"};

    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto value = static_cast<std::int32_t>(index + 1);
        const std::optional<LibraryKind> kind = bc::file_system::library_kind_from(value);
        if (!kind.has_value()) {
            ADD_FAILURE() << "no LibraryKind for " << value;
            continue;
        }
        EXPECT_EQ(static_cast<std::int32_t>(*kind), value);
        EXPECT_EQ(name_of(*kind), names[index]);
    }
    EXPECT_FALSE(bc::file_system::library_kind_from(0).has_value());
    EXPECT_FALSE(bc::file_system::library_kind_from(5).has_value());
}

// -------------------------------------------------------------------------------------------------------------
// The edge table, through the face
// -------------------------------------------------------------------------------------------------------------

struct NormalizeCase {
    std::string_view text;
    std::string_view expected;
};

TEST(PathEdgeTest, LexicallyNormalizeTable) {
    const std::vector<NormalizeCase> cases = {
        {.text = "a//b", .expected = "a/b"},
        {.text = "a/./b", .expected = "a/b"},
        {.text = "a/../b", .expected = "b"},
        {.text = "../a", .expected = "../a"},
        {.text = "a/../../b", .expected = "../b"},
        {.text = "/..", .expected = "/"},
        {.text = "//", .expected = "/"},
        {.text = "/a/", .expected = "/a"},
        {.text = ".", .expected = "."},
        {.text = "a/..", .expected = "."},
        {.text = "./", .expected = "."},
        {.text = "/a/b/../c", .expected = "/a/c"},
        {.text = "a/b/../../..", .expected = ".."},
        {.text = "./a/./", .expected = "a"},
        {.text = "/./a", .expected = "/a"},
        {.text = "/", .expected = "/"},
        {.text = "a", .expected = "a"},
        {.text = "/a/../../b", .expected = "/b"},
        {.text = "..", .expected = ".."},
        {.text = "a/b/c/../..", .expected = "a"},
    };
    for (const NormalizeCase& item : cases) {
        const Result<std::string> normalized = bc::file_system::lexically_normalize(item.text);
        ASSERT_TRUE(normalized.has_value()) << item.text;
        EXPECT_EQ(*normalized, item.expected) << item.text;
        const Result<std::string> again = bc::file_system::lexically_normalize(*normalized);
        ASSERT_TRUE(again.has_value()) << item.text;
        EXPECT_EQ(*again, *normalized) << "idempotent: " << item.text;
    }
}

struct RelateCase {
    std::string_view path;
    std::string_view other;
    PathRelation relation;
};

TEST(PathEdgeTest, RelateTable) {
    const std::vector<RelateCase> cases = {
        {.path = "/a/b", .other = "/a", .relation = PathRelation::inside},
        {.path = "/a", .other = "/a/b", .relation = PathRelation::outside},
        {.path = "/a", .other = "/a", .relation = PathRelation::same},
        {.path = "/a/./b", .other = "/a/b", .relation = PathRelation::same},
        {.path = "/a/b/..", .other = "/a", .relation = PathRelation::same},
        {.path = "/a", .other = "/b", .relation = PathRelation::outside},
        {.path = "/ab", .other = "/a", .relation = PathRelation::outside},
        {.path = "/", .other = "/", .relation = PathRelation::same},
        {.path = "/a", .other = "/", .relation = PathRelation::inside},
        {.path = "/..", .other = "/", .relation = PathRelation::same},
        {.path = "a/b", .other = "a", .relation = PathRelation::inside},
        {.path = "a/..", .other = ".", .relation = PathRelation::same},
        {.path = "a", .other = ".", .relation = PathRelation::inside},
        {.path = "a", .other = "/a", .relation = PathRelation::undecidable},
        {.path = "../a", .other = "a", .relation = PathRelation::undecidable},
        {.path = "a/../../b", .other = "b", .relation = PathRelation::undecidable},
    };
    for (const RelateCase& item : cases) {
        const Result<PathRelation> relation = bc::file_system::relate(item.path, item.other);
        ASSERT_TRUE(relation.has_value()) << item.path;
        EXPECT_EQ(*relation, item.relation) << item.path << " vs " << item.other;
    }
}

struct JoinCase {
    std::string_view base;
    std::string_view relative;
    std::string_view expected;
};

TEST(PathEdgeTest, JoinOfValidatedPartsValidates) {
    const std::vector<JoinCase> cases = {
        {.base = "/", .relative = "a", .expected = "/a"},
        {.base = "/", .relative = "a/b", .expected = "/a/b"},
        {.base = "/a", .relative = "b", .expected = "/a/b"},
        {.base = "/a", .relative = "b/c", .expected = "/a/b/c"},
        {.base = "/a/..", .relative = "b", .expected = "/a/../b"},
    };
    for (const JoinCase& item : cases) {
        const Result<AbsolutePath> joined = bc::file_system::join(absolute(item.base), relative(item.relative));
        ASSERT_TRUE(joined.has_value()) << item.base;
        EXPECT_EQ(joined->bytes(), item.expected);
        // The parent and last component of the answer are the parts that made it, when the relative part
        // was one component.
        if (item.relative.find('/') == std::string_view::npos) {
            const Result<std::optional<AbsolutePath>> back = bc::file_system::parent(*joined);
            ASSERT_TRUE(back.has_value()) << item.expected;
            EXPECT_EQ(bytes_or_empty(*back), item.base);
            const Result<std::string> last = bc::file_system::last_component(*joined);
            ASSERT_TRUE(last.has_value());
            EXPECT_EQ(*last, item.relative);
        }
    }
}

struct ComponentPartsCase {
    std::string_view component;
    std::string_view stem;
    std::optional<std::string_view> extension;
};

TEST(PathEdgeTest, StemAndExtensionTable) {
    const std::vector<ComponentPartsCase> cases = {
        {.component = "a.txt", .stem = "a", .extension = "txt"},
        {.component = "archive.tar.gz", .stem = "archive.tar", .extension = "gz"},
        {.component = ".bashrc", .stem = ".bashrc", .extension = std::nullopt},
        {.component = "archive.", .stem = "archive.", .extension = std::nullopt},
        {.component = "a", .stem = "a", .extension = std::nullopt},
        {.component = ".a.b", .stem = ".a", .extension = "b"},
        {.component = ".", .stem = ".", .extension = std::nullopt},
        {.component = "..", .stem = "..", .extension = std::nullopt},
    };
    for (const ComponentPartsCase& item : cases) {
        const Result<std::string> stem = bc::file_system::stem(item.component);
        const Result<std::optional<std::string>> extension = bc::file_system::extension(item.component);
        ASSERT_TRUE(stem.has_value()) << item.component;
        EXPECT_EQ(*stem, item.stem) << item.component;
        ASSERT_TRUE(extension.has_value()) << item.component;
        EXPECT_EQ(*extension, item.extension) << item.component;
    }
}

struct LibraryCase {
    std::string_view component;
    std::optional<LibraryName> expected;
};

TEST(PathEdgeTest, LibraryNameTable) {
    const std::vector<LibraryCase> cases = {
        {.component = "libz.so", .expected = LibraryName{.kind = LibraryKind::shared_library, .stem = "z"}},
        {.component = "libfoo.so.1.2.3", .expected = LibraryName{.kind = LibraryKind::shared_library, .stem = "foo"}},
        {.component = "libc++.dylib", .expected = LibraryName{.kind = LibraryKind::shared_library, .stem = "c++"}},
        {.component = "libz.1.dylib", .expected = LibraryName{.kind = LibraryKind::shared_library, .stem = "z.1"}},
        {.component = "libm.a", .expected = LibraryName{.kind = LibraryKind::static_library, .stem = "m"}},
        {.component = "Foundation.framework",
         .expected = LibraryName{.kind = LibraryKind::framework, .stem = "Foundation"}},
        {.component = "kernel32.dll", .expected = LibraryName{.kind = LibraryKind::shared_library, .stem = "kernel32"}},
        {.component = "libfoo.so.1.x", .expected = std::nullopt},
        {.component = "lib.so", .expected = std::nullopt},
        {.component = "libz.SO", .expected = std::nullopt},
        {.component = "foo.so", .expected = std::nullopt},
        {.component = "a", .expected = std::nullopt},
    };
    for (const LibraryCase& item : cases) {
        const Result<std::optional<LibraryName>> found = bc::file_system::library_name(item.component);
        ASSERT_TRUE(found.has_value()) << item.component;
        EXPECT_EQ(*found, item.expected) << item.component;
    }
}

TEST(PathEdgeTest, AComponentOf255BytesIsAcceptedAndOneOf256Refused) {
    const std::string longest(255, 'x');
    const std::string too_long(256, 'x');

    const Result<RelativePath> accepted = RelativePath::from("a/" + longest);
    const Result<RelativePath> refused = RelativePath::from("a/" + too_long);
    const Result<std::string> stem = bc::file_system::stem(too_long);

    EXPECT_TRUE(accepted.has_value());
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(stem.has_value());
    EXPECT_EQ(stem.error().kind, ErrorKind::invalid_argument);
}

} // namespace
