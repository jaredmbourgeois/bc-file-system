#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
//   - the same engine with the observer installed, which must change nothing it was not scripted to change;
//   - the portable-only engine, the observed engine refused every fast path the platform's probe found, so every
//     operation takes the path every platform has: a `beneath` location walked one component at a time, a copy
//     through the read-and-write loop, fsync for a durable sync. That run proves on a platform with every fast
//     path what a platform with none does. Where an operation's promise is kept by a fast path alone (the
//     no-replace rename of every `refuse` and every condemn) it answers `unsupported`, as such a platform does,
//     and the case states both answers through `expected_under`, with the reason at the case.
//
// A failure that cannot be provoked for real (the disk filling up, a sync failing after the rename) is put into
// the middle of an operation with a fault scripted into the observed engine; those cases run on the observed
// engine alone (`ObservedContract`), since a live engine takes no faults. Where a real provocation exists as
// well, the suite carries both.
//
// A case whose operation the core does not build yet is disabled, with the stage that turns it on in the
// comment above it; that operation is an honest stub until then. A case the stub's own argument check already
// answers is green.

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
using bc::file_system::FailedLocation;
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
constexpr WriteOptions kWrittenPreserveOr0644{.durability = Durability::written,
                                              .permissions = Permissions::preserve_or(0644),
                                              .existing_destination = ExistingDestination::replace,
                                              .final_symlink = FinalSymlink::refuse};
constexpr WriteOptions kDurableExactly0644Refusing{.durability = Durability::durable,
                                                   .permissions = Permissions::exactly(0644),
                                                   .existing_destination = ExistingDestination::refuse,
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

// How many components the test's own directory has: the walk from the root opens each of them with `openat`,
// so an operation's own `openat` of its leaf is the next occurrence after them.
auto components_of(const std::filesystem::path& path) -> std::uint64_t {
    std::uint64_t count = 0;
    for (const std::filesystem::path& part : path) {
        if (part != "/") {
            count += 1;
        }
    }
    return count;
}

// The names in `directory` that are staged files of `leaf`: a staged write's temporary is `.<leaf>.bc-tmp-…`
// beside its destination, and must never be seen after the write ends, however it ends.
auto staged_names_of(const std::filesystem::path& directory, std::string_view leaf) -> std::vector<std::string> {
    const std::vector<std::string> names = bc::file_system_test::names_raw(directory);
    std::vector<std::string> staged;
    staged.reserve(names.size());
    const std::string prefix = "." + std::string(leaf) + ".bc-tmp-";
    for (const std::string& name : names) {
        if (name.starts_with(prefix)) {
            staged.push_back(name);
        }
    }
    return staged;
}

// -------------------------------------------------------------------------------------------------------------
// The fixtures
// -------------------------------------------------------------------------------------------------------------

enum class EngineKind : std::uint8_t {
    live,
    observed,
    portable_only,
};

auto make_engine(EngineKind kind) -> Result<Engine> {
    if (kind == EngineKind::observed) {
        return Engine::observed_for_tests();
    }
    if (kind == EngineKind::portable_only) {
        return Engine::portable_only_for_tests();
    }
    return Engine::live();
}

auto engine_name(const testing::TestParamInfo<EngineKind>& info) -> std::string {
    if (info.param == EngineKind::observed) {
        return "Observed";
    }
    if (info.param == EngineKind::portable_only) {
        return "PortableOnly";
    }
    return "Live";
}

// The answer a case expects from the engine it runs on, where the engines differ. The live and the observed
// engine take the fast paths the platform's probe found; the portable-only engine takes none, so where an
// operation's promise is kept by a fast path alone it answers what a platform without that path answers. The
// call site says which fast path and why nothing replaces it.
template <class Answer>
auto expected_under(EngineKind kind, Answer with_the_platforms_fast_paths, Answer with_no_fast_path) -> Answer {
    if (kind == EngineKind::portable_only) {
        return with_no_fast_path;
    }
    return with_the_platforms_fast_paths;
}

// An operation's answer as a case compares it through `expected_under`: no kind for a success, the error's kind
// for a failure.
template <class Value>
auto answer_of(const Result<Value>& result) -> std::optional<ErrorKind> {
    if (result.has_value()) {
        return std::nullopt;
    }
    return result.error().kind;
}

constexpr std::optional<ErrorKind> kSucceeds = std::nullopt;
constexpr std::optional<ErrorKind> kUnsupported = ErrorKind::unsupported;
constexpr std::optional<ErrorKind> kInvalidArgument = ErrorKind::invalid_argument;
constexpr std::optional<ErrorKind> kNotFound = ErrorKind::not_found;

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

INSTANTIATE_TEST_SUITE_P(Engines, FileSystemContract,
                         testing::Values(EngineKind::live, EngineKind::observed, EngineKind::portable_only),
                         engine_name);

// What a fixture over an engine created for tests adds: scripting a fault and reading a step count.
class ContractWithObserver : public Contract<bc::file_system_test::DescriptorBalanced> {
protected:
    explicit ContractWithObserver(Result<Engine> engine) : Contract(std::move(engine)) {}

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

// The cases that script a fault or read a step count, which only an engine created for tests offers.
class ObservedContract : public ContractWithObserver {
protected:
    ObservedContract() : ContractWithObserver(Engine::observed_for_tests()) {}
};

// The cases that read what the portable-only engine did: never a fast path's step, and the steps of the path
// every platform has instead.
class PortableOnlyContract : public ContractWithObserver {
protected:
    PortableOnlyContract() : ContractWithObserver(Engine::portable_only_for_tests()) {}
};

// -------------------------------------------------------------------------------------------------------------
// read_file
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, ReadFileReturnsEveryByte) {
    bc::file_system_test::write_raw(path_of("notes.txt"), "first line\nsecond line\n", 0644);

    const auto bytes = files().read_file(in("notes.txt"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(text_of(*bytes), "first line\nsecond line\n");
}

TEST_P(FileSystemContract, ReadFileOfAnEmptyFileIsEmpty) {
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);

    const auto bytes = files().read_file(in("empty"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value());
    EXPECT_THAT(*bytes, IsEmpty());
}

TEST_P(FileSystemContract, ReadFileReturnsAFileLargerThanAnyInternalBuffer) {
    const std::string large(300 * 1024, 'x');
    bc::file_system_test::write_raw(path_of("large.bin"), large, 0644);

    const auto bytes = files().read_file(in("large.bin"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(text_of(*bytes), large);
}

TEST_P(FileSystemContract, ReadFileOfNothingIsNotFound) {
    const auto bytes = files().read_file(in("absent.txt"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_found);
    EXPECT_EQ(bytes.error().system_code, ENOENT);
    EXPECT_EQ(bytes.error().path, path_of("absent.txt").string());
    EXPECT_TRUE(bytes.error().step.has_value());
    EXPECT_FALSE(bytes.error().message().empty());
}

TEST_P(FileSystemContract, ReadFileIfPresentOfNothingIsAnEmptyValue) {
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
TEST_P(FileSystemContract, ReadFileRefusesASymlinkEvenToARegularFileUnlessToldToFollow) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto refused = files().read_file(in("link"), kWholeFileRefusingSymlink);
    const auto followed = files().read_file(in("link"), kWholeFileFollowingSymlink);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::symlink_refused);
    ASSERT_TRUE(followed.has_value());
    EXPECT_EQ(text_of(*followed), "behind the link");
}

TEST_P(FileSystemContract, ReadFileRefusesADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto bytes = files().read_file(in("folder"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_regular_file);
}

// A FIFO is refused as what it is, and the refusal comes back at once: opening a FIFO with no writer would
// otherwise wait for one forever.
TEST_P(FileSystemContract, ReadFileRefusesAFifoWithoutWaitingForAWriter) {
    bc::file_system_test::make_fifo_raw(path_of("pipe"), 0600);

    const auto bytes = files().read_file(in("pipe"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_regular_file);
    EXPECT_FALSE(bytes.error().step.has_value());
}

// The root is a directory, which the read refuses like any other.
TEST_P(FileSystemContract, ReadFileOfTheRootIsNotRegularFile) {
    const auto bytes = files().read_file(Location::at(std::filesystem::path{"/"}), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_regular_file);
}

TEST_P(FileSystemContract, ReadFileBelowAFileIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "not a directory", 0644);

    const auto bytes = files().read_file(in("file/child"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_directory);
}

TEST_P(FileSystemContract, ReadFileWithoutPermissionIsPermissionDenied) {
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

TEST_P(FileSystemContract, ReadFileUnderAtMostReturnsAFileWithinTheLimit) {
    bc::file_system_test::write_raw(path_of("alphabet"), "abcdefghij", 0644);

    const auto exactly_the_size = files().read_file(in("alphabet"), reading_at_most(10));
    const auto under_the_size = files().read_file(in("alphabet"), reading_at_most(100));

    ASSERT_TRUE(exactly_the_size.has_value());
    EXPECT_EQ(text_of(*exactly_the_size), "abcdefghij");
    ASSERT_TRUE(under_the_size.has_value());
    EXPECT_EQ(text_of(*under_the_size), "abcdefghij");
}

TEST_P(FileSystemContract, ReadFileUnderAtMostOfALargerFileIsTooLargeNeverAPrefix) {
    bc::file_system_test::write_raw(path_of("alphabet"), "abcdefghij", 0644);

    const auto bytes = files().read_file(in("alphabet"), reading_at_most(4));

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::too_large);
}

TEST_P(FileSystemContract, ReadFileUnderAtMostOfAnEmptyFileIsEmpty) {
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);

    const auto under_a_bound = files().read_file(in("empty"), reading_at_most(5));
    const auto under_zero = files().read_file(in("empty"), reading_at_most(0));

    ASSERT_TRUE(under_a_bound.has_value());
    EXPECT_THAT(*under_a_bound, IsEmpty());
    ASSERT_TRUE(under_zero.has_value());
    EXPECT_THAT(*under_zero, IsEmpty());
}

TEST_P(FileSystemContract, ReadFileUnderAtMostOfNothingIsNotFound) {
    const auto bytes = files().read_file(in("absent"), reading_at_most(16));

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::not_found);
}

TEST_P(FileSystemContract, ReadFileUnderAtMostRefusesASymlinkAndADirectory) {
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

TEST_P(FileSystemContract, WriteCreatesTheFileWithItsBytesAndItsMode) {
    const auto bytes = bytes_of("fresh content");

    const auto achieved = files().write_file(in("new.txt"), bytes, kWrittenExactly0640);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("new.txt")), "fresh content");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("new.txt")), 0640U);
}

TEST_P(FileSystemContract, WriteReplacesTheContentAndLeavesNoTemporaryFile) {
    bc::file_system_test::write_raw(path_of("config"), "old content, and longer than the new", 0644);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("config")), "new");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("config"));
}

TEST_P(FileSystemContract, WriteOfNoBytesCreatesAnEmptyFile) {
    const auto achieved = files().write_file(in("empty"), {}, kWrittenExactly0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("empty")), "");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("empty")), 0600U);
}

TEST_P(FileSystemContract, WriteUnderExactlyGivesAReplacedFileTheStatedMode) {
    bc::file_system_test::write_raw(path_of("config"), "old", 0600);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0644U);
}

TEST_P(FileSystemContract, WriteUnderPreserveOrKeepsTheReplacedFilesMode) {
    bc::file_system_test::write_raw(path_of("config"), "old", 0640);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenPreserveOr0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("config")), "new");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0640U);
}

TEST_P(FileSystemContract, WriteUnderPreserveOrGivesANewFileTheStatedMode) {
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenPreserveOr0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0600U);
}

// Under `refuse` an existing destination is left exactly as it was. The staged file takes the name with the
// platform's no-replace rename, which the portable-only engine does not have; a plain rename would replace the
// file and a look before it would leave the window open, so that engine answers `unsupported` before staging
// anything, and the file is just as untouched.
TEST_P(FileSystemContract, WriteUnderRefuseLeavesAnExistingFileAndIsAlreadyExists) {
    bc::file_system_test::write_raw(path_of("config"), "old", 0600);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("config"), bytes, kWrittenExactly0644Refusing);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, expected_under(GetParam(), ErrorKind::already_exists, ErrorKind::unsupported));
    EXPECT_EQ(achieved.error().path, path_of("config").string());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("config")), "old");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("config")), 0600U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("config"));
}

// `preserve_or` is there so a rewrite never widens a mode an operator tightened: the 0600 stays, whatever the
// caller would have given a new file.
TEST_P(FileSystemContract, WriteUnderPreserveOrKeepsAnOperatorsStricterModeAgainstTheCallers) {
    bc::file_system_test::write_raw(path_of("secret"), "old", 0600);
    const auto bytes = bytes_of("new");

    const auto achieved = files().write_file(in("secret"), bytes, kWrittenPreserveOr0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("secret")), "new");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("secret")), 0600U);
}

// The root is a directory, which a write refuses like any other, before anything is staged.
TEST_P(FileSystemContract, WriteOfTheRootIsNotRegularFile) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(Location::at(std::filesystem::path{"/"}), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_regular_file);
    EXPECT_FALSE(achieved.error().step.has_value());
}

// A leaf as long as a name may be leaves no room for the staged name's own parts, so the leaf inside the staged
// name is cut; the write still stages beside it and lands under the whole leaf.
TEST_P(FileSystemContract, WriteOfALeafAsLongAsANameMayBeStillStagesBesideIt) {
    const std::string leaf(255, 'n');
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in(leaf), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of(leaf)), "content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre(leaf));
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

TEST_P(FileSystemContract, WriteIntoADirectoryThatIsNotThereIsNotFound) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("missing/file"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
}

// Under `refuse` a symlink at the destination is refused and nothing moves; under `follow` the file behind
// it is the one replaced, and the link stays.
TEST_P(FileSystemContract, WriteRefusesADestinationThatIsASymlinkUnlessToldToFollow) {
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

// A link's target is spelled against the link's own directory, as the system spells it: a relative target
// through a subdirectory and a `..`, and a link to a link, each end at the file the system would open, and the
// write stages in that file's directory, never the link's.
TEST_P(FileSystemContract, WriteFollowsARelativeTargetAndAChainOfLinksToTheFileTheSystemWouldOpen) {
    bc::file_system_test::make_directory_raw(path_of("sub"), 0755);
    bc::file_system_test::make_directory_raw(path_of("elsewhere"), 0755);
    bc::file_system_test::write_raw(path_of("elsewhere/target.txt"), "behind two links", 0644);
    bc::file_system_test::make_symlink_raw("../elsewhere/target.txt", path_of("sub/inner"));
    bc::file_system_test::make_symlink_raw("sub/inner", path_of("outer"));
    const auto bytes = bytes_of("replacement");

    const auto achieved = files().write_file(in("outer"), bytes, kWrittenExactly0644Following);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("elsewhere/target.txt")), "replacement");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("elsewhere", "outer", "sub"));
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("sub")), ElementsAre("inner"));
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("elsewhere")), ElementsAre("target.txt"));
}

// A dangling link under `follow` is what an open with O_CREAT through it would be: the target is created, the
// link stays a link. Under `refuse` the link is refused like any other.
TEST_P(FileSystemContract, WriteThroughADanglingSymlinkCreatesTheTargetUnderFollow) {
    bc::file_system_test::make_symlink_raw("nowhere.txt", path_of("dangling"));
    const auto bytes = bytes_of("content");

    const auto refused = files().write_file(in("dangling"), bytes, kWrittenExactly0644);
    const auto followed = files().write_file(in("dangling"), bytes, kWrittenExactly0644Following);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::symlink_refused);
    ASSERT_TRUE(followed.has_value()) << followed.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("nowhere.txt")), "content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("dangling", "nowhere.txt"));
}

TEST_P(FileSystemContract, WriteRefusesADestinationThatIsADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    const auto bytes = bytes_of("replacement");

    const auto achieved = files().write_file(in("folder"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_regular_file);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("folder"));
}

