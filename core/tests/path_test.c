/* path_test.c — the C-only test of the path module, through the public header and nothing else.
 *
 * One `main` over tables of cases, registered with add_test: every public path function, every refusal, and the
 * edges (empty text, the root, a trailing slash, `a//b`, `.`, `..`, a NUL inside, a 255-byte component, one of
 * 256, the size query with a capacity of 0, buffer_too_small, idempotent normalisation, a join of validated parts
 * that validates). A failure prints the case and the test exits non-zero; nothing is reached for: the cases are
 * bytes in this file and the module consults nothing.
 */
#include <bc_file_system/bc_file_system.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* How many checks ran and how many failed; main reports both. Handed to every check rather than kept in a
 * global, so the file reads like the library it tests. */
typedef struct report {
    int checked;
    int failed;
} report;

/* `held` is an int because a C comparison is one, and so is a chain of them under &&. */
static void expect(report *tally, int held, const char *what, const char *detail) {
    tally->checked += 1;
    if (!held) {
        tally->failed += 1;
        printf("FAILED: %s: %s\n", what, detail);
    }
}

static const uint8_t *bytes_of(const char *text) {
    return (const uint8_t *)text;
}

static bool same_bytes(const uint8_t *bytes, size_t length, const char *expected) {
    if (length != strlen(expected)) {
        return false;
    }
    return memcmp(bytes, expected, length) == 0;
}

/* A buffer every answer test writes into, large enough for any case here and pre-filled so an untouched
 * buffer is visible. */
enum : uint32_t { answer_capacity = 1024, long_component_capacity = 300 };

static void fill(uint8_t *buffer, size_t capacity, uint8_t value) {
    for (size_t index = 0; index < capacity; ++index) {
        buffer[index] = value;
    }
}

static bool all_bytes_are(const uint8_t *buffer, size_t capacity, uint8_t value) {
    for (size_t index = 0; index < capacity; ++index) {
        if (buffer[index] != value) {
            return false;
        }
    }
    return true;
}

/* A component of `length` bytes of 'x', optionally behind a prefix such as "a/" or "/", for the length edges. */
static size_t build_long_path(uint8_t *buffer, const char *prefix, size_t component_length) {
    const size_t prefix_length = strlen(prefix);
    for (size_t index = 0; index < prefix_length; ++index) {
        buffer[index] = (uint8_t)prefix[index];
    }
    fill(buffer + prefix_length, component_length, 'x');
    return prefix_length + component_length;
}

/* ---- Validation ---------------------------------------------------------------------------------------------- */

typedef struct validation_case {
    const char *text;
    bool accepted;
} validation_case;

static void test_relative_path_validate(report *tally) {
    static const validation_case cases[] = {
        {"a", true},       {"a/b", true},     {"notes.txt", true}, {".hidden", true}, {"a.", true},   {"..a", true},
        {"a..b", true},    {"a/b/c.d", true}, {"", false},         {"/", false},      {"/a", false},  {"a/", false},
        {"a//b", false},   {".", false},      {"a/./b", false},    {"./a", false},    {"a/.", false}, {"..", false},
        {"a/../b", false}, {"a/..", false},   {"../a", false},     {"//", false},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const validation_case *item = &cases[index];
        bc_file_system_error error;
        const int32_t kind = bc_file_system_relative_path_validate(bytes_of(item->text), strlen(item->text), &error);
        if (item->accepted) {
            expect(tally, kind == 0, "relative_path_validate accepts", item->text);
            expect(tally, error.kind == 0 && error.operation == 0, "a success zeroes the error", item->text);
        } else {
            expect(tally, kind == bc_file_system_error_kind_invalid_argument, "relative_path_validate refuses",
                   item->text);
            expect(tally, error.kind == kind && error.operation == bc_file_system_operation_relative_path_from,
                   "a refusal names relative_path_from", item->text);
            expect(tally, error.step == 0 && error.system_code == 0 && error.name_length == 0,
                   "a refusal has no step, no system code and no name", item->text);
        }
    }
}

