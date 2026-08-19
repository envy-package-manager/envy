# Envy Architecture

## Project Manifests

All script-global variables are uppercase: manifests export `PACKAGES`; specs declare `IDENTITY`, `FETCH`, `STAGE`, `BUILD`, `INSTALL`, `SETUP`, `DEPENDENCIES`, and `PRODUCTS`.

**Syntax:** Shorthand `"namespace.name@version"` expands to `{ spec = "namespace.name@version" }`. Table syntax supports `source`, `sha256`, `file`, `fetch`, `options`, `dependencies`, `needed_by`, `setup`.

**Platform-specific packages:** Manifests are Lua scripts—use conditionals and `envy.join()` to combine common and OS-specific package lists.

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
        source = "git://github.com/vendor/openocd-recipe.git",
        ref = "a1b2c3d4e5f6...",  -- Commit SHA
        options = { target = "arm" },
    },
    {  -- Custom fetch (JFrog example)
        spec = "corporate.toolchain@v1",
        FETCH = function(tmp_dir, options)
            local jfrog = envy.asset("jfrog.cli@v2")
            envy.run(jfrog .. "/bin/jfrog rt download specs/toolchain.lua " .. tmp_dir .. "/recipe.lua")
            envy.commit_fetch({filename = "recipe.lua", sha256 = "sha256_here..."})
        end,
        DEPENDENCIES = {
            { spec = "jfrog.cli@v2", source = "...", sha256 = "...", needed_by = "recipe_fetch" }
        }
    },
    {  -- Project-local (development)
        spec = "local.wrapper@v1",
        file = "./envy/specs/wrapper.lua",
        options = { base = "arm.gcc@v2" },
    },
}

local darwin_packages = {
    "envy.homebrew@v4",
}

local linux_packages = {
    "system.apt@v1",
}

PACKAGES = ENVY_PLATFORM == "darwin" and envy.join(common, darwin_packages)
        or ENVY_PLATFORM == "linux" and envy.join(common, linux_packages)
        or common
```

**Field semantics:**
- `identity` — Spec identity declaration (**required in all spec files**, no exemptions)
- `source` — URL (http/https/s3/git/file) or Git repo for declarative fetch
- `ref` — Git commit SHA or committish (required for git sources)
- `sha256` — Expected hash for verification (**optional**, permissive by default; future strict mode will require for non-`local.*`)
- `fetch` — Custom Lua function for exotic sources (JFrog, authenticated APIs); mutually exclusive with `source`
- `file` — Project-local spec path (never cached; `local.*` namespace only)
- `subdir` — Subdirectory within archive or git repo containing spec entry point
- `options` — Recipe-specific configuration (passed to phase functions as `options` parameter)
- `setup` — Names of the spec's `SETUP` pairs to run on this host (manifest entries only; never hashed into the package key)
- `dependencies` — Transitive dependencies (specs this spec needs)
  - **Strong:** full spec spec with `source` (or manifest-provided source)
  - **Weak:** partial `recipe` plus `weak = { ... }` fallback spec
  - **Reference-only:** partial `recipe` with no `source`/`weak` (must be satisfied by some other provider)
  - **Nested fetch prerequisites:** inside `source.dependencies`, may also be weak/reference-only
- `needed_by` — Phase dependency annotation (default: `"fetch"`, custom: `"recipe_fetch"`, `"build"`, etc.)

**Uniqueness validation:** Envy validates manifests post-execution. Duplicate recipe+options combinations error (deep comparison—string `"foo@v1"` matches `{ spec = "foo@v1" }`). Same spec with conflicting sources (different `source`/`sha256`/`file`/`fetch`) errors. Same recipe+options from identical sources is duplicate. Different options yield different deployments—allowed.

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

**Dynamic shell selection (function):**
```lua
DEFAULT_SHELL = function(ctx)
  -- Query deployed assets to use as interpreter
  -- Note: ctx:asset() used here (not envy.asset()) because DEFAULT_SHELL
  -- runs in manifest context before spec phases execute
  local python = ctx:asset("python@v3.11")
  return {inline = {python .. "/bin/python3", "-c"}}