TEST_P(FileSystemContract, WriteIntoADirectoryWithoutPermissionIsPermissionDenied) {
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
TEST_F(ObservedContract, WriteThatRunsOutOfSpaceLeavesTheDestinationWholeAndNoTemporaryFile) {
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
TEST_P(FileSystemContract, WriteAgainstAFileSizeLimitIsTooLargeAndLeavesTheDestinationWhole) {
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

TEST_F(ObservedContract, WriteWhoseRenameFailsLeavesTheDestinationWholeAndNoTemporaryFile) {
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

// The temporary file's creation is the first `openat` of a write after the walk's component opens: the one
// that creates it O_EXCL beside the destination.
TEST_F(ObservedContract, WriteWhoseTemporaryFileCannotBeCreatedReportsWhy) {
    const auto bytes = bytes_of("content");
    const auto staging_open = static_cast<std::uint32_t>(components_of(root_path()) + 1);
    script(fail_step(Operation::write_file, Step::openat, staging_open, EROFS));

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(achieved.error().system_code, EROFS);
    EXPECT_EQ(achieved.error().step, Step::openat);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

// A staged name that is taken (a leftover of a process that died with the same id) is tried again with the
// next token: one more `openat`, and the write lands.
TEST_F(ObservedContract, AStagedNameThatIsTakenIsTriedAgainWithTheNextToken) {
    const auto bytes = bytes_of("content");
    const auto staging_open = static_cast<std::uint32_t>(components_of(root_path()) + 1);
    script(fail_step(Operation::write_file, Step::openat, staging_open, EEXIST));

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_EQ(count(Operation::write_file, Step::openat), components_of(root_path()) + 2);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file"));
}

TEST_F(ObservedContract, WriteWhoseModeCannotBeSetLeavesTheDestinationWholeAndNoTemporaryFile) {
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
TEST_F(ObservedContract, DurableWriteWhoseFileSyncFailsLeavesTheDestinationWhole) {
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
TEST_F(ObservedContract, DurableWriteWhoseDirectorySyncFailsReportsWritten) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(fail_step(Operation::write_file, durable_sync_step(), 2, EIO));

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the new ledger");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
}

// A directory with mode 0300 (write and search, no read) takes a file: the walk opens the destination's directory
// for search only (O_SEARCH on Apple platforms, O_PATH on Linux), as the system itself treats a directory the
// process may enter but not read, so the file is staged and renamed into place; what the mode forbids is
// opening the directory read-only afterwards for its sync, which lowers the answer to `written`.
TEST_P(FileSystemContract, DurableWriteIntoADirectoryThatCannotBeOpenedForItsSyncReportsWritten) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process opens what its permission bits forbid";
    }
    bc::file_system_test::make_directory_raw(path_of("write-only"), 0300);
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("write-only/file"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(*achieved, Durability::written);
    bc::file_system_test::set_mode_raw(path_of("write-only"), 0700);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("write-only/file")), "content");
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("write-only")), ElementsAre("file"));
}

// What "durable" means, read from the system calls the write made: the temporary file is created, written,
// given its mode and synced; it is renamed over the destination; the directory is synced. On Apple platforms
// both syncs are F_FULLFSYNC, and fsync is never called, because fsync there stops at the drive's cache. That
// the file's sync comes before the rename is the case above, which fails it and finds no rename.
TEST_F(ObservedContract, DurableWriteSyncsTheFileThenRenamesThenSyncsTheDirectory) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    // The walk's component opens, the staged file's creation, and the directory opened read-only for its sync.
    EXPECT_EQ(count(Operation::write_file, Step::openat), components_of(root_path()) + 2);
    EXPECT_EQ(count(Operation::write_file, Step::write), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::fchmod), 1U);
    EXPECT_EQ(count(Operation::write_file, durable_sync_step()), 2U);
    // The staged file is closed as a step of its own before the rename: the last call that can report a write
    // the system deferred.
    EXPECT_EQ(count(Operation::write_file, Step::close), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::rename_no_replace), 0U);
    EXPECT_EQ(count(Operation::write_file, wrong_sync_step()), 0U);
}

// Under `refuse` the rename is the platform's no-replace rename, under its own step; a destination that is
// absent takes the name, and the step counts say which rename was made.
TEST_F(ObservedContract, WriteUnderRefuseRenamesWithTheNoReplaceRename) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("fresh"), bytes, kWrittenExactly0644Refusing);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("fresh")), "content");
    EXPECT_EQ(count(Operation::write_file, Step::rename_no_replace), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 0U);
}

// A staged file is never an ancestor of its destination, so EINVAL from its no-replace rename can only be a volume
// without RENAME_NOREPLACE (a Linux file system the probe cannot see): `unsupported` with the code kept, the
// destination never created and the staged file discarded, for a write and a copy alike. Scripted, since no
// volume of this suite's platforms lacks the flag.
TEST_F(ObservedContract, AStagedNoReplaceRenameRefusedWithEinvalIsUnsupportedWithTheCodeKept) {
    bc::file_system_test::write_raw(path_of("source"), "content", 0644);
    script(fail_step(Operation::write_file, Step::rename_no_replace, 1, EINVAL));
    script(fail_step(Operation::copy_file, Step::rename_no_replace, 1, EINVAL));

    const auto written = files().write_file(in("written"), bytes_of("content"), kWrittenExactly0644Refusing);
    const auto copied = files().copy_file(in("source"), in("copied"), kWrittenExactly0644Refusing);

    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().kind, ErrorKind::unsupported);
    EXPECT_EQ(written.error().step, Step::rename_no_replace);
    EXPECT_EQ(written.error().system_code, EINVAL);
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().kind, ErrorKind::unsupported);
    EXPECT_EQ(copied.error().step, Step::rename_no_replace);
    EXPECT_EQ(copied.error().system_code, EINVAL);
    EXPECT_EQ(copied.error().failed_location, FailedLocation::second);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("source"));
}

// A `written` write makes no sync and no reported close: the staged file closes through its owner.
TEST_F(ObservedContract, WrittenWriteClosesTheStagedFileThroughItsOwnerNotAsAStep) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(count(Operation::write_file, Step::close), 0U);
}

// A close that fails before the rename is an error like a failed sync: the destination is untouched and the
// staged file is gone.
TEST_F(ObservedContract, DurableWriteWhoseCloseFailsLeavesTheDestinationWhole) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(fail_step(Operation::write_file, Step::close, 1, EIO));

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(achieved.error().step, Step::close);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 0U);
}

TEST_F(ObservedContract, WrittenWriteMakesNoSyncCall) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("ledger"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(count(Operation::write_file, Step::fsync), 0U);
    EXPECT_EQ(count(Operation::write_file, Step::fcntl_fullfsync), 0U);
}

TEST_F(ObservedContract, AnInterruptedWriteIsAskedAgain) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, Step::write, 1, EINTR));

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_EQ(count(Operation::write_file, Step::write), 2U);
}

TEST_F(ObservedContract, AnInterruptedSyncIsAskedAgain) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, durable_sync_step(), 1, EINTR));

    const auto achieved = files().write_file(in("file"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    // The interrupted file sync, the file sync again, and the directory sync.
    EXPECT_EQ(count(Operation::write_file, durable_sync_step()), 3U);
}

// A file system with no sync call at all (some network mounts) cannot do better than written, and says so.
TEST_F(ObservedContract, DurableWriteWhereSyncIsNotSupportedReportsWritten) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, durable_sync_step(), 1, ENOTSUP));

    const auto achieved = files().write_file(in("file"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
}

// EDQUOT is a full quota, `no_space`; EFBIG is the process's file-size limit, `too_large`.
TEST_F(ObservedContract, EachRoomFailureHasItsKind) {
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

TEST_P(FileSystemContract, AppendCreatesTheFileWithItsMode) {
    const auto bytes = bytes_of("first entry\n");

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0640));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\n");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("log")), 0640U);
}

TEST_P(FileSystemContract, AppendAddsToTheEnd) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("second entry\n");

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\nsecond entry\n");
}

// Two appends through the library, the first creating the file: the bytes arrive in call order, whole.
TEST_P(FileSystemContract, AppendAcrossTwoCallsConcatenatesInOrder) {
    const auto first = bytes_of("one\n");
    const auto second = bytes_of("two\n");

    const auto first_achieved = files().append(in("log"), first, Durability::written, Permissions::exactly(0644));
    const auto second_achieved = files().append(in("log"), second, Durability::written, Permissions::exactly(0644));

    ASSERT_TRUE(first_achieved.has_value()) << first_achieved.error().message();
    ASSERT_TRUE(second_achieved.has_value()) << second_achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "one\ntwo\n");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("log"));
}

// The root is a directory, which an append refuses before opening anything.
TEST_P(FileSystemContract, AppendToTheRootIsNotRegularFile) {
    const auto bytes = bytes_of("entry");

    const auto achieved = files().append(Location::at(std::filesystem::path{"/"}), bytes, Durability::written,
                                         Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_regular_file);
}

// An existing file is neither created nor replaced by an append, so its mode is not this call's to change.
TEST_P(FileSystemContract, AppendLeavesAnExistingFilesModeAloneUnderEitherPolicy) {
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
TEST_P(FileSystemContract, AppendRefusesASymlinkAndADirectory) {
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

TEST_P(FileSystemContract, AppendIntoADirectoryThatIsNotThereIsNotFound) {
    const auto bytes = bytes_of("entry");

    const auto achieved = files().append(in("missing/log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
}

TEST_F(ObservedContract, AppendThatRunsOutOfSpaceIsNoSpace) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("second entry\n");
    script(fail_step(Operation::append, Step::write, 1, ENOSPC));

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::no_space);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\n");
}

TEST_F(ObservedContract, AppendWhoseNewFileCannotGetItsModeTakesTheFileBack) {
    const auto bytes = bytes_of("entry");
    script(fail_step(Operation::append, Step::fchmod, 1, EPERM));

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::permission_denied);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("log")));
}

TEST_F(ObservedContract, DurableAppendToANewFileSyncsTheFileAndItsDirectory) {
    const auto bytes = bytes_of("entry");

    const auto achieved = files().append(in("log"), bytes, Durability::durable, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::append, durable_sync_step()), 2U);
    EXPECT_EQ(count(Operation::append, wrong_sync_step()), 0U);
}

TEST_F(ObservedContract, DurableAppendToAnExistingFileSyncsOnlyTheFile) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("entry");

    const auto achieved = files().append(in("log"), bytes, Durability::durable, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::append, durable_sync_step()), 1U);
}

// The bytes are already in the file when the sync fails, so the call reports what it achieved, not an error.
TEST_F(ObservedContract, DurableAppendWhoseSyncFailsReportsWritten) {
    bc::file_system_test::write_raw(path_of("log"), "first entry\n", 0644);
    const auto bytes = bytes_of("second entry\n");
    script(fail_step(Operation::append, durable_sync_step(), 1, EIO));

    const auto achieved = files().append(in("log"), bytes, Durability::durable, Permissions::exactly(0644));

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "first entry\nsecond entry\n");
}

// "Stop without cleanup" on an append that created its file: the kill lands between the create and the mode,
// so the empty file stays, as it would after a real kill there, and the error names the step.
TEST_F(ObservedContract, AStopAtANewAppendedFilesModeLeavesTheFileAsAKillWould) {
    const auto bytes = bytes_of("entry");
    script(stop_at(Operation::append, Step::fchmod, 1));

    const auto achieved = files().append(in("log"), bytes, Durability::written, Permissions::exactly(0644));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(achieved.error().step, Step::fchmod);
    EXPECT_EQ(achieved.error().system_code, 0);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("log")), "");
}

// -------------------------------------------------------------------------------------------------------------
// sync
// -------------------------------------------------------------------------------------------------------------

// A file and a directory each open read-only and sync; on every platform the suite runs on the sync is offered,
// so the answer is `durable`.
TEST_P(FileSystemContract, SyncOfAFileAndOfADirectoryAnswersDurable) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto file = files().sync(in("file"));
    const auto folder = files().sync(in("folder"));
    const auto own_directory = files().sync(root());

    ASSERT_TRUE(file.has_value()) << file.error().message();
    EXPECT_EQ(*file, Durability::durable);
    ASSERT_TRUE(folder.has_value()) << folder.error().message();
    EXPECT_EQ(*folder, Durability::durable);
    ASSERT_TRUE(own_directory.has_value()) << own_directory.error().message();
    EXPECT_EQ(*own_directory, Durability::durable);
}

// A sync takes no choice about a symlink, so one is refused; nothing there is `not_found`.
TEST_P(FileSystemContract, SyncOfNothingIsNotFoundAndOfASymlinkIsRefused) {
    bc::file_system_test::write_raw(path_of("target.txt"), "content", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto absent = files().sync(in("absent"));
    const auto through_link = files().sync(in("link"));

    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().kind, ErrorKind::not_found);
    ASSERT_FALSE(through_link.has_value());
    EXPECT_EQ(through_link.error().kind, ErrorKind::symlink_refused);
}

// What a sync costs: the leaf's open and one sync under this platform's step, never the other platform's.
TEST_F(ObservedContract, SyncOpensTheLeafAndMakesOneSyncCallUnderThePlatformsStep) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto achieved = files().sync(in("file"));

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(count(Operation::sync, Step::openat), components_of(root_path()) + 1);
    EXPECT_EQ(count(Operation::sync, durable_sync_step()), 1U);
    EXPECT_EQ(count(Operation::sync, wrong_sync_step()), 0U);
}

// A volume that offers no sync cannot do better than `written`, and says so.
TEST_F(ObservedContract, SyncWhereTheVolumeOffersNoneAnswersWritten) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    script(fail_step(Operation::sync, durable_sync_step(), 1, ENOTSUP));

    const auto achieved = files().sync(in("file"));

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(*achieved, Durability::written);
}

// A sync of its own that fails is an error: this call changes nothing, so the rule that lowers a failed sync
// to `written` because the change happened has nothing to apply to, and the caller asked for exactly this.
TEST_F(ObservedContract, SyncThatFailsIsIoFailedAtThePlatformsStep) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    script(fail_step(Operation::sync, durable_sync_step(), 1, EIO));

    const auto achieved = files().sync(in("file"));

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(achieved.error().system_code, EIO);
    EXPECT_EQ(achieved.error().step, durable_sync_step());
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

TEST_P(FileSystemContract, ListThisDirectoryReturnsItsEntriesSortedEachWithItsType) {
    lay_out_a_small_tree(root_path());

    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    EXPECT_THAT(*entries, ElementsAre(Entry{.entry_type = EntryType::regular_file, .name = "a.txt"},
                                      Entry{.entry_type = EntryType::symlink, .name = "link"},
                                      Entry{.entry_type = EntryType::directory, .name = "sub"}));
}

