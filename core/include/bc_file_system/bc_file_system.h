/* bc_file_system.h — the one public header of bc-file-system: a file system that code is handed.
 *
 * This header is the ABI every face (C++, Swift, Rust) is built against, so it is written in the common subset
 * every consumer's parser accepts: strict C11, gnu11 as Swift's importer reads it (Objective-C on Apple), C++20
 * and C++23. Declarations only; fixed-width integers; opaque handles; enums with explicit values; no attribute,
 * no `nullptr`, no `constexpr`, no compound literal. The C23 the core is written in stays in the `.c` files.
 *
 * THE ONE RETURN CONVENTION. Every function that can fail returns `int32_t`, which is the error kind it failed
 * with (a `bc_file_system_error_kind`) or 0 on success, and takes a `bc_file_system_error *error` as its last
 * parameter. The caller allocates the error struct. On success the whole struct is zeroed. On failure it is
 * filled, the return value equals `error->kind`, and every out-parameter is left untouched. There is no second
 * convention: no function returns a count, a durability or a flag in its return value.
 *
 * CONTRACT VIOLATIONS. NULL where the contract below forbids it, an enum field holding zero or a value outside
 * its enum, a NUL byte inside a path, a mode with bits outside 07777: the call answers
 * `bc_file_system_error_kind_invalid_argument` and touches nothing. Nothing in this library aborts, exits or
 * installs a signal handler; the host decides what a programming error costs. If `error` itself is NULL the
 * call returns `bc_file_system_error_kind_invalid_argument` and writes nowhere.
 *
 * ANSWERS OF VARIABLE LENGTH go into a buffer the caller owns, under one protocol: `uint8_t *buffer,
 * size_t capacity, size_t *length`. On success `*length` is how many bytes were stored. When the answer does not
 * fit the call fails with `bc_file_system_error_kind_buffer_too_small` and `*length` is the capacity needed, so the
 * caller grows the buffer and calls again. A NULL buffer with a capacity of 0 is the stated way to ask for the
 * size first. Nothing is ever cut short silently. A fixed-size answer is an out-parameter.
 *
 * ENUMS cross this boundary as `int32_t` fields and parameters. Zero is never a valid input value for any of
 * them, so a zeroed options struct is detectably unfilled rather than a silent choice. Every enum is closed: a
 * new value is a version bump for every face. Every enumerator is spelled as its enum's name followed by the
 * value (`bc_file_system_error_kind_not_found`), so a generator strips one prefix, and no enumerator can share
 * a spelling with a function or a struct, which C would reject.
 *
 * PATHS AND NAMES cross as `(pointer, length)` bytes, never NUL-terminated text, UTF-8 by convention and never
 * validated or normalised here: the bytes go to the system unchanged.
 *
 * POINTERS. The header carries no nullability attribute (GCC rejects them); the contract of every pointer
 * parameter is the comment at its declaration. A pointer parameter is a borrow for the duration of the call
 * unless its comment says otherwise. What the library allocates, the library frees: every handle has a release.
 *
 * THREADS. The library spawns none and holds no mutable global. A live engine is immutable after creation and
 * may be used from any number of threads at once. An engine created for tests records every step it takes, so
 * it is used from one thread at a time. A directory or lock handle is used from one thread at a time.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Handles ---------------------------------------------------------------------------------------------- */

/* The context every operation runs in: the platform's probed fast paths and, in a test build, the observer.
 * Created at a composition root by one of the two factories below; released once. */
typedef struct bc_file_system_engine bc_file_system_engine;

/* An open directory that remembers the text it was opened with, so that an error beneath it can be reported as
 * that text, a slash and the relative path. The kernel enforces containment for everything addressed beneath
 * it. */
typedef struct bc_file_system_directory bc_file_system_directory;

/* An advisory lock held on a file; released by `bc_file_system_lock_release`, which also releases the lock. */
typedef struct bc_file_system_lock bc_file_system_lock;

/* ---- Enums ------------------------------------------------------------------------------------------------- */

/* Why a call failed: fourteen decisions a caller can act on, never a passthrough of errno, plus the C face's
 * grow-and-retry signal. `error.system_code` keeps the exact errno for the log. The first six equal
 * bourgeois_concern's `FileIoError::Kind` 1 to 6 by name and value. */