end
```

**Use case:** Express all build scripts in a custom language (Python, Tcl, Ruby) without assuming it's pre-installed. The function can query `ctx:asset()` to locate envy-deployed interpreters. Bootstrap specs (Python itself) must use built-in shells.

**Implementation notes:**
- Functions evaluated lazily during engine execution (after dependency graph built)
- Result cached per manifest
- DEFAULT_SHELL functions use `ctx:asset()` (not `envy.asset()`) because they run in manifest context
- **Future work:** Dependency analysis—extract `ctx:asset()` calls from function to add implicit dependencies, ensuring interpreter deploys before dependent specs run

## Specs

### Organization

**Identity:** Specs are namespaced with version: `arm.gcc@v2`, `gnu.binutils@v3`. The `@` symbol denotes **spec version**, not asset version. Asset versions come from `options` in manifest. Multiple spec versions coexist; `local.*` namespace reserved for project-local specs.

**Sources:**
- **Declarative:** `source` field with URL (http/https/s3/file) or git repo; verified via `sha256` (URL) or `ref` (git); cached
- **Custom fetch:** `fetch` function with verification enforced at API boundary (`envy.fetch`, `envy.commit_fetch`); cached
- **Project-local:** `file` path in project tree; never cached; `local.*` namespace only
- **Fetch prerequisites (nested):** `source.dependencies` declares specs that must reach completion before this recipe’s fetch runs. These can be strong, weak, or reference-only.

**Formats:**
- **Single-file:** `.lua` file (declarative sources only)
- **Multi-file:** Directory with `recipe.lua` entry point (custom fetch, archives, git repos)

**Integrity:** Two orthogonal checks:

1. **Identity validation** (ALL specs, always required):
   - Spec must declare `identity = "..."` matching referrer's expectation
   - Catches typos, stale references, copy-paste errors
   - No namespace exemptions

2. **SHA256 verification** (optional, namespace-specific):
  - Declarative sources accept SHA256 (URL) or commit SHA (git)
  - Custom fetch accepts SHA256 per-file via `envy.fetch()` API
  - If SHA256 provided, verification happens at fetch time; mismatch causes hard failure
  - Never re-verified from cache
  - **Permissive by default**: SHA256 optional for all specs
  - **Namespace rule**: `local.*` specs never require SHA256 (files are local/trusted)
  - **Future strict mode**: Will require SHA256 for all non-`local.*` specs

### Dependency Semantics (strong, weak, reference-only)

- **Strong dependencies** provide a complete spec (manifest or explicit `source`). They are instantiated immediately and run toward their target phase.
- **Weak dependencies** specify a query (`spec = "name"` or partial identity) plus a fallback spec in `weak = { ... }`. The engine tries to satisfy the query from existing/manifest/other strong nodes; if no match, it spawns the fallback. Ambiguities raise errors with all candidates listed.
- **Reference-only dependencies** provide only a query (no `source`/`weak`). They must be satisfied by some other spec in the graph; otherwise resolution fails after convergence.
- **Nested fetch prerequisites** live in `source.dependencies` and follow the same rules. They must complete (typically to `completion`) before the parent’s `recipe_fetch` runs.
- Resolution is iterative: the engine waits for all active specs to reach their target phases, runs weak-resolution passes (matching or spawning fallbacks), and repeats while progress is made. Progress accounts for newly spawned fallbacks even when unresolved counts stay flat.

### Verbs

Specs define verbs describing how to acquire, validate, and install packages:

- **`fetch`** — Acquire source materials. Can be:
  - String: `fetch = "https://..."` (no verification)
  - Single file: `fetch = {url="...", sha256="..."}` (optional verification)
  - Multiple files: `fetch = {{url="..."}, {url="..."}}` (concurrent, optional verification per-file)
  - Custom function: `FETCH = function(tmp_dir, options) envy.fetch(...) end` (imperative with `envy.fetch()` API)
  - Function returning declarative: `FETCH = function(tmp_dir, options) return "https://..." end` (enables templating with options; return value can be any declarative form: string, table, array; can mix with imperative `envy.fetch()` calls)
- **`stage`** — Prepare staging area from fetched content. Default extracts archives; custom functions can manipulate source tree. Declarative form takes `strip` and `only`—`STAGE = {strip = 1, only = {"bin/clang-*", "lib/**/include/*.h"}}` extracts only matching paths (a directory takes its subtree), matched post-`strip`; the rest of the archive is never decompressed. Globs: `*`/`?` within a component, `**` across, `[a-z]` classes. An `only` entry nothing in the fetch dir provides is a hard error.
- **`build`** — Compile or process staged content. Specs access staging directory, dependency artifacts, and install directory.
- **`install`** — Write final artifacts to install directory. On success, envy atomically renames to asset directory and marks complete.
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
- Install writes to `install_dir`; on successful return, envy auto-marks complete
- Lock destructor renames `install/` → `pkg/`, touches `envy-complete`
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
- Lock destructor: `if (user_managed_) { purge_entry_dir(); }` vs `if (completed_) { rename_install_to_pkg(); }` — pair locks always take the ephemeral branch.
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
    url = "https://github.com/arm/specs/gcc-v2.lua",
    sha256 = "a1b2c3d4...",
    options = { version = "13.2.0" },
  },
}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local gcc_root = envy.asset("arm.gcc@v2")
  envy.run("./configure --prefix=" .. install_dir .. " CC=" .. gcc_root .. "/bin/arm-none-eabi-gcc")
  envy.run("make -j$(nproc)")
end

INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run("make install", { cwd = stage_dir })
end
```

