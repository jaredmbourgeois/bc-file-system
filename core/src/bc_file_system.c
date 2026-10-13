/* bc_file_system.c — the engine behind the public header.
 *
 * This translation unit is the hosted tier of the core: it owns the engine, the directory and lock handles,
 * the test observer's slot and every operation. It is C23 in the GNU dialect; the public header it implements
 * is the common subset, and the platform differences are behind platform.h.
 *
 * Every operation of the header is real: the handles, the platform probes, the observer and the resolution
 * layer; the operations that read (`bc_file_system_directory_open`, `bc_file_system_status_of`,
 * `bc_file_system_read_file`), the ones that write (`bc_file_system_write_file`, staged and renamed into
 * place; `bc_file_system_append`; `bc_file_system_sync`), the ones on directories (`create_directory`,
 * `create_directories`, `list_directory`, `remove_file`, `remove_directory`, `remove_tree` on held descriptors
 * with a bounded window, `create_temporary_directory`, `set_permissions`), the ones that move and copy
 * (`bc_file_system_rename` in one step, `bc_file_system_copy_file` staged like a write, by the platform's clone
 * or in-kernel copy where it has one and the read-and-write loop otherwise), and the locks, links and the
 * condemned sweep (`bc_file_system_try_lock` over an open of its own, `create_symlink` and `read_symlink`,
 * `condemn` as one no-replace rename beside the entry and `remove_condemned` as the tree removal over every
 * entry carrying one of the engine's own marks, which names.h holds). Each one validates its arguments exactly
 * as the header promises (a contract violation is `invalid_argument` and touches nothing) and announces itself
 * to the observer, then resolves its location once through resolve.h to (parent descriptor, leaf name) and
 * makes only `*at` calls on that pair from then on; under `at` an operation that acts on its location in one
 * call (or in calls that each act on that one path) takes the pair (AT_FDCWD, the whole text) instead, so that
 * call is the system's whole resolution, and an operation whose steps must act in one directory as a unit (a
 * staged write or copy, a chain of directories, a tree removal, a durable append or rename) holds the parent.
 * Each call goes through calls.h (behind the observer's `before_step`,
 * asked again on EINTR, the errno copied on the line after the call); every descriptor it holds is an
 * owned_descriptor under the cleanup idiom of owned.h, a directory it reads is an owned_directory_stream, and
 * a file or directory it created and still owns is a staged_file or a created_directory under the same idiom,
 * unlinked on a failing path and kept on a stop.
 */
#include <bc_file_system/bc_file_system.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
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
#include "names.h"
#include "owned.h"
#include "path.h"
#include "platform.h"
#include "resolve.h"

/* ---- Layout pins -------------------------------------------------------------------------------------------- */

/* Every struct that crosses the ABI is pinned by its size and the offset of its last field, as literals, so a
 * face (C++ static_assert, a Swift MemoryLayout test, a Rust const assertion) repeats the same numbers and a
 * field added here fails every face's build. A location holds two pointers, so its numbers depend on the
 * pointer width: both widths the library ships for are pinned. */
static_assert(sizeof(bc_file_system_error) == 284, "bc_file_system_error layout changed; bump the faces");
static_assert(offsetof(bc_file_system_error, name) == 21, "bc_file_system_error layout changed; bump the faces");
static_assert(offsetof(bc_file_system_error, failed_location) == 280,
              "bc_file_system_error layout changed; bump the faces");
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
    /* The scratch a copy that neither clones nor copies in the kernel moves the bytes through: one bounded
     * allocation of 1 MiB per copy, so each piece is at most this much, under the largest single transfer, and
     * the stack carries none of it (Emscripten's is 64 KiB). The size is a trade between system calls per file
     * and memory per copy; nothing observable depends on it. */
    copy_piece_capacity = UINT32_C(1) << 20,
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

/* The staging infix and the condemned prefix are names.h's: the sweep reads them from the same table. */

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
    /* How a staged file the platform's clone created is opened to be given its mode, synced and closed like one
     * the engine created itself: for writing, never through a symlink. */
    staged_reopen_flags = O_WRONLY | O_CLOEXEC | O_NOFOLLOW,
    /* How an append creates a file, and how it opens one that exists: at the end, never through a symlink (a
     * link answers ELOOP), and never blocking on the open, which a FIFO with no reader would do forever. */
    append_create_flags = O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
    append_open_flags = O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
    /* How a file or directory is opened to be synced: a directory opens read-only, a symlink is refused. */
    sync_open_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,
    /* How a directory the walk holds for search is opened again to be synced: read-only, as `.` beneath it,
     * because a search-only descriptor (O_PATH on Linux) cannot be synced. */
    directory_read_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
    /* How the entry a lock is taken on is opened: read-only (a lock reads nothing and writes nothing through it),
     * never through a symlink, never inherited by a child's exec, and never blocking on the open itself. */
    lock_open_flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK
};

/* The last enumerator of each closed enum, so a range check reads `is_in_enum(value, operation_last)`. Every
 * enum in the header runs from 1 to its last value without a gap. */
enum : int32_t {
    operation_last = bc_file_system_operation_engine_create_portable_only_for_tests,
    step_last = bc_file_system_step_faccessat,
    final_symlink_last = bc_file_system_final_symlink_follow,
    existing_destination_last = bc_file_system_existing_destination_refuse,
    read_limit_kind_last = bc_file_system_read_limit_kind_at_most,
    durability_last = bc_file_system_durability_durable,
    permissions_policy_last = bc_file_system_permissions_policy_preserve_or,
    fault_action_last = bc_file_system_fault_action_stop
};

