/* platform.c — the bodies behind platform.h: the fast-path probe and the five platform operations.
 *
 * The one translation unit of the core that names an operating system. Each platform's bodies sit together
 * under one guard on a predefined name (`__APPLE__`, `__EMSCRIPTEN__`, `__linux__`, which covers glibc, musl
 * and Android), so a reader sees one platform at a time and a platform with no body fails the build at the
 * `#error` below rather than linking a portable guess. The two feature-test knobs the Linux headers need are
 * defined at the top, before any include, which is the one build-system-independent spelling: CMake, SwiftPM
 * and cargo's `cc` then compile this file the same way with no definition of their own.
 */
#ifdef __linux__
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#endif

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h> /* only for renameatx_np on Apple platforms, which the SDK declares here; no stream is touched */
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif
#if __has_include(<linux/fs.h>)
#include <linux/fs.h>
#endif
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#endif

/* A 32-bit off_t narrows an offset silently and reads another region's bytes: the width is required everywhere,
 * and on Linux it is what the knob at the top asks for. */
static_assert(sizeof(off_t) == 8, "bc-file-system requires a 64-bit off_t");

/* ---- Shared pieces ----------------------------------------------------------------------------------------- */

/* The errnos a volume answers when it offers no sync at all (a pipe, a special file, a network mount without the
 * call); each becomes ENOTSUP so the engine reports `written` instead of an error. */
static bool means_no_sync_offered(int code) {
    if (code == EINVAL) {
        return true;
    }
    if (code == ENOTSUP) {
        return true;
    }
    return code == ENOTTY;
}

static int sync_refusal(int code) {
    if (means_no_sync_offered(code)) {
        return ENOTSUP;
    }
    return code;
}

/* ---- The operations no stage has built yet --------------------------------------------------------------- */

/* Honest refusals: the engine takes its portable path (the copy loop) until the stage named at each body gives
 * the fast path a body. Each answers ENOTSUP on every platform. */

/* stage 16 moves-and-copies-in-c: clonefileat / fclonefileat on Apple, FICLONE on Linux. */
int bc_file_system_platform_clone_file([[maybe_unused]] int source, [[maybe_unused]] int destination_directory,
                                       [[maybe_unused]] const char *destination_name, [[maybe_unused]] uint32_t mode) {
    return ENOTSUP;
}

/* stage 16 moves-and-copies-in-c: copy_file_range on Linux. */
int bc_file_system_platform_copy_range([[maybe_unused]] int source, [[maybe_unused]] int destination,
                                       [[maybe_unused]] size_t count, [[maybe_unused]] size_t *copied) {
    return ENOTSUP;
}

#ifdef __APPLE__

/* ---- Apple platforms --------------------------------------------------------------------------------------- */

/* The SDK answers every question: no Apple kernel the library ships for lacks O_NOFOLLOW_ANY, renameatx_np,
 * clonefileat or F_FULLFSYNC, and none has openat2 or copy_file_range. */
bc_file_system_platform_fast_paths bc_file_system_platform_probe_fast_paths(void) {
    return (bc_file_system_platform_fast_paths){
        .has_openat2 = false,
        .has_openat_nofollow_any = true,
        .has_rename_no_replace = true,
        .has_clone = true,
        .has_copy_range = false,
        .has_full_fsync = true,
    };
}

/* A directory descriptor is a kernel object here. */
bool bc_file_system_platform_enforces_containment(void) {
    return true;
}

/* O_NOFOLLOW_ANY makes the kernel refuse a symlink at any depth of `relative` with ELOOP (measured: in the middle
 * and at the end alike), so one openat relative to the held root resolves the whole path contained. */
int bc_file_system_platform_open_beneath(int root, const char *relative, int open_flags, int *descriptor) {
    int code = 0;
    do {
        const int opened = openat(root, relative, open_flags | O_NOFOLLOW_ANY);
        if (opened >= 0) {
            *descriptor = opened;
            return 0;
        }
        code = errno;
    } while (code == EINTR);
    return code;
}