// A listing is one level deep; a tree is read by listing each directory in turn.
TEST_P(FileSystemContract, ListOfEachDirectoryInATreeReturnsItsOwnEntries) {
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

TEST_P(FileSystemContract, ListListsASymlinkToADirectoryAsASymlinkAndNeverFollowsIt) {
    bc::file_system_test::make_directory_raw(path_of("outside"), 0755);
    bc::file_system_test::write_raw(path_of("outside/private.txt"), "not part of the tree", 0644);
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::make_symlink_raw(path_of("outside"), path_of("tree/way-out"));

    const auto entries = files().list_directory(in("tree"), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    EXPECT_THAT(*entries, ElementsAre(Entry{.entry_type = EntryType::symlink, .name = "way-out"}));
}

TEST_P(FileSystemContract, ListFollowsASymlinkGivenAsTheDirectoryItselfOnlyWhenToldTo) {
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

TEST_P(FileSystemContract, ListOfAnEmptyDirectoryIsEmpty) {
    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value());
    EXPECT_THAT(*entries, IsEmpty());
}

TEST_P(FileSystemContract, ListOfNothingIsNotFound) {
    const auto entries = files().list_directory(in("absent"), FinalSymlink::refuse);

    ASSERT_FALSE(entries.has_value());
    EXPECT_EQ(entries.error().kind, ErrorKind::not_found);
}

TEST_P(FileSystemContract, ListOfAFileIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto entries = files().list_directory(in("file"), FinalSymlink::refuse);

    ASSERT_FALSE(entries.has_value());
    EXPECT_EQ(entries.error().kind, ErrorKind::not_directory);
}

TEST_P(FileSystemContract, ListOfADirectoryWithoutPermissionIsPermissionDenied) {
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
TEST_P(FileSystemContract, ListOfADirectoryThatCannotBeReadFailsWhileItsParentListsIt) {
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
TEST_P(FileSystemContract, ListReturnsMoreEntriesThanTheFirstArrayHolds) {
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

// The root of the file system lists like any directory: the walk opens it for search, the listing opens `.`
// beneath it read-only.
TEST_P(FileSystemContract, ListOfTheRootOfTheFileSystemIsNotEmpty) {
    const auto entries = files().list_directory(Location::at(std::filesystem::path{"/"}), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value()) << entries.error().message();
    EXPECT_FALSE(entries->empty());
}

// An entry whose type the stream does not say (DT_UNKNOWN) is asked with fstatat, never followed. APFS, ext4
// and tmpfs all say the type, so that branch cannot be provoked on any platform the suite runs on and is read
// against the rule rather than faked; the types a listing reports are held by the sorted-listing case above
// whichever way they came.

// What a listing costs: the leaf opened once, read-only, one fdopendir, and every entry read twice, since the
// entries are counted before any is stored (so an array too small stores nothing) and then stored; `.`, `..`
// and the end of the stream are read each time too.
TEST_F(ObservedContract, ListCountsTheEntriesThenStoresThemAndReadsEveryEntryTwice) {
    lay_out_a_small_tree(root_path());

    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_TRUE(entries.has_value()) << entries.error().message();
    EXPECT_EQ(entries->size(), 3U);
    // The walk's component opens below the root, and the listed directory itself.
    EXPECT_EQ(count(Operation::list_directory, Step::openat), components_of(root_path()));
    EXPECT_EQ(count(Operation::list_directory, Step::fdopendir), 1U);
    // Three entries, `.` and `..`, and the end of the stream: six reads, twice.
    EXPECT_EQ(count(Operation::list_directory, Step::readdir), 12U);
}

// A read of the stream that fails discards everything read before it: a listing is complete or an error.
TEST_F(ObservedContract, AReadOfTheStreamThatFailsIsTheListingsErrorAndNothingIsAnswered) {
    lay_out_a_small_tree(root_path());
    script(fail_step(Operation::list_directory, Step::readdir, 2, EIO));

    const auto entries = files().list_directory(root(), FinalSymlink::refuse);

    ASSERT_FALSE(entries.has_value());
    EXPECT_EQ(entries.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(entries.error().step, Step::readdir);
    EXPECT_EQ(entries.error().system_code, EIO);
    EXPECT_EQ(entries.error().path, root_path().string());
}

// -------------------------------------------------------------------------------------------------------------
// status
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, StatusOfNothingIsAnEmptyValueNotAnError) {
    const auto status = files().status(in("absent"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(status->has_value());
}

TEST_P(FileSystemContract, StatusBelowAFileIsAnEmptyValue) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto status = files().status(in("file/child"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value());
    EXPECT_FALSE(status->has_value());
}

TEST_P(FileSystemContract, StatusReportsAFile) {
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

TEST_P(FileSystemContract, StatusReportsADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0750);

    const auto status = files().status(in("folder"), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value());
    ASSERT_TRUE(status->has_value());
    EXPECT_EQ((*status)->entry_type, EntryType::directory);
    EXPECT_EQ((*status)->size, 0);
    EXPECT_EQ((*status)->mode, 0750U);
}

// The root has no parent entry to ask about; it is described from the directory itself.
TEST_P(FileSystemContract, StatusOfTheRootIsADirectory) {
    const auto status = files().status(Location::at(std::filesystem::path{"/"}), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value()) << status.error().message();
    ASSERT_TRUE(status->has_value());
    EXPECT_EQ((*status)->entry_type, EntryType::directory);
    EXPECT_EQ((*status)->size, 0);
    EXPECT_GE((*status)->link_count, 1U);
    EXPECT_EQ((Identity{.device = (*status)->device, .inode = (*status)->inode}),
              bc::file_system_test::identity_raw(std::filesystem::path{"/"}));
}

// Below a missing directory, as below a file, nothing is at the location.
TEST_P(FileSystemContract, StatusBelowAMissingDirectoryIsAnEmptyValue) {
    const auto status = files().status(in("missing/child"), FinalSymlink::follow);

    ASSERT_TRUE(status.has_value()) << status.error().message();
    EXPECT_FALSE(status->has_value());
}

// Absence is an answer; a refusal on the way is not: a directory that cannot be entered is an error.
TEST_P(FileSystemContract, StatusBelowADirectoryWithoutPermissionIsPermissionDenied) {
    if (!bc::file_system_test::permissions_are_enforced(root_path())) {
        GTEST_SKIP() << "this process reads what its permission bits forbid";
    }
    bc::file_system_test::make_directory_raw(path_of("locked"), 0000);

    const auto status = files().status(in("locked/child"), FinalSymlink::refuse);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::permission_denied);
}

// Under `refuse` a symlink is reported as a symlink whatever it points at; under `follow` the report is of
// what it points at, and a dangling link is absence.
TEST_P(FileSystemContract, StatusReportsASymlinkAsASymlinkWhateverItPointsAt) {
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
// status and reads, observed: what the two operations cost and how a scripted step reaches them
// -------------------------------------------------------------------------------------------------------------

// What a read costs beyond resolving its location: the leaf is opened once, measured once, read once, and asked
// once more past its measured size to be sure it did not grow in between.
TEST_F(ObservedContract, ReadFileOpensTheLeafMeasuresItReadsItAndProbesOnceForMore) {
    bc::file_system_test::write_raw(path_of("notes.txt"), "seven b", 0644);

    const auto bytes = files().read_file(in("notes.txt"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
    const std::uint64_t components = components_of(root_path());
    EXPECT_EQ(count(Operation::read_file, Step::open), 1U);
    EXPECT_EQ(count(Operation::read_file, Step::openat), components + 1);
    EXPECT_EQ(count(Operation::read_file, Step::fstat), components + 1);
    EXPECT_EQ(count(Operation::read_file, Step::pread), 2U);
}

// An empty file is one probe and no other read; its status is one stat of the leaf after the walk.
TEST_F(ObservedContract, AnEmptyFileIsProbedOnceAndStatusStatsTheLeafOnce) {
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);

    const auto bytes = files().read_file(in("empty"), kWholeFileRefusingSymlink);
    const auto status = files().status(in("empty"), FinalSymlink::refuse);

    ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
    EXPECT_THAT(*bytes, IsEmpty());
    ASSERT_TRUE(status.has_value()) << status.error().message();
    EXPECT_EQ(count(Operation::read_file, Step::pread), 1U);
    EXPECT_EQ(count(Operation::status, Step::fstatat), components_of(root_path()) + 1);
    EXPECT_EQ(count(Operation::status, Step::openat), components_of(root_path()));
}

// A read interrupted by a signal is asked again, and the answer is whole.
TEST_F(ObservedContract, AnInterruptedReadIsAskedAgainAndTheAnswerIsWhole) {
    bc::file_system_test::write_raw(path_of("notes.txt"), "first line\nsecond line\n", 0644);
    script(fail_step(Operation::read_file, Step::pread, 1, EINTR));

    const auto bytes = files().read_file(in("notes.txt"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(bytes.has_value()) << bytes.error().message();
    EXPECT_EQ(text_of(*bytes), "first line\nsecond line\n");
    EXPECT_EQ(count(Operation::read_file, Step::pread), 3U);
}

// A read the system fails is the operation's error, at that step, with no bytes claimed.
TEST_F(ObservedContract, AFailedReadIsIoFailedAtPreadAndClaimsNoBytes) {
    bc::file_system_test::write_raw(path_of("notes.txt"), "content", 0644);
    script(fail_step(Operation::read_file, Step::pread, 1, EIO));

    const auto bytes = files().read_file(in("notes.txt"), kWholeFileRefusingSymlink);

    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(bytes.error().system_code, EIO);
    EXPECT_EQ(bytes.error().step, Step::pread);
    EXPECT_EQ(bytes.error().operation, Operation::read_file);
    EXPECT_EQ(bytes.error().path, path_of("notes.txt").string());
    EXPECT_EQ(count(Operation::read_file, Step::pread), 1U);
}

// The leaf's stat answering ENOENT is absence, whatever is really there. Beneath a held directory with one
// component nothing is walked, so the first stat `status` makes is the leaf's.
TEST_F(ObservedContract, AScriptedAbsenceOnTheLeafsStatIsAbsence) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();
    script(fail_step(Operation::status, Step::fstatat, 1, ENOENT));

    const auto status = files().status(Location::beneath(*parent, relative("file")), FinalSymlink::refuse);

    ASSERT_TRUE(status.has_value()) << status.error().message();
    EXPECT_FALSE(status->has_value());
    EXPECT_EQ(count(Operation::status, Step::fstatat), 1U);
}

// Any other refusal of the leaf's stat is the operation's error at that step.
TEST_F(ObservedContract, AScriptedRefusalOnTheLeafsStatIsTheOperationsError) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();
    script(fail_step(Operation::status, Step::fstatat, 1, EIO));

    const auto status = files().status(Location::beneath(*parent, relative("file")), FinalSymlink::refuse);

    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(status.error().step, Step::fstatat);
    EXPECT_EQ(status.error().path, path_of("file").string());
}

// Beneath a held directory the read is contained like every other operation: a symlink on the way is refused
// and a file inside reads whole.
TEST_P(FileSystemContract, ReadFileBeneathADirectoryReadsAFileAndRefusesASymlinkOnTheWay) {
    lay_out_directories(root_path());
    bc::file_system_test::write_raw(path_of("sub/deeper/notes.txt"), "beneath", 0644);
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();

    const auto inside =
        files().read_file(Location::beneath(*parent, relative("sub/deeper/notes.txt")), kWholeFileRefusingSymlink);
    const auto through_a_link =
        files().read_file(Location::beneath(*parent, relative("alias/deeper/notes.txt")), kWholeFileRefusingSymlink);

    ASSERT_TRUE(inside.has_value()) << inside.error().message();
    EXPECT_EQ(text_of(*inside), "beneath");
    ASSERT_FALSE(through_a_link.has_value());
    EXPECT_EQ(through_a_link.error().kind, ErrorKind::symlink_refused);
    EXPECT_EQ(through_a_link.error().path, path_of("alias/deeper/notes.txt").string());
}

// A `beneath` write stages beside its leaf inside the held directory, and a symlink on the way is refused like
// every `beneath` resolution's, with nothing staged anywhere.
TEST_P(FileSystemContract, WriteBeneathADirectoryStagesBesideTheLeafAndRefusesASymlinkOnTheWay) {
    lay_out_directories(root_path());
    const auto bytes = bytes_of("content");
    const auto held = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(held.has_value()) << held.error().message();

    const auto written =
        files().write_file(Location::beneath(*held, relative("sub/deeper/file")), bytes, kWrittenExactly0644);
    const auto through_link =
        files().write_file(Location::beneath(*held, relative("alias/file")), bytes, kWrittenExactly0644);

    ASSERT_TRUE(written.has_value()) << written.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("sub/deeper/file")), "content");
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("sub/deeper")), ElementsAre("file"));
    ASSERT_FALSE(through_link.has_value());
    EXPECT_EQ(through_link.error().kind, ErrorKind::symlink_refused);
    EXPECT_EQ(through_link.error().path, path_of("alias/file").string());
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("sub")), ElementsAre("deeper", "way-out"));
}

// -------------------------------------------------------------------------------------------------------------
// create_directory
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, CreateDirectoryMakesOneDirectoryWithTheModeExactly) {
    const auto created = files().create_directory(in("folder"), Permissions::exactly(0750));

    ASSERT_TRUE(created.has_value()) << created.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("folder")), 0750U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("folder"));
}

// The name is taken, by anything: a creation has no final symlink to follow or refuse, and what holds the name
// is left exactly as it was.
TEST_P(FileSystemContract, CreateDirectoryOfANameThatIsTakenIsAlreadyExistsWhateverHoldsIt) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_symlink_raw(path_of("folder"), path_of("link"));

    for (const std::string_view name : {"folder", "file", "link"}) {
        const auto created = files().create_directory(in(name), Permissions::exactly(0700));
        ASSERT_FALSE(created.has_value()) << name;
        EXPECT_EQ(created.error().kind, ErrorKind::already_exists) << name;
        EXPECT_EQ(created.error().step, Step::mkdirat) << name;
        EXPECT_EQ(created.error().system_code, EEXIST) << name;
    }
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("folder")), 0755U);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file", "folder", "link"));
}

TEST_P(FileSystemContract, CreateDirectoryBelowAMissingParentIsNotFoundAndBelowAFileIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto below_nothing = files().create_directory(in("missing/child"), Permissions::exactly(0755));
    const auto below_the_file = files().create_directory(in("file/child"), Permissions::exactly(0755));

    ASSERT_FALSE(below_nothing.has_value());
    EXPECT_EQ(below_nothing.error().kind, ErrorKind::not_found);
    ASSERT_FALSE(below_the_file.has_value());
    EXPECT_EQ(below_the_file.error().kind, ErrorKind::not_directory);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file"));
}

TEST_P(FileSystemContract, CreateDirectoryOfTheRootOfTheFileSystemIsAlreadyExists) {
    const auto created = files().create_directory(Location::at(std::filesystem::path{"/"}), Permissions::exactly(0755));

    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind, ErrorKind::already_exists);
}

// The umask is ambient state the library never consults: a directory is made with a private mode and then
// given the stated one exactly, so a restrictive umask changes nothing. The umask is process-wide, so the guard
// sets it and restores it inside this one case, and the suite runs one case at a time.
TEST_P(FileSystemContract, CreateDirectoryAndCreateDirectoriesGiveTheModeExactlyUnderARestrictiveUmask) {
    const bc::file_system_test::UmaskGuard umask{0077};

    const auto one = files().create_directory(in("one"), Permissions::exactly(0755));
    const auto chain = files().create_directories(in("a/b/c"), Permissions::exactly(0775));

    ASSERT_TRUE(one.has_value()) << one.error().message();
    ASSERT_TRUE(chain.has_value()) << chain.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("one")), 0755U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a")), 0775U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b")), 0775U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b/c")), 0775U);
}

// A directory whose mode cannot be set is taken back: a failure never leaves a directory with a mode nobody
// chose. A stop there keeps it, as a kill would.
TEST_F(ObservedContract, CreateDirectoryWhoseModeCannotBeSetTakesTheDirectoryBackAndAStopKeepsIt) {
    script(fail_step(Operation::create_directory, Step::fchmodat, 1, EIO));
    const auto failed = files().create_directory(in("failed"), Permissions::exactly(0755));
    script(stop_at(Operation::create_directory, Step::fchmodat, 2));
    const auto stopped = files().create_directory(in("stopped"), Permissions::exactly(0755));

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(failed.error().step, Step::fchmodat);
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error().system_code, 0);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("stopped"));
}

// -------------------------------------------------------------------------------------------------------------
// create_directories
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, CreateDirectoriesMakesEveryMissingDirectoryWithTheMode) {
    const auto created = files().create_directories(in("a/b/c"), Permissions::exactly(0750));

    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a")), 0750U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b")), 0750U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b/c")), 0750U);
}

TEST_P(FileSystemContract, CreateDirectoriesLeavesWhatExistsAsItIsUnderEitherPolicy) {
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

// A trailing separator is not one spelling of a path (the path module's rule since stage 10): the location is
// refused at the door, before any system call, and nothing is made.
TEST_P(FileSystemContract, CreateDirectoriesRefusesATrailingSeparatorAtTheDoor) {
    const auto created = files().create_directories(in("a/b/"), Permissions::exactly(0755));

    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().kind, ErrorKind::invalid_argument);
    EXPECT_FALSE(created.error().step.has_value());
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("a")));
}

TEST_P(FileSystemContract, CreateDirectoriesWhereAFileIsIsNotDirectory) {
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
TEST_P(FileSystemContract, CreateDirectoriesAcceptsASymlinkToADirectoryOnTheWay) {
    bc::file_system_test::make_directory_raw(path_of("real"), 0755);
    bc::file_system_test::make_symlink_raw(path_of("real"), path_of("alias"));

    const auto created = files().create_directories(in("alias/child"), Permissions::exactly(0755));

    ASSERT_TRUE(created.has_value());
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("real/child")));
}

// Beneath a held directory the chain may hold no symlink: a real directory on the way is entered and a symlink
// to one is refused, as every `beneath` resolution refuses it, with nothing made beyond it.
TEST_P(FileSystemContract, CreateDirectoriesBeneathEntersARealDirectoryAndRefusesASymlinkOnTheWay) {
    bc::file_system_test::make_directory_raw(path_of("real"), 0755);
    bc::file_system_test::make_symlink_raw(path_of("real"), path_of("alias"));
    const auto held = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(held.has_value()) << held.error().message();

    const auto inside = files().create_directories(Location::beneath(*held, relative("real/child/grandchild")),
                                                   Permissions::exactly(0755));
    const auto through_a_link =
        files().create_directories(Location::beneath(*held, relative("alias/other")), Permissions::exactly(0755));

    ASSERT_TRUE(inside.has_value()) << inside.error().message();
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("real/child/grandchild")));
    ASSERT_FALSE(through_a_link.has_value());
    EXPECT_EQ(through_a_link.error().kind, ErrorKind::symlink_refused);
    EXPECT_EQ(through_a_link.error().path, path_of("alias/other").string());
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("real/other")));
}

