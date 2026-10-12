/* resolve_test.c — the C-only test of the resolution layer, over a tree it plants under a root it is handed.
 *
 * One `main` over cases, registered with add_test with the root directory as its one argument: the test never
 * asks the system where temporary files go. Under that root it plants the S2 spike's tree with mkdirat,
 * symlinkat and openat on held descriptors, the one place the C tests touch the file system, and removes it
 * when it ends. The cases drive resolve.h directly, with no operation in between, and the public
 * `bc_file_system_directory_open` through the header: every refusal, the walk over an `at` path and a
 * `beneath` path, the fast path where the platform has one and the walk it falls back to when the fast
 * path's step is scripted to refuse, and the identity check against a directory swapped between the stat and
 * the open, which is deterministic here because the two halves of a checked open are called one at a time
 * and the swap lands between them. Every case counts the open descriptors before and after itself: a leaked
 * descriptor is the one leak no sanitizer reports.
 *
 *   root/a/b/target.txt   "INSIDE"      what a correct walk reaches
 *   root/file                           a regular file where a directory is needed
 *   root/link_to_a -> a                 a symlink on the way, followed under `at`, refused under `beneath`
 *   root/dangling -> nowhere            a symlink to nothing
 *   root/locked/              0000      a directory that cannot be opened (where permissions are enforced)
 *   root/<255 x's>/… five deep          a parent text longer than PATH_MAX, which only the walk can open
 *   outside/target.txt    "OUTSIDE"     the sibling tree a swapped-in symlink points at
 *   staging/b_symlink -> ../../outside  swapped for root/a/b: a symlink where a directory was
 *   staging/b_decoy/target.txt "DECOY"  swapped for root/a/b: a directory where another directory was
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif
#if __has_include(<linux/fs.h>)
#include <linux/fs.h>
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <bc_file_system/bc_file_system.h>

#include "engine.h"
#include "owned.h"
#include "resolve.h"

/* How many checks ran and how many failed; main reports both. Handed to every check rather than kept in a
 * global, so the file reads like the library it tests. */
typedef struct report {
    int checked;
    int failed;
} report;

/* `held` is one comparison or one predicate, never a chain: a chain under && is an int in C, and the C lint set
 * reports its conversion, so a case that needs two facts states two checks. */
static void expect(report *tally, bool held, const char *what, const char *detail) {
    tally->checked += 1;
    if (!held) {
        tally->failed += 1;
        printf("FAILED: %s: %s\n", what, detail);
    }
}

enum : uint32_t {
    /* How far the descriptor sweep looks; the test holds a handful of descriptors at a time. */
    descriptor_sweep_limit = 4096,
    /* A path buffer for the `at` texts the test builds from the root it was handed. */
    /* Holds the deep chain's text: long_component_depth names of NAME_MAX bytes, their slashes and a leaf. */
    text_capacity = 8192,
    directory_mode = 0700,
    file_mode = 0600,
    no_access_mode = 0000
};

enum : int32_t {
    directory_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC,
    /* The longest component the path module accepts, so five of them exceed PATH_MAX on every platform. */
    long_component_length = NAME_MAX,
    /* Seventeen components of NAME_MAX bytes: 17 * 256 bytes of parent text, longer than PATH_MAX on every
     * platform the library ships for (4096 on Linux, 1024 on Apple), so no one-call open can take it. */
    long_component_depth = 17
};

static const int32_t operation = bc_file_system_operation_open_directory;

/* ---- The descriptor balance ----------------------------------------------------------------------------------- */

/* How many descriptors are open: every number up to the sweep limit is asked whether it is one. */
static int count_open_descriptors(void) {
    int count = 0;
    for (int descriptor = 0; descriptor < (int)descriptor_sweep_limit; ++descriptor) {
        if (fcntl(descriptor, F_GETFD) != -1) {
            count += 1;
        }
    }
    return count;
}

/* ---- The planted tree ----------------------------------------------------------------------------------------- */

typedef struct fixture {
    const char *root_text; /* the directory the test was handed, absolute */
    int handed_root;       /* its descriptor */
    int base;              /* `<root_text>/resolve_test`, the test's own directory */
    int root;              /* `<root_text>/resolve_test/root` */
    char base_text[text_capacity];
    char root_tree_text[text_capacity];
    bool permissions_enforced;
} fixture;

static bool plant_directory(int parent, const char *name, uint32_t mode) {
    if (mkdirat(parent, name, (mode_t)mode) != 0) {
        const int code = errno;
        printf("mkdirat %s: errno %d\n", name, code);
        return false;
    }
    return true;
}

static bool plant_file(int parent, const char *name, const char *content) {
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor file = {
        .value = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t)file_mode)};
    if (file.value < 0) {
        const int code = errno;
        printf("create %s: errno %d\n", name, code);
        return false;
    }
    const size_t length = strlen(content);
    return write(file.value, content, length) == (ssize_t)length;
}

static bool plant_symlink(int parent, const char *name, const char *target) {
    if (symlinkat(target, parent, name) != 0) {
        const int code = errno;
        printf("symlinkat %s -> %s: errno %d\n", name, target, code);
        return false;
    }
    return true;
}

/* Removes one planted entry, whatever it is now (a swap may have put a directory where a symlink was). An
 * entry that is already gone is fine. */
static void remove_planted(int parent, const char *name) {
    struct stat info;
    if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return;
    }
    const int flags = S_ISDIR(info.st_mode) != 0 ? AT_REMOVEDIR : 0;
    if (unlinkat(parent, name, flags) != 0) {
        const int code = errno;
        printf("unlinkat %s: errno %d\n", name, code);
    }
}

/* A name of `long_component_length` x's, NUL-terminated. */
static void fill_long_name(char *name) {
    memset(name, 'x', long_component_length);
    name[long_component_length] = 0;
}