/* fsync on Apple platforms stops at the drive's cache; F_FULLFSYNC asks the drive to flush that cache. */
int bc_file_system_platform_sync_descriptor(int descriptor) {
    int code = 0;
    do {
        if (fcntl(descriptor, F_FULLFSYNC) == 0) {
            return 0;
        }
        code = errno;
    } while (code == EINTR);
    return sync_refusal(code);
}

int bc_file_system_platform_rename_no_replace(int from_directory, const char *from_name, int to_directory,
                                              const char *to_name) {
    int code = 0;
    do {
        if (renameatx_np(from_directory, from_name, to_directory, to_name, RENAME_EXCL) == 0) {
            return 0;
        }
        code = errno;
    } while (code == EINTR);
    return code;
}

#elifdef __EMSCRIPTEN__

/* ---- Emscripten -------------------------------------------------------------------------------------------- */

/* Measured under Node for MEMFS and NODERAWFS (the design's platform table): no backend syncs, clones, copies in
 * the kernel, renames without replacing or opens beneath a held directory. */
bc_file_system_platform_fast_paths bc_file_system_platform_probe_fast_paths(void) {
    return (bc_file_system_platform_fast_paths){
        .has_openat2 = false,
        .has_openat_nofollow_any = false,
        .has_rename_no_replace = false,
        .has_clone = false,
        .has_copy_range = false,
        .has_full_fsync = false,
    };
}

/* A directory descriptor is a path string the runtime joins names onto (`stream.path + '/' + name`): a held
 * directory goes stale on rename, a symlink on the way escapes, and no openat2 exists. Nothing is contained. */
bool bc_file_system_platform_enforces_containment(void) {
    return false;
}

/* No kernel call opens a path contained beneath a descriptor here; the engine never asks, since containment is
 * absent altogether, but the honest answer stays in place. */
int bc_file_system_platform_open_beneath([[maybe_unused]] int root, [[maybe_unused]] const char *relative,
                                         [[maybe_unused]] int open_flags, [[maybe_unused]] int *descriptor) {
    return ENOTSUP;
}

/* Both backends' fsync return 0 and do nothing, so the honest answer is "no sync offered": the engine reports
 * `written`, which is what the bytes achieved. */
int bc_file_system_platform_sync_descriptor([[maybe_unused]] int descriptor) {
    return ENOTSUP;
}

/* renameat2 declares but does not link, and renameat replaces; the NODERAWFS link-then-unlink path is stage 16's. */
int bc_file_system_platform_rename_no_replace([[maybe_unused]] int from_directory,
                                              [[maybe_unused]] const char *from_name, [[maybe_unused]] int to_directory,
                                              [[maybe_unused]] const char *to_name) {
    return ENOTSUP;
}

#elifdef __linux__

/* ---- Linux: glibc, musl and Android ------------------------------------------------------------------------ */

/* The kernel is asked through syscall(2) rather than a libc wrapper, so one body serves glibc, musl (whose 1.2.5
 * has no renameat2 wrapper) and bionic alike. A probe hands the call an invalid descriptor and an empty name,
 * which the kernel refuses (EBADF, ENOENT, EFAULT) once it has dispatched the call; a kernel without the call
 * answers ENOSYS before looking at any argument, so that one errno is the whole answer. */
static bool kernel_dispatched(long answer) {
    if (answer == 0) {
        return true;
    }
    return errno != ENOSYS;
}

#if defined(__ANDROID__) || !__has_include(<linux/openat2.h>)
/* Android's seccomp policy kills a process that makes the openat2 system call (SIGSYS through 16 QPR1), so it is
 * never probed there; a header set older than kernel 5.6 has no `struct open_how` to probe with. */
static bool probe_openat2(void) {
    return false;
}
#else
static bool probe_openat2(void) {
    struct open_how how = {};
    return kernel_dispatched(syscall(SYS_openat2, -1, "", &how, sizeof how));
}
#endif

#if __has_include(<linux/fs.h>)
static bool probe_rename_no_replace(void) {
    return kernel_dispatched(syscall(SYS_renameat2, -1, "", -1, "", RENAME_NOREPLACE));
}

