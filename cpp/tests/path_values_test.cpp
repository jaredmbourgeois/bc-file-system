#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include <bc_file_system/bc_file_system.h>
#include <bc_file_system/engine.hpp>
#include <bc_file_system/error.hpp>
#include <bc_file_system/file_system.hpp>
#include <bc_file_system/location.hpp>
#include <bc_file_system/options.hpp>
#include <bc_file_system/result.hpp>

#include "descriptor_balance.hpp"

// The path values of the C++ face: what RelativePath and AbsolutePath accept and refuse, how a Location holds
// its bytes and crosses to C, and how the core refuses an `at` location whose bytes are not an absolute path.
// The lexical rules live in the face until stage `path-module` moves them into C; these cases hold the rules,
// not their home.

namespace bc::file_system {

// gtest prints an enum class as raw bytes; the face's names read better in a failure.
inline auto PrintTo(ErrorKind kind, std::ostream* out) -> void {
    *out << name_of(kind);
}
inline auto PrintTo(Operation operation, std::ostream* out) -> void {
    *out << name_of(operation);
}

} // namespace bc::file_system

namespace {

using bc::file_system::AbsolutePath;
using bc::file_system::Engine;
using bc::file_system::Error;
using bc::file_system::ErrorKind;
using bc::file_system::FileSystem;
using bc::file_system::FinalSymlink;
using bc::file_system::Location;
using bc::file_system::LocationKind;
using bc::file_system::Operation;
using bc::file_system::ReadLimit;
using bc::file_system::ReadOptions;
using bc::file_system::RelativePath;
using bc::file_system::Result;

// The one read option set this file states, built once here.
constexpr ReadOptions kWholeFileRefusingSymlink{.limit = ReadLimit::whole_file(),
                                                .final_symlink = FinalSymlink::refuse};

// A string_view over text with a NUL byte inside it, which a literal alone would cut short.
auto with_a_nul_inside(std::string_view before, std::string_view after) -> std::string {
    std::string text(before);
    text.push_back('\0');
    text += after;
    return text;
}

auto bytes_of_c_location(const bc_file_system_location& c_location) -> std::string_view {
    return std::string_view{reinterpret_cast<const char*>(c_location.path), c_location.length};
}

// -------------------------------------------------------------------------------------------------------------
// RelativePath
// -------------------------------------------------------------------------------------------------------------

TEST(RelativePathTest, AcceptsOneOrMoreComponents) {
    const Result<RelativePath> one = RelativePath::from("notes.txt");
    const Result<RelativePath> several = RelativePath::from("notes/2026/april.md");

    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->bytes(), "notes.txt");
    ASSERT_TRUE(several.has_value());
    EXPECT_EQ(several->bytes(), "notes/2026/april.md");
}

TEST(RelativePathTest, EqualityIsAByteCompare) {
    const Result<RelativePath> first = RelativePath::from("a/b");
    const Result<RelativePath> same = RelativePath::from("a/b");
    const Result<RelativePath> other = RelativePath::from("a/c");

    ASSERT_TRUE(first.has_value() && same.has_value() && other.has_value());
    EXPECT_EQ(*first, *same);
    EXPECT_NE(*first, *other);
}

TEST(RelativePathTest, RefusesAnEmptyPath) {
    const Result<RelativePath> empty = RelativePath::from("");

    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind, ErrorKind::invalid_argument);
}

