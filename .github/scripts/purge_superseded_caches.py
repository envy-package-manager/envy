#!/usr/bin/env python3
"""Delete the GitHub Actions caches this CI job just superseded.

GitHub caps a repository at 10 GB of Actions cache and evicts LRU past that. Cache keys
are immutable and carry a commit sha (sccache) or a dependency-manifest hash (out/cache),
so every push to main mints a fresh generation while the previous one lingers until
eviction reclaims it. That doubles steady-state demand and lets jobs within a single run
evict each other's caches. Dropping the superseded generation the moment its replacement
is stored keeps exactly one generation resident per key prefix.

Every prefix is purged by exactly one job per run, so concurrent matrix jobs never contend
for the same keys. A caller passes an empty *_KEEP when it stored no replacement; purging
then would delete a good cache and leave nothing behind, so it is skipped.

Purging is opportunistic: a stale cache that survives one run is simply retried by the
next. Nothing here may fail the job, so every fallible step degrades to a message.
"""

import json
import os
import subprocess
import sys


def gh(*args: str) -> subprocess.CompletedProcess:
    """Run gh with the given arguments, capturing output and never raising."""
    try:
        return subprocess.run(["gh", *args], capture_output=True, text=True)
    except OSError as exc:
        return subprocess.CompletedProcess(args, 1, "", str(exc))


def cache_keys(repo: str, ref: str) -> list[str]:
    """Return every cache key visible in this ref's scope; empty if the query failed.

    A failed query and a genuinely empty scope both mean "purge nothing", so they need no
    distinction here.
    """
    result = gh(
        "cache", "list", "--repo", repo, "--ref", ref, "--limit", "100", "--json", "key"
    )
    if result.returncode != 0:
        print(f"could not list caches: {result.stderr.strip()}")
        return []
    try:
        return [entry["key"] for entry in json.loads(result.stdout)]
    except (json.JSONDecodeError, KeyError, TypeError) as exc:
        print(f"could not parse cache listing: {exc}")
        return []


def purge(repo: str, keys: list[str], prefix: str, keep: str) -> None:
    """Delete every cache under prefix except keep, the one this job just stored."""
    if not keep:
        print(f"no replacement stored under {prefix}; leaving existing caches alone")
        return
    for key in keys:
        if not key.startswith(prefix) or key == keep:
            continue
        if gh("cache", "delete", "--repo", repo, key).returncode == 0:
            print(f"purged {key}")
        else:
            print(f"could not purge {key}; another job may have won the race")


def main() -> int:
    repo, ref = os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_REF"]
    targets = [
        (os.environ["SCCACHE_PREFIX"], os.environ["SCCACHE_KEEP"]),
        (os.environ["DEPS_PREFIX"], os.environ["DEPS_KEEP"]),
    ]

    if not any(keep for _, keep in targets):
        print("this job stored no caches; leaving existing caches alone")
        return 0

    keys = cache_keys(repo, ref)
    for prefix, keep in targets:
        purge(repo, keys, prefix, keep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
