/* bc_file_system.c — the engine behind the public header.
 *
 * This translation unit is the hosted tier of the core: it owns the engine, the directory and lock handles,
 * the test observer's slot and every operation. It is C23 in the GNU dialect; the public header it implements
 * is the common subset, and the platform differences are behind platform.h.
 *
 * Where the library stands: the handles, the platform probes, the observer and the resolution layer are real,
 * and so are the operations that read (`bc_file_system_directory_open`, `bc_file_system_status_of`,
 * `bc_file_system_read_file`), the ones that write (`bc_file_system_write_file`, staged and renamed into
 * place; `bc_file_system_append`; `bc_file_system_sync`) and the ones on directories (`create_directory`,
 * `create_directories`, `list_directory`, `remove_file`, `remove_directory`, `remove_tree` on held descriptors
 * with a bounded window, `create_temporary_directory`, `set_permissions`); every other operation is an honest
 * stub. Each one validates its arguments exactly as the header promises (a contract violation is
 * `invalid_argument` and touches nothing) and announces itself to the observer; a stub then answers
 * `unsupported` with the operation named and the step zero, so every contract case is red for a stated reason
 * until the stage that builds it. A real operation resolves its location once through resolve.h to (parent
 * descriptor, leaf name) and makes only `*at` calls on that pair from then on, each through calls.h (behind
 * the observer's `before_step`, asked again on EINTR, the errno copied on the line after the call); every
 * descriptor it holds is an owned_descriptor under the cleanup idiom of owned.h, a directory it reads is an
 * owned_directory_stream, and a file or directory it created and still owns is a staged_file or a
 * created_directory under the same idiom, unlinked on a failing path and kept on a stop.
 */
#include <bc_file_system/bc_file_system.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "calls.h"
#include "engine.h"
#include "owned.h"
#include "path.h"
#include "platform.h"
#include "resolve.h"

/* ---- Layout pins -------------------------------------------------------------------------------------------- */

/* Every struct that crosses the ABI is pinned by its size and the offset of its last field, as literals, so a
 * face (C++ static_assert, a Swift MemoryLayout test, a Rust const assertion) repeats the same numbers and a
 * field added here fails every face's build. A location holds two pointers, so its numbers depend on the
 * pointer width: both widths the library ships for are pinned. */
static_assert(sizeof(bc_file_system_error) == 280, "bc_file_system_error layout changed; bump the faces");
static_assert(offsetof(bc_file_system_error, name) == 21, "bc_file_system_error layout changed; bump the faces");
static_assert(sizeof(bc_file_system_location) == (sizeof(void *) == 8 ? 32 : 16),
              "bc_file_system_location layout changed; bump the faces");
static_assert(offsetof(bc_file_system_location, length) == (sizeof(void *) == 8 ? 24 : 12),
              "bc_file_system_location layout changed; bump the faces");
static_assert(sizeof(bc_file_system_read_limit) == 16, "bc_file_system_read_limit layout changed; bump the faces");
static_assert(offsetof(bc_file_system_read_limit, at_most) == 8,
              "bc_file_system_read_limit layout changed; bump the faces");
static_assert(sizeof(bc_file_system_permissions) == 8, "bc_file_system_permissions layout changed; bump the faces");
static_assert(offsetof(bc_file_system_permissions, mode) == 4,
              "bc_file_system_permissions layout changed; bump the faces");
static_assert(sizeof(bc_file_system_write_options) == 20,
              "bc_file_system_write_options layout changed; bump the faces");
static_assert(offsetof(bc_file_system_write_options, final_symlink) == 16,
              "bc_file_system_write_options layout changed; bump the faces");
static_assert(sizeof(bc_file_system_read_options) == 24, "bc_file_system_read_options layout changed; bump the faces");
static_assert(offsetof(bc_file_system_read_options, final_symlink) == 16,
              "bc_file_system_read_options layout changed; bump the faces");
static_assert(sizeof(bc_file_system_status) == 48, "bc_file_system_status layout changed; bump the faces");
static_assert(offsetof(bc_file_system_status, link_count) == 40,
              "bc_file_system_status layout changed; bump the faces");
static_assert(sizeof(bc_file_system_entry) == 264, "bc_file_system_entry layout changed; bump the faces");
static_assert(offsetof(bc_file_system_entry, name) == 8, "bc_file_system_entry layout changed; bump the faces");
static_assert(sizeof(bc_file_system_fault) == 20, "bc_file_system_fault layout changed; bump the faces");
static_assert(offsetof(bc_file_system_fault, system_code) == 16, "bc_file_system_fault layout changed; bump the faces");

/* The path module names no system header, so its component limit is a number of its own; the hosted engine is
 * where that number meets the platform's. */
static_assert(bc_file_system_path_component_capacity == NAME_MAX, "the path module's component limit is not NAME_MAX");

/* ---- Constants and handles ---------------------------------------------------------------------------------- */

enum : uint32_t {
    /* How many faults one engine can hold scripted at once: bounded scratch, so a 65th is resource_exhausted. */
    fault_capacity = 64,
    /* The bits chmod sets; a mode with any other bit is a contract violation. */
    permission_bits = 07777,
    /* The most one pread or write asks for: a count past SSIZE_MAX is implementation-defined, and macOS caps a
     * single transfer at INT_MAX, so a large file is read or written in bounded pieces. */
    largest_single_transfer = UINT32_C(1) << 30,
    /* How many names a staged write tries before giving up: a name is taken only by a leftover of a process
     * that died with the same id, so the next token almost always frees it, and the bound keeps a directory
     * full of leftovers from becoming an endless loop. */
    staging_attempts = 16,
    /* How many symlinks a write told to follow walks through before calling them a loop (the kernels allow 32
     * on Apple platforms and 40 on Linux). */
    most_symlinks_followed = 32,
    /* The mode a staged file is created with: nobody else may read it before it is given its mode and renamed
     * into place. */
    staged_file_mode = 0600,
    /* The mode a directory is created with before it is given its own: nobody else may enter it in between,
     * and widening to the stated mode is the one direction a mode is ever changed in on creation. */
    directory_create_mode = 0700
};

/* The infix of a staged file's name (`.<leaf>.bc-tmp-<process id>-<token>`); a later stage decides whether the
 * condemned sweep clears leftovers that carry it. */
static const char staging_infix[] = ".bc-tmp-";

enum : size_t {
    /* The most digits a 64-bit number spells in decimal. */
    decimal_digit_capacity = 20,
    /* What a staged file's name holds beside the leaf: the leading dot, the infix, the process id, the dash and
     * the token. */
    staging_name_overhead = 1 + (sizeof staging_infix - 1) + decimal_digit_capacity + 1 + decimal_digit_capacity,
    /* How much of the leaf the staged name keeps, so that the name fits NAME_MAX whatever the leaf's length;
     * the token, not the leaf, is what makes it unique. */
    staging_leaf_capacity = NAME_MAX - staging_name_overhead
};

enum : int32_t {
    /* How a regular file is opened to be read: never for writing, never inherited by a child process, and never
     * blocking on the open itself, which a FIFO with no writer would do forever; a regular file reads the same
     * with the flag, and anything that is not one is refused right after the open. */
    file_read_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK,
    /* How a staged file is created: new, or the name is taken (O_EXCL); never through a symlink. */
    staged_create_flags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
    /* How an append creates a file, and how it opens one that exists: at the end, never through a symlink (a
     * link answers ELOOP), and never blocking on the open, which a FIFO with no reader would do forever. */
    append_create_flags = O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
    append_open_flags = O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
    /* How a file or directory is opened to be synced: a directory opens read-only, a symlink is refused. */
    sync_open_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
    /* How a directory the walk holds for search is opened again to be synced: read-only, as `.` beneath it,
     * because a search-only descriptor (O_PATH on Linux) cannot be synced. */
    directory_read_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC
};

/* The last enumerator of each closed enum, so a range check reads `is_in_enum(value, operation_last)`. Every
 * enum in the header runs from 1 to its last value without a gap. */
enum : int32_t {
    operation_last = bc_file_system_operation_path_library_name,
    step_last = bc_file_system_step_fchmodat,
    final_symlink_last = bc_file_system_final_symlink_follow,
    existing_destination_last = bc_file_system_existing_destination_refuse,
    read_limit_kind_last = bc_file_system_read_limit_kind_at_most,
    durability_last = bc_file_system_durability_durable,
    permissions_policy_last = bc_file_system_permissions_policy_preserve_or,
    fault_action_last = bc_file_system_fault_action_stop
};

struct bc_file_system_engine {
    /* Created by the observed factory: the fault slot and the step counters are live. A live engine carries the
     * same fields and never touches them, which is the one decision per step a release build pays. */
    bool observed;
    /* The platform's fast paths, probed once by whichever factory created the engine and read by the operations
     * that have one (the resolution layer, writes, moves and copies). */
    bc_file_system_platform_fast_paths fast_paths;
    /* What makes a staged file's name unique: the id of the process the engine was created in, read once by the
     * factory, and a token drawn from this counter for every staged file (never the clock, never a random
     * source). The counter is the one field a live engine writes after creation; it is atomic so two threads
     * staging at once draw different tokens, and nothing orders on it, so the relaxed order is enough. */
    uint64_t process_id;
    _Atomic uint64_t staging_counter;
    uint32_t fault_count;
    bc_file_system_fault faults[fault_capacity];
    /* How many times each operation has reached each step. Row 0 is unused because 0 is no operation; column 0
     * of each row holds how many times that operation began (`before_operation`), which the public step count
     * never reads because 0 is no step. */
    uint64_t step_counts[operation_last + 1][step_last + 1];
};

/* `struct bc_file_system_directory` is resolve.h's: the walk beneath a directory starts from its descriptor. */

struct bc_file_system_lock {
    int descriptor; /* the locked file; closing it releases the advisory lock */
};

/* ---- The return convention ---------------------------------------------------------------------------------- */

/* Fills `error` for a failure decided before any system call and answers the status every failing path
 * returns, so a call reads `return refuse(error, kind, operation)`. */
static int32_t refuse(bc_file_system_error *error, int32_t kind, int32_t operation) {
    *error = (bc_file_system_error){.kind = kind, .operation = operation};
    return kind;
}

static int32_t succeed(bc_file_system_error *error) {
    *error = (bc_file_system_error){};
    return 0;
}

/* ---- Argument validation ------------------------------------------------------------------------------------ */

/* Each check below is one comparison per line: in C a comparison is an `int` and a chain of them under `&&` is
 * too, so a bool answer is spelled as early returns rather than converted. */
static bool is_in_enum(int32_t value, int32_t last_enumerator) {
    if (value < 1) {
        return false;
    }
    return value <= last_enumerator;
}

/* A NULL pointer offers no bytes only when its length is 0. */
static bool is_byte_span(const uint8_t *bytes, size_t length) {
    if (bytes != nullptr) {
        return true;
    }
    return length == 0;
}

/* The caller's buffer under the buffer protocol. */
static bool is_answer_buffer(const uint8_t *buffer, size_t capacity, const size_t *length) {
    if (length == nullptr) {
        return false;
    }
    if (buffer != nullptr) {
        return true;
    }
    return capacity == 0;
}

/* The path's text is the path module's to judge (a NUL, an empty or `.` component, a trailing slash, relative
 * text under `at`, which would resolve against the working directory); the engine checks only what belongs to
 * the location: its kind and whether it holds the directory that kind needs. */
static bool is_valid_location(const bc_file_system_location *location) {
    if (location == nullptr) {
        return false;
    }
    if (location->kind == bc_file_system_location_kind_at) {
        if (location->directory != nullptr) {
            return false;
        }
        return bc_file_system_path_text_is_valid_absolute(location->path, location->length);
    }
    if (location->kind == bc_file_system_location_kind_beneath) {
        if (location->directory == nullptr) {
            return false;
        }
        return bc_file_system_path_text_is_valid_relative(location->path, location->length);
    }
    return false;
}

static bool is_valid_permissions(const bc_file_system_permissions *permissions) {
    if (permissions == nullptr || !is_in_enum(permissions->policy, permissions_policy_last)) {
        return false;
    }
    return permissions->mode <= permission_bits;
}

static bool is_valid_write_options(const bc_file_system_write_options *options) {
    if (options == nullptr || !is_in_enum(options->durability, durability_last) ||
        !is_valid_permissions(&options->permissions) ||
        !is_in_enum(options->existing_destination, existing_destination_last)) {
        return false;
    }
    return is_in_enum(options->final_symlink, final_symlink_last);
}

static bool is_valid_read_options(const bc_file_system_read_options *options) {
    if (options == nullptr || !is_in_enum(options->limit.kind, read_limit_kind_last)) {
        return false;
    }
    return is_in_enum(options->final_symlink, final_symlink_last);
}

static bool is_valid_fault(const bc_file_system_fault *fault) {
    if (fault == nullptr || !is_in_enum(fault->operation, operation_last) || !is_in_enum(fault->step, step_last) ||
        fault->occurrence == 0 || !is_in_enum(fault->action, fault_action_last)) {
        return false;
    }
    /* A failing step needs an errno to fail with; a stop answers nothing, so a code there is a mistake. */
    if (fault->action == bc_file_system_fault_action_fail) {
        return fault->system_code >= 1;
    }
    return fault->system_code == 0;
}

