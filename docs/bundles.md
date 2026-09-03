# Package Bundles Implementation Plan

## Overview

Add support for "bundles"—distribution containers holding multiple specs plus shared helper files. Bundles enable spec authors to group related specs in a single git repo or zip while sharing common Lua code.

## Terminology

- **Package (pkg)**: The installed artifact produced by running a spec's phases
- **Specification (spec)**: A Lua file describing how to fetch, build, and install a package
- **Bundle**: A container holding multiple specs plus shared helper files

Note: "recipe" is fully deprecated terminology—use "spec" instead.

## Problem Statement

Currently, each spec is cached in its own isolated directory (`specs/<identity>/`). This prevents code sharing between related specs. For example, if multiple specs need to fetch artifacts from JFrog Artifactory using the same authentication and API logic, each spec must duplicate that code inline or rely on external tooling.

Bundles solve this by allowing multiple specs to live together in a single cached directory, enabling standard Lua `require()` for shared helpers within the bundle.

## Design Summary

### Core Concepts

- **Bundle**: A directory with `envy-bundle.lua` manifest declaring multiple specs
- **Bundle identity**: `namespace.name@revision` (same format as specs)
- **Cache location**: `specs/` alongside atomic specs (no separate `bundles/` directory)
- **Distinction**: Presence of `envy-bundle.lua` determines bundle vs atomic spec
- **Not unpacked**: Bundles are NOT unpacked into separate spec directories; envy reaches into the bundle directory to resolve contained specs

### Libraries Are Just Specs

There is no separate "library" concept. Shared helper code (like JFrog fetchers or common build utilities) is just a spec without phase verbs—it only contains Lua code for other specs to load via `envy.loadenv_spec()`. The `SPECS` table in a bundle can include both "active" specs (with `FETCH`, `BUILD`, etc.) and "library" specs (Lua code only).

### Bundle Versioning vs Spec Versioning

Bundle identity (`acme.toolchain-specs@v1`) and spec identities (`acme.gcc@v2`) are **independent**. A bundle at `@v1` can contain specs at `@v2`, `@v3`, etc. Updating a spec's version (e.g., adding a new GCC version `acme.gcc@v3`) doesn't require updating the bundle version—just add the new spec to `SPECS`.

Bundle version changes indicate structural changes to the bundle itself (reorganizing files, changing helper APIs, removing deprecated specs).

### Namespace Freedom

Bundles can contain specs from any namespace—this is the author's freedom. For example, a corporate umbrella bundle `acme.toolchain-specs@v1` might contain specs like `vendor.gcc@v2`, `internal.buildtools@v1`, and `thirdparty.cmake@v3`.

### Bundle Manifest Format

```lua
-- envy-bundle.lua at root of git repo or zip
BUNDLE = "acme.toolchain-specs@v1"

SPECS = {
  ["acme.gcc@v2"] = "specs/gcc.lua",
  ["acme.clang@v2"] = "specs/clang.lua",
  ["acme.helpers@v1"] = "lib/helpers.lua",  -- library spec (no verbs)
}
```

- `BUNDLE`: Required identity for security verification. When envy fetches a bundle, it compares the declared identity in the user's manifest against this field. A mismatch is a fatal error, preventing identity spoofing attacks.
- `SPECS`: Required table mapping spec identities to file paths within bundle. Each referenced file must contain an `IDENTITY` declaration matching the key.

### User Manifest Syntax

Two ways to reference bundles:

1. **Named bundles** in `BUNDLES` table (reusable across multiple packages)
2. **Inline bundles** directly in package entries (one-off usage)

