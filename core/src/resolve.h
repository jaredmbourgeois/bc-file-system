/* resolve.h — the resolution layer: a location becomes (parent descriptor, leaf name), once, with containment.
 *
 * Every operation from the status-and-reads stage on starts here: it resolves its location to the descriptor of
 * the leaf's parent directory and the leaf's name, and from then on makes only `*at` calls on that pair, so
 * what was checked is what is then used. The layer never opens the leaf: the operation does, with the
 * `final_symlink` choice it was given. The security of a `beneath` location is this file's: every component of
 * the relative path is opened against the held parent with `O_DIRECTORY | O_NOFOLLOW`, so no component may be a
 * symlink and nothing above the held directory is reachable (`..` never reaches here: the path module refused
 * it), and each open is bracketed by an identity check, `fstatat` before and `fstat` after, which is the only
 * thing that catches a directory swapped for another directory between the two. Where the platform offers it,
 * one kernel call does the whole walk (`openat2` with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS` on Linux,
 * `openat` with `O_NOFOLLOW_ANY` on Apple), reported as its own step so a test can refuse it and force the walk.
 * An `at` location is resolved by the same walk from the root, following symlinks on the way, because the
 * header promises that the system resolves an absolute path; its walk is bracketed the same way so there is
 * one shape, one mapping and one test. The functions have external linkage under the library's prefix so the
 * C-only test in `core/tests` can drive them; they are declared only here. The errno table and the gated
 * `fstatat`, `fstat` and `openat` at the bottom are this unit's too, shared with the operations so the core has
 * one mapping and one loop per system call. Private to `core/src`: C23, never a common subset.
 */
#pragma once

#include <sys/stat.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include <bc_file_system/bc_file_system.h>

#include "engine.h"
#include "owned.h"

/* The directory handle the public header keeps opaque: a descriptor, and the text it was opened with so a
 * `beneath` error can be reported as that text, a slash and the relative path. Owned by this layer because the
 * walk beneath a directory starts from its descriptor; created and released by `bc_file_system.c`. */
struct bc_file_system_directory {
    int descriptor; /* open O_RDONLY | O_DIRECTORY | O_CLOEXEC; -1 never once the handle has been handed out */
    size_t opened_as_length;
    uint8_t opened_as[]; /* exactly the bytes the location was opened with, not NUL-terminated */
};

/* A location, resolved: the leaf's parent and the leaf's name. Declared with
 * `[[gnu::cleanup(bc_file_system_resolved_close)]]` so the descriptor the walk opened is closed on every path. */
typedef struct bc_file_system_resolved {
    /* The leaf's parent directory. Borrowed: it is `opened` below, or the held directory's own descriptor when
     * the leaf sits directly inside it and nothing had to be opened. */
    int parent;
    /* What the walk opened for `parent`; -1 when `parent` is the held directory's. For the root (`at("/")`)
     * this is the root itself and the leaf is empty: the parent is the location. */
    owned_descriptor opened;
    /* The last component, NUL-terminated for the `*at` call that acts on it. Empty only for the root. */
    uint8_t leaf[NAME_MAX + 1];
    size_t leaf_length;
} bc_file_system_resolved;

/* What `fstatat` recorded about an entry before it was opened, to compare with what the open answered. */
typedef struct bc_file_system_expected_entry {
    uint64_t device;   /* st_dev */
    uint64_t inode;    /* st_ino */
    bool is_directory; /* S_ISDIR, so an ENOTDIR from the open means the entry was swapped, not that it is a file */
    bool is_symlink;   /* S_ISLNK, under a refusing stat */
} bc_file_system_expected_entry;

/* A resolved location holding nothing: how one is declared before `bc_file_system_resolve` fills it, so the
 * cleanup below finds nothing to close on a failing path. A zeroed struct would name descriptor 0 instead. */
static inline bc_file_system_resolved bc_file_system_resolved_empty(void) {
    return (bc_file_system_resolved){.parent = -1, .opened = {.value = -1}, .leaf = {}, .leaf_length = 0};
}

/* The cleanup of a resolved location: closes what the walk opened, never the held directory's descriptor. */
void bc_file_system_resolved_close(bc_file_system_resolved *resolved);