static void remove_tree(fixture *tree) {
    if (tree->base < 0) {
        return;
    }
    /* The deep chain, from the bottom: each level is removed through the one above it, reached one openat at a
     * time on a single held descriptor. */
    char long_name[long_component_length + 1];
    fill_long_name(long_name);
    for (int depth = long_component_depth; depth >= 1; --depth) {
        [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor level = {.value = -1};
        int parent = tree->root;
        bool reachable = tree->root >= 0;
        for (int index = 0; reachable && index < depth - 1; ++index) {
            [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor next = {
                .value = openat(parent, long_name, directory_flags)};
            if (next.value < 0) {
                reachable = false;
                break;
            }
            adopt_descriptor(&level, &next);
            parent = level.value;
        }
        if (reachable) {
            remove_planted(parent, long_name);
        }
    }
    static const char *const under_base[] = {"root/a/b/target.txt",
                                             "root/a/b",
                                             "root/a",
                                             "root/file",
                                             "root/link_to_a",
                                             "root/dangling",
                                             "root/locked",
                                             "root",
                                             "outside/target.txt",
                                             "outside",
                                             "staging/b_decoy/target.txt",
                                             "staging/b_decoy",
                                             "staging/b_symlink",
                                             "staging"};
    for (size_t index = 0; index < sizeof under_base / sizeof under_base[0]; ++index) {
        remove_planted(tree->base, under_base[index]);
    }
    if (tree->root >= 0) {
        (void)close(tree->root); /* discard-ok: a read-only descriptor */
    }
    (void)close(tree->base); /* discard-ok: a read-only descriptor */
    tree->root = -1;
    tree->base = -1;
    remove_planted(tree->handed_root, "resolve_test");
}

/* Whether this process can open what its permission bits forbid, as the superuser can: then no permission
 * failure can be provoked and that case skips itself. */
static bool probe_permissions_enforced(int root) {
    const int opened = openat(root, "locked", directory_flags);
    if (opened >= 0) {
        (void)close(opened); /* discard-ok: a read-only descriptor */
        return false;
    }
    return true;
}

static bool plant_tree(fixture *tree, const char *root_text, int handed_root) {
    *tree = (fixture){.root_text = root_text, .handed_root = handed_root, .base = -1, .root = -1};
    /* A previous run that was killed may have left its tree behind: the names are fixed, so it is swept first. */
    tree->base = openat(handed_root, "resolve_test", directory_flags);
    if (tree->base >= 0) {
        tree->root = openat(tree->base, "root", directory_flags);
        remove_tree(tree);
    }
    if (!plant_directory(handed_root, "resolve_test", directory_mode)) {
        return false;
    }
    tree->base = openat(handed_root, "resolve_test", directory_flags);
    if (tree->base < 0) {
        return false;
    }
    /* A chain under && is an int in C. */
    const int planted =
        plant_directory(tree->base, "root", directory_mode) && plant_directory(tree->base, "root/a", directory_mode) &&
        plant_directory(tree->base, "root/a/b", directory_mode) &&
        plant_file(tree->base, "root/a/b/target.txt", "INSIDE\n") && plant_file(tree->base, "root/file", "a file\n") &&
        plant_symlink(tree->base, "root/link_to_a", "a") && plant_symlink(tree->base, "root/dangling", "nowhere") &&
        plant_directory(tree->base, "root/locked", no_access_mode) &&
        plant_directory(tree->base, "outside", directory_mode) &&
        plant_file(tree->base, "outside/target.txt", "OUTSIDE\n") &&
        plant_directory(tree->base, "staging", directory_mode) &&
        plant_symlink(tree->base, "staging/b_symlink", "../../outside") &&
        plant_directory(tree->base, "staging/b_decoy", directory_mode) &&
        plant_file(tree->base, "staging/b_decoy/target.txt", "DECOY\n");
    if (!planted) {
        return false;
    }
    tree->root = openat(tree->base, "root", directory_flags);
    if (tree->root < 0) {
        return false;
    }
    /* The deep chain of long names under root, one level at a time on held descriptors. */
    char long_name[long_component_length + 1];
    fill_long_name(long_name);
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor level = {.value = -1};
    int parent = tree->root;
    for (int depth = 0; depth < long_component_depth; ++depth) {
        if (!plant_directory(parent, long_name, directory_mode)) {
            return false;
        }
        [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor next = {
            .value = openat(parent, long_name, directory_flags)};
        if (next.value < 0) {
            return false;
        }
        adopt_descriptor(&level, &next);
        parent = level.value;
    }
    const int base_written = snprintf(tree->base_text, sizeof tree->base_text, "%s/resolve_test", root_text);
    const int root_written =
        snprintf(tree->root_tree_text, sizeof tree->root_tree_text, "%s/resolve_test/root", root_text);
    if (base_written <= 0 || (size_t)base_written >= sizeof tree->base_text) {
        return false;
    }
    if (root_written <= 0 || (size_t)root_written >= sizeof tree->root_tree_text) {
        return false;
    }
    tree->permissions_enforced = probe_permissions_enforced(tree->root);
    return true;
}

/* ---- The swap: one atomic exchange of two directory entries ----------------------------------------------- */

/* Exchanges `staging/<staged>` with `root/a/b` in one atomic rename, so a walker never sees the entry missing,
 * only replaced: the strongest form of the race. The platforms spell the exchange differently. */
#ifdef __APPLE__
static bool swap_entries(const fixture *tree, const char *staged_relative) {
    return renameatx_np(tree->base, staged_relative, tree->base, "root/a/b", RENAME_SWAP) == 0;
}

static bool platform_can_swap(void) {
    return true;
}
#elifdef __linux__
static bool swap_entries(const fixture *tree, const char *staged_relative) {
    return syscall(SYS_renameat2, tree->base, staged_relative, tree->base, "root/a/b", RENAME_EXCHANGE) == 0;
}

static bool platform_can_swap(void) {
    return true;
}
#else
static bool swap_entries([[maybe_unused]] const fixture *tree, [[maybe_unused]] const char *staged_relative) {
    return false;
}

/* No atomic exchange of two names on this platform: the race cases are reported as not run. */
static bool platform_can_swap(void) {
    return false;
}
#endif

/* ---- Helpers over the layer ------------------------------------------------------------------------------- */

static bc_file_system_engine *create_observed(report *tally) {
    bc_file_system_engine *engine = nullptr;
    bc_file_system_error error;
    expect(tally, bc_file_system_engine_create_observed_for_tests(&engine, &error) == 0,
           "engine_create_observed_for_tests succeeds", "");
    if (engine != nullptr) {
        bc_file_system_engine_before_operation(engine, operation);
    }
    return engine;
}

static bc_file_system_fault fail_step(int32_t step, uint32_t occurrence, int32_t system_code) {
    return (bc_file_system_fault){.operation = operation,
                                  .step = step,
                                  .occurrence = occurrence,
                                  .action = bc_file_system_fault_action_fail,
                                  .system_code = system_code};
}

static void script(report *tally, bc_file_system_engine *engine, bc_file_system_fault fault, const char *what) {
    bc_file_system_error error;
    expect(tally, bc_file_system_observed_fault_add(engine, &fault, &error) == 0, "fault_add succeeds", what);
}

static uint64_t count_of(report *tally, const bc_file_system_engine *engine, int32_t step) {
    uint64_t count = 0;
    bc_file_system_error error;
    expect(tally, bc_file_system_observed_step_count(engine, operation, step, &count, &error) == 0,
           "step_count succeeds", "");
    return count;
}

/* Which fast-path step this platform reports, or 0 where it walks always. */
static int32_t fast_path_step(const bc_file_system_engine *engine) {
    const bc_file_system_platform_fast_paths *fast_paths = bc_file_system_engine_fast_paths(engine);
    if (fast_paths->has_openat2) {
        return bc_file_system_step_openat2;
    }
    if (fast_paths->has_openat_nofollow_any) {
        return bc_file_system_step_openat_nofollow_any;
    }
    return 0;
}

static bc_file_system_location at_text(const char *text) {
    return bc_file_system_location_at((const uint8_t *)text, strlen(text));
}

static bc_file_system_location beneath_text(const bc_file_system_directory *directory, const char *text) {
    return bc_file_system_location_beneath(directory, (const uint8_t *)text, strlen(text));
}

/* `<root tree>/<relative>` as an `at` text, into the caller's buffer. */
static const char *under_root(const fixture *tree, const char *relative, char *buffer, size_t capacity) {
    const int written = snprintf(buffer, capacity, "%s/%s", tree->root_tree_text, relative);
    if (written <= 0 || (size_t)written >= capacity) {
        return "";
    }
    return buffer;
}

/* The identity of `name` under `parent`, as the test sees it, never through the layer. */
static bool identity_of(int parent, const char *name, struct stat *info) {
    return fstatat(parent, name, info, AT_SYMLINK_NOFOLLOW) == 0;
}

static bool descriptor_is(int descriptor, const struct stat *expected) {
    if (descriptor < 0) {
        return false;
    }
    struct stat actual;
    if (fstat(descriptor, &actual) != 0) {
        return false;
    }
    if (actual.st_dev != expected->st_dev) {
        return false;
    }
    return actual.st_ino == expected->st_ino;
}

static bool leaf_is(const bc_file_system_resolved *resolved, const char *expected) {
    if (resolved->leaf_length != strlen(expected)) {
        return false;
    }
    return memcmp(resolved->leaf, expected, resolved->leaf_length) == 0;
}

/* How many components `text` has, by the public path functions: the depth of the handed root is not known
 * to this file, and the walk's step counts depend on it. */
static uint64_t component_count(const char *text) {
    uint64_t count = 0;
    size_t cursor = 0;
    for (;;) {
        size_t offset = 0;
        size_t length = 0;
        int32_t present = 0;
        bc_file_system_error error;
        if (bc_file_system_path_next_component((const uint8_t *)text, strlen(text), &cursor, &offset, &length, &present,
                                               &error) != 0) {
            return count;
        }
        if (present == 0) {
            return count;
        }
        count += 1;
    }
}

/* Opens the root of the planted tree through the public function, as a face would, for the `beneath` cases. */
static bc_file_system_directory *open_root_directory(report *tally, bc_file_system_engine *engine,
                                                     const fixture *tree) {
    bc_file_system_directory *directory = nullptr;
    bc_file_system_error error;
    const bc_file_system_location location = at_text(tree->root_tree_text);
    expect(tally,
           bc_file_system_directory_open(engine, &location, bc_file_system_final_symlink_refuse, &directory, &error) ==
               0,
           "directory_open of the planted root succeeds", "");
    return directory;
}

/* ---- The cases: at ---------------------------------------------------------------------------------------- */

static void test_at_resolves_to_the_parent_and_the_leaf(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    char text[text_capacity];
    const bc_file_system_location location = at_text(under_root(tree, "a/b/target.txt", text, sizeof text));
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0, "at resolves", "");
    struct stat b;
    expect(tally, identity_of(tree->base, "root/a/b", &b), "the test can stat root/a/b", "");
    expect(tally, descriptor_is(resolved.parent, &b), "the parent is root/a/b", "");
    expect(tally, resolved.opened.value == resolved.parent, "the parent was opened by the walk", "");
    expect(tally, leaf_is(&resolved, "target.txt"), "the leaf is target.txt", "");
    /* The root, then every component of the handed root, then resolve_test, root, a and b; never the leaf. */
    expect(tally, count_of(tally, engine, bc_file_system_step_open) == 1, "the root was opened once", "");
    expect(tally, count_of(tally, engine, bc_file_system_step_openat) == component_count(tree->root_text) + 4,
           "every component but the leaf was opened", "");
    expect(tally, count_of(tally, engine, bc_file_system_step_identity_check) == component_count(tree->root_text) + 4,
           "every component was identity-checked", "");
    expect(tally, count_of(tally, engine, bc_file_system_step_openat2) == 0, "an at location never takes openat2", "");
    expect(tally, count_of(tally, engine, bc_file_system_step_openat_nofollow_any) == 0,
           "an at location never takes O_NOFOLLOW_ANY", "");
    bc_file_system_engine_release(engine);
}

static void test_at_follows_a_symlink_on_the_way(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    char text[text_capacity];
    const bc_file_system_location location = at_text(under_root(tree, "link_to_a/b/target.txt", text, sizeof text));
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0,
           "at through a symlink resolves", "");
    struct stat b;
    expect(tally, identity_of(tree->base, "root/a/b", &b), "the test can stat root/a/b", "");
    expect(tally, descriptor_is(resolved.parent, &b), "the parent is the directory behind the link", "");
    bc_file_system_engine_release(engine);
}

static void test_at_root_is_its_own_parent_with_an_empty_leaf(report *tally) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    const bc_file_system_location location = at_text("/");
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0, "the root resolves",
           "");
    expect(tally, resolved.leaf_length == 0, "the root has no leaf", "");
    expect(tally, resolved.parent >= 0, "the root's parent is a descriptor", "");
    /* The root is its own parent: `..` under it is itself. */
    struct stat above;
    expect(tally, fstatat(resolved.parent, "..", &above, 0) == 0, "the test can stat above the root", "");
    expect(tally, descriptor_is(resolved.parent, &above), "the parent of the root is the root", "");
    expect(tally, count_of(tally, engine, bc_file_system_step_openat) == 0, "nothing but the root was opened", "");
    bc_file_system_engine_release(engine);
}