// The root of the file system is there already; so is a chain that exists in full.
TEST_P(FileSystemContract, CreateDirectoriesOfTheRootAndOfAnExistingChainMakesNothing) {
    bc::file_system_test::make_directory_raw(path_of("a"), 0700);
    bc::file_system_test::make_directory_raw(path_of("a/b"), 0700);

    const auto root_of_all =
        files().create_directories(Location::at(std::filesystem::path{"/"}), Permissions::exactly(0755));
    const auto existing = files().create_directories(in("a/b"), Permissions::exactly(0755));

    ASSERT_TRUE(root_of_all.has_value()) << root_of_all.error().message();
    ASSERT_TRUE(existing.has_value()) << existing.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("a/b")), 0700U);
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

TEST_P(FileSystemContract, RemoveFileRemovesAFileAndSaysThereWasOne) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto removed = files().remove_file(in("file"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("file")));
}

TEST_P(FileSystemContract, RemoveFileOfNothingSaysThereWasNone) {
    const auto removed = files().remove_file(in("absent"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(*removed);
}

TEST_P(FileSystemContract, RemoveFileRemovesASymlinkAndNotWhatItPointsAt) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto removed = files().remove_file(in("link"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("link")));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("target.txt")), "behind the link");
}

TEST_P(FileSystemContract, RemoveFileRefusesADirectory) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto removed = files().remove_file(in("folder"));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::not_regular_file);
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("folder")));
}

// -------------------------------------------------------------------------------------------------------------
// remove_directory
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, RemoveDirectoryRemovesAnEmptyDirectoryAndSaysThereWasOne) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto removed = files().remove_directory(in("folder"));

    ASSERT_TRUE(removed.has_value()) << removed.error().message();
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("folder")));
}

TEST_P(FileSystemContract, RemoveDirectoryOfNothingSaysThereWasNone) {
    const auto removed = files().remove_directory(in("absent"));
    const auto below_nothing = files().remove_directory(in("absent/child"));

    ASSERT_TRUE(removed.has_value()) << removed.error().message();
    EXPECT_FALSE(*removed);
    ASSERT_TRUE(below_nothing.has_value()) << below_nothing.error().message();
    EXPECT_FALSE(*below_nothing);
}

TEST_P(FileSystemContract, RemoveDirectoryOfOneWithEntriesIsDirectoryNotEmptyAndRemovesNothing) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::write_raw(path_of("folder/file"), "content", 0644);

    const auto removed = files().remove_directory(in("folder"));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::directory_not_empty);
    EXPECT_EQ(removed.error().step, Step::unlinkat);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("folder/file")), "content");
}

TEST_P(FileSystemContract, RemoveDirectoryOfAFileOrASymlinkToADirectoryIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::make_symlink_raw(path_of("folder"), path_of("link"));

    const auto a_file = files().remove_directory(in("file"));
    const auto a_link = files().remove_directory(in("link"));

    ASSERT_FALSE(a_file.has_value());
    EXPECT_EQ(a_file.error().kind, ErrorKind::not_directory);
    ASSERT_FALSE(a_link.has_value());
    EXPECT_EQ(a_link.error().kind, ErrorKind::not_directory);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file", "folder", "link"));
}

// -------------------------------------------------------------------------------------------------------------
// remove_tree
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, RemoveTreeRemovesEverythingAndSaysThereWasOne) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    lay_out_a_small_tree(path_of("tree"));

    const auto removed = files().remove_tree(in("tree"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("tree")));
}

TEST_P(FileSystemContract, RemoveTreeOfNothingRemovesNothing) {
    const auto removed = files().remove_tree(in("absent"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_FALSE(*removed);
}

TEST_P(FileSystemContract, RemoveTreeOfAFileRemovesTheFile) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto removed = files().remove_tree(in("file"));

    ASSERT_TRUE(removed.has_value());
    EXPECT_TRUE(*removed);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("file")));
}

TEST_P(FileSystemContract, RemoveTreeRemovesSymlinksAndNeverWhatTheyPointAt) {
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

// A tree wider than its window at every level: every file and every directory goes, in one call.
TEST_P(FileSystemContract, RemoveTreeOfAWideTreeRemovesEveryEntry) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    for (int branch = 0; branch < 24; ++branch) {
        const std::string name = "branch-" + std::to_string(branch);
        bc::file_system_test::make_directory_raw(path_of("tree/" + name), 0755);
        for (int leaf = 0; leaf < 24; ++leaf) {
            bc::file_system_test::write_raw(path_of("tree/" + name + "/leaf-" + std::to_string(leaf)), "", 0644);
        }
        bc::file_system_test::make_directory_raw(path_of("tree/" + name + "/empty"), 0700);
        bc::file_system_test::make_symlink_raw(path_of("tree"), path_of("tree/" + name + "/back"));
    }

    const auto removed = files().remove_tree(in("tree"));

    ASSERT_TRUE(removed.has_value()) << removed.error().message();
    EXPECT_TRUE(*removed);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

// The removal walks on held descriptors with no recursion and a window of sixteen levels: a chain deeper than
// the window is cleared in passes that descend from the top again. The proof that nothing more is held is a
// descriptor limit lowered to what the suite holds now, the window and a handful: an operation that opened
// more would fail with EMFILE. The chain is planted on held descriptors, as the resolve test plants its own.
// The limit in force is read first, and where it is below what the planting itself needs the case skips, saying
// so, rather than failing for the runner's limit.
TEST_P(FileSystemContract, RemoveTreeClearsAChainDeeperThanItsWindowUnderALoweredDescriptorLimit) {
    constexpr std::uint32_t depth = 300;
    constexpr std::uint64_t window = 16;
    // The walk's parent, the top's stream, a child being entered, the root opened for the identity check, and
    // slack for the test framework's own needs.
    constexpr std::uint64_t allowance = 8;
    const std::uint64_t held_now = bc::file_system_test::open_descriptors().size();
    const std::uint64_t needed = held_now + window + allowance;
    if (bc::file_system_test::descriptor_soft_limit() <= needed) {
        GTEST_SKIP() << "the soft descriptor limit, " << bc::file_system_test::descriptor_soft_limit()
                     << ", is not above the " << needed << " this case needs";
    }
    bc::file_system_test::make_directory_raw(path_of("chain"), 0755);
    bc::file_system_test::make_deep_chain_raw(path_of("chain"), depth, "d");
    ASSERT_TRUE(bc::file_system_test::exists_raw(path_of("chain/d/d/d/d")));

    {
        const bc::file_system_test::DescriptorLimit limit{needed};
        const auto removed = files().remove_tree(in("chain"));
        ASSERT_TRUE(removed.has_value()) << removed.error().message();
        EXPECT_TRUE(*removed);
    }

    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("chain")));
}

TEST_P(FileSystemContract, RemoveTreeBeneathADirectoryRemovesTheTreeAndRefusesASymlinkOnTheWay) {
    lay_out_directories(root_path());
    bc::file_system_test::write_raw(path_of("sub/deeper/file"), "content", 0644);
    const auto held = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(held.has_value()) << held.error().message();

    const auto through_a_link = files().remove_tree(Location::beneath(*held, relative("alias/deeper")));
    const auto inside = files().remove_tree(Location::beneath(*held, relative("sub/deeper")));

    ASSERT_FALSE(through_a_link.has_value());
    EXPECT_EQ(through_a_link.error().kind, ErrorKind::symlink_refused);
    ASSERT_TRUE(inside.has_value()) << inside.error().message();
    EXPECT_TRUE(*inside);
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("sub")), ElementsAre("way-out"));
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("outside")));
}

// The error of a removal names the entry it failed on, relative to the location, which the face composes into
// the path; what was removed before it stays removed.
TEST_F(ObservedContract, RemoveTreeNamesTheEntryItCouldNotRemoveAndKeepsWhatItRemoved) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::make_directory_raw(path_of("tree/a"), 0755);
    bc::file_system_test::make_directory_raw(path_of("tree/a/b"), 0755);
    bc::file_system_test::write_raw(path_of("tree/a/b/kept"), "content", 0644);
    bc::file_system_test::write_raw(path_of("tree/a/b/gone"), "content", 0644);
    // The two files are unlinked in the order the stream answers them: the second is the one refused.
    script(fail_step(Operation::remove_tree, Step::unlinkat, 2, EACCES));

    const auto removed = files().remove_tree(in("tree"));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(removed.error().step, Step::unlinkat);
    EXPECT_FALSE(removed.error().name_truncated);
    const std::vector<std::string> remaining = bc::file_system_test::names_raw(path_of("tree/a/b"));
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(removed.error().path, path_of("tree/a/b/" + remaining.front()).string());
}

// A mount point inside the tree is where the removal stops, with `cross_device` and what it removed before it
// kept. A mount is nothing a test can plant without privileges, and a scripted fault can fail a step or stop it
// but never change what fstatat answers, so the case waits for either a scripted stat answer in the observer or
// a mount the suite may make (a loop mount on Linux, under a privileged leg).
TEST_F(ObservedContract, DISABLED_RemoveTreeStopsAtAMountPointInsideTheTreeWithCrossDevice) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::make_directory_raw(path_of("tree/mount"), 0755);
    // (the mount of another device at tree/mount would go here)

    const auto removed = files().remove_tree(in("tree"));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::cross_device);
    EXPECT_EQ(removed.error().path, path_of("tree/mount").string());
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("tree")));
}

// The kill points of remove_tree: a stop at any of its unlinkat calls leaves the descriptors closed and a tree
// that a second call finishes, whatever was removed before the stop.
TEST_F(ObservedContract, RemoveTreeStoppedAtAnyUnlinkatLeavesATreeASecondCallFinishes) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    lay_out_a_small_tree(path_of("tree"));
    const auto whole = files().remove_tree(in("tree"));
    ASSERT_TRUE(whole.has_value()) << whole.error().message();
    // a.txt, link, sub/b.txt, sub/deeper/c.txt, deeper, sub, and the tree itself.
    const std::uint64_t unlinks = count(Operation::remove_tree, Step::unlinkat);
    ASSERT_EQ(unlinks, 7U);

    for (std::uint64_t point = 1; point <= unlinks; ++point) {
        bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
        lay_out_a_small_tree(path_of("tree"));
        const std::uint64_t before = count(Operation::remove_tree, Step::unlinkat);
        script(stop_at(Operation::remove_tree, Step::unlinkat, static_cast<std::uint32_t>(before + point)));

        const auto stopped = files().remove_tree(in("tree"));
        ASSERT_FALSE(stopped.has_value()) << "unlinkat " << point;
        EXPECT_EQ(stopped.error().kind, ErrorKind::io_failed) << "unlinkat " << point;
        EXPECT_EQ(stopped.error().step, Step::unlinkat) << "unlinkat " << point;
        EXPECT_EQ(stopped.error().system_code, 0) << "unlinkat " << point;
        EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("tree"))) << "unlinkat " << point;

        const auto finished = files().remove_tree(in("tree"));
        ASSERT_TRUE(finished.has_value()) << "unlinkat " << point << ": " << finished.error().message();
        EXPECT_TRUE(*finished) << "unlinkat " << point;
        EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("tree"))) << "unlinkat " << point;
    }
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

TEST_F(RootRefusal, TheRootIsTheRoot) {
    expect_refused_by_spelling("/");
}

TEST_F(RootRefusal, RepeatedSeparatorsAreStillTheRoot) {
    expect_refused_by_spelling("//");
    expect_refused_by_spelling("///");
}

TEST_F(RootRefusal, TheRootsOwnDotAndDotDotAreTheRoot) {
    expect_refused_by_spelling("/.");
    expect_refused_by_spelling("/./");
    expect_refused_by_spelling("/..");
    expect_refused_by_spelling("/../..");
    expect_refused_by_spelling("/./../.");
}

TEST_F(RootRefusal, ANameCancelledByDotDotLeavesTheRoot) {
    expect_refused_by_spelling("/tmp/..");
    expect_refused_by_spelling("/tmp/../");
    expect_refused_by_spelling("/tmp/./..");
    expect_refused_by_spelling("/a/b/../..");
    expect_refused_by_spelling("/a/../b/..");
    // More ".." than names: the parent of the root is the root.
    expect_refused_by_spelling("/a/../../..");
}

// The same spellings below the test's own directory name nothing, and nothing is not the root: the call
// proceeds and answers "there was nothing". (A trailing separator is refused at the door since stage 10 and is
// not among them.)
TEST_P(FileSystemContract, AnythingBelowTheRootIsNotTheRoot) {
    for (const std::string_view spelling : {"tmp", "tmp/cache/files", "tmp/cache/..", "a/../b", "..."}) {
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
TEST_F(RootRefusal, ARootReachedThroughASymlinkIsRefusedByIdentity) {
    bc::file_system_test::make_symlink_raw("/", path_of("alias"));

    const auto removed = files().remove_tree(in("alias/."));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(count(Operation::remove_tree, Step::unlinkat), 0U);
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("alias")));
}

TEST_P(FileSystemContract, AnOrdinaryDirectoryAFileAndNothingAreNotTheRoot) {
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
TEST_F(RootRefusal, ARootReachedByDotDotThroughASymlinkIsRefusedByIdentity) {
    bc::file_system_test::make_symlink_raw("/", path_of("alias"));

    const auto removed = files().remove_tree(in("alias/.."));

    ASSERT_FALSE(removed.has_value());
    EXPECT_EQ(removed.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(count(Operation::remove_tree, Step::unlinkat), 0U);
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of("alias")));
}

// -------------------------------------------------------------------------------------------------------------
// set_permissions
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, SetPermissionsSetsTheBitsOfAFileAndADirectoryExactly) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto file = files().set_permissions(in("file"), 0600, FinalSymlink::refuse);
    const auto folder = files().set_permissions(in("folder"), 0700, FinalSymlink::follow);

    ASSERT_TRUE(file.has_value()) << file.error().message();
    ASSERT_TRUE(folder.has_value()) << folder.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("file")), 0600U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("folder")), 0700U);
}

// Under `refuse` a symlink is refused and nothing changes; under `follow` what it points at gets the mode.
TEST_P(FileSystemContract, SetPermissionsRefusesASymlinkUnderRefuseAndActsOnItsTargetUnderFollow) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto refused = files().set_permissions(in("link"), 0600, FinalSymlink::refuse);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("target.txt")), 0644U);
    const auto followed = files().set_permissions(in("link"), 0600, FinalSymlink::follow);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::symlink_refused);
    EXPECT_FALSE(refused.error().step.has_value());
    ASSERT_TRUE(followed.has_value()) << followed.error().message();
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("target.txt")), 0600U);
}

TEST_P(FileSystemContract, SetPermissionsOfNothingIsNotFoundUnderEitherChoice) {
    bc::file_system_test::make_symlink_raw(path_of("nowhere"), path_of("dangling"));

    const auto refused = files().set_permissions(in("absent"), 0600, FinalSymlink::refuse);
    const auto followed = files().set_permissions(in("absent"), 0600, FinalSymlink::follow);
    const auto through_a_dangling_link = files().set_permissions(in("dangling"), 0600, FinalSymlink::follow);

    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().kind, ErrorKind::not_found);
    ASSERT_FALSE(followed.has_value());
    EXPECT_EQ(followed.error().kind, ErrorKind::not_found);
    ASSERT_FALSE(through_a_dangling_link.has_value());
    EXPECT_EQ(through_a_dangling_link.error().kind, ErrorKind::not_found);
}

// What a mode change costs on this platform: under `follow` one fchmodat; under `refuse` one look at the entry
// and one fchmodat with AT_SYMLINK_NOFOLLOW, which Apple platforms and a glibc with the fchmodat2 call or the
// /proc fallback honour on anything that is not a symlink, so the open-and-fchmod way is never needed here.
TEST_F(ObservedContract, SetPermissionsIsOneFchmodatAndUnderRefuseOneLookFirst) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto followed = files().set_permissions(in("file"), 0600, FinalSymlink::follow);
    const auto refused = files().set_permissions(in("file"), 0640, FinalSymlink::refuse);

    ASSERT_TRUE(followed.has_value()) << followed.error().message();
    ASSERT_TRUE(refused.has_value()) << refused.error().message();
    EXPECT_EQ(count(Operation::set_permissions, Step::fchmodat), 2U);
    EXPECT_EQ(count(Operation::set_permissions, Step::fstatat), components_of(root_path()) * 2 + 1);
    EXPECT_EQ(count(Operation::set_permissions, Step::fchmod), 0U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("file")), 0640U);
}

