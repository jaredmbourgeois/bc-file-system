/* resolve.c — the bodies behind resolve.h: the walk, the fast paths, the identity check, the errno mapping.
 *
 * One shape for every open of a directory entry: `fstatat` records what is there, `openat` opens it as a
 * directory, `fstat` reads what was opened, and the two identities are compared. The walk is that shape in a
 * loop, holding one parent descriptor at a time and closing the previous one as it goes; the fast path is that
 * shape once, with the platform's one-call open in the middle. Every system call is behind the observer's
 * `before_step`, asked again on EINTR, with the errno copied on the line after the call; the errno table and
 * the gated `fstatat`, `fstat` and `openat` are shared with the operations through resolve.h, so the core has
 * one mapping and one loop per system call. Nothing here opens a leaf, reads a byte or writes one; nothing
 * allocates.
 */
#include "resolve.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bc_file_system/bc_file_system.h>

#include "engine.h"
#include "owned.h"
#include "path.h"
#include "platform.h"

/* ---- Constants ---------------------------------------------------------------------------------------------- */

enum : int32_t {
    /* How a directory is opened to walk through it: never for writing, never inherited by a child process. */
    directory_open_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC
};

/* ---- The errno mapping -------------------------------------------------------------------------------------- */

/* The kind each errno of the resolution layer decides, in one table read by one function. A table rather than
 * a switch because two errno names are the same number on some systems, which a switch would not compile; the
 * first row that matches wins. Anything absent is `io_failed`, with the errno kept in the error. */
typedef struct errno_mapping {
    int system_code;
    int32_t kind;
} errno_mapping;

static const errno_mapping errno_mappings[] = {
    {ENOENT, bc_file_system_error_kind_not_found},
    {ENOTDIR, bc_file_system_error_kind_not_directory},
    /* A symlink the system would not follow, or a loop of them: either way a symlink sat in the way. */
    {ELOOP, bc_file_system_error_kind_symlink_refused},
    {EACCES, bc_file_system_error_kind_permission_denied},
    {EPERM, bc_file_system_error_kind_permission_denied},
    {EROFS, bc_file_system_error_kind_permission_denied},
    /* A component longer than the system takes: the path module refuses those first, so this is a contract
     * violation the system noticed before the module could. */
    {ENAMETOOLONG, bc_file_system_error_kind_invalid_argument},
    {EMFILE, bc_file_system_error_kind_resource_exhausted},
    {ENFILE, bc_file_system_error_kind_resource_exhausted},
    {ENOMEM, bc_file_system_error_kind_resource_exhausted},
};

static int32_t kind_of(int system_code) {
    for (size_t index = 0; index < sizeof errno_mappings / sizeof errno_mappings[0]; ++index) {
        if (errno_mappings[index].system_code == system_code) {
            return errno_mappings[index].kind;
        }
    }
    return bc_file_system_error_kind_io_failed;
}

int32_t bc_file_system_resolve_kind_of_errno(int system_code) {
    return kind_of(system_code);
}

/* The kind an open of a directory entry decides, which the plain table cannot: ENOTDIR from an entry the stat
 * had just seen as a directory or as a symlink means it was swapped or refused, not that a file sits there;
 * ELOOP, and the EXDEV with which openat2 reports an escape from RESOLVE_BENEATH, are containment refusals. */
static int32_t kind_of_open_failure(int system_code, const bc_file_system_expected_entry *expected) {
    if (system_code == ENOTDIR) {
        if (expected->is_directory || expected->is_symlink) {
            return bc_file_system_error_kind_symlink_refused;
        }
        return bc_file_system_error_kind_not_directory;
    }
    if (system_code == EXDEV) {
        return bc_file_system_error_kind_symlink_refused;
    }
    return kind_of(system_code);
}

/* The errnos with which a fast-path open says "there is no such call here": the walk is taken instead, which
 * is also how a test forces the portable path by scripting one of them on the fast path's step. ENOTSUP is the
 * platform layer's own answer where the call is absent; the other three are what a kernel or a seccomp policy
 * answers an unknown call or flag with. An if-chain because ENOTSUP and EOPNOTSUPP are one number on Linux. */
static bool means_no_fast_path(int system_code) {
    if (system_code == ENOTSUP) {
        return true;
    }
    if (system_code == EOPNOTSUPP) {
        return true;
    }
    if (system_code == ENOSYS) {
        return true;
    }
    return system_code == EINVAL;
}

/* ---- The return convention ---------------------------------------------------------------------------------- */

/* Fills `error` for a failure before any system call and answers the status every failing path returns. The
 * outcome of a gated call, the reading of a verdict and `report_outcome` are engine.h's, shared with the operations. */
