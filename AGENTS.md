# Agent guide -- kickmsg

Project-wide directives for any LLM working in this repository. Read this
before proposing changes. These override generic defaults. kickmsg is a
lock-free, shared-memory pub/sub library for inter-process messaging on
POSIX and Windows; correctness under concurrency, crashes, and corrupt
peer memory is the whole point.

## 1. Don't be lazy

If a fix takes 2--3 lines and visibly improves the code, write the 2--3 lines.
Do not dismiss real concerns with "premature optimization", "good enough",
"out of scope", or "we can revisit later" when the work in front of you is
small and the benefit is concrete. If a reviewer points at a problem, they
already paid the cost of finding it -- apply the fix instead of arguing why it
doesn't matter. When the right fix is genuinely larger than the local change,
do the local fix *and* call out the broader work as a follow-up.

## 2. Keep It Simple, Stupid (KISS)

Pick the simplest design that solves the problem. Avoid helpers called from
exactly one place (inline them), mirrors of state another component owns
(query the authority), dead defensive checks on values a constructor already
guarantees, and speculative abstractions with no concrete consumer. Three
similar lines beat a premature abstraction; factor only when the same shape
repeats 3+ times under the same constraints.

## 3. Maintainability first, performance second

Maintainable code that is also performant beats clever code that is hard to
follow -- but "maintainable first" does not mean "ignore performance". The
publish and receive paths are hot: per-publish heap allocations, avoidable
atomics, false sharing, and syscalls on the fast path are real costs. When the
maintainable design and the performant one diverge by a few lines (a scratch
buffer, a conditional wake), apply both.

Order of operations: (1) correct, (2) clear, (3) fast on hot paths.

## 4. Don't reinvent the wheel

Grep before writing new infrastructure. What already exists:

- Shared memory: `kickmsg/os/SharedMemory.h` (create/open/try_*/unlink).
- Region + channel: `kickmsg/Region.h` (`SharedRegion`, `channel::Config`,
  recovery primitives, `validate_header_geometry`).
- Hashing: `kickmsg/Hash.h` -- `kickmsg::hash::fnv1a_64`. Don't add a new hash.
- Shm naming: `kickmsg/Naming.h` -- `sanitize_shm_component`,
  `compose_shm_name`, `to_hex`. All shm names go through these.
- Lock-free helpers: `kickmsg/types.h` -- `treiber_push`/`treiber_pop`,
  `tagged_pack`/`tagged_idx`, `slot_at`/`sub_ring_at`/`ring_entries`,
  `align_up`, `is_power_of_two`.
- OS: `kickmsg/os/Time.h` (`since_epoch`, `sleep`), `kickmsg/os/Process.h`
  (`current_pid`, `process_starttime`), `kickmsg/os/Futex.h`
  (`futex_wait`/`futex_wake_all`).
- Tests: GoogleTest + gmock unit tests (`tests/unit/`), the stress harness
  (`tests/stress/common.h`), fork+SIGKILL crash tests (`tests/crash_test.cc`).

"I didn't know it existed" is not an answer -- grep, find, or Explore first.

## 5. Coding style

There is no `.clang-format` yet; the rules below describe the established
style. Match the surrounding file.

### 5.1 Formatting

- **Allman braces** -- opening brace on its own line for every block.
- **4-space indent, no tabs.** Case labels indented inside `switch`.
- **East const** -- `T const&`, `T const*`, `Header const*`. Not `const T&`.
- **Pointer alignment left** -- `T* p`, not `T *p`.
- **Member naming** -- `snake_case` with a trailing underscore for non-public
  data members (`shm_`, `name_`, `base_`). Public/struct fields: no underscore.
- **Type naming** -- `PascalCase` for classes, structs, enums, aliases.
- **Namespace** -- single `namespace kickmsg`, contents indented, no closing
  `// namespace` comment.
- **Constructor initializers** -- one per line, `,` leading the next line.

### 5.2 Language rules

- **Header guards, never `#pragma once`.** Format `KICKMSG_<PATH>_H` (e.g.
  `kickmsg/os/SharedMemory.h` -> `KICKMSG_OS_SHARED_MEMORY_H`). Mirror an
  adjacent header.
- **No ternary operator** in code you add or modify. Rewrite as `if`/`else`,
  early return, or an `if`-assigned variable. Leave pre-existing ternaries
  unless explicitly cleaning them up.
- **Prefer `not` / `and` / `or`** over `!` / `&&` / `||` -- the codebase uses
  the keyword operators throughout.
- **`override` mandatory** on overriding virtuals.
- **`inline` only with a real rationale** (one-line accessors, `constexpr`,
  templates, header-only by design) -- not merely to dodge a `.cc` file.

### 5.3 Concurrency and shared memory (kickmsg-specific)

- **Explicit `memory_order` on every atomic op.** No bare `.load()`/`.store()`.
  When an op is `relaxed`, a one-line comment must say why it is safe (which
  other fence or release-store covers it). The release-store of `MAGIC` is the
  sole publication fence for a freshly-stamped region -- do not "fix" a
  deliberate relaxed store to release in isolation.
- **Tolerate corrupt / hostile peer bytes.** The region is mapped by
  independent processes and can be handed to `attach_open` by a caller. Any
  field read from shared memory that drives pointer math (offsets, strides,
  indices, lengths, counts) must be bounds-checked before use -- see
  `validate_header_geometry`. A crashed peer can leave partial state.
- **Preserve the recovery contract.** `repair_locked_entries` and `stats` /
  `diagnose` are safe under live traffic; `reset_retired_rings`,
  `reclaim_orphaned_slots`, `reset_schema_claim`, `sweep_stale` are post-crash
  only. Keep that distinction in the code and the docs; don't add a
  live-traffic caller of a post-crash primitive.
- **ABI is versioned.** The shm layout (`Header`, `Entry`, `SubRingHeader`,
  `SlotHeader`, `SchemaInfo`) is guarded by `MAGIC` + `VERSION`. Any layout
  change requires a `VERSION` bump and updating the `static_assert`s in
  `types.h`.

### 5.4 Comments

Default to writing nothing. Only add a comment when:

- A hidden constraint or invariant is being maintained.
- The code works around a specific bug or platform quirk.
- A reader would otherwise be surprised by the behavior (especially a memory
  ordering or crash-recovery subtlety -- those are worth a tight line).

Never restate what the code does. Never reference the current PR / issue /
"this fixes...". One short line beats a paragraph; reserve multi-line blocks
for an ASCII diagram or numbered list that genuinely helps (e.g. the publish
flow in ARCHITECTURE.md).

### 5.5 Includes

- **Library headers**: IWYU strictly -- every type named in the public API
  comes from an include in that header.
- **`.cc` files**: group standard headers and internal headers, separated by a
  blank line. Match the file's existing grouping.
- **Tests, examples, callers**: rely on transitive includes from the public
  headers you use. Don't re-include `<vector>`, `<cstring>`, `<chrono>`,
  `<cstdint>`, etc. when they arrive via `kickmsg/Publisher.h`,
  `kickmsg/Region.h`, `kickmsg/types.h`, and friends.

### 5.6 Other defaults

- **ASCII only in source** (code, comments, identifiers, docs): `--` for an
  em-dash, `'` for an apostrophe, `...` for an ellipsis, plain ASCII quotes.
  Exception: user-facing strings that genuinely need otherwise. Legacy files
  predating this rule still contain em-dashes; leave them unless you are
  already editing that line, and write new comments ASCII.
- **No emojis** in code, comments, commit messages, or docs unless asked.
- **No new docs files** (`.md`, `README`) unless the user asks.