typedef struct refusal_case {
    const char *relative;
    int32_t kind;
    int system_code;
    const char *what;
} refusal_case;

static void expect_at_refusal(report *tally, const fixture *tree, const refusal_case *item) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    char text[text_capacity];
    const bc_file_system_location location = at_text(under_root(tree, item->relative, text, sizeof text));
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == item->kind, item->what,
           item->relative);
    expect(tally, error.kind == item->kind, "the error carries the kind", item->relative);
    expect(tally, error.operation == operation, "the error names the operation", item->relative);
    expect(tally, error.step != 0, "the error names a step", item->relative);
    expect(tally, error.system_code == item->system_code, "the error carries the errno", item->relative);
    expect(tally, error.name_length == 0, "the error names no entry: the location itself failed", item->relative);
    expect(tally, resolved.parent == -1, "nothing was resolved", item->relative);
    bc_file_system_engine_release(engine);
}

static void test_at_refusals(report *tally, const fixture *tree) {
    static const refusal_case cases[] = {
        {"missing/x/leaf", bc_file_system_error_kind_not_found, ENOENT, "a missing component is not_found"},
        {"a/b/missing/leaf", bc_file_system_error_kind_not_found, ENOENT, "a missing deeper component is not_found"},
        {"file/leaf", bc_file_system_error_kind_not_directory, ENOTDIR, "a file on the way is not_directory"},
        {"dangling/leaf", bc_file_system_error_kind_not_found, ENOENT, "a dangling symlink on the way is not_found"},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        expect_at_refusal(tally, tree, &cases[index]);
    }
    if (tree->permissions_enforced) {
        const refusal_case locked = {"locked/leaf", bc_file_system_error_kind_permission_denied, EACCES,
                                     "a directory without permission is permission_denied"};
        expect_at_refusal(tally, tree, &locked);
    } else {
        printf("NOTE: this process opens what its permission bits forbid; the permission case did not run\n");
    }
}

