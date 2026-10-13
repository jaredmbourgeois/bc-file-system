#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
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

#include "descriptor_balance.hpp"
#include "temp_directory.hpp"
#include "test_paths.hpp"

// The C++ binding suite: the face's contract with the C header, apart from what the operations do to the file
// system (the contract suite's job). Each operation reaches the core once and answers from the file system,
// the mapping of every enum is total in both directions, each value type makes one
// round trip across the ABI, the error struct is converted and rendered, and the face's own compositions
// (`read_file_if_present`, the grow loop, `lines_of`) do what the design doc says. The C-level cases prove
// the header's own promises: zero is never a valid enum, NULL where forbidden is refused, a success zeroes the
// error struct.

namespace bc::file_system {

// gtest prints an enum class as raw bytes; the face's names read better in a failure.
inline auto PrintTo(ErrorKind kind, std::ostream* out) -> void {
    *out << name_of(kind);
}
inline auto PrintTo(Operation operation, std::ostream* out) -> void {
    *out << name_of(operation);
}
inline auto PrintTo(Step step, std::ostream* out) -> void {
    *out << name_of(step);
}

} // namespace bc::file_system

namespace {

using bc::file_system::AbsolutePath;
using bc::file_system::Directory;
using bc::file_system::Durability;
using bc::file_system::Engine;
using bc::file_system::Entry;
using bc::file_system::EntryType;
using bc::file_system::Error;
using bc::file_system::ErrorKind;
using bc::file_system::ExistingDestination;
using bc::file_system::FailedLocation;
using bc::file_system::Fault;
using bc::file_system::FaultAction;
using bc::file_system::FileSystem;
using bc::file_system::FinalSymlink;
using bc::file_system::lines_of;
using bc::file_system::Location;
using bc::file_system::LocationKind;
using bc::file_system::LockOutcome;
using bc::file_system::name_of;
using bc::file_system::Operation;
using bc::file_system::Permissions;
using bc::file_system::PermissionsPolicy;
using bc::file_system::ReadLimit;
using bc::file_system::ReadLimitKind;
using bc::file_system::ReadOptions;
using bc::file_system::Result;
using bc::file_system::Status;
using bc::file_system::Step;
using bc::file_system::WriteOptions;
using bc::file_system_test::bytes_of;
using bc::file_system_test::temporary_root;
using bc::file_system_test::TemporaryDirectory;
using bc::file_system_test::text_of;
using testing::ElementsAre;
using testing::IsEmpty;

// The options every case states, built once here so a field added later fails in one place.
constexpr ReadOptions kWholeFileRefusingSymlink{.limit = ReadLimit::whole_file(),
                                                .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenExactly0644{.durability = Durability::written,
                                           .permissions = Permissions::exactly(0644),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenExactly0600{.durability = Durability::written,
                                           .permissions = Permissions::exactly(0600),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kDurableExactly0600{.durability = Durability::durable,
                                           .permissions = Permissions::exactly(0600),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenPreserveOr0600{.durability = Durability::written,
                                              .permissions = Permissions::preserve_or(0600),
                                              .existing_destination = ExistingDestination::replace,
                                              .final_symlink = FinalSymlink::refuse};

// The layout pins of the public structs, as the core asserts them: a face built against a header whose layout
// moved fails here before it fails at run time.
static_assert(sizeof(bc_file_system_error) == 284);
static_assert(offsetof(bc_file_system_error, name) == 21);
static_assert(offsetof(bc_file_system_error, failed_location) == 280);
static_assert(sizeof(bc_file_system_location) == (sizeof(void*) == 8 ? 32 : 16));
static_assert(offsetof(bc_file_system_location, length) == (sizeof(void*) == 8 ? 24 : 12));
static_assert(sizeof(bc_file_system_read_limit) == 16);
static_assert(offsetof(bc_file_system_read_limit, at_most) == 8);
static_assert(sizeof(bc_file_system_permissions) == 8);
static_assert(offsetof(bc_file_system_permissions, mode) == 4);
static_assert(sizeof(bc_file_system_write_options) == 20);
static_assert(offsetof(bc_file_system_write_options, final_symlink) == 16);
static_assert(sizeof(bc_file_system_read_options) == 24);
static_assert(offsetof(bc_file_system_read_options, final_symlink) == 16);
static_assert(sizeof(bc_file_system_status) == 48);
static_assert(offsetof(bc_file_system_status, link_count) == 40);
static_assert(sizeof(bc_file_system_entry) == 264);
static_assert(offsetof(bc_file_system_entry, name) == 8);
static_assert(sizeof(bc_file_system_fault) == 20);
static_assert(offsetof(bc_file_system_fault, system_code) == 16);

auto a_c_error(std::int32_t kind, std::int32_t operation, std::int32_t step, std::int32_t system_code,
               std::string_view name) -> bc_file_system_error {
    bc_file_system_error c_error{};
    c_error.kind = kind;
    c_error.operation = operation;
    c_error.step = step;
    c_error.system_code = system_code;
    c_error.name_length = static_cast<std::uint32_t>(name.size());
    std::memcpy(static_cast<void*>(c_error.name), name.data(), name.size());
    return c_error;
}

// An error struct full of leftovers, so a test can tell that a call cleared it.
auto a_dirty_c_error() -> bc_file_system_error {
    bc_file_system_error c_error{};
    c_error.kind = 99;
    c_error.operation = 99;
    c_error.step = 99;
    c_error.system_code = 99;
    c_error.name_length = 99;
    c_error.name_truncated = 1;
    std::memset(static_cast<void*>(c_error.name), 'x', sizeof(c_error.name));
    c_error.failed_location = 99;
    return c_error;
}

auto is_all_zero(const bc_file_system_error& c_error) -> bool {
    const bc_file_system_error zero{};
    return std::memcmp(&c_error, &zero, sizeof(c_error)) == 0;
}

// -------------------------------------------------------------------------------------------------------------
// Every operation reaches C, through the face
// -------------------------------------------------------------------------------------------------------------

class BindingTest : public bc::file_system_test::DescriptorBalanced {
protected:
    auto SetUp() -> void override {
        ASSERT_TRUE(directory_.has_value());
        ASSERT_TRUE(engine_.has_value()) << engine_.error().message();
        files_.emplace(engine_.value());
    }

    [[nodiscard]] auto files() -> FileSystem& { return *files_; }
    [[nodiscard]] auto engine() -> Engine& { return engine_.value(); }

    // A location inside the test's own directory, and the text an error about it carries.
    [[nodiscard]] auto in(std::string_view relative) const -> Location { return Location::at(path_of(relative)); }
    [[nodiscard]] auto root() const -> Location { return Location::at(directory_->path()); }
    [[nodiscard]] auto path_of(std::string_view relative) const -> std::filesystem::path {
        return directory_->path() / relative;
    }
    [[nodiscard]] auto path_text_of(std::string_view relative) const -> std::string {
        return path_of(relative).string();
    }

private:
    std::optional<TemporaryDirectory> directory_ = TemporaryDirectory::create_under(temporary_root());
    Result<Engine> engine_ = Engine::observed_for_tests();
    std::optional<FileSystem> files_;
};

// The one operation that is real: the handle comes back, and `Directory::opened_as` reads the text the core
// remembers, which is the binding of `bc_file_system_directory_opened_as`.
TEST_F(BindingTest, OpenDirectoryReachesC) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const Result<Directory> directory = files().open_directory(in("folder"), FinalSymlink::refuse);

    ASSERT_TRUE(directory.has_value()) << directory.error().message();
    const Result<std::string> opened_as = directory->opened_as();
    ASSERT_TRUE(opened_as.has_value()) << opened_as.error().message();
    EXPECT_EQ(*opened_as, path_text_of("folder"));
}

// The status operation is real: the answer comes back through the face's conversion of the C struct.
TEST_F(BindingTest, StatusReachesC) {
    bc::file_system_test::write_raw(path_of("file"), "seven b", 0640);

    const auto status = files().status(in("file"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value()) << status.error().message();
    ASSERT_TRUE(status->has_value());
    EXPECT_EQ((*status)->entry_type, EntryType::regular_file);
    EXPECT_EQ((*status)->size, 7);
    EXPECT_EQ((*status)->mode, 0640U);
}

// The read operation is real: the bytes come back through the face's buffer protocol.
TEST_F(BindingTest, ReadFileReachesC) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto bytes = files().read_file(in("file"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
    EXPECT_EQ(text_of(*bytes), "content");
}

// The write operation is real: the bytes and the mode land, and the durability achieved comes back through
// the face's conversion.
TEST_F(BindingTest, WriteFileReachesC) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("file")), 0644U);
}

// The append operation is real: a file it creates gets the mode, and the bytes are in it.
TEST_F(BindingTest, AppendReachesC) {
    const auto bytes = bytes_of("entry\n");

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "entry\n");
}

// The sync operation is real: a file this platform can sync answers `durable`.
TEST_F(BindingTest, SyncReachesC) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto achieved = files().sync(in("file"));

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(*achieved, Durability::durable);
}

// The directory operations are real: each one reaches the core and its answer comes back through the face.
TEST_F(BindingTest, CreateDirectoryReachesC) {
    const auto created = files().create_directory(in("folder"), Permissions::exactly(0755));

    ASSERT_TRUE(created.has_value()) << created.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("folder")), 0755U);
}