// Where a libc refuses AT_SYMLINK_NOFOLLOW on an entry that is not a symlink (ENOTSUP), the entry is opened
// without following and fchmod sets the mode on the descriptor; a symlink there answers ELOOP, refused.
TEST_F(ObservedContract, SetPermissionsFallsBackToOpenAndFchmodWhereTheFlagIsNotSupported) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    script(fail_step(Operation::set_permissions, Step::fchmodat, 1, ENOTSUP));

    const auto changed = files().set_permissions(in("file"), 0600, FinalSymlink::refuse);

    ASSERT_TRUE(changed.has_value()) << changed.error().message();
    EXPECT_EQ(count(Operation::set_permissions, Step::fchmod), 1U);
    EXPECT_EQ(count(Operation::set_permissions, Step::openat), components_of(root_path()) + 1);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("file")), 0600U);
}

// -------------------------------------------------------------------------------------------------------------
// create_temporary_directory
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, CreateTemporaryDirectoryTwiceYieldsTwoDirectoriesWithTheirModes) {
    const auto first = files().create_temporary_directory(root(), Permissions::exactly(0700));
    const auto second = files().create_temporary_directory(root(), Permissions::exactly(0750));

    ASSERT_TRUE(first.has_value()) << first.error().message();
    ASSERT_TRUE(second.has_value()) << second.error().message();
    EXPECT_NE(*first, *second);
    EXPECT_EQ(first->find('/'), std::string::npos);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of(*first)), 0700U);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of(*second)), 0750U);
    std::vector<std::string> expected{*first, *second};
    std::ranges::sort(expected);
    EXPECT_EQ(bc::file_system_test::names_raw(root_path()), expected);
}

TEST_P(FileSystemContract, CreateTemporaryDirectoryInNothingIsNotFoundAndInAFileIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto in_nothing = files().create_temporary_directory(in("absent"), Permissions::exactly(0700));
    const auto in_a_file = files().create_temporary_directory(in("file"), Permissions::exactly(0700));

    ASSERT_FALSE(in_nothing.has_value());
    EXPECT_EQ(in_nothing.error().kind, ErrorKind::not_found);
    ASSERT_FALSE(in_a_file.has_value());
    EXPECT_EQ(in_a_file.error().kind, ErrorKind::not_directory);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file"));
}

TEST_P(FileSystemContract, CreateTemporaryDirectoryBeneathAHeldDirectoryMakesItInside) {
    bc::file_system_test::make_directory_raw(path_of("sub"), 0755);
    const auto held = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(held.has_value()) << held.error().message();

    const auto name =
        files().create_temporary_directory(Location::beneath(*held, relative("sub")), Permissions::exactly(0700));

    ASSERT_TRUE(name.has_value()) << name.error().message();
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("sub")), ElementsAre(*name));
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("sub/" + *name)), 0700U);
}

// A name that is taken is tried again with the next token: one more mkdirat, and the directory is made.
TEST_F(ObservedContract, ATemporaryNameThatIsTakenIsTriedAgainWithTheNextToken) {
    script(fail_step(Operation::create_temporary_directory, Step::mkdirat, 1, EEXIST));

    const auto name = files().create_temporary_directory(root(), Permissions::exactly(0700));

    ASSERT_TRUE(name.has_value()) << name.error().message();
    EXPECT_EQ(count(Operation::create_temporary_directory, Step::mkdirat), 2U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre(*name));
}

// -------------------------------------------------------------------------------------------------------------
// rename
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, RenameMovesAFile) {
    bc::file_system_test::write_raw(path_of("old-name"), "content", 0644);

    const auto achieved =
        files().rename(in("old-name"), in("new-name"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("new-name"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("new-name")), "content");
}

TEST_P(FileSystemContract, RenameReplacesAFileAtTheDestination) {
    bc::file_system_test::write_raw(path_of("staged"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);

    const auto achieved =
        files().rename(in("staged"), in("current"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("current"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the new content");
}

// A rename under `refuse` is the platform's no-replace rename, which the portable-only engine does not have: it
// answers `unsupported` rather than rename over what it was told to keep, and both ends stay as they were.
TEST_P(FileSystemContract, RenameUnderRefuseLeavesAnExistingDestinationAndIsAlreadyExists) {
    bc::file_system_test::write_raw(path_of("staged"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);

    const auto achieved = files().rename(in("staged"), in("current"), ExistingDestination::refuse, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, expected_under(GetParam(), ErrorKind::already_exists, ErrorKind::unsupported));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("staged")), "the new content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the old content");
}

// The error of a two-location operation says which end it belongs to (`failed_location`), and the face composes
// its path from that end: a missing source is `not_found` at the source's own stat, under the source's path.
TEST_P(FileSystemContract, RenameOfNothingIsNotFoundAtTheSourcesStatAndNamesTheSource) {
    const auto achieved =
        files().rename(in("absent"), in("new-name"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().step, Step::fstatat);
    EXPECT_EQ(achieved.error().system_code, ENOENT);
    EXPECT_EQ(achieved.error().failed_location, FailedLocation::first);
    EXPECT_EQ(achieved.error().path, path_of("absent").string());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

TEST_P(FileSystemContract, RenameIntoADirectoryThatIsNotThereIsNotFoundAndNamesTheDestination) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto achieved =
        files().rename(in("file"), in("missing/file"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().failed_location, FailedLocation::second);
    EXPECT_EQ(achieved.error().path, path_of("missing/file").string());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
}

// Each refusal of a rename names its end: what the destination holds (`already_exists`, `directory_not_empty`)
// is the destination's; a contract violation blames neither, and the face then names the destination.
TEST_P(FileSystemContract, RenameRefusalsNameTheEndTheyBelongTo) {
    bc::file_system_test::write_raw(path_of("staged"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);
    bc::file_system_test::make_directory_raw(path_of("incoming"), 0755);
    bc::file_system_test::make_directory_raw(path_of("occupied"), 0755);
    bc::file_system_test::write_raw(path_of("occupied/kept.txt"), "must survive", 0644);

    const auto taken = files().rename(in("staged"), in("current"), ExistingDestination::refuse, Durability::written);
    const auto not_empty =
        files().rename(in("incoming"), in("occupied"), ExistingDestination::replace, Durability::written);
    const auto relative = files().rename(Location::at(std::filesystem::path{"relative"}), in("current"),
                                         ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(taken.has_value());
    EXPECT_EQ(taken.error().failed_location, FailedLocation::second);
    EXPECT_EQ(taken.error().path, path_of("current").string());
    ASSERT_FALSE(not_empty.has_value());
    EXPECT_EQ(not_empty.error().kind, ErrorKind::directory_not_empty);
    EXPECT_EQ(not_empty.error().failed_location, FailedLocation::second);
    EXPECT_EQ(not_empty.error().path, path_of("occupied").string());
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().kind, ErrorKind::invalid_argument);
    EXPECT_FALSE(relative.error().failed_location.has_value());
    EXPECT_EQ(relative.error().path, path_of("current").string());
}

// Under `replace` a directory replaces only an empty directory; one that is not empty is `directory_not_empty`.
TEST_P(FileSystemContract, RenameOfADirectoryOntoOneThatIsNotEmptyIsDirectoryNotEmpty) {
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

TEST_F(ObservedContract, RenameThatFailsLeavesBothEndsAsTheyWere) {
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

TEST_F(ObservedContract, DurableRenameAcrossDirectoriesSyncsBothOfThem) {
    bc::file_system_test::make_directory_raw(path_of("from"), 0755);
    bc::file_system_test::make_directory_raw(path_of("to"), 0755);
    bc::file_system_test::write_raw(path_of("from/file"), "content", 0644);

    const auto achieved =
        files().rename(in("from/file"), in("to/file"), ExistingDestination::replace, Durability::durable);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::rename, durable_sync_step()), 2U);
    EXPECT_EQ(count(Operation::rename, wrong_sync_step()), 0U);
    // The parents the two walks hold are open for search only; each directory is opened again, read-only, as
    // `.` beneath its parent, for its sync: two more openat beyond the two walks' own.
    EXPECT_EQ(count(Operation::rename, Step::openat), (components_of(root_path()) * 2) + 4);
}

TEST_F(ObservedContract, DurableRenameWithinOneDirectorySyncsItOnce) {
    bc::file_system_test::write_raw(path_of("old-name"), "content", 0644);

    const auto achieved =
        files().rename(in("old-name"), in("new-name"), ExistingDestination::replace, Durability::durable);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::rename, durable_sync_step()), 1U);
}

// The name has already moved when the sync fails, so the call reports what it achieved, not an error.
TEST_F(ObservedContract, DurableRenameWhoseDirectorySyncFailsReportsWritten) {
    bc::file_system_test::write_raw(path_of("old-name"), "content", 0644);
    script(fail_step(Operation::rename, durable_sync_step(), 1, EIO));

    const auto achieved =
        files().rename(in("old-name"), in("new-name"), ExistingDestination::replace, Durability::durable);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("new-name"));
}

// Under `replace` a directory replaces an empty directory, and a symlink moves as itself, never as what it names.
TEST_P(FileSystemContract, RenameMovesADirectoryOntoAnEmptyOneAndASymlinkAsItself) {
    bc::file_system_test::make_directory_raw(path_of("incoming"), 0755);
    bc::file_system_test::write_raw(path_of("incoming/kept.txt"), "must survive", 0644);
    bc::file_system_test::make_directory_raw(path_of("empty"), 0755);
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto directory_moved =
        files().rename(in("incoming"), in("empty"), ExistingDestination::replace, Durability::written);
    const auto link_moved =
        files().rename(in("link"), in("moved-link"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(directory_moved.has_value()) << directory_moved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("empty/kept.txt")), "must survive");
    ASSERT_TRUE(link_moved.has_value()) << link_moved.error().message();
    EXPECT_TRUE(bc::file_system_test::is_symlink_raw(path_of("moved-link")));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("target.txt")), "behind the link");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("empty", "moved-link", "target.txt"));
}

// The system refuses a file over a directory (EISDIR) and a directory over a file (ENOTDIR); each is the kind the
// table reads, and both ends stay as they were.
TEST_P(FileSystemContract, RenameOfAFileOntoADirectoryAndOfADirectoryOntoAFileAreRefused) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::make_directory_raw(path_of("other"), 0755);
    bc::file_system_test::write_raw(path_of("plain"), "plain content", 0644);

    const auto file_onto_directory =
        files().rename(in("file"), in("folder"), ExistingDestination::replace, Durability::written);
    const auto directory_onto_file =
        files().rename(in("other"), in("plain"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(file_onto_directory.has_value());
    EXPECT_EQ(file_onto_directory.error().kind, ErrorKind::not_regular_file);
    ASSERT_FALSE(directory_onto_file.has_value());
    EXPECT_EQ(directory_onto_file.error().kind, ErrorKind::not_directory);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("plain")), "plain content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file", "folder", "other", "plain"));
}

// A no-replace rename is the platform's one call (never renameat), its EEXIST is `already_exists` at that step,
// and both files are left byte for byte.
TEST_F(ObservedContract, RenameUnderRefuseIsOneNoReplaceRenameAndLeavesBothFilesWhole) {
    bc::file_system_test::write_raw(path_of("staged"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);

    const auto achieved = files().rename(in("staged"), in("current"), ExistingDestination::refuse, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::already_exists);
    EXPECT_EQ(achieved.error().step, Step::rename_no_replace);
    EXPECT_EQ(achieved.error().system_code, EEXIST);
    EXPECT_EQ(count(Operation::rename, Step::rename_no_replace), 1U);
    EXPECT_EQ(count(Operation::rename, Step::renameat), 0U);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("staged")), "the new content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the old content");
}

// A second volume cannot be planted on this machine, so the scripted EXDEV on the rename's own step is the
// witness: the answer is `cross_device`, never a copy and a delete behind the one step the header promises.
TEST_F(ObservedContract, RenameAcrossDevicesIsCrossDeviceAndNeverCopiesAndDeletes) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    script(fail_step(Operation::rename, Step::renameat, 1, EXDEV));

    const auto achieved = files().rename(in("file"), in("moved"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::cross_device);
    EXPECT_EQ(achieved.error().system_code, EXDEV);
    EXPECT_EQ(count(Operation::rename, Step::write), 0U);
    EXPECT_EQ(count(Operation::rename, Step::unlinkat), 0U);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file"));
}

// EINVAL from a no-replace rename means a file system without the flag or a source that is an ancestor of its
// destination, which cannot be told apart; the answer is `unsupported` with the code kept, and nothing moved.
TEST_F(ObservedContract, ANoReplaceRenameRefusedWithEinvalIsUnsupportedWithTheCodeKept) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    script(fail_step(Operation::rename, Step::rename_no_replace, 1, EINVAL));

    const auto achieved = files().rename(in("file"), in("moved"), ExistingDestination::refuse, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::unsupported);
    EXPECT_EQ(achieved.error().step, Step::rename_no_replace);
    EXPECT_EQ(achieved.error().system_code, EINVAL);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file"));
}

// A missing source is found at its own stat, before any rename is attempted.
TEST_F(ObservedContract, RenameOfNothingStopsAtTheSourcesStatAndNeverRenames) {
    const auto achieved =
        files().rename(in("absent"), in("new-name"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().step, Step::fstatat);
    EXPECT_EQ(count(Operation::rename, Step::renameat), 0U);
    EXPECT_EQ(count(Operation::rename, Step::rename_no_replace), 0U);
}

// The root of the file system is neither moved nor replaced: a rename with the root at either end is refused
// with nothing asked of the system beyond the walks. The net below (a stop at the first rename of either kind)
// makes a broken refusal the proof rather than the damage, as the RootRefusal cases do for remove_tree.
TEST_F(ObservedContract, RenameOfTheRootOrOntoTheRootIsInvalidArgumentAndMovesNothing) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    script(stop_at(Operation::rename, Step::renameat, 1));
    script(stop_at(Operation::rename, Step::rename_no_replace, 1));
    const Location root_of_the_file_system = Location::at(std::filesystem::path{"/"});

    const auto of_the_root =
        files().rename(root_of_the_file_system, in("moved"), ExistingDestination::replace, Durability::written);
    const auto onto_the_root =
        files().rename(in("file"), root_of_the_file_system, ExistingDestination::refuse, Durability::written);

    ASSERT_FALSE(of_the_root.has_value());
    EXPECT_EQ(of_the_root.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(onto_the_root.has_value());
    EXPECT_EQ(onto_the_root.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(count(Operation::rename, Step::renameat), 0U);
    EXPECT_EQ(count(Operation::rename, Step::rename_no_replace), 0U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file"));
}

// -------------------------------------------------------------------------------------------------------------
// copy_file
// -------------------------------------------------------------------------------------------------------------

TEST_P(FileSystemContract, CopyWritesTheSourcesBytesAndLeavesTheSource) {
    const std::string large(std::size_t{200} * 1024, 'y');
    bc::file_system_test::write_raw(path_of("source"), large, 0644);

    const auto achieved = files().copy_file(in("source"), in("copy"), kWrittenExactly0600);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("copy")), large);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("source")), large);
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("copy")), 0600U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("copy", "source"));
}

// As for a rename of nothing: the error belongs to the source (its open is the step that failed) and the face
// composes its path from the source. Nothing is staged.
TEST_P(FileSystemContract, CopyOfNothingIsNotFoundAtTheSourcesOpenAndNamesTheSource) {
    const auto achieved = files().copy_file(in("absent"), in("copy"), kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().step, Step::openat);
    EXPECT_EQ(achieved.error().system_code, ENOENT);
    EXPECT_EQ(achieved.error().failed_location, FailedLocation::first);
    EXPECT_EQ(achieved.error().path, path_of("absent").string());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

TEST_P(FileSystemContract, CopyIntoADirectoryThatIsNotThereIsNotFoundAndLeavesTheSource) {
    bc::file_system_test::write_raw(path_of("source"), "content", 0644);

    const auto achieved = files().copy_file(in("source"), in("missing/copy"), kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::not_found);
    EXPECT_EQ(achieved.error().failed_location, FailedLocation::second);
    EXPECT_EQ(achieved.error().path, path_of("missing/copy").string());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("source"));
}

TEST_P(FileSystemContract, CopyOfAnEmptyFileAndOfAFileLargerThanAnyPieceIsByteIdentical) {
    const std::string large(std::size_t{3} * 1024 * 1024, 'z');
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);
    bc::file_system_test::write_raw(path_of("large"), large, 0644);

    const auto empty_copied = files().copy_file(in("empty"), in("empty-copy"), kWrittenExactly0644);
    const auto large_copied = files().copy_file(in("large"), in("large-copy"), kWrittenExactly0644);

    ASSERT_TRUE(empty_copied.has_value()) << empty_copied.error().message();
    ASSERT_TRUE(large_copied.has_value()) << large_copied.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("empty-copy")), "");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("large-copy")), large);
}

