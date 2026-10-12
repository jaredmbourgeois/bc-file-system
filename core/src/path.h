/* path.h — the path module's private face: what the engine needs from the lexical rules without restating one.
 *
 * The module is `path.c`, the freestanding tier of the core (design doc, "Two C tiers"): nothing but <stddef.h>
 * and <stdint.h>, no allocation, bytes in and bytes out. Its public functions are declared in the public header
 * under "Path text"; the three below are the engine's, so a location check in `bc_file_system.c` calls the
 * module instead of repeating a rule, and the resolver walks a `beneath` path with the same iterator the
 * public one is built on. Private to `core/src`: C23, never a common subset.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* The longest component the module accepts, in bytes. It is NAME_MAX on every platform the library ships for,
 * which the hosted engine pins with a static_assert beside its <limits.h>; the module names no system header,
 * so the number lives here. */
enum : uint32_t { bc_file_system_path_component_capacity = 255 };

/* The rules of a relative path, as the public header states them: at least one component, no leading slash, no
 * NUL, no empty, `.` or `..` component, no component over the capacity. A NULL `path` is refused whatever its
 * length: with 0 it is the empty text, which no rule accepts. */
bool bc_file_system_path_text_is_valid_relative(const uint8_t *path, size_t length);

/* The rules of an absolute path: a leading slash, `/` alone the root, otherwise the component rules above with
 * `..` allowed. `path` as above. */
bool bc_file_system_path_text_is_valid_absolute(const uint8_t *path, size_t length);

/* One component of a valid path per call: `*cursor` starts at 0 and advances; returns false at the end, with
 * `*offset` and `*component_length` untouched. The path must have passed one of the two predicates above. */
bool bc_file_system_path_text_next_component(const uint8_t *path, size_t length, size_t *cursor, size_t *offset,
                                             size_t *component_length);