```lua
BUNDLES = {
  -- Key is a short alias for use within this manifest
  ["toolchain"] = {
    identity = "acme.toolchain-specs@v1",  -- actual bundle identity
    source = "git://github.com/acme/toolchain-specs",
    ref = "a1b2c3d4e5f6",
  },
}

PACKAGES = {
  -- Local spec (relative path via source)
  {spec = "local.foo@r0", source = "./specs/foo.lua"},

  -- Remote atomic spec (URL via source)
  {spec = "cmake.tools@v3", source = "https://example.com/cmake.lua", sha256 = "..."},

  -- From named bundle (references BUNDLES table by alias)
  {spec = "acme.gcc@v2", bundle = "toolchain"},

  -- Inline bundle (one-off, doesn't require BUNDLES entry)
  {spec = "other.thing@v1", bundle = {
    identity = "other.misc@v1",
    source = "git://github.com/other/misc",
    ref = "deadbeef",
  }},
}
```

**BUNDLES table keys** are short aliases for convenience within the manifest. The `identity` field is the bundle's actual identity used for caching and verification. Inline bundles skip the alias and specify the bundle definition directly.

### Validation Rules

- Every package entry requires exactly one of: `source` or `bundle`
- `source` accepts URLs, local absolute paths (`/path/to/spec.lua`), and local relative paths (`./specs/foo.lua`). The existing `uri_classify()` function distinguishes these.
- Bare string identities (e.g., `"foo@v1"`) are parse errors—explicit `source` or `bundle` required
- Bundle identity in manifest must match `BUNDLE` declared in fetched `envy-bundle.lua`
- Spec identities in `SPECS` table must match `IDENTITY` in referenced files

### Lua API Additions

| Function | Context | Purpose |
|----------|---------|---------|
| `envy.package(identity)` | Spec phases | Get installed package path (already exists) |
| `envy.loadenv_spec(identity, module)` | Spec phases only | Load Lua from declared dependency into sandboxed table |
| `envy.loadenv(module)` | Any context | Load local file into sandboxed table |

**Context clarification:**

- `envy.loadenv(module)`: Allowed in any context (manifest global scope, spec global scope, phase functions). Uses Lua dot syntax (`"lib.utils"` → `lib/utils.lua`). Path is **always relative to the currently-executing Lua file** (uses `debug.getinfo` to determine caller's source file). Intended for loading local helper files in the same project or spec directory—NOT for loading other specs from the cache (users don't know cache paths).

- `envy.loadenv_spec(identity, module)`: **Only callable from within phase functions**. Uses Lua dot syntax (`"lib.common"` → `lib/common.lua`). Uses the `needed_by` dependency system and is runtime-verified. If called at global scope, envy throws an error because the phase context doesn't exist yet.

### Identity Fuzzy Matching

Both `envy.package()` and `envy.loadenv_spec()` support fuzzy identity matching, consistent with existing dependency resolution:

```lua
-- These all resolve to "acme.gcc@v2" if declared in DEPENDENCIES:
envy.package("acme.gcc@v2")  -- exact match
envy.package("acme.gcc")     -- matches any version
envy.package("gcc")          -- matches any namespace.gcc
```

### Require Semantics

**Within a bundle** (standard `require()`):

When executing a spec that lives inside a bundle, envy prepends the bundle's cache directory to Lua's `package.path`. This enables standard `require()` for sibling files:

```lua
-- Inside acme.gcc@v2 which lives in bundle at ~/.envy/specs/acme.toolchain-specs@v1/
local helpers = require("lib.helpers")  -- resolves to bundle_root/lib/helpers.lua
```

**From outside** (`envy.loadenv_spec()`):

To load code from a different spec (whether bundle or atomic spec), use `envy.loadenv_spec()`. This requires declaring the dependency:

```lua
DEPENDENCIES = {
  {bundle = "acme.toolchain-specs@v1", source = "...", ref = "..."},
}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  -- Load helper code from the bundle
  local helpers = envy.loadenv_spec("toolchain-specs", "lib.helpers")
  helpers.do_something()
end
```

The subpath is relative to the spec/bundle root, without the `.lua` extension.

**Manifest composition** (`envy.import()`):

For composing manifests from subprojects, use `envy.import()` at manifest load time:

```lua
local sub = envy.import("libs/subproject")   -- directory or manifest path
PACKAGES = envy.extend(sub.PACKAGES, {
  -- additional packages...
})
```

