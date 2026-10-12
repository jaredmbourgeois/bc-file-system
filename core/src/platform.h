/* platform.h — the private seam between the engine and the operating system.
 *
 * The one place the core's platform differences live: the probe of the fast paths the running platform offers,
 * and six operations, each with one whole-file-guarded body per platform in `platform.c`, each answering
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
    /* openat with O_NOFOLLOW_ANY, which refuses a symlink at any depth of the path: Apple platforms, by the SDK. */
    bool has_openat_nofollow_any;
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

/* Whether a held directory descriptor is a kernel object, so that a component opened against it with
 * `O_DIRECTORY | O_NOFOLLOW` is contained beneath it: true on Apple platforms and Linux; false on Emscripten,
 * where a descriptor is a path string the runtime joins names onto, so a held directory goes stale on rename
 * and a symlink on the way escapes, and the resolution layer answers `unsupported` for every `beneath`
 * location rather than weaken the guarantee. A fact of the platform, not a probe. */
bool bc_file_system_platform_enforces_containment(void);

/* The flags that open a directory to make `*at` calls through it, which needs search permission and not read
 * permission, so a directory the process may enter but not list (mode 0300, 0100) is a parent like any other,
 * as the system itself treats it: `O_SEARCH | O_DIRECTORY | O_CLOEXEC` on Apple platforms (where O_SEARCH is
 * O_EXEC | O_DIRECTORY and a 0300 directory opens), `O_PATH | O_DIRECTORY | O_CLOEXEC` on Linux (an O_PATH
 * descriptor serves every `*at` call, `fstat` and `openat` beneath it, and needs no permission at all), and
 * `O_RDONLY | O_DIRECTORY | O_CLOEXEC` on Emscripten, which has neither. A descriptor opened this way cannot be
 * read (`fdopendir` answers EBADF) or synced (`fsync` of an O_PATH descriptor is EBADF on Linux): an operation
 * that lists or syncs a directory opens it, or `.` beneath it, read-only for that. A fact of the platform, not
 * a probe. */
int bc_file_system_platform_directory_search_open_flags(void);

/* Whether the search-only open above refuses a directory the process may not search (Apple's O_SEARCH and a
 * read-only open do; Linux's O_PATH checks nothing), so the resolver knows whether to ask `faccessat(X_OK)` as a
 * step of its own after the open. A fact of the platform. */
bool bc_file_system_platform_search_open_checks_permission(void);

/* The flags `faccessat` takes to judge by the effective ids: AT_EACCESS, or 0 on bionic, which refuses the flag. */
int bc_file_system_platform_access_check_flags(void);

/* Whether the platform's `flock` holds a lock at all: true on Apple platforms and Linux, where an exclusive lock
 * on one open-file description excludes every other; false on Emscripten, whose `flock` is a stub that answers
 * 0 to every caller, so two descriptors both "acquire" and `try_lock` answers `unsupported` before any call
 * rather than hand out a lock that excludes nobody. A fact of the platform, not a probe. */
bool bc_file_system_platform_enforces_locks(void);

/* ---- The six operations -------------------------------------------------------------------------------------- */

/* Each operation returns 0 or the errno it failed with, so a call reads `const int code = sync_descriptor(…)`.
 * A name is NUL-terminated text, one component, as the system call takes it: the resolver NUL-terminates each
 * component into its own bounded buffer before calling here. A descriptor is borrowed; nothing here closes one
 * it was given. All six are real; where a platform has no body for one, that body answers ENOTSUP and says so
 * in `platform.c`. */

/* Pushes one open file or directory to stable storage with fsync, the sync every platform has: the durable sync
 * on Linux, and on Apple platforms the one an engine refused its fast paths makes, which stops at the drive's
 * cache. A volume that offers no sync (EINVAL, ENOTSUP, ENOTTY; every Emscripten backend) answers ENOTSUP so the
 * engine reports `written`. Retries on EINTR. */
int bc_file_system_platform_sync_descriptor(int descriptor);

/* Pushes one open file or directory through the drive's own cache to stable storage: fcntl(F_FULLFSYNC), the
 * durable sync on Apple platforms, where fsync stops at that cache. The engine asks for it wherever the probe
 * found it (`has_full_fsync`); everywhere else this answers ENOTSUP. A volume that offers no full sync answers
 * ENOTSUP so the engine reports `written`. Retries on EINTR. */
int bc_file_system_platform_full_sync_descriptor(int descriptor);

/* Makes `destination_name` under `destination_directory` a clone of the open regular file `source`, in one step
 * that either creates the whole file or creates nothing: fclonefileat on Apple platforms, which creates the file
 * itself with the source's permission bits (`mode` is not read there); FICLONE on Linux, which takes an open
 * destination, so the body creates it exclusively with `mode` as the umask leaves it and takes it back when the
 * ioctl refuses. The engine sets the mode exactly afterwards either way. Never replaces an existing entry
 * (EEXIST, so the engine tries another name); answers EXDEV across volumes and ENOTSUP, EOPNOTSUPP or ENOTTY
 * where the volume or the platform has no clone (Emscripten; a non-APFS volume; an ext4 without reflinks), on
 * which the engine falls to the copy loop. */
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
 * RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS on Linux (the engine calls this only where its probe found openat2,
 * never on Android, where seccomp kills the process; a kernel answering ENOSYS anyway is ENOTSUP), openat with
 * O_NOFOLLOW_ANY on Apple, where the flag refuses a symlink at any depth of the path. A symlink on the way is
 * ELOOP on both; ENOTSUP where there is no such call (Emscripten), so the engine walks the path one component at
 * a time instead, and the engine also walks on EOPNOTSUPP, ENOSYS and EINVAL, which is how a test forces the
 * walk by scripting one of them on the fast path's step. `relative` is the whole relative path, NUL-terminated,
 * never absolute: O_NOFOLLOW_ANY refuses an absolute path at the first system symlink (`/var` on macOS), and
 * RESOLVE_BENEATH refuses it outright. `open_flags` are the engine's directory flags. Stores the descriptor on
 * success. Retries on EINTR. */
int bc_file_system_platform_open_beneath(int root, const char *relative, int open_flags, int *descriptor);

/* Copies up to `count` bytes from the current offset of `source` to the current offset of `destination` inside
 * the kernel, advancing both offsets (the copy_file_range system call on Linux, through syscall(2) so one body
 * serves glibc, musl and bionic); ENOTSUP where the platform has no such call (Apple platforms, Emscripten, a
 * Linux kernel answering ENOSYS) and EXDEV or EINVAL where the kernel refuses these two files, on all of which
 * the engine runs the read-and-write loop from the offset reached. Stores how many bytes were copied, which may
 * be fewer than `count`; 0 means the source ended. Retries on EINTR. */
int bc_file_system_platform_copy_range(int source, int destination, size_t count, size_t *copied);