struct bc_file_system_engine {
    /* Created by a factory for tests (the observed one, or the portable-only one): the fault slot and the step
     * counters are live. A live engine carries the same fields and never touches them, which is the one decision
     * per step a release build pays. */
    bool observed;
    /* The fast paths the operations may take, read by the ones that have one (the resolution layer, writes, the
     * sync, moves, copies and condemn): what the platform's probe found, handed in once by the live and the
     * observed factory, or none at all from the portable-only factory, so that every operation takes the path
     * every platform has. */
    bc_file_system_platform_fast_paths fast_paths;
    /* What makes a staged file's name unique: the id of the process the engine was created in, read once by the
     * factory, and a token drawn from this counter for every staged file (never the clock, never a random
     * source). The counter is the one field a live engine writes after creation; it is atomic so two threads
     * staging at once draw different tokens, and nothing orders on it, so the relaxed order is enough. */
    uint64_t process_id;
    _Atomic uint64_t staging_counter;
    /* The identity of the root of the file system, read once by the factory, against which `remove_tree`
     * refuses the root by identity: a comparison, so the refusal costs an operation no call of its own. The root
     * is the process's root at the engine's creation. */
    uint64_t root_device;
    uint64_t root_inode;
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

/* Whether a factory installs the observer: a two-case switch, so each factory's call says which engine it
 * makes. */
typedef enum observer_choice : int32_t { observer_absent = 1, observer_installed = 2 } observer_choice;

/* No fast path at all: what the portable-only engine is created with, so that every operation takes the path
 * every platform has. Spelled field by field, as each platform's probe spells its answer. */
static bc_file_system_platform_fast_paths no_fast_paths(void) {
    return (bc_file_system_platform_fast_paths){
        .has_openat2 = false,
        .has_openat_nofollow_any = false,
        .has_rename_no_replace = false,
        .has_clone = false,
        .has_copy_range = false,
        .has_full_fsync = false,
    };
}

/* The identity of `/`, read once by the factory. Creation is no operation, so the read is not gated: no observer
 * counts it or scripts a fault into it, and it holds no descriptor (an absolute path makes fstatat ignore its
 * directory argument), so no descriptor count and no kill point of any operation sees it. Answers 0, or the
 * errno the system answered. */
static int read_root_identity(uint64_t *device, uint64_t *inode) {
    struct stat info;
    int code = 0;
    do {
        code = 0;
        if (fstatat(AT_FDCWD, "/", &info, 0) != 0) { /* reach-ok: the factory is the one live site of the engine */
            code = errno;
        }
    } while (code == EINTR);
    if (code != 0) {
        return code;
    }
    /* The identity fields are compared as bit patterns, so the width and sign of dev_t do not matter. */
    *device = (uint64_t)info.st_dev;
    *inode = (uint64_t)info.st_ino;
    return 0;
}

/* The one body of the three factories. The fast paths are a parameter, never something the engine looks up:
 * the live and the observed factory hand in what the platform's probe found, the portable-only factory hands
 * in none. */
static int32_t create_engine(observer_choice observer, bc_file_system_platform_fast_paths fast_paths, int32_t operation,
                             bc_file_system_engine **engine, bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    uint64_t root_device = 0;
    uint64_t root_inode = 0;
    const int root_code = read_root_identity(&root_device, &root_inode);
    if (root_code != 0) {
        *error = (bc_file_system_error){.kind = bc_file_system_call_kind_of_errno(root_code),
                                        .operation = operation,
                                        .step = bc_file_system_step_fstatat,
                                        .system_code = root_code};
        return error->kind;
    }
    /* calloc zeroes the fault slot and every counter in one step. */
    bc_file_system_engine *created = calloc(1, sizeof *created);
    if (created == nullptr) {
        return refuse(error, bc_file_system_error_kind_resource_exhausted, operation);
    }
    created->observed = observer == observer_installed;
    created->fast_paths = fast_paths;
    /* The process id is a fact of the process, read here, in the factory, and carried by the engine: no
     * operation asks for it. A pid is never negative, so the cast only changes the type. */
    created->process_id = (uint64_t)getpid(); /* reach-ok: the factory is the one live site of the engine */
    atomic_init(&created->staging_counter, 0);
    created->root_device = root_device;
    created->root_inode = root_inode;
    *engine = created;
    return succeed(error);
}

/* The live and the observed factory probe the platform once each, here, so no operation asks the kernel twice,
 * and the observed engine runs over the same fast paths a composition root gets. */
int32_t bc_file_system_engine_create_live(bc_file_system_engine **engine, bc_file_system_error *error) {
    return create_engine(observer_absent, bc_file_system_platform_probe_fast_paths(),
                         bc_file_system_operation_engine_create_live, engine, error);
}

int32_t bc_file_system_engine_create_observed_for_tests(bc_file_system_engine **engine, bc_file_system_error *error) {
    return create_engine(observer_installed, bc_file_system_platform_probe_fast_paths(),
                         bc_file_system_operation_engine_create_observed_for_tests, engine, error);
}

int32_t bc_file_system_engine_create_portable_only_for_tests(bc_file_system_engine **engine,
                                                             bc_file_system_error *error) {
    return create_engine(observer_installed, no_fast_paths(),
                         bc_file_system_operation_engine_create_portable_only_for_tests, engine, error);
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

/* ---- The retry in the held parent's shape --------------------------------------------------------------------- */

/* Whether an attempt in the one-call shape met ENAMETOOLONG on an `at` location's whole text, which the held
 * parent's shape may still resolve: Apple platforms bound a symlink's text joined to the rest of a path by
 * PATH_MAX, so a text that fits one call may be refused all the same, and the resolution of its parent text takes
 * such a piece one component at a time (stage 27). The operation is then made once more in the held parent's
 * shape, so ENAMETOOLONG stays `invalid_argument` only for a single component the system refuses. The refused
 * attempt changed nothing: the call that answered is the first the operation makes on that text, or a rename's
 * one move. */
static bool needs_the_held_parent(const bc_file_system_location *location, int32_t shape, int32_t kind,
                                  const bc_file_system_error *error) {
    if (kind != bc_file_system_error_kind_invalid_argument) {
        return false;
    }
    if (error->system_code != ENAMETOOLONG) {
        return false;
    }
    return bc_file_system_resolve_takes_whole_text(location, shape);
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

/* Opens the directory a resolved location names, for `access`: `leaf_name` (the leaf, or an `at` location's whole
 * text beside AT_FDCWD) under the caller's choice about a final symlink, or, for the root, `.` beneath the
 * descriptor the resolution opened (which is open for search, and a listing needs more). Under `beneath` the open
 * is bracketed by the identity check like every component before it: that is the containment the header
 * promises there. Under `at` the header promises the system's resolution, so nothing is checked around the open:
 * a directory swapped in for the leaf is one the system's own open of the path would open too, O_NOFOLLOW alone
 * refuses a final symlink under `refuse`, and the open's errno is the answer (resolve.h,
 * `bc_file_system_resolve_open_directory`). */
static int32_t open_resolved_directory(bc_file_system_engine *engine, int32_t operation, int32_t location_kind,
                                       const bc_file_system_resolved *resolved, const uint8_t *leaf_name,
                                       int32_t final_symlink, int32_t access, owned_descriptor *opened,
                                       bc_file_system_error *error) {
    const char *name = resolved->leaf_length == 0 ? "." : (const char *)leaf_name;
    if (location_kind == bc_file_system_location_kind_at) {
        return bc_file_system_resolve_open_directory(engine, operation, resolved->parent, name, final_symlink, access,
                                                     opened, error);
    }
    bc_file_system_expected_entry expected;
    const int32_t kind =
        bc_file_system_resolve_expect_entry(engine, operation, resolved->parent, name, final_symlink, &expected, error);
    if (kind != 0) {
        return kind;
    }
    return bc_file_system_resolve_open_expected_directory(engine, operation, resolved->parent, name, final_symlink,
                                                          access, &expected, opened, error);
}

/* The body of `bc_file_system_directory_open`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t open_directory_in_shape(bc_file_system_engine *engine, int32_t operation,
                                       const bc_file_system_location *location, int32_t shape, int32_t final_symlink,
                                       bc_file_system_directory **directory, bc_file_system_error *error) {
    /* One call opens the directory: under `at` the whole text by path, nothing opened before it. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The leaf is opened as a directory, for search, with the caller's choice about a final symlink: bracketed by
     * the identity check under `beneath`, like every component before it, and with nothing around the open under
     * `at` (`open_resolved_directory`), the whole text by path. The root has no leaf: the parent the resolution
     * opened is the directory itself, and it is taken over rather than opened again. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    if (resolved.leaf_length == 0) {
        opened.value = release_descriptor(&resolved.opened);
    } else {
        kind = open_resolved_directory(engine, operation, location->kind, &resolved, name.text, final_symlink,
                                       bc_file_system_directory_access_search, &opened, error);
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

    const int32_t kind = open_directory_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call,
                                                 final_symlink, directory, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return open_directory_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent,
                                   final_symlink, directory, error);
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

/* The body of `bc_file_system_status_of`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t status_of_in_shape(bc_file_system_engine *engine, int32_t operation,
                                  const bc_file_system_location *location, int32_t shape, int32_t final_symlink,
                                  int32_t *present, bc_file_system_status *status, bc_file_system_error *error) {
    /* One call states the entry: under `at` the whole text by path, so this one `fstatat` is the system's whole
     * resolution, as `lstat` and `stat` are, and an absence on the way (ENOENT, ENOTDIR) is its answer below. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    const int32_t resolution =
        bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
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
            bc_file_system_call_fstatat(engine, operation, resolved.parent, (const char *)name.text, flags, &info);
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

    const int32_t kind = status_of_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call,
                                            final_symlink, present, status, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return status_of_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, final_symlink,
                              present, status, error);
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

/* The body of `bc_file_system_read_file`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t read_file_in_shape(bc_file_system_engine *engine, int32_t operation,
                                  const bc_file_system_location *location, int32_t shape,
                                  const bc_file_system_read_options *options, uint8_t *buffer, size_t capacity,
                                  size_t *length, bc_file_system_error *error) {
    /* One call opens the file, under `at` the whole text by path; every later call is on the descriptor. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
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
        const bc_file_system_call_outcome opened =
            bc_file_system_call_openat(engine, operation, resolved.parent, (const char *)name.text, flags, &file.value);
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

    const int32_t kind = read_file_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call,
                                            options, buffer, capacity, length, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return read_file_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, options, buffer,
                              capacity, length, error);
}

/* ---- Writes ------------------------------------------------------------------------------------------------- */

/* A file this operation created in `parent` under `name` and still owns: a staged write's temporary until it
 * is renamed into place, an append's new file until its mode is set. Declared under
 * `[[gnu::cleanup(discard_staged_file)]]`, so a return on any failing path closes it and unlinks it. `kept` is
 * set where the file becomes the caller's (the rename happened; the mode was set) and where the observer
 * stopped the operation, so that the disk is left as a kill would leave it: the file stays, nothing is
 * unlinked. */
typedef struct staged_file {
    int parent; /* borrowed: the directory the file was created in, or AT_FDCWD beside an absolute name */
    owned_descriptor descriptor;
    /* NUL-terminated, in storage the operation owns and declares before the file, so it outlives the cleanup: a
     * staged name the operation spells into NAME_MAX + 1 bytes, or an append's leaf or whole `at` text. */
    uint8_t *name;
    bool created;
    bool kept;
} staged_file;

static staged_file staged_file_in(int parent, uint8_t *name) {
    return (staged_file){.parent = parent, .descriptor = {.value = -1}, .name = name, .created = false, .kept = false};
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

/* What a staged write found at its destination before staging anything: nothing, or a regular file with its
 * mode and its identity (which a copy holds against its source's). */
typedef struct destination_entry {
    bool present;
    uint32_t mode;
    uint64_t device; /* st_dev, as a bit pattern */
    uint64_t inode;  /* st_ino */
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
 * frame and its caller's hold one PATH_MAX buffer each, and the `at` resolution they end in holds a third, the
 * one place in the core where more than one is live at once (three times PATH_MAX: 12 KiB on Linux): the system
 * bounds a link's text by PATH_MAX and the text followed is held in one such buffer, so a text that does not fit
 * is refused as `invalid_argument`, the kind of the ENAMETOOLONG the system would answer. */
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
                *existing = (destination_entry){.present = false, .mode = 0, .device = 0, .inode = 0};
                return 0;
            }
            return report_outcome(error, operation, bc_file_system_step_fstatat, &outcome,
                                  bc_file_system_call_kind_of_errno(outcome.code));
        }
        if (S_ISREG(info.st_mode)) {
            *existing = (destination_entry){.present = true,
                                            .mode = (uint32_t)info.st_mode & permission_bits,
                                            .device = (uint64_t)info.st_dev,
                                            .inode = (uint64_t)info.st_ino};
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
 * atomically; `refuse` with the platform's no-replace rename (an engine without one refused the operation before
 * staging anything), whose EEXIST is `already_exists` with the destination untouched. The no-replace rename's
 * EINVAL is `unsupported` with the code kept: a staged file is a sibling of its destination, never an ancestor,
 * so the one reading left is a volume without RENAME_NOREPLACE, which the probe cannot see and the library
 * refuses rather than replace through. Afterwards the staged name is gone, so the file is kept from the cleanup;
 * a failure leaves it to be discarded. */
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
        int32_t kind = bc_file_system_call_kind_of_errno(outcome.code);
        if (step == bc_file_system_step_rename_no_replace && outcome.code == EINVAL) {
            kind = bc_file_system_error_kind_unsupported;
        }
        return report_staged_outcome(staged, error, operation, step, &outcome, kind);
    }
    staged->kept = true;
    return 0;
}

/* The end of every staged write, once the bytes are in the staged file: its mode set exactly (whatever the
 * umask or a clone left on it), then, when durable, its sync and its close as reported steps, then the rename
 * into place under the caller's choice about an existing destination, then the directory's sync, which lowers the
 * answer and never fails it. `*achieved` starts as the durability asked for and is lowered where a sync is not
 * offered or fails after the change. */
static int32_t place_staged_file(bc_file_system_engine *engine, int32_t operation, staged_file *staged,
                                 const uint8_t *leaf, uint32_t mode, const bc_file_system_write_options *options,
                                 int32_t *achieved, bc_file_system_error *error) {
    const bc_file_system_call_outcome mode_set =
        bc_file_system_call_fchmod(engine, operation, staged->descriptor.value, mode);
    if (!mode_set.succeeded) {
        return report_staged_outcome(staged, error, operation, bc_file_system_step_fchmod, &mode_set,
                                     bc_file_system_call_kind_of_errno(mode_set.code));
    }
    int32_t kind = 0;
    if (*achieved == bc_file_system_durability_durable) {
        kind = settle_staged_file(engine, operation, staged, achieved, error);
        if (kind != 0) {
            return kind;
        }
    }
    kind = rename_into_place(engine, operation, staged, leaf, options->existing_destination, error);
    if (kind != 0) {
        return kind;
    }
    /* The directory's sync makes the new name itself durable; the staged file's parent is the directory. */
    if (*achieved == bc_file_system_durability_durable) {
        return sync_directory_after_change(engine, operation, staged->parent, achieved, error);
    }
    return 0;
}

/* Whether an operation told to refuse an existing destination cannot keep that promise on this engine. The
 * promise is kept by the platform's no-replace rename, the one step that takes a name only while nothing holds
 * it; an engine without one (a platform that offers none, or the portable-only engine, refused every fast path)
 * has nothing else that keeps it: a plain rename would replace what the caller said to keep, and a look before
 * it would leave open the window the one step exists to close. Such an operation answers `unsupported` before
 * any system call rather than weaken the promise, and so does every condemn, which never replaces. */
static bool cannot_refuse_to_replace(const bc_file_system_engine *engine, int32_t existing_destination) {
    if (engine->fast_paths.has_rename_no_replace) {
        return false;
    }
    return existing_destination == bc_file_system_existing_destination_refuse;
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
    if (cannot_refuse_to_replace(engine, options->existing_destination)) {
        return refuse(error, bc_file_system_error_kind_unsupported, operation);
    }

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
    uint8_t staged_name[NAME_MAX + 1] = {};
    [[gnu::cleanup(discard_staged_file)]] staged_file staged = staged_file_in(resolved.parent, staged_name);
    kind = create_staged_file(engine, operation, &staged, resolved.leaf, resolved.leaf_length, error);
    if (kind != 0) {
        return kind;
    }
    kind = write_every_byte(engine, operation, &staged, bytes, byte_count, error);
    if (kind != 0) {
        return kind;
    }
    int32_t achieved = options->durability;
    kind = place_staged_file(engine, operation, &staged, resolved.leaf, mode, options, &achieved, error);
    if (kind != 0) {
        return kind;
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

/* The body of `bc_file_system_append`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t append_in_shape(bc_file_system_engine *engine, int32_t operation,
                               const bc_file_system_location *location, int32_t shape, const uint8_t *bytes,
                               size_t byte_count, int32_t durability, const bc_file_system_permissions *permissions,
                               int32_t *durability_achieved, bc_file_system_error *error) {
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root has no leaf and is a directory. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    /* The file is this operation's own until its mode is set: a new file whose mode cannot be set is taken
     * back, so a failure never leaves a file with a mode nobody chose. */
    [[gnu::cleanup(discard_staged_file)]] staged_file file = staged_file_in(resolved.parent, name.text);
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

    /* A durable append syncs the directory a new file appears in, so its steps act in one directory as a unit and
     * it holds that directory; a `written` one acts on the file alone, in one call that opens it (under `at` the
     * whole text by path), every later call on the descriptor. */
    const int32_t shape = durability == bc_file_system_durability_durable ? bc_file_system_resolution_shape_held_parent
                                                                          : bc_file_system_resolution_shape_one_call;
    const int32_t kind = append_in_shape(engine, operation, location, shape, bytes, byte_count, durability, permissions,
                                         durability_achieved, error);
    if (!needs_the_held_parent(location, shape, kind, error)) {
        return kind;
    }
    return append_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, bytes, byte_count,
                           durability, permissions, durability_achieved, error);
}

/* ---- sync ---------------------------------------------------------------------------------------------------- */

/* The body of `bc_file_system_sync`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t sync_in_shape(bc_file_system_engine *engine, int32_t operation, const bc_file_system_location *location,
                             int32_t shape, int32_t *durability_achieved, bc_file_system_error *error) {
    /* One call opens what is synced (under `at` the whole text by path) and the sync is on its descriptor: the
     * entry's directory is never synced here, so nothing needs it held. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    const int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
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
            engine, operation, resolved.parent, (const char *)name.text, sync_open_flags, &target.value);
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

    const int32_t kind = sync_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call,
                                       durability_achieved, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return sync_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, durability_achieved,
                         error);
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
    int parent; /* borrowed: the directory it was created in, or AT_FDCWD beside an absolute name */
    /* NUL-terminated and borrowed from the operation, which declares it before the directory so it outlives the
     * cleanup: a component, or a `create_directory`'s whole `at` text. */
    const uint8_t *name;
    bool created;
    bool kept;
} created_directory;