static int32_t refuse(bc_file_system_error *error, int32_t kind, int32_t operation) {
    *error = (bc_file_system_error){.kind = kind, .operation = operation};
    return kind;
}

/* ---- The system calls, each behind the observer and asked again on EINTR ------------------------------------ */

bc_file_system_call_outcome bc_file_system_resolve_fstatat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                           const char *name, int flags, struct stat *info) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_fstatat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        if (fstatat(parent, name, info, flags) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_resolve_fstat(bc_file_system_engine *engine, int32_t operation,
                                                         int descriptor, struct stat *info) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_fstat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        if (fstat(descriptor, info) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_resolve_openat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                          const char *name, int flags, int *descriptor) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_openat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const int opened = openat(parent, name, flags);
        if (opened >= 0) {
            *descriptor = opened;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* The one open by path in the core: the root, where every absolute path starts. */
static bc_file_system_call_outcome open_root(bc_file_system_engine *engine, int32_t operation, int *descriptor) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_open);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const int opened = open("/", directory_open_flags);
        if (opened >= 0) {
            *descriptor = opened;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* The platform's one-call open of a whole relative path beneath `held`, under the step the platform's call
 * is named by. The platform body retries its own EINTR; a scripted one is retried here. */
static bc_file_system_call_outcome open_beneath_in_one_call(bc_file_system_engine *engine, int32_t operation,
                                                            int32_t step, int held, const char *relative,
                                                            int *descriptor) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, step);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const int code = bc_file_system_platform_open_beneath(held, relative, directory_open_flags, descriptor);
        if (code == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* ---- The identity check ------------------------------------------------------------------------------------- */

static bool same_identity(const bc_file_system_expected_entry *expected, const struct stat *actual) {
    if (expected->device != (uint64_t)actual->st_dev) {
        return false;
    }
    return expected->inode == (uint64_t)actual->st_ino;
}

/* Reads the identity of the descriptor just opened and holds it against what the stat saw. The comparison is
 * itself a step, so a test can count it and can script the mismatch without a race; a scripted failure is the
 * mismatch with the scripted errno, a real one carries no errno. */
static int32_t verify_opened_identity(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                      const bc_file_system_expected_entry *expected, bc_file_system_error *error) {
    struct stat actual;
    const bc_file_system_call_outcome read = bc_file_system_resolve_fstat(engine, operation, descriptor, &actual);
    if (!read.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstat, &read, kind_of(read.code));
    }
    bc_file_system_call_outcome compared = {.succeeded = false, .code = 0, .stopped = false};
    const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_identity_check);
    if (observer_decided(&verdict, &compared)) {
        return report_outcome(error, operation, bc_file_system_step_identity_check, &compared,
                              bc_file_system_error_kind_symlink_refused);
    }
    if (!same_identity(expected, &actual)) {
        return report_outcome(error, operation, bc_file_system_step_identity_check, &compared,
                              bc_file_system_error_kind_symlink_refused);
    }
    return 0;
}

/* ---- The two halves of a checked open (resolve.h) ----------------------------------------------------------- */

static bool is_final_symlink_choice(int32_t final_symlink) {
    if (final_symlink == bc_file_system_final_symlink_refuse) {
        return true;
    }
    return final_symlink == bc_file_system_final_symlink_follow;
}

int32_t bc_file_system_resolve_expect_entry(bc_file_system_engine *engine, int32_t operation, int parent,
                                            const char *name, int32_t final_symlink,
                                            bc_file_system_expected_entry *expected, bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || parent < 0 || name == nullptr || !is_final_symlink_choice(final_symlink) ||
        expected == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const int flags = final_symlink == bc_file_system_final_symlink_refuse ? AT_SYMLINK_NOFOLLOW : 0;
    struct stat info;
    const bc_file_system_call_outcome outcome =
        bc_file_system_resolve_fstatat(engine, operation, parent, name, flags, &info);
    if (!outcome.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_fstatat, &outcome, kind_of(outcome.code));
    }
    /* The identity fields are compared as bit patterns, so the width and sign of dev_t do not matter. */
    *expected = (bc_file_system_expected_entry){.device = (uint64_t)info.st_dev,
                                                .inode = (uint64_t)info.st_ino,
                                                .is_directory = S_ISDIR(info.st_mode) != 0,
                                                .is_symlink = S_ISLNK(info.st_mode) != 0};
    return 0;
}