// Under `follow` a symlink source is the file it names; the link itself stays a link.
TEST_P(FileSystemContract, CopyFollowsASourceThatIsASymlinkWhenToldToAndCopiesTheTarget) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto achieved = files().copy_file(in("link"), in("copy"), kWrittenExactly0644Following);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("copy")), "behind the link");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("copy")), 0644U);
    EXPECT_TRUE(bc::file_system_test::is_symlink_raw(path_of("link")));
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("copy", "link", "target.txt"));
}

// A copy is of a regular file onto a regular file or onto nothing: a directory at either end is refused, with
// nothing staged and the directory as it was.
TEST_P(FileSystemContract, CopyOntoADirectoryAndOfADirectoryAreNotRegularFile) {
    bc::file_system_test::write_raw(path_of("source"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::write_raw(path_of("folder/kept.txt"), "must survive", 0644);

    const auto onto_directory = files().copy_file(in("source"), in("folder"), kWrittenExactly0644);
    const auto of_directory = files().copy_file(in("folder"), in("copy"), kWrittenExactly0644);

    ASSERT_FALSE(onto_directory.has_value());
    EXPECT_EQ(onto_directory.error().kind, ErrorKind::not_regular_file);
    EXPECT_EQ(onto_directory.error().failed_location, FailedLocation::second);
    EXPECT_EQ(onto_directory.error().path, path_of("folder").string());
    ASSERT_FALSE(of_directory.has_value());
    EXPECT_EQ(of_directory.error().kind, ErrorKind::not_regular_file);
    EXPECT_EQ(of_directory.error().failed_location, FailedLocation::first);
    EXPECT_EQ(of_directory.error().path, path_of("folder").string());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("folder/kept.txt")), "must survive");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("folder", "source"));
}

// The same file under the destination's name (a hard link included) is nothing to copy: refused before anything
// is staged, so the file is never replaced with itself.
TEST_P(FileSystemContract, CopyOfAFileOntoItselfIsInvalidArgumentAndTouchesNothing) {
    bc::file_system_test::write_raw(path_of("source"), "content", 0644);
    bc::file_system_test::make_hard_link_raw(path_of("source"), path_of("same-file"));

    const auto onto_itself = files().copy_file(in("source"), in("source"), kWrittenExactly0600);
    const auto onto_its_link = files().copy_file(in("source"), in("same-file"), kWrittenExactly0600);

    ASSERT_FALSE(onto_itself.has_value());
    EXPECT_EQ(onto_itself.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(onto_its_link.has_value());
    EXPECT_EQ(onto_its_link.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("source")), "content");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("source")), 0644U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("same-file", "source"));
}

// A copy under `refuse` takes the destination's name as a write does, with the platform's no-replace rename; the
// portable-only engine has none and answers `unsupported` before anything is staged, as for a write.
TEST_P(FileSystemContract, CopyUnderRefuseLeavesAnExistingDestinationAndIsAlreadyExists) {
    bc::file_system_test::write_raw(path_of("source"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("existing"), "the old content", 0644);

    const auto achieved = files().copy_file(in("source"), in("existing"), kWrittenExactly0644Refusing);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, expected_under(GetParam(), ErrorKind::already_exists, ErrorKind::unsupported));
    EXPECT_EQ(achieved.error().path, path_of("existing").string());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("existing")), "the old content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("existing", "source"));
}

// Under `refuse` a source that is a symlink is refused as one (the clone path takes CLONE_NOFOLLOW).
TEST_P(FileSystemContract, CopyRefusesASourceThatIsASymlink) {
    bc::file_system_test::write_raw(path_of("target.txt"), "behind the link", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target.txt"), path_of("link"));

    const auto achieved = files().copy_file(in("link"), in("copy"), kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::symlink_refused);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("copy")));
}

// preserve_or keeps the mode of the file being replaced. The source's mode plays no part.
TEST_P(FileSystemContract, CopyUnderPreserveOrKeepsTheDestinationsModeNotTheSources) {
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

TEST_F(ObservedContract, CopyWhoseRenameFailsLeavesTheDestinationWholeAndNoTemporaryFile) {
    bc::file_system_test::write_raw(path_of("source"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("existing"), "the old content", 0644);
    script(fail_step(Operation::copy_file, Step::renameat, 1, EIO));

    const auto achieved = files().copy_file(in("source"), in("existing"), kDurableExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("existing")), "the old content");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("existing", "source"));
}

TEST_F(ObservedContract, DurableCopySyncsTheFileThenTheDirectory) {
    bc::file_system_test::write_raw(path_of("source"), "content", 0644);

    const auto achieved = files().copy_file(in("source"), in("copy"), kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(count(Operation::copy_file, durable_sync_step()), 2U);
    EXPECT_EQ(count(Operation::copy_file, wrong_sync_step()), 0U);
}

// The portable path: when the fast paths refuse (ENOTSUP, the platform's own answer where it offers none; the
// scripted occurrences cover both copies on a platform that reaches both steps), the copy moves the bytes itself
// with pread and write, and the destination is the same bytes. The size of a piece is the core's own business
// and is not asserted.
TEST_F(ObservedContract, CopyThroughTheLoopWhenTheFastPathsRefuseIsByteIdentical) {
    const std::string large(std::size_t{3} * 1024 * 1024, 'z');
    bc::file_system_test::write_raw(path_of("large"), large, 0644);
    bc::file_system_test::write_raw(path_of("empty"), "", 0644);
    script(fail_step(Operation::copy_file, Step::clone_file, 1, ENOTSUP));
    script(fail_step(Operation::copy_file, Step::copy_range, 1, ENOTSUP));
    script(fail_step(Operation::copy_file, Step::clone_file, 2, ENOTSUP));
    script(fail_step(Operation::copy_file, Step::copy_range, 2, ENOTSUP));

    const auto large_copied = files().copy_file(in("large"), in("large-copy"), kWrittenExactly0600);
    const auto empty_copied = files().copy_file(in("empty"), in("empty-copy"), kWrittenExactly0600);

    ASSERT_TRUE(large_copied.has_value()) << large_copied.error().message();
    ASSERT_TRUE(empty_copied.has_value()) << empty_copied.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("large-copy")), large);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("empty-copy")), "");
    EXPECT_EQ(bc::file_system_test::mode_raw(path_of("large-copy")), 0600U);
    EXPECT_GE(count(Operation::copy_file, Step::write), 1U);
    EXPECT_GE(count(Operation::copy_file, Step::pread), 2U);
    EXPECT_THAT(staged_names_of(root_path(), "large-copy"), IsEmpty());
}

// Where the kernel moves the bytes (the clone on Apple platforms and on a Linux volume with reflinks, the
// in-kernel copy on any other Linux volume) no byte passes through the process; the suite states which platforms
// owe that, as it does for the sync step.
#if defined(__APPLE__) || defined(__linux__)
constexpr auto kernel_moves_the_bytes() noexcept -> bool {
    return true;
}
#else
constexpr auto kernel_moves_the_bytes() noexcept -> bool {
    return false;
}
#endif

TEST_F(ObservedContract, CopyByTheFastestPathPassesNoByteThroughTheProcessWhereTheKernelMovesThem) {
    const std::string content(std::size_t{200} * 1024, 'y');
    bc::file_system_test::write_raw(path_of("source"), content, 0644);

    const auto achieved = files().copy_file(in("source"), in("copy"), kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("copy")), content);
    if (kernel_moves_the_bytes()) {
        EXPECT_EQ(count(Operation::copy_file, Step::clone_file), 1U);
        EXPECT_EQ(count(Operation::copy_file, Step::write), 0U);
        EXPECT_EQ(count(Operation::copy_file, Step::pread), 0U);
    }
}

// A stop at the clone makes nothing: the step returns before the call, so no staged file exists. Where the clone
// succeeded (its one step made the file and no write or in-kernel copy followed), a stop at the clone's reopen
// leaves the clone as a kill would, beside the untouched destination. Decided from a first copy's counts, since
// whether a volume clones is known only at clone time.
TEST_F(ObservedContract, AStopAtTheCloneMakesNothingAndAStopAtItsReopenLeavesTheClone) {
    bc::file_system_test::write_raw(path_of("source"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("existing"), "the old content", 0644);
    const auto first = files().copy_file(in("source"), in("probe"), kWrittenExactly0644);
    ASSERT_TRUE(first.has_value()) << first.error().message();
    if (count(Operation::copy_file, Step::clone_file) == 0) {
        GTEST_SKIP() << "this platform reaches no clone step";
    }
    const bool cloned =
        count(Operation::copy_file, Step::write) == 0 && count(Operation::copy_file, Step::copy_range) == 0;

    script(stop_at(Operation::copy_file, Step::clone_file, 2));
    const auto at_the_clone = files().copy_file(in("source"), in("existing"), kDurableExactly0644);

    ASSERT_FALSE(at_the_clone.has_value());
    EXPECT_EQ(at_the_clone.error().step, Step::clone_file);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("existing")), "the old content");
    EXPECT_THAT(staged_names_of(root_path(), "existing"), IsEmpty());
    if (!cloned) {
        return;
    }
    // The reopen is the first openat after the two walks and the source's own open.
    const auto reopen = static_cast<std::uint32_t>((components_of(root_path()) * 2) + 2);
    const std::uint64_t reopens_so_far = count(Operation::copy_file, Step::openat);
    script(stop_at(Operation::copy_file, Step::openat, static_cast<std::uint32_t>(reopens_so_far) + reopen));
    const auto at_the_reopen = files().copy_file(in("source"), in("existing"), kDurableExactly0644);

    ASSERT_FALSE(at_the_reopen.has_value());
    EXPECT_EQ(at_the_reopen.error().step, Step::openat);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("existing")), "the old content");
    const std::vector<std::string> staged = staged_names_of(root_path(), "existing");
    ASSERT_EQ(staged.size(), 1U) << testing::PrintToString(staged);
    EXPECT_EQ(bc::file_system_test::read_raw(root_path() / staged.front()), "the new content");
}

// -------------------------------------------------------------------------------------------------------------
// try_lock
// -------------------------------------------------------------------------------------------------------------

// The lock is advisory and held by the open-file description the handle owns, so a second engine in the same
// process contends like a second process would: it is refused while the first holds the lock and takes it once
// the first lets go. The same holds for one engine asked twice.
TEST_P(FileSystemContract, TryLockAcquiresAndASecondEngineInTheSameProcessIsRefusedUntilItIsReleased) {
    bc::file_system_test::write_raw(path_of("file.lock"), "", 0644);
    Result<Engine> second_engine = make_engine(GetParam());
    ASSERT_TRUE(second_engine.has_value()) << second_engine.error().message();
    FileSystem second(second_engine.value());

    auto first = files().try_lock(in("file.lock"));
    ASSERT_TRUE(first.has_value()) << first.error().message();
    ASSERT_TRUE(first->has_value());
    const auto while_held_elsewhere = second.try_lock(in("file.lock"));
    const auto while_held_by_the_same_engine = files().try_lock(in("file.lock"));
    first->reset();
    const auto after_release = second.try_lock(in("file.lock"));

    ASSERT_TRUE(while_held_elsewhere.has_value()) << while_held_elsewhere.error().message();
    EXPECT_FALSE(while_held_elsewhere->has_value());
    ASSERT_TRUE(while_held_by_the_same_engine.has_value()) << while_held_by_the_same_engine.error().message();
    EXPECT_FALSE(while_held_by_the_same_engine->has_value());
    ASSERT_TRUE(after_release.has_value()) << after_release.error().message();
    EXPECT_TRUE(after_release->has_value());
}

// A directory may hold a lock as a file may: the lock opens the entry itself, read-only.
TEST_P(FileSystemContract, TryLockOnADirectoryAcquiresAndExcludesASecondLock) {
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto first = files().try_lock(in("folder"));
    const auto second = files().try_lock(in("folder"));

    ASSERT_TRUE(first.has_value()) << first.error().message();
    EXPECT_TRUE(first->has_value());
    ASSERT_TRUE(second.has_value()) << second.error().message();
    EXPECT_FALSE(second->has_value());
}

// A lock never creates, never follows a symlink, and never locks what is neither a file nor a directory.
TEST_P(FileSystemContract, TryLockOnNothingIsNotFoundOnASymlinkIsRefusedAndOnAFifoIsNotRegularFile) {
    bc::file_system_test::write_raw(path_of("target"), "", 0644);
    bc::file_system_test::make_symlink_raw(path_of("target"), path_of("link"));
    bc::file_system_test::make_fifo_raw(path_of("pipe"), 0600);

    const auto absent = files().try_lock(in("absent.lock"));
    const auto through_link = files().try_lock(in("link"));
    const auto fifo = files().try_lock(in("pipe"));

    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().kind, ErrorKind::not_found);
    EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("absent.lock")));
    ASSERT_FALSE(through_link.has_value());
    EXPECT_EQ(through_link.error().kind, ErrorKind::symlink_refused);
    ASSERT_FALSE(fifo.has_value());
    EXPECT_EQ(fifo.error().kind, ErrorKind::not_regular_file);
    EXPECT_FALSE(fifo.error().step.has_value());
}

// What a lock costs: the leaf's open, one look at the descriptor and one flock; a scripted EWOULDBLOCK is
// `held_elsewhere` with nothing waited for, and a platform that refuses to lock (ENOTSUP) is `unsupported`.
TEST_F(ObservedContract, TryLockIsOneOpenOneLookAndOneFlockAndReadsTheTwoRefusals) {
    bc::file_system_test::write_raw(path_of("file.lock"), "", 0644);
    script(fail_step(Operation::try_lock, Step::flock, 2, EWOULDBLOCK));
    script(fail_step(Operation::try_lock, Step::flock, 3, ENOTSUP));

    const auto acquired = files().try_lock(in("file.lock"));
    ASSERT_TRUE(acquired.has_value()) << acquired.error().message();
    EXPECT_TRUE(acquired->has_value());
    EXPECT_EQ(count(Operation::try_lock, Step::openat), components_of(root_path()) + 1);
    EXPECT_EQ(count(Operation::try_lock, Step::fstat), components_of(root_path()) + 1);
    EXPECT_EQ(count(Operation::try_lock, Step::flock), 1U);

    const auto held_elsewhere = files().try_lock(in("file.lock"));
    const auto unsupported = files().try_lock(in("file.lock"));

    ASSERT_TRUE(held_elsewhere.has_value()) << held_elsewhere.error().message();
    EXPECT_FALSE(held_elsewhere->has_value());
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_EQ(unsupported.error().kind, ErrorKind::unsupported);
    EXPECT_EQ(unsupported.error().step, Step::flock);
}

// -------------------------------------------------------------------------------------------------------------
// create_symlink and read_symlink
// -------------------------------------------------------------------------------------------------------------

// The target goes to the system as spelled, `..` and all, and comes back the same; the link is a link.
TEST_P(FileSystemContract, CreateSymlinkStoresTheTargetTextUnchangedAndReadSymlinkReadsItBack) {
    bc::file_system_test::make_directory_raw(path_of("sub"), 0755);

    const auto created = files().create_symlink("../elsewhere/./target.txt", in("sub/link"));
    const auto absolute = files().create_symlink(path_of("sub").string(), in("alias"));
    const auto text = files().read_symlink(in("sub/link"));
    const auto absolute_text = files().read_symlink(in("alias"));

    ASSERT_TRUE(created.has_value()) << created.error().message();
    ASSERT_TRUE(absolute.has_value()) << absolute.error().message();
    EXPECT_TRUE(bc::file_system_test::is_symlink_raw(path_of("sub/link")));
    ASSERT_TRUE(text.has_value()) << text.error().message();
    EXPECT_EQ(*text, "../elsewhere/./target.txt");
    ASSERT_TRUE(absolute_text.has_value()) << absolute_text.error().message();
    EXPECT_EQ(*absolute_text, path_of("sub").string());
}

TEST_P(FileSystemContract, CreateSymlinkOverAnExistingEntryIsAlreadyExistsWhateverHoldsTheName) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);
    bc::file_system_test::make_symlink_raw("file", path_of("link"));

    for (const std::string_view name : {"file", "folder", "link"}) {
        const auto created = files().create_symlink("elsewhere", in(name));
        ASSERT_FALSE(created.has_value()) << name;
        EXPECT_EQ(created.error().kind, ErrorKind::already_exists) << name;
        EXPECT_EQ(created.error().step, Step::symlinkat) << name;
        EXPECT_EQ(created.error().system_code, EEXIST) << name;
    }
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
    EXPECT_EQ(files().read_symlink(in("link")).value(), "file");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file", "folder", "link"));
}