/* ---- The cases: beneath ----------------------------------------------------------------------------------- */

static void test_beneath_resolves_with_the_fast_path_where_there_is_one(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    bc_file_system_directory *root = open_root_directory(tally, engine, tree);
    if (root == nullptr) {
        bc_file_system_engine_release(engine);
        return;
    }
    const uint64_t opens_before = count_of(tally, engine, bc_file_system_step_openat);
    const bc_file_system_location location = beneath_text(root, "a/b/target.txt");
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0, "beneath resolves", "");
    struct stat b;
    expect(tally, identity_of(tree->base, "root/a/b", &b), "the test can stat root/a/b", "");
    expect(tally, descriptor_is(resolved.parent, &b), "the parent is root/a/b", "");
    expect(tally, leaf_is(&resolved, "target.txt"), "the leaf is target.txt", "");
    const uint64_t component_opens = count_of(tally, engine, bc_file_system_step_openat) - opens_before;
    const int32_t fast_step = fast_path_step(engine);
    if (fast_step != 0) {
        expect(tally, count_of(tally, engine, fast_step) == 1, "the fast path opened the parent in one call", "");
        expect(tally, component_opens == 0, "no component was opened one at a time", "");
        expect(tally, count_of(tally, engine, bc_file_system_step_fstat) >= 1, "the fast path was identity-checked",
               "");
    } else {
        expect(tally, component_opens == 2, "the walk opened a and b", "");
    }
    bc_file_system_directory_release(root);
    bc_file_system_engine_release(engine);
}

