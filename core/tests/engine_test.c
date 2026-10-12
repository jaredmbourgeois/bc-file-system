/* engine_test.c — the C-only test of the engine: its platform probes and the observer's mechanics.
 *
 * One `main` over cases, registered with add_test. The engine's private face (engine.h) is reached directly, so
 * the observer's two hooks are driven without an operation in between: a step is counted, a fault fires at its
 * occurrence and not before, fires once, names one operation and one step, a stop is a verdict, clearing the
 * faults keeps the counts, and a live engine neither counts nor fires. The probe fields are printed for the
 * record and asserted only where the platform guarantees the answer. Nothing is reached for: the probes touch
 * no file, and the two platform operations exercised here are handed an invalid descriptor.
 */
#include <bc_file_system/bc_file_system.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "engine.h"
#include "platform.h"

/* How many checks ran and how many failed; main reports both. Handed to every check rather than kept in a
 * global, so the file reads like the library it tests. */
typedef struct report {
    int checked;
    int failed;
} report;

/* `held` is one comparison or one predicate, never a chain: a chain under && is an int in C, and the C lint set
 * reports its conversion, so a case that needs two facts states two checks. */
static void expect(report *tally, bool held, const char *what, const char *detail) {
    tally->checked += 1;
    if (!held) {
        tally->failed += 1;
        printf("FAILED: %s: %s\n", what, detail);
    }
}

/* The engines every case starts from; a factory that fails is a failed check and a NULL engine, which the cases
 * guard against. */
static bc_file_system_engine *create_live(report *tally) {
    bc_file_system_engine *engine = nullptr;
    bc_file_system_error error;
    expect(tally, bc_file_system_engine_create_live(&engine, &error) == 0, "engine_create_live succeeds", "");
    return engine;
}

static bc_file_system_engine *create_observed(report *tally) {
    bc_file_system_engine *engine = nullptr;
    bc_file_system_error error;
    expect(tally, bc_file_system_engine_create_observed_for_tests(&engine, &error) == 0,
           "engine_create_observed_for_tests succeeds", "");
    return engine;
}

/* A fault that fails `step` of `operation` with `system_code` the `occurrence`-th time it is reached. */
static bc_file_system_fault fail_step(int32_t operation, int32_t step, uint32_t occurrence, int32_t system_code) {
    return (bc_file_system_fault){.operation = operation,
                                  .step = step,
                                  .occurrence = occurrence,
                                  .action = bc_file_system_fault_action_fail,
                                  .system_code = system_code};
}

static bc_file_system_fault stop_at(int32_t operation, int32_t step, uint32_t occurrence) {
    return (bc_file_system_fault){.operation = operation,
                                  .step = step,
                                  .occurrence = occurrence,
                                  .action = bc_file_system_fault_action_stop,
                                  .system_code = 0};
}

static void script(report *tally, bc_file_system_engine *engine, bc_file_system_fault fault, const char *what) {
    bc_file_system_error error;
    expect(tally, bc_file_system_observed_fault_add(engine, &fault, &error) == 0, "fault_add succeeds", what);
}

/* The verdict of one step, through the hook every operation calls. */
static bc_file_system_step_verdict step(bc_file_system_engine *engine, int32_t operation, int32_t step_made) {
    bc_file_system_step_verdict verdict;
    bc_file_system_engine_before_step(engine, operation, step_made, &verdict);
    return verdict;
}

static uint64_t count_of(report *tally, const bc_file_system_engine *engine, int32_t operation, int32_t step_made) {
    uint64_t count = 0;
    bc_file_system_error error;
    expect(tally, bc_file_system_observed_step_count(engine, operation, step_made, &count, &error) == 0,
           "step_count succeeds", "");
    return count;
}

static const char *yes_or_no(bool value) {
    if (value) {
        return "yes";
    }
    return "no";
}

