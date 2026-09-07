# Envy Architecture

## Project Manifests

All script-global variables are uppercase: manifests export `PACKAGES`, plus optional `BUNDLES`, `DEFAULT_SHELL` and `PACKAGE_DEPOTS`; specs declare `IDENTITY`, `FETCH`, `STAGE`, `BUILD`, `INSTALL`, `SETUP`, `DEPENDENCIES`, `BUNDLES`, `PRODUCTS`, `OPTIONS`, `PLATFORMS`, `USER_MANAGED` and `EXPORTABLE`.

**Syntax:** every `PACKAGES` entry is a table—there is no bare-string shorthand—naming a `spec` plus exactly one of `source` or `bundle`, and any of `sha256`, `ref`, `options`, `platforms`, `needed_by`, `product`, `setup`. Every other key is an error: a key envy does not read is a key that silently does nothing, and a misspelled `sha256` must not quietly disable verification.

**Platform-specific packages:** Manifests are Lua scripts—use conditionals and `envy.extend()` to combine common and OS-specific package lists.

**Composition:** `envy.import(path)` runs a subproject's manifest in a sandbox and returns its globals; its entries keep anchoring relative paths and bundle aliases on the imported manifest's directory, and it sees `ENVY_IMPORTER` so a standalone-only branch can gate on it. Imported headers are inert—see the bootstrap boundary in `docs/commands.md`.

```lua
-- project/envy.lua
local common = {
    {  -- Declarative remote with verification and options
        spec = "arm.gcc@v2",
        source = "https://github.com/arm/specs/gcc-v2.lua",
        sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        options = { version = "13.2.0", target = "arm-none-eabi" },
    },
    {  -- Git repository
        spec = "vendor.openocd@v3",
        source = "git://github.com/vendor/openocd-specs.git",
        ref = "a1b2c3d4e5f6...",  -- Commit SHA
        options = { target = "arm" },
    },
    {  -- A spec whose SETUP pair this host opts into; a custom fetch is declared by
       -- the spec that needs it, never here: a manifest entry has no Lua state anything
       -- could find the closure in again, so `source = { fetch = ... }` is rejected.
        spec = "corporate.toolchain@v1",
        source = "https://corp.example/specs/toolchain.lua",
        sha256 = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
        setup = { "license" },
    },
    {  -- Project-local (development): read in place, never cached
        spec = "local.wrapper@v1",
        source = "./envy/specs/wrapper.lua",
        options = { base = "arm.gcc@v2" },
    },
}

local darwin_packages = {
    { spec = "envy.homebrew@v4", source = "specs/homebrew.lua" },
}

local linux_packages = {
    { spec = "system.apt@v1", source = "specs/apt.lua", platforms = { "linux" } },
}

PACKAGES = envy.PLATFORM == "darwin" and envy.extend(common, darwin_packages)
        or envy.PLATFORM == "linux" and envy.extend(common, linux_packages)
        or common
```

**Field semantics:**
- `spec` — the identity this entry declares; the spec file's own `IDENTITY` must match it
- `source` — URL (http/https/s3/ftp/git/file) or a path relative to the declaring file; a
  local path is loaded in place and never enters the cache
- `ref` — git commit SHA or committish (required for git sources)
- `sha256` — expected hash (**optional**, permissive by default; a future strict mode will
  require it for non-`local.*`). On a local path it makes the entry verified *and* cached
- `options` — spec configuration, passed to every phase function and hashed into the
  package key; two option sets of one identity are two packages
- `platforms` — the hosts this entry applies to (`"linux"`, `"darwin-arm64"`); a
  non-matching entry is dropped before anything is interned
- `setup` — names of the spec's `SETUP` pairs to run on this host (never hashed)
- `bundle` — a `BUNDLES` alias or an inline declaration; excludes `source`
- `product` — treat this entry as the provider of a named product (`docs/products.md`)
- `needed_by` — the phase of the *dependent* an edge blocks; meaningful on a spec's
  `DEPENDENCIES` entries, where the dependent exists

Dependencies are declared by specs, in `DEPENDENCIES`; a manifest entry names direct
needs only:
- **Strong:** `spec` plus a `source` (or `bundle`) — interned and started immediately
- **Weak:** a query plus a `weak = { ... }` fallback, spawned only if nothing matches
- **Reference-only:** a query with neither `source` nor `weak`; some other package must
  provide it, or resolution fails after convergence
- **Fetch prerequisites:** `source.dependencies` of a custom fetch. Strong only, and
  always needed by the declarer's `spec_fetch`, so they may not carry `needed_by`

**One package per (identity, options):** the key holds no source, so every declaration of
one key must agree on what to fetch. A provable disagreement—different URL, `sha256`,
path, `ref`, or bundle—is an error naming both declaring files. Two custom fetch closures
are undecidable: the first wins and warns, unless both are copies of one declaration (an
alias named twice), which is silent. Different options are different packages, always.

### Package Depots

`PACKAGE_DEPOTS` lists sources of prebuilt `.tar.zst` artifacts consulted during the import phase; hits skip fetch/build. Entries are URI strings (Lua-computable) or `{ DEPENDS, FETCH }` tables whose `FETCH(ctx)` returns depot manifest text, a path to it, or an entries table (`{ url=, sha256= }`); `DEPENDS` names manifest packages needed to run `FETCH` (Artifactory CLI, internal tooling).

```lua
PACKAGE_DEPOTS = {
  "https://cdn.example.com/depot-" .. envy.PLATFORM_ARCH .. ".txt",
  {
    DEPENDS = { "tools.jfrog@v2" },
    FETCH = function(ctx)
      local out = ctx.tmp_dir .. "/depot.txt"
      envy.run(ctx.deps["tools.jfrog@v2"].pkg_path .. "/bin/jf rt dl depots/prod.txt " .. out)
      return out
    end,
  },
}
```