static void test_beneath_one_component_needs_no_open(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    bc_file_system_directory *root = open_root_directory(tally, engine, tree);
    if (root == nullptr) {
        bc_file_system_engine_release(engine);
        return;
    }
    const uint64_t opens_before = count_of(tally, engine, bc_file_system_step_openat);
    const bc_file_system_location location = beneath_text(root, "file");
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0,
           "a one-component beneath resolves", "");
    expect(tally, resolved.opened.value == -1, "nothing was opened: the parent is the held directory", "");
    struct stat root_identity;
    expect(tally, identity_of(tree->base, "root", &root_identity), "the test can stat root", "");
    expect(tally, descriptor_is(resolved.parent, &root_identity), "the parent is the held directory", "");
    expect(tally, leaf_is(&resolved, "file"), "the leaf is file", "");
    expect(tally, count_of(tally, engine, bc_file_system_step_openat) == opens_before, "no openat was made", "");
    bc_file_system_directory_release(root);
    bc_file_system_engine_release(engine);
}

/* Scripts `system_code` on whichever fast-path step the platform reports; where there is none the walk runs
 * anyway and the script is never reached. Answers the step scripted, or 0. */
static int32_t script_fast_path(report *tally, bc_file_system_engine *engine, int32_t system_code) {
    const int32_t fast_step = fast_path_step(engine);
    if (fast_step != 0) {
        script(tally, engine, fail_step(fast_step, 1, system_code), "the fast path refuses");
    }
    return fast_step;
}

static void expect_fallback_to_the_walk(report *tally, const fixture *tree, int system_code, const char *what) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    bc_file_system_directory *root = open_root_directory(tally, engine, tree);
    if (root == nullptr) {
        bc_file_system_engine_release(engine);
        return;
    }
    const int32_t fast_step = script_fast_path(tally, engine, system_code);
    const uint64_t opens_before = count_of(tally, engine, bc_file_system_step_openat);
    const bc_file_system_location location = beneath_text(root, "a/b/target.txt");
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0, what, "");
    struct stat b;
    expect(tally, identity_of(tree->base, "root/a/b", &b), "the test can stat root/a/b", "");
    expect(tally, descriptor_is(resolved.parent, &b), "the walk reached root/a/b", what);
    expect(tally, count_of(tally, engine, bc_file_system_step_openat) - opens_before == 2, "the walk opened a and b",
           what);
    if (fast_step != 0) {
        expect(tally, count_of(tally, engine, fast_step) == 1, "the fast path was tried once", what);
    }
    bc_file_system_directory_release(root);
    bc_file_system_engine_release(engine);
}

static void test_a_refused_fast_path_falls_back_to_the_walk(report *tally, const fixture *tree) {
    expect_fallback_to_the_walk(tally, tree, ENOSYS, "ENOSYS on the fast path falls back to the walk");
    expect_fallback_to_the_walk(tally, tree, EINVAL, "EINVAL on the fast path falls back to the walk");
    expect_fallback_to_the_walk(tally, tree, EOPNOTSUPP, "EOPNOTSUPP on the fast path falls back to the walk");
    expect_fallback_to_the_walk(tally, tree, ENOTSUP, "ENOTSUP on the fast path falls back to the walk");
}

static void test_any_other_errno_on_the_fast_path_is_the_error(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    bc_file_system_directory *root = open_root_directory(tally, engine, tree);
    if (root == nullptr) {
        bc_file_system_engine_release(engine);
        return;
    }
    const int32_t fast_step = script_fast_path(tally, engine, EIO);
    const bc_file_system_location location = beneath_text(root, "a/b/target.txt");
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    const int32_t kind = bc_file_system_resolve(engine, operation, &location, &resolved, &error);
    if (fast_step == 0) {
        expect(tally, kind == 0, "without a fast path the walk resolves", "");
    } else {
        expect(tally, kind == bc_file_system_error_kind_io_failed, "EIO on the fast path is io_failed", "");
        expect(tally, error.step == fast_step, "at the fast path's step", "");
        expect(tally, error.system_code == EIO, "with its errno", "");
    }
    bc_file_system_directory_release(root);
    bc_file_system_engine_release(engine);
}