/* ---- The engine ---------------------------------------------------------------------------------------------- */

static int32_t create_engine(bool observed, int32_t operation, bc_file_system_engine **engine,
                             bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* calloc zeroes the fault slot and every counter in one step. */
    bc_file_system_engine *created = calloc(1, sizeof *created);
    if (created == nullptr) {
        return refuse(error, bc_file_system_error_kind_resource_exhausted, operation);
    }
    created->observed = observed;
    /* Probed once, here, so no operation asks the kernel twice; the observed engine probes the same way, so the
     * suite runs over the same fast paths a composition root gets. */
    created->fast_paths = bc_file_system_platform_probe_fast_paths();
    /* The process id is a fact of the process, read here, in the factory, and carried by the engine: no
     * operation asks for it. A pid is never negative, so the cast only changes the type. */
    created->process_id = (uint64_t)getpid(); /* reach-ok: the factory is the one live site of the engine */
    atomic_init(&created->staging_counter, 0);
    *engine = created;
    return succeed(error);
}

int32_t bc_file_system_engine_create_live(bc_file_system_engine **engine, bc_file_system_error *error) {
    return create_engine(false, bc_file_system_operation_engine_create_live, engine, error);
}

int32_t bc_file_system_engine_create_observed_for_tests(bc_file_system_engine **engine, bc_file_system_error *error) {
    return create_engine(true, bc_file_system_operation_engine_create_observed_for_tests, engine, error);
}

void bc_file_system_engine_release(bc_file_system_engine *engine) {
    free(engine);
}

int32_t bc_file_system_observed_fault_add(bc_file_system_engine *engine, const bc_file_system_fault *fault,
                                          bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_observed_fault_add;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !engine->observed || !is_valid_fault(fault)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    if (engine->fault_count == fault_capacity) {
        return refuse(error, bc_file_system_error_kind_resource_exhausted, operation);
    }
    engine->faults[engine->fault_count] = *fault;
    engine->fault_count += 1;
    return succeed(error);
}

int32_t bc_file_system_observed_faults_clear(bc_file_system_engine *engine, bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !engine->observed) {
        return refuse(error, bc_file_system_error_kind_invalid_argument,
                      bc_file_system_operation_observed_faults_clear);
    }
    engine->fault_count = 0;
    return succeed(error);
}

int32_t bc_file_system_observed_step_count(const bc_file_system_engine *engine, int32_t operation, int32_t step,
                                           uint64_t *count, bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !engine->observed || !is_in_enum(operation, operation_last) ||
        !is_in_enum(step, step_last) || count == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, bc_file_system_operation_observed_step_count);
    }
    /* Both indices were just proved to lie in 1..last, so the casts only change the type, never the value. */
    *count = engine->step_counts[(size_t)operation][(size_t)step];
    return succeed(error);
}

/* ---- The observer's hooks (engine.h) --------------------------------------------------------------------- */

const bc_file_system_platform_fast_paths *bc_file_system_engine_fast_paths(const bc_file_system_engine *engine) {
    return &engine->fast_paths;
}

void bc_file_system_engine_before_operation(bc_file_system_engine *engine, int32_t operation) {
    assert(is_in_enum(operation, operation_last));
    if (!engine->observed) {
        return;
    }
    engine->step_counts[(size_t)operation][0] += 1;
}

/* The one fault that fires now, if any: the first in the slot whose operation and step match and whose
 * occurrence is this count. It leaves the slot by being overwritten with the last entry, which keeps the slot
 * dense; the order of the faults carries no meaning, since a fault is identified by its three numbers. */
static bool take_firing_fault(bc_file_system_engine *engine, int32_t operation, int32_t step, uint64_t count,
                              bc_file_system_fault *fired) {
    for (uint32_t index = 0; index < engine->fault_count; ++index) {
        const bc_file_system_fault *candidate = &engine->faults[index];
        if (candidate->operation != operation || candidate->step != step || candidate->occurrence != count) {
            continue;
        }
        *fired = *candidate;
        engine->fault_count -= 1;
        engine->faults[index] = engine->faults[engine->fault_count];
        return true;
    }
    return false;
}

void bc_file_system_engine_before_step(bc_file_system_engine *engine, int32_t operation, int32_t step,
                                       bc_file_system_step_verdict *verdict) {
    assert(is_in_enum(operation, operation_last));
    assert(is_in_enum(step, step_last));
    *verdict = (bc_file_system_step_verdict){.action = bc_file_system_step_action_proceed, .system_code = 0};
    if (!engine->observed) {
        return;
    }
    /* A step belongs to an operation that announced itself; a helper that counts under a name its caller never
     * began is the bug this catches. */
    assert(engine->step_counts[(size_t)operation][0] > 0);
    engine->step_counts[(size_t)operation][(size_t)step] += 1;
    const uint64_t count = engine->step_counts[(size_t)operation][(size_t)step];
    bc_file_system_fault fired;
    if (!take_firing_fault(engine, operation, step, count, &fired)) {
        return;
    }
    /* The fault's action values are the verdict's, so the verdict is the fault read as one. */
    verdict->action = fired.action;
    verdict->system_code = fired.system_code;
}

/* ---- Directories and locks ----------------------------------------------------------------------------------- */

/* The text a directory remembers: for `at` the path's bytes, for `beneath` the parent's text, a slash and the
 * relative bytes. The handle and its text are one allocation of bounded scratch, sized here with checked
 * arithmetic; nothing bounds the text but the caller's own bytes. Answers the handle, or NULL when the size
 * overflows or the allocation fails, which the caller reports as `resource_exhausted`. */
static bc_file_system_directory *allocate_directory(const bc_file_system_location *location) {
    size_t text_length = location->length;
    if (location->kind == bc_file_system_location_kind_beneath) {
        if (ckd_add(&text_length, text_length, location->directory->opened_as_length) ||
            ckd_add(&text_length, text_length, (size_t)1)) {
            return nullptr;
        }
    }
    size_t allocation = 0;
    if (ckd_add(&allocation, sizeof(bc_file_system_directory), text_length)) {
        return nullptr;
    }
    bc_file_system_directory *created = malloc(allocation);
    if (created == nullptr) {
        return nullptr;
    }
    created->descriptor = -1;
    created->opened_as_length = text_length;
    size_t position = 0;
    if (location->kind == bc_file_system_location_kind_beneath) {
        memcpy(created->opened_as, location->directory->opened_as, location->directory->opened_as_length);
        position = location->directory->opened_as_length;
        created->opened_as[position] = '/';
        position += 1;
    }
    memcpy(created->opened_as + position, location->path, location->length);
    return created;
}

int32_t bc_file_system_directory_open(bc_file_system_engine *engine, const bc_file_system_location *location,
                                      int32_t final_symlink, bc_file_system_directory **directory,
                                      bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_open_directory;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_in_enum(final_symlink, final_symlink_last) ||
        directory == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The leaf is opened as a directory with the caller's choice about a final symlink, bracketed by the
     * identity check like every component before it. The root has no leaf: the parent the walk opened is the
     * directory itself, and it is taken over rather than opened again. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    if (resolved.leaf_length == 0) {
        opened.value = release_descriptor(&resolved.opened);
    } else {
        bc_file_system_expected_entry expected;
        kind = bc_file_system_resolve_expect_entry(engine, operation, resolved.parent, (const char *)resolved.leaf,
                                                   final_symlink, &expected, error);
        if (kind != 0) {
            return kind;
        }
        kind = bc_file_system_resolve_open_expected_directory(
            engine, operation, resolved.parent, (const char *)resolved.leaf, final_symlink,
            bc_file_system_directory_access_search, &expected, &opened, error);
        if (kind != 0) {
            return kind;
        }
    }
    bc_file_system_directory *created = allocate_directory(location);
    if (created == nullptr) {
        return refuse(error, bc_file_system_error_kind_resource_exhausted, operation);
    }
    created->descriptor = release_descriptor(&opened);
    *directory = created;
    return succeed(error);
}

