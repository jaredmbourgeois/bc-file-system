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
#include <vector>

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

// The engine's observer on its own: what an observed engine records and what its scripted faults do, and that
// a live engine refuses the three observer calls. That an observed engine with nothing scripted changes
// nothing else is the contract suite's job, which runs every case against both engines.
//
// The fault slot and the step counters are real this stage, so the cases about them are green; a fault takes
// effect only at a step the engine makes, so each case that needs one is disabled until the stage whose
// operation makes that step.

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

using bc::file_system::Durability;
using bc::file_system::Engine;
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
using bc::file_system::ReadOptions;
using bc::file_system::Result;
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
constexpr WriteOptions kDurableExactly0644{.durability = Durability::durable,
                                           .permissions = Permissions::exactly(0644),
                                           .existing_destination = ExistingDestination::replace,
                                           .final_symlink = FinalSymlink::refuse};

// The system call a durable sync is made with on the platform the suite runs on. Stated here on its own, so
// that the suite, not the implementation, says what each platform owes.
#if defined(__APPLE__)
constexpr auto durable_sync_step() noexcept -> Step {
    return Step::fcntl_fullfsync;
}
#else
constexpr auto durable_sync_step() noexcept -> Step {
    return Step::fsync;
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
// A live engine refuses the observer's calls
// -------------------------------------------------------------------------------------------------------------

class LiveEngineTest : public bc::file_system_test::DescriptorBalanced {
protected:
    auto SetUp() -> void override { ASSERT_TRUE(engine_.has_value()) << engine_.error().message(); }

    [[nodiscard]] auto engine() -> Engine& { return engine_.value(); }

private:
    Result<Engine> engine_ = Engine::live();
};

TEST_F(LiveEngineTest, RefusesAddFaultWithInvalidArgument) {
    const Result<void> added = engine().add_fault(fail_step(Operation::write_file, Step::write, 1, ENOSPC));

    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(added.error().operation, Operation::observed_fault_add);
    EXPECT_FALSE(added.error().step.has_value());
    EXPECT_EQ(added.error().path, "");
}

TEST_F(LiveEngineTest, RefusesClearFaultsWithInvalidArgument) {
    const Result<void> cleared = engine().clear_faults();

    ASSERT_FALSE(cleared.has_value());
    EXPECT_EQ(cleared.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(cleared.error().operation, Operation::observed_faults_clear);
}

TEST_F(LiveEngineTest, RefusesStepCountWithInvalidArgument) {
    const Result<std::uint64_t> counted = engine().step_count(Operation::write_file, Step::write);

    ASSERT_FALSE(counted.has_value());
    EXPECT_EQ(counted.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(counted.error().operation, Operation::observed_step_count);
}

// -------------------------------------------------------------------------------------------------------------
// The observed engine's bookkeeping
// -------------------------------------------------------------------------------------------------------------

class ObserverTest : public bc::file_system_test::DescriptorBalanced {
protected:
    auto SetUp() -> void override {
        ASSERT_TRUE(directory_.has_value());
        ASSERT_TRUE(engine_.has_value()) << engine_.error().message();
        files_.emplace(engine_.value());
    }

    [[nodiscard]] auto files() -> FileSystem& { return *files_; }
    [[nodiscard]] auto engine() -> Engine& { return engine_.value(); }

    [[nodiscard]] auto in(std::string_view relative) const -> Location { return Location::at(path_of(relative)); }
    [[nodiscard]] auto path_of(std::string_view relative) const -> std::filesystem::path {
        return directory_->path() / relative;
    }
    [[nodiscard]] auto root_path() const -> const std::filesystem::path& { return directory_->path(); }

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

private:
    std::optional<TemporaryDirectory> directory_ = TemporaryDirectory::create_under(temporary_root());
    Result<Engine> engine_ = Engine::observed_for_tests();
    std::optional<FileSystem> files_;
};

TEST_F(ObserverTest, AFreshEngineHasCountedNothing) {
    EXPECT_EQ(count(Operation::read_file, Step::openat), 0U);
    EXPECT_EQ(count(Operation::write_file, Step::write), 0U);
    EXPECT_EQ(count(Operation::remove_tree, Step::unlinkat), 0U);
    EXPECT_EQ(count(Operation::absolute_path_from, Step::identity_check), 0U);
}

TEST_F(ObserverTest, AFaultWithOccurrenceZeroIsRefused) {
    const Result<void> added = engine().add_fault(fail_step(Operation::write_file, Step::write, 0, ENOSPC));

    ASSERT_FALSE(added.has_value());
    EXPECT_EQ(added.error().kind, ErrorKind::invalid_argument);
    EXPECT_EQ(added.error().operation, Operation::observed_fault_add);
}

// A failing step needs an errno to fail with; a stop answers nothing, so a code there is a mistake.
TEST_F(ObserverTest, AFailFaultNeedsASystemCodeAndAStopFaultTakesNone) {
    const Result<void> fail_without_code = engine().add_fault(fail_step(Operation::write_file, Step::write, 1, 0));
    const Result<void> stop_with_code = engine().add_fault(Fault{.operation = Operation::write_file,
                                                                 .step = Step::write,
                                                                 .occurrence = 1,
                                                                 .action = FaultAction::stop,
                                                                 .system_code = EIO});
    const Result<void> fail_with_code = engine().add_fault(fail_step(Operation::write_file, Step::write, 1, EIO));
    const Result<void> stop_without_code = engine().add_fault(stop_at(Operation::write_file, Step::write, 1));

    ASSERT_FALSE(fail_without_code.has_value());
    EXPECT_EQ(fail_without_code.error().kind, ErrorKind::invalid_argument);
    ASSERT_FALSE(stop_with_code.has_value());
    EXPECT_EQ(stop_with_code.error().kind, ErrorKind::invalid_argument);
    EXPECT_TRUE(fail_with_code.has_value());
    EXPECT_TRUE(stop_without_code.has_value());
}

TEST_F(ObserverTest, TheSlotHoldsSixtyFourFaultsAndTheNextIsResourceExhausted) {
    for (std::uint32_t occurrence = 1; occurrence <= 64; ++occurrence) {
        script(fail_step(Operation::write_file, Step::write, occurrence, ENOSPC));
    }

    const Result<void> one_more = engine().add_fault(fail_step(Operation::write_file, Step::write, 65, ENOSPC));

    ASSERT_FALSE(one_more.has_value());
    EXPECT_EQ(one_more.error().kind, ErrorKind::resource_exhausted);
    EXPECT_EQ(one_more.error().operation, Operation::observed_fault_add);
}

TEST_F(ObserverTest, ClearFaultsMakesRoomAgain) {
    for (std::uint32_t occurrence = 1; occurrence <= 64; ++occurrence) {
        script(fail_step(Operation::write_file, Step::write, occurrence, ENOSPC));
    }

    const Result<void> cleared = engine().clear_faults();
    const Result<void> after_clearing = engine().add_fault(fail_step(Operation::write_file, Step::write, 1, ENOSPC));

    EXPECT_TRUE(cleared.has_value());
    EXPECT_TRUE(after_clearing.has_value());
}

// The step counts are the engine's lifetime record; forgetting the faults does not reset them.
// stage 13 status-and-reads
TEST_F(ObserverTest, DISABLED_ClearFaultsKeepsTheStepCounts) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    const auto status = files().status(in("file"), FinalSymlink::refuse);
    ASSERT_TRUE(status.has_value());
    const std::uint64_t before = count(Operation::status, Step::fstatat);
    ASSERT_GE(before, 1U);

    const Result<void> cleared = engine().clear_faults();

    EXPECT_TRUE(cleared.has_value());
    EXPECT_EQ(count(Operation::status, Step::fstatat), before);
}

// -------------------------------------------------------------------------------------------------------------
// A fault takes effect at its step
// -------------------------------------------------------------------------------------------------------------

// stage 14 writes-in-c
TEST_F(ObserverTest, DISABLED_AScriptedStepFailsWithItsCodeAndNothingIsWritten) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, Step::openat, 1, ENOSPC));

    const auto achieved = files().write_file(in("file"), bytes, kDurableExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(achieved.error().kind, ErrorKind::no_space);
    EXPECT_EQ(achieved.error().system_code, ENOSPC);
    EXPECT_EQ(achieved.error().step, Step::openat);
    EXPECT_EQ(achieved.error().path, path_of("file").string());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), IsEmpty());
}