**Resolution:** Topological sort ensures dependencies deploy before dependents. Cycles error (must be DAG). Dependencies specify exact spec versions—same spec version always uses same deps (reproducible builds).

**Security:** Non-local specs cannot depend on `local.*` specs. Envy enforces at load time.

## Unified DAG Execution Model

### Overview

Envy builds a dependency graph and executes packages in parallel via worker threads. No separation between "resolution" and "installation"—spec fetching and asset building interleave as dependencies require. Graph expands dynamically: spec_fetch discovers dependencies and spawns new package threads during execution.

### Phase Model

Each DAG node represents `(recipe_identity, options)` with up to seven verb phases:

- **`recipe_fetch`** — Load spec Lua file(s) into cache; discover dependencies; add child nodes to graph
- **`check`** — Test if asset already satisfied (skip remaining phases if true)
- **`fetch`** — Download/acquire source materials into `fetch/`
- **`stage`** — Prepare build staging area from fetched content
- **`build`** — Compile or process staged content
- **`install`** — Write final artifacts to install directory
- **`deploy`** — Post-install actions (env setup, capability registration)

**Node optimization:** Only declared/inferred phases create nodes. Minimal specs (just `source` field) infer `recipe_fetch` → `fetch` → `stage`, skip `build`/`install`/`deploy`. Spec without `build` verb omits build node. Zero-verb overhead for simple cases.

**Phase execution:** Scheduling lives in `task_engine` (src/task_engine.h), a domain-agnostic threaded executor: keyed tasks, linear steps, ratcheting target watermarks, per-step edges, dynamic task creation. `engine` adapts envy onto it — each package is one task whose steps are the phase ladder; SETUP pairs are single-step tasks. Inter-package dependencies become task edges via `needed_by` annotation (see below).

### Spec Fetching (Custom and Declarative)

**Identity requirement:** ALL specs must declare their identity:
```lua
-- vendor.lib@v1 spec file
IDENTITY = "vendor.lib@v1"

-- local.wrapper@v1 spec file
IDENTITY = "local.wrapper@v1"

-- Rest of recipe...
```
Envy validates declared identity matches requested identity. This prevents typos, stale references, copy-paste errors, and malicious substitution. No namespace exemptions—all specs require identity declaration.

**Declarative sources** (common case):
```lua
-- String shorthand (no verification)
FETCH = "https://example.com/gcc.tar.gz"

-- Single file with verification
FETCH = {url = "https://example.com/lib.lua", sha256 = "abc..."}

-- Multiple files (concurrent download)
FETCH = {
  {url = "https://example.com/gcc.tar.gz", sha256 = "abc..."},
  {url = "https://example.com/gcc.tar.gz.sig", sha256 = "def..."}
}

-- Git repository
FETCH = {url = "git://github.com/vendor/lib.git", ref = "a1b2c3d4..."}

-- S3 (first-class support)
FETCH = {url = "s3://bucket/lib.lua", sha256 = "ghi..."}
```

**Custom fetch functions** (exotic cases—JFrog, authenticated APIs, custom tools):
```lua
{
  spec = "corporate.toolchain@v1",
  FETCH = function(tmp_dir, options)
    local jfrog = envy.asset("jfrog.cli@v2")  -- Access installed dependency

    -- Download files concurrently with verification
    envy.fetch({
      {source = "https://internal.com/recipe.lua", sha256 = "abc..."},
      {source = "https://internal.com/helpers.lua", sha256 = "def..."}
    })
    envy.commit_fetch({"recipe.lua", "helpers.lua"})
  end,
  DEPENDENCIES = {
    { spec = "jfrog.cli@v2", source = "...", sha256 = "...", needed_by = "recipe_fetch" }
  }
}
```