typedef enum bc_file_system_error_kind {
    /* Nothing exists at the location. */
    bc_file_system_error_kind_not_found = 1,
    /* Something exists where a regular file is required, and it is not one. */
    bc_file_system_error_kind_not_regular_file = 2,
    /* Something exists where a directory is required, and it is not one. */
    bc_file_system_error_kind_not_directory = 3,
    /* EACCES, EPERM, and EROFS: a read-only volume refuses the same way a mode does. */
    bc_file_system_error_kind_permission_denied = 4,
    /* The destination holds something the call was told not to replace. */
    bc_file_system_error_kind_already_exists = 5,
    /* A directory that must be empty to be removed or replaced is not. */
    bc_file_system_error_kind_directory_not_empty = 6,
    /* A symlink sat where the call was told not to follow one: at the end of the path, or anywhere along a path
     * resolved beneath a directory. */
    bc_file_system_error_kind_symlink_refused = 7,
    /* The file is larger than a read limit, than `size_t` on this target, or (EFBIG) than the process's file-size
     * limit, which is reached only on a host that ignores SIGXFSZ — this library never installs that handling. */
    bc_file_system_error_kind_too_large = 8,
    /* EXDEV: the two locations of a rename are on different devices. */
    bc_file_system_error_kind_cross_device = 9,
    /* ENOSPC and EDQUOT: the disk or the quota is full. Never memory. */
    bc_file_system_error_kind_no_space = 10,
    /* ENOMEM, EMFILE, ENFILE: memory or descriptors ran out. */
    bc_file_system_error_kind_resource_exhausted = 11,
    /* The platform cannot keep the guarantee the call asks for, and the library refuses rather than weakens it. */
    bc_file_system_error_kind_unsupported = 12,
    /* A contract violation; nothing was touched. */
    bc_file_system_error_kind_invalid_argument = 13,
    /* Any other system failure; `error.system_code` says which. */
    bc_file_system_error_kind_io_failed = 14,
    /* The caller's buffer cannot hold the answer, and `*length` is the capacity needed. */
    bc_file_system_error_kind_buffer_too_small = 15
} bc_file_system_error_kind;

/* Which public function an error came from, in this header's order. 28 and 29 are the path module's two
 * validators (`bc_file_system_relative_path_validate`, `bc_file_system_absolute_path_validate`), named for the
 * face constructors that call them; 30 onward are the rest of the module. A later stage may append a value;
 * none is ever renumbered. */
typedef enum bc_file_system_operation {
    bc_file_system_operation_engine_create_live = 1,
    bc_file_system_operation_engine_create_observed_for_tests = 2,
    bc_file_system_operation_open_directory = 3,
    bc_file_system_operation_directory_opened_as = 4,
    bc_file_system_operation_observed_fault_add = 5,
    bc_file_system_operation_observed_faults_clear = 6,
    bc_file_system_operation_observed_step_count = 7,
    bc_file_system_operation_status = 8,
    bc_file_system_operation_read_file = 9,
    bc_file_system_operation_write_file = 10,
    bc_file_system_operation_append = 11,
    bc_file_system_operation_sync = 12,
    bc_file_system_operation_create_directory = 13,
    bc_file_system_operation_create_directories = 14,
    bc_file_system_operation_list_directory = 15,
    bc_file_system_operation_remove_file = 16,
    bc_file_system_operation_remove_directory = 17,
    bc_file_system_operation_remove_tree = 18,
    bc_file_system_operation_create_temporary_directory = 19,
    bc_file_system_operation_rename = 20,
    bc_file_system_operation_copy_file = 21,
    bc_file_system_operation_set_permissions = 22,
    bc_file_system_operation_create_symlink = 23,
    bc_file_system_operation_read_symlink = 24,
    bc_file_system_operation_try_lock = 25,
    bc_file_system_operation_condemn = 26,
    bc_file_system_operation_remove_condemned = 27,
    bc_file_system_operation_relative_path_from = 28,
    bc_file_system_operation_absolute_path_from = 29,
    bc_file_system_operation_path_join = 30,
    bc_file_system_operation_path_parent = 31,
    bc_file_system_operation_path_last_component = 32,
    bc_file_system_operation_path_next_component = 33,
    bc_file_system_operation_path_stem = 34,
    bc_file_system_operation_path_extension = 35,
    bc_file_system_operation_path_relate = 36,
    bc_file_system_operation_path_lexically_normalize = 37,
    bc_file_system_operation_path_resolve_against = 38,
    bc_file_system_operation_path_library_name = 39
} bc_file_system_operation;

/* The system call an operation was making when it failed, and the observer's vocabulary: one enumerator per
 * system call the engine makes. `error.step` is 0 for a refusal made before any system call. Later stages may
 * append a step; none is ever renumbered. */
typedef enum bc_file_system_step {
    bc_file_system_step_open = 1,
    bc_file_system_step_openat = 2,
    bc_file_system_step_fstat = 3,
    bc_file_system_step_fstatat = 4,
    bc_file_system_step_read = 5,
    bc_file_system_step_pread = 6,
    bc_file_system_step_write = 7,
    bc_file_system_step_fsync = 8,
    /* fcntl(F_FULLFSYNC): the durable sync on Apple platforms, where fsync stops at the drive's cache. */
    bc_file_system_step_fcntl_fullfsync = 9,
    bc_file_system_step_renameat = 10,
    /* The platform's no-replace rename: renameatx_np(RENAME_EXCL), renameat2(RENAME_NOREPLACE), SYS_renameat2. */
    bc_file_system_step_rename_no_replace = 11,
    bc_file_system_step_unlinkat = 12,
    bc_file_system_step_mkdirat = 13,
    bc_file_system_step_fdopendir = 14,
    bc_file_system_step_readdir = 15,
    bc_file_system_step_symlinkat = 16,
    bc_file_system_step_readlinkat = 17,
    bc_file_system_step_flock = 18,
    bc_file_system_step_fchmod = 19,
    /* copy_file_range, or the read-and-write loop where the kernel offers no copy. */
    bc_file_system_step_copy_range = 20,
    /* clonefileat, fclonefileat, FICLONE. */
    bc_file_system_step_clone_file = 21,
    bc_file_system_step_close = 22,
    /* The st_dev/st_ino comparison that catches a directory swapped in between a stat and an open. */
    bc_file_system_step_identity_check = 23,
    /* The Linux fast path of a `beneath` location: openat2 with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS over the
     * whole relative path in one call. A scripted ENOSYS, EINVAL, ENOTSUP or EOPNOTSUPP on this step makes the
     * engine walk the path one component at a time instead, which is how a test forces the portable path. */
    bc_file_system_step_openat2 = 24,
    /* The Apple fast path of a `beneath` location: openat with O_NOFOLLOW_ANY over the whole relative path in
     * one call. The same scripted errnos make the engine walk instead. */
    bc_file_system_step_openat_nofollow_any = 25
} bc_file_system_step;