Bundle aliases are scoped to the manifest that wrote them: an imported entry's `bundle = "x"`
resolves against the imported `BUNDLES` first, then the root's. Re-exporting `BUNDLES` is
unnecessary, and two projects may use the same alias for different bundles. See
`docs/lua_api.md` for `envy.import` versus `envy.loadenv`.

### Bundle Dependencies vs Spec-from-Bundle Dependencies

There are two distinct dependency patterns:

1. **Depending on a bundle** (`{bundle = "...", source = "..."}`): The bundle is fetched to `specs/` cache so the client can use `envy.loadenv_spec()` to access helper code. **No packages are installed**—this is purely for Lua code access during phases.

2. **Depending on a spec from a bundle** (`{spec = "...", bundle = "..."}`): The bundle is fetched, then the specific spec is resolved and executed. The spec's package is installed to `assets/`. If you need an installed package, use this form.

**Rule of thumb:** If you need to call `envy.package()` to get an installed path, depend on the spec. If you only need `envy.loadenv_spec()` for helper code, depend on the bundle directly.

### Spec Dependency Examples

**Spec depending on a bundle** (for shared helpers):

```lua
-- vendor.mytool@v1 spec file
IDENTITY = "vendor.mytool@v1"

DEPENDENCIES = {
  -- Depend on bundle directly to access its helper files
  {
    bundle = "acme.toolchain-specs@v1",
    source = "git://github.com/acme/toolchain-specs",
    ref = "a1b2c3d4e5f6",
  },
}

FETCH = function(tmp_dir)
  -- Use helper from the bundle
  local jfrog = envy.loadenv_spec("acme.toolchain-specs", "lib.jfrog")
  jfrog.fetch("com/vendor/mytool.tar.gz", tmp_dir)
  envy.commit_fetch("mytool.tar.gz")
end
```

**Spec depending on a spec from a bundle**:

```lua
-- vendor.app@v1 spec file
IDENTITY = "vendor.app@v1"

DEPENDENCIES = {
  -- Depend on a specific spec that lives inside a bundle
  {
    spec = "acme.gcc@v2",
    bundle = {
      identity = "acme.toolchain-specs@v1",
      source = "git://github.com/acme/toolchain-specs",
      ref = "a1b2c3d4e5f6",
    },
  },
}

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  -- Use the installed package
  local gcc = envy.package("acme.gcc")
  envy.run(gcc .. "/bin/gcc -o app main.c")
end
```

**Spec depending on both bundle helpers and a spec from that bundle**:

```lua
-- vendor.complex@v1 spec file
IDENTITY = "vendor.complex@v1"

DEPENDENCIES = {
  -- The bundle (for helpers)
  {
    bundle = "acme.toolchain-specs@v1",
    source = "git://github.com/acme/toolchain-specs",
    ref = "a1b2c3d4e5f6",
  },
  -- A spec from the same bundle (shorthand: bundle already declared above)
  {
    spec = "acme.gcc@v2",
    bundle = "acme.toolchain-specs@v1",
  },
}
```

**Bundle reference resolution (per-file scope):**

BUNDLES aliases are **per-file only**—each file (manifest or spec) has its own BUNDLES table. This prevents alias collisions and keeps files self-contained.

Both manifests and specs can declare BUNDLES tables:

```lua
-- In a spec file (with BUNDLES table)
BUNDLES = {
  ["tc"] = {identity = "acme.toolchain-specs@v1", source = "git://...", ref = "abc123"},
}
DEPENDENCIES = {
  {spec = "acme.gcc@v2", bundle = "tc"},    -- alias reference
  {spec = "acme.clang@v2", bundle = "tc"},  -- same alias, no repetition
}
```

Or inline bundle declarations in DEPENDENCIES (for single references):

```lua
-- In a spec file (inline bundle declaration)
DEPENDENCIES = {
  {bundle = "acme.toolchain-specs@v1", source = "git://...", ref = "abc123"},
  {spec = "acme.gcc@v2", bundle = "acme.toolchain-specs@v1"},  -- reference by identity
}
```