int32_t bc_file_system_directory_opened_as(const bc_file_system_directory *directory, uint8_t *buffer, size_t capacity,
                                           size_t *length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_directory_opened_as;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (directory == nullptr || !is_answer_buffer(buffer, capacity, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    if (directory->opened_as_length > capacity) {
        *length = directory->opened_as_length;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    if (directory->opened_as_length > 0) {
        memcpy(buffer, directory->opened_as, directory->opened_as_length);
    }
    *length = directory->opened_as_length;
    return succeed(error);
}

void bc_file_system_directory_release(bc_file_system_directory *directory) {
    if (directory == nullptr) {
        return;
    }
    if (directory->descriptor >= 0) {
        /* discard-ok: a read-only descriptor; nothing its close could report changes a result already given */
        (void)close(directory->descriptor);
    }
    free(directory);
}

void bc_file_system_lock_release(bc_file_system_lock *lock) {
    if (lock == nullptr) {
        return;
    }
    if (lock->descriptor >= 0) {
        /* discard-ok: closing is how the advisory lock is let go, and no bytes were written through it */
        (void)close(lock->descriptor);
    }
    free(lock);
}

/* ---- Status and reads --------------------------------------------------------------------------------------- */

static int32_t entry_type_of(mode_t mode) {
    if (S_ISREG(mode)) {
        return bc_file_system_entry_type_regular_file;
    }
    if (S_ISDIR(mode)) {
        return bc_file_system_entry_type_directory;
    }
    if (S_ISLNK(mode)) {
        return bc_file_system_entry_type_symlink;
    }
    return bc_file_system_entry_type_other;
}

/* What the header promises a status holds, from what the system recorded: the size is a regular file's only,
 * the mode is the permission bits without the type bits, the identity fields are copied as bit patterns. */
static bc_file_system_status status_from_stat(const struct stat *info) {
    int64_t size = 0;
    if (S_ISREG(info->st_mode)) {
        /* off_t is 64 bits wide on every target (platform.c asserts it), so the conversion only changes the type. */
        size = (int64_t)info->st_size;
    }
    return (bc_file_system_status){.entry_type = entry_type_of(info->st_mode),
                                   .size = size,
                                   .mode = (uint32_t)info->st_mode & permission_bits,
                                   .device = (uint64_t)info->st_dev,
                                   .inode = (uint64_t)info->st_ino,
                                   .link_count = (uint64_t)info->st_nlink};
}

/* Nothing is at a location when a component on the way is missing or is not a directory, or when the leaf
 * itself is missing: the answers lstat would spell ENOENT and ENOTDIR, which the header makes an answer of
 * `status`, not an error. */
static bool means_absence(int32_t kind) {
    if (kind == bc_file_system_error_kind_not_found) {
        return true;
    }
    return kind == bc_file_system_error_kind_not_directory;
}

/* Whether a file's size is more than this target can address as a count of bytes; `*as_size` is the size as
 * one when it is not. ckd_add reports whether the sum fits the result's width, which is the check a 32-bit
 * target (wasm32) needs and a 64-bit one never fails. */
static bool overflows_size_t(uint64_t size, size_t *as_size) {
    return ckd_add(as_size, size, (uint64_t)0);
}

int32_t bc_file_system_status_of(bc_file_system_engine *engine, const bc_file_system_location *location,
                                 int32_t final_symlink, int32_t *present, bc_file_system_status *status,
                                 bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_status;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_in_enum(final_symlink, final_symlink_last) ||
        present == nullptr || status == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    const int32_t resolution = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (resolution != 0) {
        if (means_absence(resolution)) {
            *present = 0;
            return succeed(error);
        }
        return resolution;
    }
    /* The leaf is judged as itself under `refuse` and as what it points at under `follow`. The root has no
     * parent entry to stat: the walk opened the root itself, and that descriptor is what is described. */
    struct stat info;
    int32_t step = bc_file_system_step_fstatat;
    bc_file_system_call_outcome outcome;
    if (resolved.leaf_length == 0) {
        step = bc_file_system_step_fstat;
        outcome = bc_file_system_call_fstat(engine, operation, resolved.parent, &info);
    } else {
        const int flags = final_symlink == bc_file_system_final_symlink_refuse ? AT_SYMLINK_NOFOLLOW : 0;
        outcome =
            bc_file_system_call_fstatat(engine, operation, resolved.parent, (const char *)resolved.leaf, flags, &info);
    }
    if (!outcome.succeeded) {
        const int32_t kind = bc_file_system_call_kind_of_errno(outcome.code);
        if (!outcome.stopped && means_absence(kind)) {
            *present = 0;
            return succeed(error);
        }
        return report_outcome(error, operation, step, &outcome, kind);
    }
    *status = status_from_stat(&info);
    *present = 1;
    return succeed(error);
}

/* Reads the whole of an open regular file of `size` bytes into the caller's buffer, which holds at least that
 * many, and says how many arrived: fewer when the file shrank since it was measured, in which case the bytes
 * read are the answer. */
static int32_t read_whole_file(bc_file_system_engine *engine, int32_t operation, int descriptor, uint8_t *buffer,
                               size_t size, size_t *read_total, bc_file_system_error *error) {
    size_t total = 0;
    while (total < size) {
        const size_t remaining = size - total;
        const size_t count = remaining < largest_single_transfer ? remaining : largest_single_transfer;
        size_t read_count = 0;
        const bc_file_system_call_outcome outcome =
            bc_file_system_call_pread(engine, operation, descriptor, buffer + total, count, total, &read_count);
        if (!outcome.succeeded) {
            return report_outcome(error, operation, bc_file_system_step_pread, &outcome,
                                  bc_file_system_call_kind_of_errno(outcome.code));
        }
        if (read_count == 0) {
            break;
        }
        total += read_count;
    }
    *read_total = total;
    return 0;
}

/* Whether the file holds a byte past `offset`, read into a local so the caller's buffer is never touched past
 * what was promised: how a file that grew between its measurement and its reading is noticed. */
static int32_t probe_for_more(bc_file_system_engine *engine, int32_t operation, int descriptor, uint64_t offset,
                              bool *has_more, bc_file_system_error *error) {
    uint8_t probe = 0;
    size_t probed = 0;
    const bc_file_system_call_outcome outcome =
        bc_file_system_call_pread(engine, operation, descriptor, &probe, sizeof probe, offset, &probed);
    if (!outcome.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_pread, &outcome,
                              bc_file_system_call_kind_of_errno(outcome.code));
    }
    *has_more = probed > 0;
    return 0;
}

/* The size of the open regular file against the read limit and this target's addressable size: `too_large`
 * when either refuses it. An outcome decided from what a call reported, not from a call that failed, carries no
 * step, like a refusal before any call. */
static int32_t size_within_limits(uint64_t size, const bc_file_system_read_limit *limit, int32_t operation,
                                  size_t *as_size, bc_file_system_error *error) {
    if (limit->kind == bc_file_system_read_limit_kind_at_most && size > limit->at_most) {
        return refuse(error, bc_file_system_error_kind_too_large, operation);
    }
    if (overflows_size_t(size, as_size)) {
        return refuse(error, bc_file_system_error_kind_too_large, operation);
    }
    return 0;
}

/* After a file read whole to its measured size: whether a byte lies past it, which means it grew between its
 * measurement and its reading, and if so the size it has now, measured again, so the caller can ask for it
 * again under the buffer protocol rather than answer a prefix. */
static int32_t measure_growth(bc_file_system_engine *engine, int32_t operation, int descriptor,
                              const bc_file_system_read_limit *limit, size_t size, bool *grew, size_t *grown,
                              bc_file_system_error *error) {
    bool has_more = false;
    const int32_t probed = probe_for_more(engine, operation, descriptor, size, &has_more, error);
    if (probed != 0) {
        return probed;
    }
    if (!has_more) {
        *grew = false;
        return 0;
    }
    struct stat info;
    const bc_file_system_call_outcome remeasured = bc_file_system_call_fstat(engine, operation, descriptor, &info);
    if (!remeasured.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstat, &remeasured,
                              bc_file_system_call_kind_of_errno(remeasured.code));
    }
    const int32_t kind = size_within_limits((uint64_t)info.st_size, limit, operation, grown, error);
    if (kind != 0) {
        return kind;
    }
    *grew = true;
    return 0;
}

int32_t bc_file_system_read_file(bc_file_system_engine *engine, const bc_file_system_location *location,
                                 const bc_file_system_read_options *options, uint8_t *buffer, size_t capacity,
                                 size_t *length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_read_file;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_valid_read_options(options) ||
        !is_answer_buffer(buffer, capacity, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The leaf is opened as a file, never followed under `refuse` (a symlink then answers ELOOP, which the
     * table reads as `symlink_refused`), and judged by what the descriptor turns out to be, so nothing is
     * decided from a stat the open could have raced. The root has no leaf: the directory the walk opened is
     * what the location names, and it is taken over to be judged the same way. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor file = {.value = -1};
    if (resolved.leaf_length == 0) {
        adopt_descriptor(&file, &resolved.opened);
    } else {
        const int flags = options->final_symlink == bc_file_system_final_symlink_refuse ? file_read_flags | O_NOFOLLOW
                                                                                        : file_read_flags;
        const bc_file_system_call_outcome opened = bc_file_system_call_openat(
            engine, operation, resolved.parent, (const char *)resolved.leaf, flags, &file.value);
        if (!opened.succeeded) {
            return report_outcome(error, operation, bc_file_system_step_openat, &opened,
                                  bc_file_system_call_kind_of_errno(opened.code));
        }
    }
    struct stat info;
    const bc_file_system_call_outcome measured = bc_file_system_call_fstat(engine, operation, file.value, &info);
    if (!measured.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstat, &measured,
                              bc_file_system_call_kind_of_errno(measured.code));
    }
    if (!S_ISREG(info.st_mode)) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    /* A regular file's size is never negative, so the conversion only changes the type. */
    size_t size = 0;
    kind = size_within_limits((uint64_t)info.st_size, &options->limit, operation, &size, error);
    if (kind != 0) {
        return kind;
    }
    if (size > capacity) {
        *length = size;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    size_t read_total = 0;
    kind = read_whole_file(engine, operation, file.value, buffer, size, &read_total, error);
    if (kind != 0) {
        return kind;
    }
    /* A file that ended early is answered as the bytes it had. One that still has a byte past its measured
     * size grew in between, and a prefix is never an answer: it is asked for again under the buffer protocol,
     * which bounds the retries on the caller's side. */
    if (read_total == size) {
        bool grew = false;
        size_t grown = 0;
        kind = measure_growth(engine, operation, file.value, &options->limit, size, &grew, &grown, error);
        if (kind != 0) {
            return kind;
        }
        if (grew) {
            *length = grown;
            return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
        }
    }
    *length = read_total;
    return succeed(error);
}

/* ---- Writes ------------------------------------------------------------------------------------------------- */

/* A file this operation created in `parent` under `name` and still owns: a staged write's temporary until it
 * is renamed into place, an append's new file until its mode is set. Declared under
 * `[[gnu::cleanup(discard_staged_file)]]`, so a return on any failing path closes it and unlinks it. `kept` is
 * set where the file becomes the caller's (the rename happened; the mode was set) and where the observer
 * stopped the operation, so that the disk is left as a kill would leave it: the file stays, nothing is
 * unlinked. */
typedef struct staged_file {
    int parent; /* borrowed: the directory the file was created in */
    owned_descriptor descriptor;
    uint8_t name[NAME_MAX + 1]; /* NUL-terminated */
    bool created;
    bool kept;
} staged_file;

static staged_file staged_file_in(int parent) {
    return (staged_file){.parent = parent, .descriptor = {.value = -1}, .name = {}, .created = false, .kept = false};
}

/* The cleanup. The close's result is discarded because a file that is unlinked next, or that a kill would have
 * left open, has nothing to report that changes the answer already decided; a close whose result matters (a
 * durable write's) was made as a step before coming here. The unlink is best effort after the real error. */
static void discard_staged_file(staged_file *staged) {
    close_owned_descriptor(&staged->descriptor);
    if (!staged->created || staged->kept) {
        return;
    }
    (void)unlinkat(staged->parent, (const char *)staged->name, 0); /* discard-ok: best effort after the error */
    staged->created = false;
}

/* The one place a staged operation reads a verdict. A stop keeps what is on disk, as a kill would, and is then
 * reported as `report_outcome` reports every stop (io_failed, no system code, the stopped step); a failure is
 * reported with its kind and the staged file is discarded by its cleanup on the way out. */
static int32_t report_staged_outcome(staged_file *staged, bc_file_system_error *error, int32_t operation, int32_t step,
                                     const bc_file_system_call_outcome *outcome, int32_t kind) {
    if (outcome->stopped) {
        staged->kept = true;
    }
    return report_outcome(error, operation, step, outcome, kind);
}

/* Whether a failed sync means the volume offers none: the platform's own ENOTSUP, or the codes a scripted fault
 * may spell for the same fact. Such a sync lowers the answer to `written` instead of failing the operation.
 * An if-chain because ENOTSUP and EOPNOTSUPP are one number on Linux. */
static bool means_no_sync_offered(int code) {
    if (code == ENOTSUP) {
        return true;
    }
    if (code == EOPNOTSUPP) {
        return true;
    }
    if (code == EINVAL) {
        return true;
    }
    return code == ENOTTY;
}

/* Spells `value` in decimal into `buffer`, which holds `decimal_digit_capacity` bytes, and answers how many it
 * wrote. The digits come out lowest first and are copied over reversed. */
static size_t spell_decimal(uint8_t *buffer, uint64_t value) {
    uint8_t reversed[decimal_digit_capacity];
    size_t digits = 0;
    do {
        reversed[digits] = (uint8_t)('0' + (int)(value % 10));
        digits += 1;
        value /= 10;
    } while (value > 0);
    for (size_t index = 0; index < digits; ++index) {
        buffer[index] = reversed[digits - 1 - index];
    }
    return digits;
}

/* The name of a staged file, NUL-terminated into `name` (which holds NAME_MAX + 1 bytes): the leading dot, the
 * leaf cut to what leaves room for the rest, the infix, the process id, a dash and the token. The token is what
 * makes the name unique; the leaf is there so a leftover can be read beside the file it was meant to become. */
static void spell_staged_name(uint8_t *name, const uint8_t *leaf, size_t leaf_length, uint64_t process_id,
                              uint64_t token) {
    const size_t leaf_prefix = leaf_length < staging_leaf_capacity ? leaf_length : staging_leaf_capacity;
    size_t position = 0;
    name[position] = '.';
    position += 1;
    memcpy(name + position, leaf, leaf_prefix);
    position += leaf_prefix;
    memcpy(name + position, staging_infix, sizeof staging_infix - 1);
    position += sizeof staging_infix - 1;
    position += spell_decimal(name + position, process_id);
    name[position] = '-';
    position += 1;
    position += spell_decimal(name + position, token);
    name[position] = 0;
}

/* Creates the staged file beside the leaf, exclusively, under a name no other staged file of this process
 * carries. A name that is taken (EEXIST: a leftover of a process that died with the same id) is tried again
 * with the next token, a bounded number of times. */
static int32_t create_staged_file(bc_file_system_engine *engine, int32_t operation, staged_file *staged,
                                  const uint8_t *leaf, size_t leaf_length, bc_file_system_error *error) {
    for (uint32_t attempt = 0; attempt < staging_attempts; ++attempt) {
        const uint64_t token =
            atomic_fetch_add_explicit(&engine->staging_counter, (uint64_t)1, memory_order_relaxed) + 1;
        spell_staged_name(staged->name, leaf, leaf_length, engine->process_id, token);
        const bc_file_system_call_outcome outcome =
            bc_file_system_call_openat_creating(engine, operation, staged->parent, (const char *)staged->name,
                                                staged_create_flags, staged_file_mode, &staged->descriptor.value);
        if (outcome.succeeded) {
            staged->created = true;
            return 0;
        }
        if (!outcome.stopped && outcome.code == EEXIST) {
            continue;
        }
        return report_staged_outcome(staged, error, operation, bc_file_system_step_openat, &outcome,
                                     bc_file_system_call_kind_of_errno(outcome.code));
    }
    /* Every name tried was taken: the system's refusal, not the destination's existence. */
    const bc_file_system_call_outcome taken = {.succeeded = false, .code = EEXIST, .stopped = false};
    return report_outcome(error, operation, bc_file_system_step_openat, &taken, bc_file_system_error_kind_io_failed);
}

/* Writes every byte to the file, continuing a short write from where it stopped, in pieces of at most
 * `largest_single_transfer`. A write that took nothing is a failure with no code rather than a call to make
 * again forever: the system never answers 0 for a non-empty request. */
static int32_t write_every_byte(bc_file_system_engine *engine, int32_t operation, staged_file *staged,
                                const uint8_t *bytes, size_t byte_count, bc_file_system_error *error) {
    size_t total = 0;
    while (total < byte_count) {
        const size_t remaining = byte_count - total;
        const size_t count = remaining < largest_single_transfer ? remaining : largest_single_transfer;
        size_t written = 0;
        bc_file_system_call_outcome outcome =
            bc_file_system_call_write(engine, operation, staged->descriptor.value, bytes + total, count, &written);
        if (outcome.succeeded && written == 0) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = false};
        }
        if (!outcome.succeeded) {
            return report_staged_outcome(staged, error, operation, bc_file_system_step_write, &outcome,
                                         bc_file_system_call_kind_of_errno(outcome.code));
        }
        total += written;
    }
    return 0;
}

/* A sync after the change is visible (an append's bytes are in the file; a staged file has been renamed into
 * place, and its directory is synced): a failure lowers the answer to `written`, never an error, because the
 * change happened. Only a stop, the kill a test asked for, ends the operation. */
static int32_t sync_after_change(bc_file_system_engine *engine, int32_t operation, int descriptor, int32_t *achieved,
                                 bc_file_system_error *error) {
    const bc_file_system_call_outcome synced = bc_file_system_call_sync(engine, operation, descriptor);
    if (synced.stopped) {
        return report_outcome(error, operation, bc_file_system_call_sync_step(engine), &synced,
                              bc_file_system_error_kind_io_failed);
    }
    if (!synced.succeeded) {
        *achieved = bc_file_system_durability_written; /* discard-ok: lowers the answer, never raises an error */
    }
    return 0;
}