int32_t bc_file_system_resolve_open_expected_directory(bc_file_system_engine *engine, int32_t operation, int parent,
                                                       const char *name, int32_t final_symlink,
                                                       const bc_file_system_expected_entry *expected,
                                                       owned_descriptor *opened, bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || parent < 0 || name == nullptr || !is_final_symlink_choice(final_symlink) ||
        expected == nullptr || opened == nullptr || opened->value != -1) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* O_NOFOLLOW is what makes a refusing open refuse: with O_DIRECTORY beside it a symlink answers ENOTDIR on
     * macOS and Linux alike (ELOOP without O_DIRECTORY), which the mapping above reads as the refusal. */
    const int flags =
        final_symlink == bc_file_system_final_symlink_refuse ? directory_open_flags | O_NOFOLLOW : directory_open_flags;
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor candidate = {.value = -1};
    const bc_file_system_call_outcome outcome =
        bc_file_system_resolve_openat(engine, operation, parent, name, flags, &candidate.value);
    if (!outcome.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_openat, &outcome,
                              kind_of_open_failure(outcome.code, expected));
    }
    const int32_t kind = verify_opened_identity(engine, operation, candidate.value, expected, error);
    if (kind != 0) {
        return kind;
    }
    opened->value = release_descriptor(&candidate);
    return 0;
}

/* ---- The walk ----------------------------------------------------------------------------------------------- */

/* Opens every component of `text`, each against the parent the previous one became, so that at the end
 * `resolved->parent` is the last component's descriptor. `text` is the parent text of a valid path: a valid
 * path of its kind, or empty, or the root alone, the last two of which have no components to open. */
static int32_t walk(bc_file_system_engine *engine, int32_t operation, const uint8_t *text, size_t length,
                    int32_t final_symlink, bc_file_system_resolved *resolved, bc_file_system_error *error) {
    size_t cursor = 0;
    size_t offset = 0;
    size_t component_length = 0;
    while (bc_file_system_path_text_next_component(text, length, &cursor, &offset, &component_length)) {
        /* The system call wants the component NUL-terminated: one stack buffer, bounded by NAME_MAX, which the
         * path module has already held every component under. */
        uint8_t name[NAME_MAX + 1] = {};
        memcpy(name, text + offset, component_length);
        bc_file_system_expected_entry expected;
        int32_t kind = bc_file_system_resolve_expect_entry(engine, operation, resolved->parent, (const char *)name,
                                                           final_symlink, &expected, error);
        if (kind != 0) {
            return kind;
        }
        [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor child = {.value = -1};
        kind = bc_file_system_resolve_open_expected_directory(engine, operation, resolved->parent, (const char *)name,
                                                              final_symlink, &expected, &child, error);
        if (kind != 0) {
            return kind;
        }
        /* The previous parent is closed here, so the walk holds one descriptor per level at most once. */
        adopt_descriptor(&resolved->opened, &child);
        resolved->parent = resolved->opened.value;
    }
    return 0;
}

/* ---- The fast path ------------------------------------------------------------------------------------------ */

/* Which one-call open this platform offers, and the step it is reported under. */
static bool has_fast_path(const bc_file_system_platform_fast_paths *fast_paths, int32_t *step) {
    if (fast_paths->has_openat2) {
        *step = bc_file_system_step_openat2;
        return true;
    }
    if (fast_paths->has_openat_nofollow_any) {
        *step = bc_file_system_step_openat_nofollow_any;
        return true;
    }
    return false;
}

/* The whole parent text beneath `held` in one kernel call, bracketed by the same identity check as a component.
 * `*fell_back` says the platform (or a script on the fast path's step) answered that there is no such call
 * here, and nothing has been decided: the caller walks instead. */
static int32_t open_beneath_fast(bc_file_system_engine *engine, int32_t operation, int32_t step, int held,
                                 const uint8_t *text, size_t length, bc_file_system_resolved *resolved, bool *fell_back,
                                 bc_file_system_error *error) {
    *fell_back = false;
    /* The one PATH_MAX buffer of this call; the caller has proved the text fits with its NUL. */
    uint8_t relative[PATH_MAX] = {};
    memcpy(relative, text, length);
    bc_file_system_expected_entry expected;
    const int32_t expected_kind = bc_file_system_resolve_expect_entry(
        engine, operation, held, (const char *)relative, bc_file_system_final_symlink_refuse, &expected, error);
    if (expected_kind != 0) {
        return expected_kind;
    }
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor candidate = {.value = -1};
    const bc_file_system_call_outcome outcome =
        open_beneath_in_one_call(engine, operation, step, held, (const char *)relative, &candidate.value);
    if (!outcome.stopped && means_no_fast_path(outcome.code)) {
        *fell_back = true;
        return 0;
    }
    if (!outcome.succeeded) {
        return report_outcome(error, operation, step, &outcome, kind_of_open_failure(outcome.code, &expected));
    }
    const int32_t kind = verify_opened_identity(engine, operation, candidate.value, &expected, error);
    if (kind != 0) {
        return kind;
    }
    adopt_descriptor(&resolved->opened, &candidate);
    resolved->parent = resolved->opened.value;
    return 0;
}

/* ---- resolve ------------------------------------------------------------------------------------------------ */

/* The location's text is the path module's to judge; the layer checks only what belongs to the location: its
 * kind, and that `beneath` holds an open directory. */
static bool is_resolvable_location(const bc_file_system_location *location) {
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
        if (location->directory->descriptor < 0) {
            return false;
        }
        return bc_file_system_path_text_is_valid_relative(location->path, location->length);
    }
    return false;
}