// A target the system could not hold is refused before any call: empty, a NUL inside, or PATH_MAX or longer
// (4096 bytes is at or past the bound on every platform the suite runs on: 4096 on Linux, 1024 on Apple).
TEST_P(FileSystemContract, CreateSymlinkRefusesAnEmptyANulBearingAndAnOverlongTargetBeforeAnyCall) {
    const std::string with_nul("a\0b", 3);
    const std::string overlong(4096, 't');

    const auto empty = files().create_symlink("", in("link"));
    const auto nul = files().create_symlink(with_nul, in("link"));
    const auto too_long = files().create_symlink(overlong, in("link"));

    for (const auto* refused : {&empty, &nul, &too_long}) {
        ASSERT_FALSE(refused->has_value());
        EXPECT_EQ(refused->error().kind, ErrorKind::invalid_argument);
        EXPECT_FALSE(refused->error().step.has_value());
    }
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

// A target as long as the system allows lands: 1000 bytes is under PATH_MAX everywhere.
TEST_P(FileSystemContract, CreateSymlinkTakesATargetJustUnderTheSystemsBound) {
    const std::string long_target(1000, 't');

    const auto created = files().create_symlink(long_target, in("link"));
    const auto text = files().read_symlink(in("link"));

    ASSERT_TRUE(created.has_value()) << created.error().message();
    ASSERT_TRUE(text.has_value()) << text.error().message();
    EXPECT_EQ(*text, long_target);
}

TEST_P(FileSystemContract, CreateSymlinkIntoAMissingDirectoryIsNotFoundAndAtTheRootIsAlreadyExists) {
    const auto below_nothing = files().create_symlink("target", in("missing/link"));
    const auto at_the_root = files().create_symlink("target", Location::at(std::filesystem::path{"/"}));

    ASSERT_FALSE(below_nothing.has_value());
    EXPECT_EQ(below_nothing.error().kind, ErrorKind::not_found);
    ASSERT_FALSE(at_the_root.has_value());
    EXPECT_EQ(at_the_root.error().kind, ErrorKind::already_exists);
    EXPECT_FALSE(at_the_root.error().step.has_value());
}

// A dangling link reads like any other: the text is the link's, whatever it points at.
TEST_P(FileSystemContract, ReadSymlinkReadsADanglingLinksText) {
    bc::file_system_test::make_symlink_raw("nowhere/at/all", path_of("dangling"));

    const auto text = files().read_symlink(in("dangling"));

    ASSERT_TRUE(text.has_value()) << text.error().message();
    EXPECT_EQ(*text, "nowhere/at/all");
}

// No kind of the fourteen means "not a symlink": the system's EINVAL is passed on as `io_failed` with the code,
// as the header says, for a file, a directory and the root alike; nothing there is `not_found`.
TEST_P(FileSystemContract, ReadSymlinkOfAFileADirectoryOrTheRootIsIoFailedWithEinvalAndOfNothingIsNotFound) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_directory_raw(path_of("folder"), 0755);

    const auto of_file = files().read_symlink(in("file"));
    const auto of_folder = files().read_symlink(in("folder"));
    const auto of_root = files().read_symlink(Location::at(std::filesystem::path{"/"}));
    const auto of_nothing = files().read_symlink(in("absent"));

    for (const auto* not_a_link : {&of_file, &of_folder, &of_root}) {
        ASSERT_FALSE(not_a_link->has_value());
        EXPECT_EQ(not_a_link->error().kind, ErrorKind::io_failed);
        EXPECT_EQ(not_a_link->error().system_code, EINVAL);
        EXPECT_EQ(not_a_link->error().step, Step::readlinkat);
    }
    ASSERT_FALSE(of_nothing.has_value());
    EXPECT_EQ(of_nothing.error().kind, ErrorKind::not_found);
}

// Beneath a held directory a link is made and read inside it, and a symlink on the way is refused.
TEST_P(FileSystemContract, CreateSymlinkBeneathADirectoryMakesTheLinkInsideAndRefusesASymlinkOnTheWay) {
    lay_out_directories(root_path());
    const auto held = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(held.has_value()) << held.error().message();

    const auto inside = files().create_symlink("../file", Location::beneath(*held, relative("sub/deeper/link")));
    const auto through_link = files().create_symlink("file", Location::beneath(*held, relative("alias/link")));
    const auto text = files().read_symlink(Location::beneath(*held, relative("sub/deeper/link")));

    ASSERT_TRUE(inside.has_value()) << inside.error().message();
    ASSERT_FALSE(through_link.has_value());
    EXPECT_EQ(through_link.error().kind, ErrorKind::symlink_refused);
    ASSERT_TRUE(text.has_value()) << text.error().message();
    EXPECT_EQ(*text, "../file");
    EXPECT_THAT(bc::file_system_test::names_raw(path_of("sub/deeper")), ElementsAre("link"));
}

// -------------------------------------------------------------------------------------------------------------
// condemn and remove_condemned
// -------------------------------------------------------------------------------------------------------------

// A condemn is one step of the platform's no-replace rename, so that a condemned entry is whole under its new name
// or untouched under its old one. The portable-only engine has no such rename, and a plain one would replace a
// condemned name a previous condemn left: every condemn there answers `unsupported` before any system call, and
// the cases below hold that nothing moved.

TEST_P(FileSystemContract, CondemnRenamesATreeBesideItselfAndAnswersTheCondemnedName) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    lay_out_a_small_tree(path_of("tree"));

    const auto name = files().condemn(in("tree"));

    ASSERT_EQ(answer_of(name), expected_under(GetParam(), kSucceeds, kUnsupported));
    if (!name.has_value()) {
        EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("tree"));
        return;
    }
    EXPECT_EQ(*name, ".bc-condemned-tree");
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre(".bc-condemned-tree"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of(".bc-condemned-tree/sub/b.txt")), "bee");
}

// A condemned name a previous condemn left behind is never an error: the second condemn of the same name takes
// another name, with a token of the engine's appended, and both trees wait for the sweep.
TEST_P(FileSystemContract, CondemnTwiceWithTheSameNameYieldsTwoNamesAndKeepsBothTrees) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::write_raw(path_of("tree/first"), "first", 0644);
    const auto first = files().condemn(in("tree"));
    ASSERT_EQ(answer_of(first), expected_under(GetParam(), kSucceeds, kUnsupported));
    if (!first.has_value()) {
        EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("tree"));
        return;
    }
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    bc::file_system_test::write_raw(path_of("tree/second"), "second", 0644);

    const auto second = files().condemn(in("tree"));

    ASSERT_TRUE(second.has_value()) << second.error().message();
    EXPECT_NE(*first, *second);
    EXPECT_TRUE(second->starts_with(".bc-condemned-tree-"));
    std::vector<std::string> expected{*first, *second};
    std::ranges::sort(expected);
    EXPECT_EQ(bc::file_system_test::names_raw(root_path()), expected);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of(*first + "/first")), "first");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of(*second + "/second")), "second");
}

// A file and a symlink are condemned as themselves; nothing there is `not_found`; the root is refused.
TEST_P(FileSystemContract, CondemnTakesAFileAndASymlinkAsThemselvesRefusesTheRootAndFindsNothingNotFound) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    bc::file_system_test::make_symlink_raw("file", path_of("link"));

    const auto file = files().condemn(in("file"));
    const auto link = files().condemn(in("link"));
    const auto root_of_all = files().condemn(Location::at(std::filesystem::path{"/"}));
    const auto nothing = files().condemn(in("absent"));

    // Before any system call, the portable-only engine's refusal comes first: of the file, the link, the root
    // and nothing alike.
    ASSERT_EQ(answer_of(file), expected_under(GetParam(), kSucceeds, kUnsupported));
    ASSERT_EQ(answer_of(link), expected_under(GetParam(), kSucceeds, kUnsupported));
    ASSERT_EQ(answer_of(root_of_all), expected_under(GetParam(), kInvalidArgument, kUnsupported));
    ASSERT_EQ(answer_of(nothing), expected_under(GetParam(), kNotFound, kUnsupported));
    if (!file.has_value()) {
        EXPECT_FALSE(nothing.error().step.has_value());
        EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("file", "link"));
        return;
    }
    ASSERT_TRUE(file.has_value()) << file.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of(*file)), "content");
    ASSERT_TRUE(link.has_value()) << link.error().message();
    EXPECT_TRUE(bc::file_system_test::is_symlink_raw(path_of(*link)));
    EXPECT_EQ(files().read_symlink(in(*link)).value(), "file");
    ASSERT_FALSE(root_of_all.has_value());
    EXPECT_EQ(root_of_all.error().kind, ErrorKind::invalid_argument);
    EXPECT_FALSE(root_of_all.error().step.has_value());
    ASSERT_FALSE(nothing.has_value());
    EXPECT_EQ(nothing.error().kind, ErrorKind::not_found);
    EXPECT_EQ(nothing.error().step, Step::rename_no_replace);
}

// A leaf as long as a name may be is cut inside the condemned name, so the name fits with a token appended.
TEST_P(FileSystemContract, CondemnOfALeafAsLongAsANameMayBeCutsItToFit) {
    const std::string leaf(255, 'n');
    bc::file_system_test::write_raw(path_of(leaf), "content", 0644);

    const auto first = files().condemn(in(leaf));
    ASSERT_EQ(answer_of(first), expected_under(GetParam(), kSucceeds, kUnsupported));
    if (!first.has_value()) {
        EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre(leaf));
        return;
    }
    bc::file_system_test::write_raw(path_of(leaf), "again", 0644);
    const auto second = files().condemn(in(leaf));

    ASSERT_TRUE(first.has_value()) << first.error().message();
    ASSERT_TRUE(second.has_value()) << second.error().message();
    EXPECT_LE(first->size(), 255U);
    EXPECT_LE(second->size(), 255U);
    EXPECT_NE(*first, *second);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of(*first)), "content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of(*second)), "again");
}

// A taken condemned name costs one more no-replace rename, with the token appended; the condemn is one step of
// the platform's no-replace rename, never renameat.
TEST_F(ObservedContract, CondemnWhoseFirstNameIsTakenAppendsATokenAndIsOneMoreNoReplaceRename) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    script(fail_step(Operation::condemn, Step::rename_no_replace, 1, EEXIST));

    const auto name = files().condemn(in("tree"));

    ASSERT_TRUE(name.has_value()) << name.error().message();
    EXPECT_TRUE(name->starts_with(".bc-condemned-tree-"));
    EXPECT_EQ(count(Operation::condemn, Step::rename_no_replace), 2U);
    EXPECT_EQ(count(Operation::condemn, Step::renameat), 0U);
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre(*name));
}

// The sweep clears what the engine's own marks name and nothing else: condemned trees and files, a killed
// write's staged file, a clone's leftover under the same name shape; a temporary directory and a plain hidden
// file are the caller's and stay.
TEST_P(FileSystemContract, RemoveCondemnedClearsCondemnedEntriesAndStagedLeftoversAndLeavesTheRest) {
    bc::file_system_test::make_directory_raw(path_of(".bc-condemned-tree"), 0755);
    lay_out_a_small_tree(path_of(".bc-condemned-tree"));
    bc::file_system_test::write_raw(path_of(".bc-condemned-file"), "", 0644);
    bc::file_system_test::make_symlink_raw("kept", path_of(".bc-condemned-link"));
    bc::file_system_test::write_raw(path_of(".ledger.bc-tmp-4242-7"), "a write killed before its rename", 0600);
    bc::file_system_test::write_raw(path_of(".copy.bc-tmp-4242-8"), "a clone killed before its reopen", 0600);
    bc::file_system_test::write_raw(path_of("kept"), "kept", 0644);
    bc::file_system_test::write_raw(path_of(".hidden"), "hidden", 0644);
    bc::file_system_test::make_directory_raw(path_of("tmp-4242-9"), 0700);
    bc::file_system_test::make_directory_raw(path_of("sub"), 0755);
    bc::file_system_test::write_raw(path_of("sub/.bc-condemned-deeper"), "one level deep only", 0644);

    const auto swept = files().remove_condemned(root());

    ASSERT_TRUE(swept.has_value()) << swept.error().message();
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre(".hidden", "kept", "sub", "tmp-4242-9"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("sub/.bc-condemned-deeper")), "one level deep only");
}

TEST_P(FileSystemContract, RemoveCondemnedOfAnEmptyDirectoryOrOneWithNothingMarkedChangesNothing) {
    bc::file_system_test::write_raw(path_of("kept"), "kept", 0644);

    const auto swept = files().remove_condemned(root());

    ASSERT_TRUE(swept.has_value()) << swept.error().message();
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("kept"));
}

TEST_P(FileSystemContract, RemoveCondemnedInNothingIsNotFoundAndInAFileIsNotDirectory) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);

    const auto in_nothing = files().remove_condemned(in("absent"));
    const auto in_a_file = files().remove_condemned(in("file"));

    ASSERT_FALSE(in_nothing.has_value());
    EXPECT_EQ(in_nothing.error().kind, ErrorKind::not_found);
    ASSERT_FALSE(in_a_file.has_value());
    EXPECT_EQ(in_a_file.error().kind, ErrorKind::not_directory);
}

// Condemn then sweep, through the library end to end: the tree is gone and the neighbours stay.
TEST_P(FileSystemContract, CondemnThenRemoveCondemnedClearsTheTreeAndLeavesItsNeighbours) {
    bc::file_system_test::make_directory_raw(path_of("tree"), 0755);
    lay_out_a_small_tree(path_of("tree"));
    bc::file_system_test::write_raw(path_of("neighbour"), "stays", 0644);

    const auto name = files().condemn(in("tree"));
    const auto swept = files().remove_condemned(root());

    // The sweep is no rename and runs on every engine; on the portable-only engine it finds nothing to sweep.
    ASSERT_EQ(answer_of(name), expected_under(GetParam(), kSucceeds, kUnsupported));
    ASSERT_TRUE(swept.has_value()) << swept.error().message();
    if (!name.has_value()) {
        EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("neighbour", "tree"));
        return;
    }
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("neighbour"));
}

// The sweep stops at the first entry it cannot remove, names it relative to the swept directory (the face
// composes the path), and keeps what it removed before it.
TEST_F(ObservedContract, RemoveCondemnedStopsAtTheEntryItCannotRemoveAndNamesIt) {
    bc::file_system_test::make_directory_raw(path_of(".bc-condemned-tree"), 0755);
    bc::file_system_test::make_directory_raw(path_of(".bc-condemned-tree/a"), 0755);
    bc::file_system_test::write_raw(path_of(".bc-condemned-tree/a/kept"), "content", 0644);
    bc::file_system_test::write_raw(path_of(".bc-condemned-tree/a/gone"), "content", 0644);
    // The two files are unlinked in the order the stream answers them: the second is the one refused.
    script(fail_step(Operation::remove_condemned, Step::unlinkat, 2, EACCES));

    const auto swept = files().remove_condemned(root());

    ASSERT_FALSE(swept.has_value());
    EXPECT_EQ(swept.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(swept.error().step, Step::unlinkat);
    EXPECT_FALSE(swept.error().name_truncated);
    const std::vector<std::string> remaining = bc::file_system_test::names_raw(path_of(".bc-condemned-tree/a"));
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(swept.error().path, path_of(".bc-condemned-tree/a/" + remaining.front()).string());
}

// A staged leftover the sweep cannot unlink ends the sweep with that file named.
TEST_F(ObservedContract, RemoveCondemnedNamesAStagedLeftoverItCannotRemove) {
    bc::file_system_test::write_raw(path_of(".ledger.bc-tmp-4242-7"), "leftover", 0600);
    script(fail_step(Operation::remove_condemned, Step::unlinkat, 1, EPERM));

    const auto swept = files().remove_condemned(root());

    ASSERT_FALSE(swept.has_value());
    EXPECT_EQ(swept.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(swept.error().path, path_of(".ledger.bc-tmp-4242-7").string());
    EXPECT_TRUE(bc::file_system_test::exists_raw(path_of(".ledger.bc-tmp-4242-7")));
}

// A write stopped before its rename leaves its staged file, which the sweep then clears: the kill-safety story
// end to end, through the library.
TEST_F(ObservedContract, AWriteStoppedBeforeItsRenameLeavesAStagedFileTheSweepClears) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(stop_at(Operation::write_file, Step::renameat, 1));
    const auto stopped = files().write_file(in("ledger"), bytes, kWrittenExactly0644);
    ASSERT_FALSE(stopped.has_value());
    ASSERT_EQ(staged_names_of(root_path(), "ledger").size(), 1U);

    const auto swept = files().remove_condemned(root());

    ASSERT_TRUE(swept.has_value()) << swept.error().message();
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("ledger"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
}

// -------------------------------------------------------------------------------------------------------------
// The portable-only engine: never a fast path's step, and the steps of the path every platform has instead
// -------------------------------------------------------------------------------------------------------------

// A `beneath` location is walked one component at a time, on every platform: neither fast path's step is reached,
// and the walk opens each component and then the leaf, where a fast path would open the whole parent in one call.
TEST_F(PortableOnlyContract, BeneathIsWalkedOneComponentAtATimeAndNeverTakesAFastPath) {
    lay_out_directories(root_path());
    const auto parent = files().open_directory(root(), FinalSymlink::refuse);
    ASSERT_TRUE(parent.has_value()) << parent.error().message();
    const std::uint64_t opens_before = count(Operation::open_directory, Step::openat);

    const auto deeper =
        files().open_directory(Location::beneath(*parent, relative("sub/deeper")), FinalSymlink::refuse);
    const auto through_a_symlink =
        files().open_directory(Location::beneath(*parent, relative("alias/deeper")), FinalSymlink::refuse);

    ASSERT_TRUE(deeper.has_value()) << deeper.error().message();
    EXPECT_EQ(deeper->opened_as().value(), path_of("sub/deeper").string());
    ASSERT_FALSE(through_a_symlink.has_value());
    EXPECT_EQ(through_a_symlink.error().kind, ErrorKind::symlink_refused);
    EXPECT_EQ(count(Operation::open_directory, Step::openat2), 0U);
    EXPECT_EQ(count(Operation::open_directory, Step::openat_nofollow_any), 0U);
    // `sub` and the leaf `deeper` for the first; `alias`, refused at its own open, for the second.
    EXPECT_EQ(count(Operation::open_directory, Step::openat) - opens_before, 3U);
}

// A copy moves its bytes through the read-and-write loop, on every platform: the clone's and the in-kernel copy's
// steps are never reached, every byte passes through the process, and the copy is byte for byte.
TEST_F(PortableOnlyContract, CopyMovesTheBytesThroughTheLoopAndNeverClonesOrCopiesInTheKernel) {
    const std::string content((std::size_t{3} * 1024 * 1024) + 1, 'c');
    bc::file_system_test::write_raw(path_of("source"), content, 0644);

    const auto achieved = files().copy_file(in("source"), in("copy"), kWrittenExactly0644);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("copy")), content);
    EXPECT_EQ(count(Operation::copy_file, Step::clone_file), 0U);
    EXPECT_EQ(count(Operation::copy_file, Step::copy_range), 0U);
    // The loop reads until the source answers no more bytes, so a file with any bytes takes two reads at least.
    EXPECT_GE(count(Operation::copy_file, Step::pread), 2U);
    EXPECT_GE(count(Operation::copy_file, Step::write), 1U);
    EXPECT_THAT(staged_names_of(root_path(), "copy"), IsEmpty());
}

