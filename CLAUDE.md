NO FILES OUTSIDE THE PROJECT ROOT MAY BE TOUCHED WITHOUT EXPLICIT USER PERMISSION. NO PERMANENT CHANGES TO THE HOST ENVIRONMENT ARE EVER ALLOWED.
NEVER DO MORE WORK THAN IS EXPLICITLY REQUESTED WITHOUT CONFIRMATION.
AGENTS MUST ONLY USE GIT TO EXAMINE THE REPOSITORY, NOT TO MUTATE REPOSITORY STATE, unless explicitly requested by the user. Absent such a request: no commits, no branches, no stashes, no resets—only read-only operations (status, diff, log, show, blame). DO NOT ADD OR COMMIT CODE TO GIT unless the user explicitly asks.

# Repository Guidelines

## Envy Overview
Envy is a freeform package manager orchestrated via Lua scripts—"freeform" means zero opinion on what packages represent. Authors compose packages via verbs: `fetch`, `cache`, `deploy`, `asset`, `check`, `update`. Build flows from these verbs; avoid bespoke tooling. User-wide cache serves all projects, optimized for large payloads (arm-gcc, llvm-clang, J-Link) and machine-global installs (Python, Homebrew). Deeply parallelized—preserve concurrency in all automation.

## Environment Constraints
Operate strictly within repository—no modifications outside project tree. Never alter host environment (system config, package installs, symlinks). Never modify third-party code without explicit user approval.

## Documentation Style
- All documentation should be terse and pithy. Maximize information density; minimize word count. Examples should demonstrate multiple concepts simultaneously.
- Future enhancement documents (e.g., `docs/future-enhancements.md`) should be especially concise: ~2 terse sentences and one pithy example per bullet.
- Avoid verbose explanations of trade-offs, benefits, or implementation details unless they're critical to understanding.
- Prefer short, declarative sentences. Use em-dashes and semicolons to compress related ideas. Strip filler words.

## Project Structure & Module Organization
`CMakeLists.txt` configures C++20 driver; all dependencies via `cmake/Dependencies.cmake`—no ad-hoc `FetchContent`. No Git submodules—fetch via CMake for lightweight repo. Third-party setup in `cmake/deps/<Lib>.cmake`; shared helpers in `cmake/EnvyFetchContent.cmake` or `cmake/DependencyPatches.cmake`. Patches use `configure_file()` materializing scripts in binary tree via `cmake/templates/` (idempotent reconfigures). Runtime sources live in `src/`; there is no standalone public include tree. Unit tests side-by-side with sources (`src/*_tests.cpp`); functional tests under `tests/` (create when needed). Test data files (scripts, fixtures) in `test_data/` organized by subsystem (e.g., `test_data/lua/`). Design notes in `docs/`; update `docs/dependencies.md` when pinning/patching.

## Build, Test, and Development Commands
**IMPORTANT: All builds use `./build.sh` (Unix) or `build.bat` (Windows). Do not invoke `cmake`, `ninja`, or other build tools directly without explicit user approval.** Build scripts handle configuration, incremental builds, unit test execution automatically. **Functional tests run separately via `python3.13 -m functional_tests`**—build scripts only run unit tests. Tasks incomplete until both `./build.sh` and functional tests succeed—no warnings/errors.

Configuration fixed: only `CMAKE_BUILD_TYPE` and `ENABLE_LTO` vary—no additional options. Third-party tests/examples always disabled. Cache third-party in `out/cache`—check before fetch. All artifacts under `out/build` (e.g., `out/build/envy`). Delete `out/build` to force rebuild; delete `out/` for pristine tree. Build exercises libgit2, libcurl, libssh2, mbedTLS, Lua, libarchive, BLAKE3. Third-party configures during top-level configure, builds during top-level build—never at configure-time. macOS: `otool -L out/build/envy` must show only system libs (`libz`, `libiconv`, `libSystem`, `libresolv`, `libc++`)—third-party as dynamic deps means misconfiguration.

## Coding Style & Naming Conventions
Formatting: respect `.clang-format` (Google base)—2-space indent, K&R braces, 91-col ceiling, pointers right-aligned, short constructs allowed single-line.
Naming: functions/types snake_case, constants kPascalCase, enum values SCREAMING_SNAKE (all uppercase), everything in `envy` (no sub-namespaces).
Initialization & includes: brace-init new vars; use `=` only for reassignment; `<>` for STL/OS headers, `""` for envy/third-party; order local → third-party → STL with blank lines.
Structure: favor value types over heap; keep headers self-contained; declare inline members in-class, define out-of-line right below; avoid ad-hoc FetchContent.
Atomics: default to `memory_order_seq_cst`; only tighten semantics when correctness demands it.
Formatting: prefer stdio-style (`snprintf`, `fprintf`) over iostream for output.
Control flow: consolidate cleanup paths—extract once, clean once. Ternaries over branched returns when cleanup is identical. Visit variants with `std::visit` + `envy::match`, one lambda per alternative—a new alternative then fails to compile. Don't enumerate variant alternatives with an `if constexpr` cascade; its trailing return swallows the new one silently. (Cascades over open type sets with a meaningful catch-all are fine—see `sol_util.h` `type_name_for_error`.) `switch` on `index()` only when you need the index; guard with `static_assert`. It's ~55B/site cheaper than `match`—not a reason to prefer it. Golf judiciously: compress when clarity improves, expand when debugging suffers.
Encapsulation: **NEVER use the `friend` keyword without explicit user permission.** Design proper public interfaces instead—move implementation into methods, use accessor patterns, or restructure classes to avoid circumventing access control.

## Testing Guidelines & Performance Philosophy
Focused tests via lightweight harnesses outside CTest—CMake builds only. Tests fast (<1s), clean allocations (`git_libgit2_shutdown`, `curl_easy_cleanup`, `archive_write_free`). **Unit tests must not touch filesystem** (no temp files, no writes)—use in-repo `test_data/` for fixtures; only Python functional tests may create/modify files (with proper cleanup). **Every new command must add CLI parsing tests to `src/cli_tests.cpp`**—verify arguments parse correctly, required options enforced, config variant holds correct type. Diagnose via `out/build/envy`; document repro in PR/docs. Optimize for small binaries, high runtime performance—simple cache-friendly structures, thread-aware algorithms, no excessive template metaprogramming. Use bundled mbedTLS for hashing (e.g., SHA-256) over bespoke code.

**Logging vs tracing** (two disjoint systems): **Logs** (`tui::debug/info/warn/error`) are human per-package narrative—what envy did to each package and why. INFO (default) shows one outcome per package; DEBUG (`--verbose`) shows the decision narrative; `-q/--quiet` shows warnings/errors only. Package-worker log lines auto-prefix `[identity]` via `tui::log_ctx_scope`—never hand-roll it. **Traces** (`ENVY_TRACE(event, spec, ...)`) are structured machinery events (scheduler, cache/lock, IO) for functional tests and production diagnostics—never prose, never `fprintf`. Trace output is JSONL via `--trace=file:<path>` (or `--trace=stderr`); it is orthogonal to log verbosity. Add event types to `src/trace_events.def` (single source of truth: structs, serializers, and the parser registry all derive from it); update the count sentinel in `trace_tests.cpp` and the mirror in `functional_tests/trace_parser.py`.

## Commit & Pull Request Guidelines
Conventional Commit headers (`feat:`, `fix:`, `build:`, `docs:`). Squash fixups locally—one commit per logical change. PRs describe dependency revisions, build command, runtime output (or rationale). Update `docs/dependencies.md` for cross-library changes; reference in PR. No backward compatibility—prioritize current needs, simplify aggressively.