TEST_F(BindingTest, CreateDirectoriesReachesC) {
    const auto created = files().create_directories(in("a/b/c"), Permissions::exactly(0755));

    ASSERT_TRUE(created.has_value()) << created.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b/c")), 0755U);
}

TEST_F(BindingTest, ListDirectoryReachesC) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::write_raw(path_of("folder/file"), "content", 0644);

    const auto entries = files().list_directory(in("folder"), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value()) << entries.error().message();
    EXPECT_THAT(*entries, ElementsAre(Entry{.entry_type = EntryType::regular_file, .name = "file"}));
}

TEST_F(BindingTest, RemoveFileReachesC) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto removed = files().remove_file(in("file"));

    ASSERT_TRUE(removed.has_value()) << removed.error().message();
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("file")));
}

TEST_F(BindingTest, RemoveDirectoryReachesC) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto removed = files().remove_directory(in("folder"));

    ASSERT_TRUE(removed.has_value()) << removed.error().message();
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("folder")));
}

TEST_F(BindingTest, RemoveTreeReachesC) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::make_directory_raw(path_of("tree/sub"), 0755);
    bc::file_system_test::write_raw(path_of("tree/sub/file"), "content", 0644);

    const auto removed = files().remove_tree(in("tree"));

    ASSERT_TRUE(removed.has_value()) << removed.error().message();
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("tree")));
}

TEST_F(BindingTest, CreateTemporaryDirectoryReachesC) {
    bc::file_system_test::make_directory_raw(path_of("parent"), 0755);

    const auto name = files().create_temporary_directory(in("parent"), Permissions::exactly(0700));

    ASSERT_TRUE(name.has_value()) << name.error().message();
    EXPECT_FALSE(name->empty());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("parent") / *name), 0700U);
}

// A rename is real, and the error of a two-location operation names the end it belongs to: the face reads the
// core's `failed_location` and composes the path from the source or the destination accordingly.
TEST_F(BindingTest, RenameReachesCAndItsErrorNamesTheEndThatFailed) {
    bc::file_system_test::write_raw(path_of("from"), "content", 0644);

    const auto moved = files().rename(in("from"), in("to"), ExistingDestination::replace, Durability::written);
    const auto of_nothing = files().rename(in("from"), in("to"), ExistingDestination::replace, Durability::written);
    const auto into_nothing =
        files().rename(in("to"), in("missing/to"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(moved.has_value()) << moved.error().message();
    EXPECT_EQ(*moved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("to")), "content");
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("from")));
    ASSERT_FALSE(of_nothing.has_value());
    EXPECT_EQ(of_nothing.error().kind, ErrorKind::not_found);
    EXPECT_EQ(of_nothing.error().operation, Operation::rename);
    EXPECT_EQ(of_nothing.error().failed_location, FailedLocation::first);
    EXPECT_EQ(of_nothing.error().path, path_text_of("from"));
    ASSERT_FALSE(into_nothing.has_value());
    EXPECT_EQ(into_nothing.error().kind, ErrorKind::not_found);
    EXPECT_EQ(into_nothing.error().failed_location, FailedLocation::second);
    EXPECT_EQ(into_nothing.error().path, path_text_of("missing/to"));
}

TEST_F(BindingTest, CopyFileReachesCAndItsErrorNamesTheEndThatFailed) {
    bc::file_system_test::write_raw(path_of("from"), "content", 0644);

    const auto copied = files().copy_file(in("from"), in("to"), kWrittenExactly0644);
    const auto of_nothing = files().copy_file(in("absent"), in("to"), kWrittenExactly0644);
    const auto into_nothing = files().copy_file(in("from"), in("missing/to"), kWrittenExactly0644);

    ASSERT_TRUE(copied.has_value()) << copied.error().message();
    EXPECT_EQ(*copied, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("to")), "content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("from")), "content");
    ASSERT_FALSE(of_nothing.has_value());
    EXPECT_EQ(of_nothing.error().kind, ErrorKind::not_found);
    EXPECT_EQ(of_nothing.error().operation, Operation::copy_file);
    EXPECT_EQ(of_nothing.error().failed_location, FailedLocation::first);
    EXPECT_EQ(of_nothing.error().path, path_text_of("absent"));
    ASSERT_FALSE(into_nothing.has_value());
    EXPECT_EQ(into_nothing.error().failed_location, FailedLocation::second);
    EXPECT_EQ(into_nothing.error().path, path_text_of("missing/to"));
}

TEST_F(BindingTest, SetPermissionsReachesC) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto changed = files().set_permissions(in("file"), 0600, FinalSymlink::refuse);

    ASSERT_TRUE(changed.has_value()) << changed.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("file")), 0600U);
}

// The link operations are real: the target text crosses as bytes, lands, and comes back through the buffer
// protocol.
TEST_F(BindingTest, CreateSymlinkReachesC) {
    const auto created = files().create_symlink("target", in("link"));

    ASSERT_TRUE(created.has_value()) << created.error().message();
    EXPECT_TRUE(bc::file_system_test::is_symlink_raw(path_of("link")));
}

TEST_F(BindingTest, ReadSymlinkReachesC) {
    bc::file_system_test::make_symlink_raw("target", path_of("link"));

    const auto text = files().read_symlink(in("link"));

    ASSERT_TRUE(text.has_value()) << text.error().message();
    EXPECT_EQ(*text, "target");
}

// The lock is real: the handle comes back under `acquired`, a second ask answers `held_elsewhere` as an empty
// optional, and the handle's destructor lets the lock go.
TEST_F(BindingTest, TryLockReachesC) {
    bc::file_system_test::write_raw(path_of("file.lock"), "", 0644);

    {
        const auto held = files().try_lock(in("file.lock"));
        ASSERT_TRUE(held.has_value()) << held.error().message();
        EXPECT_TRUE(held->has_value());
        const auto refused = files().try_lock(in("file.lock"));
        ASSERT_TRUE(refused.has_value()) << refused.error().message();
        EXPECT_FALSE(refused->has_value());
    }
    const auto again = files().try_lock(in("file.lock"));

    ASSERT_TRUE(again.has_value()) << again.error().message();
    EXPECT_TRUE(again->has_value());
}

TEST_F(BindingTest, CondemnReachesC) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);

    const auto name = files().condemn(in("tree"));

    ASSERT_TRUE(name.has_value()) << name.error().message();
    EXPECT_EQ(*name, ".bc-condemned-tree");
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of(".bc-condemned-tree")));
}

TEST_F(BindingTest, RemoveCondemnedReachesC) {
    bc::file_system_test::make_directory_raw(path_of("parent"), 0755);
    bc::file_system_test::make_directory_raw(path_of("parent/.bc-condemned-tree"), 0755);
    bc::file_system_test::write_raw(path_of("parent/kept"), "kept", 0644);

    const auto swept = files().remove_condemned(in("parent"));

    ASSERT_TRUE(swept.has_value()) << swept.error().message();
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("parent")), ElementsAre("kept"));
}

// The observer's three calls reach C: on an observed engine each succeeds, and a fresh engine has counted
// nothing. (Their refusal on a live engine is the observer suite's.)
TEST_F(BindingTest, AddFaultClearFaultsAndStepCountReachC) {
    const Fault fault{.operation = Operation::write_file,
                      .step = Step::write,
                      .occurrence = 1,
                      .action = FaultAction::fail,
                      .system_code = ENOSPC};

    const Result<void> added = engine().add_fault(fault);
    const Result<void> cleared = engine().clear_faults();
    const Result<std::uint64_t> counted = engine().step_count(Operation::write_file, Step::write);

    EXPECT_TRUE(added.has_value());
    EXPECT_TRUE(cleared.has_value());
    ASSERT_TRUE(counted.has_value());
    EXPECT_EQ(*counted, 0U);
}

// `Directory::opened_as` is bound by `OpenDirectoryReachesC` above; the `beneath` composition of the remembered
// text is the contract suite's.

// -------------------------------------------------------------------------------------------------------------
// The face's compositions
// -------------------------------------------------------------------------------------------------------------

// Absence becomes an empty optional; every other error passes through as received.
TEST_F(BindingTest, ReadFileIfPresentPassesAnErrorOtherThanNotFoundThrough) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto bytes = files().read_file_if_present(in("folder"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_regular_file);
    EXPECT_EQ(bytes.error().operation, Operation::read_file);
    EXPECT_EQ(bytes.error().path, path_text_of("folder"));
}