**Resolution order for `bundle = "string"`:**
1. Look in current file's BUNDLES table for alias match
2. Look in current file's DEPENDENCIES for bundle declaration with matching identity
3. Error if not found

**Aliases are ephemeral:** Resolved at parse time, then discarded. Multiple files can use different aliases for the same bundle identity—they all resolve to the same `bundle*` in the registry.

### C++ Object Model for BUNDLES

BUNDLES tables exist at the Lua level but are **not stored** in C++ structs. Aliases are resolved during parsing and discarded immediately.

**Parse-time flow:**
1. Load Lua file (manifest or spec)
2. Call `bundle::parse_aliases()` → returns `unordered_map<alias, pkg_cfg::bundle_source>`
3. Parse PACKAGES/DEPENDENCIES, resolving `bundle = "alias"` via map lookup
4. Create `pkg_cfg*` with resolved bundle identity and source
5. Map goes out of scope — only `pkg_cfg*` persists

**Runtime storage:**
```
pkg_cfg* (in pkg_cfg_pool)
├── identity: "acme.gcc@v2"
├── source: bundle_source {identity: "acme.toolchain@v1", fetch_source: git_source{...}}
├── bundle_identity: "acme.toolchain@v1"  (which bundle contains this spec)
└── bundle_path: "specs/gcc.lua"          (path within bundle)

bundle (simple struct, stored via unique_ptr in engine)
├── identity: "acme.toolchain@v1"
├── specs: {"acme.gcc@v2" -> "specs/gcc.lua", ...}
└── cache_path: "/home/user/.envy/specs/acme.toolchain@v1/"

engine.bundle_registry_
└── "acme.toolchain@v1" -> unique_ptr<bundle>
```

**Design decisions:**
- **No bundle_pool**: Unlike `pkg_cfg`, bundles are infrequently accessed (only during fetch). Engine stores them directly via `unordered_map<string, unique_ptr<bundle>>`.
- **bundle is a simple struct**: No `unmovable` base, no `ctor_tag` pattern. Created via `bundle::from_path()` static factory after fetching.
- **No opaque types for alias maps**: `bundle::parse_aliases()` returns a plain `unordered_map`. Standard container semantics, no custom wrapper needed.
- **bundle_decl is internal**: Implementation detail hidden in `bundle.cpp` anonymous namespace.

**Deduplication:** Multiple aliases (across different files) can resolve to the same bundle identity. The engine's `bundle_registry_` stores one `bundle` per identity, shared by all specs from that bundle.

**No BUNDLES storage in structs:**
- `manifest` struct: no BUNDLES member
- `pkg` struct: no BUNDLES member
- Aliases are purely a Lua-level convenience

```lua
FETCH = function(tmp_dir)
  local jfrog = envy.loadenv_spec("acme.toolchain-specs", "lib.jfrog")
  jfrog.fetch("com/vendor/complex.tar.gz", tmp_dir)
  envy.commit_fetch("complex.tar.gz")
end

BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  local gcc = envy.package("acme.gcc")
  envy.run(gcc .. "/bin/gcc -o complex main.c")
end
```

### Phase Validation

`envy.loadenv_spec()` validates:
1. Identity matches a declared dependency (fuzzy matching supported)
2. Current phase >= dependency's `needed_by` phase
3. Cannot be called at global scope (only in phase functions)

Same validation logic as `envy.package()`. Calling either at global scope (outside a phase function) is a fatal error because the phase context doesn't exist yet.

### Security Model

Bundle identity verification prevents a class of supply-chain attacks:

1. User manifest declares `bundle = {identity = "acme.toolchain-specs@v1", source = "...", ...}`
2. Envy fetches the bundle from the source
3. Envy parses `envy-bundle.lua` and extracts the `BUNDLE` field
4. If `BUNDLE != "acme.toolchain-specs@v1"`, envy aborts with an error

