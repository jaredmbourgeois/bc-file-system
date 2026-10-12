#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <bc_file_system/engine.hpp>
#include <bc_file_system/error.hpp>
#include <bc_file_system/file_system.hpp>
#include <bc_file_system/location.hpp>
#include <bc_file_system/options.hpp>
#include <bc_file_system/required.hpp>
#include <bc_file_system/result.hpp>
#include <bc_file_system/status.hpp>

#include "descriptor_balance.hpp"
#include "temp_directory.hpp"
#include "test_paths.hpp"

// The contract every engine keeps, driving C through the C++ face, run against each engine:
//
//   - the live engine, in a directory the test created;
//   - the same engine with the observer installed, which must change nothing it was not scripted to change.
//
// A failure that cannot be provoked for real (the disk filling up, a sync failing after the rename) is put into
// the middle of an operation with a fault scripted into the observed engine; those cases run on the observed
// engine alone (`ObservedContract`), since a live engine takes no faults. Where a real provocation exists as
// well, the suite carries both.
//
// Every case below that needs the engine is disabled, with the stage that turns it on in the comment above
// it; the core is an honest stub until then. A case the stub's own argument check already answers is green.

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
using bc::file_system::Durability;
using bc::file_system::Engine;
using bc::file_system::Entry;
using bc::file_system::EntryType;
using bc::file_system::ErrorKind;
using bc::file_system::ExistingDestination;
using bc::file_system::Fault;
using bc::file_system::FaultAction;
using bc::file_system::FileSystem;
using bc::file_system::FinalSymlink;
using bc::file_system::Location;
using bc::file_system::Operation;
using bc::file_system::Permissions;
using bc::file_system::ReadLimit;
using bc::file_system::ReadLimitKind;
using bc::file_system::ReadOptions;
using bc::file_system::RelativePath;
using bc::file_system::Required;
using bc::file_system::Result;
using bc::file_system::Status;
using bc::file_system::Step;
using bc::file_system::WriteOptions;
using bc::file_system_test::bytes_of;
using bc::file_system_test::Identity;
using bc::file_system_test::temporary_root;
using bc::file_system_test::TemporaryDirectory;
using bc::file_system_test::text_of;
using testing::ElementsAre;
using testing::IsEmpty;

// -------------------------------------------------------------------------------------------------------------
// The options every case states, built once here so a field added later fails in one place
// -------------------------------------------------------------------------------------------------------------

constexpr ReadOptions kWholeFileRefusingSymlink{.limit = ReadLimit::whole_file(),
                                                .final_symlink = FinalSymlink::refuse};
constexpr ReadOptions kWholeFileFollowingSymlink{.limit = ReadLimit::whole_file(),
                                                 .final_symlink = FinalSymlink::follow};

// A bounded read; the bound is the one value a case states at its call site.
constexpr auto reading_at_most(std::uint64_t bytes) noexcept -> ReadOptions {
    return ReadOptions{.limit = ReadLimit::at_most(bytes), .final_symlink = FinalSymlink::refuse};
}