TEST_F(BindingTest, ReadFileGrowsItsBufferToAFileLargerThanTheFirstCapacity) {
    // The face offers 1 MiB first; the core names the capacity it needs and the face asks once more.
    const std::string large((std::size_t{1} << 20) + (std::size_t{100} * 1024), 'g');
    bc::file_system_test::write_raw(path_of("large.bin"), large, 0644);

    const auto bytes = files().read_file(in("large.bin"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(text_of(*bytes), large);
}

// The face's buffer is never zero-filled, so an answer shorter than the buffer it was read into must hold the
// file's own bytes and nothing of what the buffer held before: a large read first leaves its bytes where the
// allocator may hand the next buffer out again, and the short file read next is answered as exactly its bytes,
// its size its own, under either limit.
TEST_F(BindingTest, ReadFileOfAFileShorterThanTheFirstOfferAnswersOnlyItsOwnBytes) {
    const std::string large(std::size_t{900} * 1024, 'L');
    bc::file_system_test::write_raw(path_of("large.bin"), large, 0644);
    bc::file_system_test::write_raw(path_of("short.txt"), "seven b", 0644);
    const ReadOptions at_most_a_mebibyte{.limit = ReadLimit::at_most(std::uint64_t{1} << 20),
                                         .final_symlink = FinalSymlink::refuse};

    const auto first = files().read_file(in("large.bin"), kWholeFileRefusingSymlink);
    const auto whole = files().read_file(in("short.txt"), kWholeFileRefusingSymlink);
    const auto again = files().read_file(in("large.bin"), kWholeFileRefusingSymlink);
    const auto bounded = files().read_file(in("short.txt"), at_most_a_mebibyte);

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->size(), large.size());
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->size(), 7U);
    EXPECT_EQ(text_of(*whole), "seven b");
    EXPECT_LT(whole->capacity(), 1024U) << "an answer filling less than half its storage is moved into its own size";
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(text_of(*again), large);
    ASSERT_TRUE(bounded.has_value());
    EXPECT_EQ(bounded->size(), 7U);
    EXPECT_EQ(text_of(*bounded), "seven b");
}

TEST_F(BindingTest, ListDirectoryGrowsItsArrayPastTheFirstCapacity) {
    // The face offers room for 1,024 entries first.
    std::vector<std::string> expected;
    for (int number = 1000; number < 2100; ++number) {
        const std::string name = "entry-" + std::to_string(number);
        bc::file_system_test::write_raw(path_of(name), "", 0644);
        expected.push_back(name);
    }

    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    std::vector<std::string> listed;
    for (const Entry& entry : *entries) {
        listed.push_back(entry.name);
    }
    EXPECT_EQ(listed, expected);
}

TEST(LinesOfTest, SplitsOnNewlineOnlyAndLeavesEveryOtherByteAlone) {
    const auto buffer = bytes_of("one\ntwo\r\nthree");

    EXPECT_THAT(lines_of(buffer), ElementsAre("one", "two\r", "three"));
}

TEST(LinesOfTest, ATrailingNewlineAddsNoEmptyLine) {
    const auto buffer = bytes_of("one\ntwo\n");

    EXPECT_THAT(lines_of(buffer), ElementsAre("one", "two"));
}

TEST(LinesOfTest, AnEmptyLineInTheMiddleIsALine) {
    const auto buffer = bytes_of("one\n\nthree");

    EXPECT_THAT(lines_of(buffer), ElementsAre("one", "", "three"));
}

TEST(LinesOfTest, AnEmptyBufferHasNoLines) {
    const std::vector<std::byte> buffer;

    EXPECT_THAT(lines_of(buffer), IsEmpty());
}

TEST(LinesOfTest, EachLineViewsIntoTheBuffer) {
    const auto buffer = bytes_of("one\ntwo");

    const std::vector<std::string_view> lines = lines_of(buffer);

    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(static_cast<const void*>(lines[0].data()), static_cast<const void*>(buffer.data()));
    EXPECT_EQ(static_cast<const void*>(lines[1].data()), static_cast<const void*>(buffer.data() + 4));
}

// -------------------------------------------------------------------------------------------------------------
// The error mapping is total: each C value names one enumerator, and each enumerator has its name
// -------------------------------------------------------------------------------------------------------------

TEST(ErrorMappingTest, EveryErrorKindHasItsNameAndItsCValue) {
    const std::array<std::string_view, 15> names = {
        "not_found",           "not_regular_file", "not_directory",    "permission_denied", "already_exists",
        "directory_not_empty", "symlink_refused",  "too_large",        "cross_device",      "no_space",
        "resource_exhausted",  "unsupported",      "invalid_argument", "io_failed",         "buffer_too_small"};

    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto value = static_cast<std::int32_t>(index + 1);
        const std::optional<ErrorKind> kind = bc::file_system::error_kind_from(value);
        ASSERT_TRUE(kind.has_value()) << value;
        EXPECT_EQ(static_cast<std::int32_t>(*kind), value);
        EXPECT_EQ(name_of(*kind), names[index]);
    }
    EXPECT_FALSE(bc::file_system::error_kind_from(0).has_value());
    EXPECT_FALSE(bc::file_system::error_kind_from(16).has_value());
    EXPECT_FALSE(bc::file_system::error_kind_from(-1).has_value());
}

// The first six equal bourgeois_concern's FileIoError::Kind by name and value, so its conversion is a table.
TEST(ErrorMappingTest, TheFirstSixKindsKeepTheirValues) {
    EXPECT_EQ(static_cast<std::int32_t>(ErrorKind::not_found), 1);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorKind::not_regular_file), 2);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorKind::not_directory), 3);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorKind::permission_denied), 4);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorKind::already_exists), 5);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorKind::directory_not_empty), 6);
}

TEST(ErrorMappingTest, EveryOperationHasItsNameAndItsCValue) {
    const std::array<std::string_view, 40> names = {"engine_create_live",
                                                    "engine_create_observed_for_tests",
                                                    "open_directory",
                                                    "directory_opened_as",
                                                    "observed_fault_add",
                                                    "observed_faults_clear",
                                                    "observed_step_count",
                                                    "status",
                                                    "read_file",
                                                    "write_file",
                                                    "append",
                                                    "sync",
                                                    "create_directory",
                                                    "create_directories",
                                                    "list_directory",
                                                    "remove_file",
                                                    "remove_directory",
                                                    "remove_tree",
                                                    "create_temporary_directory",
                                                    "rename",
                                                    "copy_file",
                                                    "set_permissions",
                                                    "create_symlink",
                                                    "read_symlink",
                                                    "try_lock",
                                                    "condemn",
                                                    "remove_condemned",
                                                    "relative_path_from",
                                                    "absolute_path_from",
                                                    "path_join",
                                                    "path_parent",
                                                    "path_last_component",
                                                    "path_next_component",
                                                    "path_stem",
                                                    "path_extension",
                                                    "path_relate",
                                                    "path_lexically_normalize",
                                                    "path_resolve_against",
                                                    "path_library_name",
                                                    "engine_create_portable_only_for_tests"};

    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto value = static_cast<std::int32_t>(index + 1);
        const std::optional<Operation> operation = bc::file_system::operation_from(value);
        ASSERT_TRUE(operation.has_value()) << value;
        EXPECT_EQ(static_cast<std::int32_t>(*operation), value);
        EXPECT_EQ(name_of(*operation), names[index]);
    }
    EXPECT_FALSE(bc::file_system::operation_from(0).has_value());
    EXPECT_FALSE(bc::file_system::operation_from(41).has_value());
}

TEST(ErrorMappingTest, EveryStepHasItsNameAndItsCValue) {
    const std::array<std::string_view, 27> names = {"open",
                                                    "openat",
                                                    "fstat",
                                                    "fstatat",
                                                    "read",
                                                    "pread",
                                                    "write",
                                                    "fsync",
                                                    "fcntl_fullfsync",
                                                    "renameat",
                                                    "rename_no_replace",
                                                    "unlinkat",
                                                    "mkdirat",
                                                    "fdopendir",
                                                    "readdir",
                                                    "symlinkat",
                                                    "readlinkat",
                                                    "flock",
                                                    "fchmod",
                                                    "copy_range",
                                                    "clone_file",
                                                    "close",
                                                    "identity_check",
                                                    "openat2",
                                                    "openat_nofollow_any",
                                                    "fchmodat",
                                                    "faccessat"};

    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto value = static_cast<std::int32_t>(index + 1);
        const std::optional<Step> step = bc::file_system::step_from(value);
        ASSERT_TRUE(step.has_value()) << value;
        EXPECT_EQ(static_cast<std::int32_t>(*step), value);
        EXPECT_EQ(name_of(*step), names[index]);
    }
    EXPECT_FALSE(bc::file_system::step_from(0).has_value());
    EXPECT_FALSE(bc::file_system::step_from(28).has_value());
}