**Fetch phase signature:** `FETCH(tmp_dir, options)` — tmp_dir is ephemeral workspace; options passed from manifest.

**Fetch behavior:**
- **Polymorphic API**: Single file `envy.fetch({source="..."})` or batch `envy.fetch({{source="..."}, ...})`
- **Concurrent**: All downloads happen in parallel
- **Atomic**: All files downloaded and verified before ANY committed to fetch_dir (all-or-nothing)
- **SHA256 optional**: If provided, verified after download; if absent, permissive

**Verification:** SHA256 is **optional**. If `sha256` field present, Envy verifies after download. If absent, download proceeds without verification (permissive mode). Custom fetch functions cannot bypass—all downloads go through `envy.fetch()` API. Future "strict mode" will require SHA256 for all non-`local.*` specs.

**Cache layout:** Custom fetch → multi-file cache directory with `recipe.lua` entry point:
```
~/.cache/envy/specs/
└── corporate.toolchain@v1/
    ├── envy-complete
    ├── recipe.lua           # Entry point (required)
    ├── helpers.lua
    ├── fetch/               # Downloaded files moved here after verification
    │   └── envy-complete
    └── work/
        └── tmp/             # Temp directory for envy.fetch() (cleaned after)
```

### Phase Dependencies via `needed_by`

**Default behavior:** Spec A depends on spec B → A's `fetch` phase waits for B's last declared phase (usually `deploy`).

**Custom phase dependencies:** Use `needed_by` annotation to couple specific phases:
```lua
DEPENDENCIES = {
  { spec = "jfrog.cli@v2", url = "...", sha256 = "...", needed_by = "recipe_fetch" }
}
```

**Semantics:** Dependency must complete its last declared phase before this node's specified phase starts. If `needed_by = "recipe_fetch"`, jfrog.cli's `deploy` completes before this recipe's `recipe_fetch` begins (spec cannot be fetched until tool is installed).

**Concrete example (corporate JFrog workflow):**
```lua
-- Manifest packages
{
  {
    spec = "corporate.toolchain@v1",
    FETCH = function(tmp_dir, options)
      local jfrog = envy.asset("jfrog.cli@v2")  -- Tool must be installed first
      -- ... fetch using jfrog CLI ...
    end,
    DEPENDENCIES = {
      {
        spec = "jfrog.cli@v2",
        source = "https://public.com/jfrog-cli-recipe.lua",
        sha256 = "...",
        needed_by = "recipe_fetch"  -- Block corporate.toolchain recipe_fetch until jfrog.cli deployed
      }
    }
  }
}
```

**Graph topology:**
```
[jfrog.cli recipe_fetch] → [jfrog.cli fetch] → [jfrog.cli install] → [jfrog.cli deploy]
                                                                           ↓
                                              [corporate.toolchain recipe_fetch] → ...
```

**Valid `needed_by` phases:** `recipe_fetch`, `check`, `fetch`, `stage`, `build`, `install`, `deploy`. Omitting `needed_by` defaults to blocking on `fetch` (standard transitive dependency).

### Dynamic Graph Expansion

**Memoization:** Nodes keyed by `"identity{key1=val1,key2=val2}"` (canonical string, options sorted lexicographically). First thread to request a node allocates it; later threads reuse existing node.

**Expansion process:**
1. Manifest roots seed graph with initial `recipe_fetch` nodes
2. `recipe_fetch` node executes: fetch spec file(s), load Lua, evaluate `dependencies` field
3. For each dependency: ensure memoized node exists, add edges based on `needed_by`
4. Child `recipe_fetch` nodes execute, discover their dependencies, add more nodes
5. Graph grows until all transitive dependencies discovered
6. `task_engine::join_all()` reaps every worker, tolerating tasks created mid-join

**Cycle detection:** Must catch cycles during graph construction. Example illegal cycle:
```lua
-- Spec A
{ spec = "A@v1", FETCH = function(tmp_dir, opts) envy.asset("B@v1") end,
  DEPENDENCIES = { { spec = "B@v1", needed_by = "recipe_fetch" } } }

-- Spec B
{ spec = "B@v1", dependencies = { { spec = "A@v1", needed_by = "recipe_fetch" } } }
```
Both specs need each other for `recipe_fetch` → deadlock. Envy detects via reachability check before adding edges; errors with cycle path.

