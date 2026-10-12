/* engine.h — the engine's private face: the probed fast paths and the observer's two hooks.
 *
 * Every operation in `bc_file_system.c` is written against these three functions: it announces itself with
 * `before_operation`, and gates every system call with `before_step`, whose verdict says whether the call runs,
 * fails as if the system had refused it, or is the point the operation returns from without cleanup, as if the
 * process had been killed there. The functions have external linkage, under the library's prefix, so the C-only
 * test in `core/tests` can drive the mechanics without an operation in between; they are declared only here and
 * are no part of the public header. Private to `core/src`: C23, never a common subset.
 *
 * THREADS. The public header promises that a live engine is immutable after creation and may be used from any
 * number of threads at once, and that an engine created for tests is used from one thread at a time. The hooks
 * keep that promise by construction: on a live engine neither hook writes anything, not even a counter, so the
 * only cost a release build pays is one read of `observed` per step; on an observed engine both hooks write the
 * counters and the fault slot, which the single-thread contract makes safe without a lock.
 */
#pragma once

#include <stdint.h>

#include <bc_file_system/bc_file_system.h>

#include "platform.h"

/* What `before_step` decided. `proceed` is 0 so a zeroed verdict reads as "run the call"; the other two carry the
 * values of the fault actions they come from. */
typedef enum bc_file_system_step_action : int32_t {
    bc_file_system_step_action_proceed = 0,
    /* The step fails with `system_code`, which the operation reports exactly as a real refusal. */
    bc_file_system_step_action_fail = bc_file_system_fault_action_fail,
    /* The operation returns from this step with no cleanup: no close, no unlink, no rename back. */
    bc_file_system_step_action_stop = bc_file_system_fault_action_stop
} bc_file_system_step_action;

typedef struct bc_file_system_step_verdict {
    int32_t action;      /* a bc_file_system_step_action */
    int32_t system_code; /* the errno to fail with under `fail`; 0 otherwise */
} bc_file_system_step_verdict;

/* The fast paths the factory probed when it created `engine`. Borrowed; lives as long as the engine. */
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