static created_directory created_directory_in(int parent, const uint8_t *name) {
    return (created_directory){.parent = parent, .name = name, .created = false, .kept = false};
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

/* The body of `bc_file_system_create_directory`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t create_directory_in_shape(bc_file_system_engine *engine, int32_t operation,
                                         const bc_file_system_location *location, int32_t shape,
                                         const bc_file_system_permissions *permissions, bc_file_system_error *error) {
    /* The directory is made and given its mode by name (under `at` the whole text by path both times), so the
     * two calls act on the location itself and nothing is held. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    const int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root is always there. A final symlink has no meaning for a creation: the name must be free, and a
     * symlink holding it is something already there, which mkdirat answers EEXIST for. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_already_exists, operation);
    }
    [[gnu::cleanup(discard_created_directory)]] created_directory created =
        created_directory_in(resolved.parent, name.text);
    const int32_t made = make_directory_with_mode(engine, operation, &created, permissions->mode, error);
    if (made != 0) {
        return made;
    }
    return succeed(error);
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
    bc_file_system_engine_before_operation(engine, operation);

    const int32_t kind = create_directory_in_shape(engine, operation, location,
                                                   bc_file_system_resolution_shape_one_call, permissions, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return create_directory_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent,
                                     permissions, error);
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
 * it is not. `*child` receives the opened directory.
 *
 * A directory this call has just made is opened with nothing checked around the open: no stat before it and no
 * identity check after it. What the header promises here is the location kind's promise, and the open keeps
 * it alone: under `beneath`, that nothing outside the held directory is reached and no symlink on the way is
 * followed, which O_NOFOLLOW on this open keeps (a symlink swapped in answers `symlink_refused`); under `at`,
 * the system's resolution. A directory swapped in for the one just made, between the mkdirat and this open, is
 * opened and the chain goes on inside it, as the system's own `mkdir -p` goes on inside it (which follows a
 * symlink there too). The bracket never closed that window: its stat came after the mkdirat, so it recorded
 * whatever stood there by then, and the identity check proved only that the open found the same. */
static int32_t open_or_create_component(bc_file_system_engine *engine, int32_t operation, int parent,
                                        const uint8_t *name, int32_t final_symlink, uint32_t mode,
                                        owned_descriptor *child, bc_file_system_error *error) {
    bc_file_system_expected_entry expected;
    int32_t kind = bc_file_system_resolve_expect_entry(engine, operation, parent, (const char *)name, final_symlink,
                                                       &expected, error);
    if (kind == bc_file_system_error_kind_not_found) {
        [[gnu::cleanup(discard_created_directory)]] created_directory created = created_directory_in(parent, name);
        kind = make_directory_with_mode(engine, operation, &created, mode, error);
        if (kind == 0) {
            return bc_file_system_resolve_open_directory(engine, operation, parent, (const char *)name, final_symlink,
                                                         bc_file_system_directory_access_search, child, error);
        }
        /* A name taken since the stat (this chain being made twice at once) is one that is there now, and what
         * is there is judged below like anything else; every other failure is the operation's. */
        if (!name_was_taken(kind, error)) {
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

/* Reads the stream once, from its start to its end: each entry is stored into the caller's array as it is read
 * while the array has room, and once it is full the rest are counted without being stored (no name copied, no
 * type asked for). `*seen` is how many were stored, or, above `capacity`, how many the listing needs. */
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

/* The body of `bc_file_system_list_directory`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t list_directory_in_shape(bc_file_system_engine *engine, int32_t operation,
                                       const bc_file_system_location *location, int32_t shape, int32_t final_symlink,
                                       bc_file_system_entry *entries, size_t capacity, size_t *count,
                                       bc_file_system_error *error) {
    /* One call opens the directory (under `at` the whole text by path), and the listing is read through it. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The directory is opened read-only, which a listing needs and the walk's search-only open does not give;
     * the stream then owns the descriptor. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    kind = open_resolved_directory(engine, operation, location->kind, &resolved, name.text, final_symlink,
                                   bc_file_system_directory_access_read, &opened, error);
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
    /* One pass: the entries are stored as they are read while the caller's array has room, and counted once it
     * is full, so a listing that fits is one read of the directory. One that does not fit answers
     * `buffer_too_small` with the count the pass reached, and the caller's next call is a pass of its own: a
     * directory that grew in between answers that call's own count, `buffer_too_small` again, so the face's
     * bounded grow loop is the only retry. A listing is complete or it is an error: what a failing pass stored
     * in the array is never an answer. */
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

    const int32_t kind = list_directory_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call,
                                                 final_symlink, entries, capacity, count, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return list_directory_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent,
                                   final_symlink, entries, capacity, count, error);
}

/* ---- remove_file and remove_directory ----------------------------------------------------------------------- */

/* The body of `bc_file_system_remove_file`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t remove_file_in_shape(bc_file_system_engine *engine, int32_t operation,
                                    const bc_file_system_location *location, int32_t shape, int32_t *existed,
                                    bc_file_system_error *error) {
    /* One call removes the entry (under `at` the whole text by path, so a missing or non-directory component on
     * the way is that call's answer, read below as the absence it always was). */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    const int32_t resolution =
        bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
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
     * everywhere, so a look at the entry tells the two apart. ENOENT is the entry's absence, and so is ENOTDIR,
     * which this call answers only for a component on the way (never for its leaf), as a resolution did. */
    const bc_file_system_call_outcome removed =
        bc_file_system_call_unlinkat(engine, operation, resolved.parent, (const char *)name.text, 0);
    if (removed.succeeded) {
        *existed = 1;
        return succeed(error);
    }
    int32_t kind = bc_file_system_call_kind_of_errno(removed.code);
    if (!removed.stopped && means_absence(kind)) {
        *existed = 0;
        return succeed(error);
    }
    if (!removed.stopped && removed.code == EPERM) {
        struct stat info;
        const bc_file_system_call_outcome looked = bc_file_system_call_fstatat(
            engine, operation, resolved.parent, (const char *)name.text, AT_SYMLINK_NOFOLLOW, &info);
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

    const int32_t kind =
        remove_file_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call, existed, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return remove_file_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, existed,
                                error);
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

/* The body of `bc_file_system_remove_directory`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t remove_directory_in_shape(bc_file_system_engine *engine, int32_t operation,
                                         const bc_file_system_location *location, int32_t shape, int32_t *existed,
                                         bc_file_system_error *error) {
    /* One call removes the directory (under `at` the whole text by path). */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    const int32_t resolution =
        bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
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
        bc_file_system_call_unlinkat(engine, operation, resolved.parent, (const char *)name.text, AT_REMOVEDIR);
    if (removed.succeeded) {
        *existed = 1;
        return succeed(error);
    }
    if (!removed.stopped && removed.code == ENOENT) {
        *existed = 0;
        return succeed(error);
    }
    /* Beside a held parent ENOTDIR is the leaf's own answer: something that is not a directory. Under the whole
     * text it is also a file on the way, which a resolution answered as absence: one look that never follows, on
     * this failing path only, tells them apart, nothing there being the absence it always was. */
    if (!removed.stopped && removed.code == ENOTDIR && bc_file_system_resolved_is_whole_text(&resolved)) {
        struct stat info;
        const bc_file_system_call_outcome looked = bc_file_system_call_fstatat(
            engine, operation, resolved.parent, (const char *)name.text, AT_SYMLINK_NOFOLLOW, &info);
        if (!looked.succeeded) {
            const int32_t kind = bc_file_system_call_kind_of_errno(looked.code);
            if (!looked.stopped && means_absence(kind)) {
                *existed = 0;
                return succeed(error);
            }
            return report_outcome(error, operation, bc_file_system_step_fstatat, &looked, kind);
        }
    }
    return report_outcome(error, operation, bc_file_system_step_unlinkat, &removed,
                          kind_of_directory_removal_failure(removed.code));
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

    const int32_t kind = remove_directory_in_shape(engine, operation, location,
                                                   bc_file_system_resolution_shape_one_call, existed, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return remove_directory_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, existed,
                                     error);
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

/* Whether the directory `expected` describes is the root of the file system, by identity: compared with the
 * root's identity the engine read when it was created, so the refusal asks the system nothing. */
static bool is_the_root_by_identity(const bc_file_system_engine *engine,
                                    const bc_file_system_expected_entry *expected) {
    if (expected->device != engine->root_device) {
        return false;
    }
    return expected->inode == engine->root_inode;
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
     * any removal (against the identity the engine read at its creation): the one argument this operation will
     * not take. */
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
     * symlink included; a directory that is not the root is opened read-only and emptied. */
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
    if (is_the_root_by_identity(engine, &expected)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* The top is opened read-only and never through a symlink. Under `beneath` the open is bracketed against
     * the stat above, the containment the header promises there; under `at` the system's resolution is the
     * promise, so the stat above decides what the leaf is and nothing is checked around the open. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    if (location->kind == bc_file_system_location_kind_at) {
        kind = bc_file_system_resolve_open_directory(engine, operation, resolved.parent, (const char *)resolved.leaf,
                                                     bc_file_system_final_symlink_refuse,
                                                     bc_file_system_directory_access_read, &opened, error);
    } else {
        kind = bc_file_system_resolve_open_expected_directory(
            engine, operation, resolved.parent, (const char *)resolved.leaf, bc_file_system_final_symlink_refuse,
            bc_file_system_directory_access_read, &expected, &opened, error);
    }
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
    /* The directory the name goes into is opened for search (bracketed under `beneath`); a final symlink is the
     * system's to resolve under `at` and refused under `beneath`, as for any component. The root is the
     * descriptor the resolution opened. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    int directory = resolved.parent;
    if (resolved.leaf_length > 0) {
        kind =
            open_resolved_directory(engine, operation, parent->kind, &resolved, resolved.leaf,
                                    symlink_choice_of(parent), bc_file_system_directory_access_search, &opened, error);
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

/* The body of `bc_file_system_set_permissions`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t set_permissions_in_shape(bc_file_system_engine *engine, int32_t operation,
                                        const bc_file_system_location *location, int32_t shape, uint32_t mode,
                                        int32_t final_symlink, bc_file_system_error *error) {
    /* The mode is set by name (under `at` the whole text by path), after one look under `refuse`: both calls act on
     * the location itself, so nothing is held. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name leaf_name;
    const int32_t kind =
        bc_file_system_resolve_to_name(engine, operation, location, shape, &leaf_name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root has no leaf: it is `.` beneath the descriptor the walk opened. */
    const char *name = resolved.leaf_length == 0 ? "." : (const char *)leaf_name.text;
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

    const int32_t kind = set_permissions_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call,
                                                  mode, final_symlink, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return set_permissions_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, mode,
                                    final_symlink, error);
}

/* ---- Moves and copies ---------------------------------------------------------------------------------------- */

/* The two operations below take two locations, and every failure of theirs says which one it belongs to in
 * `error.failed_location`: the source is the first location, the destination the second, and the faces compose
 * the error's path from the one named. A refusal of the arguments themselves names neither (0), like every
 * one-location operation's error. The field is set where the operation reads a helper's failure, since the
 * helpers (the resolver, the staged write's steps) serve one-location operations too and know nothing of it. */

enum : int32_t {
    /* The values of `bc_file_system_error.failed_location`, as the header numbers them. */
    first_location = 1,
    second_location = 2
};

/* Names the location a failing helper acted on, and answers the kind so a call reads
 * `return failed_at(error, kind, first_location)`; a success (0) passes through untouched. */
static int32_t failed_at(bc_file_system_error *error, int32_t kind, int32_t location) {
    if (kind != 0) {
        error->failed_location = location;
    }
    return kind;
}

/* ---- rename -------------------------------------------------------------------------------------------------- */

/* The kind a refused rename decides, beyond the one table. Under `replace`: a directory destination that is not
 * empty is `directory_not_empty` (ENOTEMPTY, or EEXIST where the system spells it so), and EINVAL, which renameat
 * answers only for a source that is an ancestor of its destination, is `invalid_argument` with the code kept,
 * since the arguments themselves are refused and nothing was touched. Under `refuse`: EEXIST is the table's
 * `already_exists` with the destination untouched, and EINVAL is `unsupported` with the code kept, because a
 * Linux file system without RENAME_NOREPLACE and the ancestor case answer the same EINVAL, which cannot be told
 * apart without a second call, and refusing is the honest answer for the one the library cannot rule out. */
static int32_t kind_of_rename_failure(int code, int32_t existing_destination) {
    if (existing_destination == bc_file_system_existing_destination_replace) {
        if (code == ENOTEMPTY) {
            return bc_file_system_error_kind_directory_not_empty;
        }
        if (code == EEXIST) {
            return bc_file_system_error_kind_directory_not_empty;
        }
        if (code == EINVAL) {
            return bc_file_system_error_kind_invalid_argument;
        }
        return bc_file_system_call_kind_of_errno(code);
    }
    if (code == EINVAL) {
        return bc_file_system_error_kind_unsupported;
    }
    return bc_file_system_call_kind_of_errno(code);
}

/* Whether the two parents of a rename, the source's (`first`) and the destination's (`second`), are one
 * directory, by identity (`st_dev` and `st_ino`, read with fstat on each unless they are one descriptor
 * already): how a durable rename knows whether its two parents take one sync or two. Asked before the rename,
 * so a failure is an error with nothing changed, named for the location whose parent refused the stat. */
static int32_t same_directory(bc_file_system_engine *engine, int32_t operation, int first, int second, bool *same,
                              bc_file_system_error *error) {
    if (first == second) {
        *same = true;
        return 0;
    }
    struct stat first_info;
    const bc_file_system_call_outcome first_read = bc_file_system_call_fstat(engine, operation, first, &first_info);
    if (!first_read.succeeded) {
        return failed_at(error,
                         report_outcome(error, operation, bc_file_system_step_fstat, &first_read,
                                        bc_file_system_call_kind_of_errno(first_read.code)),
                         first_location);
    }
    struct stat second_info;
    const bc_file_system_call_outcome second_read = bc_file_system_call_fstat(engine, operation, second, &second_info);
    if (!second_read.succeeded) {
        return failed_at(error,
                         report_outcome(error, operation, bc_file_system_step_fstat, &second_read,
                                        bc_file_system_call_kind_of_errno(second_read.code)),
                         second_location);
    }
    if ((uint64_t)first_info.st_dev != (uint64_t)second_info.st_dev) {
        *same = false;
        return 0;
    }
    *same = (uint64_t)first_info.st_ino == (uint64_t)second_info.st_ino;
    return 0;
}

/* The one step that moves the entry: renameat under `replace` (a file replaces a file, a directory an empty
 * directory), the platform's no-replace rename under `refuse`, each taking the two names the resolutions answered
 * (a leaf beside its held parent, or an `at` location's whole text beside AT_FDCWD). EXDEV is the table's
 * `cross_device`: the header promises one step, so a rename across devices is refused, never copied and deleted.
 * The step acts on both ends, so its failure is named for the destination (what it holds, or the device it is
 * on, is what refuses), except `not_found`: with the destination's parent held, what is missing can only be the
 * source's entry. With the destination's whole text it may also be a directory missing on the destination's way,
 * which a held parent had ruled out, so one look at the source that never follows, on this failing path only,
 * tells them apart: a source still there makes the missing thing the destination's. */
static int32_t move_entry(bc_file_system_engine *engine, int32_t operation, const bc_file_system_resolved *source,
                          const bc_file_system_name *source_name, const bc_file_system_resolved *destination,
                          const bc_file_system_name *destination_name, int32_t existing_destination,
                          bc_file_system_error *error) {
    const char *from = (const char *)source_name->text;
    const char *to = (const char *)destination_name->text;
    int32_t step = bc_file_system_step_renameat;
    bc_file_system_call_outcome outcome;
    if (existing_destination == bc_file_system_existing_destination_replace) {
        outcome = bc_file_system_call_renameat(engine, operation, source->parent, from, destination->parent, to);
    } else {
        step = bc_file_system_step_rename_no_replace;
        outcome =
            bc_file_system_call_rename_no_replace(engine, operation, source->parent, from, destination->parent, to);
    }
    if (!outcome.succeeded) {
        const int32_t kind = kind_of_rename_failure(outcome.code, existing_destination);
        int32_t location = kind == bc_file_system_error_kind_not_found ? first_location : second_location;
        if (kind == bc_file_system_error_kind_not_found && !outcome.stopped &&
            bc_file_system_resolved_is_whole_text(destination)) {
            struct stat info;
            const bc_file_system_call_outcome looked =
                bc_file_system_call_fstatat(engine, operation, source->parent, from, AT_SYMLINK_NOFOLLOW, &info);
            if (looked.stopped) {
                return failed_at(error,
                                 report_outcome(error, operation, bc_file_system_step_fstatat, &looked,
                                                bc_file_system_error_kind_io_failed),
                                 first_location);
            }
            if (looked.succeeded) {
                location = second_location;
            }
        }
        return failed_at(error, report_outcome(error, operation, step, &outcome, kind), location);
    }
    return 0;
}

/* The body of `bc_file_system_rename`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t rename_in_shape(bc_file_system_engine *engine, int32_t operation, const bc_file_system_location *source,
                               const bc_file_system_location *destination, int32_t shape, int32_t existing_destination,
                               int32_t durability, int32_t *durability_achieved, bc_file_system_error *error) {
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved from = bc_file_system_resolved_empty();
    bc_file_system_name from_name;
    int32_t kind = bc_file_system_resolve_to_name(engine, operation, source, shape, &from_name, &from, error);
    if (kind != 0) {
        return failed_at(error, kind, first_location);
    }
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved to = bc_file_system_resolved_empty();
    bc_file_system_name to_name;
    kind = bc_file_system_resolve_to_name(engine, operation, destination, shape, &to_name, &to, error);
    if (kind != 0) {
        return failed_at(error, kind, second_location);
    }
    if (from.leaf_length == 0) {
        return failed_at(error, refuse(error, bc_file_system_error_kind_invalid_argument, operation), first_location);
    }
    if (to.leaf_length == 0) {
        return failed_at(error, refuse(error, bc_file_system_error_kind_invalid_argument, operation), second_location);
    }
    /* The source is looked at first, never followed (a symlink is moved as itself): a source that is not there
     * is `not_found` at its own stat, with nothing else asked. The look decides nothing about what is moved; the
     * rename is the one step, and a source that vanished in between is the same `not_found` from it. */
    struct stat source_info;
    const bc_file_system_call_outcome looked = bc_file_system_call_fstatat(
        engine, operation, from.parent, (const char *)from_name.text, AT_SYMLINK_NOFOLLOW, &source_info);
    if (!looked.succeeded) {
        return failed_at(error,
                         report_outcome(error, operation, bc_file_system_step_fstatat, &looked,
                                        bc_file_system_call_kind_of_errno(looked.code)),
                         first_location);
    }
    bool one_directory = true;
    if (durability == bc_file_system_durability_durable) {
        kind = same_directory(engine, operation, from.parent, to.parent, &one_directory, error);
        if (kind != 0) {
            return kind;
        }
    }
    kind = move_entry(engine, operation, &from, &from_name, &to, &to_name, existing_destination, error);
    if (kind != 0) {
        return kind;
    }
    /* The name has moved: from here every failure lowers the answer. The destination's directory holds the new
     * name and is synced first; the source's held the old one and is synced too when it is another directory,
     * unless the first sync already lowered the answer, which decides it. Only a stop ends either sync. */
    int32_t achieved = durability;
    if (achieved == bc_file_system_durability_durable) {
        kind = sync_directory_after_change(engine, operation, to.parent, &achieved, error);
        if (kind != 0) {
            return failed_at(error, kind, second_location);
        }
    }
    if (achieved == bc_file_system_durability_durable && !one_directory) {
        kind = sync_directory_after_change(engine, operation, from.parent, &achieved, error);
        if (kind != 0) {
            return failed_at(error, kind, first_location);
        }
    }
    *durability_achieved = achieved;
    return succeed(error);
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
    bc_file_system_engine_before_operation(engine, operation);
    /* The promise that cannot be kept is the destination's, so the refusal is named for it. */
    if (cannot_refuse_to_replace(engine, existing_destination)) {
        return failed_at(error, refuse(error, bc_file_system_error_kind_unsupported, operation), second_location);
    }

    /* Both locations resolve first, the source then the destination, before anything moves. A durable rename
     * syncs the directories it moved the name between, so its steps act in those directories as a unit and both
     * parents are held; a `written` one is its look and its one move, which under `at` take the whole texts by
     * path. The two names are the two PATH_MAX buffers of this call, one per location. The root of the file
     * system is neither moved nor replaced. */
    const int32_t shape = durability == bc_file_system_durability_durable ? bc_file_system_resolution_shape_held_parent
                                                                          : bc_file_system_resolution_shape_one_call;
    const int32_t kind = rename_in_shape(engine, operation, source, destination, shape, existing_destination,
                                         durability, durability_achieved, error);
    if (!(needs_the_held_parent(source, shape, kind, error) ||
          needs_the_held_parent(destination, shape, kind, error))) {
        return kind;
    }
    return rename_in_shape(engine, operation, source, destination, bc_file_system_resolution_shape_held_parent,
                           existing_destination, durability, durability_achieved, error);
}

/* ---- copy_file ----------------------------------------------------------------------------------------------- */

/* The errnos with which a fast copy path (the platform's clone, its in-kernel copy) says "not these two files,
 * not on this volume, not here": the platform's own ENOTSUP, the kernel's EOPNOTSUPP or ENOTTY for a volume
 * without reflinks, EXDEV across volumes, EINVAL for a file the call will not take, and ENOSYS where a script
 * spells a missing call. Each makes the operation move the bytes the next way down. An if-chain because ENOTSUP
 * and EOPNOTSUPP are one number on Linux. */
static bool means_no_fast_copy(int code) {
    if (code == ENOTSUP) {
        return true;
    }
    if (code == EOPNOTSUPP) {
        return true;
    }
    if (code == ENOTTY) {
        return true;
    }
    if (code == ENOSYS) {
        return true;
    }
    if (code == EXDEV) {
        return true;
    }
    return code == EINVAL;
}

/* Opens the source to be read and judges it by its descriptor, as a read does: never followed under `refuse` (a
 * symlink answers ELOOP, the table's `symlink_refused`), followed to the file it names under `follow`; anything
 * that is not a regular file is `not_regular_file` with no step, the root included, since it has no leaf. */
static int32_t open_source_file(bc_file_system_engine *engine, int32_t operation, const bc_file_system_resolved *source,
                                int32_t final_symlink, owned_descriptor *file, struct stat *info,
                                bc_file_system_error *error) {
    if (source->leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    const int flags =
        final_symlink == bc_file_system_final_symlink_refuse ? file_read_flags | O_NOFOLLOW : file_read_flags;
    const bc_file_system_call_outcome opened =
        bc_file_system_call_openat(engine, operation, source->parent, (const char *)source->leaf, flags, &file->value);
    if (!opened.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_openat, &opened,
                              bc_file_system_call_kind_of_errno(opened.code));
    }
    const bc_file_system_call_outcome measured = bc_file_system_call_fstat(engine, operation, file->value, info);
    if (!measured.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstat, &measured,
                              bc_file_system_call_kind_of_errno(measured.code));
    }
    if (!S_ISREG(info->st_mode)) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    return 0;
}

