# Future Enhancements

Potential enhancements not currently prioritized.

## Built-in Specs

Embed specs in the envy binary itself (e.g., `envy.*` namespace), extracted to cache on first run. Useful for bootstrapping common toolchains without external dependencies, but adds binary size and complexity. Needs clear use case before implementation.

## Spec Source Overrides

Allow manifests to override spec sources globally. Useful for pointing to mirrors, local forks, or air-gapped environments.

```lua
-- project/envy.lua
PACKAGES = { "vendor.gcc@v2" }

overrides = {
  ["vendor.gcc@v2"] = {
    url = "https://internal-mirror.company/gcc.lua",
    sha256 = "abc123...",
  },
  ["local.wrapper@v1"] = {
    file = "./envy/specs/wrapper.lua",  -- local override
  },
}
```

**Semantics:**
- Overrides apply to all spec references (manifest packages + transitive dependencies)
- Override specifies alternate source (url+sha256 or file); options remain from original cfg
- Applied before fetching/loading, affects cache key for remote specs
- Non-local specs cannot override to local sources (security boundary)

**Implementation notes:**
- `manifest` struct gains `std::unordered_map<std::string, recipe_override>` member
- `recipe_override = std::variant<recipe::cfg::remote_source, recipe::cfg::local_source>`
- Resolver consults override map when processing each `recipe::cfg`, substitutes source before fetch
- Parse override table during `manifest::load`, validate identity format

## Manifest Transform Hooks

Beyond declarative overrides, allow manifests to programmatically transform spec specifications. Provides maximum flexibility for complex scenarios.

```lua
-- project/envy.lua
function transform_recipe(spec)
  -- Redirect all specs to internal mirror
  if spec.url and spec.url:match("^https://example.com/") then
    spec.url = spec.url:gsub("^https://example.com/", "https://internal-mirror.company/")
  end

  -- Force specific version for security
  if spec.spec == "openssl.lib@v3" then
    spec.options = spec.options or {}
    spec.options.version = "3.0.12"  -- Known secure version
  end

  return spec
end

PACKAGES = { "openssl.lib@v3", "curl.tool@v2" }
```

**Considerations:** Hook executes during manifest validation. Applied to all spec specs (packages + transitive dependencies) before override resolution. Must be pure function (no side effects). Ordering: transform → override → validation.

## Spec Version Ranges

Support semver ranges for spec dependencies to reduce churn when spec bugs are fixed. Spec versions must be semver-compliant to enable ranges.

```lua
depends = { "vendor.library@^2.0.0" }  -- Any 2.x spec version
```

## Multi-File Specs from Git Repositories

Fetch multi-file specs directly from Git repos instead of requiring pre-packaged archives. Requires Git runtime dependency.

```lua
{ spec = "vendor.gcc@v2", git = "https://github.com/vendor/specs.git", ref = "v2.0" }
```

## S3 Region Selection

Nothing in envy selects an S3 region — not `envy fetch`, not spec `FETCH` URIs, not `mirror-envy`. Region comes from `AWS_REGION`/profile, and the SDK silently defaults to `us-east-1`, so a bucket elsewhere fails as a redirect rather than as "region not set". Put it in the URI so every fetch benefits, not just mirrors.

```lua
FETCH = { url = "s3://my-bucket/tool.tar.gz?region=eu-west-1" }
```

## Spec Mirroring and Offline Support

Configure alternate download locations for air-gapped environments. Similar to npm registry mirrors or Go module proxies.

```lua
recipe_mirrors = { ["https://public.com/specs/"] = "https://internal.corp/specs/" }
```

## Spec Deprecation Metadata

Mark specs as deprecated with migration guidance. Envy warns users and suggests replacement.

```lua
deprecated = { message = "Use arm.gcc@v2 instead", replacement = "arm.gcc@v2" }
```

## Declarative Build Systems

Support declarative table form for common build systems (cmake, make, meson, ninja, cargo, etc.) to reduce boilerplate in specs. Currently all builds use imperative functions or shell scripts.

**Current approach (imperative):**
```lua
BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  envy.run([[
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=]] .. install_dir .. [[
    cmake --build build --parallel
    cmake --install build
  ]])
end
```

**Proposed declarative form:**
```lua
BUILD = {
  cmake = {
    source_dir = ".",
    build_dir = "build",
    args = { "-DCMAKE_BUILD_TYPE=Release" },
    build_args = { "--parallel" },
    INSTALL = true,
  }
}
```

**Additional build system examples:**

