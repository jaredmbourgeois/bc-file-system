/* path.c — the path module: the lexical rules of path text, the library's freestanding tier.
 *
 * Bytes in, bytes out, and nothing consulted: not the working directory, the home directory, the environment,
 * the disk or a Unicode table. The unit includes <stddef.h> and <stdint.h> and nothing else, calls no library
 * function and allocates nothing, so it compiles under -ffreestanding and for wasm32 (the two check targets
 * core/CMakeLists.txt builds on every run) as well as on every hosted target. (What the compiler itself emits
 * for a struct copy or zeroing is the toolchain's: an unoptimised hosted object names memcpy and memset for
 * those, which every freestanding environment provides; an optimised one and the bare wasm32 object name
 * nothing.) Every answer of variable length goes into the caller's buffer under the public header's buffer
 * protocol.
 *
 * Three levels of text, each a predicate below. A VALID RELATIVE PATH has one spelling, so equality is a byte
 * compare: components only, none of them `.`, `..` or empty. A VALID ABSOLUTE PATH is the same behind a leading
 * slash, with `..` allowed. LOOSE TEXT is what normalisation accepts: anything non-empty without a NUL whose
 * components fit; normalisation turns it into one of the valid forms (or a relative path that still begins
 * with `..`, which nothing lexical can take further) by dropping `.` and empty components and folding `..`.
 *
 * Normalisation and the relation between two paths both work on loose text through one iterator,
 * next_normalized_component, which yields the components normalisation keeps without writing them anywhere:
 * whether a component survives is decided by a scan of its neighbours (component_survives). That makes the
 * iterator quadratic in the number of components and free of any scratch buffer, the trade this tier makes. A
 * path can also be two spans read as one, "base", a slash, "relative" (path_text), so join and resolve_against
 * never materialise the joined text before the answer is.
 */
#include "path.h"

#include <stddef.h>
#include <stdint.h>

#include <bc_file_system/bc_file_system.h>

/* ---- The return convention ---------------------------------------------------------------------------------- */

/* The same helpers the engine has. The module shares no translation unit with the engine, because it is the
 * freestanding tier, so it carries its own copy. A NULL byte pointer is refused outright by every function
 * here, whatever its length: with a length of 0 it is the empty text, which no rule of the module accepts, and
 * with more it is a contract violation, so the two answers are one. */
static int32_t refuse(bc_file_system_error *error, int32_t kind, int32_t operation) {
    *error = (bc_file_system_error){.kind = kind, .operation = operation};
    return kind;
}

static int32_t succeed(bc_file_system_error *error) {
    *error = (bc_file_system_error){};
    return 0;
}

/* The caller's buffer under the buffer protocol. */
static bool is_answer_buffer(const uint8_t *buffer, size_t capacity, const size_t *length) {
    if (length == nullptr) {
        return false;
    }
    if (buffer != nullptr) {
        return true;
    }
    return capacity == 0;
}

/* The one place the module adds two lengths. `SIZE_MAX - left` cannot underflow, so this line is the whole
 * overflow proof; the header's <stdckdint.h> would say the same and is not among the tier's two headers. */
static bool lengths_fit(size_t left, size_t right, size_t *sum) {
    if (right > SIZE_MAX - left) {
        return false;
    }
    *sum = left + right;
    return true;
}

/* A byte loop rather than memcpy: the tier calls no library function, and under -ffreestanding the compiler
 * will not turn the loop into one either. */