This ensures that even if a source URL is compromised, an attacker cannot substitute a different bundle without the manifest author explicitly updating the declared identity.

Similarly, spec identity verification ensures each file referenced in `SPECS` contains the expected `IDENTITY`:

```lua
-- envy-bundle.lua
SPECS = {
  ["acme.gcc@v2"] = "specs/gcc.lua",  -- specs/gcc.lua must declare IDENTITY = "acme.gcc@v2"
}
```

### Cache Structure for Bundles

Bundles live in `specs/` alongside atomic specs but are **not unpacked**. When a manifest requests a spec from a bundle, envy:

1. Fetches the bundle to `specs/<bundle-identity>/` (e.g., `specs/acme.toolchain-specs@v1/`)
2. Parses `envy-bundle.lua` to find the spec's path within the bundle
3. Loads the spec from `specs/<bundle-identity>/<path>` (e.g., `specs/acme.toolchain-specs@v1/specs/gcc.lua`)

There is **no separate cache entry** for bundled specs—`acme.gcc@v2` from bundle `acme.toolchain-specs@v1` is always loaded from within the bundle directory, not from `specs/acme.gcc@v2/`.

**Installed packages** from bundled specs go to the normal location: `assets/<identity>/<hash>/`. The bundle structure only affects spec resolution, not package installation.

### Bundle Integrity Validation

Envy validates bundle integrity after fetch:

1. **`envy-bundle.lua` exists** at bundle root
2. **`BUNDLE` field matches** the expected identity from manifest
3. **All files in `SPECS` exist** at their declared paths
4. **All spec files have matching `IDENTITY`** declarations

If any validation fails, envy reports an error including the bundle's cache path so users can investigate manually:

```
error: Bundle 'acme.toolchain-specs@v1' declares spec 'acme.gcc@v2' at 'specs/gcc.lua'
       but file not found in bundle at: /home/user/.envy/specs/acme.toolchain-specs@v1/specs/gcc.lua
```

### Revision Immutability

Bundle and spec revisions are **immutable**. If a bundle at the same revision has content changes (detected via SHA256 mismatch), that's an error—the author must bump the revision. This ensures envy doesn't need to re-fetch bundles/specs on every run "just in case" content changed.

---

## Implementation Tasks

### Phase 1: Data Structures & Parsing ✓

**Implementation:**

`src/bundle.h`, `src/bundle.cpp`:
- [x] Create `bundle` struct: simple struct with `identity`, `specs` map, `cache_path`
- [x] Add `bundle::from_path()` static factory to load and validate `envy-bundle.lua`
- [x] Add `bundle::validate()` to verify all spec files exist and IDENTITY matches keys (threaded)
- [x] Add `bundle::parse_aliases()` to parse BUNDLES table → `unordered_map<alias, bundle_source>`
- [x] Add `bundle::parse_inline()` to parse inline bundle declarations
- [x] Add `bundle::configure_package_path()` to configure existing lua state's package.path
- [x] Validation: BUNDLE field exists, SPECS table exists, all paths are relative

`src/pkg_cfg.h`, `src/pkg_cfg.cpp`:
- [x] Add `bundle_source` struct with `bundle_identity` and `fetch_source` variant
- [x] Add optional `bundle_identity` field for specs-from-bundles
- [x] Add optional `bundle_path` field (path within bundle to spec file)

`src/manifest.h`, `src/manifest.cpp`:
- [x] Add `BUNDLES` table parsing via `bundle::parse_aliases()`
- [x] BUNDLES table is ephemeral: parse into `unordered_map`, discard after parsing
- [x] Add `bundle` field parsing to package entries (alias reference or inline)
- [x] Resolution: when `bundle = "alias"`, look up in map; inline calls `bundle::parse_inline()`
- [x] No BUNDLES storage in manifest struct (aliases resolved at parse time)

**Tests:**

