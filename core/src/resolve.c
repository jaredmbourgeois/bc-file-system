/* resolve.c — the bodies behind resolve.h: the one open of an `at` parent, the walk, the fast paths, the identity
 * check, the errno mapping.
 *
 * Two resolutions, one per kind of location. An `at` location is the system's to resolve, symlinks on the way
 * included, so its parent text is opened by path in one call (for search only: O_SEARCH on Apple platforms,
 * O_PATH on Linux, the platform's flags, so a directory the process may enter but not list is a parent like any
 * other), and nothing is checked around the open. A `beneath` location is the library's to contain, so each of
 * its opens has one shape: `fstatat` records what is there, `openat` opens it as a directory, `fstat` reads what
 * was opened, and the two identities are compared. The walk is that shape in a loop, holding one parent
 * descriptor at a time and closing the previous one as it goes; the fast path is that shape once, with the
 * platform's one-call open in the middle. Every system call goes through calls.h (the two opens that are this
 * file's own, a text by path and the platform's one-call open, keep the same shape here), so it is behind the
 * observer's `before_step`, asked again on EINTR, with the errno copied on the line after the call, and its
 * failure is read with the one errno table of the core. Nothing here opens a leaf, reads a byte or writes one;
 * nothing allocates.
 */
#include "resolve.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bc_file_system/bc_file_system.h>

#include "calls.h"
#include "engine.h"
#include "owned.h"
#include "path.h"
#include "platform.h"

/* ---- Constants ---------------------------------------------------------------------------------------------- */

enum : int32_t {
    /* How a directory is opened to read its entries: never for writing, never inherited by a child process. */
    directory_read_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC
};

/* How a directory is opened for `access`: to walk through it, the platform's search-only flags (O_SEARCH,
 * O_PATH), so that a directory the process may enter but not list is a parent like any other; to list it,
 * read-only, since fdopendir takes nothing less. */
static int open_flags_for(int32_t access) {
    if (access == bc_file_system_directory_access_read) {
        return directory_read_flags;
    }
    return bc_file_system_platform_directory_search_open_flags();
}

/* ---- The errno mapping -------------------------------------------------------------------------------------- */

