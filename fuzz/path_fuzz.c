/* path_fuzz.c — libFuzzer over the path module: bytes in, invariants out.
 *
 * libFuzzer owns `main` and calls LLVMFuzzerTestOneInput with each input; this file defines nothing else that is
 * reachable from outside. The input is cut into two slices at a point its first byte chooses, and each slice is
 * run through every public path function with the invariants below asserted by a trap, so a violation is a
 * crash the sanitizers attribute. The module is the freestanding tier, so the fuzzer links the hosted core
 * only for its one translation unit; nothing here touches the disk.
 *
 * The invariants:
 *   - nothing reads past a slice's length (the sanitized build's redzones catch it);
 *   - a path that validates has a parent and a last component that join back into it (when that component
 *     is a name: an absolute path may end in `..`, which is no relative path to join), and its components
 *     joined by a slash spell it;
 *   - lexically_normalize is idempotent, answers the size it then uses, writes nothing when the buffer is too
 *     small, and its answer is a valid path of the text's kind (or a relative path led by `..`, or `.`);
 *   - resolve_against answers a valid absolute path that normalises to itself;
 *   - relate is the same as relating the two normalised texts, `same` and `undecidable` read the same from
 *     both sides, `inside` from one side is `outside` from the other, and a text relates to itself as `same`
 *     unless it climbs;
 *   - a component's stem, a dot and its extension spell the component, and a library name's stem lies inside it.
 */
#include <bc_file_system/bc_file_system.h>

#include <stddef.h>
#include <stdint.h>

/* Inputs longer than this are not interesting: every rule is about components and separators, not length. The
 * answer buffers are sized so that the longest answer any function can give for such an input fits. */
enum : uint32_t { longest_input = 4096, answer_capacity = (2 * longest_input) + 2 };

/* The one way an invariant fails: a trap the sanitizer reports with the input that caused it. `held` is an int
 * because a C comparison is one, and so is a chain of them under &&. */
static inline void require(int held) {
    if (!held) {
        __builtin_trap();
    }
}