/* The platform's clone makes the staged file whole in one step, under a name of this process's own like a
 * staged write's; the clone, not an exclusive create, is what claims the name, so a name that is taken (EEXIST)
 * is tried again with the next token, a bounded number of times. A platform or volume that will not clone these
 * two answers that nothing was made (`*cloned` false), and the operation copies the bytes itself. */
static int32_t stage_copy_by_clone(bc_file_system_engine *engine, int32_t operation, staged_file *staged, int source,
                                   const uint8_t *leaf, size_t leaf_length, bool *cloned, bc_file_system_error *error) {
    for (uint32_t attempt = 0; attempt < staging_attempts; ++attempt) {
        const uint64_t token =
            atomic_fetch_add_explicit(&engine->staging_counter, (uint64_t)1, memory_order_relaxed) + 1;
        spell_staged_name(staged->name, leaf, leaf_length, engine->process_id, token);
        const bc_file_system_call_outcome outcome = bc_file_system_call_clone_file(
            engine, operation, source, staged->parent, (const char *)staged->name, staged_file_mode);
        if (outcome.succeeded) {
            staged->created = true;
            *cloned = true;
            return 0;
        }
        if (!outcome.stopped && outcome.code == EEXIST) {
            continue;
        }
        if (!outcome.stopped && means_no_fast_copy(outcome.code)) {
            *cloned = false;
            return 0;
        }
        return report_staged_outcome(staged, error, operation, bc_file_system_step_clone_file, &outcome,
                                     bc_file_system_call_kind_of_errno(outcome.code));
    }
    /* Every name tried was taken: the system's refusal, not the destination's existence. */
    const bc_file_system_call_outcome taken = {.succeeded = false, .code = EEXIST, .stopped = false};
    return report_outcome(error, operation, bc_file_system_step_clone_file, &taken,
                          bc_file_system_error_kind_io_failed);
}