/* What to do when the last component of a path is a symlink. */
typedef enum bc_file_system_final_symlink {
    /* The symlink itself is the entry: it is reported, removed or refused, never followed. */
    bc_file_system_final_symlink_refuse = 1,
    /* The operation acts on what the symlink points at. */
    bc_file_system_final_symlink_follow = 2
} bc_file_system_final_symlink;

/* What to do when a mutation's destination already holds something. */
typedef enum bc_file_system_existing_destination {
    bc_file_system_existing_destination_replace = 1,
    /* `already_exists`, and the destination is untouched. */
    bc_file_system_existing_destination_refuse = 2
} bc_file_system_existing_destination;

/* How much of a file a read may take. */
typedef enum bc_file_system_read_limit_kind {
    bc_file_system_read_limit_kind_whole_file = 1,
    /* At most `at_most` bytes; a larger file is `too_large`, never a silent prefix. */
    bc_file_system_read_limit_kind_at_most = 2
} bc_file_system_read_limit_kind;

/* How far a change must travel before the call returns, and how far it got. A call is told the durability it
 * needs and answers with the one it achieved: once a change is visible, a sync that fails lowers the answer to
 * `written` rather than raising an error, because the change happened. */
typedef enum bc_file_system_durability {
    /* Handed to the operating system; outlives the process; a power loss can still lose it. */
    bc_file_system_durability_written = 1,
    /* On stable storage: the file and its directory synced (F_FULLFSYNC on Apple, fsync elsewhere). */
    bc_file_system_durability_durable = 2
} bc_file_system_durability;

/* What the mode beside the policy means for a file that replaces an existing one. Anything newly created gets
 * the mode under both. Modes are applied exactly by fchmod; the umask is never consulted. */
typedef enum bc_file_system_permissions_policy {
    /* A replacing file gets the mode too. */
    bc_file_system_permissions_policy_exactly = 1,
    /* A replacing file keeps the mode the replaced file had, so an operator's chmod survives a rewrite. */
    bc_file_system_permissions_policy_preserve_or = 2
} bc_file_system_permissions_policy;

/* How a location addresses its target. */
typedef enum bc_file_system_location_kind {
    /* An absolute path the system resolves. Relative text is refused with `invalid_argument`. */
    bc_file_system_location_kind_at = 1,
    /* A relative path resolved beneath an open directory with containment enforced by the kernel: no component
     * may be a symlink, and nothing above the directory is reachable. */
    bc_file_system_location_kind_beneath = 2
} bc_file_system_location_kind;

/* What an entry is, judged without following a symlink. */
typedef enum bc_file_system_entry_type {
    bc_file_system_entry_type_regular_file = 1,
    bc_file_system_entry_type_directory = 2,
    bc_file_system_entry_type_symlink = 3,
    /* A socket, a FIFO, a device. */
    bc_file_system_entry_type_other = 4
} bc_file_system_entry_type;

/* The answer of `bc_file_system_try_lock`. */
typedef enum bc_file_system_lock_outcome {
    bc_file_system_lock_outcome_acquired = 1,
    /* Another process or descriptor holds the lock; nothing was waited for. */
    bc_file_system_lock_outcome_held_elsewhere = 2
} bc_file_system_lock_outcome;

/* What a scripted fault does when its step is reached. */
typedef enum bc_file_system_fault_action {
    /* The step fails with the fault's `system_code`, exactly as if the system had failed the call. */
    bc_file_system_fault_action_fail = 1,
    /* The operation returns from the step without any cleanup, as if the process had been killed there. */
    bc_file_system_fault_action_stop = 2
} bc_file_system_fault_action;

/* Where one path stands with respect to another, lexically (`bc_file_system_path_relate`). Advisory: a lexical
 * answer knows nothing of symlinks or the disk; `beneath` is what guards an access. */
typedef enum bc_file_system_path_relation {
    /* The path names something strictly beneath the other. */
    bc_file_system_path_relation_inside = 1,
    bc_file_system_path_relation_same = 2,
    /* Neither beneath the other nor the other itself; the other being beneath the path is `outside` too. */
    bc_file_system_path_relation_outside = 3,
    /* One path is absolute and the other relative, or one of them climbs out of its own start with `..`. */
    bc_file_system_path_relation_undecidable = 4
} bc_file_system_path_relation;