static void test_absolute_path_validate(report *tally) {
    static const validation_case cases[] = {
        {"/", true},      {"/a", true},       {"/a/b", true},    {"/a/../b", true}, {"/..", true},   {"/../a", true},
        {"/a/..", true},  {"/.hidden", true}, {"", false},       {"a", false},      {"//", false},   {"/a/", false},
        {"/a//b", false}, {"/.", false},      {"/a/./b", false}, {"./a", false},    {"../a", false}, {"/a/.", false},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const validation_case *item = &cases[index];
        bc_file_system_error error;
        const int32_t kind = bc_file_system_absolute_path_validate(bytes_of(item->text), strlen(item->text), &error);
        if (item->accepted) {
            expect(tally, kind == 0, "absolute_path_validate accepts", item->text);
        } else {
            expect(tally, kind == bc_file_system_error_kind_invalid_argument, "absolute_path_validate refuses",
                   item->text);
            expect(tally, error.operation == bc_file_system_operation_absolute_path_from,
                   "a refusal names absolute_path_from", item->text);
        }
    }
}

static void test_nul_and_length_edges(report *tally) {
    bc_file_system_error error;
    /* A NUL inside: three bytes, which a NUL-terminated reading would cut to one. */
    expect(tally,
           bc_file_system_relative_path_validate(bytes_of("a\0b"), 3, &error) ==
               bc_file_system_error_kind_invalid_argument,
           "a NUL inside a relative path is refused", "a\\0b");
    expect(tally,
           bc_file_system_absolute_path_validate(bytes_of("/a\0b"), 4, &error) ==
               bc_file_system_error_kind_invalid_argument,
           "a NUL inside an absolute path is refused", "/a\\0b");
    size_t stem_length = 0;
    expect(tally,
           bc_file_system_path_stem(bytes_of("a\0b"), 3, &stem_length, &error) ==
               bc_file_system_error_kind_invalid_argument,
           "a NUL inside a component is refused", "a\\0b");
    uint8_t answer[answer_capacity];
    size_t answer_length = 0;
    expect(tally,
           bc_file_system_path_lexically_normalize(bytes_of("a\0b"), 3, answer, sizeof answer, &answer_length,
                                                   &error) == bc_file_system_error_kind_invalid_argument,
           "a NUL inside loose text is refused", "a\\0b");

    /* 255 bytes is the longest component; 256 is refused everywhere a component is read. */
    uint8_t path[long_component_capacity];
    size_t length = build_long_path(path, "", 255);
    expect(tally, bc_file_system_relative_path_validate(path, length, &error) == 0, "a 255-byte component", "alone");
    length = build_long_path(path, "a/", 255);
    expect(tally, bc_file_system_relative_path_validate(path, length, &error) == 0, "a 255-byte component", "a/x…");
    length = build_long_path(path, "/", 255);
    expect(tally, bc_file_system_absolute_path_validate(path, length, &error) == 0, "a 255-byte component", "/x…");
    length = build_long_path(path, "", 255);
    expect(tally, bc_file_system_path_stem(path, length, &stem_length, &error) == 0 && stem_length == 255,
           "a 255-byte component has a stem", "x…");
    length = build_long_path(path, "", 256);
    expect(tally,
           bc_file_system_relative_path_validate(path, length, &error) == bc_file_system_error_kind_invalid_argument,
           "a 256-byte component is refused", "alone");
    length = build_long_path(path, "a/", 256);
    expect(tally,
           bc_file_system_relative_path_validate(path, length, &error) == bc_file_system_error_kind_invalid_argument,
           "a 256-byte component is refused", "a/x…");
    length = build_long_path(path, "/", 256);
    expect(tally,
           bc_file_system_absolute_path_validate(path, length, &error) == bc_file_system_error_kind_invalid_argument,
           "a 256-byte component is refused", "/x…");
    length = build_long_path(path, "", 256);
    expect(tally,
           bc_file_system_path_stem(path, length, &stem_length, &error) == bc_file_system_error_kind_invalid_argument,
           "a 256-byte component is refused", "stem");
    length = build_long_path(path, "./", 256);
    expect(tally,
           bc_file_system_path_lexically_normalize(path, length, answer, sizeof answer, &answer_length, &error) ==
               bc_file_system_error_kind_invalid_argument,
           "a 256-byte component is refused", "loose text");
}

/* ---- Join ---------------------------------------------------------------------------------------------------- */

typedef struct join_case {
    const char *base;
    const char *relative;
    const char *expected; /* NULL when the join is refused */
} join_case;

