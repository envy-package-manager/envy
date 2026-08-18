# envy init - Bootstrap Loader Design

## Philosophy

Envy's core promise: **100% project-localized tooling with zero system installation.** Users clone a repo and run `./envy sync`—no homebrew, no apt, no manual downloads. The bootstrap loader is the final piece of this puzzle: envy itself requires no installation.

The bootstrap scripts (`envy` for Unix, `envy.bat` for Windows) each:
1. Read the pinned envy version from the manifest (`@envy version`)
2. If missing, fall back to the version stamped into the script (with warning)
3. Check if that version exists in the user's cache
4. Download if missing (over HTTPS from GitHub or configured mirror)
5. Attest the archive against `SHA256SUMS`, if the manifest pins one
6. Execute the cached binary with all arguments

**Version stamping:** `envy init` stamps its own version in two places:
- The manifest (`-- @envy version "1.2.3"`) — primary source of truth
- The bootstrap scripts (`FALLBACK_VERSION="1.2.3"`) — recovery if directive deleted

The version is the *only* project value a script carries. The mirror and the sums pin are read from the manifest at run time, never stamped—see **Mirror precedence** below.

**Fast path:** If envy is cached, the bootstrap adds ~0ms overhead—it's just `exec`.

**Self-deployment:** Envy handles its own cache installation. The bootstrap script downloads to a temp location and executes from there. On startup, envy checks if the expected cache location exists; if not, it copies itself there and extracts types alongside. This keeps bootstrap scripts simple (no `mkdir`, no knowledge of cache internals) and makes envy the single source of truth for cache structure.

**Concurrent installation:** Multiple envy instances may attempt self-deployment simultaneously (e.g., parallel CI jobs, multiple terminals). Envy uses the same cache locking strategy as recipe/asset installation: acquire exclusive lock on `$CACHE/locks/envy.$VERSION.lock`, check if already deployed (another process may have finished), deploy if still missing, release lock. Lock-free fast path: if `$CACHE/envy/$VERSION/envy` exists, skip locking entirely.

---

## Command Signature

```
envy init <project-dir> <bin-dir> [--mirror=URL] [--pin-sums]
```

- `project-dir`: Where manifest (`envy.lua`) and IDE config (`.luarc.json`) live
- `bin-dir`: Where bootstrap script (`envy`) lives
- `--mirror`: Override default GitHub releases URL (for enterprise/air-gapped environments)
- `--pin-sums`: Fetch this release's `SHA256SUMS` and pin its hash, so bootstrap attests every envy binary it downloads. Needs network; fails before writing anything

---

## Files Created

| File | Location | Checked In | Purpose |
|------|----------|------------|---------|
| `envy` | bin-dir | Yes | Unix bootstrap script (bash) |
| `envy.bat` | bin-dir | Yes | Windows bootstrap script (batch) |
| `envy.lua` | project-dir | Yes | Manifest with `@envy` directives |
| `.luarc.json` | project-dir | Yes | lua_ls IDE config (merged if exists) |

---

## @envy Directives

Bootstrap metadata lives in specially-formatted comments at the top of the manifest. This ensures:
- **Formatter-resistant**: Lua code formatters don't touch comments
- **Visually distinct**: Users immediately see these are "special"
- **Grep-friendly**: Simple regex parsing in bootstrap script

### Syntax

```lua
-- @envy <key> "<value>"
```

All values are quoted. Escaping is supported:
- `\"` → literal `"`
- `\\` → literal `\`

### Available Directives

| Directive | Required | Description |
|-----------|----------|-------------|
| `version` | Yes* | Pinned envy version (semver) |
| `cache-posix` | Optional | Override cache location (macOS/Linux) |
| `cache-win` | Optional | Override cache location (Windows) |
| `mirror` | Optional | Override download mirror: `https://…` or `s3://bucket/prefix` |
| `sha256sums` | Optional | 64 hex digits: sha256 of the release's `SHA256SUMS`. Attests every downloaded archive; requires `version` |

*If `version` is missing, bootstrap resolves it from the mirror's `latest` file (written by `envy mirror-envy`), then—for non-s3 mirrors only—from the **end** of GitHub's latest-release redirect chain, then from the version stamped when `envy init` created the scripts. A repo rename or org transfer inserts a hop whose own trailing segment is still `latest`, so only the chain's end names the tag. Either network tier is discarded with a warning unless it yields `MAJOR.MINOR.PATCH`; a `vlatest/` download URL merely 404s, which a mirror bucket without `s3:ListBucket` masks as a 403.

**Mirror precedence** is `ENVY_MIRROR` env > `@envy mirror` > envy upstream, identically in the bootstrap scripts and in the running binary. Trailing slashes are stripped: for `s3://` a doubled slash is a distinct, nonexistent key.

The scripts carry **no project configuration**—only envy's own upstream URLs are stamped, so a given envy version emits byte-identical scripts for every project. `--mirror` writes `@envy mirror` to the manifest and nothing else. A stamped copy would be unreachable (the parsed directive outranks it) except when the directive is deleted, and then it resolved the script to the stale mirror while the re-exec'd binary went to upstream—two binaries for one project.

