/* names.h — the names the engine makes up for entries of its own, in one table.
 *
 * Two operations create entries under names of the engine's choosing and one sweeps them: a staged write or
 * copy stages beside its destination under `.<leaf>.bc-tmp-<process id>-<token>`, `condemn` renames an entry
 * beside itself to `.bc-condemned-<name>` (with `-<token>` appended when that name is taken), and
 * `remove_condemned` clears every entry carrying either mark. The makers and the sweep read the marks from
 * here, so they cannot drift apart, and so does a fuzzer that judges a listing. Private to `core/src` and the
 * fuzzers: C23, never a common subset; unprefixed like every name here without external linkage.
 */
#pragma once

/* The infix of a staged file's name: a leftover carrying it is a killed write's or copy's, and the sweep clears
 * it. The leaf stays in the name so a leftover can be read beside the file it was meant to become. */
static const char staging_infix[] = ".bc-tmp-";

/* The prefix of a condemned entry's name: an entry carrying it is one `condemn` set aside for the sweep. */
static const char condemned_prefix[] = ".bc-condemned-";