constexpr WriteOptions kWrittenExactly0644{.durability = Durability::written,
                                           .permissions = Permissions::exactly(0644),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenExactly0640{.durability = Durability::written,
                                           .permissions = Permissions::exactly(0640),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenExactly0600{.durability = Durability::written,
                                           .permissions = Permissions::exactly(0600),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenPreserveOr0600{.durability = Durability::written,
                                              .permissions = Permissions::preserve_or(0600),
                                              .existing_destination = ExistingDestination::replace,
                                              .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kDurableExactly0644{.durability = Durability::durable,
                                           .permissions = Permissions::exactly(0644),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kDurablePreserveOr0644{.durability = Durability::durable,
                                              .permissions = Permissions::preserve_or(0644),
                                              .existing_destination = ExistingDestination::replace,
                                              .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenExactly0644Refusing{.durability = Durability::written,
                                                   .permissions = Permissions::exactly(0644),
                                                   .existing_destination = ExistingDestination::refuse,
                                                   .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kWrittenExactly0644Following{.durability = Durability::written,
                                                    .permissions = Permissions::exactly(0644),
                                                    .existing_destination = ExistingDestination::replace,
                                                    .final_symlink = FinalSymlink::follow};
// A mode with the file-type bits in it, which is not a mode.
constexpr WriteOptions kWrittenNotAMode{.durability = Durability::written,
                                        .permissions = Permissions::exactly(0100644),
                                        .existing_destination = ExistingDestination::replace,
                                        .final_symlink = FinalSymlink::refuse};

// The system call a durable sync is made with on the platform the suite runs on, and the one that must never
// be made. Stated here on their own, so that the suite, not the implementation, says what each platform owes.
#if defined(__APPLE__)
constexpr auto durable_sync_step() noexcept -> Step {
    return Step::fcntl_fullfsync;
}
constexpr auto wrong_sync_step() noexcept -> Step {
    return Step::fsync;
}
#else
constexpr auto durable_sync_step() noexcept -> Step {
    return Step::fsync;
}
constexpr auto wrong_sync_step() noexcept -> Step {
    return Step::fcntl_fullfsync;
}
#endif

// The `occurrence`-th time `operation` reaches `step`, the step fails with `system_code` as the system would.
auto fail_step(Operation operation, Step step, std::uint32_t occurrence, int system_code) -> Fault {
    return Fault{.operation = operation,
                 .step = step,
                 .occurrence = occurrence,
                 .action = FaultAction::fail,
                 .system_code = system_code};
}

// The `occurrence`-th time `operation` reaches `step`, the operation returns from there as if killed.
auto stop_at(Operation operation, Step step, std::uint32_t occurrence) -> Fault {
    return Fault{
        .operation = operation, .step = step, .occurrence = occurrence, .action = FaultAction::stop, .system_code = 0};
}

// -------------------------------------------------------------------------------------------------------------
// The fixtures
// -------------------------------------------------------------------------------------------------------------

enum class EngineKind : std::uint8_t {
    live,
    observed,
};

auto make_engine(EngineKind kind) -> Result<Engine> {
    if (kind == EngineKind::observed) {
        return Engine::observed_for_tests();
    }
    return Engine::live();
}

auto engine_name(const testing::TestParamInfo<EngineKind>& info) -> std::string {
    return info.param == EngineKind::live ? "Live" : "Observed";
}

// What every contract fixture shares: the directory this test owns, created under the root the build handed
// the test binary, the engine, and the file system over it.
template <class GTestBase>
class Contract : public GTestBase {
protected:
    explicit Contract(Result<Engine> engine) : engine_(std::move(engine)) {}

    auto SetUp() -> void override {
        ASSERT_TRUE(directory_.has_value());
        ASSERT_TRUE(engine_.has_value()) << engine_.error().message();
        files_.emplace(engine_.value());
    }

    [[nodiscard]] auto files() -> FileSystem& { return *files_; }
    [[nodiscard]] auto engine() -> Engine& { return engine_.value(); }

    // A location inside the test's own directory; `path_of` is the same place as the raw helpers take it.
    [[nodiscard]] auto in(std::string_view relative) const -> Location { return Location::at(path_of(relative)); }
    [[nodiscard]] auto path_of(std::string_view relative) const -> std::filesystem::path {
        return directory_->path() / relative;
    }
    [[nodiscard]] auto root() const -> Location { return Location::at(directory_->path()); }
    [[nodiscard]] auto root_path() const -> const std::filesystem::path& { return directory_->path(); }

private:
    std::optional<TemporaryDirectory> directory_ = TemporaryDirectory::create_under(temporary_root());
    Result<Engine> engine_;
    std::optional<FileSystem> files_;
};

// The cases that hold on both engines.
class FileSystemContract : public Contract<bc::file_system_test::DescriptorBalancedWithParam<EngineKind>> {
protected:
    FileSystemContract() : Contract(make_engine(GetParam())) {}
};

INSTANTIATE_TEST_SUITE_P(Engines, FileSystemContract, testing::Values(EngineKind::live, EngineKind::observed),
                         engine_name);

// The cases that script a fault or read a step count, which only the observed engine offers.
class ObservedContract : public Contract<bc::file_system_test::DescriptorBalanced> {
protected:
    ObservedContract() : Contract(Engine::observed_for_tests()) {}

    // Scripts a fault, failing the case if the engine refuses it.
    auto script(const Fault& fault) -> void {
        const Result<void> added = engine().add_fault(fault);
        ASSERT_TRUE(added.has_value()) << added.error().message();
    }

    // How many times `operation` reached `step`; a refusal is a test failure and counts as nothing.
    [[nodiscard]] auto count(Operation operation, Step step) -> std::uint64_t {
        const Result<std::uint64_t> counted = engine().step_count(operation, step);
        if (!counted.has_value()) {
            ADD_FAILURE() << counted.error().message();
            return 0;
        }
        return *counted;
    }
};

// -------------------------------------------------------------------------------------------------------------
// read_file
// -------------------------------------------------------------------------------------------------------------

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileReturnsEveryByte) {
    bc::file_system_test::write_raw(path_of("notes.txt"), "first line\nsecond line\n", 0644);

    const auto bytes = files().read_file(in("notes.txt"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(text_of(*bytes), "first line\nsecond line\n");
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileOfAnEmptyFileIsEmpty) {
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);

    const auto bytes = files().read_file(in("empty"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value());
    EXPECT_THAT(*bytes, IsEmpty());
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileReturnsAFileLargerThanAnyInternalBuffer) {
    const std::string large(300 * 1024, 'x');
    bc::file_system_test::write_raw(path_of("large.bin"), large, 0644);

    const auto bytes = files().read_file(in("large.bin"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(text_of(*bytes), large);
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileOfNothingIsNotFound) {
    const auto bytes = files().read_file(in("absent.txt"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_found);
    EXPECT_EQ(bytes.error().system_code, ENOENT);
    EXPECT_EQ(bytes.error().path, path_of("absent.txt").string());
    EXPECT_TRUE(bytes.error().step.has_value());
    EXPECT_FALSE(bytes.error().message().empty());
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileIfPresentOfNothingIsAnEmptyValue) {
    bc::file_system_test::write_raw(path_of("present.txt"), "here", 0644);

    const auto absent = files().read_file_if_present(in("absent.txt"), kWholeFileRefusingSymlink);
    const auto present = files().read_file_if_present(in("present.txt"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(absent.has_value());
    EXPECT_FALSE(absent->has_value());
    ASSERT_TRUE(present.has_value());
    ASSERT_TRUE(present->has_value());
    EXPECT_EQ(text_of(**present), "here");
}

// Under `refuse` a final symlink is refused as what it is; under `follow` the read goes through it.
// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileRefusesASymlinkEvenToARegularFileUnlessToldToFollow) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto refused = files().read_file(in("link"), kWholeFileRefusingSymlink);
    const auto followed = files().read_file(in("link"), kWholeFileFollowingSymlink);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::symlink_refused);
    ASSERT_TRUE(followed.has_value());
    EXPECT_EQ(text_of(*followed), "behind the link");
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileRefusesADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto bytes = files().read_file(in("folder"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_regular_file);
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileBelowAFileIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "not a directory", 0644);

    const auto bytes = files().read_file(in("file/child"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_directory);
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileWithoutPermissionIsPermissionDenied) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process reads what its permission bits forbid";
    }
    bc::file_system_test::write_raw(path_of("secret"), "no access", 0000);

    const auto bytes = files().read_file(in("secret"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(bytes.error().system_code, EACCES);
}

// -------------------------------------------------------------------------------------------------------------
// read_file under a limit (the draft's read_range cases: a bound is never a silent prefix)
// -------------------------------------------------------------------------------------------------------------

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileUnderAtMostReturnsAFileWithinTheLimit) {
    bc::file_system_test::write_raw(path_of("alphabet"), "abcdefghij", 0644);

    const auto exactly_the_size = files().read_file(in("alphabet"), reading_at_most(10));
    const auto under_the_size = files().read_file(in("alphabet"), reading_at_most(100));

    ASSERT_TRUE(exactly_the_size.has_value());
    EXPECT_EQ(text_of(*exactly_the_size), "abcdefghij");
    ASSERT_TRUE(under_the_size.has_value());
    EXPECT_EQ(text_of(*under_the_size), "abcdefghij");
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileUnderAtMostOfALargerFileIsTooLargeNeverAPrefix) {
    bc::file_system_test::write_raw(path_of("alphabet"), "abcdefghij", 0644);

    const auto bytes = files().read_file(in("alphabet"), reading_at_most(4));

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::too_large);
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileUnderAtMostOfAnEmptyFileIsEmpty) {
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);

    const auto under_a_bound = files().read_file(in("empty"), reading_at_most(5));
    const auto under_zero = files().read_file(in("empty"), reading_at_most(0));

    ASSERT_TRUE(under_a_bound.has_value());
    EXPECT_THAT(*under_a_bound, IsEmpty());
    ASSERT_TRUE(under_zero.has_value());
    EXPECT_THAT(*under_zero, IsEmpty());
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileUnderAtMostOfNothingIsNotFound) {
    const auto bytes = files().read_file(in("absent"), reading_at_most(16));

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_found);
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_ReadFileUnderAtMostRefusesASymlinkAndADirectory) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto through_link = files().read_file(in("link"), reading_at_most(4));
    const auto of_directory = files().read_file(in("folder"), reading_at_most(4));

    ASSERT_FALSE(through_link.has_value());
    EXPECT_EQ(through_link.error().kind, ErrorKind::symlink_refused);
    ASSERT_FALSE(of_directory.has_value());
    EXPECT_EQ(of_directory.error().kind, ErrorKind::not_regular_file);
}

// A ReadLimit holds the bound it was given, and an options struct cannot be built with a field left out. The
// second is a property of the types, so it is proven where the compiler proves it: these checks fail the
// build, on every compiler, if a field becomes omittable.
template <class Options>
concept BuildableWithoutFinalSymlink = requires { Options{.limit = ReadLimit::whole_file()}; };
template <class Options>
concept BuildableWithoutLimit = requires { Options{.final_symlink = FinalSymlink::refuse}; };
template <class Options>
concept BuildableFromNothing = requires { Options{}; };

static_assert(!BuildableWithoutFinalSymlink<ReadOptions>);
static_assert(!BuildableWithoutLimit<ReadOptions>);
static_assert(!BuildableFromNothing<ReadOptions>);
static_assert(!BuildableFromNothing<WriteOptions>);
static_assert(!std::is_default_constructible_v<Required<Durability>>);

TEST(ReadLimitTest, HoldsTheBoundItWasGiven) {
    const ReadOptions options = reading_at_most(512);

    EXPECT_EQ(options.limit.value.kind(), ReadLimitKind::at_most);
    EXPECT_EQ(options.limit.value.bound(), 512U);
    EXPECT_EQ(kWholeFileRefusingSymlink.limit.value.kind(), ReadLimitKind::whole_file);
    EXPECT_EQ(kWholeFileRefusingSymlink.final_symlink.value, FinalSymlink::refuse);
}

// -------------------------------------------------------------------------------------------------------------
// write_file
// -------------------------------------------------------------------------------------------------------------

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteCreatesTheFileWithItsBytesAndItsMode) {
    const auto bytes = bytes_of("fresh content");

    const auto achieved = files().write_file(in("new.txt"), bytes, kWrittenExactly0640);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("new.txt")), "fresh content");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("new.txt")), 0640U);
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteReplacesTheContentAndLeavesNoTemporaryFile) {
    bc::file_system_test::write_raw(path_of("config"), "old content, and longer than the new", 0644);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("config")), "new");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("config"));
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteOfNoBytesCreatesAnEmptyFile) {
    const auto achieved = files().write_file(in("empty"), {}, kWrittenExactly0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("empty")), "");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("empty")), 0600U);
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteUnderExactlyGivesAReplacedFileTheStatedMode) {
    bc::file_system_test::write_raw(path_of("config"), "old", 0600);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0644U);
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteUnderPreserveOrKeepsTheReplacedFilesMode) {
    bc::file_system_test::write_raw(path_of("config"), "old", 0640);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenPreserveOr0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("config")), "new");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0640U);
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteUnderPreserveOrGivesANewFileTheStatedMode) {
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenPreserveOr0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0600U);
}

// Under `refuse` an existing destination is left exactly as it was.
// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteUnderRefuseLeavesAnExistingFileAndIsAlreadyExists) {
    bc::file_system_test::write_raw(path_of("config"), "old", 0600);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenExactly0644Refusing);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::already_exists);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("config")), "old");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0600U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("config"));
}

// A mode with bits outside 07777 is a contract violation, refused before any system call: green now.
TEST_P(FileSystemContract, WriteRefusesAModeThatIsNotAMode) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("file"), bytes, kWrittenNotAMode);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(achieved.error().operation, Operation::write_file);
    EXPECT_FALSE(achieved.error().step.has_value());
    EXPECT_EQ(achieved.error().system_code, 0);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("file")));
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteIntoADirectoryThatIsNotThereIsNotFound) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("missing/file"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
}