**`s3://` mirrors** require AWS CLI v2 on `PATH`—the bootstrap shells out to `aws s3 cp`, because curl cannot sign requests and no envy binary exists yet. Region and credentials resolve from the environment/profile exactly as for every other AWS tool; run `aws sso login` first for a credential-gated bucket. Note a stale exported `AWS_ACCESS_KEY_ID` silently beats a fresh SSO session. Prefer `https://` for public distribution to keep bootstrap dependency-free. Bucket names must use hyphens, not underscores.

**Platform-specific cache:** Use `cache-posix` for Unix-style paths (`~/...`, `$HOME/...`) and `cache-win` for Windows paths (`%LOCALAPPDATA%\...`). Each platform's bootstrap script and envy binary only parse their respective directive.

### Parsing Regex

```
^--\s*@envy\s+(\w+)\s+"((?:[^"\\]|\\.)*)"
```

After extraction, unescape the value:
- Replace `\"` with `"`
- Replace `\\` with `\`

### Dual Parsing Requirement

Both the bootstrap scripts and the envy runtime must parse `@envy` directives:

| Parser | When | Why |
|--------|------|-----|
| Bootstrap scripts | Before envy exists locally | Need `version` to know which binary to download; `cache` and `mirror` to know where/how; `sha256sums` to attest it |
| Envy runtime | After bootstrap, during execution | Need `cache` for type extraction path; `version` for cache key; `mirror` and `sha256sums` for re-exec |

The parsing logic is intentionally simple—a per-line regex—so bash, batch, the shell hooks and C++ all implement it identically. Every reader bounds the scan by the header itself rather than by a line count: blank lines and comments, stopping at the manifest's first line of code. A line cap would cut both ways—dropping a directive under a long preamble, and honoring a directive-shaped comment inside the package table—and either way the launcher would resolve a version or mirror the binary it execs ignores. Phase 1 delivers both implementations and validates they produce identical results for all test cases.

---

## Sample Manifest

```lua
-- envy.lua - Project manifest
-- @envy version "1.2.3"
-- @envy cache-posix "/opt/shared-envy-cache"
-- @envy cache-win "D:\envy-cache"
-- @envy mirror "https://internal.corp/envy-releases"

PACKAGES = {
    "local.python@v1",
    {
        spec = "arm.gcc@v2",
        source = "https://specs.example.com/gcc.lua",
        options = { version = "13.2.0" },
    },
}
```

Minimal manifest (only required directive):
```lua
-- @envy version "1.2.3"

PACKAGES = {
    "local.python@v1",
}
```

---

## Cache Location

### Platform Defaults

| Platform | Default Cache Root |
|----------|-------------------|
| macOS | `~/Library/Caches/envy` |
| Linux | `${XDG_CACHE_HOME:-~/.cache}/envy` |
| Windows | `%LOCALAPPDATA%\envy` |

### Override Precedence (highest to lowest)

1. `--cache-root` command-line argument
2. `ENVY_CACHE_ROOT` environment variable
3. `@envy cache-posix` / `@envy cache-win` directive in manifest
4. Platform default

Every tier resolves to an absolute path. A relative directive anchors to the **manifest's directory**; a relative flag or env value anchors to the cwd that supplied it. Bootstrap scripts and runtime agree, so `-- @envy cache-posix "out/.envy"` names `<manifest dir>/out/.envy` from any working directory—otherwise each subdirectory would refetch the whole package set into a tree of its own, and the relative root would leak into `envy product` output and into phase `stage_dir`/`install_dir`.

### Cache Structure

```
$CACHE_ROOT/
├── envy/
│   ├── 1.2.3/
│   │   ├── envy                    # binary (or envy.exe on Windows)
│   │   └── envy.lua                # lua_ls type definitions
│   └── 1.4.0/
│       ├── envy
│       └── envy.lua
├── specs/
│   └── ...                         # cached spec files (existing)
└── assets/
    └── ...                         # cached package artifacts (existing)