static void test_join(report *tally) {
    static const join_case cases[] = {
        {"/", "a", "/a"},  {"/", "a/b", "/a/b"},  {"/a", "b", "/a/b"},       {"/a", "b/c", "/a/b/c"},
        {"a", "b", "a/b"}, {"a/b", "c", "a/b/c"}, {"/a/..", "b", "/a/../b"}, {"/", "/a", NULL},
        {"a/", "b", NULL}, {"a", "b/", NULL},     {"", "a", NULL},           {"a", "", NULL},
        {"a", "..", NULL}, {"a", ".", NULL},      {"//", "a", NULL},         {"a", "b//c", NULL},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const join_case *item = &cases[index];
        uint8_t answer[answer_capacity];
        fill(answer, sizeof answer, 'x');
        size_t length = 0;
        bc_file_system_error error;
        const int32_t kind =
            bc_file_system_path_join(bytes_of(item->base), strlen(item->base), bytes_of(item->relative),
                                     strlen(item->relative), answer, sizeof answer, &length, &error);
        if (item->expected != NULL) {
            expect(tally, kind == 0 && same_bytes(answer, length, item->expected), "join", item->expected);
            /* A join of validated parts validates: the answer is a path of the base's kind. */
            const int32_t revalidated = item->base[0] == '/'
                                            ? bc_file_system_absolute_path_validate(answer, length, &error)
                                            : bc_file_system_relative_path_validate(answer, length, &error);
            expect(tally, revalidated == 0, "a join of validated parts validates", item->expected);
        } else {
            expect(tally, kind == bc_file_system_error_kind_invalid_argument, "join refuses", item->base);
            expect(tally, error.operation == bc_file_system_operation_path_join, "a refusal names path_join",
                   item->base);
            expect(tally, kind != 0 && all_bytes_are(answer, sizeof answer, 'x'), "a refusal writes nothing",
                   item->base);
        }
    }

    /* The buffer protocol: the size first, then exactly that much; one byte short is still too small, and a
     * failed call leaves the buffer as it was. */
    uint8_t answer[answer_capacity];
    fill(answer, sizeof answer, 'x');
    size_t length = 0;
    bc_file_system_error error;
    int32_t kind = bc_file_system_path_join(bytes_of("a"), 1, bytes_of("bc"), 2, NULL, 0, &length, &error);
    expect(tally, kind == bc_file_system_error_kind_buffer_too_small && length == 4, "a capacity of 0 asks the size",
           "a + bc");
    expect(tally, error.kind == kind && error.operation == bc_file_system_operation_path_join,
           "buffer_too_small names the operation", "a + bc");
    kind = bc_file_system_path_join(bytes_of("a"), 1, bytes_of("bc"), 2, answer, 3, &length, &error);
    expect(tally, kind == bc_file_system_error_kind_buffer_too_small && length == 4, "one byte short is too small",
           "a + bc");
    expect(tally, kind != 0 && all_bytes_are(answer, sizeof answer, 'x'), "too small writes nothing", "a + bc");
    kind = bc_file_system_path_join(bytes_of("a"), 1, bytes_of("bc"), 2, answer, 4, &length, &error);
    expect(tally, kind == 0 && same_bytes(answer, length, "a/bc"), "exactly the size fits", "a + bc");
    expect(tally, answer[4] == 'x', "the answer is never longer than its length", "a + bc");
}

/* ---- Parent, last component, the iterator -------------------------------------------------------------------- */

typedef struct parent_case {
    const char *path;
    bool present;
    const char *parent;
} parent_case;

static void test_parent(report *tally) {
    static const parent_case cases[] = {
        {"/", false, ""}, {"/a", true, "/"},  {"/a/b", true, "/a"},   {"/a/b/c", true, "/a/b"}, {"/a/..", true, "/a"},
        {"a", false, ""}, {"a/b", true, "a"}, {"a/b/c", true, "a/b"}, {"notes.txt", false, ""},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const parent_case *item = &cases[index];
        int32_t present = 7;
        size_t parent_length = 99;
        bc_file_system_error error;
        const int32_t kind =
            bc_file_system_path_parent(bytes_of(item->path), strlen(item->path), &present, &parent_length, &error);
        expect(tally, kind == 0, "parent succeeds", item->path);
        if (item->present) {
            expect(tally, present == 1 && same_bytes(bytes_of(item->path), parent_length, item->parent), "parent",
                   item->path);
        } else {
            expect(tally, present == 0 && parent_length == 99, "no parent leaves parent_length untouched", item->path);
        }
    }
    int32_t present = 7;
    size_t parent_length = 99;
    bc_file_system_error error;
    expect(tally,
           bc_file_system_path_parent(bytes_of("a/"), 2, &present, &parent_length, &error) ==
                   bc_file_system_error_kind_invalid_argument &&
               present == 7 && error.operation == bc_file_system_operation_path_parent,
           "parent refuses an invalid path and touches nothing", "a/");
}