/* Resolves `location` for `operation`, which has already validated its arguments and announced itself to the
 * observer: every system call here goes through `before_step` under that operation's name, and the error names
 * it. On success `*resolved` holds the parent and the leaf; on failure it is untouched and `*error` is filled
 * with an empty name, since what failed to resolve is the location itself, whose text the face holds. The
 * refusals: a component that is or became a symlink, or an entry swapped between the stat and the open, is
 * `symlink_refused`; a missing component `not_found`; a component that is not a directory `not_directory`; a
 * platform that cannot enforce containment (Emscripten, where a directory descriptor is a path string)
 * answers `unsupported` for every `beneath` location before any system call.
 *
 * `engine`    must not be NULL.
 * `location`  must not be NULL; a valid location (the path module's rules; `beneath` with an open directory).
 * `resolved`  must not be NULL; written only on success.
 * `error`     must not be NULL. */
int32_t bc_file_system_resolve(bc_file_system_engine *engine, int32_t operation,
                               const bc_file_system_location *location, bc_file_system_resolved *resolved,
                               bc_file_system_error *error);

/* The two halves of one checked open of a directory entry, which the walk performs for every component and an
 * operation performs for a leaf it opens as a directory. They are two functions so the C-only test can swap the
 * entry on disk between them, which is the exact interleaving the identity check exists for.
 *
 * The first half records the entry's identity with `fstatat` (step `fstatat`): under `refuse` the entry itself,
 * never what a symlink points at; under `follow` what it points at. A failure is mapped and names nothing.
 *
 * `parent`    an open directory descriptor, borrowed.
 * `name`      one component, NUL-terminated; must not be NULL.
 * `expected`  must not be NULL; written only on success. */
int32_t bc_file_system_resolve_expect_entry(bc_file_system_engine *engine, int32_t operation, int parent,
                                            const char *name, int32_t final_symlink,
                                            bc_file_system_expected_entry *expected, bc_file_system_error *error);

/* The second half opens the entry as a directory (`O_RDONLY | O_DIRECTORY | O_CLOEXEC`, plus `O_NOFOLLOW` under
 * `refuse`; step `openat`), reads the opened descriptor's identity (step `fstat`) and compares it with
 * `expected` (step `identity_check`): a mismatch is `symlink_refused`, and so is an open refused with ELOOP or
 * ENOTDIR for an entry the stat had seen as a directory or a symlink. Stores the descriptor in `*opened`, which
 * must hold none (-1) on entry and is untouched on failure. */
int32_t bc_file_system_resolve_open_expected_directory(bc_file_system_engine *engine, int32_t operation, int parent,
                                                       const char *name, int32_t final_symlink,
                                                       const bc_file_system_expected_entry *expected,
                                                       owned_descriptor *opened, bc_file_system_error *error);

/* ---- The errno mapping and the gated system calls shared with the operations ------------------------------- */

/* The kind each errno decides, from the one table of the core: ENOENT `not_found`, ENOTDIR `not_directory`,
 * ELOOP `symlink_refused`, EACCES, EPERM and EROFS `permission_denied`, ENAMETOOLONG `invalid_argument`, EMFILE,
 * ENFILE and ENOMEM `resource_exhausted`, and `io_failed` for everything else, with the errno kept beside it.
 * An open of a directory entry reads ENOTDIR against what its stat had seen before coming here. */
int32_t bc_file_system_resolve_kind_of_errno(int system_code);

/* One system call each, behind the observer, under `operation`'s name and the step the call is named by:
 * every attempt asks `before_step` first, a scripted failure is the call's errno without the call, EINTR is
 * asked again, and the errno is copied on the line after the call. The answer says whether the call succeeded,
 * failed with which code, or was the point the observer stopped the operation at; the out-parameter is written
 * only when it succeeded. `name` is one component, NUL-terminated; `parent` and `descriptor` are borrowed. */
bc_file_system_call_outcome bc_file_system_resolve_fstatat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                           const char *name, int flags, struct stat *info);
bc_file_system_call_outcome bc_file_system_resolve_fstat(bc_file_system_engine *engine, int32_t operation,
                                                         int descriptor, struct stat *info);
bc_file_system_call_outcome bc_file_system_resolve_openat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                          const char *name, int flags, int *descriptor);