/* One probe against what the platform guarantees for it. */
static void expect_fast_path(report *tally, bool probed, bool guaranteed, const char *what) {
    expect(tally, probed == guaranteed, what, "");
}

static bool is_proceed(bc_file_system_step_verdict verdict) {
    if (verdict.action != bc_file_system_step_action_proceed) {
        return false;
    }
    return verdict.system_code == 0;
}

/* ---- The platform probes ------------------------------------------------------------------------------------ */

/* What each platform guarantees about its probes; the rest is reported, not asserted. Whole-function alternatives
 * so the suite, not the implementation, says what each platform owes. */
#ifdef __APPLE__
static void expect_platform_fast_paths(report *tally, const bc_file_system_platform_fast_paths *fast_paths) {
    expect_fast_path(tally, fast_paths->has_openat2, false, "Apple: no openat2");
    expect_fast_path(tally, fast_paths->has_openat_nofollow_any, true, "Apple: O_NOFOLLOW_ANY by the SDK");
    expect_fast_path(tally, fast_paths->has_rename_no_replace, true, "Apple: renameatx_np(RENAME_EXCL) by the SDK");
    expect_fast_path(tally, fast_paths->has_clone, true, "Apple: clonefileat by the SDK");
    expect_fast_path(tally, fast_paths->has_copy_range, false, "Apple: no copy_file_range");
    expect_fast_path(tally, fast_paths->has_full_fsync, true, "Apple: F_FULLFSYNC by the SDK");
}
#elifdef __EMSCRIPTEN__
static void expect_platform_fast_paths(report *tally, const bc_file_system_platform_fast_paths *fast_paths) {
    expect_fast_path(tally, fast_paths->has_openat2, false, "Emscripten: no openat2");
    expect_fast_path(tally, fast_paths->has_openat_nofollow_any, false, "Emscripten: no O_NOFOLLOW_ANY");
    expect_fast_path(tally, fast_paths->has_rename_no_replace, false, "Emscripten: no no-replace rename");
    expect_fast_path(tally, fast_paths->has_clone, false, "Emscripten: no clone");
    expect_fast_path(tally, fast_paths->has_copy_range, false, "Emscripten: no copy_file_range");
    expect_fast_path(tally, fast_paths->has_full_fsync, false, "Emscripten: no sync at all");
}
#elifdef __ANDROID__
static void expect_platform_fast_paths(report *tally, const bc_file_system_platform_fast_paths *fast_paths) {
    expect_fast_path(tally, fast_paths->has_openat2, false, "Android: openat2 is never probed");
    expect_fast_path(tally, fast_paths->has_openat_nofollow_any, false, "Android: no O_NOFOLLOW_ANY");
    expect_fast_path(tally, fast_paths->has_full_fsync, false, "Android: fsync is the durable sync");
}
#else
static void expect_platform_fast_paths(report *tally, const bc_file_system_platform_fast_paths *fast_paths) {
    expect_fast_path(tally, fast_paths->has_openat_nofollow_any, false, "Linux: no O_NOFOLLOW_ANY");
    expect_fast_path(tally, fast_paths->has_full_fsync, false, "Linux: fsync is the durable sync");
}
#endif

static void test_live_engine_is_created_released_and_probed(report *tally) {
    bc_file_system_engine *engine = create_live(tally);
    if (engine == nullptr) {
        return;
    }
    const bc_file_system_platform_fast_paths *fast_paths = bc_file_system_engine_fast_paths(engine);
    printf("fast paths: openat2=%s openat_nofollow_any=%s rename_no_replace=%s clone=%s copy_range=%s full_fsync=%s\n",
           yes_or_no(fast_paths->has_openat2), yes_or_no(fast_paths->has_openat_nofollow_any),
           yes_or_no(fast_paths->has_rename_no_replace), yes_or_no(fast_paths->has_clone),
           yes_or_no(fast_paths->has_copy_range), yes_or_no(fast_paths->has_full_fsync));
    expect_platform_fast_paths(tally, fast_paths);
    bc_file_system_engine_release(engine);
}

