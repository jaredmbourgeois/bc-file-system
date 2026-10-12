/* harness.h — what the two engine fuzzers share: the trap, the reader of fuzzed bytes, the root each makes once
 * and the descriptor count that proves nothing leaked.
 *
 * libFuzzer owns `main`; each fuzzer defines LLVMFuzzerInitialize, which makes its root once under the
 * directory the build handed it (the generated fuzz_paths.h, never the system's temporary directory), and
 * LLVMFuzzerTestOneInput over that root. The engine under test is handed the root as a held directory, so
 * every `beneath` location is contained by the kernel and nothing a fuzzed input spells can reach outside it.
 * Everything here is `static inline` and unprefixed: nothing has external linkage. A reach (mkdir, fcntl,
 * /proc) is legal here because the fuzzer is a test and the root is its own (AGENTS.md, "Inject, never
 * reach", site 3).
 */
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <bc_file_system/bc_file_system.h>

#include "fuzz_paths.h"

/* The one way an invariant fails: a trap the sanitizer reports with the input that caused it. `held` is an int
 * because a C comparison is one, and so is a chain of them under &&. */
static inline void require(int held) {
    if (!held) {
        __builtin_trap();
    }
}

/* ---- The fuzzed bytes, read as values ----------------------------------------------------------------------- */

/* The input, consumed from the front; a reader past its end answers zero, so an input that ends early still
 * drives a whole call with the quietest arguments. */
typedef struct reader {
    const uint8_t *data;
    size_t size;
    size_t position;
} reader;

static inline uint8_t take_byte(reader *input) {
    if (input->position >= input->size) {
        return 0;
    }
    const uint8_t byte = input->data[input->position];
    input->position += 1;
    return byte;
}

static inline uint64_t take_uint64(reader *input) {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= (uint64_t)take_byte(input) << shift;
    }
    return value;
}

static inline int32_t take_int32(reader *input) {
    uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= (uint32_t)take_byte(input) << shift;
    }
    return (int32_t)value;
}

static inline bool take_flag(reader *input) {
    return (take_byte(input) & 1) != 0;
}

/* A value of a closed enum: in range most of the time (1 to `last`), any integer at all one time in eight, so
 * zero, a negative number and one past the end each reach the argument check. */
static inline int32_t take_enum(reader *input, int32_t last) {
    const uint8_t choice = take_byte(input);
    if ((choice & 7) == 0) {
        return take_int32(input);
    }
    return 1 + (int32_t)((choice >> 3) % (uint8_t)last);
}

/* A span of the input: a length byte, then that many bytes (fewer at the input's end). Borrowed from the input. */
static inline const uint8_t *take_span(reader *input, size_t *length) {
    size_t wanted = take_byte(input);
    const size_t remaining = input->size - input->position;
    if (wanted > remaining) {
        wanted = remaining;
    }
    const uint8_t *start = input->data + input->position;
    input->position += wanted;
    *length = wanted;
    return start;
}

/* ---- The root ------------------------------------------------------------------------------------------------ */

/* Spells `value` in decimal at `text`, answering how many bytes it wrote. */
static inline size_t spell_decimal(char *text, uint64_t value) {
    char reversed[20];
    size_t digits = 0;
    do {
        reversed[digits] = (char)('0' + (int)(value % 10));
        digits += 1;
        value /= 10;
    } while (value > 0);
    for (size_t index = 0; index < digits; ++index) {
        text[index] = reversed[digits - 1 - index];
    }
    return digits;
}

/* Makes `<handed root>/<label>-<pid>-<n>` for the first `n` that is free, NUL-terminated into `text` (which
 * holds `capacity` bytes), and answers its length. A fuzzer dies here rather than run without a root. */
static inline size_t make_fuzz_root(const char *label, char *text, size_t capacity) {
    const size_t handed_length = strlen(bc_file_system_fuzz_temporary_root);
    const size_t label_length = strlen(label);
    require(handed_length + 1 + label_length + 1 + 20 + 1 + 20 + 1 <= capacity);
    for (uint64_t attempt = 1; attempt <= 64; ++attempt) {
        size_t position = 0;
        memcpy(text, bc_file_system_fuzz_temporary_root, handed_length);
        position += handed_length;
        text[position] = '/';
        position += 1;
        memcpy(text + position, label, label_length);
        position += label_length;
        text[position] = '-';
        position += 1;
        position += spell_decimal(text + position, (uint64_t)getpid());
        text[position] = '-';
        position += 1;
        position += spell_decimal(text + position, attempt);
        text[position] = 0;
        if (mkdir(text, 0700) == 0) {
            return position;
        }
    }
    __builtin_trap();
}

/* ---- The descriptor count --------------------------------------------------------------------------------- */

/* The path a descriptor is open on, or false for one that is closed or names no path (a pipe, a socket: the
 * sanitizer's symbolizer holds a few of those, which is why the count below reads paths rather than numbers).
 * A descriptor on an entry that was unlinked still answers its path on both platforms. Whole-function
 * alternatives: F_GETPATH on Apple platforms, the link under /proc/self/fd on Linux. */
#ifdef __APPLE__
static inline bool descriptor_path(int descriptor, char *path) {
    return fcntl(descriptor, F_GETPATH, path) == 0;
}
#elifdef __linux__
static inline bool descriptor_path(int descriptor, char *path) {
    char link[32] = "/proc/self/fd/";
    const size_t prefix_length = strlen(link);
    const size_t digits = spell_decimal(link + prefix_length, (uint64_t)descriptor);
    link[prefix_length + digits] = 0;
    const ssize_t length = readlink(link, path, PATH_MAX - 1);
    if (length < 0) {
        return false;
    }
    path[length] = 0;
    return true;
}
#else
#error "the engine fuzzers read a descriptor's path on Apple platforms and Linux only"
#endif

/* How many descriptors below the sweep limit are open on a path under `root`: everything the engine opens is
 * (an entry under the root, or `.` beneath one), so a leaked descriptor is one more here, while what the
 * fuzzing runtime opens for itself (its symbolizer's pipes) is never counted. Taken once the harness's own
 * handles are open and compared after every input, with no `getdtablesize` and no count of the whole table. */
enum : int { descriptor_sweep_limit = 256 };

static inline int descriptors_under(const char *root, size_t root_length) {
    int count = 0;
    for (int descriptor = 0; descriptor < descriptor_sweep_limit; ++descriptor) {
        char path[PATH_MAX];
        if (!descriptor_path(descriptor, path)) {
            continue;
        }
        if (strncmp(path, root, root_length) == 0) {
            count += 1;
        }
    }
    return count;
}
