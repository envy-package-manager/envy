# Future Enhancements

Not scheduled.

## Source Overrides and Mirrors

A manifest cannot redirect a transitive dependency: two sources for one identity is an error. Overrides replace a spec's source everywhere it is referenced; rewrites map URL prefixes for spec sources and payload fetches. Root manifest only; a non-`local.*` spec cannot be overridden to a local source; only pinned fetches (sha256 or git commit) are rewritten, since an unpinned rewrite changes bytes without changing the hash.

```lua
OVERRIDES = { ["vendor.gcc@v2"] = { source = "https://mirror.corp/gcc.lua", sha256 = "ab…" } }
URL_REWRITES = { ["https://github.com/"] = "https://mirror.corp/github/" }
```

## Range Resume for Interrupted Downloads

A retry restarts from byte zero, so a failure at 88% of a 1 GB toolchain refetches 880 MB. Resume from a `.part` file with `Range:` and verify sha256 on the result. A server that answers 200 instead of 206 must truncate, not append; `If-Range:` guards mirrors that redirect to different bytes.

```cpp
setopt(CURLOPT_RESUME_FROM_LARGE, partial_bytes);  // 206: append; 200: truncate
```

## S3 Region in the URI

`fetch_request_s3::region` exists but nothing sets it. envy falls back to `AWS_DEFAULT_REGION`, `AWS_REGION`, the profile, then `us-east-1`, so a bucket in another region fails as a redirect. Parse the region from the URI.

```lua
FETCH = { source = "s3://my-bucket/tool.tar.gz?region=eu-west-1" }
```

## Linux CA Bundles

macOS and Windows use the OS certificate store. On Linux, libgit2 probes four CA bundle paths at runtime, but libcurl (mbedTLS) gets no CA path from envy and uses whatever its build detected, which need not exist on the host. Give curl the same probe and honor `SSL_CERT_FILE`.

```cpp
setopt(CURLOPT_CAINFO, ca_bundle_path().c_str());  // SSL_CERT_FILE, else first probe hit
```

## Windows Bootstrap `!` Detection

`envy.bat` enables delayed expansion before reading `@envy` lines, so a value containing `!` or `\"` is silently corrupted. Scan with delayed expansion off and fail on either character.

```
-- @envy mirror "s3://x!y"   →   ERROR: @envy directive contains '!'
```

## `.luarc.json` Without `workspace.library`

`envy init` prints instructions when `.luarc.json` exists, and `rewrite_luarc_types_path` edits only an existing `workspace.library` array. Create the array when it is missing.

## Dead `subdir` Fields

`pkg_cfg`'s source structs carry a `subdir` that nothing sets; bundles replaced it. Delete the fields and their equality checks.

## Deferred from the Dependency-System Audit

Designed and rejected for now.

- **Early ratchet of weak-free packages.** A package with no weak references could pass `spec_fetch` during resolution and overlap payload work with fetch. It moves when SETUP selection is snapshotted: `start_pkg_thread(p, fetch)` before the barrier limits what a late `merge_setup_selection` can do.
- **Per-task condvar for `wait_at`.** All watermark waits share one `cv_`, so each `notify_global()` wakes every waiter: O(N²) wakeups. N is small. Fix: `dep->cv.notify_all()`.
- **Edge-vector caching.** `cfg.edges(step)` rebuilds the edge list each step under `deps_mutex`. Caching needs invalidation on `wire_dependency`, which runs while the owner is blocked.
- **Fetch closures on `pkg_cfg`.** Storing a `sol::protected_function` would remove `find_fetch_function`'s scan, but a cfg outlives the Lua state that owns the closure.
- **Canonical-keyed dependency map.** `pkg::dependencies` keys on identity, so two option variants of one identity in one list are a parse error. Keying on the canonical string allows both but makes `envy.package("gcc")` ambiguous.
- **`-Wall -Wextra`.** Off; third-party trees in the same build need per-target suppression first.