typedef struct last_component_case {
    const char *path;
    const char *expected; /* NULL when refused */
} last_component_case;

static void test_last_component(report *tally) {
    static const last_component_case cases[] = {
        {"/a/b", "b"},   {"/a", "a"}, {"a", "a"},   {"a/b.txt", "b.txt"},
        {"/a/..", ".."}, {"/", NULL}, {"a/", NULL}, {"", NULL},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const last_component_case *item = &cases[index];
        size_t offset = 99;
        size_t component_length = 99;
        bc_file_system_error error;
        const int32_t kind = bc_file_system_path_last_component(bytes_of(item->path), strlen(item->path), &offset,
                                                                &component_length, &error);
        if (item->expected != NULL) {
            expect(tally, kind == 0 && same_bytes(bytes_of(item->path) + offset, component_length, item->expected),
                   "last_component", item->path);
        } else {
            expect(tally,
                   kind == bc_file_system_error_kind_invalid_argument && offset == 99 &&
                       error.operation == bc_file_system_operation_path_last_component,
                   "last_component refuses and touches nothing", item->path);
        }
    }
}

typedef struct iteration_case {
    const char *path;
    const char *components[4]; /* NULL-terminated; the root has none */
} iteration_case;

static void test_next_component(report *tally) {
    static const iteration_case cases[] = {
        {"/a/b/c", {"a", "b", "c", NULL}},
        {"/a", {"a", NULL}},
        {"/", {NULL}},
        {"a", {"a", NULL}},
        {"a/bb/ccc", {"a", "bb", "ccc", NULL}},
        {"/a/../b", {"a", "..", "b", NULL}},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const iteration_case *item = &cases[index];
        size_t cursor = 0;
        size_t seen = 0;
        for (;;) {
            size_t offset = 99;
            size_t component_length = 99;
            int32_t present = 7;
            bc_file_system_error error;
            const int32_t kind = bc_file_system_path_next_component(bytes_of(item->path), strlen(item->path), &cursor,
                                                                    &offset, &component_length, &present, &error);
            expect(tally, kind == 0, "next_component succeeds", item->path);
            if (present == 0) {
                expect(tally, offset == 99 && component_length == 99, "the end touches nothing but present",
                       item->path);
                break;
            }
            const char *expected = item->components[seen];
            expect(tally, expected != NULL && same_bytes(bytes_of(item->path) + offset, component_length, expected),
                   "next_component yields the next component", item->path);
            seen += 1;
        }
        expect(tally, item->components[seen] == NULL, "next_component yields every component", item->path);
        /* Asked again at the end, it stays at the end. */
        size_t offset = 0;
        size_t component_length = 0;
        int32_t present = 7;
        bc_file_system_error error;
        bc_file_system_path_next_component(bytes_of(item->path), strlen(item->path), &cursor, &offset,
                                           &component_length, &present, &error);
        expect(tally, present == 0, "the end is stable", item->path);
    }
    /* A cursor the iterator never produced is a contract violation. */
    size_t cursor = 1;
    size_t offset = 0;
    size_t component_length = 0;
    int32_t present = 7;
    bc_file_system_error error;
    expect(tally,
           bc_file_system_path_next_component(bytes_of("ab"), 2, &cursor, &offset, &component_length, &present,
                                              &error) == bc_file_system_error_kind_invalid_argument &&
               present == 7 && error.operation == bc_file_system_operation_path_next_component,
           "a cursor inside a component is refused", "ab at 1");
}

/* ---- Stem and extension ----------------------------------------------------------------------------------------- */

typedef struct component_parts_case {
    const char *component;
    const char *stem;
    bool extension_present;
    const char *extension;
} component_parts_case;