static void test_observed_engine_probes_the_same_way(report *tally) {
    bc_file_system_engine *live = create_live(tally);
    bc_file_system_engine *observed = create_observed(tally);
    if (live == nullptr || observed == nullptr) {
        bc_file_system_engine_release(live);
        bc_file_system_engine_release(observed);
        return;
    }
    const bc_file_system_platform_fast_paths *from_live = bc_file_system_engine_fast_paths(live);
    const bc_file_system_platform_fast_paths *from_observed = bc_file_system_engine_fast_paths(observed);
    expect(tally, from_live->has_openat2 == from_observed->has_openat2, "both engines probe openat2 alike", "");
    expect(tally, from_live->has_rename_no_replace == from_observed->has_rename_no_replace,
           "both engines probe the no-replace rename alike", "");
    expect(tally, from_live->has_clone == from_observed->has_clone, "both engines probe clone alike", "");
    expect(tally, from_live->has_copy_range == from_observed->has_copy_range, "both engines probe copy_range alike",
           "");
    expect(tally, from_live->has_full_fsync == from_observed->has_full_fsync, "both engines probe full_fsync alike",
           "");
    bc_file_system_engine_release(live);
    bc_file_system_engine_release(observed);
}

/* ---- The platform operations ---------------------------------------------------------------------------------- */

/* The operations with bodies, handed an invalid descriptor so nothing is touched: a platform with the call
 * reports the descriptor (never ENOTSUP, never success); Emscripten answers ENOTSUP outright. */
#ifdef __EMSCRIPTEN__
static void expect_platform_operations(report *tally) {
    expect(tally, bc_file_system_platform_sync_descriptor(-1) == ENOTSUP, "Emscripten: sync is not offered", "");
    expect(tally, bc_file_system_platform_rename_no_replace(-1, "a", -1, "b") == ENOTSUP,
           "Emscripten: the no-replace rename is not offered", "");
    expect(tally, !bc_file_system_platform_enforces_locks(), "Emscripten: flock is a stub, so no lock is held", "");
}
#else
static void expect_platform_operations(report *tally) {
    const int sync_code = bc_file_system_platform_sync_descriptor(-1);
    expect(tally, sync_code == EBADF, "sync_descriptor reports an invalid descriptor as EBADF", "");
    const int rename_code = bc_file_system_platform_rename_no_replace(-1, "a", -1, "b");
    expect(tally, rename_code != 0, "rename_no_replace on an invalid descriptor fails", "");
    expect(tally, rename_code != ENOTSUP, "rename_no_replace on an invalid descriptor is not ENOTSUP", "");
    expect(tally, bc_file_system_platform_enforces_locks(), "flock holds a lock on this platform", "");
}
#endif

static void test_platform_operations(report *tally) {
    expect_platform_operations(tally);
    /* The three operations with a fast path are real where the probe says the platform has one: handed an
     * invalid descriptor each reports the descriptor (never ENOTSUP, never success, nothing stored); where the
     * platform has none, each answers ENOTSUP outright. */
    bc_file_system_engine *engine = create_live(tally);
    if (engine == nullptr) {
        return;
    }
    const bc_file_system_platform_fast_paths *fast_paths = bc_file_system_engine_fast_paths(engine);
    int descriptor = -1;
    const int open_beneath_code = bc_file_system_platform_open_beneath(-1, "a/b", 0, &descriptor);
    if (fast_paths->has_openat2 || fast_paths->has_openat_nofollow_any) {
        expect(tally, open_beneath_code == EBADF, "open_beneath reports an invalid descriptor as EBADF", "");
    } else {
        expect(tally, open_beneath_code == ENOTSUP, "open_beneath is not offered on this platform", "");
    }
    expect(tally, descriptor == -1, "open_beneath stores nothing on failure", "");
    /* The clone is asked to make "copy" under an invalid directory from an invalid source: the first call either
     * body makes (fclonefileat; the exclusive create before FICLONE) refuses the directory. */
    const int clone_code = bc_file_system_platform_clone_file(-1, -1, "copy", 0644);
    if (fast_paths->has_clone) {
        expect(tally, clone_code == EBADF, "clone_file reports an invalid descriptor as EBADF", "");
    } else {
        expect(tally, clone_code == ENOTSUP, "clone_file is not offered on this platform", "");
    }
    size_t copied = 0;
    const int copy_range_code = bc_file_system_platform_copy_range(-1, -1, 16, &copied);
    if (fast_paths->has_copy_range) {
        expect(tally, copy_range_code == EBADF, "copy_range reports an invalid descriptor as EBADF", "");
    } else {
        expect(tally, copy_range_code == ENOTSUP, "copy_range is not offered on this platform", "");
    }
    expect(tally, copied == 0, "copy_range stores nothing on failure", "");
    bc_file_system_engine_release(engine);
}

