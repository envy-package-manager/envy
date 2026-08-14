# Cache Design

## Overview
- Single cache root (default `~/.cache/envy/`) holds specs and assets; entries become immutable once marked complete so readers never lock.
- Scene-aware locking: exclusive while building, lock-free after `envy-complete` appears; install directories live beside final paths for atomic rename.
- Project-local (`local.*`) specs stay in the repo tree and bypass the shared cache.

## Layout
```
~/.cache/envy/
├── envy/                         # Envy binaries + types (self-deployed)
│   └── {version}/
│       ├── envy                  # Binary (or envy.exe on Windows)
│       └── envy.lua              # lua_ls type definitions
├── specs/                        # Lua sources and bundles (one entry per source)
│   └── corporate.toolchain@v3/
│       └── blake3-{source_hash}/ # Keyed on the source, not identity alone
│           ├── envy-complete     # Written only once the spec loads (see below)
│           ├── pkg/              # spec.lua, or an unpacked bundle
│           ├── fetch/            # Durable fetch cache (persists across failures)
│           │   └── envy-complete # Marker: all fetches verified
│           └── work/             # Ephemeral workspace (wiped each attempt)
├── assets/                       # Asset entries (one per identity/options/platform)
│   └── {namespace}.{name}@{version}/
│       └── {platform}-{arch}-sha256-{hash}/
│           ├── envy-complete
│           ├── envy-fingerprint.blake3
│           ├── asset/            # Publish-ready payload (renamed from install/)
│           ├── fetch/            # Durable fetch cache (persists for per-file caching)
│           │   └── envy-complete # Marker: all fetches verified
│           ├── install/          # Staging area for asset preparation
│           └── work/             # Ephemeral workspace (stage/, etc.)
│               └── stage/        # Build staging tree (wiped before each attempt)
└── locks/
    └── {recipe|asset|envy}.*.lock
```

## Envy Binaries

The `envy/` directory stores envy binaries and their lua_ls type definitions. Bootstrap scripts check this location; if missing, they download to a temp directory and exec from there. The envy binary self-deploys on startup:

1. **Lock-free check:** If `$CACHE/envy/$VERSION/envy` exists, continue immediately
2. **Acquire lock:** `$CACHE/locks/envy.$VERSION.lock` (exclusive, blocking)
3. **Re-check:** Another process may have completed deployment while waiting
4. **Deploy:** Copy self to cache, extract embedded types alongside
5. **Release lock:** Continue with requested command

This uses the same locking strategy as recipe/asset installation (see Locking & Workspace Lifecycle below). Multiple concurrent envy instances (parallel CI, multiple terminals) safely coordinate without corruption or duplicate work. Each version is self-contained; deleting `envy/1.2.3/` removes that version completely.

## Keys
- **Spec/bundle**: `{identity}/blake3-{hash}` where `hash` is the leading 16 hex chars of BLAKE3 over the canonical source — URL + sha256, git URL + ref, or local path. Identity alone would not do: a complete entry is never revalidated, so repointing a spec at a new source must land on a new entry rather than serve the old bytes. A custom fetch function has no fingerprint; its entries key on the declaring file, so editing the function body in place reuses the entry.
- **Asset**: `{identity}.{platform}-{arch}-sha256-{hash}` where `hash` is the leading 16 hex chars of the archive SHA256; deterministic before download so locks can be acquired early.

## Locking & Workspace Lifecycle

### General Lock Behavior
- Locks: POSIX `fcntl(F_SETLKW)` / Windows `LockFileEx`; blocking exclusive locks only during creation.
- Patterns:
  1. Lock-free read: check `envy-complete`; if present, return path immediately.
  2. Exclusive creation: grab lock, re-check marker (someone else may have finished), proceed if still missing.
- Lock files (`locks/...`) exist only while holding the lock—`cache::scoped_entry_lock` destroys them on destruction.

### Cache-Managed Packages (Standard)
- Acquisition ensures `assets/{entry}/install/` and `assets/{entry}/work/` exist.
- Workspace separates `fetch/` (durable, persists across failures) and `work/` (ephemeral, wiped each attempt).
- Per-file caching: `fetch/` persists across failed attempts. On subsequent runs, each file is verified by SHA256 before re-downloading. Only missing or corrupted files trigger new downloads. Files without SHA256 are always re-downloaded (no cache trust without verification).
- Specs call `mark_fetch_complete()` once all fetches succeed; this drops `envy-complete` sentinel inside `fetch/`.
- On success (INSTALL returns successfully):
  - Envy auto-marks complete internally
  - Lock destructor atomically renames `install/` → `asset/`
  - Fingerprints payload into `envy-fingerprint.blake3`
  - Deletes both `work/` and `fetch/`
  - Touches `entry/envy-complete`
- Crash recovery: next locker deletes stale `install/`, recreates `work/`, preserves `fetch/` for per-file cache reuse.

### SETUP Pair Entries (Ephemeral Cache)
- Host-side work lives in `SETUP` pairs (`{ name = { CHECK, INSTALL, DEPENDS? } }`); user-managed specs (`USER_MANAGED = true`, resolved once during `phase_spec_fetch`) define only pairs, cache-managed packages may add opt-in pairs beside their payload
- Each running pair acquires an ephemeral entry keyed `BLAKE3(format_key() + "|setup:" + name)`; lock calls `mark_user_managed()` to signal ephemeral workspace. Selected pairs run as parallel tasks, so one package may hold several ephemeral pair entries concurrently
- Pair INSTALL modifies the host; workspace never persists to cache
- On completion (lock destructor detects `user_managed_` flag):
  - Entire `entry_dir` deleted (no `pkg/` rename)
  - Cache entry fully purged—no persistent artifacts
  - Lock file deleted
