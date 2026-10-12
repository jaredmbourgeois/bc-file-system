/* owned.h — the one cleanup idiom of the core: an owned resource in a struct that `[[gnu::cleanup]]` releases.
 *
 * A function that opens a descriptor, a directory stream or a scratch buffer declares it as one of the three
 * structs below with the matching cleanup attribute, and every return path releases it without a `goto` and
 * without a hand-written close at each exit. A value leaves its owner only through `release_*`, which clears
 * the owner so the cleanup sees nothing to do; a caller never writes the owner's field by hand, because the
 * next reader could not tell a transfer from a leak. `adopt_descriptor` is the walk's step: an owner gives up
 * what it held and takes another owner's value. Private to `core/src`: C23, never a common subset; every
 * function is `static inline` and unprefixed because none has external linkage.
 */
#pragma once

#include <dirent.h>
#include <unistd.h>

#include <stddef.h>
#include <stdlib.h>

/* ---- A descriptor ------------------------------------------------------------------------------------------ */

typedef struct owned_descriptor {
    int value; /* -1 once closed or released */
} owned_descriptor;

/* The cleanup. The result of close is discarded on purpose: what the core owns through this struct is read-only
 * (a directory, a file being inspected), and nothing a read-only close could report changes a result already
 * decided; a close whose result matters (the last call of a durable write) is spelled out by its operation. */
static inline void close_owned_descriptor(owned_descriptor *owned) {
    if (owned->value >= 0) {
        (void)close(owned->value); /* discard-ok: a read-only descriptor */
        owned->value = -1;
    }
}

/* The only way a descriptor leaves its owner: the owner is cleared here, never at a call site. */
static inline int release_descriptor(owned_descriptor *owned) {
    const int descriptor = owned->value;
    owned->value = -1;
    return descriptor;
}

/* `owner` closes what it held and takes what `incoming` held, which leaves `incoming` empty: one step of a
 * walk that holds one parent at a time. */
static inline void adopt_descriptor(owned_descriptor *owner, owned_descriptor *incoming) {
    close_owned_descriptor(owner);
    owner->value = release_descriptor(incoming);
}

/* ---- A directory stream (the listing stages) --------------------------------------------------------------- */

typedef struct owned_directory_stream {
    DIR *value; /* NULL once closed or released; closing it closes the descriptor fdopendir was given */
} owned_directory_stream;

static inline void close_owned_directory_stream(owned_directory_stream *owned) {
    if (owned->value != nullptr) {
        (void)closedir(owned->value); /* discard-ok: a read-only stream */
        owned->value = nullptr;
    }
}

static inline DIR *release_directory_stream(owned_directory_stream *owned) {
    DIR *stream = owned->value;
    owned->value = nullptr;
    return stream;
}

/* ---- A bounded scratch buffer (the read and listing stages) ------------------------------------------------ */

typedef struct owned_buffer {
    void *value; /* NULL once freed or released */
} owned_buffer;

static inline void free_owned_buffer(owned_buffer *owned) {
    free(owned->value);
    owned->value = nullptr;
}

static inline void *release_buffer(owned_buffer *owned) {
    void *buffer = owned->value;
    owned->value = nullptr;
    return buffer;
}