/* What a component spells when it spells a library file (`bc_file_system_path_library_name`). */
typedef enum bc_file_system_library_kind {
    /* Not one of the platform spellings. */
    bc_file_system_library_kind_none = 1,
    /* `libX.so`, `libX.so.1.2`, `libX.dylib`, `X.dll`. */
    bc_file_system_library_kind_shared = 2,
    /* `libX.a`. */
    bc_file_system_library_kind_static = 3,
    /* `X.framework`. */
    bc_file_system_library_kind_framework = 4
} bc_file_system_library_kind;

/* ---- Structs ----------------------------------------------------------------------------------------------- */

/* Why a call failed: enums and integers only. The face that holds the location renders the text and composes
 * the full path; the core never formats. Caller-allocated; zeroed on success; filled on failure. */
typedef struct bc_file_system_error {
    int32_t kind;           /* a bc_file_system_error_kind; 0 is success */
    int32_t operation;      /* the bc_file_system_operation that failed */
    int32_t step;           /* the bc_file_system_step that failed, or 0 when no call failed: a refusal before any
                               system call, or an answer decided from what a call reported (not a regular file,
                               too large, a buffer too small) */
    int32_t system_code;    /* the errno, copied on the line after the failing call, or 0 */
    uint32_t name_length;   /* how many bytes of `name` are meaningful */
    uint8_t name_truncated; /* 1 when the entry's name did not fit in `name` */
    uint8_t name[256];      /* the entry the step acted on, relative to the location; empty when it was the
                               location itself */
} bc_file_system_error;

/* Where an operation acts. Built with one of the two constructors below. */
typedef struct bc_file_system_location {
    int32_t kind;                              /* a bc_file_system_location_kind */
    const bc_file_system_directory *directory; /* the directory for `beneath`; NULL for `at` */
    const uint8_t *path;                       /* `length` bytes, no NUL inside; absolute for `at`, relative for
                                                  `beneath`; borrowed for the call */
    size_t length;
} bc_file_system_location;

/* How much of a file a read may take. */
typedef struct bc_file_system_read_limit {
    int32_t kind;     /* a bc_file_system_read_limit_kind */
    uint64_t at_most; /* the bound under `at_most`; ignored under `whole_file` */
} bc_file_system_read_limit;

/* The mode of anything a call creates or replaces, and what it means for a replaced file. */
typedef struct bc_file_system_permissions {
    int32_t policy; /* a bc_file_system_permissions_policy */
    uint32_t mode;  /* the permission bits as chmod takes them (0644); bits outside 07777 are invalid_argument */
} bc_file_system_permissions;

/* Every choice a write makes, each one required: a zero field is `invalid_argument`. Built once as a named
 * value at a composition root and passed by pointer. */
typedef struct bc_file_system_write_options {
    int32_t durability; /* a bc_file_system_durability */
    bc_file_system_permissions permissions;
    int32_t existing_destination; /* a bc_file_system_existing_destination */
    int32_t final_symlink;        /* a bc_file_system_final_symlink */
} bc_file_system_write_options;

/* Every choice a read makes, each one required. */
typedef struct bc_file_system_read_options {
    bc_file_system_read_limit limit;
    int32_t final_symlink; /* a bc_file_system_final_symlink */
} bc_file_system_read_options;

/* What the file system records about one entry. */
typedef struct bc_file_system_status {
    int32_t entry_type;  /* a bc_file_system_entry_type */
    int64_t size;        /* a regular file's size in bytes; 0 for every other type */
    uint32_t mode;       /* the permission bits (0644), without the file-type bits */
    uint64_t device;     /* st_dev: with `inode`, the entry's identity */
    uint64_t inode;      /* st_ino */
    uint64_t link_count; /* st_nlink */
} bc_file_system_status;

/* One entry of a directory listing. */
typedef struct bc_file_system_entry {
    int32_t entry_type;   /* a bc_file_system_entry_type */
    uint32_t name_length; /* how many bytes of `name` are meaningful */
    uint8_t name[256];    /* the entry's name, one component, not NUL-terminated */
} bc_file_system_entry;

/* One scripted fault for an engine created for tests: the `occurrence`-th time `operation` reaches `step`, the
 * step fails with `system_code` (`fail`) or the operation returns from there without cleanup (`stop`). */
typedef struct bc_file_system_fault {
    int32_t operation;   /* a bc_file_system_operation */
    int32_t step;        /* a bc_file_system_step */
    uint32_t occurrence; /* counted from 1 across the engine's lifetime; 0 is invalid_argument */
    int32_t action;      /* a bc_file_system_fault_action */
    int32_t system_code; /* the errno to answer under `fail` (at least 1); must be 0 under `stop` */
} bc_file_system_fault;

/* ---- Location constructors --------------------------------------------------------------------------------- */

/* An absolute path the system resolves. `path` is `length` bytes, borrowed; relative text is refused by the
 * operation, not here. */
static inline bc_file_system_location bc_file_system_location_at(const uint8_t *path, size_t length) {
    bc_file_system_location location;
    location.kind = bc_file_system_location_kind_at;
    location.directory = NULL;
    location.path = path;
    location.length = length;
    return location;
}