/* ---- The observer's mechanics --------------------------------------------------------------------------------- */

static void test_an_observed_engine_counts_every_step(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t read_file = bc_file_system_operation_read_file;
    const int32_t write_file = bc_file_system_operation_write_file;
    bc_file_system_engine_before_operation(engine, read_file);
    expect(tally, is_proceed(step(engine, read_file, bc_file_system_step_openat)), "an unscripted step proceeds", "");
    expect(tally, is_proceed(step(engine, read_file, bc_file_system_step_openat)), "an unscripted step proceeds", "");
    expect(tally, is_proceed(step(engine, read_file, bc_file_system_step_read)), "an unscripted step proceeds", "");
    bc_file_system_engine_before_operation(engine, write_file);
    expect(tally, is_proceed(step(engine, write_file, bc_file_system_step_openat)), "an unscripted step proceeds", "");

    expect(tally, count_of(tally, engine, read_file, bc_file_system_step_openat) == 2, "read_file reached openat twice",
           "");
    expect(tally, count_of(tally, engine, read_file, bc_file_system_step_read) == 1, "read_file reached read once", "");
    expect(tally, count_of(tally, engine, write_file, bc_file_system_step_openat) == 1,
           "write_file reached openat once, under its own name", "");
    expect(tally, count_of(tally, engine, write_file, bc_file_system_step_read) == 0, "write_file never reached read",
           "");
    bc_file_system_engine_release(engine);
}

static void test_a_fault_fires_at_its_occurrence_and_not_before(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t write_file = bc_file_system_operation_write_file;
    script(tally, engine, fail_step(write_file, bc_file_system_step_write, 2, ENOSPC), "write fails the 2nd time");
    bc_file_system_engine_before_operation(engine, write_file);

    const bc_file_system_step_verdict first = step(engine, write_file, bc_file_system_step_write);
    const bc_file_system_step_verdict second = step(engine, write_file, bc_file_system_step_write);
    const bc_file_system_step_verdict third = step(engine, write_file, bc_file_system_step_write);

    expect(tally, is_proceed(first), "the first occurrence proceeds", "");
    expect(tally, second.action == bc_file_system_step_action_fail, "the second occurrence fails", "");
    expect(tally, second.system_code == ENOSPC, "the failure carries the scripted errno", "");
    expect(tally, is_proceed(third), "the third occurrence proceeds: the fault fired once", "");
    expect(tally, count_of(tally, engine, write_file, bc_file_system_step_write) == 3,
           "a failed step is counted like any other", "");
    bc_file_system_engine_release(engine);
}