```

Each envy version is self-contained—binary and types live together. Multiple versions coexist; different projects can pin different versions. Rollback is a one-line manifest change. Deleting a version removes one directory.

### Cache Key

Internally, cached envy binaries use BLAKE3 hashing:

```
Cache key: BLAKE3("envy-bootstrap{version=1.2.3,platform=darwin,arch=arm64}")
```

---

## Bootstrap Scripts

Two platform-specific scripts—no polyglot tricks. Each is idiomatic for its platform, easier to read/debug, and the manifest `@envy version` directive is the single source of truth.

`envy init` creates both scripts; cross-platform projects commit both.

**Key design:** Bootstrap scripts don't create cache structure. They check if `$CACHE/envy/$VERSION/envy` exists; if so, exec it. If not, download to a temp location and exec from there. The envy binary self-deploys to the cache on startup. This keeps bootstrap scripts simple and makes envy the single authority on cache layout.

### Unix Script (`envy`)

Location: `src/resources/envy`

Bash script optimized for minimal subprocess overhead. Parses all directives in a single `head | sed` pipeline (2 subprocesses) with bash string replacement for unescaping. Downloads via `curl`.

### Windows Script (`envy.bat`)

Location: `src/resources/envy.bat`

Pure batch implementation for directive parsing—avoids ~100-200ms PowerShell startup overhead on every invocation. Downloads with native `curl.exe` and extracts the `.zip` with native `tar.exe` (bsdtar); both ship in `System32` on Windows 10 1803+ and are immune to PowerShell policy. PowerShell `Invoke-WebRequest`/`Expand-Archive` are kept only as a fallback for older Windows—avoided on the primary path because `Expand-Archive`'s module (`Microsoft.PowerShell.Archive`) can be blocked by machine policy (WDAC/AppLocker constrained-language mode) even while compiled binaries still run.

**Limitation:** Values containing `!` or `\"` escape sequences are not supported. Batch's delayed expansion interprets `!` as variable delimiters; `\"` escapes are difficult to process correctly. These edge cases are rare (version strings, paths, and URLs typically don't contain these characters).

### Security Model

Trust hierarchy:
1. HTTPS to envy's GitHub releases (default, trusted)
2. HTTPS to a configured mirror (whoever operates it is responsible for securing it)
3. `s3://` to a configured mirror — SigV4-signed by the AWS CLI, so the bucket's IAM policy is the trust boundary

A mirror moves the trust boundary to whoever ran `envy mirror-envy`: it copies release bytes verbatim, so a wrong or compromised `--from` propagates silently. Restrict write access to the mirror prefix accordingly.

### Attestation (`@envy sha256sums`)

Optional. Manifest pins the hash of the release's `SHA256SUMS`; that file names the archive's hash; the archive is what executes. Pin from `envy init --pin-sums`, refresh with `envy use <version>`, or copy what `mirror-envy` prints.

```
-- @envy version "1.2.3"
-- @envy sha256sums "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
```

- **One pin, six platforms** — the indirection exists because a cross-platform manifest cannot carry six archive hashes.
- **`mirror-envy` copies `SHA256SUMS` byte-for-byte**, never regenerates it: that is what keeps a pin valid against upstream *and* every mirror downstream. It also attests all six archives before publishing, so a mirror cannot launder corrupt bytes.
- **Buys:** git becomes the root of trust for the envy binary — defeating a tampered mirror, especially `s3://`, plain-`http`, or insider write access to a corporate prefix. Not a compromised upstream release or manifest; the scripts are checked in beside `envy.lua`, so git already was.
- **Fail-closed:** a pin requires `@envy version` (it names one release, so dynamic resolution would describe a different one), and a pinned manifest whose mirror lacks `SHA256SUMS` errors rather than silently downgrading.
- **Verified before extract** — an unattested archive is never unpacked, so a hostile mirror never chooses paths under the temp dir.
- **Not covered:** the cache fast path re-`exec`s without re-hashing. Download-time check, not per-invocation; whoever can write your cache can write your `bin/envy`.
- **No extra software:** `sha256sum`/`shasum`/`openssl` on POSIX; `certutil` on Windows, with `Get-FileHash` as fallback since certutil is a known LOLBin some environments block.

Future option: code signing for authenticity independent of the manifest.

---

## Workflows

### Workflow 1: Single-User Project Setup

Alice creates a new project with envy:

```bash
# 1. Download envy somehow (one-time, any method)
curl -fsSL -o /tmp/envy https://github.com/envy-package-manager/envy/releases/latest/download/envy-darwin-arm64
chmod +x /tmp/envy

# 2. Initialize project
mkdir my-project && cd my-project
/tmp/envy init . ./tools

# Creates:
#   ./envy.lua          - manifest with @envy version "x.y.z"
#   ./tools/envy        - bootstrap script
#   ./.luarc.json       - IDE config

# 3. From now on, use the bootstrap script
./tools/envy sync       # downloads envy to cache if needed, runs sync
./tools/envy product gn # query products

# 4. Delete the temp binary
rm /tmp/envy
```

**Result:** Alice never "installs" envy. The bootstrap script handles everything.

---

### Workflow 2: Joining an Existing Project

Bob clones Alice's project:

```bash
# 1. Clone
git clone https://github.com/alice/my-project
cd my-project

# 2. Just run envy
./tools/envy sync

# What happens:
#   - Bootstrap reads @envy version from envy.lua
#   - Checks ~/Library/Caches/envy/envy/1.2.3/envy
#   - Not found → downloads from GitHub releases
#   - Caches binary
#   - Executes: envy sync

# 3. IDE setup (one-time)
#   - .luarc.json already exists in repo
#   - If cache path differs, run: ./tools/envy init . ./tools
#   - This updates .luarc.json with correct local cache path
```

**Result:** Bob clones and runs. Zero manual installation.

---

### Workflow 3: Changing envy Version

Alice moves from 1.2.3 to 1.4.0:

```bash
# 1. Retarget the manifest. Rewrites @envy version and, if the project is attested,
#    refreshes @envy sha256sums from 1.4.0's release -- the pin a hand-edit leaves stale,
#    which then fails closed on every machine.
./tools/envy use 1.4.0
# envy.lua: @envy version "1.2.3" -> "1.4.0"
# envy.lua: @envy sha256sums "9f86d081..." -> "a3bf4f1b..."

# One call per manifest carrying the directives; a subproject pins its own version.
(cd subproject && ../tools/envy use 1.4.0 --subproject)

# 2. Run any envy command
./tools/envy sync

# What happens:
#   - Bootstrap reads @envy version "1.4.0"
#   - Checks cache for 1.4.0 → not found
#   - Downloads envy 1.4.0, attests it against the refreshed pin
#   - Caches at ~/Library/Caches/envy/envy/1.4.0/envy
#   - Old 1.2.3 remains in cache (no deletion)
#   - Executes: envy sync

# 3. Commit the change
git add envy.lua
git commit -m "chore: move envy to 1.4.0"
```

**Result:** Version change is one command per manifest, pin included. Direction-neutral—`envy use 1.2.3` walks it back. Old version remains cached (safe rollback).

`envy use` reads the manifest header rather than loading the manifest, so it never re-execs. That is what lets it repair a manifest already naming a version whose pin is stale: the pinned binary cannot be downloaded, so no other command can run at all.

---

### Workflow 4: Pinning to Specific Version

For reproducible CI builds, pin exact version:

```lua
-- envy.lua
-- @envy version "1.2.3"
```

CI pipeline:
```yaml
# .github/workflows/build.yml
jobs:
  build:
    steps:
      - uses: actions/checkout@v4
      - run: ./tools/envy sync
      # envy 1.2.3 downloaded (or cache hit from previous run)
      # deterministic across all CI runs
```

**Result:** Same envy version across all developers and CI. Reproducible builds.

---

### Workflow 5: Enterprise/Air-Gapped Environment

Corporate environment without GitHub access. `envy mirror-envy` stages or uploads the whole
release matrix in the exact layout bootstrap expects:

```bash
# 1. Stage all six platform archives plus a `latest` file, then move the bytes yourself
envy mirror-envy 1.2.3 ./staged
aws s3 cp --recursive ./staged s3://my-envy-mirror/releases

# ...or upload directly (requires credentials; envy never creates the bucket)
envy mirror-envy 1.2.3 s3://my-envy-mirror/releases

# Re-mirroring from an existing mirror instead of GitHub:
envy mirror-envy 1.2.3 ./staged --from=https://internal.corp/envy-releases

# 2. Point the project at it (mirror-envy prints these three lines for you)
#    -- @envy version "1.2.3"
#    -- @envy mirror "s3://my-envy-mirror/releases"
#    -- @envy sha256sums "9f86d081..."   # attests every download against the mirror
envy init . ./tools --mirror=s3://my-envy-mirror/releases --pin-sums

# 3. Developers clone and run normally
aws sso login              # only for a credential-gated bucket
./tools/envy sync          # downloads from the internal mirror
```

Pin `@envy version`, or publish the `latest` file `mirror-envy` writes — otherwise a
version-less bootstrap against an `https://` mirror still falls back to GitHub's redirect.

Layout, if you populate a mirror by hand (names must match byte-for-byte):

```
<mirror>/latest                              # bare version string, no newline
<mirror>/v1.2.3/envy-darwin-arm64.tar.gz     # archive root: envy
<mirror>/v1.2.3/envy-darwin-x86_64.tar.gz
<mirror>/v1.2.3/envy-linux-arm64.tar.gz
<mirror>/v1.2.3/envy-linux-x86_64.tar.gz
<mirror>/v1.2.3/envy-windows-arm64.zip       # archive root: envy.exe
<mirror>/v1.2.3/envy-windows-x86_64.zip
<mirror>/v1.2.3/SHA256SUMS                    # only if pinning; copy upstream's verbatim
```

**Result:** Works behind corporate firewall. IT controls distribution.

---

### Workflow 6: Custom Cache Location

For CI with shared cache or custom paths:

```lua
-- envy.lua
-- @envy version "1.2.3"
-- @envy cache-posix "/opt/envy-cache"
```

Or via environment:
```bash
export ENVY_CACHE_ROOT=/opt/envy-cache
./tools/envy sync
```

Or via command line:
```bash
./tools/envy --cache-root=/opt/envy-cache sync
```

**Result:** Full control over cache location for CI optimization.

---

### Workflow 7: lua_ls IDE Setup

#### VS Code