`src/bundle_tests.cpp`:
- [x] Unit tests for `bundle::parse_inline()` (remote, local, git sources)
- [x] Unit tests for `bundle::parse_aliases()` (valid table, nil, missing, errors)
- [x] Unit tests for error cases (missing identity, missing source, git without ref)
- [x] Unit tests for `bundle::from_path()` (valid bundle, missing manifest, missing BUNDLE field)
- [x] Unit tests for `bundle::resolve_spec_path()` (found, not found)
- [x] Unit tests for `bundle::validate()` (valid, missing file, IDENTITY mismatch, syntax error, parallel)

`test_data/bundles/`:
- [x] Create `simple-bundle/` with envy-bundle.lua and 2 specs
- [x] Create `invalid-bundle/` for testing missing BUNDLE field
- [x] Create `mismatched-identity/` for testing IDENTITY validation

### Phase 2: Fetch & Bundle Resolution ✓

**Implementation:**

`src/cache.h`, `src/cache.cpp`:
- No changes needed. Bundles cached at `specs/<bundle-identity>/` like atomic specs.

`src/phases/phase_spec_fetch.cpp`:
- [x] Detect `bundle_source` variant and fetch bundle (git clone, zip download, or local copy)
- [x] After fetch, call `bundle::from_path()` to load and validate `envy-bundle.lua`
- [x] Validate BUNDLE identity matches expected identity from pkg_cfg
- [x] Call `bundle::validate()` to verify all specs
- [x] For specs-from-bundles: resolve spec path using bundle's SPECS map

`src/engine.h`, `src/engine.cpp`:
- [x] Add `unordered_map<string, unique_ptr<bundle>> bundle_registry_` (guarded by `mutex_`)
- [x] Add `register_bundle()` method (check-then-insert, returns existing if present)
- [x] Add `find_bundle()` for lookup by identity

**Tests:**

`functional_tests/test_bundle_fetch.py`:
- [x] Test bundle fetching from local directory
- [x] Test bundle identity verification (mismatch = error)
- [x] Test SPECS -> IDENTITY validation (mismatch = error)
- [x] Test spec-from-bundle resolution
- [x] Test multiple specs from same bundle share one fetch
- [x] Test inline bundle declaration
- [x] Test unknown bundle alias error
- [x] Test cannot have both source and bundle

### Phase 3: Dependency Resolution ✓

**Implementation:**

`src/engine.cpp`:
- [x] Handle bundle dependencies: bundles are fetched but don't produce installed packages
- [x] Bundle fetching is atomic (whole bundle fetched together)
- [x] Multiple specs from same bundle share one fetch (bundle registry lookup)
- [x] Pure bundle dependencies (`{bundle = "...", source = "..."}`) only make bundle available for `envy.loadenv_spec()`
- [x] Spec-from-bundle dependencies (`{spec = "...", bundle = "..."}`) trigger full spec execution

`src/phases/phase_spec_fetch.cpp`:
- [x] Parse spec's BUNDLES table (if present) into temporary map
- [x] Use shared `parse_bundles_table()` utility
- [x] Pass bundles map to `parse_dependencies_table()`
- [x] No BUNDLES storage in pkg struct (aliases resolved at parse time)

`src/pkg_cfg.cpp`:
- [x] Parse `bundle` field in dependency declarations
- [x] Use shared `resolve_bundle_ref()` utility for resolution
- [x] Create pkg_cfg with appropriate fields: `bundle_identity`, `bundle_path` for spec-from-bundle

**Tests:**

`functional_tests/test_bundle_deps.py`:
- [x] Test manifest with multiple specs from same bundle
- [x] Test spec depending on a bundle directly
- [x] Test spec depending on a spec-from-bundle
- [x] Test spec depending on both bundle and spec-from-bundle
- [x] Test needed_by with bundle dependencies
- [x] Test spec with BUNDLES table (alias resolution)
- [x] Test multiple files using different aliases for same bundle identity
- [x] Test alias not found error
- [x] Test identity not found error (when no BUNDLES table)

### Phase 4: Lua API ✓

**Implementation:**

