/* engine.h — the engine's private face: the probed fast paths, the observer's two hooks, and the vocabulary of a
 * gated system call.
 *
 * Every operation in `bc_file_system.c` and the resolution layer in `resolve.c` are written against these: an
 * operation announces itself with `before_operation`, and gates every system call with `before_step`, whose
 * verdict says whether the call runs, fails as if the system had refused it, or is the point the operation
 * returns from without cleanup, as if the process had been killed there. What one gated call then comes to is
 * a `bc_file_system_call_outcome`, read from the verdict with `observer_decided` and turned into the error with
 * `report_outcome`, so every system call in the core is made and reported in one shape. The hook functions have
 * external linkage, under the library's prefix, so the C-only test in `core/tests` can drive the mechanics
 * without an operation in between; they are declared only here and are no part of the public header. Private
 * to `core/src`: C23, never a common subset.
 *
 * THREADS. The public header promises that a live engine is immutable after creation and may be used from any
 * number of threads at once, and that an engine created for tests is used from one thread at a time. The hooks
 * keep that promise by construction: on a live engine neither hook writes anything, not even a counter, so the
 * only cost a release build pays is one read of `observed` per step; on an observed engine both hooks write the
 * counters and the fault slot, which the single-thread contract makes safe without a lock. The one field a
 * live engine writes after creation is the staged-write token counter in `bc_file_system.c`, which is atomic
 * so two threads staging at once draw different tokens.
 */
#pragma once

#include <assert.h>
#include <stdint.h>

#include <bc_file_system/bc_file_system.h>

#include "platform.h"

/* What `before_step` decided. `proceed` is 0 so a zeroed verdict reads as "run the call"; the other two carry the
 * values of the fault actions they come from. */
typedef enum bc_file_system_step_action : int32_t {
    bc_file_system_step_action_proceed = 0,
    /* The step fails with `system_code`, which the operation reports exactly as a real refusal. */
    bc_file_system_step_action_fail = bc_file_system_fault_action_fail,
    /* The operation returns from this step leaving the disk as a kill would: a staged file stays where it is,
     * nothing is unlinked, renamed back or given its mode. The descriptors the operation holds do close, as a
     * process's death closes them, because the cleanup idiom runs on every return. */
    bc_file_system_step_action_stop = bc_file_system_fault_action_stop
} bc_file_system_step_action;

typedef struct bc_file_system_step_verdict {
    int32_t action;      /* a bc_file_system_step_action */
    int32_t system_code; /* the errno to fail with under `fail`; 0 otherwise */
} bc_file_system_step_verdict;

/* The fast paths `engine` was created with: what the platform's probe found, or none at all for the
 * portable-only engine. Borrowed; lives as long as the engine. */
const bc_file_system_platform_fast_paths *bc_file_system_engine_fast_paths(const bc_file_system_engine *engine);

/* The operation has validated its arguments and is about to make its first system call. On a live engine this
 * does nothing. On an observed engine it records that `operation` began, which `before_step` asserts, so a step
 * counted under an operation that never announced itself is caught in a debug build. `operation` is a valid
 * bc_file_system_operation. */
void bc_file_system_engine_before_operation(bc_file_system_engine *engine, int32_t operation);

/* The operation is about to make the system call `step`. On a live engine the verdict is always `proceed` and
 * nothing is written. On an observed engine the step is counted first, whatever the verdict, so the count records
 * what the operation reached rather than what the system did; then the fault slot is searched for a fault whose
 * operation and step match and whose occurrence is this very count, and that fault fires once, leaving the slot,
 * and becomes the verdict. `operation` and `step` are valid enumerators; `verdict` must not be NULL. */
void bc_file_system_engine_before_step(bc_file_system_engine *engine, int32_t operation, int32_t step,
                                       bc_file_system_step_verdict *verdict);

/* ---- What a gated system call comes to --------------------------------------------------------------------- */

/* The outcome of one gated system call: the call was made and succeeded; or it failed with `code`, real or
 * scripted; or the observer stopped the operation there. Every loop that makes a call writes the whole struct
 * where the call ends, so `succeeded` is true only where the call itself answered 0, and an out-parameter the
 * call fills is read only on that path. */
typedef struct bc_file_system_call_outcome {
    bool succeeded;
    int code;
    bool stopped;
} bc_file_system_call_outcome;

/* Asks the observer before one attempt of `step`. The three functions below are `static inline` and unprefixed
 * because none has external linkage: each unit that includes this header gets its own copy. */
static inline bc_file_system_step_verdict ask_observer(bc_file_system_engine *engine, int32_t operation, int32_t step) {
    bc_file_system_step_verdict verdict;
    bc_file_system_engine_before_step(engine, operation, step, &verdict);
    return verdict;
}

/* Every gated call reads the verdict the same way, spelled out in each loop rather than through a helper (the
 * analyzer loses a postcondition set six calls deep): a scripted `fail` is the call's errno without the call,
 * reported exactly as a real one and asked again when it is EINTR like a real one; a `stop` ends the attempts.
 * A step that makes no call (the identity check) reads it here. */
static inline bool observer_decided(const bc_file_system_step_verdict *verdict, bc_file_system_call_outcome *outcome) {
    if (verdict->action == bc_file_system_step_action_stop) {
        *outcome = (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        return true;
    }
    if (verdict->action == bc_file_system_step_action_fail) {
        *outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict->system_code, .stopped = false};
        return true;
    }
    return false;
}

/* Fills `error` for a step that failed and answers the status every failing path returns. The name stays
 * empty: what the step acted on is the location itself, whose text the face holds; an operation whose step
 * acted on an entry inside the location fills the name itself. A stop is answered as `io_failed` with no system
 * code at the stopped step, for every operation: a read-only one holds nothing a killed process would not lose
 * as well, so returning is the kill; one that holds a staged file on disk marks it kept before coming here
 * (`report_staged_outcome` in `bc_file_system.c`), so its cleanup leaves what a kill would leave. */
static inline int32_t report_outcome(bc_file_system_error *error, int32_t operation, int32_t step,
                                     const bc_file_system_call_outcome *outcome, int32_t kind) {
    /* Every kind the mapping answers is a failure, never 0: a caller that reads an out-parameter only when the
     * answer is 0 relies on it, and the analyzer reads this line to see that. */
    assert(kind != 0);
    if (outcome->stopped) {
        *error =
            (bc_file_system_error){.kind = bc_file_system_error_kind_io_failed, .operation = operation, .step = step};
        return bc_file_system_error_kind_io_failed;
    }
    *error = (bc_file_system_error){.kind = kind, .operation = operation, .step = step, .system_code = outcome->code};
    return kind;
}