// Under `refuse` a symlink at the destination is refused and nothing moves; under `follow` the file behind
// it is the one replaced, and the link stays.
// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteRefusesADestinationThatIsASymlinkUnlessToldToFollow) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));
    const auto bytes = bytes_of("replacement");

    const auto refused = files().write_file(in("link"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::symlink_refused);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("target.txt")), "behind the link");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("link", "target.txt"));

    const auto followed = files().write_file(in("link"), bytes, kWrittenExactly0644Following);

    ASSERT_TRUE(followed.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("target.txt")), "replacement");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("link", "target.txt"));
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteRefusesADestinationThatIsADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    const auto bytes = bytes_of("replacement");

    const auto achieved = files().write_file(in("folder"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_regular_file);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("folder"));
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteIntoADirectoryWithoutPermissionIsPermissionDenied) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process writes where its permission bits forbid";
    }
    bc::file_system_test::make_directory_raw(path_of("read-only"), 0500);
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("read-only/file"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::permission_denied);
}

// No space on write, injected: the write(2) fails with ENOSPC after the temporary file exists.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_WriteThatRunsOutOfSpaceLeavesTheDestinationWholeAndNoTemporaryFile) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(fail_step(Operation::write_file, Step::write, 1, ENOSPC));

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::no_space);
    EXPECT_EQ(achieved.error().system_code, ENOSPC);
    EXPECT_EQ(achieved.error().step, Step::write);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
}

// Too large, for real: the system refuses to let a file grow past the limit. The first write(2) is cut short
// at the limit and the next one fails with EFBIG, so this is also the case of a short write that must not be
// taken for success. EFBIG is `too_large`, not `no_space`: the file-size limit is a bound, not a full disk.
// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_WriteAgainstAFileSizeLimitIsTooLargeAndLeavesTheDestinationWhole) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const std::string too_large(64 * 1024, 'x');
    const auto bytes = bytes_of(too_large);

    const auto achieved = [&] {
        const bc::file_system_test::FileSizeLimit limit{4096};
        return files().write_file(in("ledger"), bytes, kWrittenExactly0644);
    }();

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::too_large);
    EXPECT_EQ(achieved.error().system_code, EFBIG);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_WriteWhoseRenameFailsLeavesTheDestinationWholeAndNoTemporaryFile) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(fail_step(Operation::write_file, Step::renameat, 1, EIO));

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(achieved.error().system_code, EIO);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
    // The temporary file was written and synced before the rename was tried: the failure came after it.
    EXPECT_EQ(count(Operation::write_file, Step::write), 1U);
    EXPECT_EQ(count(Operation::write_file, durable_sync_step()), 1U);
}

// The temporary file is the first `openat` of a write: the one that creates it O_EXCL beside the destination.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_WriteWhoseTemporaryFileCannotBeCreatedReportsWhy) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, Step::openat, 1, EROFS));

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(achieved.error().system_code, EROFS);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_WriteWhoseModeCannotBeSetLeavesTheDestinationWholeAndNoTemporaryFile) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(fail_step(Operation::write_file, Step::fchmod, 1, EPERM));

    const auto achieved = files().write_file(in("ledger"), bytes, kWrittenExactly0600);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("ledger")), 0644U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
}

// A sync that fails before the rename is still an error: nothing has replaced the destination yet.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_DurableWriteWhoseFileSyncFailsLeavesTheDestinationWhole) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(fail_step(Operation::write_file, durable_sync_step(), 1, EIO));

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 0U);
}

// The directory sync is the second sync of a durable write, and it comes after the rename. The destination is
// already replaced, so the call may not report an error: it reports the durability it did achieve.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_DurableWriteWhoseDirectorySyncFailsReportsWritten) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(fail_step(Operation::write_file, durable_sync_step(), 2, EIO));

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the new ledger");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
}

// The same outcome, provoked for real: files can be created in a directory with mode 0300, but the directory
// cannot be opened for the sync.
// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_DurableWriteIntoADirectoryThatCannotBeOpenedForItsSyncReportsWritten) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process opens what its permission bits forbid";
    }
    bc::file_system_test::make_directory_raw(path_of("write-only"), 0300);
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("write-only/file"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    bc::file_system_test::set_mode_raw(path_of("write-only"), 0700);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("write-only/file")), "content");
}

// What "durable" means, read from the system calls the write made: the temporary file is created, written,
// given its mode and synced; it is renamed over the destination; the directory is synced. On Apple platforms
// both syncs are F_FULLFSYNC, and fsync is never called, because fsync there stops at the drive's cache. That
// the file's sync comes before the rename is the case above, which fails it and finds no rename.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_DurableWriteSyncsTheFileThenRenamesThenSyncsTheDirectory) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_GE(count(Operation::write_file, Step::openat), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::write), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::fchmod), 1U);
    EXPECT_EQ(count(Operation::write_file, durable_sync_step()), 2U);
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 1U);
    EXPECT_EQ(count(Operation::write_file, wrong_sync_step()), 0U);
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_WrittenWriteMakesNoSyncCall) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("ledger"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(count(Operation::write_file, Step::fsync), 0U);
    EXPECT_EQ(count(Operation::write_file, Step::fcntl_fullfsync), 0U);
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_AnInterruptedWriteIsAskedAgain) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, Step::write, 1, EINTR));

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_EQ(count(Operation::write_file, Step::write), 2U);
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_AnInterruptedSyncIsAskedAgain) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, durable_sync_step(), 1, EINTR));

    const auto achieved = files().write_file(in("file"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    // The interrupted file sync, the file sync again, and the directory sync.
    EXPECT_EQ(count(Operation::write_file, durable_sync_step()), 3U);
}

// A file system with no sync call at all (some network mounts) cannot do better than written, and says so.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_DurableWriteWhereSyncIsNotSupportedReportsWritten) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, durable_sync_step(), 1, ENOTSUP));

    const auto achieved = files().write_file(in("file"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
}

// EDQUOT is a full quota, `no_space`; EFBIG is the process's file-size limit, `too_large`.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_EachRoomFailureHasItsKind) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, Step::write, 1, EDQUOT));
    script(fail_step(Operation::write_file, Step::write, 2, EFBIG));

    const auto over_quota = files().write_file(in("one"), bytes, kWrittenExactly0644);
    const auto over_size_limit = files().write_file(in("two"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(over_quota.has_value());
    EXPECT_EQ(over_quota.error().kind, ErrorKind::no_space);
    ASSERT_FALSE(over_size_limit.has_value());
    EXPECT_EQ(over_size_limit.error().kind, ErrorKind::too_large);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

// -------------------------------------------------------------------------------------------------------------
// append
// -------------------------------------------------------------------------------------------------------------

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_AppendCreatesTheFileWithItsMode) {
    const auto bytes = bytes_of("first entry\n");

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0640));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\n");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("log")), 0640U);
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_AppendAddsToTheEnd) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("second entry\n");

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\nsecond entry\n");
}

// An existing file is neither created nor replaced by an append, so its mode is not this call's to change.
// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_AppendLeavesAnExistingFilesModeAloneUnderEitherPolicy) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0640);
    const auto bytes = bytes_of("more\n");

    const auto under_exactly = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0600));
    const auto under_preserve = files().append(in("log"), bytes, Durability::written, Permissions::preserve_or(0600));

    ASSERT_TRUE(under_exactly.has_value());
    ASSERT_TRUE(under_preserve.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("log")), 0640U);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\nmore\nmore\n");
}

// An append never follows a symlink (it takes no FinalSymlink): a link at the location is refused as one.
// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_AppendRefusesASymlinkAndADirectory) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    const auto bytes = bytes_of("more");

    const auto through_link = files().append(in("link"), bytes, Durability::written, Permissions::exactly(0644));
    const auto onto_directory = files().append(in("folder"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(through_link.has_value());
    EXPECT_EQ(through_link.error().kind, ErrorKind::symlink_refused);
    ASSERT_FALSE(onto_directory.has_value());
    EXPECT_EQ(onto_directory.error().kind, ErrorKind::not_regular_file);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("target.txt")), "behind the link");
}

// stage 14 writes-in-c
TEST_P(FileSystemContract, DISABLED_AppendIntoADirectoryThatIsNotThereIsNotFound) {
    const auto bytes = bytes_of("entry");

    const auto achieved = files().append(in("missing/log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_AppendThatRunsOutOfSpaceIsNoSpace) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("second entry\n");
    script(fail_step(Operation::append, Step::write, 1, ENOSPC));

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::no_space);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\n");
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_AppendWhoseNewFileCannotGetItsModeTakesTheFileBack) {
    const auto bytes = bytes_of("entry");
    script(fail_step(Operation::append, Step::fchmod, 1, EPERM));

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::permission_denied);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("log")));
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_DurableAppendToANewFileSyncsTheFileAndItsDirectory) {
    const auto bytes = bytes_of("entry");

    const auto achieved = files().append(in("log"), bytes, Durability::durable, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::append, durable_sync_step()), 2U);
    EXPECT_EQ(count(Operation::append, wrong_sync_step()), 0U);
}

// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_DurableAppendToAnExistingFileSyncsOnlyTheFile) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("entry");

    const auto achieved = files().append(in("log"), bytes, Durability::durable, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::append, durable_sync_step()), 1U);
}

// The bytes are already in the file when the sync fails, so the call reports what it achieved, not an error.
// stage 14 writes-in-c
TEST_F(ObservedContract, DISABLED_DurableAppendWhoseSyncFailsReportsWritten) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("second entry\n");
    script(fail_step(Operation::append, durable_sync_step(), 1, EIO));

    const auto achieved = files().append(in("log"), bytes, Durability::durable, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\nsecond entry\n");
}

// -------------------------------------------------------------------------------------------------------------
// list_directory
// -------------------------------------------------------------------------------------------------------------

// Lays out:  a.txt (5 bytes) · link -> a.txt · sub/ · sub/b.txt (3 bytes) · sub/deeper/ · sub/deeper/c.txt
auto lay_out_a_small_tree(const std::filesystem::path& root) -> void {
    bc::file_system_test::write_raw(root / "a.txt", "alpha", 0644);
    bc::file_system_test::make_symlink_raw(root / "a.txt", root / "link");
    bc::file_system_test::make_directory_raw(root / "sub", 0755);
    bc::file_system_test::write_raw(root / "sub" / "b.txt", "bee", 0600);
    bc::file_system_test::make_directory_raw(root / "sub" / "deeper", 0700);
    bc::file_system_test::write_raw(root / "sub" / "deeper" / "c.txt", "", 0644);
}

auto names_of(const std::vector<Entry>& entries) -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const Entry& entry : entries) {
        names.push_back(entry.name);
    }
    return names;
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListThisDirectoryReturnsItsEntriesSortedEachWithItsType) {
    lay_out_a_small_tree(root_path());

    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    EXPECT_THAT(*entries, ElementsAre(Entry{.entry_type = EntryType::regular_file, .name = "a.txt"},
                                      Entry{.entry_type = EntryType::symlink, .name = "link"},
                                      Entry{.entry_type = EntryType::directory, .name = "sub"}));
}

// A listing is one level deep; a tree is read by listing each directory in turn.
// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListOfEachDirectoryInATreeReturnsItsOwnEntries) {
    lay_out_a_small_tree(root_path());

    const auto top = files().list_directory(root(), FinalSymlink::refuse);
    const auto sub = files().list_directory(in("sub"), FinalSymlink::refuse);
    const auto deeper = files().list_directory(in("sub/deeper"), FinalSymlink::refuse);

    ASSERT_TRUE(top.has_value());
    EXPECT_THAT(names_of(*top), ElementsAre("a.txt", "link", "sub"));
    ASSERT_TRUE(sub.has_value());
    EXPECT_THAT(names_of(*sub), ElementsAre("b.txt", "deeper"));
    ASSERT_TRUE(deeper.has_value());
    EXPECT_THAT(names_of(*deeper), ElementsAre("c.txt"));
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListListsASymlinkToADirectoryAsASymlinkAndNeverFollowsIt) {
    bc::file_system_test::make_directory_raw(path_of("outside"), 0755);
    bc::file_system_test::write_raw(path_of("outside/private.txt"), "not part of the tree", 0644);
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::make_symlink_raw(path_of("outside"), path_of("tree/way-out"));

    const auto entries = files().list_directory(in("tree"), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    EXPECT_THAT(*entries, ElementsAre(Entry{.entry_type = EntryType::symlink, .name = "way-out"}));
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListFollowsASymlinkGivenAsTheDirectoryItselfOnlyWhenToldTo) {
    bc::file_system_test::make_directory_raw(path_of("real"), 0755);
    bc::file_system_test::write_raw(path_of("real/inside.txt"), "content", 0644);
    bc::file_system_test::make_symlink_raw(path_of("real"), path_of("alias"));

    const auto followed = files().list_directory(in("alias"), FinalSymlink::follow);
    const auto refused = files().list_directory(in("alias"), FinalSymlink::refuse);

    ASSERT_TRUE(followed.has_value());
    EXPECT_THAT(names_of(*followed), ElementsAre("inside.txt"));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::symlink_refused);
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListOfAnEmptyDirectoryIsEmpty) {
    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    EXPECT_THAT(*entries, IsEmpty());
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListOfNothingIsNotFound) {
    const auto entries = files().list_directory(in("absent"), FinalSymlink::refuse);

    ASSERT_FALSE(entries.has_value());
    EXPECT_EQ(entries.error().kind, ErrorKind::not_found);
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListOfAFileIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto entries = files().list_directory(in("file"), FinalSymlink::refuse);

    ASSERT_FALSE(entries.has_value());
    EXPECT_EQ(entries.error().kind, ErrorKind::not_directory);
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListOfADirectoryWithoutPermissionIsPermissionDenied) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process lists what its permission bits forbid";
    }
    bc::file_system_test::make_directory_raw(path_of("locked"), 0000);

    const auto entries = files().list_directory(in("locked"), FinalSymlink::refuse);

    ASSERT_FALSE(entries.has_value());
    EXPECT_EQ(entries.error().kind, ErrorKind::permission_denied);
}

// A listing is complete or it is an error. A directory that cannot be read fails its own listing and names
// itself; its parent's listing, one level deep, is complete and shows it as an entry.
// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListOfADirectoryThatCannotBeReadFailsWhileItsParentListsIt) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process lists what its permission bits forbid";
    }
    bc::file_system_test::write_raw(path_of("readable.txt"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("locked"), 0000);

    const auto parent = files().list_directory(root(), FinalSymlink::refuse);
    const auto locked = files().list_directory(in("locked"), FinalSymlink::refuse);

    ASSERT_TRUE(parent.has_value());
    EXPECT_THAT(names_of(*parent), ElementsAre("locked", "readable.txt"));
    ASSERT_FALSE(locked.has_value());
    EXPECT_EQ(locked.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(locked.error().path, path_of("locked").string());
}

// More entries than the face's first array holds, so the listing is asked for a second time with the capacity
// the core named.
// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_ListReturnsMoreEntriesThanTheFirstArrayHolds) {
    std::vector<std::string> expected;
    for (int number = 100; number < 400; ++number) {
        const std::string name = "entry-with-a-reasonably-long-name-" + std::to_string(number) + ".txt";
        bc::file_system_test::write_raw(path_of(name), "", 0644);
        expected.push_back(name);
    }

    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(names_of(*entries), expected);
}