/* The sync of the directory a change is now visible in (a staged file renamed into place; an append's new
 * name). The parent the walk holds is open for search only, which a sync cannot use (fsync of an O_PATH
 * descriptor is EBADF on Linux), so the directory is opened again, read-only, as `.` beneath it; a directory
 * the process may enter but not read cannot be synced by it, and that lowers the answer to `written` like a
 * sync that fails, because the change happened. Only a stop ends the operation. */
static int32_t sync_directory_after_change(bc_file_system_engine *engine, int32_t operation, int parent,
                                           int32_t *achieved, bc_file_system_error *error) {
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor directory = {.value = -1};
    const bc_file_system_call_outcome opened =
        bc_file_system_call_openat(engine, operation, parent, ".", directory_read_flags, &directory.value);
    if (opened.stopped) {
        return report_outcome(error, operation, bc_file_system_step_openat, &opened,
                              bc_file_system_error_kind_io_failed);
    }
    if (!opened.succeeded) {
        *achieved = bc_file_system_durability_written; /* discard-ok: lowers the answer, never raises an error */
        return 0;
    }
    return sync_after_change(engine, operation, directory.value, achieved, error);
}

/* ---- write_file ---------------------------------------------------------------------------------------------- */

/* What the write found at its destination before staging anything: nothing, or a regular file with its mode. */
typedef struct destination_entry {
    bool present;
    uint32_t mode;
} destination_entry;

/* The absolute text of `location`, as the face composes it: the path for `at`; the held directory's remembered
 * text, a slash and the relative path for `beneath`. Answers false when it does not fit `capacity`. */
static bool spell_location(const bc_file_system_location *location, uint8_t *buffer, size_t capacity, size_t *length) {
    size_t position = 0;
    if (location->kind == bc_file_system_location_kind_beneath) {
        const bc_file_system_directory *directory = location->directory;
        if (directory->opened_as_length >= capacity) {
            return false;
        }
        memcpy(buffer, directory->opened_as, directory->opened_as_length);
        position = directory->opened_as_length;
        buffer[position] = '/';
        position += 1;
    }
    if (location->length > capacity - position) {
        return false;
    }
    memcpy(buffer + position, location->path, location->length);
    *length = position + location->length;
    return true;
}

/* The write was told to follow a symlink at its destination. The link's text is read and spelled as an absolute
 * path (an absolute target as it is; a relative one joined to the link's parent, which is where the system
 * resolves it) and normalised lexically, which folds `..` as `bc_file_system_path_resolve_against` would; then
 * it is resolved again as an `at` location, so the write stages beside what the link points at and the link
 * itself is never replaced. `current` holds the absolute text of the entry `resolved` names and is replaced by
 * the target's; it is empty until the first link is followed, when it is composed from the location. This
 * frame and its caller's hold one PATH_MAX buffer each, the one place in the core where two are live at once:
 * the system bounds a link's text and any path it opens by PATH_MAX, so a text that does not fit is refused as
 * `invalid_argument`, the kind of the ENAMETOOLONG the system would answer. */
static int32_t follow_destination_symlink(bc_file_system_engine *engine, int32_t operation,
                                          const bc_file_system_location *location, uint8_t *current,
                                          size_t *current_length, bc_file_system_resolved *resolved,
                                          bc_file_system_error *error) {
    if (*current_length == 0 && !spell_location(location, current, PATH_MAX, current_length)) {
        /* A location spelled longer than any path the system opens cannot be followed by its text. */
        return refuse(error, bc_file_system_error_kind_unsupported, operation);
    }
    uint8_t target[PATH_MAX];
    size_t target_length = 0;
    const bc_file_system_call_outcome read = bc_file_system_call_readlinkat(
        engine, operation, resolved->parent, (const char *)resolved->leaf, target, sizeof target, &target_length);
    if (!read.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_readlinkat, &read,
                              bc_file_system_call_kind_of_errno(read.code));
    }
    /* A text that filled the buffer may have been cut short; an empty one names nothing. */
    if (target_length == 0 || target_length == sizeof target) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t loose_length = 0;
    if (target[0] == '/') {
        memcpy(current, target, target_length);
        loose_length = target_length;
    } else {
        /* The link's parent is a prefix of its own text: everything before its leaf, where the root keeps its
         * slash (`/` is the parent of `/a`) and any other parent has none to add. */
        int32_t parent_present = 0;
        size_t parent_length = 0;
        bc_file_system_error own_error;
        if (bc_file_system_path_parent(current, *current_length, &parent_present, &parent_length, &own_error) != 0 ||
            parent_present == 0) {
            return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
        }
        size_t position = parent_length;
        if (parent_length > 1) {
            current[position] = '/';
            position += 1;
        }
        if (target_length > PATH_MAX - position) {
            return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
        }
        memcpy(current + position, target, target_length);
        loose_length = position + target_length;
    }
    /* One spelling of the target: `.` and `..` components folded, slashes collapsed, into the target buffer and
     * back, since the path module writes nowhere it reads. */
    size_t normalized_length = 0;
    bc_file_system_error own_error;
    if (bc_file_system_path_lexically_normalize(current, loose_length, target, sizeof target, &normalized_length,
                                                &own_error) != 0) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    memcpy(current, target, normalized_length);
    *current_length = normalized_length;
    /* What the walk opened for the link's parent is closed before the target's walk opens its own. */
    bc_file_system_resolved_close(resolved);
    const bc_file_system_location target_location = bc_file_system_location_at(current, *current_length);
    return bc_file_system_resolve(engine, operation, &target_location, resolved, error);
}

/* What is at the destination now decides the symlink rule and, under `preserve_or`, the mode, before anything
 * is staged: nothing there means a new file; a regular file is replaced (or refused, at the rename); a symlink
 * is refused or followed per `final_symlink`, through at most `most_symlinks_followed` links before the chain
 * is called a loop; anything else (a directory, a FIFO, a device) is `not_regular_file`, and so is the root,
 * which has no leaf. An outcome decided from what the stat reported carries no step, like a refusal. */
static int32_t judge_destination(bc_file_system_engine *engine, int32_t operation,
                                 const bc_file_system_location *location, int32_t final_symlink,
                                 bc_file_system_resolved *resolved, destination_entry *existing,
                                 bc_file_system_error *error) {
    uint8_t current[PATH_MAX];
    size_t current_length = 0;
    for (uint32_t followed = 0; followed <= most_symlinks_followed; ++followed) {
        if (resolved->leaf_length == 0) {
            return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
        }
        struct stat info;
        const bc_file_system_call_outcome outcome = bc_file_system_call_fstatat(
            engine, operation, resolved->parent, (const char *)resolved->leaf, AT_SYMLINK_NOFOLLOW, &info);
        if (!outcome.succeeded) {
            if (!outcome.stopped && outcome.code == ENOENT) {
                *existing = (destination_entry){.present = false, .mode = 0};
                return 0;
            }
            return report_outcome(error, operation, bc_file_system_step_fstatat, &outcome,
                                  bc_file_system_call_kind_of_errno(outcome.code));
        }
        if (S_ISREG(info.st_mode)) {
            *existing = (destination_entry){.present = true, .mode = (uint32_t)info.st_mode & permission_bits};
            return 0;
        }
        if (!S_ISLNK(info.st_mode)) {
            return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
        }
        if (final_symlink == bc_file_system_final_symlink_refuse) {
            return refuse(error, bc_file_system_error_kind_symlink_refused, operation);
        }
        const int32_t kind =
            follow_destination_symlink(engine, operation, location, current, &current_length, resolved, error);
        if (kind != 0) {
            return kind;
        }
    }
    /* More links than any kernel would follow: a loop of them. */
    return refuse(error, bc_file_system_error_kind_symlink_refused, operation);
}

/* A durable write pushes the staged file to stable storage and closes it, both before the rename and both
 * reported: nothing has replaced the destination yet, so a failure is the operation's and the staged file is
 * discarded. A volume that offers no sync lowers the answer to `written` and the write proceeds. The close is
 * the last call that can report a write the system deferred, which is why it is a step here. */
static int32_t settle_staged_file(bc_file_system_engine *engine, int32_t operation, staged_file *staged,
                                  int32_t *achieved, bc_file_system_error *error) {
    const bc_file_system_call_outcome synced = bc_file_system_call_sync(engine, operation, staged->descriptor.value);
    if (!synced.succeeded) {
        if (synced.stopped || !means_no_sync_offered(synced.code)) {
            return report_staged_outcome(staged, error, operation, bc_file_system_call_sync_step(engine), &synced,
                                         bc_file_system_call_kind_of_errno(synced.code));
        }
        *achieved = bc_file_system_durability_written;
    }
    const bc_file_system_call_outcome closed =
        bc_file_system_call_close(engine, operation, release_descriptor(&staged->descriptor));
    if (!closed.succeeded) {
        return report_staged_outcome(staged, error, operation, bc_file_system_step_close, &closed,
                                     bc_file_system_call_kind_of_errno(closed.code));
    }
    return 0;
}

/* The staged file takes the destination's name in one step: `replace` with renameat, which replaces a file
 * atomically; `refuse` with the platform's no-replace rename, whose EEXIST is `already_exists` with the
 * destination untouched. Afterwards the staged name is gone, so the file is kept from the cleanup; a failure
 * leaves it to be discarded. */
static int32_t rename_into_place(bc_file_system_engine *engine, int32_t operation, staged_file *staged,
                                 const uint8_t *leaf, int32_t existing_destination, bc_file_system_error *error) {
    const char *from = (const char *)staged->name;
    const char *to = (const char *)leaf;
    int32_t step = bc_file_system_step_renameat;
    bc_file_system_call_outcome outcome;
    if (existing_destination == bc_file_system_existing_destination_replace) {
        outcome = bc_file_system_call_renameat(engine, operation, staged->parent, from, staged->parent, to);
    } else {
        step = bc_file_system_step_rename_no_replace;
        outcome = bc_file_system_call_rename_no_replace(engine, operation, staged->parent, from, staged->parent, to);
    }
    if (!outcome.succeeded) {
        return report_staged_outcome(staged, error, operation, step, &outcome,
                                     bc_file_system_call_kind_of_errno(outcome.code));
    }
    staged->kept = true;
    return 0;
}

int32_t bc_file_system_write_file(bc_file_system_engine *engine, const bc_file_system_location *location,
                                  const uint8_t *bytes, size_t byte_count, const bc_file_system_write_options *options,
                                  int32_t *durability_achieved, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_write_file;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_byte_span(bytes, byte_count) ||
        !is_valid_write_options(options) || durability_achieved == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    destination_entry existing;
    kind = judge_destination(engine, operation, location, options->final_symlink, &resolved, &existing, error);
    if (kind != 0) {
        return kind;
    }
    /* `preserve_or` keeps the mode the file being replaced has, so an operator's stricter mode survives the
     * rewrite; a new file gets the stated mode under either policy. */
    uint32_t mode = options->permissions.mode;
    if (options->permissions.policy == bc_file_system_permissions_policy_preserve_or && existing.present) {
        mode = existing.mode;
    }
    /* Every write is staged: the bytes go into a new file beside the destination, which takes the destination's
     * name only once it is complete, so a reader sees the old content or the new and never a mixture. */
    [[gnu::cleanup(discard_staged_file)]] staged_file staged = staged_file_in(resolved.parent);
    kind = create_staged_file(engine, operation, &staged, resolved.leaf, resolved.leaf_length, error);
    if (kind != 0) {
        return kind;
    }
    kind = write_every_byte(engine, operation, &staged, bytes, byte_count, error);
    if (kind != 0) {
        return kind;
    }
    /* The mode is set exactly, whatever the umask left on the staged file. */
    const bc_file_system_call_outcome mode_set =
        bc_file_system_call_fchmod(engine, operation, staged.descriptor.value, mode);
    if (!mode_set.succeeded) {
        return report_staged_outcome(&staged, error, operation, bc_file_system_step_fchmod, &mode_set,
                                     bc_file_system_call_kind_of_errno(mode_set.code));
    }
    int32_t achieved = options->durability;
    if (achieved == bc_file_system_durability_durable) {
        kind = settle_staged_file(engine, operation, &staged, &achieved, error);
        if (kind != 0) {
            return kind;
        }
    }
    kind = rename_into_place(engine, operation, &staged, resolved.leaf, options->existing_destination, error);
    if (kind != 0) {
        return kind;
    }
    /* The directory's sync makes the new name itself durable; the parent the walk holds is the directory. */
    if (achieved == bc_file_system_durability_durable) {
        kind = sync_directory_after_change(engine, operation, resolved.parent, &achieved, error);
        if (kind != 0) {
            return kind;
        }
    }
    *durability_achieved = achieved;
    return succeed(error);
}

/* ---- append -------------------------------------------------------------------------------------------------- */

/* Opens the file named by `file` for appending, creating it with `mode` when nothing is there: the exclusive
 * create first, so that "new" is known for certain (a new file gets its mode set and its directory synced),
 * then the plain open when the name was taken. A symlink is refused by O_NOFOLLOW (ELOOP) and a directory by
 * the write-only open (EISDIR); anything else that is not a regular file is refused after a look at the
 * descriptor, with no step, like a refusal. */
