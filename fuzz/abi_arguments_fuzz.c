/* abi_arguments_fuzz.c — libFuzzer over the whole public header with fuzzed arguments: every function is reached
 * with enums inside and outside their range, NULL where the contract forbids it and where it allows it (a NULL
 * buffer with a capacity of 0), buffers with and without room, offsets and bounds near INT64_MAX, NUL bytes and
 * slashes inside paths, and the three invariants the header promises are held after every call:
 *
 *   - the status returned is the error's kind, a success zeroes the whole error struct, and a failure fills it
 *     with values of the enums (a contract violation as `invalid_argument` with no step and no code);
 *   - a failure leaves every out-parameter untouched (`*length` under `buffer_too_small` excepted, which the
 *     protocol fills with the capacity needed), and a success writes a value of the answer's type;
 *   - no call leaves a descriptor behind: the descriptors open on paths under the root are the harness's one
 *     handle after every input, as before the first, with no count of the whole descriptor table.
 *
 * Nothing outside the root is ever touched: a location is `beneath` the root the harness holds (contained by
 * the kernel: no component may be a symlink or `..`), the root's own text, or one component under it with every
 * slash turned into another byte, so no spelling of an input can climb out. The root is emptied after every
 * input through the engine itself, and the input's own engine is the observed one, so a fuzzed fault scripts
 * real refusals into the calls that follow it in the same input.
 */
#include <bc_file_system/bc_file_system.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"

/* Inputs longer than this exercise nothing new: a call takes at most a few spans of up to 255 bytes. */
enum : uint32_t { longest_input = 4096 };

/* Every answer buffer offered to the core is at most this large; a capacity above it is offered only with a
 * NULL buffer, which the core refuses before writing anywhere. */
enum : size_t { answer_capacity = 512, entry_capacity = 8 };

/* The last enumerator of each closed enum, so `take_enum` reads `take_enum(input, operation_last)`. */
enum : int32_t {
    operation_last = bc_file_system_operation_path_library_name,
    step_last = bc_file_system_step_faccessat,
    final_symlink_last = bc_file_system_final_symlink_follow,
    existing_destination_last = bc_file_system_existing_destination_refuse,
    read_limit_kind_last = bc_file_system_read_limit_kind_at_most,
    durability_last = bc_file_system_durability_durable,
    permissions_policy_last = bc_file_system_permissions_policy_preserve_or,
    fault_action_last = bc_file_system_fault_action_stop,
    error_kind_last = bc_file_system_error_kind_buffer_too_small
};

/* A value an out-parameter holds before a call, which a failing call must leave in place. */
enum : int32_t { untouched_int32 = 77 };
enum : size_t { untouched_size = 77 };
enum : uint64_t { untouched_uint64 = 77 };

static bc_file_system_engine *engine = nullptr;
static bc_file_system_directory *root = nullptr;
static char root_text[PATH_MAX];
static size_t root_text_length = 0;
static int descriptors_at_start = -1;

/* ---- The answer checks ---------------------------------------------------------------------------------------- */

static bool in_enum(int32_t value, int32_t last) {
    if (value < 1) {
        return false;
    }
    return value <= last;
}

/* The first invariant: the status against the error struct, for a call that was handed `error` (NULL means the
 * call was handed no error, which the header answers `invalid_argument` without writing anywhere). */
static void check_answer(int32_t status, const bc_file_system_error *error, int32_t expected_operation) {
    if (error == nullptr) {
        require(status == bc_file_system_error_kind_invalid_argument);
        return;
    }
    require(status == error->kind);
    if (status == 0) {
        const bc_file_system_error zero = {};
        require(memcmp(error, &zero, sizeof zero) == 0);
        return;
    }
    require(in_enum(error->kind, error_kind_last));
    require(error->operation == expected_operation);
    require(error->step == 0 || in_enum(error->step, step_last));
    require(error->name_length <= sizeof error->name);
    require(error->name_truncated == 0 || error->name_truncated == 1);
    require(error->failed_location >= 0 && error->failed_location <= 2);
    if (error->kind == bc_file_system_error_kind_invalid_argument && error->system_code == 0) {
        require(error->step == 0);
    }
}