static void test_stem_and_extension(report *tally) {
    static const component_parts_case cases[] = {
        {"a.txt", "a", true, "txt"},
        {"archive.tar.gz", "archive.tar", true, "gz"},
        {".bashrc", ".bashrc", false, ""},
        {"archive.", "archive.", false, ""},
        {"a", "a", false, ""},
        {".a.b", ".a", true, "b"},
        {".", ".", false, ""},
        {"..", "..", false, ""},
        {"...", "...", false, ""},
        {"a..b", "a.", true, "b"},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const component_parts_case *item = &cases[index];
        const uint8_t *component = bytes_of(item->component);
        const size_t length = strlen(item->component);
        size_t stem_length = 99;
        bc_file_system_error error;
        int32_t kind = bc_file_system_path_stem(component, length, &stem_length, &error);
        expect(tally, kind == 0 && same_bytes(component, stem_length, item->stem), "stem", item->component);
        size_t offset = 99;
        size_t extension_length = 99;
        int32_t present = 7;
        kind = bc_file_system_path_extension(component, length, &offset, &extension_length, &present, &error);
        expect(tally, kind == 0, "extension succeeds", item->component);
        if (item->extension_present) {
            expect(tally, present == 1 && same_bytes(component + offset, extension_length, item->extension),
                   "extension", item->component);
            expect(tally, stem_length + 1 + extension_length == length,
                   "stem, a dot and the extension are the component", item->component);
        } else {
            expect(tally, present == 0 && offset == 99 && extension_length == 99, "no extension touches only present",
                   item->component);
        }
    }
    static const char *const refused[] = {"", "a/b", "/"};
    for (size_t index = 0; index < sizeof refused / sizeof refused[0]; ++index) {
        size_t stem_length = 99;
        bc_file_system_error error;
        expect(tally,
               bc_file_system_path_stem(bytes_of(refused[index]), strlen(refused[index]), &stem_length, &error) ==
                       bc_file_system_error_kind_invalid_argument &&
                   stem_length == 99 && error.operation == bc_file_system_operation_path_stem,
               "stem refuses what is not one component", refused[index]);
        size_t offset = 0;
        size_t extension_length = 0;
        int32_t present = 7;
        expect(tally,
               bc_file_system_path_extension(bytes_of(refused[index]), strlen(refused[index]), &offset,
                                             &extension_length, &present,
                                             &error) == bc_file_system_error_kind_invalid_argument &&
                   present == 7 && error.operation == bc_file_system_operation_path_extension,
               "extension refuses what is not one component", refused[index]);
    }
}

/* ---- Relate ------------------------------------------------------------------------------------------------- */

typedef struct relate_case {
    const char *path;
    const char *other;
    int32_t relation;
} relate_case;

static void test_relate(report *tally) {
    static const relate_case cases[] = {
        {"/a/b", "/a", bc_file_system_path_relation_inside},
        {"/a", "/a/b", bc_file_system_path_relation_outside},
        {"/a", "/a", bc_file_system_path_relation_same},
        {"/a/./b", "/a/b", bc_file_system_path_relation_same},
        {"/a/b/..", "/a", bc_file_system_path_relation_same},
        {"/a//b/", "/a/b", bc_file_system_path_relation_same},
        {"/a/b/c", "/a", bc_file_system_path_relation_inside},
        {"/a", "/b", bc_file_system_path_relation_outside},
        {"/ab", "/a", bc_file_system_path_relation_outside},
        {"/", "/", bc_file_system_path_relation_same},
        {"/a", "/", bc_file_system_path_relation_inside},
        {"/..", "/", bc_file_system_path_relation_same},
        {"/a/../..", "/", bc_file_system_path_relation_same},
        {"a/b", "a", bc_file_system_path_relation_inside},
        {"a", "a/b", bc_file_system_path_relation_outside},
        {"a/..", ".", bc_file_system_path_relation_same},
        {"a", ".", bc_file_system_path_relation_inside},
        {".", "a", bc_file_system_path_relation_outside},
        {"a", "/a", bc_file_system_path_relation_undecidable},
        {"/a", "a", bc_file_system_path_relation_undecidable},
        {"../a", "a", bc_file_system_path_relation_undecidable},
        {"a", "../a", bc_file_system_path_relation_undecidable},
        {"a/../../b", "b", bc_file_system_path_relation_undecidable},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const relate_case *item = &cases[index];
        int32_t relation = 0;
        bc_file_system_error error;
        const int32_t kind = bc_file_system_path_relate(bytes_of(item->path), strlen(item->path), bytes_of(item->other),
                                                        strlen(item->other), &relation, &error);
        expect(tally, kind == 0 && relation == item->relation, "relate", item->path);
        /* `same` and `undecidable` read the same from either side; `inside` from one side is `outside` from the
         * other. */
        int32_t reverse = 0;
        bc_file_system_path_relate(bytes_of(item->other), strlen(item->other), bytes_of(item->path), strlen(item->path),
                                   &reverse, &error);
        if (item->relation == bc_file_system_path_relation_inside) {
            expect(tally, reverse == bc_file_system_path_relation_outside, "inside reversed is outside", item->path);
        } else if (item->relation == bc_file_system_path_relation_same ||
                   item->relation == bc_file_system_path_relation_undecidable) {
            expect(tally, reverse == item->relation, "same and undecidable are symmetric", item->path);
        }
    }
    int32_t relation = 7;
    bc_file_system_error error;
    expect(tally,
           bc_file_system_path_relate(bytes_of(""), 0, bytes_of("a"), 1, &relation, &error) ==
                   bc_file_system_error_kind_invalid_argument &&
               relation == 7 && error.operation == bc_file_system_operation_path_relate,
           "relate refuses empty text and touches nothing", "");
}