// -------------------------------------------------------------------------------------------------------------
// status
// -------------------------------------------------------------------------------------------------------------

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_StatusOfNothingIsAnEmptyValueNotAnError) {
    const auto status = files().status(in("absent"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(status->has_value());
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_StatusBelowAFileIsAnEmptyValue) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto status = files().status(in("file/child"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(status->has_value());
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_StatusReportsAFile) {
    bc::file_system_test::write_raw(path_of("file"), "seven b", 0640);

    const auto status = files().status(in("file"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value());
    ASSERT_TRUE(status->has_value());
    const Status& file = **status;
    EXPECT_EQ(file.entry_type, EntryType::regular_file);
    EXPECT_EQ(file.size, 7);
    EXPECT_EQ(file.mode, 0640U);
    EXPECT_EQ(file.link_count, 1U);
    EXPECT_EQ((Identity{.device = file.device, .inode = file.inode}),
              bc::file_system_test::identity_raw(path_of("file")));
}

// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_StatusReportsADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0750);

    const auto status = files().status(in("folder"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value());
    ASSERT_TRUE(status->has_value());
    EXPECT_EQ((*status)->entry_type, EntryType::directory);
    EXPECT_EQ((*status)->size, 0);
    EXPECT_EQ((*status)->mode, 0750U);
}

// Under `refuse` a symlink is reported as a symlink whatever it points at; under `follow` the report is of
// what it points at, and a dangling link is absence.
// stage 13 status-and-reads
TEST_P(FileSystemContract, DISABLED_StatusReportsASymlinkAsASymlinkWhateverItPointsAt) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));
    bc::file_system_test::make_symlink_raw(path_of("nowhere"), path_of("dangling"));

    const auto to_a_file = files().status(in("link"), FinalSymlink::refuse);
    const auto to_nothing = files().status(in("dangling"), FinalSymlink::refuse);
    const auto through_to_the_file = files().status(in("link"), FinalSymlink::follow);
    const auto through_to_nothing = files().status(in("dangling"), FinalSymlink::follow);

    ASSERT_TRUE(to_a_file.has_value());
    ASSERT_TRUE(to_a_file->has_value());
    EXPECT_EQ((*to_a_file)->entry_type, EntryType::symlink);
    ASSERT_TRUE(to_nothing.has_value());
    ASSERT_TRUE(to_nothing->has_value());
    EXPECT_EQ((*to_nothing)->entry_type, EntryType::symlink);
    ASSERT_TRUE(through_to_the_file.has_value());
    ASSERT_TRUE(through_to_the_file->has_value());
    EXPECT_EQ((*through_to_the_file)->entry_type, EntryType::regular_file);
    ASSERT_TRUE(through_to_nothing.has_value());
    EXPECT_FALSE(through_to_nothing->has_value());
}

// -------------------------------------------------------------------------------------------------------------
// open_directory, and beneath (the resolution layer: every operation from here on resolves its location this way)
// -------------------------------------------------------------------------------------------------------------

// Lays out:  sub/ · sub/deeper/ · file · alias -> sub · outside/ · sub/way-out -> ../outside · dangling -> nowhere
auto lay_out_directories(const std::filesystem::path& root) -> void {
    bc::file_system_test::make_directory_raw(root / "sub", 0755);
    bc::file_system_test::make_directory_raw(root / "sub" / "deeper", 0755);
    bc::file_system_test::write_raw(root / "file", "not a directory", 0644);
    bc::file_system_test::make_symlink_raw("sub", root / "alias");
    bc::file_system_test::make_directory_raw(root / "outside", 0755);
    bc::file_system_test::make_symlink_raw("../outside", root / "sub" / "way-out");
    bc::file_system_test::make_symlink_raw("nowhere", root / "dangling");
}

// A relative path the suite spells as a literal it knows to be valid.
auto relative(std::string_view text) -> RelativePath {
    return RelativePath::from(text).value();
}

// How many components the test's own directory has: the walk from the root opens each of them.
auto components_of(const std::filesystem::path& path) -> std::uint64_t {
    std::uint64_t count = 0;
    for (const std::filesystem::path& part : path) {
        if (part != "/") {
            count += 1;
        }
    }
    return count;
}

TEST_P(FileSystemContract, OpenDirectoryOpensADirectoryAndRemembersItsText) {
    lay_out_directories(root_path());

    const auto directory = files().open_directory(in("sub"), FinalSymlink::refuse);

    ASSERT_TRUE(directory.has_value()) << directory.error().message();
    const auto opened_as = directory->opened_as();
    ASSERT_TRUE(opened_as.has_value());
    EXPECT_EQ(*opened_as, path_of("sub").string());
}

TEST_P(FileSystemContract, OpenDirectoryOfTheRootOpens) {
    const auto directory = files().open_directory(Location::at(std::filesystem::path{"/"}), FinalSymlink::refuse);

    ASSERT_TRUE(directory.has_value()) << directory.error().message();
    const auto opened_as = directory->opened_as();
    ASSERT_TRUE(opened_as.has_value());
    EXPECT_EQ(*opened_as, "/");
}

TEST_P(FileSystemContract, OpenDirectoryOfNothingIsNotFound) {
    const auto directory = files().open_directory(in("absent"), FinalSymlink::refuse);

    ASSERT_FALSE(directory.has_value());
    EXPECT_EQ(directory.error().kind, ErrorKind::not_found);
    EXPECT_EQ(directory.error().system_code, ENOENT);
    EXPECT_EQ(directory.error().operation, Operation::open_directory);
    EXPECT_EQ(directory.error().path, path_of("absent").string());
    EXPECT_TRUE(directory.error().step.has_value());
}

TEST_P(FileSystemContract, OpenDirectoryOfAFileAndBelowAFileIsNotDirectory) {
    lay_out_directories(root_path());

    const auto of_the_file = files().open_directory(in("file"), FinalSymlink::refuse);
    const auto below_the_file = files().open_directory(in("file/child"), FinalSymlink::refuse);

    ASSERT_FALSE(of_the_file.has_value());
    EXPECT_EQ(of_the_file.error().kind, ErrorKind::not_directory);
    EXPECT_EQ(of_the_file.error().system_code, ENOTDIR);
    ASSERT_FALSE(below_the_file.has_value());
    EXPECT_EQ(below_the_file.error().kind, ErrorKind::not_directory);
    EXPECT_EQ(below_the_file.error().path, path_of("file/child").string());
}

// Under `refuse` a final symlink is refused as what it is; under `follow` the directory behind it opens, and the
// handle remembers the text it was asked for, the link's.
TEST_P(FileSystemContract, OpenDirectoryRefusesAFinalSymlinkUnlessToldToFollow) {
    lay_out_directories(root_path());

    const auto refused = files().open_directory(in("alias"), FinalSymlink::refuse);
    const auto followed = files().open_directory(in("alias"), FinalSymlink::follow);
    const auto dangling = files().open_directory(in("dangling"), FinalSymlink::follow);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::symlink_refused);
    ASSERT_TRUE(followed.has_value()) << followed.error().message();
    EXPECT_EQ(followed->opened_as().value(), path_of("alias").string());
    const auto behind_the_link =
        files().open_directory(Location::beneath(*followed, relative("deeper")), FinalSymlink::refuse);
    EXPECT_TRUE(behind_the_link.has_value()) << behind_the_link.error().message();
    ASSERT_FALSE(dangling.has_value());
    EXPECT_EQ(dangling.error().kind, ErrorKind::not_found);
}

// Under `at` the system resolves the path, symlinks on the way included; only `beneath` refuses them.
TEST_P(FileSystemContract, OpenDirectoryUnderAtFollowsASymlinkOnTheWay) {
    lay_out_directories(root_path());

    const auto directory = files().open_directory(in("alias/deeper"), FinalSymlink::refuse);

    ASSERT_TRUE(directory.has_value()) << directory.error().message();
    EXPECT_EQ(directory->opened_as().value(), path_of("alias/deeper").string());
}

TEST_P(FileSystemContract, BeneathOpensADirectoryAndRemembersTheParentsTextASlashAndTheRelativePath) {
    lay_out_directories(root_path());
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();

    const auto deeper =
        files().open_directory(Location::beneath(*parent, relative("sub/deeper")), FinalSymlink::refuse);

    ASSERT_TRUE(deeper.has_value()) << deeper.error().message();
    EXPECT_EQ(deeper->opened_as().value(), path_of("sub/deeper").string());
}

// Containment: nothing above the held directory is reachable, because no component may be a symlink and `..`
// cannot be spelled. A symlink on the way and a symlink at the end are both refused, and the error names the
// text the caller holds, the directory's text, a slash and the relative path.
TEST_P(FileSystemContract, BeneathRefusesASymlinkOnTheWayAndAtTheEnd) {
    lay_out_directories(root_path());
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();

    const auto on_the_way =
        files().open_directory(Location::beneath(*parent, relative("alias/deeper")), FinalSymlink::refuse);
    const auto at_the_end =
        files().open_directory(Location::beneath(*parent, relative("sub/way-out")), FinalSymlink::refuse);

    ASSERT_FALSE(on_the_way.has_value());
    EXPECT_EQ(on_the_way.error().kind, ErrorKind::symlink_refused);
    EXPECT_EQ(on_the_way.error().path, path_of("alias/deeper").string());
    EXPECT_TRUE(on_the_way.error().step.has_value());
    ASSERT_FALSE(at_the_end.has_value());
    EXPECT_EQ(at_the_end.error().kind, ErrorKind::symlink_refused);
    EXPECT_FALSE(RelativePath::from("../outside").has_value());
}

TEST_P(FileSystemContract, BeneathOfNothingIsNotFoundAndBelowAFileIsNotDirectory) {
    lay_out_directories(root_path());
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();

    const auto absent =
        files().open_directory(Location::beneath(*parent, relative("sub/absent")), FinalSymlink::refuse);
    const auto below_a_file =
        files().open_directory(Location::beneath(*parent, relative("file/child")), FinalSymlink::refuse);

    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().kind, ErrorKind::not_found);
    EXPECT_EQ(absent.error().path, path_of("sub/absent").string());
    ASSERT_FALSE(below_a_file.has_value());
    EXPECT_EQ(below_a_file.error().kind, ErrorKind::not_directory);
}

TEST_P(FileSystemContract, OpenDirectoryWithoutPermissionIsPermissionDenied) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process opens what its permission bits forbid";
    }
    bc::file_system_test::make_directory_raw(path_of("locked"), 0000);

    const auto itself = files().open_directory(in("locked"), FinalSymlink::refuse);
    const auto below_it = files().open_directory(in("locked/child"), FinalSymlink::refuse);

    ASSERT_FALSE(itself.has_value());
    EXPECT_EQ(itself.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(itself.error().system_code, EACCES);
    ASSERT_FALSE(below_it.has_value());
    EXPECT_EQ(below_it.error().kind, ErrorKind::permission_denied);
}