/* An answer of variable length: on success `*length` fits the capacity, under `buffer_too_small` it is the
 * capacity needed, and any other failure leaves it alone. */
static void check_length_answer(int32_t status, const size_t *length, size_t capacity) {
    if (length == nullptr) {
        return;
    }
    if (status == 0) {
        require(*length <= capacity);
    } else if (status == bc_file_system_error_kind_buffer_too_small) {
        require(*length > capacity);
    } else {
        require(*length == untouched_size);
    }
}

/* ---- The fuzzed arguments ------------------------------------------------------------------------------------ */

/* The error struct a call is handed, or NULL one time in sixteen. */
static bc_file_system_error *take_error(reader *input, bc_file_system_error *storage) {
    if ((take_byte(input) & 15) == 0) {
        return nullptr;
    }
    *storage = (bc_file_system_error){.kind = untouched_int32};
    return storage;
}

static bc_file_system_engine *take_engine(reader *input) {
    if ((take_byte(input) & 15) == 0) {
        return nullptr;
    }
    return engine;
}

/* One component made of `span`, into `component` (which holds at least 256 bytes): a slash would make it a
 * path that could climb, and `.` or `..` would name the directory itself or its parent, so each becomes another
 * byte; an empty span becomes `x`. A NUL stays, for the engine to refuse. This is what keeps every `at` location
 * and every symlink target the fuzzer spells inside the root. Answers the length. */
static size_t one_component(const uint8_t *span, size_t span_length, uint8_t *component) {
    const bool is_dot = span_length == 1 && span[0] == '.';
    const bool is_dot_dot = span_length == 2 && span[0] == '.' && span[1] == '.';
    if (span_length == 0 || is_dot || is_dot_dot) {
        component[0] = 'x';
        return 1;
    }
    for (size_t index = 0; index < span_length; ++index) {
        component[index] = span[index] == '/' ? (uint8_t)'_' : span[index];
    }
    return span_length;
}

/* A location over the root: beneath it with any bytes (the path module and the kernel judge them), the root's
 * own text where the call may name it, one sanitised component under it, relative text (refused at the door),
 * a kind outside the enum, NULL, or `beneath` with no directory. The text lives in `storage`. */
typedef struct fuzzed_location {
    bc_file_system_location value;
    bool is_null;
    uint8_t text[PATH_MAX];
} fuzzed_location;

static const bc_file_system_location *location_of(const fuzzed_location *location) {
    if (location->is_null) {
        return nullptr;
    }
    return &location->value;
}

static void take_location(reader *input, fuzzed_location *location, bool may_name_root) {
    location->is_null = false;
    size_t span_length = 0;
    const uint8_t *span = take_span(input, &span_length);
    memcpy(location->text, span, span_length);
    const uint8_t choice = take_byte(input) % 8;
    if (choice <= 1) {
        location->value = bc_file_system_location_beneath(root, location->text, span_length);
        return;
    }
    if (choice == 2 && may_name_root) {
        memcpy(location->text, root_text, root_text_length);
        location->value = bc_file_system_location_at(location->text, root_text_length);
        return;
    }
    if (choice <= 3) {
        memcpy(location->text, root_text, root_text_length);
        location->text[root_text_length] = '/';
        const size_t component_length = one_component(span, span_length, location->text + root_text_length + 1);
        location->value = bc_file_system_location_at(location->text, root_text_length + 1 + component_length);
        return;
    }
    if (choice == 4) {
        /* Relative text under `at`, which the engine refuses before any call. */
        location->text[0] = 'r';
        location->value = bc_file_system_location_at(location->text, span_length == 0 ? 1 : span_length);
        return;
    }
    if (choice == 5) {
        location->value = bc_file_system_location_beneath(root, location->text, span_length);
        location->value.kind = take_int32(input);
        return;
    }
    if (choice == 6) {
        location->is_null = true;
        return;
    }
    location->value = bc_file_system_location_beneath(nullptr, location->text, span_length);
}