/* A relative path resolved beneath `directory`, which must outlive the location. `path` is `length` bytes,
 * borrowed, at least one component, with no leading slash. */
static inline bc_file_system_location bc_file_system_location_beneath(const bc_file_system_directory *directory,
                                                                      const uint8_t *path, size_t length) {
    bc_file_system_location location;
    location.kind = bc_file_system_location_kind_beneath;
    location.directory = directory;
    location.path = path;
    location.length = length;
    return location;
}

/* ---- The engine and its observer --------------------------------------------------------------------------- */

/* The engine of a composition root: probes the platform's fast paths once and carries no observer. The only
 * function in this header that may be named outside a composition root's body.
 *
 * `engine`  must not be NULL; receives the new engine on success, owned by the caller until
 *           `bc_file_system_engine_release`.
 * `error`   must not be NULL. */
int32_t bc_file_system_engine_create_live(bc_file_system_engine **engine, bc_file_system_error *error);

/* The same engine with the observer installed: every step is counted, and the faults scripted with
 * `bc_file_system_observed_fault_add` take effect. The only way to install an observer; test code only, which
 * the house-rules guard enforces. Parameters as `bc_file_system_engine_create_live`. */
int32_t bc_file_system_engine_create_observed_for_tests(bc_file_system_engine **engine, bc_file_system_error *error);

/* Releases an engine. `engine` may be NULL (nothing happens); must not be used afterwards. Every directory and
 * lock opened through it must have been released first. */
void bc_file_system_engine_release(bc_file_system_engine *engine);

/* Scripts one fault into an engine created for tests. A live engine answers `invalid_argument`. The slot holds
 * a bounded number of faults; one more is `resource_exhausted`.
 *
 * `engine`  must not be NULL.
 * `fault`   must not be NULL; copied.
 * `error`   must not be NULL. */
int32_t bc_file_system_observed_fault_add(bc_file_system_engine *engine, const bc_file_system_fault *fault,
                                          bc_file_system_error *error);

/* Forgets every scripted fault; the step counts are kept. A live engine answers `invalid_argument`. */
int32_t bc_file_system_observed_faults_clear(bc_file_system_engine *engine, bc_file_system_error *error);

/* How many times `operation` has reached `step` in this engine's lifetime. A live engine answers
 * `invalid_argument`.
 *
 * `count`   must not be NULL; receives the answer on success. */
int32_t bc_file_system_observed_step_count(const bc_file_system_engine *engine, int32_t operation, int32_t step,
                                           uint64_t *count, bc_file_system_error *error);

/* ---- Directories ------------------------------------------------------------------------------------------- */

/* Opens the directory at `location` and remembers the text it was opened with: for `at`, the path's bytes; for
 * `beneath`, the parent's remembered text, a slash and the relative bytes. Something that is not a directory is
 * `not_directory`; nothing there is `not_found`; a final symlink is followed or refused per `final_symlink`
 * (followed wherever it points, under `beneath` too: the choice is the caller's, and containment covers the
 * path to it). The root, `/`, opens. Where the platform cannot enforce containment beneath a directory
 * (Emscripten, where a descriptor is a path string) a directory still opens through `at`, and every `beneath`
 * location resolved against it, this call's included, answers `unsupported` before any system call.
 *
 * `engine`     must not be NULL; must outlive the directory.
 * `location`   must not be NULL.
 * `directory`  must not be NULL; receives the handle on success, owned by the caller until
 *              `bc_file_system_directory_release`.
 * `error`      must not be NULL. */
int32_t bc_file_system_directory_open(bc_file_system_engine *engine, const bc_file_system_location *location,
                                      int32_t final_symlink, bc_file_system_directory **directory,
                                      bc_file_system_error *error);

/* The text the directory was opened with, under the buffer protocol.
 *
 * `directory`  must not be NULL.
 * `buffer`     `capacity` bytes the caller owns; may be NULL only when `capacity` is 0.
 * `length`     must not be NULL. */
int32_t bc_file_system_directory_opened_as(const bc_file_system_directory *directory, uint8_t *buffer, size_t capacity,
                                           size_t *length, bc_file_system_error *error);

/* Closes the directory and frees the handle. `directory` may be NULL (nothing happens). */
void bc_file_system_directory_release(bc_file_system_directory *directory);

/* Releases the lock and frees the handle. `lock` may be NULL (nothing happens). */
void bc_file_system_lock_release(bc_file_system_lock *lock);

/* ---- Operations -------------------------------------------------------------------------------------------- */

/* Every operation takes the engine it runs in (must not be NULL), the location it acts on (must not be NULL;
 * borrowed for the call), its required choices, its answer's out-parameters (each must not be NULL, each written
 * only on success) and the error (must not be NULL). Byte parameters (`bytes`, `target`) may be NULL only when
 * their length is 0. */

/* What is at `location`. Absence is an answer, not an error: `*present` is 0 and `*status` is untouched. */
int32_t bc_file_system_status_of(bc_file_system_engine *engine, const bc_file_system_location *location,
                                 int32_t final_symlink, int32_t *present, bc_file_system_status *status,
                                 bc_file_system_error *error);