// stage 13 status-and-reads
TEST_F(ObserverTest, DISABLED_OnlyTheNthOccurrenceFails) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    script(fail_step(Operation::read_file, Step::openat, 2, EIO));

    const auto first = files().read_file(in("file"), kWholeFileRefusingSymlink);
    const auto second = files().read_file(in("file"), kWholeFileRefusingSymlink);
    const auto third = files().read_file(in("file"), kWholeFileRefusingSymlink);

    EXPECT_TRUE(first.has_value());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(second.error().system_code, EIO);
    EXPECT_TRUE(third.has_value());
}

// A fault names one operation and one step: another operation reaching the same step, and the same operation
// reaching another step, both run.
// stage 13 status-and-reads
TEST_F(ObserverTest, DISABLED_AScriptNamesOneOperationAndOneStep) {
    bc::file_system_test::write_raw(path_of("scripted"), "content", 0644);
    script(fail_step(Operation::read_file, Step::openat, 1, EACCES));

    const auto other_operation = files().status(in("scripted"), FinalSymlink::refuse);
    const auto scripted = files().read_file(in("scripted"), kWholeFileRefusingSymlink);

    EXPECT_TRUE(other_operation.has_value());
    ASSERT_FALSE(scripted.has_value());
    EXPECT_EQ(scripted.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(scripted.error().system_code, EACCES);
}

// The scenario the observer exists for: code that stages a file and renames it into place is shown a rename
// that fails after the staged file was written, and the test reads what that code left behind.
// stage 16 moves-and-copies-in-c
TEST_F(ObserverTest, DISABLED_ARenameCanFailAfterTheStagedFileWasWritten) {
    bc::file_system_test::write_raw(path_of("current"), "the old content", 0644);
    const auto bytes = bytes_of("the new content");
    script(fail_step(Operation::rename, Step::renameat, 1, EIO));

    const auto staged = files().write_file(in("staged"), bytes, kWrittenExactly0644);
    const auto renamed = files().rename(in("staged"), in("current"), ExistingDestination::replace, Durability::written);

    ASSERT_TRUE(staged.has_value());
    ASSERT_FALSE(renamed.has_value());
    EXPECT_EQ(renamed.error().kind, ErrorKind::io_failed);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("current")), "the old content");
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("staged")), "the new content");
}