/* A mode: the permission bits with the owner's kept, so a directory the fuzzer makes (or the root itself) stays
 * one it can read and empty; one time in eight a value with a bit outside 07777, which the engine refuses
 * before any call. A mode the engine would take and the fuzzer could not live with (the root at 0000) is never
 * spelled. */
static uint32_t take_mode(reader *input) {
    const uint32_t raw = (uint32_t)take_int32(input);
    if ((take_byte(input) & 7) == 0) {
        return raw | 010000U;
    }
    return (raw & 07777U) | 0700U;
}

static bc_file_system_permissions take_permissions(reader *input) {
    return (bc_file_system_permissions){.policy = take_enum(input, permissions_policy_last), .mode = take_mode(input)};
}

static bc_file_system_write_options take_write_options(reader *input) {
    return (bc_file_system_write_options){.durability = take_enum(input, durability_last),
                                          .permissions = take_permissions(input),
                                          .existing_destination = take_enum(input, existing_destination_last),
                                          .final_symlink = take_enum(input, final_symlink_last)};
}

/* A read bound: small, or at the top of the range, or past INT64_MAX. */
static uint64_t take_bound(reader *input) {
    const uint8_t choice = take_byte(input) % 4;
    if (choice == 0) {
        return take_byte(input);
    }
    if (choice == 1) {
        return (uint64_t)INT64_MAX;
    }
    if (choice == 2) {
        return (uint64_t)INT64_MAX + 1;
    }
    return take_uint64(input);
}

static bc_file_system_read_options take_read_options(reader *input) {
    return (bc_file_system_read_options){
        .limit = {.kind = take_enum(input, read_limit_kind_last), .at_most = take_bound(input)},
        .final_symlink = take_enum(input, final_symlink_last)};
}

/* A pointer to `value`, or NULL one time in eight. */
static void *maybe(reader *input, void *value) {
    if ((take_byte(input) & 7) == 0) {
        return nullptr;
    }
    return value;
}

/* An answer buffer under the protocol: the real buffer with a capacity it holds, or no buffer with a capacity
 * of 0 (the size query), or no buffer with a capacity it cannot back (a violation). */
static uint8_t *take_buffer(reader *input, uint8_t *storage, size_t *capacity) {
    const uint8_t choice = take_byte(input) % 4;
    if (choice == 0) {
        *capacity = 0;
        return nullptr;
    }
    if (choice == 1) {
        *capacity = (size_t)take_uint64(input);
        return nullptr;
    }
    *capacity = take_byte(input) % (answer_capacity + 1);
    return storage;
}

/* ---- One call per group --------------------------------------------------------------------------------------- */

static void fuzz_engine_factories(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    bc_file_system_engine *created = nullptr;
    bc_file_system_engine **slot = maybe(input, &created);
    const bool live = take_flag(input);
    const int32_t status = live ? bc_file_system_engine_create_live(slot, error)
                                : bc_file_system_engine_create_observed_for_tests(slot, error);
    check_answer(status, error,
                 live ? bc_file_system_operation_engine_create_live
                      : bc_file_system_operation_engine_create_observed_for_tests);
    if (status == 0) {
        require(created != nullptr);
    } else {
        require(created == nullptr);
    }
    bc_file_system_engine_release(created);
    bc_file_system_engine_release(nullptr);
}