```lua
-- Make-based build
BUILD = {
  make = {
    makefile = "Makefile",
    jobs = 4,
    targets = { "all", "install" },
    env = { CC = "gcc", CFLAGS = "-O2" },
  }
}

-- Meson + Ninja
BUILD = {
  meson = {
    args = { "--buildtype=release" },
    ninja = { jobs = 8 },
  }
}

-- Cargo (Rust)
BUILD = {
  cargo = {
    profile = "release",
    features = { "ssl", "compression" },
    target_dir = "target",
  }
}

-- Autotools
BUILD = {
  autotools = {
    configure_args = { "--prefix=" .. ctx.install_dir, "--enable-shared" },
    make_jobs = 4,
  }
}
```

**Implementation considerations:**
- Table form is syntactic sugar; translates to `ctx.run()` calls internally
- Supports common patterns while still allowing `build = function(ctx)` for complex cases
- Spec validation checks for valid build system keys and required fields
- Each build system has sensible defaults (e.g., `make.jobs` defaults to available cores)
- Build systems inject correct paths (install_dir, stage_dir) automatically
- Mixed forms not allowed: choose either table or function, not both

**Benefits:**
- Reduces spec boilerplate for standard build patterns
- Self-documenting: table keys make build configuration explicit
- Easier to validate and lint specs statically
- Common patterns standardized across specs

**Trade-offs:**
- Adds complexity to spec parsing and validation
- May not cover all edge cases (custom build systems, complex workflows)
- Escape hatch via function form still required for advanced builds

## Shell Execution: Async Pipe Reading and Stream Separation

### Current Implementation

`shell_run()` (both Windows and POSIX) uses synchronous pipe reading on the main thread:

1. Spawn child process with stdout+stderr redirected to single pipe
2. Close write end of pipe in parent
3. Read pipe synchronously with `stream_pipe_lines()` until EOF (child closes write end)
4. Wait for child process exit
5. Return exit code

**Key behaviors:**
- Parent actively drains pipe while child runs (no deadlock in normal cases)
- Pipe provides backpressure: child blocks on write if pipe buffer fills (typically 4-64KB)
- Parent's `on_output_line` callback is invoked synchronously for each line
- Both stdout and stderr merge into single pipe; no way to distinguish streams

### Limitations

**1. Slow callback stalls child**

If `on_output_line` callback blocks (e.g., synchronous disk I/O, network call), parent stops reading pipe. Child fills pipe buffer and blocks on write. Build effectively pauses until callback completes.

- **Real-world impact:** Low. Most callbacks just append to vector or print to terminal (fast).
- **Workaround:** Keep callbacks fast; defer expensive work to after `shell_run` returns.

**2. Pathologically long lines**

If child writes 100KB+ without newline, `stream_pipe_lines` accumulates entire line in memory before calling callback. Unbounded accumulation could exhaust memory.

- **Real-world impact:** Very low. Normal build output has reasonable line lengths (<10KB).
- **Example pathological case:** Binary dump to stdout, base64-encoded data without newlines.

**3. No stdout/stderr separation**

Both streams merge in pipe. Caller receives combined output in temporal order but cannot distinguish stdout from stderr.

- **Real-world impact:** Low. Most build tools merge streams anyway (cmake, ninja, make).
- **Use case:** Parsing structured stdout (JSON, XML) while ignoring stderr diagnostics.

### Potential Solutions

#### Option A: Background Reader Thread

Spawn thread to read pipe asynchronously while main thread waits for process:

```cpp
std::thread reader([&]() {
    stream_pipe_lines(read_end.get(), cfg.on_output_line);
});
shell_result result = wait_for_child(process);
reader.join();
```

**Pros:**
- Decouples pipe reading from process waiting
- Slow callbacks no longer stall child
- Simple threading model

**Cons:**
- Thread overhead (minor)
- Exception propagation complexity: callback throws on thread, must marshal to main
- Requires thread-safe callback or synchronization

#### Option B: Async I/O with Event Loop

Use platform-specific async I/O to multiplex pipe and process:

**Windows:** `OVERLAPPED` I/O with `WaitForMultipleObjects` on `{process_handle, pipe_event}`
**POSIX:** `select()`/`poll()` on pipe fd, `waitpid()` with `WNOHANG`

**Pros:**
- No threading complexity
- Can separate stdout/stderr with dual pipes + multiplexing
- Callback remains on main thread (easier exception handling)

**Cons:**
- Significant platform-specific complexity
- Event loop state machine harder to reason about
- Overkill for current usage patterns

#### Option C: Stdout/Stderr Separation

Requires dual pipes + either threading or async I/O:

**Threading approach:**
```cpp
std::thread stdout_reader([&]() { read_stdout_lines(cfg.on_stdout_line); });
std::thread stderr_reader([&]() { read_stderr_lines(cfg.on_stderr_line); });
wait_for_child(process);
stdout_reader.join();
stderr_reader.join();
```

**Trade-offs:**
- Loses temporal ordering: can't interleave stdout/stderr lines accurately
- Child can deadlock if one pipe fills while other is empty (needs careful reader design)
- Questionable value: most build tools intentionally merge streams