static int32_t open_for_append(bc_file_system_engine *engine, int32_t operation, staged_file *file, uint32_t mode,
                               bool *is_new, bc_file_system_error *error) {
    const char *name = (const char *)file->name;
    const bc_file_system_call_outcome created = bc_file_system_call_openat_creating(
        engine, operation, file->parent, name, append_create_flags, mode, &file->descriptor.value);
    if (created.succeeded) {
        file->created = true;
        *is_new = true;
        return 0;
    }
    if (created.stopped || created.code != EEXIST) {
        return report_outcome(error, operation, bc_file_system_step_openat, &created,
                              bc_file_system_call_kind_of_errno(created.code));
    }
    const bc_file_system_call_outcome opened =
        bc_file_system_call_openat(engine, operation, file->parent, name, append_open_flags, &file->descriptor.value);
    if (!opened.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_openat, &opened,
                              bc_file_system_call_kind_of_errno(opened.code));
    }
    struct stat info;
    const bc_file_system_call_outcome measured =
        bc_file_system_call_fstat(engine, operation, file->descriptor.value, &info);
    if (!measured.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstat, &measured,
                              bc_file_system_call_kind_of_errno(measured.code));
    }
    if (!S_ISREG(info.st_mode)) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    *is_new = false;
    return 0;
}

int32_t bc_file_system_append(bc_file_system_engine *engine, const bc_file_system_location *location,
                              const uint8_t *bytes, size_t byte_count, int32_t durability,
                              const bc_file_system_permissions *permissions, int32_t *durability_achieved,
                              bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_append;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_byte_span(bytes, byte_count) ||
        !is_in_enum(durability, durability_last) || !is_valid_permissions(permissions) ||
        durability_achieved == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root has no leaf and is a directory. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    /* The file is this operation's own until its mode is set: a new file whose mode cannot be set is taken
     * back, so a failure never leaves a file with a mode nobody chose. */
    [[gnu::cleanup(discard_staged_file)]] staged_file file = staged_file_in(resolved.parent);
    memcpy(file.name, resolved.leaf, resolved.leaf_length);
    bool is_new = false;
    kind = open_for_append(engine, operation, &file, permissions->mode, &is_new, error);
    if (kind != 0) {
        return kind;
    }
    if (is_new) {
        const bc_file_system_call_outcome mode_set =
            bc_file_system_call_fchmod(engine, operation, file.descriptor.value, permissions->mode);
        if (!mode_set.succeeded) {
            return report_staged_outcome(&file, error, operation, bc_file_system_step_fchmod, &mode_set,
                                         bc_file_system_call_kind_of_errno(mode_set.code));
        }
    }
    /* From here the file is the caller's: an append is not atomic, and the header says a failure may leave
     * some leading part of the bytes in it. */
    file.kept = true;
    kind = write_every_byte(engine, operation, &file, bytes, byte_count, error);
    if (kind != 0) {
        return kind;
    }
    int32_t achieved = durability;
    if (achieved == bc_file_system_durability_durable) {
        /* The bytes are visible already, so every failure from here lowers the answer; the close is the last
         * call that can report a write the system deferred, and a new name needs its directory synced too. */
        kind = sync_after_change(engine, operation, file.descriptor.value, &achieved, error);
        if (kind != 0) {
            return kind;
        }
        const bc_file_system_call_outcome closed =
            bc_file_system_call_close(engine, operation, release_descriptor(&file.descriptor));
        if (closed.stopped) {
            return report_outcome(error, operation, bc_file_system_step_close, &closed,
                                  bc_file_system_error_kind_io_failed);
        }
        if (!closed.succeeded) {
            achieved = bc_file_system_durability_written; /* discard-ok: lowers the answer, never an error */
        }
        if (is_new && achieved == bc_file_system_durability_durable) {
            kind = sync_directory_after_change(engine, operation, resolved.parent, &achieved, error);
            if (kind != 0) {
                return kind;
            }
        }
    }
    *durability_achieved = achieved;
    return succeed(error);
}

/* ---- sync ---------------------------------------------------------------------------------------------------- */

int32_t bc_file_system_sync(bc_file_system_engine *engine, const bc_file_system_location *location,
                            int32_t *durability_achieved, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_sync;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || durability_achieved == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    const int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* A file or a directory opens read-only to be synced; a symlink is refused, since the call takes no choice
     * about one. The root has no leaf: the directory the walk opened is what the location names. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor target = {.value = -1};
    if (resolved.leaf_length == 0) {
        adopt_descriptor(&target, &resolved.opened);
    } else {
        const bc_file_system_call_outcome opened = bc_file_system_call_openat(
            engine, operation, resolved.parent, (const char *)resolved.leaf, sync_open_flags, &target.value);
        if (!opened.succeeded) {
            return report_outcome(error, operation, bc_file_system_step_openat, &opened,
                                  bc_file_system_call_kind_of_errno(opened.code));
        }
    }
    /* This call makes no change of its own, so the rule that lowers a failed sync to `written` does not apply:
     * a volume that offers no sync answers `written`, and a sync that fails is the error it is. */
    const bc_file_system_call_outcome synced = bc_file_system_call_sync(engine, operation, target.value);
    if (synced.succeeded) {
        *durability_achieved = bc_file_system_durability_durable;
        return succeed(error);
    }
    if (!synced.stopped && means_no_sync_offered(synced.code)) {
        *durability_achieved = bc_file_system_durability_written;
        return succeed(error);
    }
    return report_outcome(error, operation, bc_file_system_call_sync_step(engine), &synced,
                          bc_file_system_call_kind_of_errno(synced.code));
}

/* ---- Directories --------------------------------------------------------------------------------------------- */

/* The prefix of a temporary directory's name (`tmp-<process id>-<token>`): the engine's counter makes it
 * unique, as it does a staged file's name, and it carries no `.bc-` infix because the directory is the
 * caller's to keep or remove, never a sweep's. */
static const char temporary_directory_prefix[] = "tmp-";

/* The most a temporary directory's name holds: the prefix, the process id, the dash and the token. */
enum : size_t {
    temporary_name_capacity =
        (sizeof temporary_directory_prefix - 1) + decimal_digit_capacity + 1 + decimal_digit_capacity
};
static_assert(temporary_name_capacity <= NAME_MAX, "a temporary directory's name must be one component");

/* Whether a platform answered that a flag it was given has no meaning here (ENOTSUP, EOPNOTSUPP: one number
 * on Linux, two on macOS, so an if-chain). */
static bool means_flag_unsupported(int code) {
    if (code == ENOTSUP) {
        return true;
    }
    return code == EOPNOTSUPP;
}

/* A directory this operation created in `parent` under `name` and still owns, until it has its mode: declared
 * under `[[gnu::cleanup(discard_created_directory)]]`, so a failing path removes it again (a directory with a
 * mode nobody chose is never left behind) and a stop keeps it, as a kill would. */
typedef struct created_directory {
    int parent;                 /* borrowed: the directory it was created in */
    uint8_t name[NAME_MAX + 1]; /* NUL-terminated */
    bool created;
    bool kept;
} created_directory;

static created_directory created_directory_in(int parent, const uint8_t *name) {
    created_directory created = {.parent = parent, .name = {}, .created = false, .kept = false};
    memcpy(created.name, name, strlen((const char *)name));
    return created;
}

/* The cleanup: the removal is best effort after the real error, and nothing it could report changes it. */
static void discard_created_directory(created_directory *created) {
    if (!created->created || created->kept) {
        return;
    }
    (void)unlinkat(created->parent, (const char *)created->name, AT_REMOVEDIR); /* discard-ok: after the error */
    created->created = false;
}

/* The one place an operation holding a created directory reads a verdict: a stop keeps the directory, as a
 * kill would, and is reported as every stop is. */
static int32_t report_created_outcome(created_directory *created, bc_file_system_error *error, int32_t operation,
                                      int32_t step, const bc_file_system_call_outcome *outcome, int32_t kind) {
    if (outcome->stopped) {
        created->kept = true;
    }
    return report_outcome(error, operation, step, outcome, kind);
}

/* Sets the mode of `name` under `parent` exactly, on the entry itself and never on what a symlink points at.
 * The first way is `fchmodat` with AT_SYMLINK_NOFOLLOW, which Apple platforms honour on every entry and
 * which glibc since 2.32 and musl honour on everything but a symlink (through the fchmodat2 call where the
 * kernel has it, else through an O_PATH descriptor and /proc); a libc that cannot honour the flag answers
 * ENOTSUP or EOPNOTSUPP, and then the entry is opened without following (a symlink answers ELOOP, which the
 * table reads as `symlink_refused`) and `fchmod` sets the mode on the descriptor. Answers the outcome of the
 * call that decided, with `*failed_step` naming it when it failed. */
static bc_file_system_call_outcome set_mode_never_following(bc_file_system_engine *engine, int32_t operation,
                                                            int parent, const char *name, uint32_t mode,
                                                            int32_t *failed_step) {
    const bc_file_system_call_outcome by_name =
        bc_file_system_call_fchmodat(engine, operation, parent, name, mode, AT_SYMLINK_NOFOLLOW);
    if (by_name.succeeded) {
        return by_name;
    }
    if (by_name.stopped || !means_flag_unsupported(by_name.code)) {
        *failed_step = bc_file_system_step_fchmodat;
        return by_name;
    }
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor entry = {.value = -1};
    const bc_file_system_call_outcome opened =
        bc_file_system_call_openat(engine, operation, parent, name, file_read_flags | O_NOFOLLOW, &entry.value);
    if (!opened.succeeded) {
        *failed_step = bc_file_system_step_openat;
        return opened;
    }
    const bc_file_system_call_outcome changed = bc_file_system_call_fchmod(engine, operation, entry.value, mode);
    if (!changed.succeeded) {
        *failed_step = bc_file_system_step_fchmod;
    }
    return changed;
}

/* The one call that makes a directory, in the shape every creating operation uses: made with a private mode
 * first (nobody else may enter it before it has its own), then given the stated mode exactly, whatever the
 * umask left. `created` must name the parent and the name and hold nothing yet. */
static int32_t make_directory_with_mode(bc_file_system_engine *engine, int32_t operation, created_directory *created,
                                        uint32_t mode, bc_file_system_error *error) {
    const char *name = (const char *)created->name;
    const bc_file_system_call_outcome made =
        bc_file_system_call_mkdirat(engine, operation, created->parent, name, directory_create_mode);
    if (!made.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_mkdirat, &made,
                              bc_file_system_call_kind_of_errno(made.code));
    }
    created->created = true;
    int32_t failed_step = 0;
    const bc_file_system_call_outcome mode_set =
        set_mode_never_following(engine, operation, created->parent, name, mode, &failed_step);
    if (!mode_set.succeeded) {
        return report_created_outcome(created, error, operation, failed_step, &mode_set,
                                      bc_file_system_call_kind_of_errno(mode_set.code));
    }
    created->kept = true;
    return 0;
}

/* Opens the directory a resolved location names, for `access`, bracketed by the identity check like every
 * component before it: the leaf under the caller's choice about a final symlink, or, for the root, `.` beneath
 * the descriptor the walk opened (which is open for search, and a listing needs more). */
static int32_t open_resolved_directory(bc_file_system_engine *engine, int32_t operation,
                                       const bc_file_system_resolved *resolved, int32_t final_symlink, int32_t access,
                                       bc_file_system_expected_entry *expected, owned_descriptor *opened,
                                       bc_file_system_error *error) {
    const char *name = resolved->leaf_length == 0 ? "." : (const char *)resolved->leaf;
    const int32_t kind =
        bc_file_system_resolve_expect_entry(engine, operation, resolved->parent, name, final_symlink, expected, error);
    if (kind != 0) {
        return kind;
    }
    return bc_file_system_resolve_open_expected_directory(engine, operation, resolved->parent, name, final_symlink,
                                                          access, expected, opened, error);
}

/* The choice a location makes about a symlink on its way: an `at` path is the system's to resolve, a
 * `beneath` path may hold no symlink at all. What an operation without a `final_symlink` of its own applies to
 * the directory it was handed. */
static int32_t symlink_choice_of(const bc_file_system_location *location) {
    if (location->kind == bc_file_system_location_kind_at) {
        return bc_file_system_final_symlink_follow;
    }
    return bc_file_system_final_symlink_refuse;
}

/* ---- create_directory ---------------------------------------------------------------------------------------- */

int32_t bc_file_system_create_directory(bc_file_system_engine *engine, const bc_file_system_location *location,
                                        const bc_file_system_permissions *permissions, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_create_directory;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_valid_permissions(permissions)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    const int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root is always there. A final symlink has no meaning for a creation: the name must be free, and a
     * symlink holding it is something already there, which mkdirat answers EEXIST for. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_already_exists, operation);
    }
    [[gnu::cleanup(discard_created_directory)]] created_directory created =
        created_directory_in(resolved.parent, resolved.leaf);
    const int32_t made = make_directory_with_mode(engine, operation, &created, permissions->mode, error);
    if (made != 0) {
        return made;
    }
    return succeed(error);
}

/* ---- create_directories -------------------------------------------------------------------------------------- */