static void fuzz_observer(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    bc_file_system_engine *fuzzed_engine = take_engine(input);
    const uint8_t choice = take_byte(input) % 3;
    if (choice == 0) {
        bc_file_system_fault fault = {.operation = take_enum(input, operation_last),
                                      .step = take_enum(input, step_last),
                                      .occurrence = (uint32_t)take_byte(input) % 4,
                                      .action = take_enum(input, fault_action_last),
                                      .system_code = take_enum(input, 40)};
        if (fault.action == bc_file_system_fault_action_stop && take_flag(input)) {
            fault.system_code = 0;
        }
        const int32_t status = bc_file_system_observed_fault_add(fuzzed_engine, maybe(input, &fault), error);
        check_answer(status, error, bc_file_system_operation_observed_fault_add);
        return;
    }
    if (choice == 1) {
        check_answer(bc_file_system_observed_faults_clear(fuzzed_engine, error), error,
                     bc_file_system_operation_observed_faults_clear);
        return;
    }
    uint64_t count = untouched_uint64;
    const int32_t status = bc_file_system_observed_step_count(fuzzed_engine, take_enum(input, operation_last),
                                                              take_enum(input, step_last), maybe(input, &count), error);
    check_answer(status, error, bc_file_system_operation_observed_step_count);
    if (status != 0) {
        require(count == untouched_uint64);
    }
}

static void fuzz_directory_handle(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    bc_file_system_directory *opened = nullptr;
    const int32_t status = bc_file_system_directory_open(
        take_engine(input), location_of(&location), take_enum(input, final_symlink_last), maybe(input, &opened), error);
    check_answer(status, error, bc_file_system_operation_open_directory);
    if (status != 0) {
        require(opened == nullptr);
    }
    /* The text of the handle just opened, or of the root, under the buffer protocol. */
    const bc_file_system_directory *asked = opened != nullptr && take_flag(input) ? opened : root;
    uint8_t answer[answer_capacity];
    size_t capacity = 0;
    uint8_t *buffer = take_buffer(input, answer, &capacity);
    size_t length = untouched_size;
    bc_file_system_error text_storage;
    bc_file_system_error *text_error = take_error(input, &text_storage);
    const int32_t text_status = bc_file_system_directory_opened_as(take_flag(input) ? asked : nullptr, buffer, capacity,
                                                                   maybe(input, &length), text_error);
    check_answer(text_status, text_error, bc_file_system_operation_directory_opened_as);
    check_length_answer(text_status, &length, capacity);
    bc_file_system_directory_release(opened);
    bc_file_system_directory_release(nullptr);
}

static void fuzz_status(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    int32_t present = untouched_int32;
    bc_file_system_status status_answer = {.entry_type = untouched_int32};
    const int32_t status =
        bc_file_system_status_of(take_engine(input), location_of(&location), take_enum(input, final_symlink_last),
                                 maybe(input, &present), maybe(input, &status_answer), error);
    check_answer(status, error, bc_file_system_operation_status);
    if (status != 0) {
        require(present == untouched_int32);
        require(status_answer.entry_type == untouched_int32);
    } else {
        require(present == 0 || present == 1);
        if (present == 1) {
            require(in_enum(status_answer.entry_type, bc_file_system_entry_type_other));
        }
    }
}

static void fuzz_read_file(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    bc_file_system_read_options options = take_read_options(input);
    uint8_t answer[answer_capacity];
    size_t capacity = 0;
    uint8_t *buffer = take_buffer(input, answer, &capacity);
    size_t length = untouched_size;
    const int32_t status = bc_file_system_read_file(take_engine(input), location_of(&location), maybe(input, &options),
                                                    buffer, capacity, maybe(input, &length), error);
    check_answer(status, error, bc_file_system_operation_read_file);
    check_length_answer(status, &length, capacity);
}