/* ---- Normalisation and resolution ---------------------------------------------------------------------------- */

typedef struct normalize_case {
    const char *text;
    const char *expected;
} normalize_case;

static void test_lexically_normalize(report *tally) {
    static const normalize_case cases[] = {
        {"a//b", "a/b"},
        {"a/./b", "a/b"},
        {"a/../b", "b"},
        {"../a", "../a"},
        {"a/../../b", "../b"},
        {"/..", "/"},
        {"//", "/"},
        {"/a/", "/a"},
        {".", "."},
        {"a/..", "."},
        {"./", "."},
        {"/a/b/../c", "/a/c"},
        {"a/b/../../..", ".."},
        {"./a/./", "a"},
        {"/./a", "/a"},
        {"/", "/"},
        {"a", "a"},
        {"/a/../../b", "/b"},
        {"..", ".."},
        {"a/b/c/../..", "a"},
        {"/a/b/c/../../d", "/a/d"},
        {"...", "..."},
        {"a/.../b", "a/.../b"},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const normalize_case *item = &cases[index];
        uint8_t answer[answer_capacity];
        size_t length = 0;
        bc_file_system_error error;
        const int32_t kind = bc_file_system_path_lexically_normalize(bytes_of(item->text), strlen(item->text), answer,
                                                                     sizeof answer, &length, &error);
        expect(tally, kind == 0 && same_bytes(answer, length, item->expected), "lexically_normalize", item->text);
        /* Idempotent: the answer normalises to itself. */
        uint8_t again[answer_capacity];
        size_t again_length = 0;
        const int32_t again_kind =
            bc_file_system_path_lexically_normalize(answer, length, again, sizeof again, &again_length, &error);
        expect(tally, again_kind == 0 && again_length == length && memcmp(answer, again, length) == 0,
               "lexically_normalize is idempotent", item->text);
    }

    uint8_t answer[answer_capacity];
    fill(answer, sizeof answer, 'x');
    size_t length = 0;
    bc_file_system_error error;
    int32_t kind = bc_file_system_path_lexically_normalize(bytes_of("a/./b"), 5, NULL, 0, &length, &error);
    expect(tally, kind == bc_file_system_error_kind_buffer_too_small && length == 3, "a capacity of 0 asks the size",
           "a/./b");
    kind = bc_file_system_path_lexically_normalize(bytes_of("a/./b"), 5, answer, 2, &length, &error);
    expect(tally, kind == bc_file_system_error_kind_buffer_too_small && length == 3 && all_bytes_are(answer, 3, 'x'),
           "too small writes nothing", "a/./b");
    kind = bc_file_system_path_lexically_normalize(bytes_of(""), 0, answer, sizeof answer, &length, &error);
    expect(tally,
           kind == bc_file_system_error_kind_invalid_argument &&
               error.operation == bc_file_system_operation_path_lexically_normalize,
           "empty text is refused", "");
}

typedef struct resolve_case {
    const char *base;
    const char *relative;
    const char *expected; /* NULL when refused */
} resolve_case;