static void test_a_fault_names_one_operation_and_one_step(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t read_file = bc_file_system_operation_read_file;
    const int32_t status = bc_file_system_operation_status;
    script(tally, engine, fail_step(read_file, bc_file_system_step_openat, 1, EACCES), "read_file's openat fails");
    bc_file_system_engine_before_operation(engine, status);
    bc_file_system_engine_before_operation(engine, read_file);

    expect(tally, is_proceed(step(engine, status, bc_file_system_step_openat)),
           "another operation reaching the same step proceeds", "");
    expect(tally, is_proceed(step(engine, read_file, bc_file_system_step_fstat)),
           "the same operation reaching another step proceeds", "");
    const bc_file_system_step_verdict scripted = step(engine, read_file, bc_file_system_step_openat);
    expect(tally, scripted.action == bc_file_system_step_action_fail, "the scripted operation and step fail", "");
    expect(tally, scripted.system_code == EACCES, "with the scripted errno", "");
    bc_file_system_engine_release(engine);
}

static void test_a_stop_is_a_verdict(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t write_file = bc_file_system_operation_write_file;
    script(tally, engine, stop_at(write_file, bc_file_system_step_renameat, 1), "stop at the rename");
    bc_file_system_engine_before_operation(engine, write_file);

    expect(tally, is_proceed(step(engine, write_file, bc_file_system_step_write)), "the write proceeds", "");
    const bc_file_system_step_verdict stopped = step(engine, write_file, bc_file_system_step_renameat);
    expect(tally, stopped.action == bc_file_system_step_action_stop, "the rename is the stop", "");
    expect(tally, stopped.system_code == 0, "a stop carries no errno", "");
    expect(tally, count_of(tally, engine, write_file, bc_file_system_step_renameat) == 1,
           "the stopped step was counted", "");
    bc_file_system_engine_release(engine);
}

static void test_two_faults_fire_independently(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t copy_file = bc_file_system_operation_copy_file;
    const int32_t rename = bc_file_system_operation_rename;
    script(tally, engine, fail_step(copy_file, bc_file_system_step_openat, 1, ENOSPC), "copy's open fails");
    script(tally, engine, fail_step(rename, bc_file_system_step_renameat, 1, EACCES), "rename fails");
    bc_file_system_engine_before_operation(engine, copy_file);
    bc_file_system_engine_before_operation(engine, rename);

    const bc_file_system_step_verdict copied = step(engine, copy_file, bc_file_system_step_openat);
    const bc_file_system_step_verdict renamed = step(engine, rename, bc_file_system_step_renameat);
    expect(tally, copied.system_code == ENOSPC, "the copy's fault fired with its own errno", "");
    expect(tally, renamed.system_code == EACCES, "the rename's fault fired with its own errno", "");
    bc_file_system_engine_release(engine);
}

static void test_a_fired_fault_leaves_the_slot(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t write_file = bc_file_system_operation_write_file;
    for (uint32_t occurrence = 1; occurrence <= 64; ++occurrence) {
        script(tally, engine, fail_step(write_file, bc_file_system_step_write, occurrence, ENOSPC), "fill the slot");
    }
    bc_file_system_engine_before_operation(engine, write_file);
    expect(tally, step(engine, write_file, bc_file_system_step_write).action == bc_file_system_step_action_fail,
           "the first of the sixty-four fires", "");

    const bc_file_system_fault one_more = fail_step(write_file, bc_file_system_step_write, 65, ENOSPC);
    bc_file_system_error error;
    expect(tally, bc_file_system_observed_fault_add(engine, &one_more, &error) == 0,
           "the fired fault left the slot, so a sixty-fourth fits again", "");
    bc_file_system_engine_release(engine);
}

static void test_clearing_the_faults_keeps_the_counts(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t status = bc_file_system_operation_status;
    script(tally, engine, fail_step(status, bc_file_system_step_fstatat, 2, EIO), "fstatat fails the 2nd time");
    bc_file_system_engine_before_operation(engine, status);
    expect(tally, is_proceed(step(engine, status, bc_file_system_step_fstatat)), "the first fstatat proceeds", "");

    bc_file_system_error error;
    expect(tally, bc_file_system_observed_faults_clear(engine, &error) == 0, "faults_clear succeeds", "");
    expect(tally, count_of(tally, engine, status, bc_file_system_step_fstatat) == 1, "the count survives the clearing",
           "");
    expect(tally, is_proceed(step(engine, status, bc_file_system_step_fstatat)),
           "the cleared fault no longer fires at its occurrence", "");
    expect(tally, count_of(tally, engine, status, bc_file_system_step_fstatat) == 2,
           "and counting goes on from where it was", "");
    bc_file_system_engine_release(engine);
}