### Recommendation

**Defer until real-world need emerges.** Current implementation handles tested workloads correctly (all functional tests pass, including large outputs). The limitations are theoretical edge cases that haven't caused issues in practice.

**If implemented:**
- Option A (background thread) is simplest and handles slow callbacks
- Option C (stream separation) has unclear value proposition; most tools merge anyway
- Option B (async I/O) is over-engineered for current requirements

**Alternative:** Add configuration option for max line length with truncation warning if pathologically long lines become an issue.

## Dynamic Alias Computation

Allow spec files to compute aliases from options passed by manifest. String form is static; function form takes options table and returns alias string. Enables single spec to generate descriptive aliases like `python3.13` or `gcc-arm-13.2` based on version option.

```lua
-- local.python@r4.lua (spec file)
IDENTITY = "local.python@r4"

-- Static alias (current)
alias = "python"

-- Dynamic alias (proposed)
alias = function(options)
  return "python" .. options.version
end

-- Manifest usage:
-- { spec = "local.python@r4", source = "...", options = {version = "3.13"} }
-- Result: registered as alias "python3.13"
```

**Implementation:** Function evaluated during recipe_fetch phase after loading spec Lua. Takes `options` table from recipe_spec (not full ctx—aliases needed before asset phases). Cached per spec instance. String result must be unique (enforced at registration time).

## `.luarc.json` Merge Logic

`envy init` creates `.luarc.json` if absent but only prints guidance if it exists. Full merge would parse existing JSON, append envy types path to `workspace.library` array, preserve user settings, write back. Requires JSON library or minimal hand-rolled parser.

```json
{ "workspace.library": ["/path/to/cache/envy/0.1.0/envy.lua"] }
```

## SHA256-Based Spec Cache Directories

Incorporate content hash (SHA256) into spec/bundle cache directory names for stronger cache invalidation. Currently specs are keyed only by identity (`specs/<identity>/`). Content changes without revision bumps are caught at runtime but require user intervention.

```
# Current
specs/vendor.gcc@v2/

# Proposed
specs/vendor.gcc@v2-abc123def.../
```

**Trade-offs:**
- Pro: Automatic invalidation when content changes (no stale cache issues)
- Pro: Multiple versions of same identity can coexist during transition
- Con: SHA256 optional in envy (local specs, git refs without sha256 field)
- Con: Directory names become unwieldy; tooling/debugging harder
- Con: Cache bloat if content changes frequently without revision bumps

**Implementation note:** Would require computing content hash during fetch and incorporating into cache path. For git sources, could use commit SHA. For local sources, would need to hash file content.

## Hierarchical Platform Intersection

`util_platform_intersect` uses strict string equality, so `["linux"]` ∩ `["linux-x86_64"]` yields empty—treated as "matches nothing" via `kPlatformNone`. Should subsume OS-only constraints against OS-arch entries (mirroring `util_platform_matches` semantics): `"linux"` ∩ `"linux-x86_64"` → `"linux-x86_64"`.

```cpp
// Current: strict equality (broken for mixed granularity)
if (v == w) { result.push_back(v); }

// Fix: if either is a prefix of "os-arch", keep the more specific
if (v == w || w.starts_with(v + "-")) { result.push_back(w); }
else if (v.starts_with(w + "-")) { result.push_back(v); }
```

## Cross-Platform Spec Variants

Higher-level abstraction for platform-specific variants within a single spec identity. Current Lua approach handles this programmatically.

## Skip the Totals Pre-Scan for Selective Extraction

`extract_all_archives` header-scans every archive for progress denominators, which for a tar.xz means a full decompression pass — so a selective stage decompresses twice. With one archive in the fetch dir, its own selector check is already the union check, so the pre-scan could be skipped (percent-less progress, half the work).

```cpp
if (!selectors.empty() && archive_count == 1) { /* extract() validates; totals stay {0,0} */ }
```

## Range Resume for Interrupted Downloads

Retry restarts from byte zero, so a transport failure at 88% of a 1GB toolchain re-downloads 880MB. Resume from a `.part` sidecar via `Range:`, verifying sha256 on the assembled file; the traps are servers that answer 200 instead of 206 (append onto a partial and the file doubles) and per-request mirrors that redirect to different bytes, which `If-Range:` guards.

```cpp
setopt(CURLOPT_RESUME_FROM_LARGE, partial_bytes);  // 206 -> append; 200 -> truncate, restart
```

## Trace Coverage for Bootstrap/Bundle/AWS

Machinery trace events (`src/trace_events.def`) cover the scheduler, cache/lock, fetch, git-resolve, depot, and deploy paths but not `bootstrap.cpp`, `bundle.cpp`, or `aws_util.cpp`. Add events there if those subsystems need production diagnostics (e.g., `bundle_parsed`, `s3_request`).