Semantics: all depot manifests merge into one flat index before any import proceeds (order irrelevant—cache keys are hash-unique; duplicate keys keep the first, differing SHA256 warns). Fetching is lazy: a single-step `#depot` engine task starts on first import that needs it; `DEPENDS` closures are flagged depot-bootstrap—they always source-build (breaks circularity) and must use strong dependencies only. URI download failures warn and degrade to source builds; a failed `DEPENDS` build or throwing `FETCH` is fatal. `--ignore-depot`/`ENVY_IGNORE_DEPOT` skips the task entirely (no deps spawn). Depot config is never hashed.

## Shell Configuration

Manifests can specify a `DEFAULT_SHELL` global to control how `envy.run()` executes scripts across all specs. This enables portable build scripts in custom languages without requiring pre-installed interpreters.

**Built-in shells (constants):**
- `ENVY_SHELL.BASH` — POSIX bash (default on macOS/Linux)
- `ENVY_SHELL.SH` — POSIX sh
- `ENVY_SHELL.CMD` — Windows cmd.exe
- `ENVY_SHELL.POWERSHELL` — Windows PowerShell (default on Windows)

**Custom shells (table):**
- **File mode:** `{file = "/path/to/interpreter", ext = ".ext"}` or `{file = {"/path/to/exe", "--arg"}, ext = ".tcl"}`
  - Script written to temp file with extension, path passed as final argument
  - Shorthand: `file = "/path"` expands to `file = {"/path"}`
- **Inline mode:** `{inline = {"/path/to/exe", "-c"}}`
  - Script content passed as final argument (no temp file)

**Dynamic shell selection (function):** takes no arguments; returns a constant or custom-shell table. Wrap it in `{DEPENDS, SHELL}`—mirroring `PACKAGE_DEPOTS`—to name an envy-managed interpreter. `DEPENDS` lists identities from `PACKAGES`, installed before the first string verb that needs the shell; `SHELL` must then be a function, since only a function is evaluated late enough to name a package.

```lua
DEFAULT_SHELL = {
  DEPENDS = { "py.python@v3.11" },
  SHELL = function() return {inline = {envy.product("python3"), "-c"}} end,
}
```

**Use case:** Express all build scripts in a custom language (Python, Tcl, Ruby) without assuming it's pre-installed.