/* The kernel's own copy, in pieces, until the source ends (`*complete`), or until the kernel answers that it
 * will not copy these two, in which case `*copied` is how far it got and the read-and-write loop continues from
 * there: both descriptors' offsets moved with the bytes, so nothing is copied twice or skipped. */
static int32_t copy_by_copy_range(bc_file_system_engine *engine, int32_t operation, staged_file *staged, int source,
                                  uint64_t *copied, bool *complete, bc_file_system_error *error) {
    *copied = 0;
    for (;;) {
        size_t piece = 0;
        const bc_file_system_call_outcome outcome = bc_file_system_call_copy_range(
            engine, operation, source, staged->descriptor.value, largest_single_transfer, &piece);
        if (!outcome.succeeded) {
            if (!outcome.stopped && means_no_fast_copy(outcome.code)) {
                *complete = false;
                return 0;
            }
            return report_staged_outcome(staged, error, operation, bc_file_system_step_copy_range, &outcome,
                                         bc_file_system_call_kind_of_errno(outcome.code));
        }
        if (piece == 0) {
            *complete = true;
            return 0;
        }
        *copied += piece;
    }
}

/* The portable copy: the source read at the running offset into one bounded scratch buffer and written to the
 * staged file, whose own offset stands at `offset` already, until the source answers no more bytes. The source's
 * size is never consulted, so the copy is of the source as it is read, to its end. */
