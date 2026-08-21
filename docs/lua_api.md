# envy.* Lua API Reference

Complete reference for the `envy.*` namespace available to spec scripts.

## Platform Constants

```lua
envy.PLATFORM      -- "darwin" | "linux" | "windows"
envy.ARCH          -- "arm64" | "x86_64"
envy.PLATFORM_ARCH -- e.g., "darwin-arm64", "linux-x86_64"
envy.EXE_EXT       -- "" on Unix, ".exe" on Windows
```

## Shell Constants

```lua
ENVY_SHELL.BASH       -- Unix only; error on Windows
ENVY_SHELL.SH         -- Unix only; error on Windows
ENVY_SHELL.CMD        -- Windows only; error on Unix
ENVY_SHELL.POWERSHELL -- Windows only; error on Unix
```

Platform-incompatible shells throw at runtime; all constants exist on all platforms.

---

## Logging

```lua
envy.debug(msg)  -- Decision narrative (shown with --verbose)
envy.info(msg)   -- Informational
envy.warn(msg)   -- Warning
envy.error(msg)  -- Error (does not throw)
envy.stdout(msg) -- Direct stdout (bypasses TUI)
```

---

## Command Execution

### envy.run(script, [opts]) → table

Execute shell script in phase context.

**Arguments:**
- `script` — string or array of strings (joined with newlines)
- `opts` — optional table:
  - `cwd` — working directory (default: phase's run_dir)
  - `env` — table of environment variable overrides
  - `shell` — `ENVY_SHELL.*` constant
  - `quiet` — suppress output (default: false)
  - `capture` — capture stdout/stderr (default: false)
  - `check` — throw on non-zero exit (default: true)
  - `interactive` — enable TTY passthrough (default: false)

**Returns:** `{ exit_code, stdout, stderr }`

```lua
-- Simple command
envy.run("make -j$(nproc)")

-- With options
local result = envy.run("./configure --prefix=" .. install_dir, {
  cwd = "subdir",
  env = { CC = "clang" },
  capture = true
})
print(result.stdout)

-- Multi-line script
envy.run({
  "cmake -B build -G Ninja",
  "cmake --build build",
  "cmake --install build"
})

-- Probe system state without throwing
local res = envy.run("dpkg -l foo", { capture = true, check = false })
if res.exit_code ~= 0 then error("foo not installed") end
```

**Default cwd by phase:**
- FETCH: tmp_dir
- STAGE/BUILD/INSTALL: stage_dir
- SETUP pair CHECK/INSTALL: project_root

**Backgrounded processes:** `envy.run` returns when the shell exits, not when its
descendants do; output a backgrounded grandchild writes after that point is dropped.
Redirect it to a file to keep it — `cmd >log 2>&1 &` (bash/sh), `start /b cmd >log 2>&1`
(cmd), `Start-Process -RedirectStandardOutput log` (powershell).

---

## File Operations

All relative paths resolve against phase's run_dir.

### envy.copy(src, dst)
Copy file or directory recursively; creates parent directories.

### envy.move(src, dst)
Move/rename file or directory.

### envy.remove(path)
Delete file or directory recursively.

### envy.exists(path) → bool
Check if path exists.

### envy.is_file(path) → bool
Check if path is regular file.

### envy.is_dir(path) → bool
Check if path is directory.

```lua
if not envy.exists("src") then
  envy.copy(fetch_dir .. "/upstream-src", "src")
end
envy.move("output/lib", install_dir .. "/lib")
```

---

## Archive Extraction

### envy.extract(archive_path, dest_dir, [opts]) → int

Extract single archive; returns file count.

**Options:**
- `strip` — components to strip from paths (default: 0)
- `only` — archive-relative paths or globs naming the sole entries to extract (omit for
  everything). A path takes that entry; a directory takes its whole subtree. Matched
  *after* `strip`. Unselected entries are never decompressed to disk. An entry matching
  nothing is an error, as are a malformed pattern and an empty list; a selected hard link
  needs its target selected too.

Glob syntax: `*` (any run) and `?` (one char) stay inside one component, `**` spans
components, `[a-z]`/`[!a-z]` are classes (`[*]`, `[?]`, `[[]` for literals). Case-sensitive
everywhere, so one spec behaves the same on every platform.

```lua
envy.extract(fetch_dir .. "/source.tar.gz", ".", { strip = 1 })
envy.extract(fetch_dir .. "/llvm.tar.xz", ".", { strip = 1, only = { "bin/clang-*" } })
```

### envy.extract_all(src_dir, dest_dir, [opts])

Extract all archives in directory; loose (non-archive) files are copied. Same options as
`envy.extract`—`only` spans the whole directory (matching loose files by filename), so one
entry per archive is enough to satisfy the list.

```lua
-- Take just the requested tools out of a 10 GB toolchain tarball; leave the rest packed.
local want = {}
for _, tool in ipairs(options.tools) do want[#want + 1] = "bin/" .. tool .. envy.EXE_EXT end
envy.extract_all(fetch_dir, stage_dir, { strip = 1, only = want })
```

A misspelled tool name fails here, at stage, instead of surfacing later as a missing file
in `INSTALL`—which is why an assembled `only` list beats a broad glob for option-driven
selections.

---

## Fetch Operations

### envy.fetch(source, opts) → string|table

Download files to destination.

**source formats:**
- `"https://example.com/file.tar.gz"` — URL string
- `{ source = "...", sha256 = "...", post_data = "..." }` — single spec with optional hash and POST body
- `{ { source = "..." }, { source = "..." } }` — array of specs

**spec fields:**
- `source` — required URL
- `sha256` — optional hash for verification
- `ref` — required for git URLs (branch, tag, or SHA)
- `post_data` — optional form-urlencoded body; triggers HTTP POST (HTTP/HTTPS only)
- `dest` — optional override for cache filename (use when URL lacks a recognizable filename)

**opts:**
- `dest` — required, destination directory

**Returns:** basename(s) of downloaded files.

```lua
-- Single file
local name = envy.fetch("https://example.com/file.tar.gz", { dest = tmp_dir })

-- With hash verification
envy.fetch({ source = url, sha256 = "abc123..." }, { dest = tmp_dir })

-- Multiple files (parallel download)
local files = envy.fetch({
  { source = url1, sha256 = hash1 },
  { source = url2, sha256 = hash2 }
}, { dest = tmp_dir })

-- POST request (for license-gated downloads)
envy.fetch({
  source = "https://example.com/download/file.pkg",
  sha256 = hash,
  post_data = "accept_license=yes"
}, { dest = tmp_dir })

-- Dest override (URL has no usable basename)
envy.fetch({
  source = "https://cipd.example.com/dl/pkg/+/git_revision:" .. ref,
  dest = "pkg.zip"
}, { dest = tmp_dir })
```

### envy.commit_fetch(files)

Move verified files from tmp_dir to fetch_dir. FETCH phase only.

**files formats:**
- `"filename"` — single file, no hash check
- `{ filename = "...", sha256 = "..." }` — single file with hash
- `{ "file1", "file2" }` — array of filenames
- `{ { filename = "...", sha256 = "..." }, ... }` — array with hashes

```lua
FETCH = function(tmp_dir, options)
  envy.fetch(url, { dest = tmp_dir })
  envy.commit_fetch({ filename = "file.tar.gz", sha256 = expected_hash })
end
```

### envy.verify_hash(file_path, expected_sha256) → bool

Verify file SHA256 hash; returns true if match, false otherwise.

```lua
if not envy.verify_hash(file, hash) then
  error("Hash mismatch!")
end
```

---

## Dependency Access

### envy.asset(identity) → string

Get installed asset path for declared dependency.

**Requirements:**
- Caller must have strong dependency on `identity`
- Access must occur at or after dependency's `needed_by` phase

```lua
BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  local gcc = envy.asset("arm.gcc@v2")
  envy.run("./configure --prefix=" .. install_dir .. " CC=" .. gcc .. "/bin/arm-none-eabi-gcc")
end
```

### envy.product(name) → string

Get a named product value from a provider you depend on. The name resolves either
from an explicit `product =` on a dependency entry or, failing that, from the
project-wide product registry — but a dependency edge is required either way, and
its `needed_by` must already have been reached. The edge is what drove the provider
through install; the registry only answers *who* provides the name. A provider you
reach only transitively is refused.

```lua
DEPENDENCIES = {
  { spec = "corp.python@v3", source = "python.lua", needed_by = "build" },
}
BUILD = function(...)
  local python = envy.product("python_path")  -- registry names the provider, edge allows it
  envy.run(python .. " setup.py build")
end
```

Works inside a `source.fetch` function too: `source.dependencies` entries are wired
with `needed_by = spec_fetch` before the fetch function runs, so their products are
readable there.

```lua
source = {
  dependencies = { { spec = "tools.jfrog-cli@r1", source = "jfrog.lua" } },
  fetch = function(tmp_dir)
    envy.run(envy.product("jf") .. " rt dl specs/ " .. tmp_dir)
  end,
}
```

A fetch dependency must be a strong reference to carry a product: the weak pass runs
only at a resolution barrier, after every spec_fetch, so `{ product = "jf" }` with no
`spec`/`source` could never be edged in time and is rejected at parse.

### envy.loadenv_spec(identity, module) → table

Load Lua file from declared dependency into sandboxed environment.

**Arguments:**
- `identity` — dependency identity (supports fuzzy matching: `"helpers"` matches `"acme.helpers@v1"`)
- `module` — module path using Lua dot syntax (e.g., `"lib.common"` → `lib/common.lua`)

**Returns:** Table containing all globals defined in the loaded file.

**Requirements:**
- Must be called within phase function (not global scope)
- Caller must have dependency on `identity` with appropriate `needed_by`
- Access must occur at or after dependency's `needed_by` phase

```lua
DEPENDENCIES = {
  {
    bundle = "acme.toolchain@v1",
    needed_by = "fetch",
  },
}

FETCH = function(tmp_dir, options)
  -- Fuzzy match: "toolchain" matches "acme.toolchain@v1"
  local helpers = envy.loadenv_spec("toolchain", "lib.common")
  local url = helpers.build_url(options.version)
  return { source = url }
end
```

**Error conditions:**
- Called at global scope → error (must be in phase function)
- Undeclared dependency → error
- `needed_by` phase not reached → error
- File not found → error
- Lua parse/execution error → error

### envy.loadenv(module) → table

Load Lua file relative to current file into sandboxed environment.

**Arguments:**
- `module` — module path using Lua dot syntax (e.g., `"lib.utils"` → `lib/utils.lua`)

**Returns:** Table containing all globals defined in the loaded file.

**Differences from `require()`:**
- Path is relative to current file (not cwd)
- Returns globals table (not return value)
- Always reloads (no caching)

```lua
-- helpers.lua (same directory as spec)
local helpers = envy.loadenv("helpers")
local version = helpers.DEFAULT_VERSION

-- lib/utils.lua (subdirectory)
local utils = envy.loadenv("lib.utils")
```

**When to use which:**
- `envy.loadenv(module)` — Load helpers from same project/bundle (relative to current file)
- `envy.loadenv_spec(identity, module)` — Load from declared dependency bundle (validates dependency graph)
- `require(module)` — Standard Lua module loading (uses package.path, cwd-relative)

---

## Path Utilities

### envy.path.join(...) → string
Join path components.

### envy.path.basename(path) → string
Extract filename with extension.

### envy.path.dirname(path) → string
Extract parent directory.

### envy.path.stem(path) → string
Extract filename without extension.

### envy.path.extension(path) → string
Extract file extension (with dot).

```lua
local p = envy.path.join(stage_dir, "bin", "tool" .. envy.EXE_EXT)
local ext = envy.path.extension("archive.tar.gz")  -- ".gz"
```

---

## Template Substitution

### envy.template(str, values) → string

Mustache-style `{{placeholder}}` substitution.

```lua
local cmd = envy.template(
  "./configure --prefix={{prefix}} CC={{cc}}",
  { prefix = install_dir, cc = "clang" }
)
```

---

## Thread-Local Context

The `envy.*` functions operate on thread-local phase context set by C++ via `phase_context_guard` RAII:

```cpp
phase_context_guard ctx_guard{ &engine, recipe, run_dir };
// Lua code called here can use envy.run(), envy.asset(), etc.
```

Context provides:
- Current spec pointer (for `envy.asset` dependency validation)
- Engine pointer (for TUI progress tracking)
- Run directory (default cwd for `envy.run` and file operations)

Functions requiring context throw if called outside phase execution.

---

## Migration from ctx.* (deprecated)

| Old API | New API |
|---------|---------|
| `ctx:run(cmd)` | `envy.run(cmd)` |
| `ctx:extract(...)` | `envy.extract(...)` |
| `ctx:copy(...)` | `envy.copy(...)` |
| `ctx:move(...)` | `envy.move(...)` |
| `ctx:asset(id)` | `envy.asset(id)` |
| `ctx:product(name)` | `envy.product(name)` |
| `ctx.stage_dir` | Use `stage_dir` function parameter |
| `ctx.fetch_dir` | Use `fetch_dir` function parameter |
| `ctx.install_dir` | Use `install_dir` function parameter |

The new API passes directories as function parameters instead of context fields:

```lua
-- Old
BUILD = function(ctx)
  ctx:run("make DESTDIR=" .. ctx.stage_dir)
end

-- New
BUILD = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.run("./configure --prefix=" .. install_dir)
  envy.run("make -j")
end
```