// The small enums, each enumerator against the header's constant.
TEST(ErrorMappingTest, EverySmallEnumIsPinnedToTheHeader) {
    EXPECT_EQ(static_cast<std::int32_t>(FinalSymlink::refuse), bc_file_system_final_symlink_refuse);
    EXPECT_EQ(static_cast<std::int32_t>(FinalSymlink::follow), bc_file_system_final_symlink_follow);
    EXPECT_EQ(static_cast<std::int32_t>(ExistingDestination::replace), bc_file_system_existing_destination_replace);
    EXPECT_EQ(static_cast<std::int32_t>(ExistingDestination::refuse), bc_file_system_existing_destination_refuse);
    EXPECT_EQ(static_cast<std::int32_t>(Durability::written), bc_file_system_durability_written);
    EXPECT_EQ(static_cast<std::int32_t>(Durability::durable), bc_file_system_durability_durable);
    EXPECT_EQ(static_cast<std::int32_t>(ReadLimitKind::whole_file), bc_file_system_read_limit_kind_whole_file);
    EXPECT_EQ(static_cast<std::int32_t>(ReadLimitKind::at_most), bc_file_system_read_limit_kind_at_most);
    EXPECT_EQ(static_cast<std::int32_t>(PermissionsPolicy::exactly), bc_file_system_permissions_policy_exactly);
    EXPECT_EQ(static_cast<std::int32_t>(PermissionsPolicy::preserve_or), bc_file_system_permissions_policy_preserve_or);
    EXPECT_EQ(static_cast<std::int32_t>(LocationKind::at), bc_file_system_location_kind_at);
    EXPECT_EQ(static_cast<std::int32_t>(LocationKind::beneath), bc_file_system_location_kind_beneath);
    EXPECT_EQ(static_cast<std::int32_t>(EntryType::regular_file), bc_file_system_entry_type_regular_file);
    EXPECT_EQ(static_cast<std::int32_t>(EntryType::directory), bc_file_system_entry_type_directory);
    EXPECT_EQ(static_cast<std::int32_t>(EntryType::symlink), bc_file_system_entry_type_symlink);
    EXPECT_EQ(static_cast<std::int32_t>(EntryType::other), bc_file_system_entry_type_other);
    EXPECT_EQ(static_cast<std::int32_t>(LockOutcome::acquired), bc_file_system_lock_outcome_acquired);
    EXPECT_EQ(static_cast<std::int32_t>(LockOutcome::held_elsewhere), bc_file_system_lock_outcome_held_elsewhere);
    EXPECT_EQ(static_cast<std::int32_t>(FaultAction::fail), bc_file_system_fault_action_fail);
    EXPECT_EQ(static_cast<std::int32_t>(FaultAction::stop), bc_file_system_fault_action_stop);
}

TEST(ErrorMappingTest, TheFailedLocationIsEmptyForZeroAndForAValueOutsideItsRange) {
    EXPECT_FALSE(bc::file_system::failed_location_from(0).has_value());
    EXPECT_EQ(bc::file_system::failed_location_from(1), FailedLocation::first);
    EXPECT_EQ(bc::file_system::failed_location_from(2), FailedLocation::second);
    EXPECT_FALSE(bc::file_system::failed_location_from(3).has_value());
    EXPECT_FALSE(bc::file_system::failed_location_from(-1).has_value());
}

TEST(ErrorMappingTest, TheSmallEnumsRefuseAValueOutsideTheirRange) {
    EXPECT_FALSE(bc::file_system::durability_from(0).has_value());
    EXPECT_EQ(bc::file_system::durability_from(2), Durability::durable);
    EXPECT_FALSE(bc::file_system::durability_from(3).has_value());
    EXPECT_FALSE(bc::file_system::entry_type_from(0).has_value());
    EXPECT_EQ(bc::file_system::entry_type_from(4), EntryType::other);
    EXPECT_FALSE(bc::file_system::entry_type_from(5).has_value());
    EXPECT_FALSE(bc::file_system::lock_outcome_from(0).has_value());
    EXPECT_EQ(bc::file_system::lock_outcome_from(1), LockOutcome::acquired);
    EXPECT_FALSE(bc::file_system::lock_outcome_from(3).has_value());
}

// -------------------------------------------------------------------------------------------------------------
// The error struct crosses once, in error_from, and renders in Error::message
// -------------------------------------------------------------------------------------------------------------

TEST(ErrorConversionTest, ComposesThePathFromTheLocationAndTheEntryName) {
    const bc_file_system_error c_error =
        a_c_error(bc_file_system_error_kind_not_found, bc_file_system_operation_read_file, bc_file_system_step_openat,
                  ENOENT, "child");

    const Error error = bc::file_system::error_from(c_error, Operation::read_file, "/a/b");

    EXPECT_EQ(error.kind, ErrorKind::not_found);
    EXPECT_EQ(error.operation, Operation::read_file);
    EXPECT_EQ(error.step, Step::openat);
    EXPECT_EQ(error.system_code, ENOENT);
    EXPECT_EQ(error.path, "/a/b/child");
    EXPECT_FALSE(error.name_truncated);
}

TEST(ErrorConversionTest, ThePathIsTheLocationAloneWhenTheStepActedOnIt) {
    const bc_file_system_error c_error =
        a_c_error(bc_file_system_error_kind_permission_denied, bc_file_system_operation_status,
                  bc_file_system_step_fstatat, EACCES, "");

    const Error error = bc::file_system::error_from(c_error, Operation::status, "/a/b");

    EXPECT_EQ(error.path, "/a/b");
}

TEST(ErrorConversionTest, ThePathIsTheNameAloneWhenTheCallTookNoLocation) {
    const bc_file_system_error c_error =
        a_c_error(bc_file_system_error_kind_io_failed, bc_file_system_operation_engine_create_live, 0, EIO, "probe");

    const Error error = bc::file_system::error_from(c_error, Operation::engine_create_live, "");

    EXPECT_EQ(error.path, "probe");
    EXPECT_FALSE(error.step.has_value());
}

// The field that says which end of a two-location operation failed crosses as itself, and the two-location
// conversion composes the path from the end it names: the first, the second, or the second when it names
// neither.
TEST(ErrorConversionTest, TheFailedLocationCrossesAndChoosesThePathOfATwoLocationError) {
    bc_file_system_error at_the_source = a_c_error(bc_file_system_error_kind_not_found, bc_file_system_operation_rename,
                                                   bc_file_system_step_fstatat, ENOENT, "");
    at_the_source.failed_location = 1;
    bc_file_system_error at_the_destination =
        a_c_error(bc_file_system_error_kind_already_exists, bc_file_system_operation_rename,
                  bc_file_system_step_rename_no_replace, EEXIST, "");
    at_the_destination.failed_location = 2;
    const bc_file_system_error at_neither =
        a_c_error(bc_file_system_error_kind_invalid_argument, bc_file_system_operation_rename, 0, 0, "");

    const Error source = bc::file_system::error_from_two_locations(at_the_source, Operation::rename, "/from", "/to");
    const Error destination =
        bc::file_system::error_from_two_locations(at_the_destination, Operation::rename, "/from", "/to");
    const Error neither = bc::file_system::error_from_two_locations(at_neither, Operation::rename, "/from", "/to");
    const Error one_location = bc::file_system::error_from(at_neither, Operation::rename, "/one");

    EXPECT_EQ(source.failed_location, FailedLocation::first);
    EXPECT_EQ(source.path, "/from");
    EXPECT_EQ(destination.failed_location, FailedLocation::second);
    EXPECT_EQ(destination.path, "/to");
    EXPECT_FALSE(neither.failed_location.has_value());
    EXPECT_EQ(neither.path, "/to");
    EXPECT_FALSE(one_location.failed_location.has_value());
    EXPECT_EQ(one_location.path, "/one");
}

// The core's name field is bounded; a longer name arrives cut short and flagged, never silently.
TEST(ErrorConversionTest, ANameLongerThanTheFieldArrivesCutShortAndFlagged) {
    const std::string long_name(sizeof(bc_file_system_error::name), 'n');
    bc_file_system_error c_error = a_c_error(bc_file_system_error_kind_not_found, bc_file_system_operation_status,
                                             bc_file_system_step_fstatat, ENAMETOOLONG, long_name);
    c_error.name_truncated = 1;

    const Error error = bc::file_system::error_from(c_error, Operation::status, "/a");

    EXPECT_EQ(error.path, "/a/" + long_name);
    EXPECT_TRUE(error.name_truncated);
}

// A core and a face built together cannot disagree on an enum (the pins above), so the stand-ins are never
// reached in practice; they are what the face answers rather than trusting a foreign integer.
TEST(ErrorConversionTest, AValueOutsideTheEnumGetsAStandIn) {
    const bc_file_system_error c_error = a_c_error(99, 99, 99, 0, "");

    const Error error = bc::file_system::error_from(c_error, Operation::sync, "/a");

    EXPECT_EQ(error.kind, ErrorKind::io_failed);
    EXPECT_EQ(error.operation, Operation::sync);
    EXPECT_FALSE(error.step.has_value());
}

TEST(ErrorConversionTest, TheMessageNamesTheOperationThePathAndTheSystemsWording) {
    const Error error{.kind = ErrorKind::not_found,
                      .operation = Operation::read_file,
                      .step = Step::openat,
                      .system_code = ENOENT,
                      .path = "/etc/config.toml",
                      .name_truncated = false,
                      .failed_location = std::nullopt};

    EXPECT_EQ(error.message(), "read_file: /etc/config.toml: " + std::system_category().message(ENOENT));
}

