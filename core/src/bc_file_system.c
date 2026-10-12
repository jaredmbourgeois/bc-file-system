/* bc_file_system.c — the engine behind the public header.
 *
 * This translation unit is the hosted tier of the core: it owns the engine, the directory and lock handles,
 * the test observer's slot and every operation. It is C23 in the GNU dialect; the public header it implements
 * is the common subset, and the platform differences are behind platform.h.
 *
 * Where the library stands: the handles and the observer's bookkeeping are real, and every operation on the
 * file system is an honest stub. Each one validates its arguments exactly as the header promises (a contract
 * violation is `invalid_argument` and touches nothing) and then answers `unsupported` with the operation named
 * and the step zero, so every contract case is red for a stated reason until the stage that builds it. Nothing
 * here opens a file yet, so the cleanup idiom (`[[gnu::cleanup]]` on an owned descriptor) is not needed yet.
 */
#include <bc_file_system/bc_file_system.h>

#include <unistd.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "platform.h"

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
    permission_bits = 07777
};

/* The last enumerator of each closed enum, so a range check reads `is_in_enum(value, operation_last)`. Every
 * enum in the header runs from 1 to its last value without a gap. */
enum : int32_t {
    operation_last = bc_file_system_operation_path_library_name,
    step_last = bc_file_system_step_identity_check,
    final_symlink_last = bc_file_system_final_symlink_follow,
    existing_destination_last = bc_file_system_existing_destination_refuse,
    read_limit_kind_last = bc_file_system_read_limit_kind_at_most,
    durability_last = bc_file_system_durability_durable,
    permissions_policy_last = bc_file_system_permissions_policy_preserve_or,
    fault_action_last = bc_file_system_fault_action_stop
};

struct bc_file_system_engine {
    /* Created by the observed factory: the fault slot and the step counters are live. A live engine carries the
     * same fields and never touches them, which is the one indirect decision per step a release build pays. */
    bool observed;
    /* The platform's fast paths, probed once by the live factory (the resolution layer fills these in); every
     * probe reads "absent" until then, and under the portable_only test run. */
    bool has_openat2;
    bool has_rename_no_replace;
    bool has_clone;
    uint32_t fault_count;
    bc_file_system_fault faults[fault_capacity];
    /* How many times each operation has reached each step; row and column 0 are unused because 0 is no value. */
    uint64_t step_counts[operation_last + 1][step_last + 1];
};

struct bc_file_system_directory {
    int descriptor; /* -1 until an open lands */
    size_t opened_as_length;
    uint8_t opened_as[]; /* the text it was opened with, not NUL-terminated; sized by the open */
};

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
    /* calloc zeroes the probes, the fault slot and every counter in one step. */
    bc_file_system_engine *created = calloc(1, sizeof *created);
    if (created == nullptr) {
        return refuse(error, bc_file_system_error_kind_resource_exhausted, operation);
    }
    created->observed = observed;
    *engine = created;
    return succeed(error);
}

int32_t bc_file_system_engine_create_live(bc_file_system_engine **engine, bc_file_system_error *error) {
    /* The platform probes (openat2, the no-replace rename, cloning) run here once the resolution layer lands;
     * until then every fast path reads as absent. */
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

/* ---- Directories and locks ----------------------------------------------------------------------------------- */

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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 13 status-and-reads */
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

/* ---- Operations --------------------------------------------------------------------------------------------- */

/* Every operation below checks its contract and then answers `unsupported` with its operation named: the honest
 * stub, replaced group by group by the stage each comment names. */

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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 13 status-and-reads */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 13 status-and-reads */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 14 writes-in-c */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 14 writes-in-c */
}

int32_t bc_file_system_sync(bc_file_system_engine *engine, const bc_file_system_location *location,
                            int32_t *durability_achieved, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_sync;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || durability_achieved == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 14 writes-in-c */
}

int32_t bc_file_system_create_directory(bc_file_system_engine *engine, const bc_file_system_location *location,
                                        const bc_file_system_permissions *permissions, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_create_directory;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || !is_valid_permissions(permissions)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
}

int32_t bc_file_system_remove_file(bc_file_system_engine *engine, const bc_file_system_location *location,
                                   int32_t *existed, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_remove_file;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_valid_location(location) || existed == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
}

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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 16 moves-and-copies-in-c */
}

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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 15 directories-in-c */
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
    return refuse(error, bc_file_system_error_kind_unsupported, operation); /* stage 17 locks-links-condemn */
}
