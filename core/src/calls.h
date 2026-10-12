/* calls.h — every gated system call of the core, one loop each, and the errno table their failures are read with.
 *
 * An operation never makes a system call itself: it calls the function here that is named for the call, and
 * that function asks the observer before every attempt (`before_step`, under the operation's name and the step
 * the call is named by), makes the call, copies the errno on the next line and asks again on EINTR, so a
 * scripted EINTR is retried and counted like a real one. The answer is a `bc_file_system_call_outcome`: the
 * call succeeded, or it failed with a code (real or scripted), or the observer stopped the operation there; an
 * out-parameter is written only on success. Two of the calls (the sync and the no-replace rename) go through
 * the platform layer, whose body retries its own EINTR; a scripted one is retried here, and the sync's step is
 * the one the platform syncs with (`fcntl_fullfsync` where the engine probed it, `fsync` elsewhere), so a test
 * reads what each platform owes. Every name is one component, NUL-terminated; every descriptor is borrowed.
 * The functions have external linkage under the library's prefix because two units call them; they are
 * declared only here. Private to `core/src`: C23, never a common subset.
 */
#pragma once

#include <sys/stat.h>

#include <stddef.h>
#include <stdint.h>

#include <bc_file_system/bc_file_system.h>

#include "engine.h"

/* The kind each errno decides, from the one table of the core: ENOENT `not_found`, ENOTDIR `not_directory`,
 * ELOOP `symlink_refused`, EACCES, EPERM and EROFS `permission_denied`, EEXIST `already_exists`, EISDIR
 * `not_regular_file`, ENOSPC and EDQUOT `no_space`, EFBIG `too_large`, EXDEV `cross_device`, ENOTSUP and
 * EOPNOTSUPP `unsupported`, ENAMETOOLONG `invalid_argument`, EMFILE, ENFILE and ENOMEM `resource_exhausted`,
 * and `io_failed` for everything else, with the errno kept beside it. The resolution layer reads ENOTDIR and
 * EXDEV from a component open against what its stat had seen before coming here. */
int32_t bc_file_system_call_kind_of_errno(int system_code);

/* ---- Looking ------------------------------------------------------------------------------------------------ */

bc_file_system_call_outcome bc_file_system_call_fstatat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                        const char *name, int flags, struct stat *info);
bc_file_system_call_outcome bc_file_system_call_fstat(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                      struct stat *info);
/* The text of the symlink `name` under `parent`, as readlinkat answers it: `*length` bytes stored, not
 * NUL-terminated, and a length equal to `capacity` means the text may have been cut short. */
bc_file_system_call_outcome bc_file_system_call_readlinkat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                           const char *name, uint8_t *buffer, size_t capacity,
                                                           size_t *length);

/* ---- Opening ------------------------------------------------------------------------------------------------ */

/* `openat` without a mode: `flags` must not create. */
bc_file_system_call_outcome bc_file_system_call_openat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                       const char *name, int flags, int *descriptor);
/* `openat` with a mode: `flags` carry O_CREAT, and a file created gets `mode` as the umask leaves it, which is
 * why every operation that creates one sets the mode exactly with `bc_file_system_call_fchmod` afterwards. The
 * step is `openat`, like the open above. */
bc_file_system_call_outcome bc_file_system_call_openat_creating(bc_file_system_engine *engine, int32_t operation,
                                                                int parent, const char *name, int flags, uint32_t mode,
                                                                int *descriptor);

/* ---- Reading and writing ----------------------------------------------------------------------------------- */

/* `pread` of at most `count` bytes at `offset`; `*read_count` 0 means the file ends at `offset`. */
bc_file_system_call_outcome bc_file_system_call_pread(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                      uint8_t *buffer, size_t count, uint64_t offset,
                                                      size_t *read_count);
/* `write` of at most `count` bytes at the descriptor's offset; `*written_count` may be fewer than `count` (a
 * short write, which the caller continues from), and a 0 for a non-zero `count` is the caller's to refuse. */
bc_file_system_call_outcome bc_file_system_call_write(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                      const uint8_t *bytes, size_t count, size_t *written_count);

/* ---- Making a change stick --------------------------------------------------------------------------------- */

/* The step a sync is made under on this engine's platform: `fcntl_fullfsync` where the engine probed
 * F_FULLFSYNC, `fsync` elsewhere. What `bc_file_system_call_sync` asks the observer with, and what an operation
 * reports a failed sync at. */
int32_t bc_file_system_call_sync_step(const bc_file_system_engine *engine);
/* The platform's durable sync of one open file or directory (`bc_file_system_platform_sync_descriptor`), under
 * the step above. ENOTSUP, and the codes a scripted fault may spell for the same fact (EOPNOTSUPP, EINVAL,
 * ENOTTY), mean the volume offers no sync. */
bc_file_system_call_outcome bc_file_system_call_sync(bc_file_system_engine *engine, int32_t operation, int descriptor);
/* `close`, made once and never asked again: after EINTR the descriptor's state is unspecified, and a second
 * close is the double-close class. The descriptor is consumed whatever the answer, so the caller releases it
 * from its owner first. Reported only where its failure matters: it is the last call that can report a write
 * the system deferred, so a durable write makes it a step; a read-only descriptor closes through its owner. */
bc_file_system_call_outcome bc_file_system_call_close(bc_file_system_engine *engine, int32_t operation, int descriptor);
bc_file_system_call_outcome bc_file_system_call_fchmod(bc_file_system_engine *engine, int32_t operation, int descriptor,
                                                       uint32_t mode);
/* `renameat`, which replaces whatever `to_name` holds (a file; an empty directory for a directory). */
bc_file_system_call_outcome bc_file_system_call_renameat(bc_file_system_engine *engine, int32_t operation,
                                                         int from_parent, const char *from_name, int to_parent,
                                                         const char *to_name);
/* The platform's no-replace rename (`bc_file_system_platform_rename_no_replace`, step `rename_no_replace`):
 * EEXIST with the destination untouched when `to_name` holds anything; ENOTSUP where the platform has no such
 * rename. */
bc_file_system_call_outcome bc_file_system_call_rename_no_replace(bc_file_system_engine *engine, int32_t operation,
                                                                  int from_parent, const char *from_name, int to_parent,
                                                                  const char *to_name);
bc_file_system_call_outcome bc_file_system_call_unlinkat(bc_file_system_engine *engine, int32_t operation, int parent,
                                                         const char *name, int flags);