- Subsequent runs use the pair's CHECK verb (not cache marker) to skip work
- Crash recovery: next locker deletes entire stale entry, starts fresh

### Lock Destructor Three-Way Branch
The `scoped_entry_lock` destructor handles three distinct completion modes:
1. **Success (completed_):** Cache-managed package finished installation
   - Rename `install/` → `asset/`
   - Create `envy-complete` marker and fingerprint
   - Delete `work/` and `fetch/`
2. **User-managed (user_managed_):** Ephemeral workspace no longer needed
   - Delete entire `entry_dir` (all subdirectories)
   - No `envy-complete` marker created
3. **Failure (neither flag set):** Cache-managed package failed
   - Conditional purge: if `install/` and `fetch/` both empty, delete entry
   - Otherwise preserve `fetch/` for per-file cache reuse

## Integrity & Verification

**Specs:**
- **Declarative sources (URL):** Manifest supplies SHA256; Envy downloads to temp, verifies hash, moves to cache on match. Mismatch = hard failure.
- **Declarative sources (git):** Manifest supplies `ref` (commit SHA or committish); Envy clones repo, checks out ref, verifies git tree integrity. No separate SHA256 needed (git hash is fingerprint).
- **Custom fetch functions:** API-enforced per-file verification. `ctx:fetch(url, sha256)` and `ctx:import_file(src, dest, sha256)` verify before writing to cache. Custom fetch cannot bypass (no direct cache directory access).
- **Verification timing:** SHA256 computed at fetch time only; never re-verified from cache (`envy-complete` marker signals immutable entry).

**Assets:**
- Specs declare expected hashes for downloads; verification happens before extraction and during per-file cache reuse.
- Per-file caching: declarative fetch arrays with SHA256 verification enable cache reuse across partial failures. On each attempt, existing files in `fetch/` are verified by SHA256 before re-downloading. Cache hits skip download; cache misses (corruption, missing files) trigger re-download.
- Trust chain: once spec passes integrity, its declared downloads inherit trust. Files without SHA256 cannot be cached (always re-downloaded).
- BLAKE3 fingerprint file captures every asset payload (mmap-friendly header, entry table, string blob) so verification tools compare without locks.

## Operational Scenarios

### Spec Fetch

The entry is finalized last, never at fetch time: `envy-complete` is trusted forever after, so it is written only once the fetched bytes have proven to be a loadable spec declaring the expected identity (a bundle: a parseable manifest whose identity matches and whose specs validate). A throw before that drops the lock uncompleted, the destructor scrubs the entry, and the next run refetches. Marking first would bake a 404 body or a malformed bundle into an entry that fails identically forever.

1. **Declarative URL (first fetch):** miss → lock → download to `pkg/spec.lua` → verify SHA256 if pinned → load spec, check IDENTITY → touch `envy-complete` → release.

2. **Declarative git (first fetch):** miss → lock → clone into `pkg/` at `ref` → load `pkg/spec.lua`, check IDENTITY → touch `envy-complete` → release.

3. **Custom fetch (first fetch):** miss → lock → call fetch function (ctx.tmp_dir, ctx.fetch, ctx.commit_fetch) → move `fetch/spec.lua` to `pkg/` → load it, check IDENTITY → touch `envy-complete` → release.

4. **Bundle (first fetch):** miss → lock → download/clone/copy into `pkg/` → parse `pkg/envy-bundle.lua`, check BUNDLE identity, validate every declared spec → touch `envy-complete` → release.

5. **Concurrent spec fetch:** waiter blocks on lock; when creator finishes, waiter rechecks `envy-complete` and returns path without refetching.

6. **Source changed:** a new URL, ref, or path hashes to a different entry — a miss, not a stale hit. The old entry stays valid for anyone still declaring the old source.

### Asset Install

1. **First asset install**: miss → lock → create `install/` + `fetch/` + `work/` → download into `fetch/` → verify SHA256 per file → `mark_fetch_complete()` on success → stage sources in `work/stage/` → write payload into `install/` → rename to `asset/` → fingerprint `asset/` → delete `fetch/` + `work/` → touch entry `envy-complete` → release.

2. **Concurrent asset install**: waiter blocks on lock; when creator finishes, waiter rechecks `envy-complete` and returns final path without recaching.

3. **Crash recovery**: crash leaves `install/` (and maybe `fetch/` + `work/`); next locker deletes stale `install/` and `work/`, preserves `fetch/` for per-file cache reuse, and restarts. Declarative fetch verifies each cached file by SHA256 before re-downloading.

4. **Partial failure recovery**: partial download leaves some files in `fetch/`; next attempt verifies cached files by SHA256, reuses cache hits, only downloads missing/corrupted files.

5. **Multi-project sharing**: identical `(identity, options, platform, hash)` reuses the same asset directory; no duplication.

### SETUP Pair Install

1. **First run with CHECK=false**: pair CHECK returns false (not satisfied) → pair lock acquired, `mark_user_managed()` called → post-lock re-CHECK returns false → pair INSTALL runs against `project_root` and modifies the host (udev rules, brew install, etc.) → lock destructor detects `user_managed_` flag → entire `entry_dir` deleted → lock released.

2. **Second run with CHECK=true**: pair CHECK returns true (already satisfied) → no lock acquired → pair skipped.

3. **Concurrent install**: Process A checks (false) → acquires pair lock, marks user-managed, re-checks (false) → runs INSTALL. Process B checks (false) → blocks on lock. Process A completes, destructor purges entry. Process B acquires lock, re-checks (NOW true, A finished) → releases lock immediately without running INSTALL.

4. **Selection isolation**: pair selection is per-referrer (manifest or dependency entries) and never hashed—projects sharing the user-wide cache reuse one payload entry while each run evaluates only its explicitly selected pairs (no selection = no pairs, any package type).