### Command Execution Model

Commands implement `bool execute()` returning success/failure.

**Simple commands:** Synchronous work, no parallelism needed.

**Package commands:** Create engine, execute packages in parallel via worker threads:
```cpp
bool cmd_install::execute() {
  engine eng{ cache_, manifest_->packages() };
  eng.execute();  // Spawns worker threads, waits for completion
  print_summary(eng.roots());
  return true;
}
```

**Parallelism:** Each task (package or SETUP pair) gets its own `std::thread` worker. Workers block on dependency watermarks via condition variables — legal because workers are plain threads, not pooled. Dependency edges wait to "setup complete" while ratcheting the dependency through export, so export overlaps dependents' builds.

**Lifetime:** Engine owns packages; `task_engine` (destroyed first) fails and joins all workers before package storage dies.

## Filesystem Cache

Cache layout, locking, verification, and recovery live in `docs/cache.md`.

## Platform-Specific Specs

Specs run only on host platform—no cross-deployment. Single spec file adapts via platform variables envy provides. Authors structure platform logic however they want.

**Envy-provided globals:**
- `ENVY_PLATFORM` — `"darwin"`, `"linux"`, `"windows"`
- `ENVY_ARCH` — `"arm64"`, `"x86_64"`
- `ENVY_PLATFORM_ARCH` — Combined: `"darwin-arm64"`, `"linux-x86_64"`
- `ENVY_OS_VERSION` — `"14.0"` (macOS), `"22.04"` (Ubuntu)

**Single-file with conditionals:**
```lua
FETCH = function(tmp_dir, options)
  local version = options.version or "13.2.0"
  local hashes = {
    ["13.2.0"] = {
      ["darwin-arm64"] = "a1b2...", ["linux-x86_64"] = "c3d4...",
    },
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
├── recipe.lua
├── darwin.lua
├── linux.lua
└── checksums.lua
```

```lua
-- recipe.lua
local impl = require(ENVY_PLATFORM)  -- Loads darwin.lua or linux.lua
FETCH = function(ctx) return impl.fetch(ctx, require("checksums")) end
STAGE = impl.stage
INSTALL = impl.install
```

**Platform validation:**
```lua
local SUPPORTED = { darwin = { arm64 = true }, linux = { x86_64 = true } }
assert(SUPPORTED[ENVY_PLATFORM] and SUPPORTED[ENVY_PLATFORM][ENVY_ARCH],
       "Unsupported platform: " .. ENVY_PLATFORM_ARCH)
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

**Cache layout:**
```
~/.cache/envy/bundles/
└── acme.toolchain@v1/
    ├── envy-bundle.lua
    ├── specs/
    │   ├── gcc.lua
    │   ├── binutils.lua
    │   └── helpers.lua
    └── lib/
        └── common.lua    # Shared helpers
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

**Inter-spec dependencies within bundles:**
Specs use `envy.loadenv_spec(identity, module)` to load Lua code from declared bundle dependencies. Uses Lua dot syntax for module paths (e.g., `"lib.common"` → `lib/common.lua`). Requires `needed_by` annotation.

```lua
-- acme.gcc@v2 spec (within bundle)
DEPENDENCIES = {
  {
    bundle = "acme.toolchain@v1",  -- Depend on own bundle for helpers
    needed_by = "fetch",
  },
}

FETCH = function(tmp_dir, options)
  -- Load helper from bundle (fuzzy match: "toolchain" matches "acme.toolchain@v1")
  local common = envy.loadenv_spec("toolchain", "lib.common")
  local url = common.build_download_url("gcc", options.version)
  return {source = url, sha256 = options.sha256}
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

**Spec process execution:** Three modes via `ctx` API:

- **`run_capture(cmd, args)`** — Stdout/stderr piped to string, returned to Lua. Stdin closed. No TUI interaction (silent checks like `brew list | grep foo`).
- **`run(cmd, args)`** — Stdout/stderr piped line-by-line to `tui::info()`. Stdin closed. No TUI pause (build output appears as logs).
- **`run_interactive(cmd, args)`** — Stdin/stdout/stderr inherited. TUI calls `pause_rendering()`, clears progress bars, waits for subprocess exit, calls `resume_rendering()`. Render loop idles on atomic flag.

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
