# Envy Commands

Envy is a multi-tool CLI following Git's subcommand pattern. Each subcommand targets a distinct workflow; global flags apply across all commands.

## Global Flags

Logging is per-package narrative. Default (INFO) prints one outcome line per package plus command summaries. The three levels are mutually exclusive:

**`--verbose`** — DEBUG: per-package decision narrative (why each choice was made); decorated with timestamp and severity tag.
**`-q`, `--quiet`** — Warnings and errors only.
**`--trace[=stderr|file:<path>]`** — Emit structured machinery trace events (scheduler, cache/lock, IO) as JSONL to a file and/or human-readable text to stderr. Comma-separate multiple sinks (`--trace=stderr,file:/tmp/t.jsonl`); bare `--trace` defaults to stderr. Orthogonal to log verbosity—does not change the log level.
**`-v`, `--version`** — Print version information (alias for `envy version`).
**`-h`, `--help`, `help`** — Print top-level help summarizing available subcommands. Subcommands also support `--help` for detailed usage.

## Subcommands

### Meta

**`envy version`** — Print envy version and third-party component versions.
**`envy licenses`** — Emit envy’s license followed by every bundled third-party license; canonical source for compliance exports.

**`envy use <version> [--manifest=...] [--subproject] [--mirror=...] [--pin-sums|--no-pin-sums] [--force]`** — Retarget the manifest's `@envy version` and refresh its `@envy sha256sums` in one step; upgrades and downgrades alike. Splices values into the existing header lines, so comments, indentation and CRLF survive. Reads the header directly instead of loading the manifest—no re-exec, no Lua—so it repairs the state nothing else can: a manifest already naming a version whose pin is stale, which cannot download its own binary. Pinning follows the manifest (pinned stays pinned, unpinned stays unpinned) unless `--pin-sums`/`--no-pin-sums` says otherwise. SHA256SUMS is fetched from the manifest's `@envy mirror` (or `ENVY_MIRROR`, or `--mirror`) even when there is no pin to write—that fetch is what turns a typo'd or unmirrored version into an error here rather than a failed bootstrap elsewhere; `--force` skips it, and is refused when a pin is in play. Network before file: a failed run leaves the manifest byte-for-byte unchanged. A manifest with no `@envy version` is an error, not a conversion—that project floats to `latest` deliberately. Edits the manifest only: the bootstrap scripts and `.luarc.json` are stamped from the *running* binary's version, so the retargeted envy has to restamp them—`sync`/`deploy` do that after re-execing, and `use` prints a reminder.

```bash
envy use 0.1.6                          # root manifest
cd subproject && envy use 0.1.6 --subproject
envy sync                               # restamps scripts + .luarc.json as the new envy
```

### Package Management

**`envy package <identity> [--manifest=...]`** — Query and install package, print package path. Loads manifest (auto-discovered or via `--manifest`), finds matching spec, installs only that package plus transitive dependencies if not cached, prints absolute path to package directory to stdout. Other manifest packages are not processed. Errors if identity ambiguous (multiple option variants) or programmatic package (no cached artifacts). Exits 0 with path on success, exits 1 with "not found" on failure.

### Cache

**`envy cache`** — Print the cache root and its disk usage: one line per package entry (`identity/platform-arch-blake3-hash`), one per cached envy deployment, one per remaining top-level directory (`specs`, `locks`), then the total. Rows are largest-first; sizes are apparent file sizes, symlinked trees excluded. Resolves the root through the full precedence chain—`--cache-root`/`ENVY_CACHE_ROOT`, then a discovered manifest's `@envy cache-*` directive (read as text; the manifest's Lua never runs), then the platform default. Measurement is a lock-free parallel walk over platform-native directory enumeration (`openat`/`fdopendir`/`fstatat`, `FindFirstFileExW` with `FIND_FIRST_EX_LARGE_FETCH`), one work-queue entry per directory.

### Shell Integration

**`envy shell <shell>`** — Print the `source` line to add to your shell profile for automatic PATH management. Supported shells: `bash`, `zsh`, `fish`, `powershell`. Hook files are created automatically during self-deploy; this command just prints the line. Warns if using a non-default cache location. See `docs/shell-integration.md` for details.

### Utilities

**`envy fetch <url> [destination]`** — Download file from any supported transport (HTTP/HTTPS, FTP/FTPS, SMB, Git, SSH, S3). Destination defaults to current directory with URL's filename. Verifies TLS, supports authentication (SSH keys, AWS credentials). Displays progress, optionally prints SHA256 on completion.

**`envy extract <archive> [destination]`** — Extract archive to specified location (defaults to current directory). Supports all libarchive formats: tar, tar.gz, tar.xz, tar.bz2, tar.zst, zip, 7z, rar, iso. Preserves permissions, timestamps, symlinks. Reports file count on completion.

**`envy compress <path> [output]`** — Create archive from file or directory. Format auto-detected from output extension (.tar.gz, .tgz, .tar.xz, .tar.bz2, .tar.zst, .tar, .zip). Defaults to `<basename>.tar.gz` if output not specified.

**`envy hash <path...> [--prefix=<url>]`** — Print the SHA256 of each path as `HASH  filename`, `sha256sum`-style. A directory contributes its `*.tar.zst` entries, non-recursively—the shape `envy export` writes. `--prefix` prepends a URL prefix to each name, so the output drops straight into a depot manifest.

**`envy git-resolve <url> <ref>`** — Resolve a git ref (tag/branch/sha) in a remote repo to a full commit sha via libgit2's ref advertisement (no clone, no `git` binary); prints the sha to stdout. Prefer fully-qualified refs (`refs/tags/…`, `refs/heads/…`); a bare trailing segment (`v1.5.23`) resolves when unambiguous. Annotated tags peel to their commit; a full 40/64-hex sha is echoed back (lowercased, no network). Turns a mutable tag/branch into an immutable sha to pin in a manifest — resolving once at authoring time, not on every script run.

**`envy lua [script]`** — Execute Lua script with envy's embedded runtime. If no script provided, opens interactive REPL. Exposes envy verbs (`fetch`, `extract`, `hash`) to Lua environment.