TEST(RelativePathTest, RefusesALeadingSlash) {
    const Result<RelativePath> absolute = RelativePath::from("/etc/passwd");
    const Result<RelativePath> root = RelativePath::from("/");

    ASSERT_FALSE(absolute.has_value());
    EXPECT_EQ(absolute.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(root.has_value());
    EXPECT_EQ(root.error().kind, ErrorKind::invalid_argument);
}

TEST(RelativePathTest, RefusesAnEmptyComponent) {
    const Result<RelativePath> doubled = RelativePath::from("a//b");
    const Result<RelativePath> trailing = RelativePath::from("a/b/");

    ASSERT_FALSE(doubled.has_value());
    EXPECT_EQ(doubled.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(trailing.has_value());
    EXPECT_EQ(trailing.error().kind, ErrorKind::invalid_argument);
}

TEST(RelativePathTest, RefusesADotDotComponent) {
    const Result<RelativePath> alone = RelativePath::from("..");
    const Result<RelativePath> inside = RelativePath::from("a/../b");
    const Result<RelativePath> last = RelativePath::from("a/..");

    ASSERT_FALSE(alone.has_value());
    EXPECT_EQ(alone.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(inside.has_value());
    EXPECT_EQ(inside.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(last.has_value());
    EXPECT_EQ(last.error().kind, ErrorKind::invalid_argument);
}

TEST(RelativePathTest, RefusesANulByte) {
    const std::string bytes = with_a_nul_inside("a", "b");

    const Result<RelativePath> refused = RelativePath::from(bytes);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::invalid_argument);
}

// A refusal is an Error like any other: it names the operation that refused, the bytes it refused, no step
// and no system code, and its message reads like every other error's.
TEST(RelativePathTest, ARefusalNamesTheOperationAndTheBytes) {
    const Result<RelativePath> refused = RelativePath::from("a/../b");

    ASSERT_FALSE(refused.has_value());
    const Error& error = refused.error();
    EXPECT_EQ(error.kind, ErrorKind::invalid_argument);
    EXPECT_EQ(error.operation, Operation::relative_path_from);
    EXPECT_FALSE(error.step.has_value());
    EXPECT_EQ(error.system_code, 0);
    EXPECT_EQ(error.path, "a/../b");
    EXPECT_FALSE(error.name_truncated);
    EXPECT_EQ(error.message(), "relative_path_from: a/../b: invalid_argument");
}

// -------------------------------------------------------------------------------------------------------------
// AbsolutePath
// -------------------------------------------------------------------------------------------------------------

TEST(AbsolutePathTest, AcceptsAPathStartingAtTheRoot) {
    const Result<AbsolutePath> root = AbsolutePath::from("/");
    const Result<AbsolutePath> file = AbsolutePath::from("/etc/config.toml");

    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(root->bytes(), "/");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->bytes(), "/etc/config.toml");
}

// `..` is allowed in an absolute path and reported by the path module; it is never resolved here.
TEST(AbsolutePathTest, KeepsADotDotComponentAsItWasSpelled) {
    const Result<AbsolutePath> climbing = AbsolutePath::from("/a/../b");

    ASSERT_TRUE(climbing.has_value());
    EXPECT_EQ(climbing->bytes(), "/a/../b");
}

TEST(AbsolutePathTest, RefusesRelativeText) {
    const Result<AbsolutePath> empty = AbsolutePath::from("");
    const Result<AbsolutePath> bare = AbsolutePath::from("etc");
    const Result<AbsolutePath> dotted = AbsolutePath::from("./etc");
    const Result<AbsolutePath> climbing = AbsolutePath::from("..");

    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(bare.has_value());
    EXPECT_EQ(bare.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(dotted.has_value());
    EXPECT_EQ(dotted.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(climbing.has_value());
    EXPECT_EQ(climbing.error().kind, ErrorKind::invalid_argument);
}

TEST(AbsolutePathTest, RefusesANulByte) {
    const std::string bytes = with_a_nul_inside("/a", "b");

    const Result<AbsolutePath> refused = AbsolutePath::from(bytes);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::invalid_argument);
}

TEST(AbsolutePathTest, ARefusalNamesTheOperationAndTheBytes) {
    const Result<AbsolutePath> refused = AbsolutePath::from("etc");

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(refused.error().operation, Operation::absolute_path_from);
    EXPECT_FALSE(refused.error().step.has_value());
    EXPECT_EQ(refused.error().path, "etc");
}

TEST(AbsolutePathTest, EqualityIsAByteCompare) {
    const Result<AbsolutePath> first = AbsolutePath::from("/a/b");
    const Result<AbsolutePath> same = AbsolutePath::from("/a/b");
    const Result<AbsolutePath> other = AbsolutePath::from("/a/b/");

    ASSERT_TRUE(first.has_value() && same.has_value() && other.has_value());
    EXPECT_EQ(*first, *same);
    EXPECT_NE(*first, *other);
}

// -------------------------------------------------------------------------------------------------------------
// Location
// -------------------------------------------------------------------------------------------------------------

TEST(LocationTest, AtHoldsTheAbsoluteBytesAndCrossesToCAsAt) {
    const Location location = Location::at(std::filesystem::path{"/etc/config.toml"});

    const bc_file_system_location c_location = location.to_c();

    EXPECT_EQ(location.kind(), LocationKind::at);
    EXPECT_EQ(location.text(), "/etc/config.toml");
    EXPECT_EQ(c_location.kind, bc_file_system_location_kind_at);
    EXPECT_EQ(c_location.directory, nullptr);
    EXPECT_EQ(bytes_of_c_location(c_location), "/etc/config.toml");
}

TEST(LocationTest, AtFromATypedAbsolutePathHoldsTheSameBytes) {
    const Result<AbsolutePath> path = AbsolutePath::from("/var/log/app.log");
    ASSERT_TRUE(path.has_value());

    const Location location = Location::at(*path);

    EXPECT_EQ(location.kind(), LocationKind::at);
    EXPECT_EQ(location.text(), "/var/log/app.log");
    EXPECT_EQ(bytes_of_c_location(location.to_c()), "/var/log/app.log");
}

// `at` from a std::filesystem::path validates nothing: the bytes go to the core, and the operation refuses
// them there (the cases below).
TEST(LocationTest, AtFromAFileSystemPathKeepsRelativeTextForTheOperationToRefuse) {
    const Location location = Location::at(std::filesystem::path{"relative.txt"});

    EXPECT_EQ(location.kind(), LocationKind::at);
    EXPECT_EQ(location.text(), "relative.txt");
    EXPECT_EQ(bytes_of_c_location(location.to_c()), "relative.txt");
}

// -------------------------------------------------------------------------------------------------------------
// The core's refusal of an `at` location, through the face
// -------------------------------------------------------------------------------------------------------------

// Each refusal below happens in the core's argument check, before any system call, so it is green against the
// stub: the engine answers `invalid_argument` with the operation named and no step.
class LocationRefusalTest : public bc::file_system_test::DescriptorBalanced {
protected:
    auto SetUp() -> void override {
        ASSERT_TRUE(engine_.has_value()) << engine_.error().message();
        files_.emplace(engine_.value());
    }

    [[nodiscard]] auto files() -> FileSystem& { return *files_; }

private:
    Result<Engine> engine_ = Engine::observed_for_tests();
    std::optional<FileSystem> files_;
};

TEST_F(LocationRefusalTest, RelativeTextUnderAtIsInvalidArgument) {
    const Location relative = Location::at(std::filesystem::path{"relative.txt"});

    const auto status = files().status(relative, FinalSymlink::refuse);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(status.error().operation, Operation::status);
    EXPECT_FALSE(status.error().step.has_value());
    EXPECT_EQ(status.error().system_code, 0);
    EXPECT_EQ(status.error().path, "relative.txt");
}

TEST_F(LocationRefusalTest, AnEmptyPathUnderAtIsInvalidArgument) {
    const Location empty = Location::at(std::filesystem::path{});

    const auto status = files().status(empty, FinalSymlink::refuse);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(status.error().operation, Operation::status);
}

TEST_F(LocationRefusalTest, ANulByteUnderAtIsInvalidArgument) {
    const Location with_nul = Location::at(std::filesystem::path{with_a_nul_inside("/etc", "hosts")});

    const auto status = files().status(with_nul, FinalSymlink::refuse);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(status.error().operation, Operation::status);
}

// The refusal is the operation's, so every operation refuses the same text the same way.
TEST_F(LocationRefusalTest, EveryOperationRefusesRelativeTextUnderAt) {
    const Location relative = Location::at(std::filesystem::path{"relative.txt"});

    const auto read = files().read_file(relative, kWholeFileRefusingSymlink);
    const auto removed = files().remove_file(relative);
    const auto opened = files().open_directory(relative, FinalSymlink::refuse);

    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(read.error().operation, Operation::read_file);
    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(removed.error().operation, Operation::remove_file);
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(opened.error().operation, Operation::open_directory);
}

} // namespace