static void test_beneath_refuses_a_symlink_on_the_way(report *tally, const fixture *tree) {
    /* Twice: through the fast path where there is one, and through the walk forced by a scripted refusal. */
    for (int forced = 0; forced < 2; ++forced) {
        bc_file_system_engine *engine = create_observed(tally);
        if (engine == nullptr) {
            return;
        }
        bc_file_system_directory *root = open_root_directory(tally, engine, tree);
        if (root == nullptr) {
            bc_file_system_engine_release(engine);
            return;
        }
        if (forced == 1) {
            (void)script_fast_path(tally, engine, ENOSYS);
        }
        const char *what =
            forced == 1 ? "the walk refuses a symlink on the way" : "the fast path refuses a symlink on the way";
        const bc_file_system_location location = beneath_text(root, "link_to_a/b/target.txt");
        [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved =
            bc_file_system_resolved_empty();
        bc_file_system_error error;
        expect(tally,
               bc_file_system_resolve(engine, operation, &location, &resolved, &error) ==
                   bc_file_system_error_kind_symlink_refused,
               what, "");
        expect(tally, error.kind == bc_file_system_error_kind_symlink_refused, "the error is symlink_refused", what);
        expect(tally, error.system_code != 0, "the refusal carries the system's errno", what);
        expect(tally, resolved.parent == -1, "nothing was resolved", what);
        bc_file_system_directory_release(root);
        bc_file_system_engine_release(engine);
    }
}

static void test_beneath_refusals(report *tally, const fixture *tree) {
    static const refusal_case cases[] = {
        {"missing/x/leaf", bc_file_system_error_kind_not_found, ENOENT, "beneath: a missing component is not_found"},
        {"file/leaf", bc_file_system_error_kind_not_directory, ENOTDIR, "beneath: a file on the way is not_directory"},
        {"dangling/leaf", bc_file_system_error_kind_symlink_refused, 0,
         "beneath: a dangling symlink on the way is refused"},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        bc_file_system_engine *engine = create_observed(tally);
        if (engine == nullptr) {
            return;
        }
        bc_file_system_directory *root = open_root_directory(tally, engine, tree);
        if (root == nullptr) {
            bc_file_system_engine_release(engine);
            return;
        }
        const refusal_case *item = &cases[index];
        const bc_file_system_location location = beneath_text(root, item->relative);
        [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved =
            bc_file_system_resolved_empty();
        bc_file_system_error error;
        expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == item->kind, item->what,
               item->relative);
        expect(tally, error.kind == item->kind, "the error carries the kind", item->relative);
        if (item->system_code != 0) {
            expect(tally, error.system_code == item->system_code, "the error carries the errno", item->relative);
        }
        expect(tally, resolved.parent == -1, "nothing was resolved", item->relative);
        bc_file_system_directory_release(root);
        bc_file_system_engine_release(engine);
    }
}

static void test_a_parent_longer_than_path_max_is_walked(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    bc_file_system_directory *root = open_root_directory(tally, engine, tree);
    if (root == nullptr) {
        bc_file_system_engine_release(engine);
        return;
    }
    /* long_component_depth components of NAME_MAX bytes and the leaf: the parent text alone is longer than
     * PATH_MAX on every platform the library ships for (the constant's comment), so no one-call open can take it
     * and the walk must. */
    char text[text_capacity];
    size_t position = 0;
    for (int depth = 0; depth < long_component_depth; ++depth) {
        memset(text + position, 'x', long_component_length);
        position += long_component_length;
        text[position] = '/';
        position += 1;
    }
    memcpy(text + position, "leaf", 5);
    const bc_file_system_location location = beneath_text(root, text);
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0,
           "a parent text longer than PATH_MAX resolves", "");
    expect(tally, leaf_is(&resolved, "leaf"), "the leaf is leaf", "");
    const int32_t fast_step = fast_path_step(engine);
    if (fast_step != 0) {
        expect(tally, count_of(tally, engine, fast_step) == 0, "the fast path was not tried on a text it cannot take",
               "");
    }
    expect(tally, count_of(tally, engine, bc_file_system_step_openat) >= long_component_depth,
           "the walk opened every long component", "");
    bc_file_system_directory_release(root);
    bc_file_system_engine_release(engine);
}

static void test_an_interrupted_component_open_is_asked_again(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    script(tally, engine, fail_step(bc_file_system_step_openat, 1, EINTR), "the first openat is interrupted");
    script(tally, engine, fail_step(bc_file_system_step_fstatat, 2, EINTR), "the second fstatat is interrupted");
    char text[text_capacity];
    const bc_file_system_location location = at_text(under_root(tree, "a/b/target.txt", text, sizeof text));
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally, bc_file_system_resolve(engine, operation, &location, &resolved, &error) == 0,
           "an interrupted walk still resolves", "");
    const uint64_t components = component_count(tree->root_text) + 4;
    expect(tally, count_of(tally, engine, bc_file_system_step_openat) == components + 1,
           "the interrupted openat was asked again", "");
    expect(tally, count_of(tally, engine, bc_file_system_step_fstatat) == components + 1,
           "the interrupted fstatat was asked again", "");
    bc_file_system_engine_release(engine);
}

static void test_a_scripted_identity_check_is_symlink_refused(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    script(tally, engine, fail_step(bc_file_system_step_identity_check, 1, EIO), "the first identity check fails");
    char text[text_capacity];
    const bc_file_system_location location = at_text(under_root(tree, "a/b/target.txt", text, sizeof text));
    [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved = bc_file_system_resolved_empty();
    bc_file_system_error error;
    expect(tally,
           bc_file_system_resolve(engine, operation, &location, &resolved, &error) ==
               bc_file_system_error_kind_symlink_refused,
           "a scripted identity mismatch is symlink_refused", "");
    expect(tally, error.step == bc_file_system_step_identity_check, "at the identity check", "");
    expect(tally, error.system_code == EIO, "with the scripted errno", "");
    bc_file_system_engine_release(engine);
}

/* ---- The race: the S2 shape, deterministic ----------------------------------------------------------------- */

typedef enum swap_kind : int32_t {
    swap_kind_none = 1,
    swap_kind_decoy_directory = 2,
    swap_kind_symlink_to_outside = 3
} swap_kind;

static const char *staged_name_of(swap_kind kind) {
    if (kind == swap_kind_decoy_directory) {
        return "staging/b_decoy";
    }
    return "staging/b_symlink";
}

/* The two halves of the checked open of root/a/b, with the swap landing between them: exactly the window the
 * identity check covers. The spike needed a second thread for this; here the halves are callable one at a
 * time, so the swap runs on this thread and the interleaving is exact without one. The swap is undone after. */
static void expect_race(report *tally, const fixture *tree, swap_kind kind, int32_t expected_kind,
                        int32_t expected_step, const char *what) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor a = {.value = openat(tree->root, "a", directory_flags)};
    expect(tally, a.value >= 0, "the test can open root/a", what);
    if (a.value < 0) {
        bc_file_system_engine_release(engine);
        return;
    }
    bc_file_system_expected_entry expected;
    bc_file_system_error error;
    expect(tally,
           bc_file_system_resolve_expect_entry(engine, operation, a.value, "b", bc_file_system_final_symlink_refuse,
                                               &expected, &error) == 0,
           "the stat half records b", what);
    expect(tally, expected.is_directory, "b was a directory when the stat looked", what);
    bool swapped = false;
    if (kind != swap_kind_none) {
        swapped = swap_entries(tree, staged_name_of(kind));
        expect(tally, swapped, "the swap landed", what);
    }
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor opened = {.value = -1};
    const int32_t kind_answered = bc_file_system_resolve_open_expected_directory(
        engine, operation, a.value, "b", bc_file_system_final_symlink_refuse, bc_file_system_directory_access_search,
        &expected, &opened, &error);
    expect(tally, kind_answered == expected_kind, what, "");
    if (expected_kind == 0) {
        struct stat b;
        expect(tally, identity_of(tree->base, "root/a/b", &b), "the test can stat root/a/b", what);
        expect(tally, descriptor_is(opened.value, &b), "the open half answered b itself", what);
    } else {
        expect(tally, error.kind == expected_kind, "the error carries the kind", what);
        expect(tally, error.step == expected_step, "the error names the step that refused", what);
        expect(tally, opened.value == -1, "nothing was handed out", what);
    }
    if (swapped) {
        expect(tally, swap_entries(tree, staged_name_of(kind)), "the swap was undone", what);
    }
    bc_file_system_engine_release(engine);
}