static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool bytes_equal(const uint8_t *left, const uint8_t *right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

/* ---- The text and its bytes --------------------------------------------------------------------------------- */

enum path_kind { path_kind_absolute = 1, path_kind_relative = 2 };

/* One path read from up to two spans: `first`, and when `joined`, a slash and then `second`. Reading through
 * byte_at lets join and resolve_against see "base/relative" without copying it first. `length` is the whole:
 * first_length, plus one and second_length when joined; the caller proved the sum fits (lengths_fit). */
typedef struct path_text {
    const uint8_t *first;
    size_t first_length;
    const uint8_t *second;
    size_t second_length;
    bool joined;
    size_t length;
} path_text;

static path_text single_text(const uint8_t *bytes, size_t length) {
    return (path_text){.first = bytes,
                       .first_length = length,
                       .second = nullptr,
                       .second_length = 0,
                       .joined = false,
                       .length = length};
}

static path_text joined_text(const uint8_t *first, size_t first_length, const uint8_t *second, size_t second_length,
                             size_t whole_length) {
    return (path_text){.first = first,
                       .first_length = first_length,
                       .second = second,
                       .second_length = second_length,
                       .joined = true,
                       .length = whole_length};
}

/* `index` is below `length`, which for a single span is `first_length`, so the second span is read only when
 * the text is joined. */
static uint8_t byte_at(const path_text *text, size_t index) {
    if (!text->joined || index < text->first_length) {
        return text->first[index];
    }
    if (index == text->first_length) {
        return '/';
    }
    return text->second[index - text->first_length - 1];
}

static enum path_kind kind_of(const path_text *text) {
    if (text->length > 0 && byte_at(text, 0) == '/') {
        return path_kind_absolute;
    }
    return path_kind_relative;
}

static bool has_nul(const path_text *text) {
    for (size_t index = 0; index < text->length; ++index) {
        if (byte_at(text, index) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- Raw components ----------------------------------------------------------------------------------------- */

/* The next raw component from `*cursor`: the bytes between two separators, which may be none. `*cursor` is 0
 * before the first call and advances past the separator that ended the component, so after the last one it
 * stands one past the text. The leading slash of an absolute path is the root, not a separator: the root alone
 * has no component, and neither has the empty text. `a/` has two, the second empty, which is how a trailing
 * slash is seen and refused, and `//` has two empty ones behind the root. */
static bool next_raw_component(const path_text *text, size_t *cursor, size_t *offset, size_t *component_length) {
    const bool absolute = kind_of(text) == path_kind_absolute;
    size_t start = *cursor;
    if (start == 0 && absolute) {
        start = 1;
    }
    if (start > text->length) {
        return false;
    }
    if (start == text->length) {
        if (text->length == 0) {
            return false;
        }
        if (absolute && text->length == 1) {
            return false;
        }
        /* Otherwise the byte before `start` is a separator, and the component after it is empty. */
    }
    size_t end = start;
    while (end < text->length && byte_at(text, end) != '/') {
        end += 1;
    }
    *offset = start;
    *component_length = end - start;
    *cursor = end + 1;
    return true;
}

/* What a raw component is to normalisation: nothing to keep (`.` or empty), a climb (`..`), or a name. */
enum component_kind { component_kind_skipped = 1, component_kind_dotdot = 2, component_kind_normal = 3 };

static enum component_kind component_kind_of(const path_text *text, size_t offset, size_t component_length) {
    if (component_length == 0) {
        return component_kind_skipped;
    }
    if (component_length == 1 && byte_at(text, offset) == '.') {
        return component_kind_skipped;
    }
    if (component_length == 2 && byte_at(text, offset) == '.' && byte_at(text, offset + 1) == '.') {
        return component_kind_dotdot;
    }
    return component_kind_normal;
}

/* ---- The three levels of text ------------------------------------------------------------------------------- */

/* Non-empty, no NUL, every component within the capacity. What normalisation accepts. */
static bool is_loose_text(const path_text *text) {
    if (text->length == 0 || has_nul(text)) {
        return false;
    }
    size_t cursor = 0;
    size_t offset = 0;
    size_t component_length = 0;
    while (next_raw_component(text, &cursor, &offset, &component_length)) {
        if (component_length > bc_file_system_path_component_capacity) {
            return false;
        }
    }
    return true;
}

/* Loose text of the given kind whose every component is a name, or `..` where the kind allows it. */
static bool is_valid_text(const path_text *text, enum path_kind kind, bool dotdot_allowed) {
    if (!is_loose_text(text) || kind_of(text) != kind) {
        return false;
    }
    size_t cursor = 0;
    size_t offset = 0;
    size_t component_length = 0;
    while (next_raw_component(text, &cursor, &offset, &component_length)) {
        const enum component_kind component = component_kind_of(text, offset, component_length);
        if (component == component_kind_skipped) {
            return false;
        }
        if (component == component_kind_dotdot && !dotdot_allowed) {
            return false;
        }
    }
    return true;
}

static bool is_valid_relative_text(const path_text *text) {
    return is_valid_text(text, path_kind_relative, false);
}

static bool is_valid_absolute_text(const path_text *text) {
    return is_valid_text(text, path_kind_absolute, true);
}

static bool is_valid_path_text(const path_text *text) {
    if (is_valid_relative_text(text)) {
        return true;
    }
    return is_valid_absolute_text(text);
}

/* One entry name on its own: within the capacity, no separator, no NUL. `.` and `..` are names here; the path
 * rules above are what refuse them inside a path. */
static bool is_component(const uint8_t *bytes, size_t length) {
    if (length == 0 || length > bc_file_system_path_component_capacity) {
        return false;
    }
    for (size_t index = 0; index < length; ++index) {
        if (bytes[index] == '/' || bytes[index] == 0) {
            return false;
        }
    }
    return true;
}

/* ---- Normalisation without a buffer ------------------------------------------------------------------------- */

/* Whether normalisation keeps the raw component at `offset`. A `.` or empty component never survives. A name
 * survives unless a later `..` folds it away: scanning forward, each later name shields it once and each `..`
 * spends a shield, so the first `..` that finds no shield is the one that folds it. A `..` survives only in a
 * relative path, and only when nothing before it is left to fold: the same count run from the start, names
 * raising it and `..` lowering it, must stand at zero when the `..` is reached. (At the root a `..` has nothing
 * to climb to and is dropped: `/..` is `/`.) */
static bool component_survives(const path_text *text, size_t offset, size_t component_length) {
    const enum component_kind kind = component_kind_of(text, offset, component_length);
    if (kind == component_kind_skipped) {
        return false;
    }
    size_t cursor = 0;
    size_t other_offset = 0;
    size_t other_length = 0;
    if (kind == component_kind_normal) {
        cursor = offset + component_length + 1;
        size_t shields = 0;
        while (next_raw_component(text, &cursor, &other_offset, &other_length)) {
            const enum component_kind later = component_kind_of(text, other_offset, other_length);
            if (later == component_kind_normal) {
                shields += 1;
            } else if (later == component_kind_dotdot) {
                if (shields == 0) {
                    return false;
                }
                shields -= 1;
            }
        }
        return true;
    }
    if (kind_of(text) == path_kind_absolute) {
        return false;
    }
    size_t depth = 0;
    while (next_raw_component(text, &cursor, &other_offset, &other_length) && other_offset != offset) {
        const enum component_kind earlier = component_kind_of(text, other_offset, other_length);
        if (earlier == component_kind_normal) {
            depth += 1;
        } else if (earlier == component_kind_dotdot && depth > 0) {
            depth -= 1;
        }
    }
    return depth == 0;
}

/* The components normalisation keeps, in the order they stand in the text, which is the order they take in the
 * answer: a kept `..` always precedes every kept name, since a name before it would have been its shield. */
static bool next_normalized_component(const path_text *text, size_t *cursor, size_t *offset, size_t *component_length) {
    while (next_raw_component(text, cursor, offset, component_length)) {
        if (component_survives(text, *offset, *component_length)) {
            return true;
        }
    }
    return false;
}

/* The normalised spelling's length: a slash, then the kept components with a slash between each pair, for an
 * absolute path; the kept components alone for a relative one, or `.` when none is kept. Never longer than the
 * loose text, which held every kept byte and separator, so the sum cannot overflow. */
static size_t normalized_length(const path_text *text) {
    const bool absolute = kind_of(text) == path_kind_absolute;
    size_t total = 0;
    if (absolute) {
        total = 1;
    }
    size_t kept = 0;
    size_t cursor = 0;
    size_t offset = 0;
    size_t component_length = 0;
    while (next_normalized_component(text, &cursor, &offset, &component_length)) {
        if (kept > 0) {
            total += 1;
        }
        total += component_length;
        kept += 1;
    }
    if (kept == 0 && !absolute) {
        return 1;
    }
    return total;
}

/* The normalised spelling into `buffer`, which holds normalized_length(text) bytes. */
static void write_normalized(const path_text *text, uint8_t *buffer) {
    const bool absolute = kind_of(text) == path_kind_absolute;
    size_t written = 0;
    if (absolute) {
        buffer[written] = '/';
        written += 1;
    }
    size_t kept = 0;
    size_t cursor = 0;
    size_t offset = 0;
    size_t component_length = 0;
    while (next_normalized_component(text, &cursor, &offset, &component_length)) {
        if (kept > 0) {
            buffer[written] = '/';
            written += 1;
        }
        for (size_t index = 0; index < component_length; ++index) {
            buffer[written] = byte_at(text, offset + index);
            written += 1;
        }
        kept += 1;
    }
    if (kept == 0 && !absolute) {
        buffer[written] = '.';
    }
}

static bool components_equal(const path_text *text, size_t offset, size_t component_length, const path_text *other,
                             size_t other_offset, size_t other_length) {
    if (component_length != other_length) {
        return false;
    }
    for (size_t index = 0; index < component_length; ++index) {
        if (byte_at(text, offset + index) != byte_at(other, other_offset + index)) {
            return false;
        }
    }
    return true;
}

/* Where `path` stands with respect to `other` once both are normalised, component by component. Two kinds
 * never compare, and a relative path that still begins with `..` names something no lexical rule can place;
 * only the first kept component need be looked at for that, since a kept `..` always leads. */
static int32_t relation_of(const path_text *path, const path_text *other) {
    if (kind_of(path) != kind_of(other)) {
        return bc_file_system_path_relation_undecidable;
    }
    size_t path_cursor = 0;
    size_t path_offset = 0;
    size_t path_length = 0;
    size_t other_cursor = 0;
    size_t other_offset = 0;
    size_t other_length = 0;
    bool path_present = next_normalized_component(path, &path_cursor, &path_offset, &path_length);
    bool other_present = next_normalized_component(other, &other_cursor, &other_offset, &other_length);
    if (path_present && component_kind_of(path, path_offset, path_length) == component_kind_dotdot) {
        return bc_file_system_path_relation_undecidable;
    }
    if (other_present && component_kind_of(other, other_offset, other_length) == component_kind_dotdot) {
        return bc_file_system_path_relation_undecidable;
    }
    for (;;) {
        if (!path_present && !other_present) {
            return bc_file_system_path_relation_same;
        }
        if (!path_present) {
            /* `other` goes on below `path`: `path` is above it, which is not inside. */
            return bc_file_system_path_relation_outside;
        }
        if (!other_present) {
            return bc_file_system_path_relation_inside;
        }
        if (!components_equal(path, path_offset, path_length, other, other_offset, other_length)) {
            return bc_file_system_path_relation_outside;
        }
        path_present = next_normalized_component(path, &path_cursor, &path_offset, &path_length);
        other_present = next_normalized_component(other, &other_cursor, &other_offset, &other_length);
    }
}

/* ---- Components: stem, extension, library names ------------------------------------------------------------- */

/* The dot that starts the extension: the last one, when it is neither the first byte (`.bashrc` has no
 * extension) nor the last (`archive.` has none). */
static bool extension_dot_of(const uint8_t *component, size_t length, size_t *dot) {
    size_t index = length;
    while (index > 0) {
        index -= 1;
        if (component[index] == '.') {
            if (index == 0 || index == length - 1) {
                return false;
            }
            *dot = index;
            return true;
        }
    }
    return false;
}

static bool is_digit(uint8_t byte) {
    if (byte < '0') {
        return false;
    }
    return byte <= '9';
}

/* Empty, or one or more groups of a dot and at least one digit: what may follow `.so`. */
static bool is_version_suffix(const uint8_t *bytes, size_t length) {
    size_t index = 0;
    while (index < length) {
        if (bytes[index] != '.') {
            return false;
        }
        index += 1;
        if (index == length || !is_digit(bytes[index])) {
            return false;
        }
        while (index < length && is_digit(bytes[index])) {
            index += 1;
        }
    }
    return true;
}

/* `prefix` + at least one byte of stem + `suffix`, spelled byte for byte. */
static bool is_library_spelling(const uint8_t *component, size_t length, const uint8_t *prefix, size_t prefix_length,
                                const uint8_t *suffix, size_t suffix_length, size_t *stem_offset, size_t *stem_length) {
    if (length <= prefix_length + suffix_length) {
        return false;
    }
    if (!bytes_equal(component, prefix, prefix_length)) {
        return false;
    }
    if (!bytes_equal(component + (length - suffix_length), suffix, suffix_length)) {
        return false;
    }
    *stem_offset = prefix_length;
    *stem_length = length - prefix_length - suffix_length;
    return true;
}

/* `libX.so` with an optional `.N.N…` after it. The `.so` is the first one whose tail is a version suffix;
 * there can be only one, since a tail of `.so…` is never all digits. */
static bool is_versioned_shared_object(const uint8_t *component, size_t length, size_t *stem_offset,
                                       size_t *stem_length) {
    static const uint8_t prefix[] = "lib";
    static const uint8_t marker[] = ".so";
    const size_t prefix_length = sizeof prefix - 1;
    const size_t marker_length = sizeof marker - 1;
    if (length <= prefix_length + marker_length || !bytes_equal(component, prefix, prefix_length)) {
        return false;
    }
    for (size_t at = prefix_length + 1; at + marker_length <= length; ++at) {
        if (!bytes_equal(component + at, marker, marker_length)) {
            continue;
        }
        const size_t tail = at + marker_length;
        if (is_version_suffix(component + tail, length - tail)) {
            *stem_offset = prefix_length;
            *stem_length = at - prefix_length;
            return true;
        }
    }
    return false;
}

static int32_t library_kind_of(const uint8_t *component, size_t length, size_t *stem_offset, size_t *stem_length) {
    static const uint8_t lib[] = "lib";
    static const uint8_t dylib[] = ".dylib";
    static const uint8_t archive[] = ".a";
    static const uint8_t framework[] = ".framework";
    static const uint8_t dll[] = ".dll";
    if (is_versioned_shared_object(component, length, stem_offset, stem_length)) {
        return bc_file_system_library_kind_shared;
    }
    if (is_library_spelling(component, length, lib, sizeof lib - 1, dylib, sizeof dylib - 1, stem_offset,
                            stem_length)) {
        return bc_file_system_library_kind_shared;
    }
    if (is_library_spelling(component, length, lib, sizeof lib - 1, archive, sizeof archive - 1, stem_offset,
                            stem_length)) {
        return bc_file_system_library_kind_static;
    }
    if (is_library_spelling(component, length, nullptr, 0, framework, sizeof framework - 1, stem_offset, stem_length)) {
        return bc_file_system_library_kind_framework;
    }
    if (is_library_spelling(component, length, nullptr, 0, dll, sizeof dll - 1, stem_offset, stem_length)) {
        return bc_file_system_library_kind_shared;
    }
    return bc_file_system_library_kind_none;
}

/* ---- The engine's face -------------------------------------------------------------------------------------- */

bool bc_file_system_path_text_is_valid_relative(const uint8_t *path, size_t length) {
    if (path == nullptr) {
        return false;
    }
    const path_text text = single_text(path, length);
    return is_valid_relative_text(&text);
}

bool bc_file_system_path_text_is_valid_absolute(const uint8_t *path, size_t length) {
    if (path == nullptr) {
        return false;
    }
    const path_text text = single_text(path, length);
    return is_valid_absolute_text(&text);
}

bool bc_file_system_path_text_next_component(const uint8_t *path, size_t length, size_t *cursor, size_t *offset,
                                             size_t *component_length) {
    const path_text text = single_text(path, length);
    return next_raw_component(&text, cursor, offset, component_length);
}

/* ---- The public functions ----------------------------------------------------------------------------------- */

int32_t bc_file_system_relative_path_validate(const uint8_t *path, size_t length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_relative_path_from;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (!bc_file_system_path_text_is_valid_relative(path, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    return succeed(error);
}

int32_t bc_file_system_absolute_path_validate(const uint8_t *path, size_t length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_absolute_path_from;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (!bc_file_system_path_text_is_valid_absolute(path, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    return succeed(error);
}

static bool is_root(const uint8_t *path, size_t length) {
    if (length != 1) {
        return false;
    }
    return path[0] == '/';
}

int32_t bc_file_system_path_join(const uint8_t *base, size_t base_length, const uint8_t *relative,
                                 size_t relative_length, uint8_t *buffer, size_t capacity, size_t *length,
                                 bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_join;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (base == nullptr || relative == nullptr || !is_answer_buffer(buffer, capacity, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const path_text base_text = single_text(base, base_length);
    const path_text relative_text = single_text(relative, relative_length);
    if (!is_valid_path_text(&base_text) || !is_valid_relative_text(&relative_text)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    /* The root already ends in its slash: "/" and "a" join as "/a", never "//a". */
    const bool base_is_root = is_root(base, base_length);
    size_t separator_length = 1;
    if (base_is_root) {
        separator_length = 0;
    }
    size_t needed = 0;
    if (!lengths_fit(base_length, separator_length, &needed) || !lengths_fit(needed, relative_length, &needed)) {
        return refuse(error, bc_file_system_error_kind_too_large, operation);
    }
    if (needed > capacity) {
        *length = needed;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    copy_bytes(buffer, base, base_length);
    if (!base_is_root) {
        buffer[base_length] = '/';
    }
    copy_bytes(buffer + base_length + separator_length, relative, relative_length);
    *length = needed;
    return succeed(error);
}

/* The index of the last separator in a valid path, or false when it has none. The root's slash counts: for
 * `/a` it is at 0, and the parent of `/a` is `/`. */
static bool last_separator_of(const uint8_t *path, size_t length, size_t *separator) {
    size_t index = length;
    while (index > 0) {
        index -= 1;
        if (path[index] == '/') {
            *separator = index;
            return true;
        }
    }
    return false;
}

int32_t bc_file_system_path_parent(const uint8_t *path, size_t length, int32_t *present, size_t *parent_length,
                                   bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_parent;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (path == nullptr || present == nullptr || parent_length == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const path_text text = single_text(path, length);
    if (!is_valid_path_text(&text)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t separator = 0;
    if (!last_separator_of(path, length, &separator) || length == 1) {
        /* A one-component relative path, or the root. */
        *present = 0;
        return succeed(error);
    }
    *present = 1;
    *parent_length = separator == 0 ? 1 : separator;
    return succeed(error);
}

int32_t bc_file_system_path_last_component(const uint8_t *path, size_t length, size_t *offset, size_t *component_length,
                                           bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_last_component;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (path == nullptr || offset == nullptr || component_length == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const path_text text = single_text(path, length);
    if (!is_valid_path_text(&text) || is_root(path, length)) {
        /* The root is a valid path with no component to name, so it is refused here. */
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t separator = 0;
    size_t start = 0;
    if (last_separator_of(path, length, &separator)) {
        start = separator + 1;
    }
    *offset = start;
    *component_length = length - start;
    return succeed(error);
}

/* A cursor the iterator produced, or 0: one past the text, or just after a separator. */
static bool is_cursor_of(const uint8_t *path, size_t length, size_t cursor) {
    if (cursor == 0) {
        return true;
    }
    if (cursor - 1 == length) {
        return true;
    }
    if (cursor > length) {
        return false;
    }
    return path[cursor - 1] == '/';
}

int32_t bc_file_system_path_next_component(const uint8_t *path, size_t length, size_t *cursor, size_t *offset,
                                           size_t *component_length, int32_t *present, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_next_component;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (path == nullptr || cursor == nullptr || offset == nullptr || component_length == nullptr ||
        present == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const path_text text = single_text(path, length);
    if (!is_valid_path_text(&text) || !is_cursor_of(path, length, *cursor)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    *present = 0;
    if (bc_file_system_path_text_next_component(path, length, cursor, offset, component_length)) {
        *present = 1;
    }
    return succeed(error);
}

int32_t bc_file_system_path_stem(const uint8_t *component, size_t length, size_t *stem_length,
                                 bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_stem;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (component == nullptr || stem_length == nullptr || !is_component(component, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t dot = 0;
    *stem_length = length;
    if (extension_dot_of(component, length, &dot)) {
        *stem_length = dot;
    }
    return succeed(error);
}

int32_t bc_file_system_path_extension(const uint8_t *component, size_t length, size_t *offset, size_t *extension_length,
                                      int32_t *present, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_extension;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (component == nullptr || offset == nullptr || extension_length == nullptr || present == nullptr ||
        !is_component(component, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t dot = 0;
    if (!extension_dot_of(component, length, &dot)) {
        *present = 0;
        return succeed(error);
    }
    *present = 1;
    *offset = dot + 1;
    *extension_length = length - dot - 1;
    return succeed(error);
}

int32_t bc_file_system_path_relate(const uint8_t *path, size_t length, const uint8_t *other, size_t other_length,
                                   int32_t *relation, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_relate;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (path == nullptr || other == nullptr || relation == nullptr) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const path_text subject_text = single_text(path, length);
    const path_text other_text = single_text(other, other_length);
    if (!is_loose_text(&subject_text) || !is_loose_text(&other_text)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    *relation = relation_of(&subject_text, &other_text);
    return succeed(error);
}

int32_t bc_file_system_path_lexically_normalize(const uint8_t *path, size_t length, uint8_t *buffer, size_t capacity,
                                                size_t *normalized, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_lexically_normalize;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (path == nullptr || !is_answer_buffer(buffer, capacity, normalized)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const path_text text = single_text(path, length);
    if (!is_loose_text(&text)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const size_t needed = normalized_length(&text);
    if (needed > capacity) {
        *normalized = needed;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    write_normalized(&text, buffer);
    *normalized = needed;
    return succeed(error);
}

int32_t bc_file_system_path_resolve_against(const uint8_t *absolute_base, size_t base_length, const uint8_t *relative,
                                            size_t relative_length, uint8_t *buffer, size_t capacity, size_t *length,
                                            bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_resolve_against;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (absolute_base == nullptr || relative == nullptr || !is_answer_buffer(buffer, capacity, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    const path_text base_text = single_text(absolute_base, base_length);
    const path_text relative_text = single_text(relative, relative_length);
    if (!is_valid_absolute_text(&base_text) || !is_loose_text(&relative_text) ||
        kind_of(&relative_text) != path_kind_relative) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t whole_length = 0;
    if (!lengths_fit(base_length, 1, &whole_length) || !lengths_fit(whole_length, relative_length, &whole_length)) {
        return refuse(error, bc_file_system_error_kind_too_large, operation);
    }
    const path_text joined = joined_text(absolute_base, base_length, relative, relative_length, whole_length);
    const size_t needed = normalized_length(&joined);
    if (needed > capacity) {
        *length = needed;
        return refuse(error, bc_file_system_error_kind_buffer_too_small, operation);
    }
    write_normalized(&joined, buffer);
    *length = needed;
    return succeed(error);
}

int32_t bc_file_system_path_library_name(const uint8_t *component, size_t length, int32_t *kind, size_t *stem_offset,
                                         size_t *stem_length, bc_file_system_error *error) {
    const int32_t operation = bc_file_system_operation_path_library_name;
    if (error == nullptr) {
        return bc_file_system_error_kind_invalid_argument;
    }
    if (component == nullptr || kind == nullptr || stem_offset == nullptr || stem_length == nullptr ||
        !is_component(component, length)) {
        return refuse(error, bc_file_system_error_kind_invalid_argument, operation);
    }
    size_t found_offset = 0;
    size_t found_length = 0;
    const int32_t found_kind = library_kind_of(component, length, &found_offset, &found_length);
    *kind = found_kind;
    if (found_kind != bc_file_system_library_kind_none) {
        *stem_offset = found_offset;
        *stem_length = found_length;
    }
    return succeed(error);
}