// What resolving an `at` location costs, read from the system calls it made: the root once, then every
// component of the test's own directory and the leaf, each opened against the one before it, each bracketed
// by the identity check.
TEST_F(ObservedContract, OpenDirectoryOpensTheRootThenEveryComponentThenTheLeaf) {
    lay_out_directories(root_path());

    const auto directory = files().open_directory(in("sub"), FinalSymlink::refuse);

    ASSERT_TRUE(directory.has_value()) << directory.error().message();
    const std::uint64_t opens = components_of(root_path()) + 1;
    EXPECT_EQ(count(Operation::open_directory, Step::open), 1U);
    EXPECT_EQ(count(Operation::open_directory, Step::openat), opens);
    EXPECT_EQ(count(Operation::open_directory, Step::fstatat), opens);
    EXPECT_EQ(count(Operation::open_directory, Step::fstat), opens);
    EXPECT_EQ(count(Operation::open_directory, Step::identity_check), opens);
    EXPECT_EQ(count(Operation::open_directory, Step::openat2), 0U);
    EXPECT_EQ(count(Operation::open_directory, Step::openat_nofollow_any), 0U);
}

// The fast path, where the platform has one, opens the whole parent in one call under its own step; where it
// has none, the walk opens each component. Either way the leaf is one more openat.
TEST_F(ObservedContract, BeneathTakesTheFastPathWhereThePlatformHasOneAndWalksOtherwise) {
    lay_out_directories(root_path());
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();
    const std::uint64_t opens_before = count(Operation::open_directory, Step::openat);

    const auto deeper =
        files().open_directory(Location::beneath(*parent, relative("sub/deeper")), FinalSymlink::refuse);

    ASSERT_TRUE(deeper.has_value()) << deeper.error().message();
    const std::uint64_t fast_path_opens =
        count(Operation::open_directory, Step::openat2) + count(Operation::open_directory, Step::openat_nofollow_any);
    const std::uint64_t component_opens = count(Operation::open_directory, Step::openat) - opens_before;
    if (fast_path_opens == 1) {
        EXPECT_EQ(component_opens, 1U);
    } else {
        EXPECT_EQ(fast_path_opens, 0U);
        EXPECT_EQ(component_opens, 2U);
    }
}

// The portable path through the same gate every face can script: a refusal on the fast path's step, with one
// of the errnos a kernel without the call would answer, makes the engine walk instead. Both steps are scripted;
// only the platform's own is ever reached.
TEST_F(ObservedContract, BeneathFallsBackToTheWalkWhenTheFastPathIsRefused) {
    lay_out_directories(root_path());
    script(fail_step(Operation::open_directory, Step::openat2, 1, ENOSYS));
    script(fail_step(Operation::open_directory, Step::openat_nofollow_any, 1, ENOSYS));
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();
    const std::uint64_t opens_before = count(Operation::open_directory, Step::openat);

    const auto deeper =
        files().open_directory(Location::beneath(*parent, relative("sub/deeper")), FinalSymlink::refuse);

    ASSERT_TRUE(deeper.has_value()) << deeper.error().message();
    EXPECT_EQ(deeper->opened_as().value(), path_of("sub/deeper").string());
    EXPECT_EQ(count(Operation::open_directory, Step::openat) - opens_before, 2U);
}

// Any other errno on the fast path is the operation's error, never a silent walk.
TEST_F(ObservedContract, AnyOtherErrnoOnTheFastPathIsTheOperationsError) {
    lay_out_directories(root_path());
    script(fail_step(Operation::open_directory, Step::openat2, 1, EIO));
    script(fail_step(Operation::open_directory, Step::openat_nofollow_any, 1, EIO));
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();

    const auto deeper =
        files().open_directory(Location::beneath(*parent, relative("sub/deeper")), FinalSymlink::refuse);

    const std::uint64_t fast_path_opens =
        count(Operation::open_directory, Step::openat2) + count(Operation::open_directory, Step::openat_nofollow_any);
    if (fast_path_opens == 0) {
        EXPECT_TRUE(deeper.has_value()) << "this platform has no fast path; the walk resolved";
        return;
    }
    ASSERT_FALSE(deeper.has_value());
    EXPECT_EQ(deeper.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(deeper.error().system_code, EIO);
    EXPECT_TRUE(deeper.error().step == Step::openat2 || deeper.error().step == Step::openat_nofollow_any);
}

// The identity check is a step of its own, so the directory-swapped-between-stat-and-open race can be scripted
// without a race: its refusal is `symlink_refused` at that step. The race itself is the C-only test's.
TEST_F(ObservedContract, ARefusedIdentityCheckIsSymlinkRefused) {
    lay_out_directories(root_path());
    script(fail_step(Operation::open_directory, Step::identity_check, 1, EIO));

    const auto directory = files().open_directory(in("sub"), FinalSymlink::refuse);

    ASSERT_FALSE(directory.has_value());
    EXPECT_EQ(directory.error().kind, ErrorKind::symlink_refused);
    EXPECT_EQ(directory.error().step, Step::identity_check);
    EXPECT_EQ(directory.error().system_code, EIO);
    EXPECT_EQ(directory.error().path, path_of("sub").string());
}

TEST_F(ObservedContract, AnInterruptedComponentOpenIsAskedAgain) {
    lay_out_directories(root_path());
    script(fail_step(Operation::open_directory, Step::openat, 1, EINTR));

    const auto directory = files().open_directory(in("sub"), FinalSymlink::refuse);

    ASSERT_TRUE(directory.has_value()) << directory.error().message();
    EXPECT_EQ(count(Operation::open_directory, Step::openat), components_of(root_path()) + 2);
}

// -------------------------------------------------------------------------------------------------------------
// create_directories
// -------------------------------------------------------------------------------------------------------------

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_CreateDirectoriesMakesEveryMissingDirectoryWithTheMode) {
    const auto created = files().create_directories(in("a/b/c"), Permissions::exactly(0750));

    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a")), 0750U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b")), 0750U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b/c")), 0750U);
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_CreateDirectoriesLeavesWhatExistsAsItIsUnderEitherPolicy) {
    bc::file_system_test::make_directory_raw(path_of("existing"), 0700);

    const auto under_exactly = files().create_directories(in("existing/new"), Permissions::exactly(0755));
    const auto again = files().create_directories(in("existing"), Permissions::exactly(0755));
    const auto under_preserve = files().create_directories(in("existing"), Permissions::preserve_or(0755));

    ASSERT_TRUE(under_exactly.has_value());
    ASSERT_TRUE(again.has_value());
    ASSERT_TRUE(under_preserve.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("existing")), 0700U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("existing/new")), 0755U);
}

// Under `at` the bytes go to the system as spelled, which accepts a trailing separator. Whether an absolute
// spelling with one is one spelling of its path is the path module's to decide (stage 10); this case holds
// the system's answer until then.
// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_CreateDirectoriesAcceptsATrailingSeparator) {
    const auto created = files().create_directories(in("a/b/"), Permissions::exactly(0755));

    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b")), 0755U);
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_CreateDirectoriesWhereAFileIsIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto onto_the_file = files().create_directories(in("file"), Permissions::exactly(0755));
    const auto below_the_file = files().create_directories(in("file/child"), Permissions::exactly(0755));

    ASSERT_FALSE(onto_the_file.has_value());
    EXPECT_EQ(onto_the_file.error().kind, ErrorKind::not_directory);
    ASSERT_FALSE(below_the_file.has_value());
    EXPECT_EQ(below_the_file.error().kind, ErrorKind::not_directory);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
}

// Under `at` the system resolves the path, symlinks on the way included; only `beneath` refuses them.
// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_CreateDirectoriesAcceptsASymlinkToADirectoryOnTheWay) {
    bc::file_system_test::make_directory_raw(path_of("real"), 0755);
    bc::file_system_test::make_symlink_raw(path_of("real"), path_of("alias"));

    const auto created = files().create_directories(in("alias/child"), Permissions::exactly(0755));

    ASSERT_TRUE(created.has_value());
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("real/child")));
}

// A mode with bits outside 07777 is a contract violation, refused before any system call: green now.
TEST_P(FileSystemContract, CreateDirectoriesRefusesAModeThatIsNotAMode) {
    const auto created = files().create_directories(in("a"), Permissions::exactly(040755));

    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(created.error().operation, Operation::create_directories);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("a")));
}

// -------------------------------------------------------------------------------------------------------------
// remove_file
// -------------------------------------------------------------------------------------------------------------

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveFileRemovesAFileAndSaysThereWasOne) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto removed = files().remove_file(in("file"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("file")));
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveFileOfNothingSaysThereWasNone) {
    const auto removed = files().remove_file(in("absent"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(*removed);
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveFileRemovesASymlinkAndNotWhatItPointsAt) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto removed = files().remove_file(in("link"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("link")));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("target.txt")), "behind the link");
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveFileRefusesADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto removed = files().remove_file(in("folder"));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::not_regular_file);
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("folder")));
}