// stage 16 moves-and-copies-in-c
TEST_F(ObserverTest, DISABLED_RenameAndCopyErrorsNameTheirDestination) {
    bc::file_system_test::write_raw(path_of("source"), "content", 0644);
    script(fail_step(Operation::copy_file, Step::openat, 1, ENOSPC));
    script(fail_step(Operation::rename, Step::renameat, 1, EACCES));

    const auto copied = files().copy_file(in("source"), in("copy"), kWrittenExactly0644);
    const auto renamed = files().rename(in("source"), in("renamed"), ExistingDestination::replace, Durability::written);

    ASSERT_FALSE(copied.has_value());
    EXPECT_EQ(copied.error().kind, ErrorKind::no_space);
    EXPECT_EQ(copied.error().path, path_of("copy").string());
    ASSERT_FALSE(renamed.has_value());
    EXPECT_EQ(renamed.error().kind, ErrorKind::permission_denied);
    EXPECT_EQ(renamed.error().path, path_of("renamed").string());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("source"));
}

// Every step is counted across the engine's lifetime, whether the operation succeeded, failed for real or
// failed by script.
// stage 16 moves-and-copies-in-c
TEST_F(ObserverTest, DISABLED_EveryStepIsCountedAcrossTheEnginesLifetime) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::remove_file, Step::unlinkat, 1, EACCES));

    const auto written = files().write_file(in("file"), bytes, kWrittenExactly0644);
    const auto missing = files().read_file(in("absent"), kWholeFileRefusingSymlink);
    const auto refused = files().remove_file(in("file"));
    const auto moved = files().rename(in("file"), in("moved"), ExistingDestination::replace, Durability::written);

    EXPECT_TRUE(written.has_value());
    EXPECT_FALSE(missing.has_value());
    EXPECT_FALSE(refused.has_value());
    EXPECT_TRUE(moved.has_value());
    EXPECT_EQ(count(Operation::write_file, Step::write), 1U);
    EXPECT_GE(count(Operation::read_file, Step::openat), 1U);
    EXPECT_EQ(count(Operation::remove_file, Step::unlinkat), 1U);
    EXPECT_EQ(count(Operation::rename, Step::renameat), 1U);
}