`src/lua_envy.cpp`:
- [x] Implement `envy.loadenv(path)` using Lua's `loadfile()` with custom `_ENV`:
  ```cpp
  // Pseudocode:
  // 1. Create new table for _ENV
  // 2. Set __index metamethod to point to _G for stdlib access
  // 3. loadfile(path) returns a chunk
  // 4. Set chunk's _ENV to our table
  // 5. Execute chunk
  // 6. Return the _ENV table (now contains assigned globals)
  ```
- [x] Path resolution: relative to the currently-executing file's directory

`src/lua_ctx/lua_envy_deps.cpp`:
- [x] Implement `envy.loadenv_spec(identity, subpath)`
- [x] Reuse dependency validation from `envy.package()` (same identity matching logic)
- [x] Locate spec/bundle cache directory via cache API
- [x] Construct full path: `cache_dir/subpath.lua`
- [x] Execute using same `loadfile()` + custom `_ENV` pattern as `envy.loadenv()`
- [x] Return resulting table
- [x] **Runtime verify**: error if called at global scope (check phase context is active)

`src/phases/phase_spec_fetch.cpp`, `src/bundle.cpp`:
- [x] Add bundle root to `package.path` when executing spec within bundle
- [x] Prepend `bundle_root/?.lua;bundle_root/?/init.lua` to `package.path`
- [x] Ensure specs in bundles can `require()` sibling files

**Tests:**

`functional_tests/test_loadenv_spec.py`:
- [x] Test `envy.loadenv_spec()` within phase functions
- [x] Test `envy.loadenv_spec()` at global scope (error)
- [x] Test `envy.loadenv_spec()` phase validation (needed_by)
- [x] Test fuzzy matching in `envy.loadenv_spec()` (matches `gcc` to `acme.gcc@v2`)
- [x] Test standard `require()` within bundle for local files

`functional_tests/test_loadenv.py`:
- [x] Test `envy.loadenv()` in manifest global scope
- [x] Test `envy.loadenv()` in spec global scope
- [x] Test `envy.loadenv()` in phase functions
- [x] Test `envy.loadenv()` sandboxing (globals captured in returned table)
- [x] Test `envy.loadenv()` path resolution (relative to current file)
- [x] Test `envy.extend()` with loaded manifest tables

### Phase 5: Documentation

**Implementation:**

`docs/architecture.md`:
- [x] Add Bundles section explaining concept and cache structure
- [x] Update manifest format documentation
- [x] Document bundle manifest format (`envy-bundle.lua`)

`docs/lua_api.md`:
- [x] Document `envy.loadenv_spec()`
- [x] Document `envy.loadenv()`
- [x] Add manifest composition examples
- [x] Add spec dependency examples

`src/resources/envy.lua` (lua_ls types):
- [x] Add type definitions for `envy.loadenv_spec()`
- [x] Add type definitions for `envy.loadenv()`

---

## Migration Notes

- Existing manifests continue to work (bundles are additive, not required)
- `envy.package()` already exists—no rename needed
- Replace "recipe" with "spec" in any remaining documentation

---

### Phase 6: Bundles as Packages (Unified Execution Model) ✓

Bundles are packages from the engine's perspective. They get their own `pkg` with execution context, can have custom fetch functions with dependencies, and follow a simplified lifecycle: `spec_fetch` → `complete`.

```
Regular package: spec_fetch → validate → fetch → build → install → deploy → complete
Bundle package:  spec_fetch → complete
```

**Extended Manifest Syntax:**

```lua
BUNDLES = {
  -- Simple case: remote/git/local source
  ["tools"] = {
    identity = "acme.tools@v1",
    source = "https://example.com/tools.zip",
    sha256 = "abc123..."
  },

  -- Custom fetch with dependencies
  ["private-tools"] = {
    identity = "acme.private@v1",
    source = {
      fetch = function(tmp_dir)
        -- custom fetch using jfrog cli
        envy.run(envy.product("jfrog") .. " rt dl ...")
        envy.commit_fetch({"envy-bundle.lua", "specs"})
      end,
      dependencies = {
        { spec = "jfrog.cli@v2", source = "jfrog.cli@v2.lua" }
      }
    }
  }
}
```