static void test_the_identity_check_catches_a_swapped_directory(report *tally, const fixture *tree) {
    if (!platform_can_swap()) {
        printf("NOTE: this platform has no atomic exchange of two names; the race cases did not run\n");
        return;
    }
    expect_race(tally, tree, swap_kind_none, 0, 0, "no swap: the open half opens b");
    /* A directory swapped for another directory passes every O_NOFOLLOW variant; only the identities differ. */
    expect_race(tally, tree, swap_kind_decoy_directory, bc_file_system_error_kind_symlink_refused,
                bc_file_system_step_identity_check, "a decoy directory swapped in is caught by the identity check");
    /* A symlink swapped in fails at the open itself (ENOTDIR under O_DIRECTORY | O_NOFOLLOW); the check is never
     * reached. */
    expect_race(tally, tree, swap_kind_symlink_to_outside, bc_file_system_error_kind_symlink_refused,
                bc_file_system_step_openat, "a symlink swapped in is refused at the open");
}

/* The S2 probe-1 shape through the whole layer: the swap lands before the walk reaches b, and the walk (or the
 * fast path) refuses what it finds there. The outside file is never reached. */
static void test_a_symlink_swapped_in_before_the_walk_is_refused(report *tally, const fixture *tree) {
    if (!platform_can_swap()) {
        return;
    }
    expect(tally, swap_entries(tree, "staging/b_symlink"), "the swap landed", "");
    for (int forced = 0; forced < 2; ++forced) {
        bc_file_system_engine *engine = create_observed(tally);
        if (engine == nullptr) {
            break;
        }
        bc_file_system_directory *root = open_root_directory(tally, engine, tree);
        if (root != nullptr) {
            if (forced == 1) {
                (void)script_fast_path(tally, engine, ENOSYS);
            }
            const bc_file_system_location location = beneath_text(root, "a/b/target.txt");
            [[gnu::cleanup(bc_file_system_resolved_close)]] bc_file_system_resolved resolved =
                bc_file_system_resolved_empty();
            bc_file_system_error error;
            expect(tally,
                   bc_file_system_resolve(engine, operation, &location, &resolved, &error) ==
                       bc_file_system_error_kind_symlink_refused,
                   "a symlink swapped in for b is refused", forced == 1 ? "by the walk" : "by the fast path");
            bc_file_system_directory_release(root);
        }
        bc_file_system_engine_release(engine);
    }
    expect(tally, swap_entries(tree, "staging/b_symlink"), "the swap was undone", "");
}

/* ---- The cases: directory_open through the header ----------------------------------------------------------- */

static bool opened_as_is(const bc_file_system_directory *directory, const char *expected) {
    uint8_t buffer[text_capacity];
    size_t length = 0;
    bc_file_system_error error;
    if (bc_file_system_directory_opened_as(directory, buffer, sizeof buffer, &length, &error) != 0) {
        return false;
    }
    if (length != strlen(expected)) {
        return false;
    }
    return memcmp(buffer, expected, length) == 0;
}

static void test_directory_open_remembers_its_text(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    bc_file_system_directory *root = open_root_directory(tally, engine, tree);
    if (root == nullptr) {
        bc_file_system_engine_release(engine);
        return;
    }
    expect(tally, opened_as_is(root, tree->root_tree_text), "an at directory remembers the path's bytes", "");

    bc_file_system_directory *b = nullptr;
    bc_file_system_error error;
    const bc_file_system_location beneath = beneath_text(root, "a/b");
    expect(tally, bc_file_system_directory_open(engine, &beneath, bc_file_system_final_symlink_refuse, &b, &error) == 0,
           "a beneath directory opens", "");
    char expected_text[text_capacity];
    expect(tally, opened_as_is(b, under_root(tree, "a/b", expected_text, sizeof expected_text)),
           "a beneath directory remembers the parent's text, a slash and the relative bytes", "");
    struct stat b_identity;
    expect(tally, identity_of(tree->base, "root/a/b", &b_identity), "the test can stat root/a/b", "");
    expect(tally, descriptor_is(b->descriptor, &b_identity), "the handle holds root/a/b", "");

    bc_file_system_directory *slash = nullptr;
    const bc_file_system_location at_slash = at_text("/");
    expect(tally,
           bc_file_system_directory_open(engine, &at_slash, bc_file_system_final_symlink_refuse, &slash, &error) == 0,
           "the root opens", "");
    expect(tally, opened_as_is(slash, "/"), "the root remembers /", "");
    bc_file_system_directory_release(slash);
    bc_file_system_directory_release(b);
    bc_file_system_directory_release(root);
    bc_file_system_engine_release(engine);
}

