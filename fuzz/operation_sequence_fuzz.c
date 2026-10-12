/* operation_sequence_fuzz.c — libFuzzer over sequences of operations on one tree, with the engine's promises
 * checked after every one of them.
 *
 * Each input is a sequence of operations over a small, fixed vocabulary of names and payloads, so the
 * operations meet each other's leftovers: a write beside a condemned tree, a rename onto a locked file, a copy
 * through a symlink, a sweep after a kill. Every operation runs on the observed engine and the sequence may
 * script faults into it, failing a step or stopping an operation as a kill would, which is where the
 * kill-safety of every multi-step operation is exercised against every other. Three invariants hold after
 * every operation, whatever it answered:
 *
 *   - descriptor balance: the descriptors open on paths under the root are the harness's two handles, plus
 *     one while the sequence holds a lock (a handle owns one descriptor), with no count of the whole table;
 *   - nothing is created outside the root: the listing of the root's own parent is the root alone;
 *   - no partial file under a live name: every regular file under the root holds one of the payloads the
 *     sequence writes, followed by whatever the appends added, byte for byte; a staged file (its name carries
 *     the engine's staging infix) is a killed write's and is never a live name.
 *
 * The checks read the tree through a second, live engine with a root handle of its own, so a fault the sequence
 * scripted into the observed engine fires in the sequence and never in a check. The root is
 * `<handed root>/sequence-<pid>-<n>/root`, one level below a parent of its own, so the parent listing is this
 * fuzzer's alone. Between inputs the root is emptied through the checking engine, the handles released and the
 * faults cleared.
 */
#include <bc_file_system/bc_file_system.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "names.h"

enum : uint32_t { longest_input = 4096, most_operations_per_input = 200 };

/* The vocabulary. Every name is a valid relative path beneath the root; the nested ones make the directories
 * above them matter. */
static const char *const names[] = {"a", "b", "c", "dir", "dir/x", "dir/y", "link", "deep", "deep/er", ".hidden"};
enum : size_t { name_count = sizeof names / sizeof names[0] };

/* The payloads: what a regular file under a live name may hold, apart from the appends. The large one crosses
 * every buffer the engine and this fuzzer hold. */