static void test_resolve_against(report *tally) {
    static const resolve_case cases[] = {
        {"/home/me", "docs/../notes.txt", "/home/me/notes.txt"},
        {"/", "a", "/a"},
        {"/a/b", "../../../c", "/c"},
        {"/a", ".", "/a"},
        {"/a/..", "b", "/b"},
        {"/a", "b/", "/a/b"},
        {"/a", "./b//c/.", "/a/b/c"},
        {"/a", "..", "/"},
        {"a", "b", NULL},
        {"/a", "/b", NULL},
        {"/a/", "b", NULL},
        {"/a", "", NULL},
        {"", "b", NULL},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const resolve_case *item = &cases[index];
        uint8_t answer[answer_capacity];
        size_t length = 0;
        bc_file_system_error error;
        const int32_t kind =
            bc_file_system_path_resolve_against(bytes_of(item->base), strlen(item->base), bytes_of(item->relative),
                                                strlen(item->relative), answer, sizeof answer, &length, &error);
        if (item->expected != NULL) {
            expect(tally, kind == 0 && same_bytes(answer, length, item->expected), "resolve_against", item->relative);
            expect(tally, bc_file_system_absolute_path_validate(answer, length, &error) == 0,
                   "a resolved path is a valid absolute path", item->relative);
        } else {
            expect(tally,
                   kind == bc_file_system_error_kind_invalid_argument &&
                       error.operation == bc_file_system_operation_path_resolve_against,
                   "resolve_against refuses", item->relative);
        }
    }
    size_t length = 0;
    bc_file_system_error error;
    const int32_t kind =
        bc_file_system_path_resolve_against(bytes_of("/a"), 2, bytes_of("b"), 1, NULL, 0, &length, &error);
    expect(tally, kind == bc_file_system_error_kind_buffer_too_small && length == 4, "a capacity of 0 asks the size",
           "/a + b");
}

/* ---- Library names ------------------------------------------------------------------------------------------- */

typedef struct library_case {
    const char *component;
    int32_t kind;
    const char *stem;
} library_case;

static void test_library_name(report *tally) {
    static const library_case cases[] = {
        {"libz.so", bc_file_system_library_kind_shared, "z"},
        {"libz.so.1", bc_file_system_library_kind_shared, "z"},
        {"libfoo.so.1.2.3", bc_file_system_library_kind_shared, "foo"},
        {"libfoo.so.bar.so", bc_file_system_library_kind_shared, "foo.so.bar"},
        {"libc++.dylib", bc_file_system_library_kind_shared, "c++"},
        {"libz.1.dylib", bc_file_system_library_kind_shared, "z.1"},
        {"libm.a", bc_file_system_library_kind_static, "m"},
        {"Foundation.framework", bc_file_system_library_kind_framework, "Foundation"},
        {"kernel32.dll", bc_file_system_library_kind_shared, "kernel32"},
        {"libfoo.dll", bc_file_system_library_kind_shared, "libfoo"},
        {"libfoo.so.1.x", bc_file_system_library_kind_none, ""},
        {"libfoo.so.", bc_file_system_library_kind_none, ""},
        {"libfoo.so..1", bc_file_system_library_kind_none, ""},
        {"lib.so", bc_file_system_library_kind_none, ""},
        {"lib.a", bc_file_system_library_kind_none, ""},
        {"libz.SO", bc_file_system_library_kind_none, ""},
        {"foo.so", bc_file_system_library_kind_none, ""},
        {".dll", bc_file_system_library_kind_none, ""},
        {".framework", bc_file_system_library_kind_none, ""},
        {"libfoo", bc_file_system_library_kind_none, ""},
        {"a", bc_file_system_library_kind_none, ""},
        {"notes.txt", bc_file_system_library_kind_none, ""},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        const library_case *item = &cases[index];
        int32_t kind = 0;
        size_t stem_offset = 99;
        size_t stem_length = 99;
        bc_file_system_error error;
        const int32_t status = bc_file_system_path_library_name(bytes_of(item->component), strlen(item->component),
                                                                &kind, &stem_offset, &stem_length, &error);
        expect(tally, status == 0 && kind == item->kind, "library_name kind", item->component);
        if (item->kind != bc_file_system_library_kind_none) {
            expect(tally, status == 0 && same_bytes(bytes_of(item->component) + stem_offset, stem_length, item->stem),
                   "library_name stem", item->component);
        } else {
            expect(tally, stem_offset == 99 && stem_length == 99, "none leaves the stem untouched", item->component);
        }
    }
    int32_t kind = 0;
    size_t stem_offset = 0;
    size_t stem_length = 0;
    bc_file_system_error error;
    expect(tally,
           bc_file_system_path_library_name(bytes_of("a/b"), 3, &kind, &stem_offset, &stem_length, &error) ==
                   bc_file_system_error_kind_invalid_argument &&
               error.operation == bc_file_system_operation_path_library_name,
           "library_name refuses what is not one component", "a/b");
}