/* The bytes of a regular file, under the buffer protocol and the read limit. Anything that is not a regular
 * file is `not_regular_file`. A file that grows between two calls is `buffer_too_small` again. */
int32_t bc_file_system_read_file(bc_file_system_engine *engine, const bc_file_system_location *location,
                                 const bc_file_system_read_options *options, uint8_t *buffer, size_t capacity,
                                 size_t *length, bc_file_system_error *error);

/* Replaces or creates the regular file at `location` with `byte_count` bytes, atomically: the bytes are staged
 * in a temporary file in the same directory, synced as asked, and renamed into place, so a reader sees the old
 * content or the new, never a mixture. The directory must exist. `*durability_achieved` receives a
 * bc_file_system_durability. */
int32_t bc_file_system_write_file(bc_file_system_engine *engine, const bc_file_system_location *location,
                                  const uint8_t *bytes, size_t byte_count, const bc_file_system_write_options *options,
                                  int32_t *durability_achieved, bc_file_system_error *error);

/* Adds `byte_count` bytes to the end of the regular file at `location`, creating it with `permissions` when it
 * is absent. Not atomic: after an error some leading part of the bytes may be in the file. */
int32_t bc_file_system_append(bc_file_system_engine *engine, const bc_file_system_location *location,
                              const uint8_t *bytes, size_t byte_count, int32_t durability,
                              const bc_file_system_permissions *permissions, int32_t *durability_achieved,
                              bc_file_system_error *error);

/* Pushes the file or directory at `location` to stable storage. A volume that offers no sync answers
 * `written`. */
int32_t bc_file_system_sync(bc_file_system_engine *engine, const bc_file_system_location *location,
                            int32_t *durability_achieved, bc_file_system_error *error);

/* Creates one directory; one already there is `already_exists`; a missing parent is `not_found`. */
int32_t bc_file_system_create_directory(bc_file_system_engine *engine, const bc_file_system_location *location,
                                        const bc_file_system_permissions *permissions, bc_file_system_error *error);

/* Creates the directory and every missing one on the way, each with `permissions`; one already there is left
 * as it is; something on the way that is not a directory is `not_directory`. */
int32_t bc_file_system_create_directories(bc_file_system_engine *engine, const bc_file_system_location *location,
                                          const bc_file_system_permissions *permissions, bc_file_system_error *error);

/* The entries of a directory, sorted by name bytes so a listing is the same on every run, into the caller's
 * array of `capacity` entries; `*count` receives how many were stored, or how many are needed on
 * `buffer_too_small`. A listing is complete or it is an error. The directory's own final symlink is followed or
 * refused per `final_symlink`; a symlink inside is an entry, never followed. */
int32_t bc_file_system_list_directory(bc_file_system_engine *engine, const bc_file_system_location *location,
                                      int32_t final_symlink, bc_file_system_entry *entries, size_t capacity,
                                      size_t *count, bc_file_system_error *error);

/* Removes the file, symlink or other non-directory at `location`; a directory is `not_regular_file`. Absence is
 * an answer: `*existed` receives 0, and 1 when something was removed. */
int32_t bc_file_system_remove_file(bc_file_system_engine *engine, const bc_file_system_location *location,
                                   int32_t *existed, bc_file_system_error *error);

/* Removes the empty directory at `location`; one that is not empty is `directory_not_empty`; something that is
 * not a directory is `not_directory`. `*existed` as for `bc_file_system_remove_file`. */
int32_t bc_file_system_remove_directory(bc_file_system_engine *engine, const bc_file_system_location *location,
                                        int32_t *existed, bc_file_system_error *error);

/* Removes `location` and everything below it, by unlinkat on held descriptors with a bounded number open and no
 * recursion; a symlink is removed, never followed. The root of the file system is refused by spelling and by
 * identity; a tree that crosses onto another device is refused with `cross_device`. After an error part of the
 * tree may be gone. `*existed` as for `bc_file_system_remove_file`. */
int32_t bc_file_system_remove_tree(bc_file_system_engine *engine, const bc_file_system_location *location,
                                   int32_t *existed, bc_file_system_error *error);

/* Creates a new directory with a unique name inside `parent`, with `permissions`, and stores the created name
 * (one component) under the buffer protocol. */
int32_t bc_file_system_create_temporary_directory(bc_file_system_engine *engine, const bc_file_system_location *parent,
                                                  const bc_file_system_permissions *permissions, uint8_t *buffer,
                                                  size_t capacity, size_t *length, bc_file_system_error *error);

/* Renames `source` to `destination` in one step. Under `replace` a file replaces a file and a directory replaces an
 * empty directory (`directory_not_empty` otherwise); under `refuse` an existing destination is `already_exists` and
 * untouched. A durable rename syncs both directories. Different devices are `cross_device`. */
int32_t bc_file_system_rename(bc_file_system_engine *engine, const bc_file_system_location *source,
                              const bc_file_system_location *destination, int32_t existing_destination,
                              int32_t durability, int32_t *durability_achieved, bc_file_system_error *error);

/* Copies the regular file at `source` to `destination` with the guarantees of `bc_file_system_write_file`: atomic at
 * the destination, the platform's clone path when it has one, the copy loop otherwise. */
