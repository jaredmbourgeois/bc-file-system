/* platform.h — the private seam between the engine and the operating system.
 *
 * The one place the core's platform differences live: the probe of the fast paths the running platform offers,
 * and five operations, each with one whole-file-guarded body per platform in `platform.c`, each answering
 * `ENOTSUP` where the fast path is absent so the engine takes its portable path. The resolution layer above
 * never keys a branch on an errno the kernels disagree on. This header is private to `core/src`: it is C23,
 * never a common subset, and nothing outside the core includes it.
 */
#pragma once

/* The toolchain floor, in two parts because the measured floor is uneven: GCC 14 reports 202000L under gnu23
 * while every clang at or above the floor reports 202311L, so the version test is 202000L; Apple clang 16
 * reports 202000L too but has no <stdckdint.h>, so the header test names that refusal. */
#if __STDC_VERSION__ < 202000L || !__has_include(<stdckdint.h>)
#error                                                                                                                 \
    "bc-file-system's core is C23 in the GNU dialect (-std=gnu23, or gnu2x where only that name is known) with <stdckdint.h>: the floor is GCC 14, clang 19, Apple clang 21 (Xcode 27), NDK r28 and Emscripten 2025"
#endif

#include <stddef.h>
#include <stdint.h>

/* ---- The fast paths ------------------------------------------------------------------------------------------ */

/* What the running platform offers beyond the portable path, probed once by the engine's factory and read by the
 * operations that have a fast path. A probe touches no file: on Linux it asks the kernel about an invalid
 * descriptor and reads presence from the errno (any refusal but ENOSYS means the kernel dispatched the call);
 * on Apple platforms the SDK answers, so nothing is probed; on Emscripten everything is absent; on Android
 * openat2 is never probed, because seccomp kills a process that makes the call. */
typedef struct bc_file_system_platform_fast_paths {
    /* openat2 with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS: Linux kernel 5.6 and later, never Android. */
    bool has_openat2;
    /* renameatx_np(RENAME_EXCL) on Apple platforms; renameat2(RENAME_NOREPLACE) on Linux. */
    bool has_rename_no_replace;
    /* clonefileat and fclonefileat on Apple platforms; FICLONE on Linux, where the answer is per volume: the ioctl
     * is in the headers, and a volume without reflinks answers ENOTTY or EOPNOTSUPP at clone time, which the clone
     * body turns into the copy loop. */
    bool has_clone;
    /* copy_file_range, Linux only. */
    bool has_copy_range;
    /* fcntl(F_FULLFSYNC), the durable sync on Apple platforms, where fsync stops at the drive's cache. */
    bool has_full_fsync;
} bc_file_system_platform_fast_paths;

/* Probes every fast path once. Touches no file; may not fail. */
bc_file_system_platform_fast_paths bc_file_system_platform_probe_fast_paths(void);

/* ---- The five operations ------------------------------------------------------------------------------------- */

/* Each operation returns 0 or the errno it failed with, so a call reads `const int code = sync_descriptor(…)`.
 * A name is NUL-terminated text, one component, as the system call takes it: the resolver NUL-terminates each
 * component into its own bounded buffer before calling here. A descriptor is borrowed; nothing here closes one
 * it was given. `sync_descriptor` and `rename_no_replace` are real; the other three answer ENOTSUP until the
 * stage that first needs them (the resolution layer, moves and copies) lands, and say so in `platform.c`. */

/* Pushes one open file or directory to stable storage: fcntl(F_FULLFSYNC) on Apple platforms, where fsync stops
 * at the drive's cache, fsync elsewhere. A volume that offers no sync (EINVAL, ENOTSUP, ENOTTY; every Emscripten
 * backend) answers ENOTSUP so the engine reports `written`. Retries on EINTR. */
int bc_file_system_platform_sync_descriptor(int descriptor);

/* Makes `destination_name` under `destination_directory` a clone of the open `source` with `mode`, where the
 * platform clones (clonefileat and fclonefileat on Apple, FICLONE on Linux): never replaces an existing entry
 * (EEXIST), answers EXDEV across volumes and ENOTSUP where there is no clone, so the engine falls to the copy
 * loop in both cases. */
int bc_file_system_platform_clone_file(int source, int destination_directory, const char *destination_name,
                                       uint32_t mode);

/* Renames `from_name` under `from_directory` to `to_name` under `to_directory` without replacing anything:
 * renameatx_np(RENAME_EXCL) on Apple, the renameat2 system call with RENAME_NOREPLACE on Linux (glibc, musl and
 * Android alike: the raw call, so one body serves three libcs and a musl without the wrapper). EEXIST with the
 * destination untouched when it exists; ENOTSUP where the platform has no such rename (Emscripten; a Linux kernel
 * that answers ENOSYS), which the engine reports as `unsupported`. Retries on EINTR. */
int bc_file_system_platform_rename_no_replace(int from_directory, const char *from_name, int to_directory,
                                              const char *to_name);

/* Opens `relative` beneath `root` in one call with containment enforced by the kernel: openat2 with
 * RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS on Linux when the engine's probe found it (never on Android, where
 * seccomp kills the process), O_NOFOLLOW_ANY on Apple. ENOTSUP where there is no such call, so the engine walks
 * the path one component at a time instead. `relative` is the whole relative path, NUL-terminated. Stores the
 * descriptor on success. */
int bc_file_system_platform_open_beneath(int root, const char *relative, int open_flags, int *descriptor);

/* Copies up to `count` bytes from the current offset of `source` to the current offset of `destination` inside
 * the kernel (copy_file_range) where the platform offers it; ENOTSUP otherwise, so the engine runs the read-and-
 * write loop. Stores how many bytes were copied, which may be fewer than `count`. */
int bc_file_system_platform_copy_range(int source, int destination, size_t count, size_t *copied);