1. Install [Lua extension](https://marketplace.visualstudio.com/items?itemName=sumneko.lua) by sumneko

2. `.luarc.json` is already in your project (created by `envy init`):
```json
{
  "$schema": "https://raw.githubusercontent.com/LuaLS/vscode-lua/master/setting/schema.json",
  "runtime.version": "Lua 5.4",
  "workspace.library": [
    "/Users/alice/Library/Caches/envy/envy/1.2.3"
  ],
  "diagnostics.globals": [
    "envy", "IDENTITY", "PACKAGES", "DEPENDENCIES", "PRODUCTS",
    "FETCH", "STAGE", "BUILD", "INSTALL", "SETUP"
  ]
}
```

3. Open project in VS Code → lua_ls activates → full autocomplete for `envy.*`

#### Neovim (with nvim-lspconfig)

```lua
-- init.lua or lua/plugins/lsp.lua
require('lspconfig').lua_ls.setup {
  settings = {
    Lua = {
      -- .luarc.json in project root is auto-detected
    }
  }
}
```

#### Updating .luarc.json After Clone

If you clone on a different machine, the cache path in `.luarc.json` may be wrong:

```bash
# Re-run init to update .luarc.json with local cache path
./tools/envy init . ./tools

# This merges your existing .luarc.json, only updating:
#   - workspace.library (cache path)
#   - diagnostics.globals (if new globals added)
# Your custom settings are preserved.
```

#### Making .luarc.json Portable

Option A: Use environment variable (requires shell setup)
```json
{
  "workspace.library": ["${ENVY_CACHE_ROOT}/envy/1.2.3"]
}
```
Set `export ENVY_CACHE_ROOT=~/.cache/envy` in your shell profile.

Option B: Accept per-machine paths (simpler)
- Each developer runs `./tools/envy init . ./tools` once after clone
- `.luarc.json` has correct local path
- Small git churn (acceptable)

---

## .luarc.json Merge Behavior

`envy init` is **non-destructive**. It merges envy's requirements with existing settings:

**Before (user's custom config):**
```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": ["./my-lua-libs"],
  "diagnostics.severity": { "lowercase-global": "Warning" }
}
```

**After `envy init . ./tools`:**
```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": [
    "./my-lua-libs",
    "/Users/alice/Library/Caches/envy/envy/1.2.3"
  ],
  "diagnostics.severity": { "lowercase-global": "Warning" },
  "diagnostics.globals": [
    "envy", "IDENTITY", "PACKAGES", "DEPENDENCIES", "PRODUCTS",
    "FETCH", "STAGE", "BUILD", "INSTALL", "SETUP"
  ]
}
```

**Merge rules:**
- `workspace.library`: append envy types path if not present
- `diagnostics.globals`: append envy globals if not present
- All other settings: preserve user's values

---

## Type Definitions

### Extraction

envy binary embeds lua_ls type definitions. On any envy command:

1. Check `$CACHE/envy/$VERSION/envy.lua` exists (types live alongside binary)
2. If missing: write embedded types to same directory as binary
3. Update `.luarc.json` if cache path changed

Types and binary are co-located so each version is self-contained. Deleting a version directory removes everything for that version.

### Content (~200 lines)

```lua
---@meta

-- Platform constants
---@class envy
---@field PLATFORM "darwin"|"linux"|"windows"
---@field ARCH "arm64"|"x86_64"
---@field PLATFORM_ARCH string
---@field EXE_EXT string
envy = {}

---@class envy.path
envy.path = {}

---Join path components with platform-appropriate separator
---@param ... string
---@return string
function envy.path.join(...) end

-- ... (complete API coverage)

-- Phase function signatures
---@param tmp_dir string
function FETCH(tmp_dir) end

---@param fetch_dir string
---@param stage_dir string
---@param tmp_dir string
function STAGE(fetch_dir, stage_dir, tmp_dir) end

-- ... etc
```

---

## What Happens: Step by Step

### `envy init . ./tools`

1. **Validate arguments**
   - project-dir exists (or create)
   - bin-dir exists (or create)

2. **Write bootstrap scripts** to `./tools/`
   - `envy` (bash) and `envy.bat` (batch)
   - Stamp `FALLBACK_VERSION` with running envy's version
   - Stamp `ENVY_MIRROR` if `--mirror` provided
   - Set executable bit on `envy` (Unix)

3. **Write manifest template** to `./envy.lua`
   - Skip if already exists
   - Stamp `-- @envy version "X.Y.Z"` with running envy's version

4. **Extract type definitions** to `$CACHE/envy/$VERSION/`
   - Write embedded lua types
   - Write `envy.lua` alongside binary

5. **Write/merge .luarc.json** to `./`
   - If doesn't exist: create with envy settings
   - If exists: parse JSON, merge arrays, preserve other settings
   - Point `workspace.library` to `$CACHE/envy/$VERSION/`

### `./tools/envy sync` (cached)

1. **Bootstrap script executes**
2. **Parse `@envy version`** from `envy.lua`'s header—blank lines and comments up to the first line of code—and unescape
3. **Parse `@envy cache-posix`/`cache-win`** (optional, platform-specific)
4. **Resolve cache dir** (env > manifest > default)
5. **Check cache** → `$CACHE/envy/1.2.3/envy` exists
6. **exec** → replace shell with envy binary, pass all args
7. **envy sync runs** → normal package synchronization

Total overhead: ~1-2ms (grep + sed unescape + exec)

### `./tools/envy sync` (not cached)

1. **Bootstrap script executes**
2. **Parse `@envy version`** → "1.2.3"
3. **Resolve cache dir** → `~/Library/Caches/envy`
4. **Check cache** → `$CACHE/envy/1.2.3/envy` not found
5. **Determine platform** → darwin-arm64
6. **Download to temp** → `curl https://github.com/.../envy-darwin-arm64` → `/tmp/envy-1.2.3-$$`
7. **exec** → temp binary (bootstrap's job is done)
8. **envy self-deploys** → copies self to `$CACHE/envy/1.2.3/envy`, extracts types alongside
9. **envy sync runs** → normal package synchronization

Next bootstrap run finds the cached version and skips download.

### `./tools/envy sync` (version directive missing)

Recovery flow when user accidentally deletes `@envy version` from manifest:

1. **Bootstrap script executes**
2. **Parse `@envy version`** → not found
3. **Fall back to stamped version** → `FALLBACK_VERSION="1.2.3"`
4. **Emit warning** → `WARNING: @envy version not found in envy.lua, using fallback 1.2.3`
5. **Continue normally** → check cache, download if needed, exec
6. **envy runs, detects missing directive** → restores `-- @envy version "1.2.3"` to manifest
7. **Next run is clean** → no warning, reads version from manifest

The fallback version is whatever envy created this bootstrap script—a reasonable default since it's the version the project was initialized with.

---

## Implementation Tasks

### Phase 1: Bootstrap Scripts and Directive Parsing

Phase 1 delivers complete `@envy` directive parsing in both bootstrap scripts and the envy runtime. The result: new comment-metadata manifests are fully parsed by all consumers.

**Bootstrap scripts:**
- [x] Create `src/resources/envy` - bash script template
- [x] Create `src/resources/envy.bat` - batch script template
- [x] Implement directive parsing (optimized single-pass sed for bash, pure batch for Windows)
- [x] Implement fallback to `FALLBACK_VERSION` with warning when `@envy version` missing
- [x] Implement cache resolution (env > manifest > default)
- [x] Implement platform/arch detection (Unix and Windows; both detect x86_64/arm64)
- [x] Implement download with curl
- [x] Implement error handling and user feedback

**Runtime directive parsing:**
- [x] Add `@envy` directive parsing to manifest loader (manual string parsing, no regex)
- [x] Parse `@envy version`, `@envy cache-posix`/`cache-win`, `@envy mirror` with escape handling
- [x] Implement unescape logic (`\"` → `"`, `\\` → `\`)
- [x] Expose parsed directives via `manifest` struct (version, cache, mirror fields)
- [ ] Detect missing `@envy version` and restore it (using running envy's version)

**Validation:**
- [x] Create test manifest fixtures with edge cases (escapes, missing directives, malformed)
- [x] Unit tests for C++ directive parsing
- [ ] Unit tests for directive restoration
- [x] Bootstrap script integration tests (see below)
- [ ] Verify bootstrap and runtime parse identically for all fixtures

**Bootstrap script integration tests:**

Python test harness starts local HTTP server serving the real envy binary, creates temp dir with manifest fixture, runs bootstrap with `ENVY_MIRROR=http://localhost:PORT`, verifies `envy version` output.
- [x] Test fixtures in `test_data/bootstrap/fixtures/`:
  - `simple.lua` — basic `@envy version`
  - `with_escapes.lua` — version with escaped quotes
  - `missing_version.lua` — triggers fallback + warning
  - `all_directives.lua` — version + cache + mirror
  - `whitespace_variants.lua` — tabs, extra spaces

This validates the full bootstrap pipeline: parse → download → cache → exec → arg forwarding.

### Phase 2: Build-Time Embedding

- [x] Create `cmake/scripts/embed_resource.py`
- [x] Create `cmake/EmbedResource.cmake`
- [x] Embed `src/resources/envy.lua` (lua_ls type definitions)
- [x] Embed `src/resources/manifest_template.lua`
- [x] Embed `src/resources/envy` (Unix only)
- [x] Embed `src/resources/envy.bat` (Windows only)
- [x] Update CMakeLists.txt (platform-conditional embedding)

### Phase 3: Runtime Support

- [x] Add `platform::get_exe_path()` to `src/platform.h` and platform-specific impls
- [x] Cache path functions already exist: `platform::get_default_cache_root()`

### Phase 4: cmd_init Implementation

- [x] Create `src/cmds/cmd_init.{h,cpp}`
- [x] Implement `write_bootstrap()` - stamp template with version and download URL
- [x] Implement `write_manifest()` - with current version
- [x] Implement `extract_lua_ls_types()` - write to cache
- [x] Implement `write_luarc()` - create if not exists, print guidance if exists
- [x] Register in `src/cli.cpp`
- [x] Add CLI parsing tests

### Phase 5: Self-Deployment on Startup

Envy self-deploys to cache on startup (before any command runs). Uses `platform::file_lock` for concurrent safety.

- [x] Add self-deployment check to main() before command dispatch
- [x] Lock-free fast path: if `$CACHE/envy/$VERSION/envy` exists, skip
- [x] Use `platform::file_lock` on `envy.$VERSION.lock` for exclusive access
- [x] Re-check after acquiring lock (another process may have completed)
- [x] Copy self to `$CACHE/envy/$VERSION/envy`
- [x] Extract embedded types to `$CACHE/envy/$VERSION/`
- [x] Release lock, continue with command

### Phase 6: Testing

**`functional_tests/test_init.py`:**
- [x] Init creates all expected files (bootstrap, manifest, .luarc.json)
- [x] Init creates directories if missing
- [x] Manifest contains @envy version directive
- [x] Init does not overwrite existing manifest
- [x] Bootstrap script has executable permissions (Unix)
- [x] .luarc.json is valid JSON with correct structure
- [x] .luarc.json workspace.library points to cache
- [x] Init prints guidance when .luarc.json exists
- [x] Init respects --mirror option
- [x] Init extracts type definitions to cache
- [x] Self-deployment creates binary in cache
- [x] Self-deployment creates type definitions
- [x] Self-deployment fast path when cached (no re-write)
- [x] Self-deployed binary has executable permissions
- [x] Cached binary can be executed directly

**`functional_tests/test_bootstrap.py`:**
- [x] Bootstrap downloads and executes envy
- [x] Bootstrap caches binary for fast path
- [x] Bootstrap uses fallback version when @envy version missing
- [x] Bootstrap parses version with escaped characters
- [x] Bootstrap fails gracefully without manifest

**Manual testing:**
- [ ] Manual test: scripts on all platforms (macOS, Linux, Windows)

### Phase 7: Documentation

- [x] Update README.md quickstart (added "Using Envy in Your Project" section)
- [x] Document mirror configuration for enterprise (--mirror flag, ENVY_MIRROR env var)
- [x] Document cache management (cache structure, cleanup commands)

---

## Files to Create

```
src/resources/
├── envy                      # Bash bootstrap template ✓
├── envy.bat                  # Batch bootstrap template ✓
├── envy.lua                  # lua_ls type definitions ✓
└── manifest_template.lua     # Default manifest content ✓

test_data/
└── bootstrap/fixtures/       # Manifest fixtures for bootstrap testing ✓

cmake/
├── scripts/embed_resource.py # C++ header generation ✓
└── EmbedResource.cmake       # CMake function ✓

src/cmds/
├── cmd_init.h                # ✓
└── cmd_init.cpp              # Init command implementation ✓
```

## Files to Modify

- `CMakeLists.txt` - Add sources, embedding, include paths ✓
- `src/cli.h` - Add cmd_init to variant ✓
- `src/cli.cpp` - Register init subcommand ✓
- `src/manifest.cpp` - Parse `@envy` directives from comments (version, cache, mirror) ✓
- `src/platform.h` - Add `get_exe_path()` ✓
- `src/platform_posix.cpp` - Implement `get_exe_path()` (macOS + Linux) ✓
- `src/platform_win.cpp` - Implement `get_exe_path()` (Windows) ✓

---

## Future Work / TODOs

### Windows Bootstrap: Detect Unsupported Characters

The pure batch Windows bootstrap has a limitation: values containing `!` or `\"` cause silent corruption or parse failures. Currently we document this limitation but don't detect it at runtime.

**Potential improvement:** Pre-scan `@envy` lines with delayed expansion OFF, using `findstr` to detect `!` before enabling delayed expansion. If found, emit a clear error message instead of silently corrupting the value.

```batch
setlocal DisableDelayedExpansion
for /f "delims=" %%L in ('findstr /n "@envy" "%MANIFEST%"') do (
    for /f "tokens=1 delims=:" %%N in ("%%L") do (
        if %%N LEQ 20 (
            echo %%L | findstr /C:"!" >nul && (
                echo ERROR: @envy directive contains '!' - not supported on Windows >&2
                exit /b 1
            )
        )
    )
)
```

This works because `%%L` loop variables expand raw content when delayed expansion is disabled, preserving literal `!` characters for findstr to detect.

**Status:** Deferred. The edge case is rare enough (version strings, paths, and URLs don't typically contain `!`) that we accept the limitation for now. Revisit if users report issues.

### `@envy cache-posix`/`cache-win` Directive — Partial Fix Complete

**Bug discovered 2024-12:** The `@envy cache` directive (now `cache-posix`/`cache-win`) was parsed but not used by the C++ runtime. Additionally, tilde (`~`) expansion was broken in both bootstrap script and C++ paths.

**Status (2024-12):** C++ runtime fix complete. Bootstrap script expansion still pending.

#### Problem 1: C++ Runtime Ignores `meta.cache` — FIXED

~~The manifest's cache directive is parsed into `manifest::meta.cache` but never consulted.~~ **Fixed:** Commands now call `cache::ensure(cli_cache_root, m->meta.cache)` which uses `resolve_cache_root()` with full precedence chain.

Current precedence (fully implemented):
1. `--cache-root` CLI flag
2. `ENVY_CACHE_ROOT` env var
3. `@envy cache-posix`/`cache-win` directive ← **Now works**
4. Platform default

#### Problem 2: No Tilde Expansion — FIXED

~~Even if the directive were used, `~` paths would fail.~~ **Fixed:** `expand_path()` uses `wordexp()` on POSIX for full shell-like expansion (~, $VAR, ${VAR}). Bootstrap scripts now also expand `~` and `$VAR`.

#### Problem 3: No Functional Test Coverage — FIXED

~~All tests use `ENVY_CACHE_ROOT` env var (takes precedence, masks the bug).~~ **Fixed:** `functional_tests/test_cache_directive.py` tests:
- Tilde expansion (`~/.cache`)
- Env var expansion (`$HOME/.cache`)
- CLI precedence over manifest
- Env precedence over manifest
- Plain path directive

#### Fix Plan — C++ Runtime Complete

**Approach: `cache::ensure()` static method** ✓

Added a static `ensure()` method to the `cache` class, following the existing `ensure_asset()` / `ensure_recipe()` pattern. This method resolves the cache root (with path expansion), constructs the cache, and self-deploys. Only commands that need cache call it; stateless commands don't touch cache.

**Command categories:**

| Command | Needs Cache | Needs Manifest | Action |
|---------|-------------|----------------|--------|
| `sync` | Yes | Yes | `cache::ensure(cli_root, m->meta.cache)` |
| `package` | Yes | Yes | `cache::ensure(cli_root, m->meta.cache)` |
| `product` | Yes | Yes | `cache::ensure(cli_root, m->meta.cache)` |
| `init` | Yes | No | `cache::ensure(cli_root, std::nullopt)` |
| `fetch` | No | No | — |
| `extract` | No | No | — |
| `hash` | No | No | — |
| `lua` | No | No | — |
| `version` | No | No | — |

**Phase A: Add `cache::ensure()` to `cache.{h,cpp}`** ✓

```cpp
// cache.h - added static method and free functions
std::filesystem::path expand_path(std::string_view path);  // wordexp on POSIX
std::filesystem::path resolve_cache_root(
    std::optional<std::filesystem::path> const &cli_override,
    std::optional<std::string> const &manifest_cache);

class cache {
public:
  static std::unique_ptr<cache> ensure(
      std::optional<std::filesystem::path> const &cli_cache_root,
      std::optional<std::string> const &manifest_cache);
  // ... existing members ...
};
```

**Phase B: Update commands** ✓

All commands updated to use new signature. Manifest-aware commands call `cache::ensure(cli_cache_root_, m->meta.cache)`, cache-only commands pass `std::nullopt`, stateless commands don't construct cache.

**Phase C: Refactor `main.cpp`** ✓

Removed centralized cache construction and self-deploy. Commands handle their own cache setup via `cache::ensure()`.

**Phase D: Unit tests in `cache_tests.cpp`** ✓

Added 10 tests for `expand_path()` and `resolve_cache_root()` covering tilde expansion, env var expansion, and precedence chain.

**Phase E: Bootstrap script path expansion** ✓

Unix (`src/resources/envy`): Expands `~` to `$HOME` and uses `eval` for `$VAR`/`${VAR}` expansion.

Windows (`src/resources/envy.bat`): Expands `~` to `%USERPROFILE%`. Note: `$VAR` expansion not supported on Windows; use full paths or `%VAR%` syntax.

**Phase G: Functional tests** ✓

Added `functional_tests/test_cache_directive.py` with 5 tests:
- `test_cache_directive_tilde_expansion` — verifies `~` expands to home
- `test_cache_directive_env_var_expansion` — verifies `$HOME` expands
- `test_cli_cache_root_overrides_manifest` — verifies CLI precedence
- `test_env_cache_root_overrides_manifest` — verifies env precedence
- `test_cache_directive_plain_path` — verifies plain paths work

Bootstrap path expansion is tested via the existing functional tests which exercise the full bootstrap → C++ pipeline.

**Implementation order:**

1. [x] Add `cache::ensure()` static method to `cache.{h,cpp}` with path expansion logic
2. [x] Add unit tests for path expansion and `cache::ensure()` precedence in `cache_tests.cpp`
3. [x] Update command configs to store `cache_root` (remove `cache &` from constructors)
4. [x] Update `cmd::create()` factory to pass `cache_root` instead of `cache &`
5. [x] Refactor `main.cpp`: remove central cache construction and self-deploy
6. [x] Update manifest-aware commands (`sync`, `package`, `product`) to use `cache::ensure()`
7. [x] Update cache-only command (`init`) to use `cache::ensure()`
8. [x] Fix bootstrap script path expansion (Unix) — tilde + $VAR
9. [x] Fix bootstrap script path expansion (Windows) — tilde
10. [x] Add functional tests for `@envy cache-posix`/`cache-win` directive
11. [x] Delete `src/runtime.{h,cpp,_tests.cpp}` (superseded by `cache::ensure()`)