**Execution Flow:**

```
Bundle pkg created (with its own execution context)
  ↓
Dependencies resolve (e.g., jfrog.cli installs)
  ↓
spec_fetch phase runs:
  - If custom fetch: execute fetch function with phase context
  - Else: fetch via remote/git/local source
  - Parse envy-bundle.lua
  - Validate all specs (threaded)
  - Register bundle in engine
  ↓
Mark complete (skip validate/fetch/build/install/deploy)
```

**Implementation:**

`src/spec_util.h`, `src/spec_util.cpp`:
- [x] Add `extract_spec_identity()` helper: returns IDENTITY or throws
- [x] Throws on: parse error, missing IDENTITY, empty IDENTITY
- [x] Accepts optional `package_path_root` for bundle-local requires

`src/spec_util_tests.cpp`:
- [x] Test valid spec returns IDENTITY
- [x] Test missing IDENTITY throws
- [x] Test empty IDENTITY throws
- [x] Test parse error throws
- [x] Test bundle-local require works (package_path_root set)

`src/bundle.h`, `src/bundle.cpp`:
- [x] Rename `validate_integrity()` → `validate()`
- [x] Implement threaded validation: one `std::thread` per spec
- [x] Use `extract_spec_identity()` helper for each spec
- [x] Validate all bundles on every load (local and cached)

`src/bundle_tests.cpp`:
- [x] Update test names for `validate()`
- [x] Add IDENTITY mismatch detection test
- [x] Add spec parse error test

**Custom fetch bundles implementation:**

`src/pkg_cfg.h`, `src/pkg_cfg.cpp`:
- [x] Extend `bundle_source` to support `source = { fetch = ..., dependencies = ... }`
- [x] Add `custom_fetch_source` struct with dependencies vector

`src/bundle.cpp`:
- [x] Parse custom fetch function + dependencies in BUNDLES table
- [x] `parse_source_table_for_bundle()` handles `source = { fetch = ..., dependencies = ... }`
- [x] `decl_to_source()` converts custom_fetch_source to pkg_cfg variant

`src/manifest.h`, `src/manifest.cpp`:
- [x] Create `pkg_cfg` for bundles with custom fetch (added to packages list)
- [x] Wire dependencies with `source_dependencies` (resolved before spec_fetch)
- [x] Packages referencing custom fetch bundles get implicit dependency on bundle pkg
- [x] Add `lookup_bundle_fetch()` to find fetch function in BUNDLES table

`src/engine.h`, `src/engine.cpp`:
- [x] Bundle packages get their own execution context when they have custom fetch
- [x] Add optional manifest pointer for bundle fetch function lookup
- [x] Update constructor to accept manifest pointer

`src/phases/phase_spec_fetch.cpp`:
- [x] `fetch_bundle_only()` handles custom fetch with phase context
- [x] Support `envy.commit_fetch()` for bundle custom fetch
- [x] Rename `validate_integrity()` → `validate()` at 4 call sites

`CMakeLists.txt`:
- [x] Add `src/spec_util.cpp` to envy sources
- [x] Add `src/spec_util_tests.cpp` to unit test sources

**Tests:**

`functional_tests/test_bundle_custom_fetch.py`:
- [x] Test bundle with custom fetch function
- [x] Test bundle with custom fetch + dependencies
- [x] Test dependency resolution before bundle fetch
- [x] Test `envy.commit_fetch()` in bundle fetch context
- [x] Test bundle validation after custom fetch

---

## Open Items / Future Work

- [ ] `DESCRIPTION` field on specs for `envy describe` command
- [ ] Consider `envy describe <identity>` command to show spec/bundle metadata
- [ ] Incorporate SHA256 into spec/bundle cache directory names for stronger cache invalidation (see `docs/future-enhancements.md`)