int32_t bc_file_system_copy_file(bc_file_system_engine *engine, const bc_file_system_location *source,
                                 const bc_file_system_location *destination,
                                 const bc_file_system_write_options *options, int32_t *durability_achieved,
                                 bc_file_system_error *error);

/* Sets the permission bits of the entry at `location` exactly to `mode` (bits outside 07777 are
 * `invalid_argument`). */
int32_t bc_file_system_set_permissions(bc_file_system_engine *engine, const bc_file_system_location *location,
                                       uint32_t mode, int32_t final_symlink, bc_file_system_error *error);

/* Creates a symlink at `location` whose text is the `target_length` bytes of `target`, passed to the system
 * unchanged. An existing entry is `already_exists`. */
int32_t bc_file_system_create_symlink(bc_file_system_engine *engine, const uint8_t *target, size_t target_length,
                                      const bc_file_system_location *location, bc_file_system_error *error);

/* The text of the symlink at `location`, under the buffer protocol. An entry that is not a symlink is
 * `io_failed` with EINVAL, as readlinkat answers it: no kind of the fourteen means "not a symlink", and one is
 * added only when a consumer measures a site that decides on it. */
int32_t bc_file_system_read_symlink(bc_file_system_engine *engine, const bc_file_system_location *location,
                                    uint8_t *buffer, size_t capacity, size_t *length, bc_file_system_error *error);

/* Tries to take an advisory exclusive lock on the file at `location` without waiting. `*outcome` receives a
 * bc_file_system_lock_outcome; `*lock` receives the handle under `acquired` and NULL under `held_elsewhere`.
 * Where the platform's flock is a stub (Emscripten) the answer is `unsupported`. */
int32_t bc_file_system_try_lock(bc_file_system_engine *engine, const bc_file_system_location *location,
                                bc_file_system_lock **lock, int32_t *outcome, bc_file_system_error *error);

/* Renames the tree at `location` to `.bc-condemned-<name>` beside it, so a later sweep removes it, and stores
 * the condemned name (one component) under the buffer protocol. */
int32_t bc_file_system_condemn(bc_file_system_engine *engine, const bc_file_system_location *location, uint8_t *buffer,
                               size_t capacity, size_t *length, bc_file_system_error *error);

/* Removes every `.bc-condemned-` entry directly inside `parent`, as `bc_file_system_remove_tree` would. */
int32_t bc_file_system_remove_condemned(bc_file_system_engine *engine, const bc_file_system_location *parent,
                                        bc_file_system_error *error);

/* ---- Path text --------------------------------------------------------------------------------------------- */

/* The path module: the lexical rules of path text, with nothing consulted, not the working directory, the home
 * directory, the environment, the disk or a Unicode table. It is the library's freestanding tier (nothing but
 * <stddef.h> and <stdint.h>, no allocation, caller buffers), so it builds for wasm32 and under -ffreestanding as
 * well as on every hosted target. Each function validates its pointers and lengths first and answers
 * `invalid_argument`, touching nothing, when text breaks the rules below; an answer of variable length follows the
 * buffer protocol at the top of this header. The operations reported are `relative_path_from`,
 * `absolute_path_from` and the `bc_file_system_operation_path_*` values.
 *
 * A RELATIVE PATH has one spelling, so equality is a byte compare: at least one component, no leading slash, no
 * NUL, no empty component (`a//b`, a trailing slash), no `.` and no `..` component, no component over 255 bytes
 * (NAME_MAX on every platform the library ships for). An ABSOLUTE PATH starts with a slash: `/` alone is the
 * root; otherwise the same component rules, except that `..` is allowed: it is reported, never resolved against
 * the disk. A COMPONENT is one entry name: 1 to 255 bytes, no slash, no NUL (`.` and `..` are components on
 * their own; the path rules are what refuse them inside a path). LOOSE TEXT, which only `lexically_normalize`,
 * `relate` and `resolve_against` accept, is any non-empty text without a NUL whose components are at most 255
 * bytes: `.`, `..` and empty components are what normalisation removes. Every `path` or `component` below is
 * `length` bytes and may be NULL only when `length` is 0; the empty text is refused by every function here, so
 * a NULL answers `invalid_argument` either way. */

/* Whether `path` is a relative path by the rules above. Reports `bc_file_system_operation_relative_path_from`. */
int32_t bc_file_system_relative_path_validate(const uint8_t *path, size_t length, bc_file_system_error *error);

/* Whether `path` is an absolute path by the rules above. Reports `bc_file_system_operation_absolute_path_from`. */
int32_t bc_file_system_absolute_path_validate(const uint8_t *path, size_t length, bc_file_system_error *error);

/* `base`, a slash and `relative`, under the buffer protocol; the root already ends in its slash, so `/` and `a`
 * join as `/a`. `base` is a valid absolute or relative path and `relative` a valid relative path, so the answer
 * is a valid path of `base`'s kind. A sum of lengths that does not fit `size_t` is `too_large`. */
int32_t bc_file_system_path_join(const uint8_t *base, size_t base_length, const uint8_t *relative,
                                 size_t relative_length, uint8_t *buffer, size_t capacity, size_t *length,
                                 bc_file_system_error *error);