// A durable rename is one renameat and a sync of each directory by fsync, the sync every platform has: neither the
// no-replace rename nor the full sync is reached.
TEST_F(PortableOnlyContract, DurableRenameIsOneRenameatAndSyncsBothDirectoriesByFsync) {
    bc::file_system_test::make_directory_raw(path_of("from"), 0755);
    bc::file_system_test::make_directory_raw(path_of("to"), 0755);
    bc::file_system_test::write_raw(path_of("from/ledger"), "content", 0644);

    const auto achieved =
        files().rename(in("from/ledger"), in("to/ledger"), ExistingDestination::replace, Durability::durable);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("to/ledger")), "content");
    EXPECT_EQ(count(Operation::rename, Step::rename_no_replace), 0U);
    EXPECT_EQ(count(Operation::rename, Step::renameat), 1U);
    EXPECT_EQ(count(Operation::rename, Step::fsync), 2U);
    EXPECT_EQ(count(Operation::rename, Step::fcntl_fullfsync), 0U);
}

// A durable write syncs its staged file and then its directory by fsync and takes the name with renameat.
TEST_F(PortableOnlyContract, DurableWriteSyncsByFsyncAndTakesTheNameWithRenameat) {
    const auto bytes = bytes_of("the ledger");

    const auto achieved = files().write_file(in("ledger"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value()) << achieved.error().message();
    EXPECT_EQ(*achieved, Durability::durable);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the ledger");
    EXPECT_EQ(count(Operation::write_file, Step::fsync), 2U);
    EXPECT_EQ(count(Operation::write_file, Step::fcntl_fullfsync), 0U);
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::rename_no_replace), 0U);
}

// What only the no-replace rename keeps is refused before any system call: a rename, a write and a copy told to
// refuse an existing destination, and a condemn. Not one step is counted (the walk of an `at` location begins
// with opening the root), nothing is staged, nothing moves, and a two-location refusal names the destination,
// whose promise it is.
TEST_F(PortableOnlyContract, WhatOnlyTheNoReplaceRenameKeepsIsUnsupportedBeforeAnySystemCall) {
    bc::file_system_test::write_raw(path_of("source"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);

    const auto renamed = files().rename(in("source"), in("current"), ExistingDestination::refuse, Durability::written);
    const auto written = files().write_file(in("fresh"), bytes_of("new"), kWrittenExactly0644Refusing);
    const auto copied = files().copy_file(in("source"), in("fresh"), kWrittenExactly0644Refusing);
    const auto condemned = files().condemn(in("source"));

    ASSERT_FALSE(renamed.has_value());
    EXPECT_EQ(renamed.error().kind, ErrorKind::unsupported);
    EXPECT_FALSE(renamed.error().step.has_value());
    EXPECT_EQ(renamed.error().failed_location, FailedLocation::second);
    EXPECT_EQ(renamed.error().path, path_of("current").string());
    ASSERT_FALSE(written.has_value());
    EXPECT_EQ(written.error().kind, ErrorKind::unsupported);
    EXPECT_FALSE(written.error().step.has_value());
    EXPECT_EQ(written.error().path, path_of("fresh").string());
    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().kind, ErrorKind::unsupported);
    EXPECT_FALSE(copied.error().step.has_value());
    EXPECT_EQ(copied.error().failed_location, FailedLocation::second);
    EXPECT_EQ(copied.error().path, path_of("fresh").string());
    ASSERT_FALSE(condemned.has_value());
    EXPECT_EQ(condemned.error().kind, ErrorKind::unsupported);
    EXPECT_FALSE(condemned.error().step.has_value());
    for (const Operation operation :
         {Operation::rename, Operation::write_file, Operation::copy_file, Operation::condemn}) {
        EXPECT_EQ(count(operation, Step::open), 0U) << name_of(operation);
        EXPECT_EQ(count(operation, Step::rename_no_replace), 0U) << name_of(operation);
        EXPECT_EQ(count(operation, Step::renameat), 0U) << name_of(operation);
    }
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("current", "source"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the old content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("source")), "the new content");
}

// The same operations under `replace` are unchanged by the missing rename: a rename and a write replace, a copy
// copies, each through renameat.
TEST_F(PortableOnlyContract, UnderReplaceTheSameOperationsReplaceThroughRenameat) {
    bc::file_system_test::write_raw(path_of("source"), "the new content", 0644);
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);
    bc::file_system_test::write_raw(path_of("copied"), "the old copy", 0644);

    const auto copied = files().copy_file(in("source"), in("copied"), kWrittenExactly0644);
    const auto written = files().write_file(in("current"), bytes_of("written"), kWrittenExactly0644);
    const auto renamed = files().rename(in("source"), in("current"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(copied.has_value()) << copied.error().message();
    ASSERT_TRUE(written.has_value()) << written.error().message();
    ASSERT_TRUE(renamed.has_value()) << renamed.error().message();
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("copied", "current"));
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("copied")), "the new content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the new content");
    EXPECT_EQ(count(Operation::copy_file, Step::renameat), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 1U);
    EXPECT_EQ(count(Operation::rename, Step::renameat), 1U);
}

// -------------------------------------------------------------------------------------------------------------
// Descriptors
// -------------------------------------------------------------------------------------------------------------

// Every operation, on its success path and on a failure path that comes after something was opened, gives back
// each descriptor it took. The fixture checks this for every case; this case runs every operation, several
// times, and then a write whose rename fails after the temporary file was opened, written and synced. The
// static_casts discard results this test does not judge.
TEST_F(ObservedContract, EveryOperationClosesWhatItOpened) {
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

// -------------------------------------------------------------------------------------------------------------
// The kill points of write_file: one case per step, as a table
// -------------------------------------------------------------------------------------------------------------

// "Stop without cleanup" makes the operation return from a step as the process would have if it had been killed
// there: the descriptors close, nothing is unlinked, renamed back or given its mode. Each row scripts that stop
// at one step of a durable write over an existing destination (or, for the no-replace rename, over none) and
// asserts the invariant every kill must keep: the destination holds the old content or the new, never a mixture
// and never an empty file; at most one staged file remains, and only before the rename, which is what a later
// sweep clears. The steps are the write's in order: the staged file's creation, the write of the bytes, its
// mode, its sync, its close, the rename, the directory's sync.
struct KillPoint {
    const char* label; // the case's name
    Step step;
    std::uint32_t occurrence;                 // of `step` within one write, after the walk's own openat calls
    ExistingDestination existing_destination; // `refuse` rows start with no destination
    bool destination_replaced;                // the rename had happened when the kill landed
    bool staged_file_remains;                 // the staged file had been created and not yet renamed
};

constexpr std::array<KillPoint, 8> kill_points{{
    {.label = "TheStagedFilesCreation",
     .step = Step::openat,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = false},
    {.label = "TheWriteOfTheBytes",
     .step = Step::write,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheMode",
     .step = Step::fchmod,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheSyncOfTheStagedFile",
     .step = durable_sync_step(),
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheCloseOfTheStagedFile",
     .step = Step::close,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheRenameIntoPlace",
     .step = Step::renameat,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheNoReplaceRenameIntoPlace",
     .step = Step::rename_no_replace,
     .occurrence = 1,
     .existing_destination = ExistingDestination::refuse,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheSyncOfTheDirectory",
     .step = durable_sync_step(),
     .occurrence = 2,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = true,
     .staged_file_remains = false},
}};

auto kill_point_name(const testing::TestParamInfo<KillPoint>& info) -> std::string {
    return info.param.label;
}

class WriteKillPoints : public Contract<bc::file_system_test::DescriptorBalancedWithParam<KillPoint>> {
protected:
    WriteKillPoints() : Contract(Engine::observed_for_tests()) {}

    auto script(const Fault& fault) -> void {
        const Result<void> added = engine().add_fault(fault);
        ASSERT_TRUE(added.has_value()) << added.error().message();
    }

    // The occurrence the row names, counted from the write's own first openat: the walk opens every component
    // of the test's directory with openat before the write opens anything of its own.
    [[nodiscard]] auto occurrence_of(const KillPoint& point) const -> std::uint32_t {
        if (point.step == Step::openat) {
            return static_cast<std::uint32_t>(components_of(root_path()) + point.occurrence);
        }
        return point.occurrence;
    }
};

INSTANTIATE_TEST_SUITE_P(Steps, WriteKillPoints, testing::ValuesIn(kill_points), kill_point_name);

TEST_P(WriteKillPoints, LeavesTheDestinationWholeOrReplacedAndNeverAMixture) {
    const KillPoint& point = GetParam();
    const bool starts_with_a_destination = point.existing_destination == ExistingDestination::replace;
    if (starts_with_a_destination) {
        bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    }
    const auto bytes = bytes_of("the new ledger");
    const WriteOptions options = starts_with_a_destination ? kDurableExactly0644 : kDurableExactly0644Refusing;
    script(stop_at(Operation::write_file, point.step, occurrence_of(point)));

    const auto achieved = files().write_file(in("ledger"), bytes, options);

    // The stop is reported as the step it landed on, with no system code: the operation did not fail, it ended.
    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(achieved.error().step, point.step);
    EXPECT_EQ(achieved.error().system_code, 0);

    // The invariant: the destination is whole, old or new; a staged file remains only before the rename.
    const std::vector<std::string> staged = staged_names_of(root_path(), "ledger");
    EXPECT_EQ(staged.size(), point.staged_file_remains ? 1U : 0U) << testing::PrintToString(staged);
    if (point.destination_replaced) {
        EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the new ledger");
    } else if (starts_with_a_destination) {
        EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    } else {
        EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("ledger")));
    }
    const std::vector<std::string> names = bc::file_system_test::names_raw(root_path());
    const std::size_t destinations = names.size() - staged.size();
    EXPECT_EQ(destinations, (point.destination_replaced || starts_with_a_destination) ? 1U : 0U)
        << testing::PrintToString(names);
}

} // namespace

namespace {

// -------------------------------------------------------------------------------------------------------------
// The kill points of copy_file: one case per step, as a table
// -------------------------------------------------------------------------------------------------------------

// The same invariant as the write's table, for a durable copy over an existing destination (or, for the
// no-replace rename, over none): the destination holds the old bytes or the whole new bytes, never a prefix,
// and at most one staged file remains, only before the rename. Every row forces the read-and-write loop by
// refusing the fast-path steps first (ENOTSUP, the platform's own answer where it offers none), so the rows are
// the same on every platform; the clone's own kill points are a case of their own above. The steps in order:
// the staged file's creation, the read of the source, the write, the mode, the sync, the close, the rename, the
// directory's sync.
constexpr std::array<KillPoint, 9> copy_kill_points{{
    {.label = "TheStagedFilesCreation",
     .step = Step::openat,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = false},
    {.label = "TheReadOfTheSource",
     .step = Step::pread,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheWriteOfTheBytes",
     .step = Step::write,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheMode",
     .step = Step::fchmod,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheSyncOfTheStagedFile",
     .step = durable_sync_step(),
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheCloseOfTheStagedFile",
     .step = Step::close,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheRenameIntoPlace",
     .step = Step::renameat,
     .occurrence = 1,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheNoReplaceRenameIntoPlace",
     .step = Step::rename_no_replace,
     .occurrence = 1,
     .existing_destination = ExistingDestination::refuse,
     .destination_replaced = false,
     .staged_file_remains = true},
    {.label = "TheSyncOfTheDirectory",
     .step = durable_sync_step(),
     .occurrence = 2,
     .existing_destination = ExistingDestination::replace,
     .destination_replaced = true,
     .staged_file_remains = false},
}};

class CopyKillPoints : public Contract<bc::file_system_test::DescriptorBalancedWithParam<KillPoint>> {
protected:
    CopyKillPoints() : Contract(Engine::observed_for_tests()) {}

    auto script(const Fault& fault) -> void {
        const Result<void> added = engine().add_fault(fault);
        ASSERT_TRUE(added.has_value()) << added.error().message();
    }

    // The occurrence the row names, counted from the copy's own first openat of the staged file: the two walks
    // open every component of the test's directory, and the source is opened once, before that.
    [[nodiscard]] auto occurrence_of(const KillPoint& point) const -> std::uint32_t {
        if (point.step == Step::openat) {
            return static_cast<std::uint32_t>((components_of(root_path()) * 2) + 1 + point.occurrence);
        }
        return point.occurrence;
    }
};

INSTANTIATE_TEST_SUITE_P(Steps, CopyKillPoints, testing::ValuesIn(copy_kill_points), kill_point_name);

TEST_P(CopyKillPoints, LeavesTheDestinationWholeOrReplacedAndNeverAMixture) {
    const KillPoint& point = GetParam();
    const bool starts_with_a_destination = point.existing_destination == ExistingDestination::replace;
    bc::file_system_test::write_raw(path_of("source"), "the new ledger", 0644);
    if (starts_with_a_destination) {
        bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    }
    const WriteOptions options = starts_with_a_destination ? kDurableExactly0644 : kDurableExactly0644Refusing;
    script(fail_step(Operation::copy_file, Step::clone_file, 1, ENOTSUP));
    script(fail_step(Operation::copy_file, Step::copy_range, 1, ENOTSUP));
    script(stop_at(Operation::copy_file, point.step, occurrence_of(point)));

    const auto achieved = files().copy_file(in("source"), in("ledger"), options);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(achieved.error().step, point.step);
    EXPECT_EQ(achieved.error().system_code, 0);

    const std::vector<std::string> staged = staged_names_of(root_path(), "ledger");
    EXPECT_EQ(staged.size(), point.staged_file_remains ? 1U : 0U) << testing::PrintToString(staged);
    if (point.destination_replaced) {
        EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the new ledger");
    } else if (starts_with_a_destination) {
        EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    } else {
        EXPECT_FALSE(bc::file_system_test::exists_raw(path_of("ledger")));
    }
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("source")), "the new ledger");
    const std::vector<std::string> names = bc::file_system_test::names_raw(root_path());
    const std::size_t destinations = names.size() - staged.size() - 1;
    EXPECT_EQ(destinations, (point.destination_replaced || starts_with_a_destination) ? 1U : 0U)
        << testing::PrintToString(names);
}

} // namespace