static int32_t copy_by_loop(bc_file_system_engine *engine, int32_t operation, staged_file *staged, int source,
                            uint64_t offset, bc_file_system_error *error) {
    [[gnu::cleanup(free_owned_buffer)]] owned_buffer scratch = {.value = malloc(copy_piece_capacity)};
    if (scratch.value == nullptr) {
        return refuse(error, bc_file_system_error_kind_resource_exhausted, operation);
    }
    uint8_t *bytes = scratch.value;
    for (;;) {
        size_t read_count = 0;
        const bc_file_system_call_outcome read =
            bc_file_system_call_pread(engine, operation, source, bytes, copy_piece_capacity, offset, &read_count);
        if (!read.succeeded) {
            return report_staged_outcome(staged, error, operation, bc_file_system_step_pread, &read,
                                         bc_file_system_call_kind_of_errno(read.code));
        }
        if (read_count == 0) {
            return 0;
        }
        const int32_t written = write_every_byte(engine, operation, staged, bytes, read_count, error);
        if (written != 0) {
            return written;
        }
        offset += read_count;
    }
}

/* Puts the source's bytes into a staged file beside the destination, the fastest way the platform offers for
 * these two files and the next way down where it refuses: the clone first (one step that makes the whole file,
 * then opened to be settled like any staged file), else an exclusive create and the kernel's copy, else the same
 * create and the read-and-write loop. Every way ends with the staged file open on `staged->descriptor`, holding
 * every byte, and the same steps follow. */
static int32_t stage_copied_bytes(bc_file_system_engine *engine, int32_t operation, staged_file *staged, int source,
                                  const uint8_t *leaf, size_t leaf_length, bc_file_system_error *error) {
    const bc_file_system_platform_fast_paths *fast_paths = bc_file_system_engine_fast_paths(engine);
    if (fast_paths->has_clone) {
        bool cloned = false;
        const int32_t kind = stage_copy_by_clone(engine, operation, staged, source, leaf, leaf_length, &cloned, error);
        if (kind != 0) {
            return kind;
        }
        if (cloned) {
            const bc_file_system_call_outcome opened =
                bc_file_system_call_openat(engine, operation, staged->parent, (const char *)staged->name,
                                           staged_reopen_flags, &staged->descriptor.value);
            if (!opened.succeeded) {
                return report_staged_outcome(staged, error, operation, bc_file_system_step_openat, &opened,
                                             bc_file_system_call_kind_of_errno(opened.code));
            }
            return 0;
        }
    }
    const int32_t created = create_staged_file(engine, operation, staged, leaf, leaf_length, error);
    if (created != 0) {
        return created;
    }
    uint64_t copied = 0;
    if (fast_paths->has_copy_range) {
        bool complete = false;
        const int32_t kind = copy_by_copy_range(engine, operation, staged, source, &copied, &complete, error);
        if (kind != 0) {
            return kind;
        }
        if (complete) {
            return 0;
        }
    }
    return copy_by_loop(engine, operation, staged, source, copied, error);
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
    /* The promise that cannot be kept is the destination's, as for a rename. */
    if (cannot_refuse_to_replace(engine, options->existing_destination)) {
        return failed_at(error, refuse(error, bc_file_system_error_kind_unsupported, operation), second_location);
    }

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved from = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, source, &from, error);
    if (kind != 0) {
        return failed_at(error, kind, first_location);
    }
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved to = bc_file_system_resolved_empty();
    kind = bc_file_system_resolve(engine, operation, destination, &to, error);
    if (kind != 0) {
        return failed_at(error, kind, second_location);
    }
    /* The source is open and known to be a regular file before the destination is judged, so a copy of nothing
     * or of a directory touches nothing at the destination. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor file = {.value = -1};
    struct stat source_info;
    kind = open_source_file(engine, operation, &from, options->final_symlink, &file, &source_info, error);
    if (kind != 0) {
        return failed_at(error, kind, first_location);
    }
    /* The destination follows every rule of a staged write: a regular file is replaced (its mode kept under
     * `preserve_or`; the source's mode plays no part), a symlink refused or followed, anything else refused. */
    destination_entry existing;
    kind = judge_destination(engine, operation, destination, options->final_symlink, &to, &existing, error);
    if (kind != 0) {
        return failed_at(error, kind, second_location);
    }
    /* A destination that is the source itself, under either name of the same file (a hard link included), is
     * refused before anything is staged: there is nothing to copy, and staging would only replace the file with
     * itself. The destination is what is refused: it is the source under another name. */
    if (existing.present && existing.device == (uint64_t)source_info.st_dev &&
        existing.inode == (uint64_t)source_info.st_ino) {
        return failed_at(error, refuse(error, bc_file_system_error_kind_invalid_argument, operation), second_location);
    }
    uint32_t mode = options->permissions.mode;
    if (options->permissions.policy == bc_file_system_permissions_policy_preserve_or && existing.present) {
        mode = existing.mode;
    }
    /* From here every step acts beside the destination: the staged file lives in its directory, and the
     * source is only read from. A read of the source that fails is still reported there, under the source. */
    uint8_t staged_name[NAME_MAX + 1] = {};
    [[gnu::cleanup(discard_staged_file)]] staged_file staged = staged_file_in(to.parent, staged_name);
    kind = stage_copied_bytes(engine, operation, &staged, file.value, to.leaf, to.leaf_length, error);
    if (kind != 0) {
        if (error->step == bc_file_system_step_pread) {
            return failed_at(error, kind, first_location);
        }
        return failed_at(error, kind, second_location);
    }
    int32_t achieved = options->durability;
    kind = place_staged_file(engine, operation, &staged, to.leaf, mode, options, &achieved, error);
    if (kind != 0) {
        return failed_at(error, kind, second_location);
    }
    *durability_achieved = achieved;
    return succeed(error);
}

/* ---- try_lock ------------------------------------------------------------------------------------------------ */