**Semantics:**
- Value forms (constant, custom table) resolve at manifest load; a function is evaluated once, by a single-step `#default_shell` engine task mirroring `#depot`—it starts on the first request that needs it, its edges hold it until `DEPENDS` is installed, and it publishes the result for the run. Evaluating at load is impossible—no package exists yet to own the interpreter dependency.
- **Bootstrap-shell rule:** the platform built-in is used wherever the manifest shell cannot exist yet—no package context at all, a package in *any* bootstrap closure (`DEFAULT_SHELL` `DEPENDS`, `PACKAGE_DEPOTS` `DEPENDS`, `source.dependencies`), and any Lua running in the manifest state (a manifest bundle's `source.fetch`, a depot `FETCH`, the `SHELL` function itself). One rule, not three carve-outs: bootstrap work runs before the shell exists, and evaluating the shell needs the manifest's single non-recursive Lua lock, so it can never nest inside itself. Bootstrap specs need no annotation—membership is enough.
- Lazy: `DEPENDS` starts on the first request for a shell, so a run that executes no string verb never fetches the interpreter—`envy deploy` and a bare `envy product` listing resolve only, and ask for one just if a custom fetch runs a string verb. `DEPENDS` is interned during resolution regardless, so a bad identity still fails early.
- `DEPENDS` is a strong-only closure, like `source.dependencies`; a weak reference in it is an error. `DEPENDS` without a function `SHELL` is rejected.
- Root manifest only: an imported manifest that declares `DEFAULT_SHELL` (or `PACKAGE_DEPOTS`) is an error unless the root adopts the value—see the bootstrap boundary in `docs/commands.md`.
- The function authorizes against a synthetic consumer, `envy.DEFAULT_SHELL@v1`, holding one edge per `DEPENDS` entry—wired through the same `wire_dependency` path as every other edge, so it appears in the access traces and in errors.
- Traced as `default_shell_resolving{depends}` then `default_shell_resolved{shell}`; a package's block on the depot is `depot_wait{duration_ms, result}`.

## Specs

### Organization

**Identity:** Specs are namespaced with version: `arm.gcc@v2`, `gnu.binutils@v3`. The `@` symbol denotes **spec version**, not asset version. Asset versions come from `options` in manifest. Multiple spec versions coexist; `local.*` namespace reserved for project-local specs.

**Sources:**
- **Declarative:** `source` field with a URL (http/https/s3/ftp) or git repo; verified via `sha256` (URL) or `ref` (git); cached
- **Custom fetch:** `source = { fetch = ..., dependencies = ... }` on the entry that depends on the spec, with verification enforced at the API boundary (`envy.fetch`, `envy.commit_fetch`); cached, keyed on declaring file + identity + options
- **Project-local:** a `source` path in the project tree; loaded in place and never cached. Any namespace may do this; what `local.*` controls is who may depend on it (see **Security** below)
- **Fetch prerequisites (nested):** `source.dependencies` declares specs that must reach completion before this spec's fetch runs. Strong only — weak resolution happens at a barrier that waits for every spec_fetch, including that of the consumer whose fetch function is waiting, so nothing weak can be ordered in time.

**Formats:**
- **Single-file:** `.lua` file (declarative sources only)
- **Multi-file:** directory with a `spec.lua` entry point (custom fetch, archives, git repos). A custom fetch commits whatever it likes; the whole committed tree becomes the spec directory, so siblings are reachable via `envy.loadenv` and `require`

**Integrity:** Two orthogonal checks:

1. **Identity validation** (ALL specs, always required):
   - Spec must declare `IDENTITY = "..."` matching referrer's expectation
   - Catches typos, stale references, copy-paste errors
   - No namespace exemptions

2. **SHA256 verification** (optional, namespace-specific):
  - Declarative sources accept SHA256 (URL) or commit SHA (git)
  - Custom fetch accepts SHA256 per-file via `envy.fetch()` API
  - If SHA256 provided, verification happens at fetch time; mismatch causes hard failure
  - Never re-verified from cache
  - **Permissive by default**: SHA256 optional for all specs
  - **Future strict mode**: will require SHA256 for all non-`local.*` specs

### Dependency Semantics (strong, weak, reference-only)

- **Strong dependencies** provide a complete spec (manifest or explicit `source`). They are instantiated immediately and run toward their target phase.
- **Weak dependencies** specify a query (`spec = "name"` or partial identity) plus a fallback spec in `weak = { ... }`. The engine tries to satisfy the query from packages already in the graph; if no match, it spawns the fallback. Ambiguity is an error listing every candidate, sorted. Which package a weak reference resolved to is hashed into the consumer's cache key—different provider, different package.
- **Reference-only dependencies** provide only a query (no `source`/`weak`). They must be satisfied by some other spec in the graph; otherwise resolution fails after convergence.
- **Nested fetch prerequisites** live in `source.dependencies` and follow the same rules, except that they must be strong. They must complete before the parent's `spec_fetch` runs — and because the whole closure therefore runs during resolution, nothing in it may hold a weak reference either.
- Resolution is iterative: the engine waits for every in-flight `spec_fetch`, runs one weak-resolution pass (matching or spawning fallbacks), and repeats while progress is made. Progress accounts for newly spawned fallbacks even when unresolved counts stay flat.

### Verbs

Specs define verbs describing how to acquire, validate, and install packages:

- **`fetch`** — Acquire source materials. Can be:
  - String: `FETCH = "https://..."` (no verification)
  - Single file: `FETCH = {source="...", sha256="..."}` (optional verification)
  - Multiple files: `FETCH = {{source="..."}, {source="..."}}` (concurrent, optional verification per-file)
  - Custom function: `FETCH = function(tmp_dir, options) envy.fetch(...) end` (imperative with `envy.fetch()` API)
  - Function returning declarative: `FETCH = function(tmp_dir, options) return "https://..." end` (enables templating with options; return value can be any declarative form: string, table, array; can mix with imperative `envy.fetch()` calls)
  - **Transport retry:** http, ftp and git retry transient transport failures 3 times with jittered exponential backoff (~1s, ~4s, ±50%); the jitter keeps a batch of threads off a single bad mirror. Retryable: connect/DNS failure, mid-body read failure, stall timeout, 5xx, 429. Not retryable: 4xx except 429, sha256 mismatch, callback abort. Safe because fetches are idempotent GETs and payloads are verified after transport. `ENVY_FETCH_ATTEMPTS` (default 3, 1 disables) and `ENVY_FETCH_RETRY_BASE_MS` (default 1000) tune it; s3 is excluded because the AWS SDK retries internally. Transports are bounded on both platforms—curl by `LOW_SPEED_LIMIT`/`LOW_SPEED_TIME`, WinINet by connect/send/receive timeouts—so a mirror that accepts and then stops sending fails in ~60s instead of hanging.
- **`stage`** — Prepare staging area from fetched content. Default extracts archives; custom functions can manipulate source tree. Declarative form takes `strip` and `only`—`STAGE = {strip = 1, only = {"bin/clang-*", "lib/**/include/*.h"}}` extracts only matching paths (a directory takes its subtree), matched post-`strip`; the rest of the archive is never decompressed. Globs: `*`/`?` within a component, `**` across, `[a-z]` classes. An `only` entry nothing in the fetch dir provides is a hard error.
- **`build`** — Compile or process staged content. Specs access staging directory, dependency artifacts, and install directory.
- **`install`** — Write final artifacts into the entry's `pkg/` directory. On a clean return the lock destructor drops the ephemeral dirs and touches `envy-complete`, which is what makes the entry a cache hit ever after.
- **`setup`** — Named host-side CHECK/INSTALL pairs (`SETUP = { name = { CHECK, INSTALL, PLATFORMS?, DEPENDS? } }`). Run after install, check-gated every invocation, never cached or hashed. Explicit-only selection; selected pairs run as parallel tasks. See below.

### SETUP Pairs: Host State Beside (or Instead of) the Cache

Payload bytes live in the cache and are keyed by `(identity, options, platform)`. Host state (udev rules, system package managers, credentials) is per-machine, per-intent, and idempotent-checkable — it must never influence the package hash. `SETUP` pairs express it:

- Pair verbs: `CHECK(pkg_dir, options) -> bool|string` (string runs as shell; true/exit 0 = satisfied) and `INSTALL(pkg_dir, options) -> nil|string` (string runs as shell). `pkg_dir` is the installed payload path for cache-managed packages, `nil` for user-managed. cwd = `project_root`. String scripts (and strings returned by function verbs) execute outside the package's Lua lock, so pairs of one package parallelize on shell time; function bodies themselves serialize on the shared Lua state.
- Selection is **explicit-only**: `setup = { "name", ... }` on a manifest package entry or a dependency entry (spec authors may demand host state from their dependencies; weak/reference and pure-bundle deps may not). Effective set = union across all referrers, closed transitively over `DEPENDS`. No selection = nothing runs, any package type; `setup = {}` ≡ absent. Unknown explicit names are hard errors.
- Per-pair `DEPENDS = { "sibling", ... }` sequences pairs within one spec (validated at parse: unknown targets, cycles). Selecting a pair auto-selects its `DEPENDS` closure. A `PLATFORMS`-filtered `DEPENDS` target skips silently but still satisfies dependents.
- Per-pair `PLATFORMS` filters against the host (mismatches skip silently). Pair names are `[A-Za-z0-9_.-]+`.
- Selection is **never** part of `format_key()`/BLAKE3 — one depot artifact serves every selection. Different projects sharing a user-wide cache get their own selections honored on every run because pairs are CHECK-gated, not marker-gated.
- Execution: each selected pair becomes a first-class single-step `task_engine` task (keyed `<canonical>#setup:<name>`) spawned by the parent's `setup` phase (after `install`, before `export`). Unrelated pairs run in parallel on their own worker threads; `DEPENDS` become ordinary task edges. The parent waits for all its pair tasks and aggregates failures; a failing pair blocks its dependent pairs, unrelated pairs complete. Dependents of the package wait for its setup phase, so host state is ready before they proceed.

**Double-check lock per pair:** pre-lock CHECK (skip if satisfied) → acquire ephemeral cache entry lock keyed `BLAKE3(format_key() + "|setup:" + name)`, marked user-managed → re-CHECK (skip if another process finished) → INSTALL → destructor purges entry. Concurrent envy processes run each pair's INSTALL at most once.

### User-Managed vs Cache-Managed Packages

Specs declare their mode via top-level `USER_MANAGED` (boolean or function-returning-boolean; defaults to `false`). The value is resolved once at spec load and determines `p->type` for the rest of the pipeline.

**Cache-Managed Packages** (`USER_MANAGED = false` or absent):
- Artifacts stored in cache—hash-based lookup via `cache::ensure_pkg()`
- Install writes into the entry's `pkg/` directory (the `install_dir` phase argument)
- Lock destructor drops `work/` (and `fetch/` unless preserved), then touches `envy-complete`
- Subsequent runs: cache hit skips payload phases; selected SETUP pairs still evaluate
- Full pipeline: FETCH → STAGE → BUILD → INSTALL (+ optional SETUP pairs)
- Example: toolchains, libraries, build tools

**User-Managed Packages** (`USER_MANAGED = true`):
- The package **is** its SETUP pairs — host state only, no payload, no persistent cache entry
- Must define at least one SETUP pair; must NOT define FETCH/STAGE/BUILD/INSTALL
- Selection is explicit like everything else — an unselected user-managed package participates in the graph (loadenv/products) but mutates nothing
- Example: brew/apt wrappers, environment setup, credential files

Top-level `CHECK` is invalid everywhere — CHECK/INSTALL pairs live only inside `SETUP`.

**Implementation mechanics:**
- Resolution: `resolve_user_managed()` reads `USER_MANAGED` once during `phase_spec_fetch`; sets `p->type`. Function form is called with no args and must return a boolean.
- `phase_setup.cpp` computes the selection closure and calls `engine::run_setup_pairs_for()`, which spawns one single-step task per pair and waits for all of them — pairs never masquerade as packages. `phase_check.cpp` does hash lookup for cache-managed only (user-managed acquires no package lock, so payload phases no-op).
- A selection merging in after a package's setup phase snapshots it (only reachable via exotic fetch-dependency ordering) is a hard error, not a silent drop.
- Lock destructor: completed → mark `envy-complete`; user-managed → purge the whole entry; otherwise → drop the partial install. Pair locks always take the user-managed branch, so host state leaves no entry behind.
- Validation: `phase_spec_fetch.cpp::validate_phases()` + `parse_setup_table()` enforce the rules above at spec load.

**Example: System Package Wrapper**
```lua
-- python.interpreter@v3 (user-managed)
IDENTITY = "python.interpreter@v3"
USER_MANAGED = true

SETUP = {
  python = {
    CHECK = function(pkg_dir, options)
      local result = envy.run("python3 --version", {quiet = true, check = false})
      return result.exit_code == 0
    end,
    INSTALL = function(pkg_dir, options)
      if envy.PLATFORM == "darwin" then
        envy.run("brew install python3")
      elseif envy.PLATFORM == "linux" then
        envy.run("sudo apt-get install -y python3")
      end
    end,
  },
}
```

**Example: Cache-Managed Toolchain with Optional Host Setup**
```lua
-- segger.jlink@r0 (cache-managed payload + opt-in host mutation)
IDENTITY = "segger.jlink@r0"

FETCH = {source = "https://segger.com/JLink.tgz", sha256 = "abc..."}

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.extract(fetch_dir .. "JLink.tgz", install_dir, { strip = 1 })
end

SETUP = {
  udev_rules = {
    PLATFORMS = { "linux" },
    DEPENDS = { "plugdev_group" },  -- selecting udev_rules pulls this in, runs it first
    CHECK = function(pkg_dir, options)
      local r = envy.run("cmp -s " .. pkg_dir .. "99-jlink.rules /etc/udev/rules.d/99-jlink.rules",
                         { quiet = true, check = false })
      return r.exit_code == 0
    end,
    INSTALL = function(pkg_dir, options)
      envy.run({
        "sudo cp " .. pkg_dir .. "99-jlink.rules /etc/udev/rules.d/",
        "sudo udevadm control -R",
      }, { interactive = true })
    end,
  },
  plugdev_group = {
    PLATFORMS = { "linux" },
    CHECK = "groups | grep -q plugdev",
    INSTALL = "sudo usermod -aG plugdev $USER",
  },
}

-- Manifest (bench machines opt in; CI never selects the pair). Selecting
-- udev_rules transitively selects plugdev_group and runs it first:
-- { spec = "segger.jlink@r0", source = "...", options = { version = "9.30" },
--   setup = not os.getenv("CI") and { "udev_rules" } or nil }
-- Spec authors can also demand a dependency's pairs:
-- DEPENDENCIES = { { spec = "local.brew@r0", source = "...", setup = { "brew" } } }
```

### Dependencies

Specs declare dependencies; transitive resolution is automatic. Manifest authors specify only direct needs.

```lua
-- vendor.openocd@v3
DEPENDENCIES = {
  {
    spec = "arm.gcc@v2",
    source = "https://github.com/arm/specs/gcc-v2.lua",
    sha256 = "a1b2c3d4...",
    options = { version = "13.2.0" },
    needed_by = "build",  -- the default; "check".."install" are the choices
  },
}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local gcc_root = envy.package("arm.gcc@v2")
  envy.run("./configure --prefix=" .. install_dir .. " CC=" .. gcc_root .. "/bin/arm-none-eabi-gcc")
  envy.run("make -j$(nproc)")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run("make install", { cwd = stage_dir })
end
```

**Resolution:** an edge holds the dependent's `needed_by` phase until the dependency finishes `setup`, so host state is in place before anyone builds against it. Cycles are refused where the edge would be added, with the path printed. Dependencies name exact spec revisions—one revision always resolves the same way.

**Security:** a non-`local.*` spec cannot depend on a `local.*` spec—a published spec must not reach into someone's working tree. Enforced while parsing `DEPENDENCIES`, naming both identities.

## Unified DAG Execution Model

### Overview

Envy builds a dependency graph and executes packages in parallel via worker threads. No separation between "resolution" and "installation"—spec fetching and asset building interleave as dependencies require. Graph expands dynamically: spec_fetch discovers dependencies and spawns new package threads during execution.

### Phase Model

One task per `(identity, options)`, whose steps are a fixed ladder. A phase with no verb
to run costs one no-op step, so there is nothing to declare or infer:

- **`spec_fetch`** — acquire and load the spec; parse `DEPENDENCIES`, `PRODUCTS`, `SETUP`; wire edges and spawn the packages they name
- **`check`** — compute the package key and test the cache; a hit skips the payload phases
- **`import`** — take a prebuilt artifact from a package depot, if one matches the key
- **`fetch`** — download source materials into `fetch/`
- **`stage`** — prepare the staging area from fetched content
- **`build`** — compile or process staged content
- **`install`** — write final artifacts; on success the entry is marked complete
- **`setup`** — run the selected `SETUP` pairs as their own tasks (host state, never cached)
- **`export`** — write the package's export artifact when one was asked for
- **`completion`** — report the outcome row

A dependency edge is satisfied once the dependency finishes `setup`; the edge still
ratchets it through `export`, so export overlaps dependents' builds.

**Phase execution:** Scheduling lives in `task_engine` (src/task_engine.h), a domain-agnostic threaded executor: keyed tasks, linear steps, ratcheting target watermarks, per-step edges, dynamic task creation. `engine` adapts envy onto it — each package is one task whose steps are the phase ladder; SETUP pairs are single-step tasks. Inter-package dependencies become task edges via `needed_by` annotation (see below).

### Spec Fetching (Custom and Declarative)

**Identity requirement:** ALL specs must declare their identity:
```lua
-- vendor.lib@v1 spec file
IDENTITY = "vendor.lib@v1"
```
Envy validates declared identity matches requested identity. This prevents typos, stale references, copy-paste errors, and malicious substitution. No namespace exemptions—all specs require identity declaration.

**Declarative sources** (common case), the spec's own payload:
```lua
FETCH = "https://example.com/gcc.tar.gz"                                -- no verification
FETCH = { source = "https://example.com/lib.tar.gz", sha256 = "abc..." }
FETCH = {                                                    -- concurrent, verified apiece
  { source = "https://example.com/gcc.tar.gz", sha256 = "abc..." },
  { source = "https://example.com/gcc.tar.gz.sig" },
}
FETCH = { source = "git://github.com/vendor/lib.git", ref = "a1b2c3d4..." }
FETCH = { source = "s3://bucket/lib.tar.gz", sha256 = "ghi..." }        -- first-class
```

**Custom *spec* fetch** (exotic cases—JFrog, authenticated APIs, custom tools) is a
`source` table on the entry that *depends* on the spec, because the closure has to live
in a file envy has already loaded:
```lua
-- in some spec's DEPENDENCIES (or a BUNDLES declaration)
{ spec = "corporate.toolchain@v1", source = {
    dependencies = { { spec = "jfrog.cli@v2", source = "jfrog.lua" } },
    fetch = function(tmp_dir, options)
      envy.run(envy.product("jf") .. " rt dl specs/toolchain " .. tmp_dir)
      envy.commit_fetch({ "spec.lua", "lib" })   -- the whole tree becomes the spec dir
    end } }
```
`source.dependencies` are wired at `needed_by = spec_fetch` and installed before the
closure runs, which is what makes `envy.product("jf")` legal inside it.

**Fetch phase signature:** `FETCH(tmp_dir, options)` — tmp_dir is an ephemeral workspace; options come from the entry that declared the package.

**Fetch behavior:**
- **Polymorphic API**: Single file `envy.fetch({source="..."})` or batch `envy.fetch({{source="..."}, ...})`
- **Concurrent**: All downloads happen in parallel
- **Atomic**: All files downloaded and verified before ANY committed to fetch_dir (all-or-nothing)
- **SHA256 optional**: If provided, verified after download; if absent, permissive

**Verification:** SHA256 is **optional**. If `sha256` field present, Envy verifies after download. If absent, download proceeds without verification (permissive mode). Custom fetch functions cannot bypass—all downloads go through `envy.fetch()` API. Future "strict mode" will require SHA256 for all non-`local.*` specs.

**Cache layout:** a custom fetch produces a multi-file spec directory. The entry point
`spec.lua` is required; every other committed file lands beside it, so `require` and
`envy.loadenv` reach them:
```
~/.cache/envy/specs/corporate.toolchain@v1/blake3-{source_hash}/
├── envy-complete           # written only once the spec loads with the right IDENTITY
├── pkg/
│   ├── spec.lua            # Entry point (required)
│   └── lib/helpers.lua     # Anything else the fetch committed
├── fetch/                  # Committed files land here first
└── work/tmp/               # tmp_dir handed to the fetch function (cleaned after)
```

A custom fetch's cache entry is keyed by the declaring file, the identity, and the
options—a closure has no fingerprint. Editing the function body in place reuses the
entry; move it, or bump the revision, to force a refetch.

### Phase Dependencies via `needed_by`

**Semantics:** `needed_by` names the phase of the *dependent* that the edge blocks. The
dependency must finish `setup` before that phase starts, whatever phases it declares.

**Valid phases:** `check`, `import`, `fetch`, `stage`, `build`, `install`. The default is
`build`—the phase that usually needs a toolchain in place. `spec_fetch` is deliberately
not selectable: a dependency needed that early is a *fetch prerequisite*, and belongs in
`source.dependencies`, where it is a strong reference by construction.

```lua
-- vendor.openocd@v3
DEPENDENCIES = {
  { spec = "corp.pkgconf@r1", source = "pkgconf.lua", needed_by = "stage" },
  { spec = "arm.gcc@v2", source = "gcc.lua" },              -- build, the default
}
```

**Graph topology** (`corp.pkgconf@r1` gates the earlier phase, so it lands first):
```
[corp.pkgconf@r1 …setup] ─┐
                          ├→ [vendor.openocd@v3 stage] → [build] → [install] → …
[arm.gcc@v2 …setup] ──────┘  (gcc only has to be there by build)
```

Repeat declarations of one identity merge into a single edge carrying the *earliest*
`needed_by` any of them asked for.

### Dynamic Graph Expansion

**Memoization:** nodes are keyed by the canonical string `identity{["key"]=value,…}`—options as a Lua table literal, keys quoted and sorted, so the key round-trips and cannot collide. The first thread to ask for a node allocates it; the rest reuse it.

**Expansion process:**
1. Manifest roots seed the graph, each started toward `spec_fetch`
2. `spec_fetch` fetches the spec file(s), loads the Lua, and reads `DEPENDENCIES`
3. Each dependency is interned (memoized key), wired with its `needed_by`, and started
4. Their `spec_fetch` steps discover more, and so on until the graph stops growing
5. `task_engine::join_all()` reaps every worker, tolerating tasks created mid-join

**Cycle detection:** an edge that would close a cycle is refused where it would be added.
Example illegal cycle:
```lua
-- local.a@v1 spec: needs B installed to build
DEPENDENCIES = { { spec = "local.b@v1", source = "b.lua", needed_by = "build" } }

-- local.b@v1 spec: needs A's payload to stage
DEPENDENCIES = { { spec = "local.a@v1", source = "a.lua", needed_by = "stage" } }
```
Each waits for the other's `setup` → deadlock. `engine::wire_dependency` — the only place an edge is added — refuses an edge whose target already reaches the parent and prints the path it found ("Dependency cycle detected: local.b@v1 -> local.a@v1 -> local.b@v1"). Reachability, not the spawn path, so a back edge from a second manifest root, a weak fallback, or a `source.dependencies` prerequisite is caught too. Repeat declarations of one identity merge into one edge at the earliest `needed_by`; two option variants of it in one dependency list are a parse error.

### Command Execution Model

Commands implement `void execute()`; failure is an exception, which the CLI turns into an error line and a non-zero exit.

**Simple commands:** Synchronous work, no parallelism needed.

**Package commands:** create an engine over the loaded manifest and hand it the targets;
failure is an exception carrying every package's message, deduplicated and sorted.
```cpp
void cmd_install::execute() {
  auto const [m, c]{ cmd_startup_load("install", cfg_.manifest_path, ...) };
  engine eng{ *c, m.get() };
  eng.run_full(targets);  // spawns workers, joins them, throws on any failure
}
```
`run_full` runs the whole ladder; `resolve_graph` stops at `spec_fetch`, which is what
the query commands (`deploy`, `product`, `package`, `export`) build on.

**Parallelism:** Each task (package or SETUP pair) gets its own `std::thread` worker. Workers block on dependency watermarks via condition variables — legal because workers are plain threads, not pooled. Dependency edges wait to "setup complete" while ratcheting the dependency through export, so export overlaps dependents' builds.

**Lifetime:** Engine owns packages; `task_engine` (destroyed first) fails and joins all workers before package storage dies.

## Filesystem Cache

Cache layout, locking, verification, and recovery live in `docs/cache.md`.

## Platform-Specific Specs

Specs run only on host platform—no cross-deployment. Single spec file adapts via platform variables envy provides. Authors structure platform logic however they want.

**Envy-provided values** (fields of the `envy` table, available everywhere):
- `envy.PLATFORM` — `"darwin"`, `"linux"`, `"windows"`
- `envy.ARCH` — `"arm64"`, `"x86_64"`
- `envy.PLATFORM_ARCH` — combined: `"darwin-arm64"`, `"linux-x86_64"`
- `envy.EXE_EXT` — `""`, or `".exe"` on Windows

`ENVY_SHELL` (the shell constants) and `ENVY_IMPORTER` are the only bare globals envy
installs.

**Single-file with conditionals:**
```lua
FETCH = function(tmp_dir, options)          -- returning a declarative form is enough
  local version = options.version or "13.2.0"
  local hashes = {
    ["13.2.0"] = { ["darwin-arm64"] = "a1b2...", ["linux-x86_64"] = "c3d4..." },
  }

  return {
    source = string.format("https://arm.com/gcc-%s-%s-%s.tar.xz",
                           version, envy.PLATFORM, envy.ARCH),
    sha256 = hashes[version][envy.PLATFORM_ARCH],
  }
end

STAGE = {strip = 1}  -- Declarative form: extract all archives, strip 1 level

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  -- Copy bin/ to install_dir, platform-specific post-processing if needed
  envy.copy(stage_dir .. "/bin", install_dir .. "/bin")
end
```

**Multi-file with platform modules:**
```
arm.gcc@v2/
├── spec.lua
├── darwin.lua
├── linux.lua
└── checksums.lua
```

```lua
-- spec.lua
local impl = envy.loadenv(envy.PLATFORM)  -- loads darwin.lua or linux.lua beside this file
FETCH = function(tmp_dir, options) return impl.fetch(options, envy.loadenv("checksums")) end
STAGE = impl.stage
INSTALL = impl.install
```

**Platform constraints:** a spec's `PLATFORMS` narrows the platforms its *products* are
deployed for (intersected with each product's own `platforms`); which hosts run the
package at all is the manifest entry's `platforms`. A spec that cannot run here at all
says so directly:
```lua
PLATFORMS = { "darwin-arm64", "linux-x86_64" }             -- product constraint
assert(envy.PLATFORM_ARCH ~= "windows-arm64", "unsupported: " .. envy.PLATFORM_ARCH)
```

## Bundles

Bundles are collections of related specs distributed together. Single `envy-bundle.lua` manifest plus spec files, optionally with shared Lua helpers.

**Bundle manifest format:**
```lua
-- envy-bundle.lua
BUNDLE = "acme.toolchain@v1"
SPECS = {
  ["acme.gcc@v2"] = "specs/gcc.lua",
  ["acme.binutils@v1"] = "specs/binutils.lua",
  ["acme.helpers@v1"] = "specs/helpers.lua",
}
```

**Cache layout:** a bundle is a spec entry like any other—`specs/`, keyed on its source—
and is never unpacked into per-spec directories:
```
~/.cache/envy/specs/acme.toolchain@v1/blake3-{source_hash}/pkg/
├── envy-bundle.lua
├── specs/
│   ├── gcc.lua
│   ├── binutils.lua
│   └── helpers.lua
└── lib/
    └── common.lua    # Shared helpers, reachable by require() from any spec in the bundle
```

**Referencing bundles from manifests:**
```lua
PACKAGES = {
  -- Spec from bundle (inline)
  {
    spec = "acme.gcc@v2",
    bundle = {
      identity = "acme.toolchain@v1",
      source = "https://example.com/toolchain-bundle.zip",
      sha256 = "abc...",
    },
  },
  -- Bundle alias (reusable)
  { spec = "acme.binutils@v1", bundle = "toolchain" },
}

BUNDLES = {
  ["toolchain"] = {
    identity = "acme.toolchain@v1",
    source = "https://example.com/toolchain-bundle.zip",
  },
}
```

**Using another bundle's helpers:** depend on the bundle itself—`bundle` plus its own
`source`, which is what distinguishes it from a spec-from-bundle entry—and read code out
of it with `envy.loadenv_spec(identity, module)`, dot syntax for the path. Its own
siblings a spec just `require`s: the bundle root is on `package.path`.

```lua
-- vendor.mytool@v1, in some other bundle or standalone
DEPENDENCIES = {
  { bundle = "acme.toolchain@v1", source = "git://github.com/acme/specs", ref = "a1b2c3d",
    needed_by = "fetch" },     -- the access is checked against this, and build is too late
}

FETCH = function(tmp_dir, options)
  -- Fuzzy match: "toolchain" matches "acme.toolchain@v1"
  local common = envy.loadenv_spec("toolchain", "lib.common")
  return { source = common.build_download_url("gcc", options.version) }
end
```

**`envy.loadenv()` vs `envy.loadenv_spec()`:**
- `envy.loadenv(module)` — Load Lua file relative to current file. Works at global scope or in phases. Uses dot syntax (`"lib.utils"` → `lib/utils.lua`).
- `envy.loadenv_spec(identity, module)` — Load from declared dependency. Phase context required; validates `needed_by`. Uses dot syntax.

**Validation:** Bundle validation runs threaded—each spec's IDENTITY verified against SPECS table keys. All bundles validated on every load.

## TUI / Output

### Stream Semantics

**Stdout:** Machine-readable output only—`envy hash`, `envy lua --eval`, future asset path queries. Never logs, progress, or diagnostics.

**Stderr:** All human communication—logs, progress bars, warnings, errors. Thread-safe queue-based rendering.

### Immediate-Mode Architecture

**Stateless rendering:** TUI is pure function `(frame, width, now, ansi) → string`. Workers cache section frames; renderer reads at 30fps. No animation state—spinners computed from timestamps, progress bars show current values. Benefits: deterministic output, full unit testability, no state sync.

**Section state:** Vector of sections (allocation order = render order). Each: handle, active flag, cached frame. Worker thread calls `section_set_content(handle, frame)` on progress events; main thread renders all active sections each cycle.

**Render cycle (30fps):**
1. ANSI: clear previous progress region (cursor up, clear to end). Fallback: no-op.
2. Flush log queue (logs print in cleared space)
3. Get terminal width (syscall), current time
4. For each active section: `render_section_frame(cached_frame, width, ansi, now)`
5. ANSI: update line count for next clear. Fallback: throttle (2s), print if changed.

**Critical ordering:** Clear BEFORE flush prevents logs from being erased. Logs print where old progress was; new progress renders below logs.

**Section frame types:**
- `progress_data`: percent (0-100), status string → `[label] status [=====>   ] 42.5%`
- `text_stream_data`: line buffer, line_limit, start_time → last N lines of build output with spinner
- `spinner_data`: text, start_time, frame_duration → animated `|/-\` computed from elapsed time
- `static_text_data`: text → `[label] text`

**Interactive mode:** Global mutex serializes specs needing terminal control (sudo, installers). Acquire locks, pauses rendering; release unlocks, resumes. RAII guard available.

**Integration:** Phases delegate TUI management to `tui_actions` helpers (`run_progress`, `fetch_progress_tracker`, `extract_progress_tracker`)—single-responsibility, consistent formatting, testable in isolation. `envy.run()` auto-creates `run_progress` when spec has `tui_section`; Lua code gets TUI integration automatically.

**Test API:** `#ifdef ENVY_UNIT_TEST` exposes `g_terminal_width`, `g_isatty`, `g_now` globals and `test::render_section_frame()` for pure rendering tests without TUI thread.

### Log Formatting

**Plain mode** (`init(std::nullopt)`): Clean output, no timestamps/severity prefixes. Threshold = info.
```
Fetching gcc-13.2.0.tar.xz...
Warning: Spec deprecated
Error: SHA256 mismatch
```

**Structured mode** (explicit `-v/--verbose` flag): Timestamps + severity on all messages.
```
[2024-10-19 12:34:56.123] [DEBUG] Cache miss for arm.gcc@v2
[2024-10-19 12:34:56.234] [INFO] Fetching gcc-13.2.0.tar.xz...
[2024-10-19 12:34:56.789] [WARN] Spec deprecated
```

### Thread Model

Main thread runs TUI render loop; worker threads push to thread-safe queues. Uniform 16ms log refresh; progress refresh at 16ms (TTY) or 1024ms (non-TTY). Workers call `tui::is_tty()` to choose progress bar style (animated vs. periodic snapshots). Progress library handles ANSI clear/redraw; TUI owns timing and queue orchestration.

### API Surface

```cpp
namespace envy::tui {
  enum class level { debug, info, warn, error };

  // Lifecycle
  void init(std::optional<level> threshold);  // nullopt = plain mode
  void run();                                 // Blocking render loop
  void shutdown();                            // Signal exit, flush queues
  bool is_tty();                              // Expose isatty(STDERR_FILENO)

  // Logging to stderr (thread-safe, printf-style, queued)
  void debug(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
  void error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

  // Stdout (direct write, bypasses TUI, never queued)
  void print_stdout(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

  // Progress (thread-safe, retained-mode handles)
  struct progress_config { std::string label; /* style, type, etc. */ };
  int create_progress(progress_config cfg);
  void update_progress(int handle, float percent);
  void complete_progress(int handle);

  // Rendering control (for interactive subprocess handoff)
  void pause_rendering();   // Stop render loop, clear progress bars
  void resume_rendering();  // Restart render loop

  // Output redirection (for testing)
  void set_output_handler(std::function<void(std::string_view)> fn);
}
```

**Implementation:** Flat namespace with module-internal state (global mutexes, queues, atomics)—avoids singleton boilerplate while maintaining single logical instance. Single log queue protected by mutex. Workers format messages via `vsnprintf`, append to queue. Progress state stored in retained-mode map—workers update percentage via handle whenever desired. Main thread drains log queue at 16ms intervals, always flushes logs, renders current progress state at 16ms (TTY) or 1024ms (non-TTY). Atomic bools for shutdown and pause coordination. Non-TTY mode skips ANSI codes.

### Interactive Input & Process Spawning

**REPL mode** (`envy lua`): TUI runs in interactive mode—logs bypass queue, go straight to stderr via `fprintf`. No render loop, no progress bars.

**Spec process execution:** one verb, `envy.run(script, opts)`, in three modes:

- default — stdout/stderr piped line-by-line to `tui::info()`, stdin closed; build output appears as logs
- `capture = true` — stdout/stderr collected into the returned `{ exit_code, stdout, stderr }`; pair with `check = false` for silent probes like `brew list | grep foo`
- `interactive = true` — stdin/stdout/stderr inherited. TUI calls `pause_rendering()`, clears progress bars, waits for the child, then `resume_rendering()`; the render loop idles on an atomic flag

**Platform abstraction:** Unix uses `fork()`/`execvp()`/`pipe()`/`dup2()`. Windows uses `CreateProcess()`/`STARTUPINFO` with redirected handles. Both hide behind `envy::process` interface. Terminal control via `isatty()`/`_isatty()` + ANSI escape codes (Windows 10+ `ENABLE_VIRTUAL_TERMINAL_PROCESSING` via `SetConsoleMode`).

## Testing

### Unit Tests

Side-by-side with source: `src/cache/lock.cpp` + `src/cache/lock_test.cpp`. Doctest C++ single-file amalgamation; automatic registration. All test `.cpp` files compiled directly (no static archive) into `out/build/envy_unit_tests` executable. Runs as CMake build step; touches `out/build/envy_unit_tests.timestamp` on exit 0. Top-level targets: `envy` tool + test timestamp. `./build.sh` builds everything—tests run automatically.

### Functional Tests

Python 3.13+ stdlib only (`unittest`—no third-party deps). Located in `functional_tests/` flat (no subdirs). Parallel execution; each test uses isolated cache directory via `ENVY_TEST_ID` environment variable. Per-test cleanup via fixtures (context managers).

**Binary split:** Tests run `envy_functional_tester`—same sources and `main.cpp` as `envy`, plus `ENVY_FUNCTIONAL_TESTER=1`. It exists chiefly to carry the sanitizers (`envy` ships unsanitized and stripped); the extra commands are secondary. Tests default to it via `test_config.get_envy_executable()`; only tests about the shipped artifact (bootstrap, re-exec, mirroring) use `get_envy_production_executable()`, and those forgo sanitizer coverage. Everything else drives public commands—`install --manifest` over a generated manifest runs any spec through the engine, so no test-only command is needed for spec work.

**Cache testing:** `cache-test ensure-package <identity> <platform> <arch> <hash>` and `cache-test ensure-spec <identity>` call `cache::ensure_pkg`/`ensure_spec` directly, so two processes can be choreographed around one lock without Lua. Flags: `--cache-root`, `--test-id`, `--barrier-signal[-after]`, `--barrier-wait[-after]`, `--crash-after`, `--fail-before-complete`. Barriers are filesystem markers; `--crash-after` SIGKILLs (not `abort()`—a Mach exception makes macOS file a crash report per run). The commands print nothing: tests read outcomes from the trace stream production already emits (`cache_hit.fast_path`, `cache_miss` = this process staged, `lock_acquired.lock_path`, `cache_entry_finalized.entry_dir`). Single-process layout and path construction are unit-tested in `cache_tests.cpp` instead.

**Spec testing (future):** Test specs embedded as string literals, written to temp dirs—namespace `functionaltest.*` (e.g., `functionaltest.gcc@v1`). Specs use filesystem `fetch` for speed; HTTP tests spawn local servers separately.

**CI:** GitHub Actions on Darwin/Linux/Windows × x64/arm64.