/* Whether a failed creation was mkdirat answering EEXIST: the name is held by something now. */
static bool name_was_taken(int32_t kind, const bc_file_system_error *error) {
    if (kind != bc_file_system_error_kind_already_exists) {
        return false;
    }
    if (error->step != bc_file_system_step_mkdirat) {
        return false;
    }
    return error->system_code == EEXIST;
}

/* One component of the chain: opened as a directory when it is there (bracketed like a walk's component, the
 * system resolving a symlink under `at` and `beneath` refusing one), made with the mode and then opened when
 * it is not. `*child` receives the opened directory. */
static int32_t open_or_create_component(bc_file_system_engine *engine, int32_t operation, int parent,
                                        const uint8_t *name, int32_t final_symlink, uint32_t mode,
                                        owned_descriptor *child, bc_file_system_error *error) {
    bc_file_system_expected_entry expected;
    int32_t kind = bc_file_system_resolve_expect_entry(engine, operation, parent, (const char *)name, final_symlink,
                                                       &expected, error);
    if (kind == bc_file_system_error_kind_not_found) {
        [[gnu::cleanup(discard_created_directory)]] created_directory created = created_directory_in(parent, name);
        kind = make_directory_with_mode(engine, operation, &created, mode, error);
        /* A name taken since the stat (this chain being made twice at once) is one that is there now, and what
         * is there is judged below like anything else; every other failure is the operation's. */
        if (kind != 0 && !name_was_taken(kind, error)) {
            return kind;
        }
        kind = bc_file_system_resolve_expect_entry(engine, operation, parent, (const char *)name, final_symlink,
                                                   &expected, error);
    }
    if (kind != 0) {
        return kind;
    }
    return bc_file_system_resolve_open_expected_directory(engine, operation, parent, (const char *)name, final_symlink,
                                                          bc_file_system_directory_access_search, &expected, child,
                                                          error);
}