/* Where the leaf sits in a valid path, and how much of the path spells its parent: the bytes before the leaf
 * without the slash that separates them, except that the root's slash is the parent of a top-level entry. A
 * path with no component (the root) has an empty leaf and is its own parent. */
static void locate_leaf(const uint8_t *path, size_t length, size_t *leaf_offset, size_t *leaf_length,
                        size_t *parent_length) {
    size_t cursor = 0;
    size_t offset = 0;
    size_t component_length = 0;
    *leaf_offset = length;
    *leaf_length = 0;
    while (bc_file_system_path_text_next_component(path, length, &cursor, &offset, &component_length)) {
        *leaf_offset = offset;
        *leaf_length = component_length;
    }
    if (*leaf_offset <= 1) {
        *parent_length = *leaf_offset;
        return;
    }
    *parent_length = *leaf_offset - 1;
}

static int32_t resolve_at(bc_file_system_engine *engine, int32_t operation, const uint8_t *path, size_t parent_length,
                          bc_file_system_resolved *resolved, bc_file_system_error *error) {
    const bc_file_system_call_outcome outcome = open_root(engine, operation, &resolved->opened.value);
    if (!outcome.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_open, &outcome, kind_of(outcome.code));
    }
    resolved->parent = resolved->opened.value;
    /* The header promises that the system resolves an absolute path, symlinks on the way included. */
    return walk(engine, operation, path, parent_length, bc_file_system_final_symlink_follow, resolved, error);
}

static int32_t resolve_beneath(bc_file_system_engine *engine, int32_t operation, int held, const uint8_t *path,
                               size_t parent_length, bc_file_system_resolved *resolved, bc_file_system_error *error) {
    /* Where a directory descriptor is a path string (Emscripten) a held directory enforces nothing, and the
     * library refuses rather than weakens the guarantee: every `beneath` location is unsupported there. */
    if (!bc_file_system_platform_enforces_containment()) {
        return refuse(error, bc_file_system_error_kind_unsupported, operation);
    }
    resolved->parent = held;
    if (parent_length == 0) {
        return 0;
    }
    int32_t fast_step = 0;
    if (has_fast_path(bc_file_system_engine_fast_paths(engine), &fast_step) && parent_length < PATH_MAX) {
        bool fell_back = false;
        const int32_t kind =
            open_beneath_fast(engine, operation, fast_step, held, path, parent_length, resolved, &fell_back, error);
        if (!fell_back) {
            return kind;
        }
    }
    return walk(engine, operation, path, parent_length, bc_file_system_final_symlink_refuse, resolved, error);
}

void bc_file_system_resolved_close(bc_file_system_resolved *resolved) {
    close_owned_descriptor(&resolved->opened);
}

int32_t bc_file_system_resolve(bc_file_system_engine *engine, int32_t operation,
                               const bc_file_system_location *location, bc_file_system_resolved *resolved,
                               bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || !is_resolvable_location(location) || resolved == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t leaf_offset = 0;
    size_t leaf_length = 0;
    size_t parent_length = 0;
    locate_leaf(location->path, location->length, &leaf_offset, &leaf_length, &parent_length);

    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved candidate = bc_file_system_resolved_empty();
    memcpy(candidate.leaf, location->path + leaf_offset, leaf_length);
    candidate.leaf_length = leaf_length;
    int32_t kind = 0;
    if (location->kind == bc_file_system_location_kind_at) {
        kind = resolve_at(engine, operation, location->path, parent_length, &candidate, error);
    } else {
        kind = resolve_beneath(engine, operation, location->directory->descriptor, location->path, parent_length,
                               &candidate, error);
    }
    if (kind != 0) {
        return kind;
    }
    /* Written only now, on success: the caller's struct is untouched by every failing path above. */
    *resolved = bc_file_system_resolved_empty();
    memcpy(resolved->leaf, candidate.leaf, candidate.leaf_length);
    resolved->leaf_length = candidate.leaf_length;
    resolved->parent = candidate.parent;
    resolved->opened.value = release_descriptor(&candidate.opened);
    return 0;
}
