# Agent Instructions

## Mission

Implement the extensible gisland UI platform.

gisland is a C++23 Linux/X11 overlay rendered with raylib. Its long-term product
behavior is defined by external process modules using a versioned declarative
JSONL protocol. The graphical core owns validation, arbitration, layout,
rendering, theming, focus, process supervision, hot reload, and IPC.

## Required Reading

Read these local documents completely before starting a substantial platform
increment, in this order:

1. `docs/superpowers/specs/2026-07-28-raylib-project-setup-design.md`
2. `docs/superpowers/plans/2026-07-28-raylib-project-setup.md`
3. `docs/superpowers/specs/2026-07-28-extensible-ui-platform-design.md`
4. `docs/superpowers/plans/2026-07-28-core-models-and-arbitration.md`

The first two documents explain the existing scaffold and are already
implemented. Do not repeat their tasks. The extensible platform spec is the
authoritative product design. The core models and arbitration plan is the first
actionable implementation plan.

Focused bug fixes and small visual refinements do not require rereading all four
documents when their relevant constraints are already established. Inspect the
affected code, tests, and focused design material instead.

## Execution Order

1. Execute `2026-07-28-core-models-and-arbitration.md` task by task using TDD.
2. Validate each increment using the proportional verification policy below.
3. Create a focused spec and implementation plan for only the next substantial
   delivery increment from the platform design.
4. Continue one independently testable increment at a time.

Do not implement the entire platform in one change. Keep protocol, process
supervision, rendering, X11 behavior, IPC, hot reload, and the shipped module as
separate reviewed increments.

Small related visual refinements may be grouped into one coherent increment.
They do not require an individual spec and plan for every micro-adjustment.

## Engineering Rules

- Use C++23 and modern target-based CMake.
- Pin FetchContent dependencies to immutable release tags.
- Write a failing behavioral test first, observe the intended failure, then add
  the smallest implementation that makes it pass.
- Keep JSON and TOML at explicit boundaries. Internal consumers receive typed,
  validated values only.
- Keep files and classes focused on one responsibility.
- Do not add mutable global state, service locators, speculative compatibility,
  or unrelated abstractions.
- Apply warnings, sanitizers, and clang-tidy only to project-owned targets.
- Treat modules as trusted user processes, but never execute action strings
  emitted by a module inside the core.
- Operational module/configuration errors are logs-only and must not terminate
  the graphical process.
- Do not add remote CI unless the user requests it separately.

## Git Rules

- Never stage, commit, or push `docs/superpowers/` or `.opencode/`.
- Do not modify or remove unrelated user changes.
- Work on a feature branch in an isolated worktree.
- Commit only coherent, verified project changes.
- Never force-push or bypass hooks.

## Verification

Use proportional verification. Do not run the complete quality matrix after
every small iteration.

### Fast Iteration

While developing a focused change:

- Build only affected targets.
- Run the smallest relevant unit or renderer tests.
- For visual work, use an isolated Xvfb test or preview rather than the active
  desktop session.
- Do not reinstall locally until the candidate is ready for user evaluation.

### Coherent Increment

When the user accepts a grouped change or a coherent increment is ready:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

cmake --build --preset dev --target format-check
git diff --check
```

For graphical increments, also run:

```bash
cmake --build --preset graphical
./scripts/run-visual-tests.sh build/graphical
```

Install locally and perform a focused live smoke test only after these checks
pass.

### Full Quality Matrix

Run the complete matrix before a release, an integration milestone, a risky
low-level change, or when the user explicitly requests it:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tidy
cmake --build --preset tidy
ctest --preset tidy

cmake --build --preset dev --target format-check
```

Also compile and test with Clang using a separate build directory during the full
quality matrix. Sanitizers, clang-tidy, and separate Clang builds are not
required for each cosmetic or theme-only iteration.

Do not update YouTrack unless the user explicitly requests it. Always report the
checks actually run, and do not claim completion from code inspection alone.