static void fuzz_write_file(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, false);
    size_t byte_count = 0;
    const uint8_t *bytes = take_span(input, &byte_count);
    if (take_flag(input)) {
        bytes = nullptr;
        if (take_flag(input)) {
            byte_count = 0;
        }
    }
    bc_file_system_write_options options = take_write_options(input);
    int32_t achieved = untouched_int32;
    const int32_t status = bc_file_system_write_file(take_engine(input), location_of(&location), bytes, byte_count,
                                                     maybe(input, &options), maybe(input, &achieved), error);
    check_answer(status, error, bc_file_system_operation_write_file);
    if (status != 0) {
        require(achieved == untouched_int32);
    } else {
        require(in_enum(achieved, durability_last));
    }
}

static void fuzz_append(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, false);
    size_t byte_count = 0;
    const uint8_t *bytes = take_span(input, &byte_count);
    bc_file_system_permissions permissions = take_permissions(input);
    int32_t achieved = untouched_int32;
    const int32_t status = bc_file_system_append(take_engine(input), location_of(&location), bytes, byte_count,
                                                 take_enum(input, durability_last), maybe(input, &permissions),
                                                 maybe(input, &achieved), error);
    check_answer(status, error, bc_file_system_operation_append);
    if (status != 0) {
        require(achieved == untouched_int32);
    }
}

static void fuzz_sync(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    int32_t achieved = untouched_int32;
    const int32_t status =
        bc_file_system_sync(take_engine(input), location_of(&location), maybe(input, &achieved), error);
    check_answer(status, error, bc_file_system_operation_sync);
    if (status != 0) {
        require(achieved == untouched_int32);
    }
}

static void fuzz_create_directories(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    bc_file_system_permissions permissions = take_permissions(input);
    const bool chain = take_flag(input);
    const int32_t status = chain ? bc_file_system_create_directories(take_engine(input), location_of(&location),
                                                                     maybe(input, &permissions), error)
                                 : bc_file_system_create_directory(take_engine(input), location_of(&location),
                                                                   maybe(input, &permissions), error);
    check_answer(status, error,
                 chain ? bc_file_system_operation_create_directories : bc_file_system_operation_create_directory);
}

static void fuzz_list_directory(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    bc_file_system_entry entries[entry_capacity];
    size_t capacity = take_byte(input) % (entry_capacity + 1);
    bc_file_system_entry *array = entries;
    if (take_flag(input)) {
        array = nullptr;
        if (take_flag(input)) {
            capacity = 0;
        }
    }
    size_t count = untouched_size;
    const int32_t status =
        bc_file_system_list_directory(take_engine(input), location_of(&location), take_enum(input, final_symlink_last),
                                      array, capacity, maybe(input, &count), error);
    check_answer(status, error, bc_file_system_operation_list_directory);
    check_length_answer(status, &count, capacity);
    if (status == 0) {
        for (size_t index = 0; index < count; ++index) {
            require(in_enum(entries[index].entry_type, bc_file_system_entry_type_other));
            require(entries[index].name_length >= 1 && entries[index].name_length <= sizeof entries[index].name);
        }
    }
}

static void fuzz_removals(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, false);
    int32_t existed = untouched_int32;
    const uint8_t choice = take_byte(input) % 3;
    int32_t status = 0;
    int32_t operation = 0;
    if (choice == 0) {
        operation = bc_file_system_operation_remove_file;
        status = bc_file_system_remove_file(take_engine(input), location_of(&location), maybe(input, &existed), error);
    } else if (choice == 1) {
        operation = bc_file_system_operation_remove_directory;
        status =
            bc_file_system_remove_directory(take_engine(input), location_of(&location), maybe(input, &existed), error);
    } else {
        operation = bc_file_system_operation_remove_tree;
        status = bc_file_system_remove_tree(take_engine(input), location_of(&location), maybe(input, &existed), error);
    }
    check_answer(status, error, operation);
    if (status != 0) {
        require(existed == untouched_int32);
    } else {
        require(existed == 0 || existed == 1);
    }
}