/* Whether `flock` answered that another open-file description holds the lock: EWOULDBLOCK, which is EAGAIN on
 * every platform the library ships for (an if-chain, since one number cannot be two cases of a switch). */
static bool means_lock_held_elsewhere(int code) {
    if (code == EWOULDBLOCK) {
        return true;
    }
    return code == EAGAIN;
}

/* The body of `bc_file_system_try_lock`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t try_lock_in_shape(bc_file_system_engine *engine, int32_t operation,
                                 const bc_file_system_location *location, int32_t shape, bc_file_system_lock **lock,
                                 int32_t *outcome, bc_file_system_error *error) {
    /* One call opens the entry (under `at` the whole text by path); the lock is taken on its descriptor. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name leaf_name;
    const int32_t kind =
        bc_file_system_resolve_to_name(engine, operation, location, shape, &leaf_name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The lock opens the entry itself, read-only and never through a symlink (ELOOP is the table's
     * `symlink_refused`), so the descriptor the handle owns is the open-file description the lock is held by;
     * the walk's search-only descriptor could not hold one (flock on an O_PATH descriptor is EBADF). A regular
     * file or a directory may be locked, the root included, as `.` beneath the descriptor the walk opened;
     * anything else (a FIFO, a device) is refused after a look at the descriptor, with no step, like a refusal. */
    const char *name = resolved.leaf_length == 0 ? "." : (const char *)leaf_name.text;
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor file = {.value = -1};
    const bc_file_system_call_outcome opened =
        bc_file_system_call_openat(engine, operation, resolved.parent, name, lock_open_flags, &file.value);
    if (!opened.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_openat, &opened,
                              bc_file_system_call_kind_of_errno(opened.code));
    }
    struct stat info;
    const bc_file_system_call_outcome looked = bc_file_system_call_fstat(engine, operation, file.value, &info);
    if (!looked.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstat, &looked,
                              bc_file_system_call_kind_of_errno(looked.code));
    }
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) {
        return refuse(error, bc_file_system_error_kind_not_regular_file, operation);
    }
    /* Exclusive and without waiting: a lock held elsewhere is an answer, not an error, and nothing is waited
     * for. A platform or volume that refuses to lock (ENOTSUP) is the table's `unsupported`. */
    const bc_file_system_call_outcome locked =
        bc_file_system_call_flock(engine, operation, file.value, LOCK_EX | LOCK_NB);
    if (!locked.succeeded) {
        if (!locked.stopped && means_lock_held_elsewhere(locked.code)) {
            *lock = nullptr;
            *outcome = bc_file_system_lock_outcome_held_elsewhere;
            return succeed(error);
        }
        return report_outcome(error, operation, bc_file_system_step_flock, &locked,
                              bc_file_system_call_kind_of_errno(locked.code));
    }
    /* The handle owns the descriptor from here; closing it is how the lock is let go, so a handle that cannot
     * be allocated lets the lock go through the descriptor's owner. */
    bc_file_system_lock *created = malloc(sizeof *created);
    if (created == nullptr) {
        return refuse(error, bc_file_system_error_kind_resource_exhausted, operation);
    }
    created->descriptor = release_descriptor(&file);
    *lock = created;
    *outcome = bc_file_system_lock_outcome_acquired;
    return succeed(error);
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

    /* A platform whose flock is a stub (Emscripten: every caller "acquires") offers no lock at all, and the
     * library refuses before any call rather than hand out a lock that excludes nobody. */
    if (!bc_file_system_platform_enforces_locks()) {
        return refuse(error, bc_file_system_error_kind_unsupported, operation);
    }
    const int32_t kind =
        try_lock_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call, lock, outcome, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return try_lock_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, lock, outcome,
                             error);
}

/* ---- create_symlink and read_symlink -------------------------------------------------------------------------- */

/* Whether `target` is text the system can take as a symlink's: at least one byte, no NUL inside (the NUL that
 * ends it is the engine's), and short enough for the bounded buffer it is terminated into, which is the bound
 * of any path the system opens (PATH_MAX, the terminator included). */
static bool is_symlink_target(const uint8_t *target, size_t length) {
    if (target == nullptr || length == 0 || length >= PATH_MAX) {
        return false;
    }
    return memchr(target, 0, length) == nullptr;
}

/* The body of `bc_file_system_create_symlink`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t create_symlink_in_shape(bc_file_system_engine *engine, int32_t operation, const uint8_t *target,
                                       size_t target_length, const bc_file_system_location *location, int32_t shape,
                                       bc_file_system_error *error) {
    /* One call makes the link (under `at` at the whole text by path). */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name name;
    const int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root is always there; a link cannot take its name. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_already_exists, operation);
    }
    /* The target goes to the system as the caller spelled it, NUL-terminated and nothing more: never resolved,
     * never normalised, so a relative text or a `..` is the link's to mean. The second PATH_MAX buffer of this
     * call, beside the location's name: the two texts the one call takes. */
    uint8_t text[PATH_MAX];
    memcpy(text, target, target_length);
    text[target_length] = 0;
    const bc_file_system_call_outcome made =
        bc_file_system_call_symlinkat(engine, operation, (const char *)text, resolved.parent, (const char *)name.text);
    if (!made.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_symlinkat, &made,
                              bc_file_system_call_kind_of_errno(made.code));
    }
    return succeed(error);
}