static int32_t kind_of(int system_code) {
    return bc_file_system_call_kind_of_errno(system_code);
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

/* ---- The two opens of this file's own, in the shape of calls.c's ----------------------------------------- */

/* The one open by path in the core: an `at` location's parent text, absolute and NUL-terminated, opened for
 * search with every symlink on the way followed, as the system resolves any path. */
static bc_file_system_call_outcome open_by_path(bc_file_system_engine *engine, int32_t operation, const char *text,
                                                int *descriptor) {
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
        const int opened = open(text, open_flags_for(bc_file_system_directory_access_search));
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
        const int code = bc_file_system_platform_open_beneath(
            held, relative, bc_file_system_platform_directory_search_open_flags(), descriptor);
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
    const bc_file_system_call_outcome read = bc_file_system_call_fstat(engine, operation, descriptor, &actual);
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

static bool is_directory_access_choice(int32_t access) {
    if (access == bc_file_system_directory_access_search) {
        return true;
    }
    return access == bc_file_system_directory_access_read;
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
        bc_file_system_call_fstatat(engine, operation, parent, name, flags, &info);
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

/* The search permission a platform's search-only open did not check: `faccessat(opened, ".", X_OK)` as its own
 * step, so a directory the process may not search answers permission_denied on every platform at the same point,
 * the open. A platform whose open checks it makes no call. */
static int32_t verify_search_permission(bc_file_system_engine *engine, int32_t operation, int opened,
                                        bc_file_system_error *error) {
    if (bc_file_system_platform_search_open_checks_permission()) {
        return 0;
    }
    const bc_file_system_call_outcome outcome = bc_file_system_call_faccessat(
        engine, operation, opened, ".", X_OK, bc_file_system_platform_access_check_flags());
    if (outcome.succeeded) {
        return 0;
    }
    return report_outcome(error, operation, bc_file_system_step_faccessat, &outcome,
                          bc_file_system_call_kind_of_errno(outcome.code));
}

int32_t bc_file_system_resolve_open_expected_directory(bc_file_system_engine *engine, int32_t operation, int parent,
                                                       const char *name, int32_t final_symlink, int32_t access,
                                                       const bc_file_system_expected_entry *expected,
                                                       owned_descriptor *opened, bc_file_system_error *error) {
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (engine == nullptr || parent < 0 || name == nullptr || !is_final_symlink_choice(final_symlink) ||
        !is_directory_access_choice(access) || expected == nullptr || opened == nullptr || opened->value != -1) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* O_NOFOLLOW is what makes a refusing open refuse: with O_DIRECTORY beside it a symlink answers ENOTDIR on
     * macOS and Linux alike (ELOOP without O_DIRECTORY), which the mapping above reads as the refusal. */
    const int flags = final_symlink == bc_file_system_final_symlink_refuse ? open_flags_for(access) | O_NOFOLLOW
                                                                           : open_flags_for(access);
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor candidate = {.value = -1};
    const bc_file_system_call_outcome outcome =
        bc_file_system_call_openat(engine, operation, parent, name, flags, &candidate.value);
    if (!outcome.succeeded) {
        return report_outcome(error, operation, bc_file_system_step_openat, &outcome,
                              kind_of_open_failure(outcome.code, expected));
    }
    const int32_t kind = verify_opened_identity(engine, operation, candidate.value, expected, error);
    if (kind != 0) {
        return kind;
    }
    if (access == bc_file_system_directory_access_search) {
        const int32_t permission = verify_search_permission(engine, operation, candidate.value, error);
        if (permission != 0) {
            return permission;
        }
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
                                                              final_symlink, bc_file_system_directory_access_search,
                                                              &expected, &child, error);
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

/* ---- The pieces of an `at` parent text ---------------------------------------------------------------------- */

enum : size_t {
    /* The longest text one by-path open takes: the system refuses PATH_MAX bytes with their NUL, so a piece is at
     * most PATH_MAX - 1 bytes (1023 on Apple platforms, 4095 on Linux; a component is at most NAME_MAX, 255, so
     * every piece holds at least one). */
    at_piece_capacity = PATH_MAX - 1,
    /* The limit that admits one component and no more, since a piece always takes its first. */
    one_component_only = 0
};

/* Where the piece of `text` (a valid absolute path of `length` bytes) that begins at `start` ends: as many whole
 * components as keep the piece within `limit` bytes, and always at least one, so every piece moves the
 * resolution on. The first piece begins at 0 and carries the text's own slash; a later one begins at the first
 * byte of a component, after the slash that ended the piece before it. A text with no component, the root, is
 * one piece: its slash. */
static size_t piece_end_of(const uint8_t *text, size_t length, size_t start, size_t limit) {
    size_t end = start == 0 ? 1 : start;
    bool took_a_component = false;
    size_t cursor = start;
    size_t offset = 0;
    size_t component_length = 0;
    while (bc_file_system_path_text_next_component(text, length, &cursor, &offset, &component_length)) {
        const size_t candidate = offset + component_length;
        if (took_a_component && candidate - start > limit) {
            break;
        }
        end = candidate;
        took_a_component = true;
    }
    return end;
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

/* Resolves an `at` location's parent text as the system resolves any absolute path, which is what the header
 * promises: by path, every symlink on the way followed, in one call (step `open`) where the text fits the
 * system's limit. No stat precedes the open and no identity check follows it: a component swapped between the
 * two would be one the system's own resolution follows as well, so the bracket guards nothing `at` promises,
 * and the window `beneath` closes is not closed here. The root, `at("/")`, opens `/` as its own parent.
 *
 * A text one call cannot take (PATH_MAX bytes or more, the NUL counted: 1024 on Apple platforms, 4096 on Linux)
 * is opened piece after piece, each as many whole components as fit, beneath the descriptor the piece before it
 * became (step `openat`): the kernel resolves each piece as it would have resolved that stretch of the whole
 * text, so the answer is the one a system without the limit would give, at one call per PATH_MAX bytes rather
 * than one per component. A piece the system answers ENAMETOOLONG for is one in which a symlink's text, joined
 * to the rest of the piece, outgrew what the system resolves in one call (Apple platforms bound that expansion
 * by PATH_MAX; Linux does not). That piece is opened again one component at a time, where a symlink's text is
 * joined to nothing after it, so `at` still resolves every path whose links each fit, as the walk it replaces
 * did.
 *
 * The errno table of the by-path open, `kind_of`'s with nothing added, since every answer the open can give is
 * one the table already reads: ENOENT `not_found` (a component, or a symlink's target, is missing); ENOTDIR
 * `not_directory` (a file in the middle, or at the end: the system saw it, no stat preceded the open); ELOOP
 * `symlink_refused` (a loop, or more symlinks than the system follows); EACCES `permission_denied` (a directory
 * on the way the process may not search); ENAMETOOLONG `invalid_argument`, only for a single component the
 * system refuses (a name longer than the volume's own limit), since a longer text is opened in pieces and a
 * piece of several components is opened again one at a time; EMFILE, ENFILE and ENOMEM `resource_exhausted`.
 *
 * On a platform whose search-only open checks no permission (Linux's O_PATH) the last directory opened is asked
 * for search permission by a step of its own (`faccessat`), as the walk's opens are; a piece after the first is
 * looked up beneath the one before it, which the system refuses without search permission there, so the
 * earlier ones need no step. */
static int32_t resolve_at(bc_file_system_engine *engine, int32_t operation, const uint8_t *path, size_t parent_length,
                          bc_file_system_resolved *resolved, bc_file_system_error *error) {
    /* The one PATH_MAX buffer of this call: each piece, NUL-terminated for the system. */
    uint8_t piece[PATH_MAX] = {};
    size_t start = 0;
    /* Where a piece the system refused as too long ended: up to there, one component per call. */
    size_t one_component_at_a_time_until = 0;
    for (;;) {
        const size_t limit = start < one_component_at_a_time_until ? one_component_only : at_piece_capacity;
        const size_t end = piece_end_of(path, parent_length, start, limit);
        assert(end - start < sizeof piece);
        memcpy(piece, path + start, end - start);
        piece[end - start] = 0;
        /* The first piece begins the absolute text and is opened by path; a later one is relative to the last. */
        [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
        int32_t step = bc_file_system_step_open;
        bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
        if (start == 0) {
            outcome = open_by_path(engine, operation, (const char *)piece, &opened.value);
        } else {
            step = bc_file_system_step_openat;
            outcome = bc_file_system_call_openat(engine, operation, resolved->parent, (const char *)piece,
                                                 open_flags_for(bc_file_system_directory_access_search), &opened.value);
        }
        if (!outcome.succeeded) {
            const bool several_components = piece_end_of(path, parent_length, start, one_component_only) < end;
            if (!outcome.stopped && outcome.code == ENAMETOOLONG && several_components) {
                one_component_at_a_time_until = end;
                continue;
            }
            return report_outcome(error, operation, step, &outcome, kind_of(outcome.code));
        }
        /* The previous piece's directory is closed here: one descriptor is held at a time. */
        adopt_descriptor(&resolved->opened, &opened);
        resolved->parent = resolved->opened.value;
        if (end >= parent_length) {
            break;
        }
        start = end + 1;
    }
    return verify_search_permission(engine, operation, resolved->parent, error);
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