/* FICLONE cannot be probed without a file: ioctl(2) checks the descriptor before the request, so an invalid
 * descriptor answers EBADF whether or not the volume clones. The ioctl is in the headers; whether a given volume
 * honours it is answered at clone time (ENOTTY, EOPNOTSUPP), and the clone body falls to the copy loop then. */
static bool probe_clone(void) {
    return true;
}
#else
static bool probe_rename_no_replace(void) {
    return false;
}

static bool probe_clone(void) {
    return false;
}
#endif

static bool probe_copy_range(void) {
    return kernel_dispatched(syscall(SYS_copy_file_range, -1, nullptr, -1, nullptr, 0, 0));
}

bc_file_system_platform_fast_paths bc_file_system_platform_probe_fast_paths(void) {
    return (bc_file_system_platform_fast_paths){
        .has_openat2 = probe_openat2(),
        .has_openat_nofollow_any = false,
        .has_rename_no_replace = probe_rename_no_replace(),
        .has_clone = probe_clone(),
        .has_copy_range = probe_copy_range(),
        .has_full_fsync = false,
    };
}

/* A directory descriptor is a kernel object here. */
bool bc_file_system_platform_enforces_containment(void) {
    return true;
}

#if defined(__ANDROID__) || !__has_include(<linux/openat2.h>)
/* The engine never calls this where the probe answered no (Android, or headers older than kernel 5.6); the
 * honest answer stays in place. */
int bc_file_system_platform_open_beneath([[maybe_unused]] int root, [[maybe_unused]] const char *relative,
                                         [[maybe_unused]] int open_flags, [[maybe_unused]] int *descriptor) {
    return ENOTSUP;
}
#else
/* RESOLVE_BENEATH keeps the resolution under `root` (an escape is EXDEV) and RESOLVE_NO_SYMLINKS refuses every
 * symlink on the way with ELOOP, so one call resolves the whole path contained. The flags travel in the
 * `open_how` struct as a 64-bit field; the engine's flags are non-negative, so the widening is exact. */
int bc_file_system_platform_open_beneath(int root, const char *relative, int open_flags, int *descriptor) {
    /* The RESOLVE_* macros are `unsigned long`; the field is the kernel's 64-bit type, so GCC asks for the
     * widening to be spelled. */
    struct open_how how = {.flags = (uint64_t)open_flags, .resolve = (uint64_t)(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)};
    int code = 0;
    do {
        const long opened = syscall(SYS_openat2, root, relative, &how, sizeof how);
        if (opened >= 0) {
            /* A descriptor is an int; the kernel answered one. */
            *descriptor = (int)opened;
            return 0;
        }
        code = errno;
    } while (code == EINTR);
    /* A kernel without the call, which the probe did not see: the engine walks. */
    if (code == ENOSYS) {
        return ENOTSUP;
    }
    return code;
}
#endif

int bc_file_system_platform_sync_descriptor(int descriptor) {
    int code = 0;
    do {
        if (fsync(descriptor) == 0) {
            return 0;
        }
        code = errno;
    } while (code == EINTR);
    return sync_refusal(code);
}

#if __has_include(<linux/fs.h>)
int bc_file_system_platform_rename_no_replace(int from_directory, const char *from_name, int to_directory,
                                              const char *to_name) {
    int code = 0;
    do {
        if (syscall(SYS_renameat2, from_directory, from_name, to_directory, to_name, RENAME_NOREPLACE) == 0) {
            return 0;
        }
        code = errno;
    } while (code == EINTR);
    /* A kernel without the call: the engine reports `unsupported` rather than renaming over something. */
    if (code == ENOSYS) {
        return ENOTSUP;
    }
    return code;
}
#else
int bc_file_system_platform_rename_no_replace([[maybe_unused]] int from_directory,
                                              [[maybe_unused]] const char *from_name, [[maybe_unused]] int to_directory,
                                              [[maybe_unused]] const char *to_name) {
    return ENOTSUP;
}
#endif

#else
#error                                                                                                                 \
    "bc-file-system's platform layer has no body for this operating system: the shipped platforms are Apple, Linux (glibc, musl, Android) and Emscripten"
#endif