// Each operation counts under its own name: resolving a location opens its parent directory, so every
// operation reaches `open` at least once.
// stage 17 locks-links-condemn
TEST_F(ObserverTest, DISABLED_EachOperationCountsUnderItsOwnName) {
    bc::file_system_test::write_raw(path_of("file"), "content", 0644);
    const auto bytes = bytes_of("more");
    const Location root = Location::at(root_path());

    static_cast<void>(files().read_file(in("file"), kWholeFileRefusingSymlink));
    static_cast<void>(files().write_file(in("written"), bytes, kWrittenExactly0644));
    static_cast<void>(files().append(in("file"), bytes, Durability::written, Permissions::exactly(0644)));
    static_cast<void>(files().sync(in("file")));
    static_cast<void>(files().list_directory(root, FinalSymlink::refuse));
    static_cast<void>(files().status(in("file"), FinalSymlink::refuse));
    static_cast<void>(files().create_directory(in("folder"), Permissions::exactly(0755)));
    static_cast<void>(files().create_directories(in("a/b"), Permissions::exactly(0755)));
    static_cast<void>(files().copy_file(in("file"), in("copy"), kWrittenExactly0644));
    static_cast<void>(files().rename(in("copy"), in("renamed"), ExistingDestination::replace, Durability::written));
    static_cast<void>(files().set_permissions(in("renamed"), 0600, FinalSymlink::refuse));
    static_cast<void>(files().create_symlink("file", in("link")));
    static_cast<void>(files().read_symlink(in("link")));
    static_cast<void>(files().try_lock(in("file")));
    static_cast<void>(files().remove_file(in("renamed")));
    static_cast<void>(files().remove_directory(in("folder")));
    static_cast<void>(files().remove_tree(in("a")));
    static_cast<void>(files().create_temporary_directory(root, Permissions::exactly(0700)));
    static_cast<void>(files().condemn(in("link")));
    static_cast<void>(files().remove_condemned(root));
    static_cast<void>(files().open_directory(root, FinalSymlink::refuse));

    const std::vector<Operation> operations = {Operation::read_file,        Operation::write_file,
                                               Operation::append,           Operation::sync,
                                               Operation::list_directory,   Operation::status,
                                               Operation::create_directory, Operation::create_directories,
                                               Operation::copy_file,        Operation::rename,
                                               Operation::set_permissions,  Operation::create_symlink,
                                               Operation::read_symlink,     Operation::try_lock,
                                               Operation::remove_file,      Operation::remove_directory,
                                               Operation::remove_tree,      Operation::create_temporary_directory,
                                               Operation::condemn,          Operation::remove_condemned,
                                               Operation::open_directory};
    for (const Operation operation : operations) {
        EXPECT_GE(count(operation, Step::open), 1U) << bc::file_system::name_of(operation);
    }
}

// stage 14 writes-in-c
TEST_F(ObserverTest, DISABLED_LetsEveryStepRunWhenNothingIsScripted) {
    const auto bytes = bytes_of("content");

    const auto achieved = files().write_file(in("file"), bytes, kDurableExactly0644);
    const auto back = files().read_file(in("file"), kWholeFileRefusingSymlink);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::durable);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(text_of(*back), "content");
}

// stage 14 writes-in-c
TEST_F(ObserverTest, DISABLED_FailsOnlyTheNthOccurrenceOfTheScriptedStep) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, Step::write, 2, ENOSPC));

    const auto first = files().write_file(in("one"), bytes, kWrittenExactly0644);
    const auto second = files().write_file(in("two"), bytes, kWrittenExactly0644);
    const auto third = files().write_file(in("three"), bytes, kWrittenExactly0644);

    EXPECT_TRUE(first.has_value());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, ErrorKind::no_space);
    EXPECT_TRUE(third.has_value());
    EXPECT_THAT(bc::file_system_test::names_raw(root_path()), ElementsAre("one", "three"));
}

// An occurrence counts across locations: the second durable sync of one write is the directory's, not a
// second file's.
// stage 14 writes-in-c
TEST_F(ObserverTest, DISABLED_CountsAStepAcrossLocations) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, durable_sync_step(), 2, EIO));

    const auto achieved = files().write_file(in("file"), bytes, kDurableExactly0644);

    ASSERT_TRUE(achieved.has_value());
    EXPECT_EQ(*achieved, Durability::written);
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("file")), "content");
}

// A step that was failed by script still counts: the count records what the operation reached, not what the
// system did.
// stage 14 writes-in-c
TEST_F(ObserverTest, DISABLED_CountsAStepWhetherOrNotItWasFailed) {
    const auto bytes = bytes_of("content");
    script(fail_step(Operation::write_file, Step::renameat, 1, EIO));

    const auto achieved = files().write_file(in("file"), bytes, kWrittenExactly0644);

    ASSERT_FALSE(achieved.has_value());
    EXPECT_EQ(count(Operation::write_file, Step::write), 1U);
    EXPECT_EQ(count(Operation::write_file, Step::renameat), 1U);
}

// "Stop without cleanup": the operation returns from the step as if the process had been killed there, so
// the staged temporary file is left beside the untouched destination, which is what a kill-point case reads.
// stage 14 writes-in-c
TEST_F(ObserverTest, DISABLED_AStopFaultReturnsFromTheStepWithoutCleanup) {
    bc::file_system_test::write_raw(path_of("ledger"), "the old ledger", 0644);
    const auto bytes = bytes_of("the new ledger");
    script(stop_at(Operation::write_file, Step::renameat, 1));

    const auto achieved = files().write_file(in("ledger"), bytes, kWrittenExactly0644);

    EXPECT_FALSE(achieved.has_value());
    EXPECT_EQ(bc::file_system_test::read_raw(path_of("ledger")), "the old ledger");
    EXPECT_EQ(bc::file_system_test::names_raw(root_path()).size(), 2U);
}

} // namespace
