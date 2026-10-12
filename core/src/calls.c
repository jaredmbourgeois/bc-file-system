/* calls.c — the bodies behind calls.h: one loop per system call, and the errno table.
 *
 * Every loop has the same shape, spelled out in full rather than through a helper, because the analyzer loses
 * a postcondition set several calls deep (the resolution layer's lesson): ask the observer; a `stop` ends the
 * attempts with `stopped`; a scripted `fail` is the call's errno without the call, retried when it is EINTR
 * like a real one; otherwise make the call, and on failure copy errno on the very next line. The whole outcome
 * struct is written where the call ends, so `succeeded` is true only where the call itself answered.
 */
#include "calls.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h> /* only for renameat, which POSIX declares here; no stream is touched */
#include <sys/stat.h>
#include <unistd.h>

#include <stddef.h>
#include <stdint.h>

#include <bc_file_system/bc_file_system.h>

#include "engine.h"
#include "platform.h"

/* ---- The errno mapping -------------------------------------------------------------------------------------- */

/* The kind each errno decides, in one table read by one function. A table rather than a switch because two
 * errno names are the same number on some systems (ENOTSUP and EOPNOTSUPP on Linux), which a switch would not
 * compile; the first row that matches wins. Anything absent is `io_failed`, with the errno kept in the error. */
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
    /* The destination of a no-replace rename, or of an exclusive create, holds something already. */
    {EEXIST, bc_file_system_error_kind_already_exists},
    /* A directory sits where a regular file was to be opened for writing or renamed over. */
    {EISDIR, bc_file_system_error_kind_not_regular_file},
    {ENOSPC, bc_file_system_error_kind_no_space},
    {EDQUOT, bc_file_system_error_kind_no_space},
    /* The process's file-size limit, reached only on a host that ignores SIGXFSZ (the library never installs
     * that handling). */
    {EFBIG, bc_file_system_error_kind_too_large},
    {EXDEV, bc_file_system_error_kind_cross_device},
    /* The platform's own answer where a call is absent, and the kernel's for a call it has no body for. */
    {ENOTSUP, bc_file_system_error_kind_unsupported},
    {EOPNOTSUPP, bc_file_system_error_kind_unsupported},
    /* A component longer than the system takes: the path module refuses those first, so this is a contract
     * violation the system noticed before the module could. */
    {ENAMETOOLONG, bc_file_system_error_kind_invalid_argument},
    {EMFILE, bc_file_system_error_kind_resource_exhausted},
    {ENFILE, bc_file_system_error_kind_resource_exhausted},
    {ENOMEM, bc_file_system_error_kind_resource_exhausted},
};

int32_t bc_file_system_call_kind_of_errno(int system_code) {
    for (size_t index = 0; index < sizeof errno_mappings / sizeof errno_mappings[0]; ++index) {
        if (errno_mappings[index].system_code == system_code) {
            return errno_mappings[index].kind;
        }
    }
    return bc_file_system_error_kind_io_failed;
}

/* ---- Looking ------------------------------------------------------------------------------------------------ */