int32_t bc_file_system_create_symlink(bc_file_system_engine *engine, const uint8_t *target, size_t target_length,
                                      const bc_file_system_location *location, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_create_symlink;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_symlink_target(target, target_length) || !is_valid_location(location)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    bc_file_system_engine_before_operation(engine, operation);

    const int32_t kind = create_symlink_in_shape(engine, operation, target, target_length, location,
                                                 bc_file_system_resolution_shape_one_call, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return create_symlink_in_shape(engine, operation, target, target_length, location,
                                   bc_file_system_resolution_shape_held_parent, error);
}

/* The body of `bc_file_system_read_symlink`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t read_symlink_in_shape(bc_file_system_engine *engine, int32_t operation,
                                     const bc_file_system_location *location, int32_t shape, uint8_t *buffer,
                                     size_t capacity, size_t *length, bc_file_system_error *error) {
    /* One call reads the link (under `at` the whole text by path). */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name leaf_name;
    const int32_t kind =
        bc_file_system_resolve_to_name(engine, operation, location, shape, &leaf_name, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* readlinkat cuts a text short silently when the buffer is too small, so the text is read whole into a
     * scratch as large as any path the system holds (the second PATH_MAX buffer of this call, beside the
     * location's name) and only then held against the caller's capacity, under the buffer protocol. The root is a
     * directory, which readlinkat refuses with EINVAL like any entry that is not a symlink, as the header says. */
    const char *name = resolved.leaf_length == 0 ? "." : (const char *)leaf_name.text;
    uint8_t text[PATH_MAX];
    size_t text_length = 0;
    const bc_file_system_call_outcome read =
        bc_file_system_call_readlinkat(engine, operation, resolved.parent, name, text, sizeof text, &text_length);
    if (!read.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_readlinkat, &read,
                              bc_file_system_call_kind_of_errno(read.code));
    }
    /* A text that filled the scratch may have been cut short: longer than any path the system opens, which
     * no system of the library's makes, and refused rather than answered in part. */
    if (text_length == sizeof text) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    if (text_length > capacity) {
        *length = text_length;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    if (text_length > 0) {
        memcpy(buffer, text, text_length);
    }
    *length = text_length;
    return succeed(error);
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

    const int32_t kind = read_symlink_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call,
                                               buffer, capacity, length, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return read_symlink_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, buffer,
                                 capacity, length, error);
}

/* ---- condemn ------------------------------------------------------------------------------------------------- */

enum : size_t {
    /* What a condemned name holds beside the leaf: the prefix, and the dash and token a retry appends. */
    condemned_name_overhead = (sizeof condemned_prefix - 1) + 1 + decimal_digit_capacity,
    /* How much of the leaf a condemned name keeps, so that the name fits NAME_MAX with a token appended. */
    condemned_leaf_capacity = NAME_MAX - condemned_name_overhead
};

/* The condemned name of `leaf`, NUL-terminated into `name` (which holds NAME_MAX + 1 bytes): the prefix and the
 * leaf, cut to what leaves room for a token; then, when `token` is not 0, a dash and the token, which is how a
 * name a previous condemn left behind is told apart from this one. Answers the length. */
static size_t spell_condemned_name(uint8_t *name, const uint8_t *leaf, size_t leaf_length, uint64_t token) {
    const size_t leaf_prefix = leaf_length < condemned_leaf_capacity ? leaf_length : condemned_leaf_capacity;
    size_t position = 0;
    memcpy(name, condemned_prefix, sizeof condemned_prefix - 1);
    position += sizeof condemned_prefix - 1;
    memcpy(name + position, leaf, leaf_prefix);
    position += leaf_prefix;
    if (token != 0) {
        name[position] = '-';
        position += 1;
        position += spell_decimal(name + position, token);
    }
    name[position] = 0;
    return position;
}

/* The body of `bc_file_system_condemn`, in the shape its entry chose: the whole text first where the
 * operation acts in one call, then the held parent's when `needs_the_held_parent` says so. */
static int32_t condemn_in_shape(bc_file_system_engine *engine, int32_t operation,
                                const bc_file_system_location *location, int32_t shape, uint8_t *buffer,
                                size_t capacity, size_t *length, bc_file_system_error *error) {
    /* One call moves the entry beside itself: under `at` from its whole text to a second text, its parent's text
     * and the condemned name, which the resolution's bound (the parent, a slash and NAME_MAX bytes fit one call)
     * guarantees one call takes. */
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_name entry;
    const int32_t kind = bc_file_system_resolve_to_name(engine, operation, location, shape, &entry, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The root of the file system is neither renamed nor swept. */
    if (resolved.leaf_length == 0) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* One step, the platform's no-replace rename within the one parent, so a condemned entry is whole under its
     * new name or untouched under its old one. The plain condemned name is tried first; one that is taken (a
     * previous condemn the sweep never cleared) is tried again with a token of the engine's own appended, a
     * bounded number of times, so a stale leftover never fails a condemn. The name is held against the caller's
     * capacity before each try, so a size query renames nothing and a name that outgrew the buffer on a retry
     * is answered with nothing moved. ENOENT is the entry's absence; EINVAL, which a Linux file system without
     * the no-replace rename answers, is `unsupported`, as it is for a rename under `refuse`.
     *
     * The destination is the name alone beside a held parent; beside AT_FDCWD it is a text of its own, the
     * entry's text up to its leaf (the parent and its slash) with the name after it. The second PATH_MAX buffer
     * of this call, beside the entry's. */
    uint8_t destination[PATH_MAX];
    size_t prefix_length = 0;
    if (bc_file_system_resolved_is_whole_text(&resolved)) {
        prefix_length = location->length - resolved.leaf_length;
        memcpy(destination, location->path, prefix_length);
    }
    assert(prefix_length + NAME_MAX < sizeof destination);
    uint8_t *name = destination + prefix_length;
    uint64_t token = 0;
    for (uint32_t attempt = 0; attempt < staging_attempts; ++attempt) {
        const size_t name_length = spell_condemned_name(name, resolved.leaf, resolved.leaf_length, token);
        if (name_length > capacity) {
            *length = name_length;
            return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
        }
        const bc_file_system_call_outcome moved = bc_file_system_call_rename_no_replace(
            engine, operation, resolved.parent, (const char *)entry.text, resolved.parent, (const char *)destination);
        if (moved.succeeded) {
            memcpy(buffer, name, name_length);
            *length = name_length;
            return succeed(error);
        }
        if (!moved.stopped && moved.code == EEXIST) {
            token = atomic_fetch_add_explicit(&engine->staging_counter, (uint64_t)1, memory_order_relaxed) + 1;
            continue;
        }
        return report_outcome(error, operation, bc_file_system_step_rename_no_replace, &moved,
                              kind_of_rename_failure(moved.code, bc_file_system_existing_destination_refuse));
    }
    /* Every name tried was taken: the system's refusal, not the entry's existence. */
    const bc_file_system_call_outcome taken = {.succeeded = false, .code = EEXIST, .stopped = false};
    return report_outcome(error, operation, bc_file_system_step_rename_no_replace, &taken,
                          bc_file_system_error_kind_io_failed);
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
    /* A condemn never replaces: its one step is the no-replace rename, whatever the caller holds. */
    if (cannot_refuse_to_replace(engine, bc_file_system_existing_destination_refuse)) {
        return refuse(error, bc_file_system_error_kind_unsupported, operation);
    }

    const int32_t kind = condemn_in_shape(engine, operation, location, bc_file_system_resolution_shape_one_call, buffer,
                                          capacity, length, error);
    if (!needs_the_held_parent(location, bc_file_system_resolution_shape_one_call, kind, error)) {
        return kind;
    }
    return condemn_in_shape(engine, operation, location, bc_file_system_resolution_shape_held_parent, buffer, capacity,
                            length, error);
}

/* ---- remove_condemned ---------------------------------------------------------------------------------------- */

/* Whether a name is one `condemn` made: it starts with the condemned prefix. */
static bool is_condemned_name(const uint8_t *name, size_t length) {
    if (length < sizeof condemned_prefix - 1) {
        return false;
    }
    return memcmp(name, condemned_prefix, sizeof condemned_prefix - 1) == 0;
}

/* Whether a name is a staged file's, left by a write or a copy that was killed between staging and renaming
 * into place (a clone's leftover carries the same infix): the infix sits after the leaf, so anywhere inside. */
static bool is_staged_name(const uint8_t *name) {
    return strstr((const char *)name, staging_infix) != nullptr;
}

/* Puts `entry` and a slash before the name the error carries, so an error inside a condemned tree names its
 * entry relative to the swept directory; a name the field cannot hold is cut at the end and `name_truncated`
 * says so. An error that named nothing (the entry itself refused) names the entry alone. */
static void name_entry_before_error_name(bc_file_system_error *error, const uint8_t *entry) {
    const size_t entry_length = strlen((const char *)entry);
    if (error->name_length == 0) {
        memcpy(error->name, entry, entry_length);
        error->name_length = (uint32_t)entry_length;
        return;
    }
    /* An entry's name is at most NAME_MAX, one byte less than the field, so the entry and its slash fit and
     * only the tail is at stake. */
    const size_t room = sizeof error->name - entry_length - 1;
    size_t kept = error->name_length;
    if (kept > room) {
        kept = room;
        error->name_truncated = 1;
    }
    memmove(error->name + entry_length + 1, error->name, kept);
    memcpy(error->name, entry, entry_length);
    error->name[entry_length] = '/';
    error->name_length = (uint32_t)(entry_length + 1 + kept);
}

/* Removes the condemned entry `name` of the directory `parent`, as `remove_tree` removes a location: what the
 * entry is comes from a stat on the held directory, never followed; a directory is opened read-only, bracketed,
 * and emptied through a stream of its own, by the same bounded walk; anything else is unlinked as itself; one
 * that vanished since the listing answered it is nothing to do. An error names the entry it failed on,
 * relative to the swept directory. */
static int32_t remove_condemned_entry(bc_file_system_engine *engine, int32_t operation, int parent, const uint8_t *name,
                                      bc_file_system_error *error) {
    bc_file_system_expected_entry expected;
    int32_t kind = bc_file_system_resolve_expect_entry(engine, operation, parent, (const char *)name,
                                                       bc_file_system_final_symlink_refuse, &expected, error);
    if (kind == bc_file_system_error_kind_not_found && error->system_code == ENOENT) {
        return 0;
    }
    if (kind != 0) {
        name_entry_before_error_name(error, name);
        return kind;
    }
    if (!expected.is_directory) {
        const bc_file_system_call_outcome removed =
            bc_file_system_call_unlinkat(engine, operation, parent, (const char *)name, 0);
        if (removed.succeeded) {
            return 0;
        }
        if (!removed.stopped && removed.code == ENOENT) {
            return 0;
        }
        kind = report_outcome(error, operation, bc_file_system_step_unlinkat, &removed,
                              bc_file_system_call_kind_of_errno(removed.code));
        name_entry_before_error_name(error, name);
        return kind;
    }
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    kind = bc_file_system_resolve_open_expected_directory(
        engine, operation, parent, (const char *)name, bc_file_system_final_symlink_refuse,
        bc_file_system_directory_access_read, &expected, &opened, error);
    if (kind != 0) {
        name_entry_before_error_name(error, name);
        return kind;
    }
    [[gnu::cleanup(close_owned_directory_stream)]] owned_directory_stream top = {.value = nullptr};
    const bc_file_system_call_outcome streamed =
        bc_file_system_call_fdopendir(engine, operation, opened.value, &top.value);
    if (!streamed.succeeded) {
        kind = report_outcome(error, operation, bc_file_system_step_fdopendir, &streamed,
                              bc_file_system_call_kind_of_errno(streamed.code));
        name_entry_before_error_name(error, name);
        return kind;
    }
    (void)release_descriptor(&opened);
    kind = remove_directory_tree(engine, operation, parent, name, top.value, expected.device, error);
    if (kind != 0) {
        name_entry_before_error_name(error, name);
    }
    return kind;
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

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    int32_t kind = bc_file_system_resolve(engine, operation, parent, &resolved, error);
    if (kind != 0) {
        return kind;
    }
    /* The directory is opened read-only (bracketed under `beneath`) and read through a stream whose descriptor
     * serves every removal inside it: a final symlink is the system's to resolve under `at` and refused under
     * `beneath`, as for any component. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    kind = open_resolved_directory(engine, operation, parent->kind, &resolved, resolved.leaf, symlink_choice_of(parent),
                                   bc_file_system_directory_access_read, &opened, error);
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
    /* One pass over the stream: an entry carrying either of the engine's marks is removed as it is reached (a
     * condemned one as a tree, a staged leftover as the one file it is), everything else is left alone, and the
     * first entry that refuses ends the sweep with that entry named, what was removed before it kept. The
     * stream is read on while entries it already answered are removed, which every system the library ships for
     * takes. */
    for (;;) {
        uint8_t name[NAME_MAX + 1];
        size_t name_length = 0;
        unsigned char d_type = DT_UNKNOWN;
        bool found = false;
        kind = next_entry_name(engine, operation, stream.value, name, &name_length, &d_type, &found, error);
        if (kind != 0) {
            return kind;
        }
        if (!found) {
            return succeed(error);
        }
        if (is_condemned_name(name, name_length)) {
            kind = remove_condemned_entry(engine, operation, dirfd(stream.value), name, error);
            if (kind != 0) {
                return kind;
            }
        } else if (is_staged_name(name)) {
            const bc_file_system_call_outcome removed =
                bc_file_system_call_unlinkat(engine, operation, dirfd(stream.value), (const char *)name, 0);
            if (!removed.succeeded && !(removed.code == ENOENT && !removed.stopped)) {
                kind = report_outcome(error, operation, bc_file_system_step_unlinkat, &removed,
                                      bc_file_system_call_kind_of_errno(removed.code));
                name_entry_before_error_name(error, name);
                return kind;
            }
        }
    }
}