/* The parent of a valid path is a prefix of it. `*present` receives 1 and `*parent_length` how many leading
 * bytes of `path` spell the parent (`/` for `/a`, `a/b` for `a/b/c`). The root and a one-component relative path
 * have none: `*present` receives 0 and `*parent_length` is untouched.
 *
 * `present`        must not be NULL.
 * `parent_length`  must not be NULL. */
int32_t bc_file_system_path_parent(const uint8_t *path, size_t length, int32_t *present, size_t *parent_length,
                                   bc_file_system_error *error);

/* The last component of a valid path, located inside it: `*offset` and `*component_length`. The root has no
 * component and is `invalid_argument`.
 *
 * `offset`            must not be NULL.
 * `component_length`  must not be NULL. */
int32_t bc_file_system_path_last_component(const uint8_t *path, size_t length, size_t *offset, size_t *component_length,
                                           bc_file_system_error *error);

/* The components of a valid path, one per call. `*cursor` is 0 before the first call and advances with each;
 * while `*present` receives 1, `*offset` and `*component_length` locate the next component inside `path`; at the
 * end `*present` receives 0 and the other two are untouched. A `*cursor` that is neither 0 nor a value a previous
 * call stored is `invalid_argument`.
 *
 * `cursor`, `offset`, `component_length`, `present`  must not be NULL. */
int32_t bc_file_system_path_next_component(const uint8_t *path, size_t length, size_t *cursor, size_t *offset,
                                           size_t *component_length, int32_t *present, bc_file_system_error *error);

/* The stem of one component: the bytes before its extension, or all of them when it has none. `*stem_length`
 * receives how many leading bytes of `component` the stem is.
 *
 * `stem_length`  must not be NULL. */
int32_t bc_file_system_path_stem(const uint8_t *component, size_t length, size_t *stem_length,
                                 bc_file_system_error *error);

/* The extension of one component: the bytes after its last dot, when that dot is neither the first byte
 * (`.bashrc` has no extension) nor the last (`archive.` has none); the dot itself is excluded. `*present`
 * receives 1 with `*offset` and `*extension_length` locating it inside `component`, or 0 with the other two
 * untouched.
 *
 * `offset`, `extension_length`, `present`  must not be NULL. */
int32_t bc_file_system_path_extension(const uint8_t *component, size_t length, size_t *offset, size_t *extension_length,
                                      int32_t *present, bc_file_system_error *error);

/* Where `path` stands with respect to `other`, lexically. Both are loose text, each normalised as
 * `bc_file_system_path_lexically_normalize` would and then compared component by component; `*relation` receives
 * a bc_file_system_path_relation. Two absolute or two relative paths compare; one of each is `undecidable`, and
 * so is a relative path that still begins with `..` once normalised, since no lexical rule knows where that
 * climbs to. Advisory: nothing here guards an access; `beneath` does.
 *
 * `relation`  must not be NULL. */
int32_t bc_file_system_path_relate(const uint8_t *path, size_t length, const uint8_t *other, size_t other_length,
                                   int32_t *relation, bc_file_system_error *error);

/* Loose text reduced to one spelling, under the buffer protocol, with `*normalized` the answer's length: doubled
 * slashes collapse, `.` components go, `a/..` folds away, a trailing slash goes, a `..` at the root is dropped
 * (`/..` is `/`), and a leading `..` of a relative path stays. A relative path with nothing left is `.`. The
 * disk is never consulted, so a `..` folded through a symlink names a different entry than the system would:
 * that is the trade of a lexical rule, and `relate` is advisory for the same reason. Idempotent.
 *
 * `normalized`  must not be NULL. */
int32_t bc_file_system_path_lexically_normalize(const uint8_t *path, size_t length, uint8_t *buffer, size_t capacity,
                                                size_t *normalized, bc_file_system_error *error);

/* `relative`, loose text with no leading slash, resolved against the valid absolute path `absolute_base`: the
 * two joined and normalised, so the answer is a valid absolute path, under the buffer protocol. How a composition
 * root turns a command-line argument into an `at` location against the working directory it was handed. A sum
 * of lengths that does not fit `size_t` is `too_large`. */
int32_t bc_file_system_path_resolve_against(const uint8_t *absolute_base, size_t base_length, const uint8_t *relative,
                                            size_t relative_length, uint8_t *buffer, size_t capacity, size_t *length,
                                            bc_file_system_error *error);

/* Whether one component spells a library file the way a platform names one, and the library's own name inside
 * it: `libX.so`, `libX.so.1`, `libX.so.1.2.3` and `libX.dylib` (shared), `libX.a` (static), `X.framework`
 * (framework), `X.dll` (shared), with X at least one byte, byte for byte (`libX.SO` is none). `*kind` receives a
 * bc_file_system_library_kind; `*stem_offset` and `*stem_length` locate X inside `component`, and under `none`
 * are untouched.
 *
 * `kind`, `stem_offset`, `stem_length`  must not be NULL. */
int32_t bc_file_system_path_library_name(const uint8_t *component, size_t length, int32_t *kind, size_t *stem_offset,
                                         size_t *stem_length, bc_file_system_error *error);

#ifdef __cplusplus
}
#endif