// -------------------------------------------------------------------------------------------------------------
// remove_tree
// -------------------------------------------------------------------------------------------------------------

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveTreeRemovesEverythingAndSaysThereWasOne) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    lay_out_a_small_tree(path_of("tree"));

    const auto removed = files().remove_tree(in("tree"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("tree")));
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveTreeOfNothingRemovesNothing) {
    const auto removed = files().remove_tree(in("absent"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(*removed);
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveTreeOfAFileRemovesTheFile) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto removed = files().remove_tree(in("file"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("file")));
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_RemoveTreeRemovesSymlinksAndNeverWhatTheyPointAt) {
    bc::file_system_test::make_directory_raw(path_of("outside"), 0755);
    bc::file_system_test::write_raw(path_of("outside/kept.txt"), "must survive", 0644);
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::make_symlink_raw(path_of("outside"), path_of("tree/way-out"));
    bc::file_system_test::make_symlink_raw(path_of("outside"), path_of("alias"));

    const auto tree = files().remove_tree(in("tree"));
    const auto alias = files().remove_tree(in("alias"));

    ASSERT_TRUE(tree.has_value());
    EXPECT_TRUE(*tree);
    ASSERT_TRUE(alias.has_value());
    EXPECT_TRUE(*alias);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("tree")));
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("alias")));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("outside/kept.txt")), "must survive");
}

// -------------------------------------------------------------------------------------------------------------
// remove_tree and the root of the file system (the draft's file_system_root cases)
// -------------------------------------------------------------------------------------------------------------

// `remove_tree` refuses the root by spelling, before any system call, and by identity, before any removal.
// Both are asked here. No case calls `remove_tree` on a spelling of the root without the net below: a fault
// that stops the operation at its first `unlinkat`, so that if the refusal were broken, the case would be
// the proof and not the damage. The net is the observed engine's, so these run on it alone.
class RootRefusal : public ObservedContract {
protected:
    auto SetUp() -> void override {
        ObservedContract::SetUp();
        script(stop_at(Operation::remove_tree, Step::unlinkat, 1));
    }

    // `remove_tree` on the spelling, which must be refused with nothing removed.
    auto expect_refused_by_spelling(std::string_view spelling) -> void {
        const auto removed = files().remove_tree(Location::at(std::filesystem::path{spelling}));
        ASSERT_FALSE(removed.has_value()) << spelling;
        EXPECT_EQ(removed.error().kind, ErrorKind::invalid_argument) << spelling;
        EXPECT_FALSE(removed.error().step.has_value()) << spelling;
        EXPECT_EQ(count(Operation::remove_tree, Step::unlinkat), 0U) << spelling;
    }
};

// stage 15 directories-in-c
TEST_F(RootRefusal, DISABLED_TheRootIsTheRoot) {
    expect_refused_by_spelling("/");
}

// stage 15 directories-in-c
TEST_F(RootRefusal, DISABLED_RepeatedSeparatorsAreStillTheRoot) {
    expect_refused_by_spelling("//");
    expect_refused_by_spelling("///");
}

// stage 15 directories-in-c
TEST_F(RootRefusal, DISABLED_TheRootsOwnDotAndDotDotAreTheRoot) {
    expect_refused_by_spelling("/.");
    expect_refused_by_spelling("/./");
    expect_refused_by_spelling("/..");
    expect_refused_by_spelling("/../..");
    expect_refused_by_spelling("/./../.");
}

// stage 15 directories-in-c
TEST_F(RootRefusal, DISABLED_ANameCancelledByDotDotLeavesTheRoot) {
    expect_refused_by_spelling("/tmp/..");
    expect_refused_by_spelling("/tmp/../");
    expect_refused_by_spelling("/tmp/./..");
    expect_refused_by_spelling("/a/b/../..");
    expect_refused_by_spelling("/a/../b/..");
    // More ".." than names: the parent of the root is the root.
    expect_refused_by_spelling("/a/../../..");
}

// The same spellings below the test's own directory name nothing, and nothing is not the root: the call
// proceeds and answers "there was nothing".
// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_AnythingBelowTheRootIsNotTheRoot) {
    for (const std::string_view spelling : {"tmp", "tmp/", "tmp/cache/files", "tmp/cache/..", "a/../b", "..."}) {
        const auto removed = files().remove_tree(in(spelling));
        ASSERT_TRUE(removed.has_value()) << spelling << ": " << removed.error().message();
        EXPECT_FALSE(*removed) << spelling;
    }
}

// Where a relative path leads is not in its text, so `at` refuses relative text at the door, and `beneath`
// cannot spell `..` at all: neither form ever reaches the question. Both refusals are the argument check's,
// so this case is green now.
TEST_P(FileSystemContract, ARelativePathIsRefusedAtTheDoorAndNeverCalledTheRoot) {
    for (const std::string_view spelling : {"", ".", "..", "tmp/..", "cache"}) {
        const auto removed = files().remove_tree(Location::at(std::filesystem::path{spelling}));
        ASSERT_FALSE(removed.has_value()) << spelling;
        EXPECT_EQ(removed.error().kind, ErrorKind::invalid_argument) << spelling;
        EXPECT_EQ(removed.error().operation, Operation::remove_tree) << spelling;
    }
    EXPECT_FALSE(RelativePath::from("..").has_value());
    EXPECT_FALSE(RelativePath::from("tmp/..").has_value());
    EXPECT_TRUE(RelativePath::from("cache").has_value());
}

// The identity check: a directory that is the root however the path reached it. A symlink to the root with
// `/.` behind it spells no root, and the system resolves it to one.
// stage 15 directories-in-c
TEST_F(RootRefusal, DISABLED_ARootReachedThroughASymlinkIsRefusedByIdentity) {
    bc::file_system_test::make_symlink_raw("/", path_of("alias"));

    const auto removed = files().remove_tree(in("alias/."));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(count(Operation::remove_tree, Step::unlinkat), 0U);
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("alias")));
}

// stage 15 directories-in-c
TEST_P(FileSystemContract, DISABLED_AnOrdinaryDirectoryAFileAndNothingAreNotTheRoot) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto a_directory = files().remove_tree(in("folder"));
    const auto a_file = files().remove_tree(in("file"));
    const auto nothing = files().remove_tree(in("absent"));

    ASSERT_TRUE(a_directory.has_value());
    EXPECT_TRUE(*a_directory);
    ASSERT_TRUE(a_file.has_value());
    EXPECT_TRUE(*a_file);
    ASSERT_TRUE(nothing.has_value());
    EXPECT_FALSE(*nothing);
}

// What the spelling cannot show: `alias/..` reduces, by text, to the test's own directory, while the system
// resolves the alias to the root and its parent to the root again. The spelling says "the test directory";
// the system says "the root"; the identity check refuses.
// stage 15 directories-in-c
TEST_F(RootRefusal, DISABLED_ARootReachedByDotDotThroughASymlinkIsRefusedByIdentity) {
    bc::file_system_test::make_symlink_raw("/", path_of("alias"));

    const auto removed = files().remove_tree(in("alias/.."));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(count(Operation::remove_tree, Step::unlinkat), 0U);
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("alias")));
}

// -------------------------------------------------------------------------------------------------------------
// rename
// -------------------------------------------------------------------------------------------------------------

// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_RenameMovesAFile) {
    bc::file_system_test::write_raw(path_of("old-name"), "content", 0644);

    const auto achieved =
        files().rename(in("old-name"), in("new-name"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("new-name"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("new-name")), "content");
}

// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_RenameReplacesAFileAtTheDestination) {
    bc::file_system_test::write_raw(path_of("staged"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);

    const auto achieved =
        files().rename(in("staged"), in("current"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("current"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the new content");
}

// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_RenameUnderRefuseLeavesAnExistingDestinationAndIsAlreadyExists) {
    bc::file_system_test::write_raw(path_of("staged"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);

    const auto achieved = files().rename(in("staged"), in("current"), ExistingDestination::refuse, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::already_exists);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("staged")), "the new content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the old content");
}

// The error of a rename whose source is missing names the source, the entry that was not there. (The face
// composes a rename's error path from its destination, decision 12 of the header-and-faces report; the
// contract asks for the location that failed, which stage 16 reconciles.)
// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_RenameOfNothingIsNotFoundAndNamesTheSource) {
    const auto achieved =
        files().rename(in("absent"), in("new-name"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().path, path_of("absent").string());
}

// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_RenameIntoADirectoryThatIsNotThereIsNotFoundAndNamesTheDestination) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto achieved =
        files().rename(in("file"), in("missing/file"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().path, path_of("missing/file").string());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
}

// Under `replace` a directory replaces only an empty directory; one that is not empty is `directory_not_empty`.
// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_RenameOfADirectoryOntoOneThatIsNotEmptyIsDirectoryNotEmpty) {
    bc::file_system_test::make_directory_raw(path_of("incoming"), 0755);
    bc::file_system_test::make_directory_raw(path_of("occupied"), 0755);
    bc::file_system_test::write_raw(path_of("occupied/kept.txt"), "must survive", 0644);

    const auto achieved =
        files().rename(in("incoming"), in("occupied"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::directory_not_empty);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("occupied/kept.txt")), "must survive");
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("incoming")));
}