static void test_a_fault_with_occurrence_zero_is_refused(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const bc_file_system_fault fault =
        fail_step(bc_file_system_operation_write_file, bc_file_system_step_write, 0, EIO);
    bc_file_system_error error;
    expect(tally,
           bc_file_system_observed_fault_add(engine, &fault, &error) == bc_file_system_error_kind_invalid_argument,
           "occurrence 0 is invalid_argument", "");
    expect(tally, error.operation == bc_file_system_operation_observed_fault_add, "named as fault_add's refusal", "");
    bc_file_system_engine_release(engine);
}

static void test_a_live_engine_refuses_the_observer_calls(report *tally) {
    bc_file_system_engine *engine = create_live(tally);
    if (engine == nullptr) {
        return;
    }
    const bc_file_system_fault fault =
        fail_step(bc_file_system_operation_write_file, bc_file_system_step_write, 1, EIO);
    uint64_t count = 0;
    bc_file_system_error error;
    expect(tally,
           bc_file_system_observed_fault_add(engine, &fault, &error) == bc_file_system_error_kind_invalid_argument,
           "a live engine refuses fault_add", "");
    expect(tally, bc_file_system_observed_faults_clear(engine, &error) == bc_file_system_error_kind_invalid_argument,
           "a live engine refuses faults_clear", "");
    expect(tally,
           bc_file_system_observed_step_count(engine, bc_file_system_operation_write_file, bc_file_system_step_write,
                                              &count, &error) == bc_file_system_error_kind_invalid_argument,
           "a live engine refuses step_count", "");
    expect(tally, count == 0, "and leaves the out-parameter untouched", "");
    bc_file_system_engine_release(engine);
}

static void test_a_live_engine_neither_counts_nor_fires(report *tally) {
    bc_file_system_engine *engine = create_live(tally);
    if (engine == nullptr) {
        return;
    }
    const int32_t write_file = bc_file_system_operation_write_file;
    bc_file_system_engine_before_operation(engine, write_file);
    /* Every verdict is `proceed`: a live engine has no slot to search and, by the header's thread contract, no
     * counter to write; the refusal of step_count above is the only observable fact, and it is this one. */
    expect(tally, is_proceed(step(engine, write_file, bc_file_system_step_openat)), "a live step proceeds", "");
    expect(tally, is_proceed(step(engine, write_file, bc_file_system_step_write)), "a live step proceeds", "");
    bc_file_system_engine_release(engine);
}

/* ---- Entry point -------------------------------------------------------------------------------------------- */

int main(void) {
    report tally = {.checked = 0, .failed = 0};
    test_live_engine_is_created_released_and_probed(&tally);
    test_observed_engine_probes_the_same_way(&tally);
    test_platform_operations(&tally);
    test_an_observed_engine_counts_every_step(&tally);
    test_a_fault_fires_at_its_occurrence_and_not_before(&tally);
    test_a_fault_names_one_operation_and_one_step(&tally);
    test_a_stop_is_a_verdict(&tally);
    test_two_faults_fire_independently(&tally);
    test_a_fired_fault_leaves_the_slot(&tally);
    test_clearing_the_faults_keeps_the_counts(&tally);
    test_a_fault_with_occurrence_zero_is_refused(&tally);
    test_a_live_engine_refuses_the_observer_calls(&tally);
    test_a_live_engine_neither_counts_nor_fires(&tally);
    printf("bc_file_system_engine_test: %d checks, %d failed\n", tally.checked, tally.failed);
    return tally.failed == 0 ? 0 : 1;
}