/* ---- The contract every function keeps ----------------------------------------------------------------------- */

static void test_null_contracts(report *tally) {
    uint8_t answer[answer_capacity];
    size_t length = 0;
    size_t offset = 0;
    int32_t present = 0;
    int32_t value = 0;
    bc_file_system_error error;
    const int32_t invalid = bc_file_system_error_kind_invalid_argument;

    /* A NULL error pointer: the one case that cannot be reported anywhere but the return value. */
    expect(tally, bc_file_system_relative_path_validate(bytes_of("a"), 1, NULL) == invalid, "NULL error", "validate");
    expect(tally,
           bc_file_system_path_join(bytes_of("a"), 1, bytes_of("b"), 1, answer, sizeof answer, &length, NULL) ==
               invalid,
           "NULL error", "join");
    expect(tally,
           bc_file_system_path_lexically_normalize(bytes_of("a"), 1, answer, sizeof answer, &length, NULL) == invalid,
           "NULL error", "normalize");

    /* NULL bytes with a length of 0 are an empty span, refused as empty text; with a length they are a violation. */
    expect(tally, bc_file_system_relative_path_validate(NULL, 0, &error) == invalid, "NULL with length 0", "empty");
    expect(tally, bc_file_system_relative_path_validate(NULL, 1, &error) == invalid, "NULL with a length", "violation");
    expect(tally, bc_file_system_path_relate(bytes_of("a"), 1, NULL, 1, &value, &error) == invalid, "NULL other",
           "relate");

    /* A NULL out-parameter. */
    expect(tally, bc_file_system_path_parent(bytes_of("a/b"), 3, NULL, &length, &error) == invalid, "NULL present",
           "parent");
    expect(tally, bc_file_system_path_parent(bytes_of("a/b"), 3, &present, NULL, &error) == invalid,
           "NULL parent_length", "parent");
    expect(tally, bc_file_system_path_last_component(bytes_of("a/b"), 3, NULL, &length, &error) == invalid,
           "NULL offset", "last_component");
    expect(tally,
           bc_file_system_path_next_component(bytes_of("a"), 1, NULL, &offset, &length, &present, &error) == invalid,
           "NULL cursor", "next_component");
    expect(tally, bc_file_system_path_stem(bytes_of("a"), 1, NULL, &error) == invalid, "NULL stem_length", "stem");
    expect(tally, bc_file_system_path_extension(bytes_of("a"), 1, &offset, &length, NULL, &error) == invalid,
           "NULL present", "extension");
    expect(tally, bc_file_system_path_relate(bytes_of("a"), 1, bytes_of("b"), 1, NULL, &error) == invalid,
           "NULL relation", "relate");
    expect(tally, bc_file_system_path_library_name(bytes_of("a"), 1, NULL, &offset, &length, &error) == invalid,
           "NULL kind", "library_name");

    /* The buffer protocol's NULL: legal only with a capacity of 0. */
    expect(tally, bc_file_system_path_join(bytes_of("a"), 1, bytes_of("b"), 1, NULL, 1, &length, &error) == invalid,
           "NULL buffer with a capacity", "join");
    expect(tally,
           bc_file_system_path_join(bytes_of("a"), 1, bytes_of("b"), 1, answer, sizeof answer, NULL, &error) == invalid,
           "NULL length", "join");
    expect(tally,
           bc_file_system_path_resolve_against(bytes_of("/a"), 2, bytes_of("b"), 1, NULL, 1, &length, &error) ==
               invalid,
           "NULL buffer with a capacity", "resolve_against");

    /* Every refusal fills the struct so the return value equals its kind. */
    expect(tally, error.kind == invalid && error.operation == bc_file_system_operation_path_resolve_against,
           "the last refusal filled the error", "resolve_against");
}

int main(void) {
    report tally = {.checked = 0, .failed = 0};
    test_relative_path_validate(&tally);
    test_absolute_path_validate(&tally);
    test_nul_and_length_edges(&tally);
    test_join(&tally);
    test_parent(&tally);
    test_last_component(&tally);
    test_next_component(&tally);
    test_stem_and_extension(&tally);
    test_relate(&tally);
    test_lexically_normalize(&tally);
    test_resolve_against(&tally);
    test_library_name(&tally);
    test_null_contracts(&tally);
    printf("bc_file_system_path_test: %d checks, %d failed\n", tally.checked, tally.failed);
    return tally.failed == 0 ? 0 : 1;
}