int32_t bc_file_system_create_directories(bc_file_system_engine *engine, const bc_file_system_location *location,
                                          const bc_file_system_permissions *permissions, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_create_directories;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_valid_permissions(permissions)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    /* The chain is walked from where the location starts, the root or the held directory, component by
     * component, each one opened when it is there and made when it is not; so a parent that is missing is
     * made on the way, which is what this operation is for, and an existing one is left as it is. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved start = bc_file_system_resolved_empty();
    int current = -1;
    if (location->kind == bc_file_system_location_kind_at) {
        const bc_file_system_location root = bc_file_system_location_at((const uint8_t *)"/", 1);
        const int32_t kind = bc_file_system_resolve(engine, operation, &root, &start, error);
        if (kind != 0) {
            return kind;
        }
        current = start.parent;
    } else {
        if (!bc_file_system_platform_enforces_containment()) {
            return refuse(error, bc_file_system_error_kind_unsupported, operation);
        }
        current = location->directory->descriptor;
    }
    const int32_t final_symlink = symlink_choice_of(location);
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor held = {.value = -1};
    size_t cursor = 0;
    size_t offset = 0;
    size_t component_length = 0;
    while (bc_file_system_path_text_next_component(location->path, location->length, &cursor, &offset,
                                                   &component_length)) {
        uint8_t name[NAME_MAX + 1] = {};
        memcpy(name, location->path + offset, component_length);
        [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor child = {.value = -1};
        const int32_t kind =
            open_or_create_component(engine, operation, current, name, final_symlink, permissions->mode, &child, error);
        if (kind != 0) {
            return kind;
        }
        adopt_descriptor(&held, &child);
        current = held.value;
    }
    return succeed(error);
}

/* ---- list_directory ------------------------------------------------------------------------------------------ */

/* The next entry of the stream that is not `.` or `..`, or none at the end. The name is copied out before the
 * next call, as the stream owns it. */
static int32_t next_entry_name(bc_file_system_engine *engine, int32_t operation, DIR *stream, uint8_t *name,
                               size_t *name_length, unsigned char *entry_type, bool *found,
                               bc_file_system_error *error) {
    for (;;) {
        struct dirent *entry = nullptr;
        const bc_file_system_call_outcome read = bc_file_system_call_readdir(engine, operation, stream, &entry);
        if (!read.succeeded) {
            return report_outcome(error, operation, bc_file_system_step_readdir, &read,
                                  bc_file_system_call_kind_of_errno(read.code));
        }
        if (entry == nullptr) {
            *found = false;
            return 0;
        }
        const size_t length = strlen(entry->d_name);
        if (length == 1 && entry->d_name[0] == '.') {
            continue;
        }
        if (length == 2 && entry->d_name[0] == '.' && entry->d_name[1] == '.') {
            continue;
        }
        /* NAME_MAX bounds every name the system answers; a longer one would be the system breaking its word. */
        if (length > NAME_MAX) {
            return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
        }
        memcpy(name, entry->d_name, length);
        name[length] = 0;
        *name_length = length;
        *entry_type = entry->d_type;
        *found = true;
        return 0;
    }
}

/* What a directory stream says an entry is, from the advisory d_type; `DT_UNKNOWN` means the file system did
 * not say and the entry is asked with fstatat, never followed. */
static int32_t entry_type_of_listed(bc_file_system_engine *engine, int32_t operation, int directory, const char *name,
                                    unsigned char d_type, int32_t *entry_type, bool *present,
                                    bc_file_system_error *error) {
    *present = true;
    if (d_type == DT_REG) {
        *entry_type = bc_file_system_entry_type_regular_file;
        return 0;
    }
    if (d_type == DT_DIR) {
        *entry_type = bc_file_system_entry_type_directory;
        return 0;
    }
    if (d_type == DT_LNK) {
        *entry_type = bc_file_system_entry_type_symlink;
        return 0;
    }
    if (d_type != DT_UNKNOWN) {
        *entry_type = bc_file_system_entry_type_other;
        return 0;
    }
    struct stat info;
    const bc_file_system_call_outcome outcome =
        bc_file_system_call_fstatat(engine, operation, directory, name, AT_SYMLINK_NOFOLLOW, &info);
    if (!outcome.succeeded) {
        /* An entry removed between the read and the stat is not part of the listing. */
        if (!outcome.stopped && outcome.code == ENOENT) {
            *present = false;
            return 0;
        }
        const int32_t kind = report_outcome(error, operation, bc_file_system_step_fstatat, &outcome,
                                            bc_file_system_call_kind_of_errno(outcome.code));
        const size_t length = strlen(name);
        memcpy(error->name, name, length);
        error->name_length = (uint32_t)length;
        return kind;
    }
    *entry_type = entry_type_of(info.st_mode);
    return 0;
}

/* The order of a listing: by name bytes, a shorter name before a longer one it is a prefix of; qsort's
 * comparator. */
static int compare_entry_names(const void *left, const void *right) {
    const bc_file_system_entry *first = left;
    const bc_file_system_entry *second = right;
    const size_t shorter = first->name_length < second->name_length ? first->name_length : second->name_length;
    const int order = memcmp(first->name, second->name, shorter);
    if (order != 0) {
        return order;
    }
    if (first->name_length < second->name_length) {
        return -1;
    }
    if (first->name_length > second->name_length) {
        return 1;
    }
    return 0;
}

/* Counts the entries of the stream, from its start to its end. */
static int32_t count_entries(bc_file_system_engine *engine, int32_t operation, DIR *stream, size_t *total,
                             bc_file_system_error *error) {
    *total = 0;
    for (;;) {
        uint8_t name[NAME_MAX + 1];
        size_t name_length = 0;
        unsigned char d_type = DT_UNKNOWN;
        bool found = false;
        const int32_t kind = next_entry_name(engine, operation, stream, name, &name_length, &d_type, &found, error);
        if (kind != 0) {
            return kind;
        }
        if (!found) {
            return 0;
        }
        *total += 1;
    }
}

/* Stores the entries of the stream into the caller's array, at most `capacity` of them, and counts every one:
 * `*seen` above `capacity` means the directory grew since it was counted. */
static int32_t store_entries(bc_file_system_engine *engine, int32_t operation, DIR *stream,
                             bc_file_system_entry *entries, size_t capacity, size_t *seen,
                             bc_file_system_error *error) {
    *seen = 0;
    for (;;) {
        uint8_t name[NAME_MAX + 1];
        size_t name_length = 0;
        unsigned char d_type = DT_UNKNOWN;
        bool found = false;
        int32_t kind = next_entry_name(engine, operation, stream, name, &name_length, &d_type, &found, error);
        if (kind != 0) {
            return kind;
        }
        if (!found) {
            return 0;
        }
        if (*seen >= capacity) {
            *seen += 1;
            continue;
        }
        int32_t entry_type = 0;
        bool present = false;
        kind = entry_type_of_listed(engine, operation, dirfd(stream), (const char *)name, d_type, &entry_type, &present,
                                    error);
        if (kind != 0) {
            return kind;
        }
        if (!present) {
            continue;
        }
        bc_file_system_entry *stored = &entries[*seen];
        *stored = (bc_file_system_entry){.entry_type = entry_type, .name_length = (uint32_t)name_length, .name = {}};
        memcpy(stored->name, name, name_length);
        *seen += 1;
    }
}

int32_t bc_file_system_list_directory(bc_file_system_engine *engine, const bc_file_system_location *location,
                                      int32_t final_symlink, bc_file_system_entry *entries, size_t capacity,
                                      size_t *count, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_list_directory;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_in_enum(final_symlink, final_symlink_last) ||
        (entries == nullptr && capacity != 0) || count == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The directory is opened read-only, which a listing needs and the walk's search-only open does not give,
     * bracketed like every component before it; the stream then owns the descriptor. */
    bc_file_system_expected_entry expected;
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    kind = open_resolved_directory(engine, operation, &resolved, final_symlink, bc_file_system_directory_access_read,
                                   &expected, &opened, error);
    if (kind != 0) {
        return kind;
    }
    [[gnu::cleanup(close_owned_directory_stream)]] owned_directory_stream stream = {.value = nullptr};
    const bc_file_system_call_outcome streamed =
        bc_file_system_call_fdopendir(engine, operation, opened.value, &stream.value);
    if (!streamed.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fdopendir, &streamed,
                              bc_file_system_call_kind_of_errno(streamed.code));
    }
    (void)release_descriptor(&opened);
    /* The entries are counted first, so a listing the caller's array cannot hold stores nothing and answers
     * the count it needs; then the stream is read again from its start and the entries stored. A directory
     * that grew in between is answered `buffer_too_small` again, with its new count, so the face's bounded
     * grow loop is the only retry. A listing is complete or it is an error: nothing stored on the way is an
     * answer. */
    size_t total = 0;
    kind = count_entries(engine, operation, stream.value, &total, error);
    if (kind != 0) {
        return kind;
    }
    if (total > capacity) {
        *count = total;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    rewinddir(stream.value);
    size_t seen = 0;
    kind = store_entries(engine, operation, stream.value, entries, capacity, &seen, error);
    if (kind != 0) {
        return kind;
    }
    if (seen > capacity) {
        *count = seen;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    /* Sorted by name bytes so the listing is the same on every run and every platform: qsort, which the hosted
     * tier may use, over the caller's own array. */
    if (seen > 1) {
        qsort(entries, seen, sizeof entries[0], compare_entry_names);
    }
    *count = seen;
    return succeed(error);
}

/* ---- remove_file and remove_directory ----------------------------------------------------------------------- */

int32_t bc_file_system_remove_file(bc_file_system_engine *engine, const bc_file_system_location *location,
                                   int32_t *existed, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_remove_file;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || existed == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    const int32_t resolution = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (resolution != 0) {
        if (means_absence(resolution)) {
            *existed = 0;
            return succeed(error);
        }
        return resolution;
    }
    /* The root is a directory. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    /* unlinkat without AT_REMOVEDIR removes a file or a symlink (the link itself, never what it points at) and
     * refuses a directory: EISDIR on Linux, EPERM on Apple platforms, where EPERM also means what it means
     * everywhere, so a look at the entry tells the two apart. */
    const bc_file_system_call_outcome removed =
        bc_file_system_call_unlinkat(engine, operation, resolved.parent, (const char *)resolved.leaf, 0);
    if (removed.succeeded) {
        *existed = 1;
        return succeed(error);
    }
    if (!removed.stopped && removed.code == ENOENT) {
        *existed = 0;
        return succeed(error);
    }
    int32_t kind = bc_file_system_call_kind_of_errno(removed.code);
    if (!removed.stopped && removed.code == EPERM) {
        struct stat info;
        const bc_file_system_call_outcome looked = bc_file_system_call_fstatat(
            engine, operation, resolved.parent, (const char *)resolved.leaf, AT_SYMLINK_NOFOLLOW, &info);
        if (!looked.succeeded) {
            return report_outcome(error, operation, bc_file_system_step_fstatat, &looked,
                                  bc_file_system_call_kind_of_errno(looked.code));
        }
        if (S_ISDIR(info.st_mode)) {
            kind = bc_file_system_error_kind_not_regular_file;
        }
    }
    return report_outcome(error, operation, bc_file_system_step_unlinkat, &removed, kind);
}

/* The kind a refused directory removal decides: a directory with entries is `directory_not_empty` (ENOTEMPTY,
 * or EEXIST where the system spells it that way), anything else is the table's. */
static int32_t kind_of_directory_removal_failure(int code) {
    if (code == ENOTEMPTY) {
        return bc_file_system_error_kind_directory_not_empty;
    }
    if (code == EEXIST) {
        return bc_file_system_error_kind_directory_not_empty;
    }
    return bc_file_system_call_kind_of_errno(code);
}

int32_t bc_file_system_remove_directory(bc_file_system_engine *engine, const bc_file_system_location *location,
                                        int32_t *existed, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_remove_directory;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || existed == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    const int32_t resolution = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (resolution != 0) {
        if (means_absence(resolution)) {
            *existed = 0;
            return succeed(error);
        }
        return resolution;
    }
    /* The root of the file system is refused, as it is for a tree. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const bc_file_system_call_outcome removed =
        bc_file_system_call_unlinkat(engine, operation, resolved.parent, (const char *)resolved.leaf, AT_REMOVEDIR);
    if (removed.succeeded) {
        *existed = 1;
        return succeed(error);
    }
    if (!removed.stopped && removed.code == ENOENT) {
        *existed = 0;
        return succeed(error);
    }
    return report_outcome(error, operation, bc_file_system_step_unlinkat, &removed,
                          kind_of_directory_removal_failure(removed.code));
}

/* ---- remove_tree --------------------------------------------------------------------------------------------- */

enum : uint32_t {
    /* How many levels below the top of the tree the removal holds open at once. A process under launchd starts
     * with 256 descriptors, so one per level would fail a few hundred levels down; with a window of the
     * deepest sixteen the removal re-descends from the top for anything deeper, in passes, which cleared a
     * 400-level chain in half a second (the S6 spike) and never needs more than the window and a handful. */
    removal_window_capacity = 16
};

/* One level of the tree the removal holds: the directory's stream, whose descriptor serves the calls inside
 * it, its name under the level above, and whether its stream was already read again after a removal that the
 * system refused as not empty. */
typedef struct held_level {
    owned_directory_stream stream;
    uint8_t name[NAME_MAX + 1]; /* NUL-terminated */
    bool rescanned;
} held_level;

/* The deepest `count` of the `depth` levels the walk stands below the top: when the two differ, levels nearer
 * the top fell out of the window and the walk cannot climb through them, so the pass ends and the next one
 * descends from the top again. Declared under `[[gnu::cleanup(close_removal_window)]]`; a zeroed window holds
 * nothing (a NULL stream is none). */
typedef struct removal_window {
    held_level levels[removal_window_capacity];
    uint32_t count;
    uint32_t depth;
} removal_window;

static void close_removal_window(removal_window *window) {
    for (uint32_t index = 0; index < window->count; ++index) {
        close_owned_directory_stream(&window->levels[index].stream);
    }
    window->count = 0;
    window->depth = 0;
}

/* Holds `stream` as the new deepest level; when the window is full the level nearest the top is let go. */
static void push_level(removal_window *window, owned_directory_stream *stream, const uint8_t *name) {
    if (window->count == removal_window_capacity) {
        close_owned_directory_stream(&window->levels[0].stream);
        memmove(&window->levels[0], &window->levels[1], sizeof window->levels[0] * (removal_window_capacity - 1));
        window->count -= 1;
    }
    held_level *level = &window->levels[window->count];
    *level = (held_level){.stream = {.value = release_directory_stream(stream)}, .name = {}, .rescanned = false};
    memcpy(level->name, name, strlen((const char *)name));
    window->count += 1;
    window->depth += 1;
}

/* Lets go of the deepest level, whose directory has been removed. */
static void pop_level(removal_window *window) {
    close_owned_directory_stream(&window->levels[window->count - 1].stream);
    window->count -= 1;
    window->depth -= 1;
}

/* Names the entry a step of the removal failed on, relative to the location: the held levels' names, a slash
 * between each, and the entry's own (empty for the level's directory itself). When the window no longer holds
 * every level down from the top, or the spelling does not fit, the name is cut where it stops being a prefix
 * and `name_truncated` says so. */
static void name_entry_in_error(bc_file_system_error *error, const removal_window *window, const uint8_t *entry) {
    const bool whole_chain_held = window->depth == window->count;
    size_t position = 0;
    bool fits = whole_chain_held;
    for (uint32_t index = 0; fits && index < window->count; ++index) {
        const size_t length = strlen((const char *)window->levels[index].name);
        if (position + length + 1 > sizeof error->name) {
            fits = false;
            break;
        }
        if (position > 0) {
            error->name[position] = '/';
            position += 1;
        }
        memcpy(error->name + position, window->levels[index].name, length);
        position += length;
    }
    const size_t entry_length = strlen((const char *)entry);
    if (fits && entry_length > 0) {
        if (position + entry_length + 1 > sizeof error->name) {
            fits = false;
        } else {
            if (position > 0) {
                error->name[position] = '/';
                position += 1;
            }
            memcpy(error->name + position, entry, entry_length);
            position += entry_length;
        }
    }
    error->name_length = (uint32_t)position;
    error->name_truncated = 0;
    if (!fits) {
        error->name_truncated = 1;
    }
}

/* The directory the walk stands in: the deepest held level's, or the top's. */
static DIR *current_stream(const removal_window *window, DIR *top) {
    if (window->count == 0) {
        return top;
    }
    return window->levels[window->count - 1].stream.value;
}

/* Removes the deepest held level's directory through its parent. The parent is held when the level above is
 * in the window or the level is directly below the top; otherwise it fell out of the window and the pass
 * must end, which `*parent_lost` says. A directory the system refuses as not empty is read again once, from
 * its start, since an entry may have arrived since its stream ended; a second refusal is the error. */
static int32_t remove_deepest_level(bc_file_system_engine *engine, int32_t operation, removal_window *window, DIR *top,
                                    bool *parent_lost, bc_file_system_error *error) {
    *parent_lost = false;
    /* Called only below the top, where at least the deepest level is held: the pass loop guarantees it, and
     * the analyzer reads this line to see that. */
    assert(window->count > 0);
    held_level *level = &window->levels[window->count - 1];
    int parent = -1;
    if (window->count >= 2) {
        parent = dirfd(window->levels[window->count - 2].stream.value);
    } else if (window->depth == 1) {
        parent = dirfd(top);
    } else {
        *parent_lost = true;
        return 0;
    }
    const bc_file_system_call_outcome removed =
        bc_file_system_call_unlinkat(engine, operation, parent, (const char *)level->name, AT_REMOVEDIR);
    if (removed.succeeded) {
        pop_level(window);
        return 0;
    }
    const int32_t kind = kind_of_directory_removal_failure(removed.code);
    if (!removed.stopped && kind == bc_file_system_error_kind_directory_not_empty && !level->rescanned) {
        level->rescanned = true;
        rewinddir(level->stream.value);
        return 0;
    }
    const int32_t reported = report_outcome(error, operation, bc_file_system_step_unlinkat, &removed, kind);
    name_entry_in_error(error, window, (const uint8_t *)"");
    return reported;
}

/* Descends into the directory entry `name` of the directory the walk stands in: opened without following and
 * bracketed by the identity check, held against the tree's device (a mount point inside the tree is where the
 * removal stops, with `cross_device`), then read through a stream of its own. */
static int32_t descend_into(bc_file_system_engine *engine, int32_t operation, removal_window *window, DIR *top,
                            const uint8_t *name, const bc_file_system_expected_entry *expected, uint64_t tree_device,
                            bc_file_system_error *error) {
    if (expected->device != tree_device) {
        const int32_t kind = refuse(error, bc_file_system_error_kind_cross_device, operation);
        name_entry_in_error(error, window, name);
        return kind;
    }
    const int directory = dirfd(current_stream(window, top));
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor child = {.value = -1};
    int32_t kind = bc_file_system_resolve_open_expected_directory(
        engine, operation, directory, (const char *)name, bc_file_system_final_symlink_refuse,
        bc_file_system_directory_access_read, expected, &child, error);
    if (kind != 0) {
        name_entry_in_error(error, window, name);
        return kind;
    }
    [[gnu::cleanup(close_owned_directory_stream)]] owned_directory_stream stream = {.value = nullptr};
    const bc_file_system_call_outcome streamed =
        bc_file_system_call_fdopendir(engine, operation, child.value, &stream.value);
    if (!streamed.succeeded) {
        kind = report_outcome(error, operation, bc_file_system_step_fdopendir, &streamed,
                              bc_file_system_call_kind_of_errno(streamed.code));
        name_entry_in_error(error, window, name);
        return kind;
    }
    (void)release_descriptor(&child);
    push_level(window, &stream, name);
    return 0;
}

/* Where a pass of the removal stands after one step. */
typedef enum pass_verdict : int32_t {
    pass_continues = 1,
    /* The parent of the deepest level fell out of the window: the next pass descends from the top again. */
    pass_ends = 2,
    tree_removed = 3
} pass_verdict;

/* The top is empty: it goes through the location's parent. A refusal as not empty (an entry arrived since its
 * stream ended) is answered by one more read of it from the start, once. */
static int32_t remove_top(bc_file_system_engine *engine, int32_t operation, int parent, const uint8_t *leaf,
                          bool *top_rescanned, int32_t *verdict, bc_file_system_error *error) {
    const bc_file_system_call_outcome removed =
        bc_file_system_call_unlinkat(engine, operation, parent, (const char *)leaf, AT_REMOVEDIR);
    if (removed.succeeded) {
        *verdict = tree_removed;
        return 0;
    }
    const int32_t kind = kind_of_directory_removal_failure(removed.code);
    if (!removed.stopped && kind == bc_file_system_error_kind_directory_not_empty && !*top_rescanned) {
        *top_rescanned = true;
        *verdict = pass_ends;
        return 0;
    }
    return report_outcome(error, operation, bc_file_system_step_unlinkat, &removed, kind);
}

/* An entry of the directory the walk stands in: what it is comes from a stat on the held directory, never from
 * the advisory d_type, since the answer decides whether it is entered or unlinked. A directory is entered; an
 * entry that is nothing else (a file, a symlink, a device) is unlinked where it is, never followed; one that
 * vanished since the stream answered it is nothing to do. */
static int32_t remove_entry(bc_file_system_engine *engine, int32_t operation, removal_window *window, DIR *top,
                            const uint8_t *name, uint64_t tree_device, bc_file_system_error *error) {
    const int directory = dirfd(current_stream(window, top));
    bc_file_system_expected_entry expected;
    int32_t kind = bc_file_system_resolve_expect_entry(engine, operation, directory, (const char *)name,
                                                       bc_file_system_final_symlink_refuse, &expected, error);
    if (kind == bc_file_system_error_kind_not_found && error->system_code == ENOENT) {
        return 0;
    }
    if (kind != 0) {
        name_entry_in_error(error, window, name);
        return kind;
    }
    if (expected.is_directory) {
        return descend_into(engine, operation, window, top, name, &expected, tree_device, error);
    }
    const bc_file_system_call_outcome removed =
        bc_file_system_call_unlinkat(engine, operation, directory, (const char *)name, 0);
    if (removed.succeeded) {
        return 0;
    }
    if (!removed.stopped && removed.code == ENOENT) {
        return 0;
    }
    kind = report_outcome(error, operation, bc_file_system_step_unlinkat, &removed,
                          bc_file_system_call_kind_of_errno(removed.code));
    name_entry_in_error(error, window, name);
    return kind;
}

/* One step of a pass: the next entry of the directory the walk stands in is dealt with, or, when there is none,
 * the directory itself is removed through its parent and the walk climbs. */
static int32_t advance_removal(bc_file_system_engine *engine, int32_t operation, removal_window *window, DIR *top,
                               int parent, const uint8_t *leaf, uint64_t tree_device, bool *top_rescanned,
                               int32_t *verdict, bc_file_system_error *error) {
    *verdict = pass_continues;
    uint8_t name[NAME_MAX + 1];
    size_t name_length = 0;
    unsigned char d_type = DT_UNKNOWN;
    bool found = false;
    const int32_t kind =
        next_entry_name(engine, operation, current_stream(window, top), name, &name_length, &d_type, &found, error);
    if (kind != 0) {
        name_entry_in_error(error, window, (const uint8_t *)"");
        return kind;
    }
    if (found) {
        return remove_entry(engine, operation, window, top, name, tree_device, error);
    }
    if (window->depth == 0) {
        return remove_top(engine, operation, parent, leaf, top_rescanned, verdict, error);
    }
    bool parent_lost = false;
    const int32_t removed = remove_deepest_level(engine, operation, window, top, &parent_lost, error);
    if (parent_lost) {
        *verdict = pass_ends;
    }
    return removed;
}

/* Empties the directory `top` (whose descriptor serves the stream) and removes it from `parent` under `leaf`,
 * by unlinkat on held descriptors, with no recursion and never more than the window open. Each pass descends
 * from the top: an entry that is not a directory is unlinked where it is found (a symlink is removed, never
 * followed); a directory is entered; a directory read to its end is removed through its parent and the walk
 * climbs, until the parent is a level the window let go, which ends the pass. The top, read to its end, is
 * removed through the location's parent, and that is the end. */
static int32_t remove_directory_tree(bc_file_system_engine *engine, int32_t operation, int parent, const uint8_t *leaf,
                                     DIR *top, uint64_t tree_device, bc_file_system_error *error) {
    [[gnu::cleanup(close_removal_window)]] removal_window window = {};
    bool top_rescanned = false;
    for (;;) {
        close_removal_window(&window);
        rewinddir(top);
        int32_t verdict = pass_continues;
        while (verdict == pass_continues) {
            const int32_t kind = advance_removal(engine, operation, &window, top, parent, leaf, tree_device,
                                                 &top_rescanned, &verdict, error);
            if (kind != 0) {
                return kind;
            }
        }
        if (verdict == tree_removed) {
            return 0;
        }
    }
}

/* Whether an `at` path spells the root of the file system once `.` and `..` are folded: `/`, `/..`, `/tmp/..`
 * and `/a/../../..` all do, and a normalised absolute path is the root exactly when it is one byte long. The
 * path module is asked for the length alone, so no buffer is needed. */
static bool spells_the_root(const bc_file_system_location *location) {
    if (location->kind != bc_file_system_location_kind_at) {
        return false;
    }
    size_t normalized_length = 0;
    bc_file_system_error own_error;
    const int32_t answer = bc_file_system_path_lexically_normalize(location->path, location->length, nullptr, 0,
                                                                   &normalized_length, &own_error);
    if (answer != bc_file_system_error_kind_buffer_too_small) {
        return false;
    }
    return normalized_length == 1;
}

/* Whether the directory `expected` describes is the root of the file system, by identity: the root is opened
 * (the one open by path in the core) and described, and the two identities compared. */
static int32_t is_the_root_by_identity(bc_file_system_engine *engine, int32_t operation,
                                       const bc_file_system_expected_entry *expected, bool *is_root,
                                       bc_file_system_error *error) {
    const bc_file_system_location root_location = bc_file_system_location_at((const uint8_t *)"/", 1);
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved root = bc_file_system_resolved_empty();
    const int32_t kind = bc_file_system_resolve(engine, operation, &root_location, &root, error);
    if (kind != 0) {
        return kind;
    }
    struct stat info;
    const bc_file_system_call_outcome described = bc_file_system_call_fstat(engine, operation, root.parent, &info);
    if (!described.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstat, &described,
                              bc_file_system_call_kind_of_errno(described.code));
    }
    if (expected->device != (uint64_t)info.st_dev) {
        *is_root = false;
        return 0;
    }
    *is_root = expected->inode == (uint64_t)info.st_ino;
    return 0;
}

int32_t bc_file_system_remove_tree(bc_file_system_engine *engine, const bc_file_system_location *location,
                                   int32_t *existed, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_remove_tree;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || existed == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* The root of the file system is refused by spelling before any system call, and by identity below before
     * any removal: the one argument this operation will not take. */
    if (spells_the_root(location)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    const int32_t resolution = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (resolution != 0) {
        if (means_absence(resolution)) {
            *existed = 0;
            return succeed(error);
        }
        return resolution;
    }
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* What the leaf is, never followed: nothing is absence; anything but a directory is unlinked as it is, a
     * symlink included; a directory is opened read-only, bracketed, and emptied. */
    bc_file_system_expected_entry expected;
    int32_t kind = bc_file_system_resolve_expect_entry(engine, operation, resolved.parent, (const char *)resolved.leaf,
                                                       bc_file_system_final_symlink_refuse, &expected, error);
    if (kind == bc_file_system_error_kind_not_found && error->system_code == ENOENT) {
        *existed = 0;
        return succeed(error);
    }
    if (kind != 0) {
        return kind;
    }
    if (!expected.is_directory) {
        const bc_file_system_call_outcome removed =
            bc_file_system_call_unlinkat(engine, operation, resolved.parent, (const char *)resolved.leaf, 0);
        if (!removed.succeeded) {
            if (!removed.stopped && removed.code == ENOENT) {
                *existed = 0;
                return succeed(error);
            }
            return report_outcome(error, operation, bc_file_system_step_unlinkat, &removed,
                                  bc_file_system_call_kind_of_errno(removed.code));
        }
        *existed = 1;
        return succeed(error);
    }
    bool is_root = false;
    kind = is_the_root_by_identity(engine, operation, &expected, &is_root, error);
    if (kind != 0) {
        return kind;
    }
    if (is_root) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    kind = bc_file_system_resolve_open_expected_directory(
        engine, operation, resolved.parent, (const char *)resolved.leaf, bc_file_system_final_symlink_refuse,
        bc_file_system_directory_access_read, &expected, &opened, error);
    if (kind != 0) {
        return kind;
    }
    [[gnu::cleanup(close_owned_directory_stream)]] owned_directory_stream top = {.value = nullptr};
    const bc_file_system_call_outcome streamed =
        bc_file_system_call_fdopendir(engine, operation, opened.value, &top.value);
    if (!streamed.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fdopendir, &streamed,
                              bc_file_system_call_kind_of_errno(streamed.code));
    }
    (void)release_descriptor(&opened);
    kind = remove_directory_tree(engine, operation, resolved.parent, resolved.leaf, top.value, expected.device, error);
    if (kind != 0) {
        return kind;
    }
    *existed = 1;
    return succeed(error);
}

/* ---- create_temporary_directory ------------------------------------------------------------------------------ */

/* The name of a temporary directory, NUL-terminated into `name`: the prefix, the process id, a dash and the
 * token, which is what makes it unique. Answers its length. */
static size_t spell_temporary_name(uint8_t *name, uint64_t process_id, uint64_t token) {
    size_t position = 0;
    memcpy(name, temporary_directory_prefix, sizeof temporary_directory_prefix - 1);
    position += sizeof temporary_directory_prefix - 1;
    position += spell_decimal(name + position, process_id);
    name[position] = '-';
    position += 1;
    position += spell_decimal(name + position, token);
    name[position] = 0;
    return position;
}

int32_t bc_file_system_create_temporary_directory(bc_file_system_engine *engine, const bc_file_system_location *parent,
                                                  const bc_file_system_permissions *permissions, uint8_t *buffer,
                                                  size_t capacity, size_t *length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_create_temporary_directory;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(parent) || !is_valid_permissions(permissions) ||
        !is_answer_buffer(buffer, capacity, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, parent, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The directory the name goes into is opened for search, bracketed; a final symlink is the system's to
     * resolve under `at` and refused under `beneath`, as for any component. The root is the descriptor the walk
     * opened. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    int directory = resolved.parent;
    if (resolved.leaf_length > 0) {
        bc_file_system_expected_entry expected;
        kind = open_resolved_directory(engine, operation, &resolved, symlink_choice_of(parent),
                                       bc_file_system_directory_access_search, &expected, &opened, error);
        if (kind != 0) {
            return kind;
        }
        directory = opened.value;
    }
    /* A name that is taken (a leftover of a process that died with the same id) is tried again with the next
     * token, a bounded number of times; a name the caller's buffer cannot hold is answered before anything is
     * made, so the size query creates nothing. */
    uint8_t name[NAME_MAX + 1] = {};
    size_t name_length = 0;
    bool made = false;
    for (uint32_t attempt = 0; attempt < staging_attempts && !made; ++attempt) {
        const uint64_t token =
            atomic_fetch_add_explicit(&engine->staging_counter, (uint64_t)1, memory_order_relaxed) + 1;
        name_length = spell_temporary_name(name, engine->process_id, token);
        if (name_length > capacity) {
            *length = name_length;
            return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
        }
        const bc_file_system_call_outcome outcome =
            bc_file_system_call_mkdirat(engine, operation, directory, (const char *)name, directory_create_mode);
        if (outcome.succeeded) {
            made = true;
        } else if (outcome.stopped || outcome.code != EEXIST) {
            return report_outcome(error, operation, bc_file_system_step_mkdirat, &outcome,
                                  bc_file_system_call_kind_of_errno(outcome.code));
        }
    }
    if (!made) {
        /* Every name tried was taken: the system's refusal, not the parent's existence. */
        const bc_file_system_call_outcome taken = {.succeeded = false, .code = EEXIST, .stopped = false};
        return report_outcome(error, operation, bc_file_system_step_mkdirat, &taken,
                              bc_file_system_error_kind_io_failed);
    }
    [[gnu::cleanup(discard_created_directory)]] created_directory created = created_directory_in(directory, name);
    created.created = true;
    int32_t failed_step = 0;
    const bc_file_system_call_outcome mode_set =
        set_mode_never_following(engine, operation, directory, (const char *)name, permissions->mode, &failed_step);
    if (!mode_set.succeeded) {
        return report_created_outcome(&created, error, operation, failed_step, &mode_set,
                                      bc_file_system_call_kind_of_errno(mode_set.code));
    }
    created.kept = true;
    memcpy(buffer, name, name_length);
    *length = name_length;
    return succeed(error);
}

/* ---- set_permissions ----------------------------------------------------------------------------------------- */

int32_t bc_file_system_set_permissions(bc_file_system_engine *engine, const bc_file_system_location *location,
                                       uint32_t mode, int32_t final_symlink, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_set_permissions;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || mode > permission_bits ||
        !is_in_enum(final_symlink, final_symlink_last)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    const int32_t kind = bc_file_system_resolve(engine, operation, location, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root has no leaf: it is `.` beneath the descriptor the walk opened. */
    const char *name = resolved.leaf_length == 0 ? "." : (const char *)resolved.leaf;
    if (final_symlink == bc_file_system_final_symlink_follow) {
        const bc_file_system_call_outcome changed =
            bc_file_system_call_fchmodat(engine, operation, resolved.parent, name, mode, 0);
        if (!changed.succeeded) {
            return report_outcome(error, operation, bc_file_system_step_fchmodat, &changed,
                                  bc_file_system_call_kind_of_errno(changed.code));
        }
        return succeed(error);
    }
    /* Under `refuse` a symlink is refused rather than given a mode of its own (which Apple platforms would
     * allow and Linux refuses): the entry is looked at first, without following. The look does not decide
     * where the change lands, since the change itself never follows a symlink either; it only decides the
     * answer for one. */
    struct stat info;
    const bc_file_system_call_outcome looked =
        bc_file_system_call_fstatat(engine, operation, resolved.parent, name, AT_SYMLINK_NOFOLLOW, &info);
    if (!looked.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstatat, &looked,
                              bc_file_system_call_kind_of_errno(looked.code));
    }
    if (S_ISLNK(info.st_mode)) {
        return refuse(error, bc_file_system_error_kind_symlink_refused, operation);
    }
    int32_t failed_step = 0;
    const bc_file_system_call_outcome changed =
        set_mode_never_following(engine, operation, resolved.parent, name, mode, &failed_step);
    if (!changed.succeeded) {
        return report_outcome(error, operation, failed_step, &changed, bc_file_system_call_kind_of_errno(changed.code));
    }
    return succeed(error);
}

/* ---- Operations not built yet ------------------------------------------------------------------------------- */

/* Every operation below checks its contract, announces itself to the observer (the shape every real operation
 * keeps: validate, `before_operation`, then every system call behind `before_step`) and then answers
 * `unsupported` with its operation named: the honest stub, replaced group by group by the stage each comment
 * names. */

int32_t bc_file_system_rename(bc_file_system_engine *engine, const bc_file_system_location *source,
                              const bc_file_system_location *destination, int32_t existing_destination,
                              int32_t durability, int32_t *durability_achieved, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_rename;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(source) || !is_valid_location(destination) ||
        !is_in_enum(existing_destination, existing_destination_last) || !is_in_enum(durability, durability_last) ||
        durability_achieved == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 16 moves-and-copies-in-c */
}

int32_t bc_file_system_copy_file(bc_file_system_engine *engine, const bc_file_system_location *source,
                                 const bc_file_system_location *destination,
                                 const bc_file_system_write_options *options, int32_t *durability_achieved,
                                 bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_copy_file;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(source) || !is_valid_location(destination) ||
        !is_valid_write_options(options) || durability_achieved == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 16 moves-and-copies-in-c */
}

int32_t bc_file_system_create_symlink(bc_file_system_engine *engine, const uint8_t *target, size_t target_length,
                                      const bc_file_system_location *location, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_create_symlink;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_byte_span(target, target_length) || !is_valid_location(location)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 17 locks-links-condemn */
}

int32_t bc_file_system_read_symlink(bc_file_system_engine *engine, const bc_file_system_location *location,
                                    uint8_t *buffer, size_t capacity, size_t *length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_read_symlink;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_answer_buffer(buffer, capacity, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 17 locks-links-condemn */
}

int32_t bc_file_system_try_lock(bc_file_system_engine *engine, const bc_file_system_location *location,
                                bc_file_system_lock **lock, int32_t *outcome, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_try_lock;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || lock == nullptr || outcome == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 17 locks-links-condemn */
}

int32_t bc_file_system_condemn(bc_file_system_engine *engine, const bc_file_system_location *location, uint8_t *buffer,
                               size_t capacity, size_t *length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_condemn;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_answer_buffer(buffer, capacity, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 17 locks-links-condemn */
}

int32_t bc_file_system_remove_condemned(bc_file_system_engine *engine, const bc_file_system_location *parent,
                                        bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_remove_condemned;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(parent)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 17 locks-links-condemn */
}