TEST(ErrorConversionTest, TheMessageNamesTheKindWhenNoSystemCallFailed) {
    const Error error{.kind = ErrorKind::invalid_argument,
                      .operation = Operation::write_file,
                      .step = std::nullopt,
                      .system_code = 0,
                      .path = "relative.txt",
                      .name_truncated = false,
                      .failed_location = std::nullopt};

    EXPECT_EQ(error.message(), "write_file: relative.txt: invalid_argument");
}

TEST(ErrorConversionTest, TheMessageDropsThePathSegmentWhenTheCallTookNoLocation) {
    const Error error{.kind = ErrorKind::resource_exhausted,
                      .operation = Operation::engine_create_live,
                      .step = std::nullopt,
                      .system_code = 0,
                      .path = "",
                      .name_truncated = false,
                      .failed_location = std::nullopt};

    EXPECT_EQ(error.message(), "engine_create_live: resource_exhausted");
}

// -------------------------------------------------------------------------------------------------------------
// One ABI round trip per value type
// -------------------------------------------------------------------------------------------------------------

TEST(AbiRoundTripTest, ReadLimitCrossesAsItsKindAndItsBound) {
    const bc_file_system_read_limit whole = bc::file_system::to_c(ReadLimit::whole_file());
    const bc_file_system_read_limit bounded = bc::file_system::to_c(ReadLimit::at_most(512));

    EXPECT_EQ(whole.kind, bc_file_system_read_limit_kind_whole_file);
    EXPECT_EQ(whole.at_most, 0U);
    EXPECT_EQ(bounded.kind, bc_file_system_read_limit_kind_at_most);
    EXPECT_EQ(bounded.at_most, 512U);
    EXPECT_EQ(ReadLimit::at_most(512).bound(), 512U);
    EXPECT_EQ(ReadLimit::at_most(512).kind(), ReadLimitKind::at_most);
}

TEST(AbiRoundTripTest, PermissionsCrossAsPolicyAndMode) {
    const bc_file_system_permissions exactly = bc::file_system::to_c(Permissions::exactly(0640));
    const bc_file_system_permissions preserving = bc::file_system::to_c(Permissions::preserve_or(0600));

    EXPECT_EQ(exactly.policy, bc_file_system_permissions_policy_exactly);
    EXPECT_EQ(exactly.mode, 0640U);
    EXPECT_EQ(preserving.policy, bc_file_system_permissions_policy_preserve_or);
    EXPECT_EQ(preserving.mode, 0600U);
}

TEST(AbiRoundTripTest, WriteOptionsCrossAsFourFields) {
    const bc_file_system_write_options c_options = bc::file_system::to_c(kDurableExactly0600);

    EXPECT_EQ(c_options.durability, bc_file_system_durability_durable);
    EXPECT_EQ(c_options.permissions.policy, bc_file_system_permissions_policy_exactly);
    EXPECT_EQ(c_options.permissions.mode, 0600U);
    EXPECT_EQ(c_options.existing_destination, bc_file_system_existing_destination_replace);
    EXPECT_EQ(c_options.final_symlink, bc_file_system_final_symlink_refuse);
}

TEST(AbiRoundTripTest, ReadOptionsCrossAsLimitAndFinalSymlink) {
    const bc_file_system_read_options c_options = bc::file_system::to_c(kWholeFileRefusingSymlink);

    EXPECT_EQ(c_options.limit.kind, bc_file_system_read_limit_kind_whole_file);
    EXPECT_EQ(c_options.final_symlink, bc_file_system_final_symlink_refuse);
}

TEST(AbiRoundTripTest, StatusCrossesAndComesBackEqual) {
    const Status status{.entry_type = EntryType::regular_file,
                        .size = 1234,
                        .mode = 0640,
                        .device = 16777220,
                        .inode = 98765432101,
                        .link_count = 2};

    const bc_file_system_status c_status = bc::file_system::to_c(status);
    const std::optional<Status> back = bc::file_system::status_from(c_status);

    EXPECT_EQ(c_status.entry_type, bc_file_system_entry_type_regular_file);
    EXPECT_EQ(c_status.size, 1234);
    EXPECT_EQ(c_status.link_count, 2U);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, status);
}

TEST(AbiRoundTripTest, AStatusWithAnEntryTypeOutsideTheEnumIsRefused) {
    bc_file_system_status c_status{};
    c_status.entry_type = 0;

    EXPECT_FALSE(bc::file_system::status_from(c_status).has_value());
}

TEST(AbiRoundTripTest, AnEntryCrossesAsItsTypeAndItsNameBytes) {
    bc_file_system_entry c_entry{};
    c_entry.entry_type = bc_file_system_entry_type_symlink;
    const std::string_view name = "notes.txt";
    c_entry.name_length = static_cast<std::uint32_t>(name.size());
    std::memcpy(static_cast<void*>(c_entry.name), name.data(), name.size());

    const std::optional<Entry> entry = bc::file_system::entry_from(c_entry);

    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(*entry, (Entry{.entry_type = EntryType::symlink, .name = "notes.txt"}));
}

TEST(AbiRoundTripTest, AnEntryWithABadTypeOrAnImpossibleLengthIsRefused) {
    bc_file_system_entry bad_type{};
    bad_type.entry_type = 7;
    bad_type.name_length = 1;
    bc_file_system_entry too_long{};
    too_long.entry_type = bc_file_system_entry_type_regular_file;
    too_long.name_length = sizeof(too_long.name) + 1;

    EXPECT_FALSE(bc::file_system::entry_from(bad_type).has_value());
    EXPECT_FALSE(bc::file_system::entry_from(too_long).has_value());
}

TEST(AbiRoundTripTest, AFaultCrossesAndComesBackEqual) {
    const Fault fault{.operation = Operation::write_file,
                      .step = Step::fcntl_fullfsync,
                      .occurrence = 2,
                      .action = FaultAction::fail,
                      .system_code = EIO};

    const bc_file_system_fault c_fault = bc::file_system::to_c(fault);
    const std::optional<Fault> back = bc::file_system::fault_from(c_fault);

    EXPECT_EQ(c_fault.operation, bc_file_system_operation_write_file);
    EXPECT_EQ(c_fault.step, bc_file_system_step_fcntl_fullfsync);
    EXPECT_EQ(c_fault.occurrence, 2U);
    EXPECT_EQ(c_fault.action, bc_file_system_fault_action_fail);
    EXPECT_EQ(c_fault.system_code, EIO);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, fault);
}

TEST(AbiRoundTripTest, AFaultWithAFieldOutsideItsEnumIsRefused) {
    bc_file_system_fault c_fault{};
    c_fault.operation = bc_file_system_operation_write_file;
    c_fault.step = bc_file_system_step_write;
    c_fault.occurrence = 1;
    c_fault.action = 0;
    c_fault.system_code = EIO;

    EXPECT_FALSE(bc::file_system::fault_from(c_fault).has_value());
}

TEST(AbiRoundTripTest, ALocationCrossesAsItsKindAndItsBorrowedBytes) {
    const Result<AbsolutePath> path = AbsolutePath::from("/etc/config.toml");
    ASSERT_TRUE(path.has_value());
    const Location location = Location::at(*path);

    const bc_file_system_location c_location = location.to_c();

    EXPECT_EQ(c_location.kind, bc_file_system_location_kind_at);
    EXPECT_EQ(c_location.directory, nullptr);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(c_location.path), c_location.length), "/etc/config.toml");
}

// A `beneath` location's round trip through the core is the contract suite's open_directory cases'.

// -------------------------------------------------------------------------------------------------------------
// Result, the face's outcome type
// -------------------------------------------------------------------------------------------------------------

auto a_failure() -> Error {
    return Error{.kind = ErrorKind::not_found,
                 .operation = Operation::read_file,
                 .step = Step::openat,
                 .system_code = ENOENT,
                 .path = "/some/path",
                 .name_truncated = false,
                 .failed_location = std::nullopt};
}

TEST(ResultTest, HoldsAValue) {
    const Result<std::string> result = std::string{"content"};

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(static_cast<bool>(result));
    EXPECT_EQ(result.value(), "content");
    EXPECT_EQ(*result, "content");
    EXPECT_EQ(result->size(), 7U);
}

TEST(ResultTest, HoldsAnError) {
    const Result<std::string> result = a_failure();

    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.error(), a_failure());
}

TEST(ResultTest, GivesUpAValueThatCanOnlyBeMoved) {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(7);

    const std::unique_ptr<int> taken = std::move(result).value();

    ASSERT_NE(taken, nullptr);
    EXPECT_EQ(*taken, 7);
}

TEST(ResultTest, OfNothingIsSuccessOrAnError) {
    const Result<void> success;
    const Result<void> failure = a_failure();

    EXPECT_TRUE(success.has_value());
    ASSERT_FALSE(failure.has_value());
    EXPECT_EQ(failure.error().kind, ErrorKind::not_found);
}

TEST(ResultDeathTest, AskingAFailureForItsValueEndsTheProcess) {
    const Result<std::string> result = a_failure();

    EXPECT_DEATH(static_cast<void>(result.value()), "");
}