// stage 16 moves-and-copies-in-c
TEST_F(ObservedContract, DISABLED_RenameThatFailsLeavesBothEndsAsTheyWere) {
    bc::file_system_test::write_raw(path_of("staged"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);
    script(fail_step(Operation::rename, Step::renameat, 1, EIO));

    const auto achieved =
        files().rename(in("staged"), in("current"), ExistingDestination::replace, Durability::durable);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("staged")), "the new content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the old content");
}

// stage 16 moves-and-copies-in-c
TEST_F(ObservedContract, DISABLED_DurableRenameAcrossDirectoriesSyncsBothOfThem) {
    bc::file_system_test::make_directory_raw(path_of("from"), 0755);
    bc::file_system_test::make_directory_raw(path_of("to"), 0755);
    bc::file_system_test::write_raw(path_of("from/file"), "content", 0644);

    const auto achieved =
        files().rename(in("from/file"), in("to/file"), ExistingDestination::replace, Durability::durable);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::rename, durable_sync_step()), 2U);
    EXPECT_EQ(count(Operation::rename, wrong_sync_step()), 0U);
}

// stage 16 moves-and-copies-in-c
TEST_F(ObservedContract, DISABLED_DurableRenameWithinOneDirectorySyncsItOnce) {
    bc::file_system_test::write_raw(path_of("old-name"), "content", 0644);

    const auto achieved =
        files().rename(in("old-name"), in("new-name"), ExistingDestination::replace, Durability::durable);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::rename, durable_sync_step()), 1U);
}

// The name has already moved when the sync fails, so the call reports what it achieved, not an error.
// stage 16 moves-and-copies-in-c
TEST_F(ObservedContract, DISABLED_DurableRenameWhoseDirectorySyncFailsReportsWritten) {
    bc::file_system_test::write_raw(path_of("old-name"), "content", 0644);
    script(fail_step(Operation::rename, durable_sync_step(), 1, EIO));

    const auto achieved =
        files().rename(in("old-name"), in("new-name"), ExistingDestination::replace, Durability::durable);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("new-name"));
}

// -------------------------------------------------------------------------------------------------------------
// copy_file
// -------------------------------------------------------------------------------------------------------------

// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_CopyWritesTheSourcesBytesAndLeavesTheSource) {
    const std::string large(200 * 1024, 'y');
    bc::file_system_test::write_raw(path_of("source"), large, 0644);

    const auto achieved = files().copy_file(in("source"), in("copy"), kWrittenExactly0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("copy")), large);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("source")), large);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("copy")), 0600U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("copy", "source"));
}

// As for a rename of nothing: the contract names the source, the entry that was not there.
// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_CopyOfNothingIsNotFoundAndNamesTheSource) {
    const auto achieved = files().copy_file(in("absent"), in("copy"), kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().path, path_of("absent").string());
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("copy")));
}

// Under `refuse` a source that is a symlink is refused as one (the clone path takes CLONE_NOFOLLOW).
// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_CopyRefusesASourceThatIsASymlink) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto achieved = files().copy_file(in("link"), in("copy"), kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::symlink_refused);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("copy")));
}

// preserve_or keeps the mode of the file being replaced. The source's mode plays no part.
// stage 16 moves-and-copies-in-c
TEST_P(FileSystemContract, DISABLED_CopyUnderPreserveOrKeepsTheDestinationsModeNotTheSources) {
    bc::file_system_test::write_raw(path_of("source"), "the new content", 0666);
    bc::file_system_test::write_raw(path_of("existing"), "the old content", 0640);

    const auto onto_existing = files().copy_file(in("source"), in("existing"), kWrittenPreserveOr0600);
    const auto onto_nothing = files().copy_file(in("source"), in("new"), kWrittenPreserveOr0600);

    ASSERT_TRUE(onto_existing.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("existing")), "the new content");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("existing")), 0640U);
    ASSERT_TRUE(onto_nothing.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("new")), 0600U);
}

// stage 16 moves-and-copies-in-c
TEST_F(ObservedContract, DISABLED_CopyWhoseRenameFailsLeavesTheDestinationWholeAndNoTemporaryFile) {
    bc::file_system_test::write_raw(path_of("source"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("existing"), "the old content", 0644);
    script(fail_step(Operation::copy_file, Step::renameat, 1, EIO));

    const auto achieved = files().copy_file(in("source"), in("existing"), kDurableExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("existing")), "the old content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("existing", "source"));
}

// stage 16 moves-and-copies-in-c
TEST_F(ObservedContract, DISABLED_DurableCopySyncsTheFileThenTheDirectory) {
    bc::file_system_test::write_raw(path_of("source"), "content", 0644);

    const auto achieved = files().copy_file(in("source"), in("copy"), kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::copy_file, durable_sync_step()), 2U);
    EXPECT_EQ(count(Operation::copy_file, wrong_sync_step()), 0U);
}

// -------------------------------------------------------------------------------------------------------------
// Descriptors
// -------------------------------------------------------------------------------------------------------------

// Every operation, on its success path and on a failure path that comes after something was opened, gives back
// each descriptor it took. The fixture checks this for every case; this case runs every operation, several
// times, and then a write whose rename fails after the temporary file was opened, written and synced. The
// static_casts discard results this test does not judge.
// stage 17 locks-links-condemn
TEST_F(ObservedContract, DISABLED_EveryOperationClosesWhatItOpened) {
    lay_out_a_small_tree(root_path());
    const auto bytes = bytes_of("content");
    const std::vector<int> before = bc::file_system_test::open_descriptors();

    for (int round = 0; round < 8; ++round) {
        static_cast<void>(files().read_file(in("a.txt"), kWholeFileRefusingSymlink));
        static_cast<void>(files().read_file(in("sub"), kWholeFileRefusingSymlink));
        static_cast<void>(files().read_file(in("link"), kWholeFileRefusingSymlink));
        static_cast<void>(files().read_file(in("a.txt"), reading_at_most(2)));
        static_cast<void>(files().write_file(in("written"), bytes, kDurableExactly0644));
        static_cast<void>(files().write_file(in("sub"), bytes, kWrittenExactly0644));
        static_cast<void>(files().append(in("log"), bytes, Durability::durable, Permissions::exactly(0644)));
        static_cast<void>(files().append(in("sub"), bytes, Durability::written, Permissions::exactly(0644)));
        static_cast<void>(files().sync(in("log")));
        static_cast<void>(files().list_directory(root(), FinalSymlink::refuse));
        static_cast<void>(files().list_directory(in("a.txt"), FinalSymlink::refuse));
        static_cast<void>(files().status(in("a.txt"), FinalSymlink::refuse));
        static_cast<void>(files().create_directory(in("made"), Permissions::exactly(0755)));
        static_cast<void>(files().create_directories(in("made/deeper"), Permissions::exactly(0755)));
        static_cast<void>(files().create_temporary_directory(in("made"), Permissions::exactly(0700)));
        static_cast<void>(files().set_permissions(in("made"), 0700, FinalSymlink::refuse));
        static_cast<void>(files().copy_file(in("a.txt"), in("copy"), kDurablePreserveOr0644));
        static_cast<void>(files().copy_file(in("sub"), in("copy"), kWrittenExactly0644));
        static_cast<void>(files().rename(in("copy"), in("renamed"), ExistingDestination::replace, Durability::durable));
        static_cast<void>(files().create_symlink("renamed", in("renamed-link")));
        static_cast<void>(files().read_symlink(in("renamed-link")));
        static_cast<void>(files().try_lock(in("renamed")));
        static_cast<void>(files().remove_file(in("renamed")));
        static_cast<void>(files().remove_file(in("renamed-link")));
        static_cast<void>(files().condemn(in("made")));
        static_cast<void>(files().remove_condemned(root()));
        static_cast<void>(files().remove_tree(in("made")));
        static_cast<void>(files().open_directory(in("sub"), FinalSymlink::refuse));
    }

    // A rename that fails after the temporary file was opened, written and synced.
    script(fail_step(Operation::write_file, Step::renameat, 1, EIO));
    static_cast<void>(files().write_file(in("written"), bytes, kDurableExactly0644));

    EXPECT_EQ(bc::file_system_test::open_descriptors(), before);
}

} // namespace