bc_file_system_call_outcome bc_file_system_call_fstatat(bc_file_system_engine *engine, int32_t operation, int parent,
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

bc_file_system_call_outcome bc_file_system_call_fstat(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                      struct stat *info) {
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

bc_file_system_call_outcome bc_file_system_call_readlinkat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                           const char *name, uint8_t *buffer, size_t capacity,
                                                           size_t *length) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_readlinkat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const ssize_t answer = readlinkat(parent, name, (char *)buffer, capacity);
        if (answer >= 0) {
            *length = (size_t)answer;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* ---- Opening ------------------------------------------------------------------------------------------------ */

/* The one loop behind both opens: `mode` is read by the system only when `flags` create. */
static bc_file_system_call_outcome open_entry(bc_file_system_engine *engine, int32_t operation, int parent,
                                              const char *name, int flags, mode_t mode, int *descriptor) {
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
        const int opened = openat(parent, name, flags, mode);
        if (opened >= 0) {
            *descriptor = opened;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_openat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                       const char *name, int flags, int *descriptor) {
    return open_entry(engine, operation, parent, name, flags, 0, descriptor);
}

bc_file_system_call_outcome bc_file_system_call_openat_creating(bc_file_system_engine *engine, int32_t operation,
                                                                int parent, const char *name, int flags, uint32_t mode,
                                                                int *descriptor) {
    /* A mode is at most 07777, which every platform's mode_t holds; the cast only changes the type. */
    return open_entry(engine, operation, parent, name, flags, (mode_t)mode, descriptor);
}

/* ---- Reading and writing ----------------------------------------------------------------------------------- */

bc_file_system_call_outcome bc_file_system_call_pread(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                      uint8_t *buffer, size_t count, uint64_t offset,
                                                      size_t *read_count) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_pread);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        /* The offset never passes a regular file's size, which off_t holds (platform.c asserts its width). */
        const ssize_t answer = pread(descriptor, buffer, count, (off_t)offset);
        if (answer >= 0) {
            *read_count = (size_t)answer;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_write(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                      const uint8_t *bytes, size_t count, size_t *written_count) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_write);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const ssize_t answer = write(descriptor, bytes, count);
        if (answer >= 0) {
            *written_count = (size_t)answer;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* ---- Making a change stick --------------------------------------------------------------------------------- */

/* The step names the call the platform makes, so the suite can hold that fsync is never the durable sync on a
 * platform where fsync stops at the drive's cache. */
int32_t bc_file_system_call_sync_step(const bc_file_system_engine *engine) {
    if (bc_file_system_engine_fast_paths(engine)->has_full_fsync) {
        return bc_file_system_step_fcntl_fullfsync;
    }
    return bc_file_system_step_fsync;
}

bc_file_system_call_outcome bc_file_system_call_sync(bc_file_system_engine *engine, int32_t operation, int descriptor) {
    const int32_t step = bc_file_system_call_sync_step(engine);
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
        const int code = bc_file_system_platform_sync_descriptor(descriptor);
        if (code == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* No loop: close is made once whatever the observer or the system answers. */
bc_file_system_call_outcome bc_file_system_call_close(bc_file_system_engine *engine, int32_t operation,
                                                      int descriptor) {
    const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_close);
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    if (observer_decided(&verdict, &outcome)) {
        /* The descriptor is consumed as if the call had been made: a scripted answer never leaks it. */
        (void)close(descriptor); /* discard-ok: the scripted answer is the one reported */
        return outcome;
    }
    if (close(descriptor) == 0) {
        return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
    }
    const int code = errno;
    return (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
}

bc_file_system_call_outcome bc_file_system_call_fchmod(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                       uint32_t mode) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_fchmod);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        /* A mode is at most 07777, which every platform's mode_t holds; the cast only changes the type. */
        if (fchmod(descriptor, (mode_t)mode) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_renameat(bc_file_system_engine *engine, int32_t operation,
                                                         int from_parent, const char *from_name, int to_parent,
                                                         const char *to_name) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_renameat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        if (renameat(from_parent, from_name, to_parent, to_name) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_rename_no_replace(bc_file_system_engine *engine, int32_t operation,
                                                                  int from_parent, const char *from_name, int to_parent,
                                                                  const char *to_name) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict =
            ask_observer(engine, operation, bc_file_system_step_rename_no_replace);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const int code = bc_file_system_platform_rename_no_replace(from_parent, from_name, to_parent, to_name);
        if (code == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_unlinkat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                         const char *name, int flags) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_unlinkat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        if (unlinkat(parent, name, flags) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_fchmodat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                         const char *name, uint32_t mode, int flags) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_fchmodat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        /* A mode is at most 07777, which every platform's mode_t holds; the cast only changes the type. */
        if (fchmodat(parent, name, (mode_t)mode, flags) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_faccessat(bc_file_system_engine *engine, int32_t operation,
                                                          int directory, const char *name, int mode, int flags) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_faccessat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        if (faccessat(directory, name, mode, flags) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* ---- Directories --------------------------------------------------------------------------------------------- */

bc_file_system_call_outcome bc_file_system_call_mkdirat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                        const char *name, uint32_t mode) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_mkdirat);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        /* A mode is at most 07777, which every platform's mode_t holds; the cast only changes the type. */
        if (mkdirat(parent, name, (mode_t)mode) == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_fdopendir(bc_file_system_engine *engine, int32_t operation,
                                                          int descriptor, DIR **stream) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_fdopendir);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        DIR *opened = fdopendir(descriptor);
        if (opened != nullptr) {
            *stream = opened;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        const int code = errno;
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_readdir(bc_file_system_engine *engine, int32_t operation, DIR *stream,
                                                        struct dirent **entry) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_readdir);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        /* The end of the stream and an error both answer NULL; only errno tells them apart, so it is zeroed
         * first and read on the line after. */
        errno = 0;
        struct dirent *read = readdir(stream);
        const int code = errno;
        if (read != nullptr || code == 0) {
            *entry = read;
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

/* ---- Copying ------------------------------------------------------------------------------------------------ */

/* The platform body makes its own retries; a scripted EINTR is retried here, like every other call's. */
bc_file_system_call_outcome bc_file_system_call_clone_file(bc_file_system_engine *engine, int32_t operation, int source,
                                                           int destination_directory, const char *destination_name,
                                                           uint32_t mode) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_clone_file);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const int code = bc_file_system_platform_clone_file(source, destination_directory, destination_name, mode);
        if (code == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}

bc_file_system_call_outcome bc_file_system_call_copy_range(bc_file_system_engine *engine, int32_t operation, int source,
                                                           int destination, size_t count, size_t *copied) {
    bc_file_system_call_outcome outcome = {.succeeded = false, .code = 0, .stopped = false};
    do {
        const bc_file_system_step_verdict verdict = ask_observer(engine, operation, bc_file_system_step_copy_range);
        if (verdict.action == bc_file_system_step_action_stop) {
            return (bc_file_system_call_outcome){.succeeded = false, .code = 0, .stopped = true};
        }
        if (verdict.action == bc_file_system_step_action_fail) {
            outcome = (bc_file_system_call_outcome){.succeeded = false, .code = verdict.system_code, .stopped = false};
            continue;
        }
        const int code = bc_file_system_platform_copy_range(source, destination, count, copied);
        if (code == 0) {
            return (bc_file_system_call_outcome){.succeeded = true, .code = 0, .stopped = false};
        }
        outcome = (bc_file_system_call_outcome){.succeeded = false, .code = code, .stopped = false};
    } while (outcome.code == EINTR);
    return outcome;
}