TEST(ResultDeathTest, AskingASuccessForItsErrorEndsTheProcess) {
    const Result<std::string> result = std::string{"content"};

    EXPECT_DEATH(static_cast<void>(result.error()), "");
}

// -------------------------------------------------------------------------------------------------------------
// The header's own promises, at the C level
// -------------------------------------------------------------------------------------------------------------

// An engine created and released through the C functions directly, so a case can hand the core exactly the
// integers it wants to, including the ones the C++ face cannot form.
class CoreBindingTest : public bc::file_system_test::DescriptorBalanced {
protected:
    auto SetUp() -> void override {
        ASSERT_TRUE(directory_.has_value());
        bc_file_system_error c_error{};
        ASSERT_EQ(bc_file_system_engine_create_observed_for_tests(&engine_, &c_error), 0);
    }

    auto TearDown() -> void override { bc_file_system_engine_release(engine_); }

    [[nodiscard]] auto engine() -> bc_file_system_engine* { return engine_; }

    // The path of an entry inside the test's directory, as text that outlives the location borrowing it.
    [[nodiscard]] auto path_text_of(std::string_view relative) const -> std::string {
        return (directory_->path() / relative).string();
    }
    [[nodiscard]] auto root_text() const -> std::string { return directory_->path().string(); }
    [[nodiscard]] auto path_of(std::string_view relative) const -> std::filesystem::path {
        return directory_->path() / relative;
    }

    // An `at` location over `text`, which must outlive the call it is passed to.
    [[nodiscard]] static auto at(const std::string& text) -> bc_file_system_location {
        return bc_file_system_location_at(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    }

private:
    std::optional<TemporaryDirectory> directory_ = TemporaryDirectory::create_under(temporary_root());
    bc_file_system_engine* engine_ = nullptr;
};

TEST_F(CoreBindingTest, ASuccessZeroesTheErrorStruct) {
    bc_file_system_error c_error = a_dirty_c_error();

    const std::int32_t status = bc_file_system_observed_faults_clear(engine(), &c_error);

    EXPECT_EQ(status, 0);
    EXPECT_TRUE(is_all_zero(c_error));
}

// A failure of a real system call fills every field: the kind, the operation, the step that failed and its
// errno; the name stays empty because the step acted on the location itself.
TEST_F(CoreBindingTest, AFailureFillsTheKindAndTheOperationAndReturnsTheKind) {
    const std::string path = path_text_of("absent");
    const bc_file_system_location location = at(path);
    const bc_file_system_read_options c_options = bc::file_system::to_c(kWholeFileRefusingSymlink);
    bc_file_system_error c_error = a_dirty_c_error();
    std::size_t length = 77;

    const std::int32_t status =
        bc_file_system_read_file(engine(), &location, &c_options, nullptr, 0, &length, &c_error);

    EXPECT_EQ(status, bc_file_system_error_kind_not_found);
    EXPECT_EQ(c_error.kind, bc_file_system_error_kind_not_found);
    EXPECT_EQ(c_error.operation, bc_file_system_operation_read_file);
    EXPECT_EQ(c_error.step, bc_file_system_step_openat);
    EXPECT_EQ(c_error.system_code, ENOENT);
    EXPECT_EQ(c_error.name_length, 0U);
    EXPECT_EQ(c_error.name_truncated, 0);
    // The out-parameter is left untouched on failure.
    EXPECT_EQ(length, 77U);
}

TEST_F(CoreBindingTest, ANullErrorPointerIsInvalidArgumentAndWritesNowhere) {
    const std::string path = path_text_of("file");
    const bc_file_system_location location = at(path);
    std::int32_t present = 7;
    bc_file_system_status c_status{};

    const std::int32_t status = bc_file_system_status_of(engine(), &location, bc_file_system_final_symlink_refuse,
                                                         &present, &c_status, nullptr);

    EXPECT_EQ(status, bc_file_system_error_kind_invalid_argument);
    EXPECT_EQ(present, 7);
}

TEST_F(CoreBindingTest, ANullEngineIsInvalidArgument) {
    const std::string path = path_text_of("file");
    const bc_file_system_location location = at(path);
    std::int32_t existed = 0;
    bc_file_system_error c_error{};

    const std::int32_t status = bc_file_system_remove_file(nullptr, &location, &existed, &c_error);

    EXPECT_EQ(status, bc_file_system_error_kind_invalid_argument);
    EXPECT_EQ(c_error.operation, bc_file_system_operation_remove_file);
}

// Zero is never a valid enum input: a zeroed options struct is detectably unfilled, not a silent choice.
TEST_F(CoreBindingTest, AZeroEnumIsInvalidArgument) {
    const std::string path = path_text_of("file");
    const bc_file_system_location location = at(path);
    const bc_file_system_write_options zeroed_options{};
    std::int32_t achieved = 0;
    std::int32_t present = 0;
    bc_file_system_status c_status{};
    bc_file_system_error c_error{};

    const std::int32_t zero_final_symlink =
        bc_file_system_status_of(engine(), &location, 0, &present, &c_status, &c_error);
    const std::int32_t zeroed_struct =
        bc_file_system_write_file(engine(), &location, nullptr, 0, &zeroed_options, &achieved, &c_error);

    EXPECT_EQ(zero_final_symlink, bc_file_system_error_kind_invalid_argument);
    EXPECT_EQ(zeroed_struct, bc_file_system_error_kind_invalid_argument);
    EXPECT_EQ(c_error.operation, bc_file_system_operation_write_file);
}

TEST_F(CoreBindingTest, AnEnumPastItsLastValueIsInvalidArgument) {
    const std::string path = path_text_of("file");
    const bc_file_system_location location = at(path);
    std::int32_t present = 0;
    bc_file_system_status c_status{};
    bc_file_system_error c_error{};
    bc_file_system_fault c_fault{};
    c_fault.operation = bc_file_system_operation_write_file;
    c_fault.step = bc_file_system_step_write;
    c_fault.occurrence = 1;
    c_fault.action = bc_file_system_fault_action_stop + 1;
    c_fault.system_code = 0;

    const std::int32_t past_final_symlink = bc_file_system_status_of(
        engine(), &location, bc_file_system_final_symlink_follow + 1, &present, &c_status, &c_error);
    const std::int32_t past_fault_action = bc_file_system_observed_fault_add(engine(), &c_fault, &c_error);

    EXPECT_EQ(past_final_symlink, bc_file_system_error_kind_invalid_argument);
    EXPECT_EQ(past_fault_action, bc_file_system_error_kind_invalid_argument);
}

TEST_F(CoreBindingTest, AModeWithBitsOutsidePermissionBitsIsInvalidArgument) {
    const std::string path = path_text_of("file");
    const bc_file_system_location location = at(path);
    bc_file_system_permissions c_permissions{};
    c_permissions.policy = bc_file_system_permissions_policy_exactly;
    c_permissions.mode = 0100644;
    bc_file_system_error c_error{};

    const std::int32_t set =
        bc_file_system_set_permissions(engine(), &location, 0100644, bc_file_system_final_symlink_refuse, &c_error);
    const std::int32_t created = bc_file_system_create_directory(engine(), &location, &c_permissions, &c_error);

    EXPECT_EQ(set, bc_file_system_error_kind_invalid_argument);
    EXPECT_EQ(created, bc_file_system_error_kind_invalid_argument);
}

TEST_F(CoreBindingTest, ANulByteInsideAPathIsInvalidArgument) {
    std::string path = path_text_of("fi");
    path.push_back('\0');
    path += "le";
    const bc_file_system_location location = at(path);
    std::int32_t existed = 0;
    bc_file_system_error c_error{};

    const std::int32_t status = bc_file_system_remove_file(engine(), &location, &existed, &c_error);

    EXPECT_EQ(status, bc_file_system_error_kind_invalid_argument);
}

// A NULL byte pointer with a length of 0 is the stated way to pass no bytes, not a contract violation: the
// write gets past its checks and creates the empty file.
TEST_F(CoreBindingTest, WriteOfNoBytesWithANullPointerIsNotAContractViolation) {
    const std::string path = path_text_of("empty");
    const bc_file_system_location location = at(path);
    const bc_file_system_write_options c_options = bc::file_system::to_c(kWrittenExactly0644);
    std::int32_t achieved = 0;
    bc_file_system_error c_error{};

    const std::int32_t status =
        bc_file_system_write_file(engine(), &location, nullptr, 0, &c_options, &achieved, &c_error);

    EXPECT_EQ(status, 0);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("empty")), "");
}