static bool same_bytes(const uint8_t *left, size_t left_length, const uint8_t *right, size_t right_length) {
    if (left_length != right_length) {
        return false;
    }
    for (size_t index = 0; index < left_length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool starts_with_dotdot(const uint8_t *bytes, size_t length) {
    if (length < 2 || bytes[0] != '.' || bytes[1] != '.') {
        return false;
    }
    if (length == 2) {
        return true;
    }
    return bytes[2] == '/';
}

/* The invariants of a text that validates as a path of either kind. */
static void check_valid_path(const uint8_t *path, size_t length, bool absolute) {
    bc_file_system_error error;
    int32_t present = 0;
    size_t parent_length = 0;
    require(bc_file_system_path_parent(path, length, &present, &parent_length, &error) == 0);
    if (present == 1) {
        size_t offset = 0;
        size_t component_length = 0;
        require(bc_file_system_path_last_component(path, length, &offset, &component_length, &error) == 0);
        require(offset + component_length == length);
        /* join(parent, last) round-trips, when the last component is a name rather than a climb. */
        bool last_is_dotdot = false;
        if (component_length == 2 && path[offset] == '.' && path[offset + 1] == '.') {
            last_is_dotdot = true;
        }
        if (!last_is_dotdot) {
            uint8_t joined[answer_capacity];
            size_t joined_length = 0;
            require(bc_file_system_path_join(path, parent_length, path + offset, component_length, joined,
                                             sizeof joined, &joined_length, &error) == 0);
            require(same_bytes(joined, joined_length, path, length));
        }
    }
    /* The components, joined by a slash, spell the path. */
    uint8_t spelled[answer_capacity];
    size_t spelled_length = 0;
    if (absolute) {
        spelled[0] = '/';
        spelled_length = 1;
    }
    size_t cursor = 0;
    size_t seen = 0;
    for (;;) {
        size_t offset = 0;
        size_t component_length = 0;
        int32_t component_present = 0;
        require(bc_file_system_path_next_component(path, length, &cursor, &offset, &component_length,
                                                   &component_present, &error) == 0);
        if (component_present == 0) {
            break;
        }
        require(offset + component_length <= length);
        require(component_length >= 1 && component_length <= 255);
        if (seen > 0) {
            spelled[spelled_length] = '/';
            spelled_length += 1;
        }
        for (size_t index = 0; index < component_length; ++index) {
            spelled[spelled_length] = path[offset + index];
            spelled_length += 1;
        }
        seen += 1;
        /* The stem, a dot and the extension spell the component; a library stem lies inside it. */
        size_t stem_length = 0;
        require(bc_file_system_path_stem(path + offset, component_length, &stem_length, &error) == 0);
        size_t extension_offset = 0;
        size_t extension_length = 0;
        int32_t extension_present = 0;
        require(bc_file_system_path_extension(path + offset, component_length, &extension_offset, &extension_length,
                                              &extension_present, &error) == 0);
        if (extension_present == 1) {
            require(stem_length + 1 + extension_length == component_length);
            require(extension_offset == stem_length + 1);
        } else {
            require(stem_length == component_length);
        }
        int32_t kind = 0;
        size_t library_stem_offset = 0;
        size_t library_stem_length = 0;
        require(bc_file_system_path_library_name(path + offset, component_length, &kind, &library_stem_offset,
                                                 &library_stem_length, &error) == 0);
        if (kind != bc_file_system_library_kind_none) {
            require(library_stem_length >= 1);
            require(library_stem_offset + library_stem_length <= component_length);
        }
    }
    require(same_bytes(spelled, spelled_length, path, length));
}

/* The invariants of lexically_normalize over loose text: the answer, when there is one, in `normalized`. */
static bool check_normalize(const uint8_t *text, size_t length, uint8_t *normalized, size_t *normalized_length) {
    bc_file_system_error error;
    size_t needed = 0;
    const int32_t sized = bc_file_system_path_lexically_normalize(text, length, nullptr, 0, &needed, &error);
    if (sized == bc_file_system_error_kind_invalid_argument) {
        return false;
    }
    require(sized == bc_file_system_error_kind_buffer_too_small);
    require(needed >= 1 && needed <= length + 1);
    /* One byte short writes nothing. */
    normalized[0] = 0xFF;
    size_t short_length = 0;
    require(bc_file_system_path_lexically_normalize(text, length, normalized, needed - 1, &short_length, &error) ==
            bc_file_system_error_kind_buffer_too_small);
    require(short_length == needed && normalized[0] == 0xFF);
    require(bc_file_system_path_lexically_normalize(text, length, normalized, needed, normalized_length, &error) == 0);
    require(*normalized_length == needed);
    /* Idempotent. */
    uint8_t again[answer_capacity];
    size_t again_length = 0;
    require(bc_file_system_path_lexically_normalize(normalized, needed, again, sizeof again, &again_length, &error) ==
            0);
    require(same_bytes(again, again_length, normalized, needed));
    /* A valid path of the text's kind, or the two relative spellings validation refuses on purpose. */
    if (text[0] == '/') {
        require(bc_file_system_absolute_path_validate(normalized, needed, &error) == 0);
        check_valid_path(normalized, needed, true);
    } else if (needed == 1 && normalized[0] == '.') {
        /* The directory itself. */
    } else if (starts_with_dotdot(normalized, needed)) {
        /* A climb no lexical rule can resolve. */
    } else {
        require(bc_file_system_relative_path_validate(normalized, needed, &error) == 0);
        check_valid_path(normalized, needed, false);
    }
    return true;
}

static int32_t relation_between(const uint8_t *path, size_t length, const uint8_t *other, size_t other_length) {
    bc_file_system_error error;
    int32_t relation = 0;
    require(bc_file_system_path_relate(path, length, other, other_length, &relation, &error) == 0);
    require(relation >= bc_file_system_path_relation_inside && relation <= bc_file_system_path_relation_undecidable);
    return relation;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2 || size > longest_input) {
        return 0;
    }
    /* The first byte chooses where the rest is cut into the two texts. */
    const size_t rest = size - 1;
    const size_t cut = data[0] % rest;
    const uint8_t *first = data + 1;
    const size_t first_length = cut;
    const uint8_t *second = data + 1 + cut;
    const size_t second_length = rest - cut;

    bc_file_system_error error;
    if (bc_file_system_relative_path_validate(first, first_length, &error) == 0) {
        check_valid_path(first, first_length, false);
    }
    if (bc_file_system_absolute_path_validate(first, first_length, &error) == 0) {
        check_valid_path(first, first_length, true);
        /* resolve_against: a valid absolute base and loose relative text give a valid absolute path that is
         * its own normalisation. */
        if (second_length >= 1 && second[0] != '/') {
            uint8_t resolved[answer_capacity];
            size_t resolved_length = 0;
            const int32_t kind = bc_file_system_path_resolve_against(
                first, first_length, second, second_length, resolved, sizeof resolved, &resolved_length, &error);
            if (kind == 0) {
                require(bc_file_system_absolute_path_validate(resolved, resolved_length, &error) == 0);
                uint8_t again[answer_capacity];
                size_t again_length = 0;
                require(bc_file_system_path_lexically_normalize(resolved, resolved_length, again, sizeof again,
                                                                &again_length, &error) == 0);
                require(same_bytes(again, again_length, resolved, resolved_length));
            } else {
                require(kind == bc_file_system_error_kind_invalid_argument);
            }
        }
    }

    uint8_t first_normalized[answer_capacity];
    size_t first_normalized_length = 0;
    uint8_t second_normalized[answer_capacity];
    size_t second_normalized_length = 0;
    const bool first_loose = check_normalize(first, first_length, first_normalized, &first_normalized_length);
    const bool second_loose = check_normalize(second, second_length, second_normalized, &second_normalized_length);
    if (first_loose && second_loose) {
        const int32_t forward = relation_between(first, first_length, second, second_length);
        const int32_t backward = relation_between(second, second_length, first, first_length);
        if (forward == bc_file_system_path_relation_inside) {
            require(backward == bc_file_system_path_relation_outside);
        } else if (forward == bc_file_system_path_relation_outside) {
            require(backward == bc_file_system_path_relation_inside ||
                    backward == bc_file_system_path_relation_outside);
        } else {
            require(backward == forward);
        }
        /* Relating the normalised texts says the same. */
        require(relation_between(first_normalized, first_normalized_length, second_normalized,
                                 second_normalized_length) == forward);
        /* A text against itself. */
        const int32_t self = relation_between(first, first_length, first, first_length);
        if (first[0] != '/' && starts_with_dotdot(first_normalized, first_normalized_length)) {
            require(self == bc_file_system_path_relation_undecidable);
        } else {
            require(self == bc_file_system_path_relation_same);
        }
    }
    return 0;
}