static void fuzz_create_temporary_directory(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    bc_file_system_permissions permissions = take_permissions(input);
    uint8_t answer[answer_capacity];
    size_t capacity = 0;
    uint8_t *buffer = take_buffer(input, answer, &capacity);
    size_t length = untouched_size;
    const int32_t status = bc_file_system_create_temporary_directory(take_engine(input), location_of(&location),
                                                                     maybe(input, &permissions), buffer, capacity,
                                                                     maybe(input, &length), error);
    check_answer(status, error, bc_file_system_operation_create_temporary_directory);
    check_length_answer(status, &length, capacity);
}

static void fuzz_rename_and_copy(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location source;
    fuzzed_location destination;
    take_location(input, &source, false);
    take_location(input, &destination, false);
    int32_t achieved = untouched_int32;
    const bool copy = take_flag(input);
    int32_t status = 0;
    if (copy) {
        bc_file_system_write_options options = take_write_options(input);
        status = bc_file_system_copy_file(take_engine(input), location_of(&source), location_of(&destination),
                                          maybe(input, &options), maybe(input, &achieved), error);
    } else {
        status = bc_file_system_rename(take_engine(input), location_of(&source), location_of(&destination),
                                       take_enum(input, existing_destination_last), take_enum(input, durability_last),
                                       maybe(input, &achieved), error);
    }
    check_answer(status, error, copy ? bc_file_system_operation_copy_file : bc_file_system_operation_rename);
    if (status != 0) {
        require(achieved == untouched_int32);
    }
}

static void fuzz_set_permissions(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    const int32_t status = bc_file_system_set_permissions(take_engine(input), location_of(&location), take_mode(input),
                                                          take_enum(input, final_symlink_last), error);
    check_answer(status, error, bc_file_system_operation_set_permissions);
}

static void fuzz_symlinks(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    if (take_flag(input)) {
        /* The target is one component (never a path that could lead out of the root when followed), or no
         * target at all, with or without a length, for the argument check. */
        size_t span_length = 0;
        const uint8_t *span = take_span(input, &span_length);
        uint8_t component[256];
        size_t target_length = one_component(span, span_length, component);
        const uint8_t *target = component;
        if (take_flag(input)) {
            target = nullptr;
            if (take_flag(input)) {
                target_length = 0;
            }
        }
        check_answer(
            bc_file_system_create_symlink(take_engine(input), target, target_length, location_of(&location), error),
            error, bc_file_system_operation_create_symlink);
        return;
    }
    uint8_t answer[answer_capacity];
    size_t capacity = 0;
    uint8_t *buffer = take_buffer(input, answer, &capacity);
    size_t length = untouched_size;
    const int32_t status = bc_file_system_read_symlink(take_engine(input), location_of(&location), buffer, capacity,
                                                       maybe(input, &length), error);
    check_answer(status, error, bc_file_system_operation_read_symlink);
    check_length_answer(status, &length, capacity);
}

static void fuzz_try_lock(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, true);
    bc_file_system_lock *lock = nullptr;
    int32_t outcome = untouched_int32;
    const int32_t status = bc_file_system_try_lock(take_engine(input), location_of(&location), maybe(input, &lock),
                                                   maybe(input, &outcome), error);
    check_answer(status, error, bc_file_system_operation_try_lock);
    if (status != 0) {
        require(lock == nullptr);
        require(outcome == untouched_int32);
    } else {
        require(in_enum(outcome, bc_file_system_lock_outcome_held_elsewhere));
        require((outcome == bc_file_system_lock_outcome_acquired) == (lock != nullptr));
    }
    bc_file_system_lock_release(lock);
    bc_file_system_lock_release(nullptr);
}

static void fuzz_condemn(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    fuzzed_location location;
    take_location(input, &location, false);
    if (take_flag(input)) {
        fuzzed_location parent;
        take_location(input, &parent, true);
        check_answer(bc_file_system_remove_condemned(take_engine(input), location_of(&parent), error), error,
                     bc_file_system_operation_remove_condemned);
        return;
    }
    uint8_t answer[answer_capacity];
    size_t capacity = 0;
    uint8_t *buffer = take_buffer(input, answer, &capacity);
    size_t length = untouched_size;
    const int32_t status = bc_file_system_condemn(take_engine(input), location_of(&location), buffer, capacity,
                                                  maybe(input, &length), error);
    check_answer(status, error, bc_file_system_operation_condemn);
    check_length_answer(status, &length, capacity);
}