// Likewise a NULL buffer with a capacity of 0 asks for the size; a NULL buffer with a capacity is a violation.
TEST_F(CoreBindingTest, ANullBufferIsLegalOnlyWithZeroCapacity) {
    const std::string path = path_text_of("link");
    const bc_file_system_location location = at(path);
    std::size_t length = 0;
    bc_file_system_error c_error{};

    const std::int32_t asking_for_the_size =
        bc_file_system_read_symlink(engine(), &location, nullptr, 0, &length, &c_error);
    const std::int32_t lying_about_capacity =
        bc_file_system_read_symlink(engine(), &location, nullptr, 16, &length, &c_error);

    // Nothing is at `link`, so the size query reaches the core and answers its absence; the lie is refused first.
    EXPECT_EQ(asking_for_the_size, bc_file_system_error_kind_not_found);
    EXPECT_EQ(lying_about_capacity, bc_file_system_error_kind_invalid_argument);
}

TEST_F(CoreBindingTest, ReadFileIntoABufferTooSmallReportsTheLengthItNeeds) {
    bc::file_system_test::write_raw(path_of("file"), "eleven byte", 0644);
    const std::string path = path_text_of("file");
    const bc_file_system_location location = at(path);
    const bc_file_system_read_options c_options = bc::file_system::to_c(kWholeFileRefusingSymlink);
    bc_file_system_error c_error{};
    std::size_t length = 0;

    const std::int32_t with_no_buffer =
        bc_file_system_read_file(engine(), &location, &c_options, nullptr, 0, &length, &c_error);

    EXPECT_EQ(with_no_buffer, bc_file_system_error_kind_buffer_too_small);
    EXPECT_EQ(c_error.kind, bc_file_system_error_kind_buffer_too_small);
    EXPECT_EQ(length, 11U);

    // One byte short is still too small, and the buffer is left alone: nothing is cut short silently.
    std::vector<std::uint8_t> short_buffer(10, std::uint8_t{'?'});
    const std::int32_t one_short = bc_file_system_read_file(engine(), &location, &c_options, short_buffer.data(),
                                                            short_buffer.size(), &length, &c_error);

    EXPECT_EQ(one_short, bc_file_system_error_kind_buffer_too_small);
    EXPECT_EQ(length, 11U);
    EXPECT_EQ(std::string(short_buffer.begin(), short_buffer.end()), "??????????");
}

TEST_F(CoreBindingTest, ReadFileIntoABufferOfTheReportedLengthSucceeds) {
    bc::file_system_test::write_raw(path_of("file"), "eleven byte", 0644);
    const std::string path = path_text_of("file");
    const bc_file_system_location location = at(path);
    const bc_file_system_read_options c_options = bc::file_system::to_c(kWholeFileRefusingSymlink);
    bc_file_system_error c_error{};
    std::size_t length = 0;
    ASSERT_EQ(bc_file_system_read_file(engine(), &location, &c_options, nullptr, 0, &length, &c_error),
              bc_file_system_error_kind_buffer_too_small);

    std::vector<std::uint8_t> buffer(length);
    const std::int32_t status =
        bc_file_system_read_file(engine(), &location, &c_options, buffer.data(), buffer.size(), &length, &c_error);

    EXPECT_EQ(status, 0);
    EXPECT_TRUE(is_all_zero(c_error));
    EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(length)), "eleven byte");
}

TEST_F(CoreBindingTest, AFileThatGrewBetweenTheTwoCallsIsTooSmallAgain) {
    bc::file_system_test::write_raw(path_of("log"), "short", 0644);
    const std::string path = path_text_of("log");
    const bc_file_system_location location = at(path);
    const bc_file_system_read_options c_options = bc::file_system::to_c(kWholeFileRefusingSymlink);
    bc_file_system_error c_error{};
    std::size_t length = 0;
    ASSERT_EQ(bc_file_system_read_file(engine(), &location, &c_options, nullptr, 0, &length, &c_error),
              bc_file_system_error_kind_buffer_too_small);
    ASSERT_EQ(length, 5U);
    std::vector<std::uint8_t> buffer(length);

    bc::file_system_test::write_raw(path_of("log"), "short, then longer", 0644);
    const std::int32_t status =
        bc_file_system_read_file(engine(), &location, &c_options, buffer.data(), buffer.size(), &length, &c_error);

    EXPECT_EQ(status, bc_file_system_error_kind_buffer_too_small);
    EXPECT_EQ(length, 18U);
}

TEST_F(CoreBindingTest, ReadFileOfAnEmptyFileNeedsNoBuffer) {
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);
    const std::string path = path_text_of("empty");
    const bc_file_system_location location = at(path);
    const bc_file_system_read_options c_options = bc::file_system::to_c(kWholeFileRefusingSymlink);
    bc_file_system_error c_error = a_dirty_c_error();
    std::size_t length = 77;

    const std::int32_t status =
        bc_file_system_read_file(engine(), &location, &c_options, nullptr, 0, &length, &c_error);

    EXPECT_EQ(status, 0);
    EXPECT_EQ(length, 0U);
    EXPECT_TRUE(is_all_zero(c_error));
}

TEST_F(CoreBindingTest, ReadSymlinkFollowsTheSameProtocol) {
    bc::file_system_test::make_symlink_raw("target.txt", path_of("link"));
    const std::string path = path_text_of("link");
    const bc_file_system_location location = at(path);
    bc_file_system_error c_error{};
    std::size_t length = 0;
    std::vector<std::uint8_t> buffer(10);

    const std::int32_t too_small =
        bc_file_system_read_symlink(engine(), &location, buffer.data(), 3, &length, &c_error);
    EXPECT_EQ(too_small, bc_file_system_error_kind_buffer_too_small);
    EXPECT_EQ(length, 10U);

    const std::int32_t fits =
        bc_file_system_read_symlink(engine(), &location, buffer.data(), buffer.size(), &length, &c_error);
    EXPECT_EQ(fits, 0);
    EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(length)), "target.txt");
}

TEST_F(CoreBindingTest, ListDirectoryReportsTheCapacityItNeedsThenFillsTheArray) {
    bc::file_system_test::write_raw(path_of("a.txt"), "alpha", 0640);
    bc::file_system_test::make_directory_raw(path_of("sub"), 0755);
    bc::file_system_test::make_symlink_raw("a.txt", path_of("link"));
    const std::string path = root_text();
    const bc_file_system_location location = at(path);
    bc_file_system_error c_error{};
    std::size_t count = 0;

    const std::int32_t with_no_array = bc_file_system_list_directory(
        engine(), &location, bc_file_system_final_symlink_refuse, nullptr, 0, &count, &c_error);

    EXPECT_EQ(with_no_array, bc_file_system_error_kind_buffer_too_small);
    EXPECT_EQ(count, 3U);

    // One entry too few is still too small, in the same one pass: the two entries that fit are stored as they are
    // read (left in the array, not an answer) and the third is counted, and nothing is written past the capacity
    // offered.
    constexpr std::int32_t untouched = 99;
    std::vector<bc_file_system_entry> entries(
        count, bc_file_system_entry{.entry_type = untouched, .name_length = 0, .name = {}});
    const std::int32_t one_short = bc_file_system_list_directory(
        engine(), &location, bc_file_system_final_symlink_refuse, entries.data(), entries.size() - 1, &count, &c_error);
    EXPECT_EQ(one_short, bc_file_system_error_kind_buffer_too_small);
    EXPECT_EQ(count, 3U);
    EXPECT_NE(entries[0].entry_type, untouched);
    EXPECT_NE(entries[1].entry_type, untouched);
    EXPECT_EQ(entries[2].entry_type, untouched);

    const std::int32_t status = bc_file_system_list_directory(engine(), &location, bc_file_system_final_symlink_refuse,
                                                              entries.data(), entries.size(), &count, &c_error);

    ASSERT_EQ(status, 0);
    ASSERT_EQ(count, 3U);
    // Each of the three calls read the directory once: three entries, `.`, `..` and the end of the stream.
    std::uint64_t reads = 0;
    ASSERT_EQ(bc_file_system_observed_step_count(engine(), bc_file_system_operation_list_directory,
                                                 bc_file_system_step_readdir, &reads, &c_error),
              0);
    EXPECT_EQ(reads, 3U * 6U);
    std::vector<std::string> listed;
    for (const bc_file_system_entry& entry : entries) {
        listed.emplace_back(reinterpret_cast<const char*>(entry.name), entry.name_length);
    }
    EXPECT_THAT(listed, ElementsAre("a.txt", "link", "sub"));
    EXPECT_EQ(entries[0].entry_type, bc_file_system_entry_type_regular_file);
    EXPECT_EQ(entries[1].entry_type, bc_file_system_entry_type_symlink);
    EXPECT_EQ(entries[2].entry_type, bc_file_system_entry_type_directory);
}