enum : size_t { large_payload_length = 70'000 };
static uint8_t large_payload[large_payload_length];
typedef struct payload {
    const uint8_t *bytes;
    size_t length;
} payload;
static payload payloads[4];
enum : size_t { payload_count = sizeof payloads / sizeof payloads[0] };

/* The byte every append adds. No payload ends in it, so the appends are told apart from the payload. */
static const uint8_t append_byte = '+';

/* The modes the sequence states: a directory keeps the owner's search and read, so the invariant walk can
 * enter every directory the sequence makes, and a file keeps the owner's read. */
static const uint32_t directory_modes[] = {0700, 0755};
static const uint32_t file_modes[] = {0600, 0644, 0700, 0755};

/* The errnos a scripted fault answers with: the ones the engine reads specially and one it does not. */
static const int32_t fault_codes[] = {EIO, ENOSPC, EACCES, EEXIST, ENOENT, EINTR, ENOTSUP};

enum : int32_t {
    operation_first_over_a_location = bc_file_system_operation_status,
    operation_last_over_a_location = bc_file_system_operation_remove_condemned,
    step_last = bc_file_system_step_faccessat
};

/* The tree walk reads every file into this; the engine answers `buffer_too_small` with the size it needs, and
 * no file the sequence can make is larger than the largest payload and the appends one input adds. */
enum : size_t { read_capacity = large_payload_length + most_operations_per_input + 1, entry_capacity = 512 };
static uint8_t read_buffer[read_capacity];
static bc_file_system_entry entries[entry_capacity];

static bc_file_system_engine *engine = nullptr;
static bc_file_system_directory *root = nullptr;
/* The checks' engine and its own handle on the root: live, so nothing scripted reaches a check. */
static bc_file_system_engine *checker = nullptr;
static bc_file_system_directory *checker_root = nullptr;
static char parent_text[PATH_MAX];
static size_t parent_text_length = 0;
static char root_text[PATH_MAX];
static size_t root_text_length = 0;
static int descriptors_at_start = -1;
static bc_file_system_lock *held_lock = nullptr;

/* ---- The sequence's arguments ------------------------------------------------------------------------------- */

static bc_file_system_location take_name(reader *input) {
    const char *name = names[take_byte(input) % name_count];
    return bc_file_system_location_beneath(root, (const uint8_t *)name, strlen(name));
}

static payload take_payload(reader *input) {
    return payloads[take_byte(input) % payload_count];
}

static uint32_t take_directory_mode(reader *input) {
    return directory_modes[take_byte(input) % (sizeof directory_modes / sizeof directory_modes[0])];
}

static uint32_t take_file_mode(reader *input) {
    return file_modes[take_byte(input) % (sizeof file_modes / sizeof file_modes[0])];
}

static int32_t take_two(reader *input, int32_t first, int32_t second) {
    return take_flag(input) ? first : second;
}

static bc_file_system_permissions take_file_permissions(reader *input) {
    return (bc_file_system_permissions){.policy = take_two(input, bc_file_system_permissions_policy_exactly,
                                                           bc_file_system_permissions_policy_preserve_or),
                                        .mode = take_file_mode(input)};
}

static bc_file_system_write_options take_write_options(reader *input) {
    return (bc_file_system_write_options){
        .durability = take_two(input, bc_file_system_durability_written, bc_file_system_durability_durable),
        .permissions = take_file_permissions(input),
        .existing_destination =
            take_two(input, bc_file_system_existing_destination_replace, bc_file_system_existing_destination_refuse),
        .final_symlink = take_two(input, bc_file_system_final_symlink_refuse, bc_file_system_final_symlink_follow)};
}

/* ---- One operation ------------------------------------------------------------------------------------------- */

/* Every answer is a status the engine chose; the sequence reads it for nothing but the invariant that the
 * struct agrees with it. What an operation did to the tree is what the walk afterwards judges. */
static void expect_consistent(int32_t status, const bc_file_system_error *error) {
    require(status == error->kind);
}

static void run_one_operation(reader *input) {
    bc_file_system_error error;
    const bc_file_system_location location = take_name(input);
    const bc_file_system_location other = take_name(input);
    const uint8_t choice = take_byte(input) % 22;
    if (choice == 0) {
        const payload bytes = take_payload(input);
        const bc_file_system_write_options options = take_write_options(input);
        int32_t achieved = 0;
        expect_consistent(
            bc_file_system_write_file(engine, &location, bytes.bytes, bytes.length, &options, &achieved, &error),
            &error);
    } else if (choice == 1) {
        const bc_file_system_permissions permissions = take_file_permissions(input);
        int32_t achieved = 0;
        expect_consistent(
            bc_file_system_append(engine, &location, &append_byte, 1,
                                  take_two(input, bc_file_system_durability_written, bc_file_system_durability_durable),
                                  &permissions, &achieved, &error),
            &error);
    } else if (choice == 2) {
        const bc_file_system_read_options options = {
            .limit = {.kind = bc_file_system_read_limit_kind_whole_file, .at_most = 0},
            .final_symlink = take_two(input, bc_file_system_final_symlink_refuse, bc_file_system_final_symlink_follow)};
        size_t length = 0;
        expect_consistent(
            bc_file_system_read_file(engine, &location, &options, read_buffer, sizeof read_buffer, &length, &error),
            &error);
    } else if (choice == 3) {
        int32_t present = 0;
        bc_file_system_status status;
        expect_consistent(bc_file_system_status_of(
                              engine, &location,
                              take_two(input, bc_file_system_final_symlink_refuse, bc_file_system_final_symlink_follow),
                              &present, &status, &error),
                          &error);
    } else if (choice == 4) {
        int32_t achieved = 0;
        expect_consistent(bc_file_system_sync(engine, &location, &achieved, &error), &error);
    } else if (choice == 5) {
        const bc_file_system_permissions permissions = {.policy = bc_file_system_permissions_policy_exactly,
                                                        .mode = take_directory_mode(input)};
        expect_consistent(take_flag(input) ? bc_file_system_create_directory(engine, &location, &permissions, &error)
                                           : bc_file_system_create_directories(engine, &location, &permissions, &error),
                          &error);
    } else if (choice == 6) {
        size_t count = 0;
        expect_consistent(bc_file_system_list_directory(
                              engine, &location,
                              take_two(input, bc_file_system_final_symlink_refuse, bc_file_system_final_symlink_follow),
                              entries, entry_capacity, &count, &error),
                          &error);
    } else if (choice == 7) {
        int32_t existed = 0;
        expect_consistent(bc_file_system_remove_file(engine, &location, &existed, &error), &error);
    } else if (choice == 8) {
        int32_t existed = 0;
        expect_consistent(bc_file_system_remove_directory(engine, &location, &existed, &error), &error);
    } else if (choice == 9) {
        int32_t existed = 0;
        expect_consistent(bc_file_system_remove_tree(engine, &location, &existed, &error), &error);
    } else if (choice == 10) {
        const bc_file_system_permissions permissions = {.policy = bc_file_system_permissions_policy_exactly,
                                                        .mode = take_directory_mode(input)};
        uint8_t name[NAME_MAX];
        size_t length = 0;
        expect_consistent(bc_file_system_create_temporary_directory(engine, &location, &permissions, name, sizeof name,
                                                                    &length, &error),
                          &error);
    } else if (choice == 11) {
        int32_t achieved = 0;
        expect_consistent(
            bc_file_system_rename(engine, &location, &other,
                                  take_two(input, bc_file_system_existing_destination_replace,
                                           bc_file_system_existing_destination_refuse),
                                  take_two(input, bc_file_system_durability_written, bc_file_system_durability_durable),
                                  &achieved, &error),
            &error);
    } else if (choice == 12) {
        const bc_file_system_write_options options = take_write_options(input);
        int32_t achieved = 0;
        expect_consistent(bc_file_system_copy_file(engine, &location, &other, &options, &achieved, &error), &error);
    } else if (choice == 13) {
        /* A directory keeps a mode the walk can enter; a file's mode is any of the stated ones. */
        const uint32_t mode = take_flag(input) ? take_directory_mode(input) : take_file_mode(input);
        expect_consistent(bc_file_system_set_permissions(
                              engine, &location, mode,
                              take_two(input, bc_file_system_final_symlink_refuse, bc_file_system_final_symlink_follow),
                              &error),
                          &error);
    } else if (choice == 14) {
        /* The target is another name of the vocabulary, relative: a link points inside the root or dangles. */
        expect_consistent(bc_file_system_create_symlink(engine, other.path, other.length, &location, &error), &error);
    } else if (choice == 15) {
        size_t length = 0;
        expect_consistent(
            bc_file_system_read_symlink(engine, &location, read_buffer, sizeof read_buffer, &length, &error), &error);
    } else if (choice == 16) {
        /* One lock at a time: a second is let go before the next is taken, so the descriptor invariant can
         * count the one it holds. */
        bc_file_system_lock_release(held_lock);
        held_lock = nullptr;
        bc_file_system_lock *lock = nullptr;
        int32_t outcome = 0;
        const int32_t status = bc_file_system_try_lock(engine, &location, &lock, &outcome, &error);
        expect_consistent(status, &error);
        if (status == 0 && outcome == bc_file_system_lock_outcome_acquired) {
            held_lock = lock;
        }
    } else if (choice == 17) {
        uint8_t name[NAME_MAX];
        size_t length = 0;
        expect_consistent(bc_file_system_condemn(engine, &location, name, sizeof name, &length, &error), &error);
    } else if (choice == 18) {
        const bc_file_system_location at_root =
            bc_file_system_location_at((const uint8_t *)root_text, root_text_length);
        expect_consistent(bc_file_system_remove_condemned(engine, take_flag(input) ? &at_root : &location, &error),
                          &error);
    } else if (choice == 19) {
        /* A directory opened through the engine, one operation beneath it, and the handle released: the
         * remembered text and the contained walk from a held directory below the root. */
        bc_file_system_directory *opened = nullptr;
        const int32_t status = bc_file_system_directory_open(
            engine, &location,
            take_two(input, bc_file_system_final_symlink_refuse, bc_file_system_final_symlink_follow), &opened, &error);
        expect_consistent(status, &error);
        if (status == 0) {
            const bc_file_system_location beneath = bc_file_system_location_beneath(opened, other.path, other.length);
            int32_t present = 0;
            bc_file_system_status status_answer;
            expect_consistent(bc_file_system_status_of(engine, &beneath, bc_file_system_final_symlink_refuse, &present,
                                                       &status_answer, &error),
                              &error);
            bc_file_system_directory_release(opened);
        }
    } else if (choice == 20) {
        const bc_file_system_fault fault = {
            .operation = operation_first_over_a_location +
                         (int32_t)(take_byte(input) %
                                   (uint8_t)(operation_last_over_a_location - operation_first_over_a_location + 1)),
            .step = 1 + (int32_t)(take_byte(input) % (uint8_t)step_last),
            .occurrence = 1 + (uint32_t)(take_byte(input) % 4),
            .action = take_two(input, bc_file_system_fault_action_fail, bc_file_system_fault_action_stop),
            .system_code = 0};
        bc_file_system_fault scripted = fault;
        if (scripted.action == bc_file_system_fault_action_fail) {
            scripted.system_code = fault_codes[take_byte(input) % (sizeof fault_codes / sizeof fault_codes[0])];
        }
        expect_consistent(bc_file_system_observed_fault_add(engine, &scripted, &error), &error);
    } else {
        expect_consistent(bc_file_system_observed_faults_clear(engine, &error), &error);
    }
}

/* ---- The invariants ----------------------------------------------------------------------------------------- */

/* Whether `bytes` is one of the payloads followed by appends alone. */
static bool is_a_whole_content(const uint8_t *bytes, size_t length) {
    size_t appended = 0;
    while (appended < length && bytes[length - 1 - appended] == append_byte) {
        appended += 1;
    }
    const size_t body = length - appended;
    for (size_t index = 0; index < payload_count; ++index) {
        if (payloads[index].length == body && memcmp(payloads[index].bytes, bytes, body) == 0) {
            return true;
        }
    }
    return false;
}

static bool carries_staging_infix(const uint8_t *name, size_t length) {
    const size_t infix_length = sizeof staging_infix - 1;
    if (length < infix_length) {
        return false;
    }
    for (size_t start = 0; start + infix_length <= length; ++start) {
        if (memcmp(name + start, staging_infix, infix_length) == 0) {
            return true;
        }
    }
    return false;
}

/* The directories still to be read, as paths relative to the root (empty for the root itself): the walk holds
 * them in a list rather than on the stack, since the core itself allows no recursion and a fuzzer should not
 * need what the library refuses. The vocabulary spells at most two levels; the temporary directories are
 * empty, so every level is one listing. */
enum : size_t { pending_capacity = 256 };
static uint8_t pending[pending_capacity][PATH_MAX];
static size_t pending_lengths[pending_capacity];

/* Reads every regular file under the root through the checking engine and holds its bytes to the payloads.
 * Directories are entered by name on `beneath` locations, which the kernel contains; a symlink is listed as
 * itself and never followed; a staged file is a killed write's and is skipped. */
static void check_every_file_is_whole(void) {
    bc_file_system_error error;
    const bc_file_system_read_options options = {
        .limit = {.kind = bc_file_system_read_limit_kind_whole_file, .at_most = 0},
        .final_symlink = bc_file_system_final_symlink_refuse};
    size_t pending_count = 1;
    pending_lengths[0] = 0;
    while (pending_count > 0) {
        pending_count -= 1;
        const size_t relative_length = pending_lengths[pending_count];
        uint8_t relative[PATH_MAX];
        memcpy(relative, pending[pending_count], relative_length);
        const bc_file_system_location at_root =
            bc_file_system_location_at((const uint8_t *)root_text, root_text_length);
        const bc_file_system_location directory =
            relative_length == 0 ? at_root : bc_file_system_location_beneath(checker_root, relative, relative_length);
        size_t count = 0;
        require(bc_file_system_list_directory(checker, &directory, bc_file_system_final_symlink_refuse, entries,
                                              entry_capacity, &count, &error) == 0);
        for (size_t index = 0; index < count; ++index) {
            uint8_t path[PATH_MAX];
            size_t path_length = 0;
            if (relative_length > 0) {
                memcpy(path, relative, relative_length);
                path_length = relative_length;
                path[path_length] = '/';
                path_length += 1;
            }
            require(path_length + entries[index].name_length < sizeof path);
            memcpy(path + path_length, entries[index].name, entries[index].name_length);
            path_length += entries[index].name_length;
            if (entries[index].entry_type == bc_file_system_entry_type_directory) {
                require(pending_count < pending_capacity);
                memcpy(pending[pending_count], path, path_length);
                pending_lengths[pending_count] = path_length;
                pending_count += 1;
                continue;
            }
            if (entries[index].entry_type != bc_file_system_entry_type_regular_file) {
                continue;
            }
            if (carries_staging_infix(entries[index].name, entries[index].name_length)) {
                continue;
            }
            const bc_file_system_location file = bc_file_system_location_beneath(checker_root, path, path_length);
            size_t length = 0;
            require(bc_file_system_read_file(checker, &file, &options, read_buffer, sizeof read_buffer, &length,
                                             &error) == 0);
            require(is_a_whole_content(read_buffer, length));
        }
    }
}

static void check_invariants(void) {
    bc_file_system_error error;
    /* Descriptor balance, with the one the held lock owns. */
    const int expected = descriptors_at_start + (held_lock != nullptr ? 1 : 0);
    require(descriptors_under(root_text, root_text_length) == expected);
    /* Nothing outside the root: its parent holds the root alone. */
    const bc_file_system_location at_parent =
        bc_file_system_location_at((const uint8_t *)parent_text, parent_text_length);
    size_t count = 0;
    require(bc_file_system_list_directory(checker, &at_parent, bc_file_system_final_symlink_refuse, entries,
                                          entry_capacity, &count, &error) == 0);
    require(count == 1);
    require(entries[0].name_length == 4 && memcmp(entries[0].name, "root", 4) == 0);
    /* No partial file under a live name. */
    check_every_file_is_whole();
}

/* ---- The root between inputs -------------------------------------------------------------------------------- */

/* Empties the root through the checking engine, so the next input starts from the same place: every entry a
 * listing answers goes as a tree. An entry the engine cannot remove is a finding of its own. */
static void empty_the_root(void) {
    bc_file_system_lock_release(held_lock);
    held_lock = nullptr;
    bc_file_system_error error;
    require(bc_file_system_observed_faults_clear(engine, &error) == 0);
    const bc_file_system_location at_root = bc_file_system_location_at((const uint8_t *)root_text, root_text_length);
    size_t count = 0;
    require(bc_file_system_list_directory(checker, &at_root, bc_file_system_final_symlink_refuse, entries,
                                          entry_capacity, &count, &error) == 0);
    for (size_t index = 0; index < count; ++index) {
        const bc_file_system_location entry =
            bc_file_system_location_beneath(checker_root, entries[index].name, entries[index].name_length);
        int32_t existed = 0;
        require(bc_file_system_remove_tree(checker, &entry, &existed, &error) == 0);
    }
}

/* ---- libFuzzer's two entry points ---------------------------------------------------------------------------- */

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize([[maybe_unused]] int *argc, [[maybe_unused]] char ***argv) {
    parent_text_length = make_fuzz_root("sequence", parent_text, sizeof parent_text);
    require(parent_text_length + 5 < sizeof root_text);
    memcpy(root_text, parent_text, parent_text_length);
    memcpy(root_text + parent_text_length, "/root", 6);
    root_text_length = parent_text_length + 5;
    require(mkdir(root_text, 0700) == 0);
    for (size_t index = 0; index < large_payload_length; ++index) {
        large_payload[index] = (uint8_t)('A' + (index % 26));
    }
    payloads[0] = (payload){.bytes = (const uint8_t *)"", .length = 0};
    payloads[1] = (payload){.bytes = (const uint8_t *)"alpha", .length = 5};
    payloads[2] = (payload){.bytes = (const uint8_t *)"the second payload\n", .length = 19};
    payloads[3] = (payload){.bytes = large_payload, .length = large_payload_length};
    bc_file_system_error error;
    require(bc_file_system_engine_create_observed_for_tests(&engine, &error) == 0);
    require(bc_file_system_engine_create_live(&checker, &error) == 0);
    const bc_file_system_location at_root = bc_file_system_location_at((const uint8_t *)root_text, root_text_length);
    require(bc_file_system_directory_open(engine, &at_root, bc_file_system_final_symlink_refuse, &root, &error) == 0);
    require(bc_file_system_directory_open(checker, &at_root, bc_file_system_final_symlink_refuse, &checker_root,
                                          &error) == 0);
    descriptors_at_start = descriptors_under(root_text, root_text_length);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2 || size > longest_input) {
        return 0;
    }
    reader input = {.data = data, .size = size, .position = 0};
    uint32_t operations = 0;
    while (input.position < input.size && operations < most_operations_per_input) {
        run_one_operation(&input);
        check_invariants();
        operations += 1;
    }
    empty_the_root();
    require(descriptors_under(root_text, root_text_length) == descriptors_at_start);
    return 0;
}