static void test_directory_open_leaf_refusals(report *tally, const fixture *tree) {
    static const refusal_case cases[] = {
        {"absent", bc_file_system_error_kind_not_found, ENOENT, "directory_open of nothing is not_found"},
        {"file", bc_file_system_error_kind_not_directory, ENOTDIR, "directory_open of a file is not_directory"},
        {"link_to_a", bc_file_system_error_kind_symlink_refused, ENOTDIR,
         "directory_open of a symlink under refuse is symlink_refused"},
        {"dangling", bc_file_system_error_kind_symlink_refused, ENOTDIR,
         "directory_open of a dangling symlink under refuse is symlink_refused"},
    };
    for (size_t index = 0; index < sizeof cases / sizeof cases[0]; ++index) {
        bc_file_system_engine *engine = create_observed(tally);
        if (engine == nullptr) {
            return;
        }
        const refusal_case *item = &cases[index];
        char text[text_capacity];
        const bc_file_system_location location = at_text(under_root(tree, item->relative, text, sizeof text));
        bc_file_system_directory *directory = nullptr;
        bc_file_system_error error;
        expect(tally,
               bc_file_system_directory_open(engine, &location, bc_file_system_final_symlink_refuse, &directory,
                                             &error) == item->kind,
               item->what, item->relative);
        expect(tally, error.system_code == item->system_code, "the error carries the errno", item->relative);
        expect(tally, directory == nullptr, "no handle was handed out", item->relative);
        bc_file_system_engine_release(engine);
    }
}

static void test_directory_open_follows_a_final_symlink_when_told_to(report *tally, const fixture *tree) {
    bc_file_system_engine *engine = create_observed(tally);
    if (engine == nullptr) {
        return;
    }
    char text[text_capacity];
    const bc_file_system_location location = at_text(under_root(tree, "link_to_a", text, sizeof text));
    bc_file_system_directory *directory = nullptr;
    bc_file_system_error error;
    expect(tally,
           bc_file_system_directory_open(engine, &location, bc_file_system_final_symlink_follow, &directory, &error) ==
               0,
           "directory_open of a symlink under follow opens what it points at", "");
    if (directory != nullptr) {
        struct stat a;
        expect(tally, identity_of(tree->base, "root/a", &a), "the test can stat root/a", "");
        expect(tally, descriptor_is(directory->descriptor, &a), "the handle holds root/a", "");
        expect(tally, opened_as_is(directory, text), "the handle remembers the text it was opened with, the link's",
               "");
        bc_file_system_directory_release(directory);
    }
    bc_file_system_engine_release(engine);
}

/* ---- Entry point -------------------------------------------------------------------------------------------- */

typedef void (*tree_case)(report *tally, const fixture *tree);

static void run_balanced(report *tally, const fixture *tree, tree_case run, const char *name) {
    const int before = count_open_descriptors();
    run(tally, tree);
    const int after = count_open_descriptors();
    expect(tally, before == after, "the case left every descriptor as it found it", name);
}

int main(int argument_count, char **arguments) {
    if (argument_count != 2 || arguments[1][0] != '/') {
        printf("usage: bc_file_system_resolve_test <absolute root directory>\n");
        return 2;
    }
    report tally = {.checked = 0, .failed = 0};
    /* The one open by path in this test: the root it was handed on the command line. */
    [[gnu::cleanup(close_owned_descriptor)]] owned_descriptor handed_root = {.value =
                                                                                 open(arguments[1], directory_flags)};
    if (handed_root.value < 0) {
        const int code = errno;
        printf("open %s: errno %d\n", arguments[1], code);
        return 2;
    }
    fixture tree;
    if (!plant_tree(&tree, arguments[1], handed_root.value)) {
        printf("bc_file_system_resolve_test: the tree could not be planted under %s\n", arguments[1]);
        remove_tree(&tree);
        return 2;
    }

    run_balanced(&tally, &tree, test_at_resolves_to_the_parent_and_the_leaf, "at resolves");
    run_balanced(&tally, &tree, test_at_follows_a_symlink_on_the_way, "at follows a symlink");
    {
        const int before = count_open_descriptors();
        test_at_root_is_its_own_parent_with_an_empty_leaf(&tally);
        expect(&tally, before == count_open_descriptors(), "the case left every descriptor as it found it", "the root");
    }
    run_balanced(&tally, &tree, test_at_refusals, "at refusals");
    run_balanced(&tally, &tree, test_beneath_resolves_with_the_fast_path_where_there_is_one, "beneath resolves");
    run_balanced(&tally, &tree, test_beneath_one_component_needs_no_open, "beneath one component");
    run_balanced(&tally, &tree, test_a_refused_fast_path_falls_back_to_the_walk, "fallback");
    run_balanced(&tally, &tree, test_any_other_errno_on_the_fast_path_is_the_error, "fast path error");
    run_balanced(&tally, &tree, test_beneath_refuses_a_symlink_on_the_way, "beneath symlink");
    run_balanced(&tally, &tree, test_beneath_refusals, "beneath refusals");
    run_balanced(&tally, &tree, test_a_parent_longer_than_path_max_is_walked, "longer than PATH_MAX");
    run_balanced(&tally, &tree, test_an_interrupted_component_open_is_asked_again, "EINTR");
    run_balanced(&tally, &tree, test_a_scripted_identity_check_is_symlink_refused, "scripted identity check");
    run_balanced(&tally, &tree, test_the_identity_check_catches_a_swapped_directory, "the race");
    run_balanced(&tally, &tree, test_a_symlink_swapped_in_before_the_walk_is_refused, "swapped before the walk");
    run_balanced(&tally, &tree, test_directory_open_remembers_its_text, "directory_open text");
    run_balanced(&tally, &tree, test_directory_open_leaf_refusals, "directory_open refusals");
    run_balanced(&tally, &tree, test_directory_open_follows_a_final_symlink_when_told_to, "directory_open follow");

    remove_tree(&tree);
    printf("bc_file_system_resolve_test: %d checks, %d failed\n", tally.checked, tally.failed);
    return tally.failed == 0 ? 0 : 1;
}