// A directory that grows between the call that overflowed and the next answers the next call's own count, from a
// pass of its own: never the first count, and never a prefix of the listing. The grow loop's bound is the only
// retry.
TEST_F(CoreBindingTest, ListDirectoryOfADirectoryThatGrewBetweenTwoCallsAnswersTheSecondCallsOwnCount) {
    bc::file_system_test::write_raw(path_of("a.txt"), "alpha", 0640);
    bc::file_system_test::write_raw(path_of("b.txt"), "beta", 0640);
    bc::file_system_test::write_raw(path_of("c.txt"), "gamma", 0640);
    const std::string path = root_text();
    const bc_file_system_location location = at(path);
    bc_file_system_error c_error{};
    std::size_t count = 0;
    std::vector<bc_file_system_entry> entries(4);

    const std::int32_t first = bc_file_system_list_directory(engine(), &location, bc_file_system_final_symlink_refuse,
                                                             entries.data(), 1, &count, &c_error);
    ASSERT_EQ(first, bc_file_system_error_kind_buffer_too_small);
    ASSERT_EQ(count, 3U);
    bc::file_system_test::write_raw(path_of("d.txt"), "delta", 0640);
    const std::int32_t second = bc_file_system_list_directory(engine(), &location, bc_file_system_final_symlink_refuse,
                                                              entries.data(), 3, &count, &c_error);
    ASSERT_EQ(second, bc_file_system_error_kind_buffer_too_small);
    ASSERT_EQ(count, 4U);
    const std::int32_t third = bc_file_system_list_directory(engine(), &location, bc_file_system_final_symlink_refuse,
                                                             entries.data(), entries.size(), &count, &c_error);

    ASSERT_EQ(third, 0);
    ASSERT_EQ(count, 4U);
    std::vector<std::string> listed;
    listed.reserve(entries.size());
    for (const bc_file_system_entry& entry : entries) {
        listed.emplace_back(reinterpret_cast<const char*>(entry.name), entry.name_length);
    }
    EXPECT_THAT(listed, ElementsAre("a.txt", "b.txt", "c.txt", "d.txt"));
}

TEST_F(CoreBindingTest, ListDirectoryListsOneLevel) {
    bc::file_system_test::make_directory_raw(path_of("sub"), 0755);
    bc::file_system_test::write_raw(path_of("sub/b.txt"), "bee", 0600);
    const std::string path = root_text();
    const bc_file_system_location location = at(path);
    bc_file_system_error c_error{};
    std::size_t count = 0;
    std::vector<bc_file_system_entry> entries(8);

    const std::int32_t status = bc_file_system_list_directory(engine(), &location, bc_file_system_final_symlink_refuse,
                                                              entries.data(), entries.size(), &count, &c_error);

    ASSERT_EQ(status, 0);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(entries[0].name), entries[0].name_length), "sub");
}

TEST_F(CoreBindingTest, StatusOfAnswersPresentForSomethingAndAbsentForNothing) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0640);
    const std::string file = path_text_of("file");
    const std::string absent = path_text_of("absent");
    const bc_file_system_location file_location = at(file);
    const bc_file_system_location absent_location = at(absent);
    bc_file_system_error c_error{};
    bc_file_system_status c_status{};
    std::int32_t present = 0;

    EXPECT_EQ(bc_file_system_status_of(engine(), &file_location, bc_file_system_final_symlink_refuse, &present,
                                       &c_status, &c_error),
              0);
    EXPECT_EQ(present, 1);
    EXPECT_EQ(c_status.entry_type, bc_file_system_entry_type_regular_file);
    EXPECT_EQ(c_status.size, 7);
    EXPECT_EQ(c_status.mode, 0640U);

    EXPECT_EQ(bc_file_system_status_of(engine(), &absent_location, bc_file_system_final_symlink_refuse, &present,
                                       &c_status, &c_error),
              0);
    EXPECT_EQ(present, 0);
    EXPECT_TRUE(is_all_zero(c_error));
}

TEST_F(CoreBindingTest, WriteAnswersWithTheDurabilityAchieved) {
    const std::string plain = path_text_of("plain");
    const std::string synced = path_text_of("synced");
    const bc_file_system_location plain_location = at(plain);
    const bc_file_system_location synced_location = at(synced);
    const auto bytes = bytes_of("content");
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const bc_file_system_write_options written = bc::file_system::to_c(kWrittenExactly0644);
    const bc_file_system_write_options durable = bc::file_system::to_c(kDurableExactly0600);
    bc_file_system_error c_error{};
    std::int32_t achieved_written = 0;
    std::int32_t achieved_durable = 0;

    EXPECT_EQ(
        bc_file_system_write_file(engine(), &plain_location, data, bytes.size(), &written, &achieved_written, &c_error),
        0);
    EXPECT_EQ(bc_file_system_write_file(engine(), &synced_location, data, bytes.size(), &durable, &achieved_durable,
                                        &c_error),
              0);

    EXPECT_EQ(achieved_written, bc_file_system_durability_written);
    EXPECT_EQ(achieved_durable, bc_file_system_durability_durable);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("plain")), "content");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("plain")), 0644U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("synced")), 0600U);
}

TEST_F(CoreBindingTest, WriteOfNoBytesTakesANullPointer) {
    const std::string path = path_text_of("empty");
    const bc_file_system_location location = at(path);
    const bc_file_system_write_options c_options = bc::file_system::to_c(kWrittenExactly0644);
    bc_file_system_error c_error{};
    std::int32_t achieved = 0;

    const std::int32_t status =
        bc_file_system_write_file(engine(), &location, nullptr, 0, &c_options, &achieved, &c_error);

    EXPECT_EQ(status, 0);
    EXPECT_EQ(achieved, bc_file_system_durability_written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("empty")), "");
}

TEST_F(CoreBindingTest, WriteUnderPreserveOrKeepsTheReplacedFilesMode) {
    bc::file_system_test::write_raw(path_of("config"), "old", 0640);
    const std::string path = path_text_of("config");
    const bc_file_system_location location = at(path);
    const auto bytes = bytes_of("new");
    const bc_file_system_write_options c_options = bc::file_system::to_c(kWrittenPreserveOr0600);
    bc_file_system_error c_error{};
    std::int32_t achieved = 0;

    const std::int32_t status =
        bc_file_system_write_file(engine(), &location, reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                  bytes.size(), &c_options, &achieved, &c_error);

    EXPECT_EQ(status, 0);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("config")), "new");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0640U);
}

TEST_F(CoreBindingTest, AppendAddsToTheEnd) {
    bc::file_system_test::write_raw(path_of("log"), "one\n", 0644);
    const std::string path = path_text_of("log");
    const bc_file_system_location location = at(path);
    const auto bytes = bytes_of("two\n");
    const bc_file_system_permissions c_permissions = bc::file_system::to_c(Permissions::exactly(0644));
    bc_file_system_error c_error{};
    std::int32_t achieved = 0;

    const std::int32_t status =
        bc_file_system_append(engine(), &location, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
                              bc_file_system_durability_written, &c_permissions, &achieved, &c_error);

    EXPECT_EQ(status, 0);
    EXPECT_EQ(achieved, bc_file_system_durability_written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "one\ntwo\n");
}

TEST_F(CoreBindingTest, CreateDirectoriesRenameCopyAndTheTwoRemovals) {
    const std::string tree = path_text_of("a/b");
    const std::string source = path_text_of("a/b/source");
    const std::string copy = path_text_of("a/b/copy");
    const std::string renamed = path_text_of("a/b/renamed");
    const std::string top = path_text_of("a");
    const bc_file_system_location tree_location = at(tree);
    const bc_file_system_location source_location = at(source);
    const bc_file_system_location copy_location = at(copy);
    const bc_file_system_location renamed_location = at(renamed);
    const bc_file_system_location top_location = at(top);
    const bc_file_system_permissions c_permissions = bc::file_system::to_c(Permissions::exactly(0750));
    const bc_file_system_write_options c_options = bc::file_system::to_c(kWrittenExactly0600);
    bc_file_system_error c_error{};
    std::int32_t achieved = 0;
    std::int32_t existed = 0;

    ASSERT_EQ(bc_file_system_create_directories(engine(), &tree_location, &c_permissions, &c_error), 0);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b")), 0750U);

    bc::file_system_test::write_raw(path_of("a/b/source"), "content", 0644);
    ASSERT_EQ(bc_file_system_copy_file(engine(), &source_location, &copy_location, &c_options, &achieved, &c_error), 0);
    EXPECT_EQ(achieved, bc_file_system_durability_written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("a/b/copy")), "content");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b/copy")), 0600U);

    ASSERT_EQ(bc_file_system_rename(engine(), &copy_location, &renamed_location,
                                    bc_file_system_existing_destination_refuse, bc_file_system_durability_durable,
                                    &achieved, &c_error),
              0);
    EXPECT_EQ(achieved, bc_file_system_durability_durable);
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("a/b")), ElementsAre("renamed", "source"));

    EXPECT_EQ(bc_file_system_remove_file(engine(), &renamed_location, &existed, &c_error), 0);
    EXPECT_EQ(existed, 1);
    EXPECT_EQ(bc_file_system_remove_file(engine(), &renamed_location, &existed, &c_error), 0);
    EXPECT_EQ(existed, 0);
    EXPECT_TRUE(is_all_zero(c_error));

    EXPECT_EQ(bc_file_system_remove_tree(engine(), &top_location, &existed, &c_error), 0);
    EXPECT_EQ(existed, 1);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("a")));
    EXPECT_EQ(bc_file_system_remove_tree(engine(), &top_location, &existed, &c_error), 0);
    EXPECT_EQ(existed, 0);
}

} // namespace
