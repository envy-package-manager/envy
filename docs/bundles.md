# Package Bundles

## Overview

"Bundles" are distribution containers holding multiple specs plus shared helper files. Bundles enable spec authors to group related specs in a single git repo or zip while sharing common Lua code.

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
    needed_by = "fetch",  -- the access below is checked against this; build is too late
  },
}

FETCH = function(tmp_dir, options)
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

A custom-fetch `source = { fetch = ..., dependencies = ... }` is legal in every one of
these positions — a manifest or spec BUNDLES alias, an inline `bundle = {...}` table in
PACKAGES or DEPENDENCIES, and a pure bundle dependency — and one lookup finds it in all
of them. The function runs in the declaring file's Lua state as the *bundle* package:
the bundle's own `source.dependencies` are what `envy.package`/`envy.product` authorize
against. `envy.commit_fetch` must produce `envy-bundle.lua`; everything else it commits
becomes the bundle directory alongside it.

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
pkg_cfg* (in the process-wide pkg_cfg pool; see docs/pkg_cfg_ownership.md)
├── identity: "acme.gcc@v2"
├── source: bundle_source {bundle_identity: "acme.toolchain@v1", fetch_source: git_source{...}}
├── bundle_identity: "acme.toolchain@v1"  (which bundle contains this spec)
└── source_dependencies: [ the bundle's own BUNDLE_ONLY cfg ]

bundle (simple struct, stored via unique_ptr in engine)
├── identity: "acme.toolchain@v1"
├── specs: {"acme.gcc@v2" -> "specs/gcc.lua", ...}   (the path within the bundle)
└── cache_path: ".../specs/acme.toolchain@v1/blake3-{source_hash}/pkg"

engine.bundle_registry_
└── "acme.toolchain@v1" -> unique_ptr<bundle>
```

The path within the bundle lives only in `bundle::specs`, read through
`bundle::resolve_spec_path` at `spec_fetch`; a cfg never carries one.

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
-- with `needed_by = "fetch"` on the bundle dependency above
FETCH = function(tmp_dir, options)
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

### Bundles as Packages

Bundles are packages from the engine's perspective. They get their own `pkg` with execution context, can have custom fetch functions with dependencies, and follow a simplified lifecycle: `spec_fetch` → `complete`.

```
Regular package: spec_fetch → check → import → fetch → stage → build → install → setup → export → complete
Bundle package:  spec_fetch → complete   (the step reports finished-early; its watermark
                                          jumps to done, so dependents' edges clear)
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
Mark complete (the payload phases never run)
```