/* The path module: pure functions over spans, with NULL and length pairs and the buffer protocol fuzzed. */
static void fuzz_path_module(reader *input) {
    bc_file_system_error storage;
    bc_file_system_error *error = take_error(input, &storage);
    size_t first_length = 0;
    const uint8_t *first = take_span(input, &first_length);
    size_t second_length = 0;
    const uint8_t *second = take_span(input, &second_length);
    if (take_flag(input)) {
        first = nullptr;
    }
    uint8_t answer[answer_capacity];
    size_t capacity = 0;
    uint8_t *buffer = take_buffer(input, answer, &capacity);
    size_t length = untouched_size;
    size_t offset = untouched_size;
    size_t other_length = untouched_size;
    int32_t present = untouched_int32;
    size_t cursor = take_flag(input) ? 0 : (size_t)take_uint64(input);
    const uint8_t choice = take_byte(input) % 12;
    int32_t status = 0;
    int32_t operation = 0;
    if (choice == 0) {
        operation = bc_file_system_operation_relative_path_from;
        status = bc_file_system_relative_path_validate(first, first_length, error);
    } else if (choice == 1) {
        operation = bc_file_system_operation_absolute_path_from;
        status = bc_file_system_absolute_path_validate(first, first_length, error);
    } else if (choice == 2) {
        operation = bc_file_system_operation_path_join;
        status = bc_file_system_path_join(first, first_length, second, second_length, buffer, capacity,
                                          maybe(input, &length), error);
        check_length_answer(status, &length, capacity);
    } else if (choice == 3) {
        operation = bc_file_system_operation_path_parent;
        status = bc_file_system_path_parent(first, first_length, maybe(input, &present), maybe(input, &length), error);
    } else if (choice == 4) {
        operation = bc_file_system_operation_path_last_component;
        status = bc_file_system_path_last_component(first, first_length, maybe(input, &offset), maybe(input, &length),
                                                    error);
    } else if (choice == 5) {
        operation = bc_file_system_operation_path_next_component;
        status = bc_file_system_path_next_component(first, first_length, maybe(input, &cursor), maybe(input, &offset),
                                                    maybe(input, &length), maybe(input, &present), error);
    } else if (choice == 6) {
        operation = bc_file_system_operation_path_stem;
        status = bc_file_system_path_stem(first, first_length, maybe(input, &length), error);
    } else if (choice == 7) {
        operation = bc_file_system_operation_path_extension;
        status = bc_file_system_path_extension(first, first_length, maybe(input, &offset), maybe(input, &length),
                                               maybe(input, &present), error);
    } else if (choice == 8) {
        operation = bc_file_system_operation_path_relate;
        status = bc_file_system_path_relate(first, first_length, second, second_length, maybe(input, &present), error);
    } else if (choice == 9) {
        operation = bc_file_system_operation_path_lexically_normalize;
        status = bc_file_system_path_lexically_normalize(first, first_length, buffer, capacity, maybe(input, &length),
                                                         error);
        check_length_answer(status, &length, capacity);
    } else if (choice == 10) {
        operation = bc_file_system_operation_path_resolve_against;
        status = bc_file_system_path_resolve_against(first, first_length, second, second_length, buffer, capacity,
                                                     maybe(input, &length), error);
        check_length_answer(status, &length, capacity);
    } else {
        operation = bc_file_system_operation_path_library_name;
        status = bc_file_system_path_library_name(first, first_length, maybe(input, &present), maybe(input, &offset),
                                                  maybe(input, &other_length), error);
    }
    check_answer(status, error, operation);
    if (status != 0 && status != bc_file_system_error_kind_buffer_too_small) {
        require(offset == untouched_size);
        require(other_length == untouched_size);
        require(present == untouched_int32);
    }
}

/* ---- The root between inputs -------------------------------------------------------------------------------- */

/* Empties the root through the engine, with nothing scripted, so the next input starts from the same place:
 * every entry a listing answers goes as a tree. An entry the engine cannot remove is a finding of its own. */
static void empty_the_root(void) {
    bc_file_system_error error;
    require(bc_file_system_observed_faults_clear(engine, &error) == 0);
    const bc_file_system_location at_root = bc_file_system_location_at((const uint8_t *)root_text, root_text_length);
    /* A few calls make a few entries; the array holds many more. */
    bc_file_system_entry entries[64];
    size_t count = 0;
    require(bc_file_system_list_directory(engine, &at_root, bc_file_system_final_symlink_refuse, entries,
                                          sizeof entries / sizeof entries[0], &count, &error) == 0);
    for (size_t index = 0; index < count; ++index) {
        const bc_file_system_location entry =
            bc_file_system_location_beneath(root, entries[index].name, entries[index].name_length);
        int32_t existed = 0;
        require(bc_file_system_remove_tree(engine, &entry, &existed, &error) == 0);
    }
}

/* ---- libFuzzer's two entry points ---------------------------------------------------------------------------- */

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize([[maybe_unused]] int *argc, [[maybe_unused]] char ***argv) {
    root_text_length = make_fuzz_root("abi", root_text, sizeof root_text);
    /* The root's text, a slash and the longest component must fit the text a location borrows. */
    require(root_text_length + 1 + 255 < PATH_MAX);
    bc_file_system_error error;
    require(bc_file_system_engine_create_observed_for_tests(&engine, &error) == 0);
    const bc_file_system_location at_root = bc_file_system_location_at((const uint8_t *)root_text, root_text_length);
    require(bc_file_system_directory_open(engine, &at_root, bc_file_system_final_symlink_refuse, &root, &error) == 0);
    descriptors_at_start = descriptors_under(root_text, root_text_length);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > longest_input) {
        return 0;
    }
    reader input = {.data = data, .size = size, .position = 0};
    /* A few calls per input, each over a group chosen by one byte, so a fault scripted by one call can reach
     * the calls after it. */
    const int calls = 1 + take_byte(&input) % 4;
    for (int call = 0; call < calls; ++call) {
        const uint8_t group = take_byte(&input) % 17;
        if (group == 0) {
            fuzz_engine_factories(&input);
        } else if (group == 1) {
            fuzz_observer(&input);
        } else if (group == 2) {
            fuzz_directory_handle(&input);
        } else if (group == 3) {
            fuzz_status(&input);
        } else if (group == 4) {
            fuzz_read_file(&input);
        } else if (group == 5) {
            fuzz_write_file(&input);
        } else if (group == 6) {
            fuzz_append(&input);
        } else if (group == 7) {
            fuzz_sync(&input);
        } else if (group == 8) {
            fuzz_create_directories(&input);
        } else if (group == 9) {
            fuzz_list_directory(&input);
        } else if (group == 10) {
            fuzz_removals(&input);
        } else if (group == 11) {
            fuzz_create_temporary_directory(&input);
        } else if (group == 12) {
            fuzz_rename_and_copy(&input);
        } else if (group == 13) {
            fuzz_set_permissions(&input);
        } else if (group == 14) {
            fuzz_symlinks(&input);
        } else if (group == 15) {
            fuzz_try_lock(&input);
        } else {
            fuzz_condemn(&input);
        }
        if (take_flag(&input)) {
            fuzz_path_module(&input);
        }
    }
    empty_the_root();
    require(descriptors_under(root_text, root_text_length) == descriptors_at_start);
    return 0;
}
