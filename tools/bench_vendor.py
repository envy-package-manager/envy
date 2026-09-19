#!/usr/bin/env python3
"""Benchmark the vendor copy, and the copy strategies it could be using instead.

Two halves, because they answer two different questions.

**What envy does now** drives `envy vendor` over generated corpora and reads the stage
split out of the `vendor_result` trace event: `hash_ms`, `wipe_ms`, `copy_ms`. Three
scenarios per shape -- an absent destination (copy only), a dirty one (wipe plus copy),
and a clean one (hash only) -- because they stress different parts and a project hits all
three.

**What it could do** copies the same corpus with each candidate strategy: serial, a
thread pool at several widths, and a copy-on-write clone where the filesystem has one
(`clonefile` on APFS, `FICLONE` on btrfs/XFS). The Python strategies pay a per-file
interpreter tax that envy would not, so read them against each other rather than against
the envy numbers; `cp`/`cp -c` are there as the tax-free reference for the same work.

Corpora come from tools/bench_tree_hash.py -- same shapes, same deterministic content.
Vendoring writes the corpus three times over (cache, destination, probe destination), so
this sweeps at 1/8 scale by default where the hash benchmark sweeps at full size.

    python3 tools/bench_vendor.py                    # full sweep, human tables
    python3 tools/bench_vendor.py --profile mixed    # one shape
    python3 tools/bench_vendor.py --skip-probe       # only what envy does today
    python3 tools/bench_vendor.py --json out.json

Caches are warm, for the reason bench_tree_hash.py gives: a cold-cache number nobody can
reproduce is worse than a warm one everybody can.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from bench_tree_hash import (  # noqa: E402
    MIB,
    PROFILES,
    PROFILES_BY_NAME,
    generate,
    thread_counts,
)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BENCH_ROOT = ROOT / "out" / "bench"

# One identity per shape: a cache entry is keyed on identity and options, not on the
# spec's text, so a single name would have every profile after the first hit the first
# one's payload and silently measure the wrong corpus.


def identity_for(profile) -> str:
    return "local.bench" + profile.name.replace("-", "") + "@r1"


SPEC = """IDENTITY = "{identity}"
FETCH = {{ source = "file://{seed}" }}
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options)
  envy.copy("{corpus}", install_dir)
end
"""

MANIFEST = """-- @envy bin "envy-bin"
VENDOR_ROOT = "vendor"
PACKAGES = {{
  {{ spec = "{identity}", source = "{spec}", vendor = true }},
}}
"""


# -- the project under test ------------------------------------------------------------


def project_for(profile, corpus: Path, root: Path, envy: Path, verbose: bool) -> Path:
    """A project whose one package installs `corpus`, with its cache already primed.

    Priming is `envy install`, which is also the slow part on a many-file shape: it is
    the same serial copy this benchmark is here to question, paid once into the cache.
    """
    project = root / f"vendor-project-{profile.name}"
    manifest = project / "envy.lua"
    primed = project / ".primed"

    if not primed.exists():
        if project.exists():
            shutil.rmtree(project)
        project.mkdir(parents=True)
        seed = project / "seed.txt"
        seed.write_text("seed\n", encoding="utf-8")
        spec = project / "bench.lua"
        spec.write_text(
            SPEC.format(
                identity=identity_for(profile),
                seed=seed.as_posix(),
                corpus=corpus.as_posix(),
            ),
            encoding="utf-8",
        )
        manifest.write_text(
            MANIFEST.format(identity=identity_for(profile), spec=spec.as_posix()),
            encoding="utf-8",
        )
        if verbose:
            print(f"  priming cache for {profile.name}", file=sys.stderr)
        run_envy(envy, root, ["install", "--manifest", str(manifest)])
        primed.write_text("primed by tools/bench_vendor.py\n", encoding="utf-8")

    return project


def cache_root(root: Path) -> Path:
    return root / "vendor-cache"


def run_envy(envy: Path, root: Path, args: list[str], trace: Path | None = None) -> None:
    command = [str(envy), "--cache-root", str(cache_root(root)), "-q"]
    if trace:
        command.append(f"--trace=file:{trace}")
    result = subprocess.run(
        command + args, capture_output=True, text=True, encoding="utf-8"
    )
    if result.returncode != 0:
        raise SystemExit(f"envy {args[0]} failed ({result.returncode}):\n{result.stderr}")


def vendor_result(
    envy: Path, root: Path, project: Path, identity: str, threads: int = 0
) -> dict:
    """One `envy vendor` run; returns its vendor_result trace record."""
    trace = project / "bench-trace.jsonl"
    trace.unlink(missing_ok=True)
    run_envy(
        envy,
        root,
        [
            "vendor",
            identity,
            "--threads",
            str(threads),
            "--manifest",
            str(project / "envy.lua"),
        ],
        trace=trace,
    )
    for line in trace.read_text(encoding="utf-8").splitlines():
        record = json.loads(line)
        if record.get("event") == "vendor_result":
            return record
    raise SystemExit("envy vendor emitted no vendor_result event")


# -- what envy does now ----------------------------------------------------------------

SCENARIOS = ("copy", "revendor", "check")


def stage(scenario: str, dest: Path) -> None:
    """Put the destination in the state this scenario is about to measure."""
    if scenario == "copy":
        shutil.rmtree(dest, ignore_errors=True)
    elif scenario == "revendor":
        # One byte of drift is the whole trigger: the wipe and recopy that follows is
        # the same size whether one file changed or all of them.
        victim = next(p for p in sorted(dest.rglob("*")) if p.is_file())
        victim.write_bytes(victim.read_bytes() + b"x")


def sweep_envy(
    envy, profiles, root: Path, reps: int, verbose: bool, threads: list[int]
) -> list[dict]:
    """Every shape in every scenario; the copy scenario also across thread counts.

    Only `copy` sweeps threads: it is the one scenario that is copy and nothing else, so
    it is the one that can answer what the default should be.
    """
    records = []
    for profile in profiles:
        corpus = generate(profile, root, verbose)
        expected_files = sum(1 for p in corpus.rglob("*") if p.is_file())
        project = project_for(profile, corpus, root, envy, verbose)
        identity = identity_for(profile)
        dest = project / "vendor" / identity.split(".")[1].split("@")[0]

        plan = [("copy", n) for n in threads] + [(s, 0) for s in SCENARIOS[1:]]
        for scenario, n in plan:
            samples = []
            for _ in range(reps + 1):  # the first is a warm-up
                stage(scenario, dest)
                samples.append(vendor_result(envy, root, project, identity, n))
            samples = samples[1:]
            best = min(samples, key=lambda r: r["duration_ms"])
            if scenario != "check" and best["files"] != expected_files:
                raise SystemExit(
                    f"{profile.name}: vendored {best['files']} files, but the corpus "
                    f"has {expected_files} -- the cache entry is another profile's"
                )
            if verbose:
                print(
                    f"  {profile.name}/{scenario}: {best['action']}", file=sys.stderr
                )
            copy_ms = best["copy_ms"] or 1
            records.append(
                {
                    "profile": profile.name,
                    "scenario": scenario,
                    "threads": n,
                    "action": best["action"],
                    "files": best["files"],
                    "bytes": best["bytes"],
                    "hash_ms": best["hash_ms"],
                    "wipe_ms": best["wipe_ms"],
                    "copy_ms": best["copy_ms"],
                    "min_ms": best["duration_ms"],
                    "median_ms": statistics.median(r["duration_ms"] for r in samples),
                    "copy_mb_per_s": best["bytes"] / MIB / (copy_ms / 1000.0),
                    "copy_files_per_s": best["files"] / (copy_ms / 1000.0),
                }
            )
    return records


# -- what it could do ------------------------------------------------------------------


def clone_file(src: str, dst: str) -> None:
    """One copy-on-write clone, or OSError if the filesystem has no such thing."""
    raise OSError("clone unsupported on this platform")


if sys.platform == "darwin":
    _libc = ctypes.CDLL("libSystem.dylib", use_errno=True)
    _libc.clonefile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]

    def clone_file(src: str, dst: str) -> None:  # noqa: F811
        if _libc.clonefile(src.encode(), dst.encode(), 0) != 0:
            raise OSError(ctypes.get_errno(), "clonefile", dst)

elif sys.platform.startswith("linux"):
    import fcntl

    FICLONE = 0x40049409

    def clone_file(src: str, dst: str) -> None:  # noqa: F811
        with open(src, "rb") as s, open(dst, "wb") as d:
            fcntl.ioctl(d.fileno(), FICLONE, s.fileno())


def walk_corpus(corpus: Path) -> tuple[list[str], list[str]]:
    """(directories, files) as relative POSIX paths, parents before children."""
    dirs, files = [], []
    for base, subdirs, names in os.walk(corpus):
        subdirs.sort()
        rel = Path(base).relative_to(corpus)
        if rel != Path("."):
            dirs.append(rel.as_posix())
        files.extend((rel / n).as_posix() for n in sorted(names))
    return dirs, files


def copy_tree(corpus: Path, dest: Path, layout, copy, threads: int) -> None:
    """Recreate `corpus` under `dest` with one file-copy primitive at `threads` width.

    Directories first and serially, exactly as the sorted walk in the vendor phase
    guarantees: a parallel copy that also created parents would be measuring a race.
    """
    dirs, files = layout
    dest.mkdir(parents=True)
    for rel in dirs:
        (dest / rel).mkdir()

    def one(rel: str) -> None:
        copy(str(corpus / rel), str(dest / rel))

    if threads <= 1:
        for rel in files:
            one(rel)
    else:
        with ThreadPoolExecutor(max_workers=threads) as pool:
            list(pool.map(one, files, chunksize=64))


def external_copy(corpus: Path, dest: Path, clone: bool) -> None:
    """`cp -R`, the same work with no interpreter in the loop."""
    flags = "-Rc" if clone and sys.platform == "darwin" else "-R"
    command = ["cp", flags, str(corpus), str(dest)]
    if clone and sys.platform.startswith("linux"):
        command = ["cp", "-R", "--reflink=always", str(corpus), str(dest)]
    subprocess.run(command, check=True, capture_output=True)


def strategies(threads: list[int]) -> list[tuple[str, Callable]]:
    """Every candidate, as (name, run(corpus, dest, layout))."""
    out: list[tuple[str, Callable]] = [
        ("py-serial", lambda c, d, ly: copy_tree(c, d, ly, shutil.copyfile, 1))
    ]
    for n in threads:
        if n > 1:
            out.append(
                (
                    f"py-threads-{n}",
                    lambda c, d, ly, n=n: copy_tree(c, d, ly, shutil.copyfile, n),
                )
            )
    out.append(("py-clone", lambda c, d, ly: copy_tree(c, d, ly, clone_file, 1)))
    widest = max(threads)
    if widest > 1:
        out.append(
            (
                f"py-clone-{widest}",
                lambda c, d, ly: copy_tree(c, d, ly, clone_file, widest),
            )
        )
    out.append(("cp", lambda c, d, ly: external_copy(c, d, False)))
    out.append(("cp-clone", lambda c, d, ly: external_copy(c, d, True)))
    return out


# -- wiping -----------------------------------------------------------------------------
#
# The vendor phase wipes before it recopies, and once the copy got fast the wipe became
# the second-largest stage. std::filesystem::remove_all resolves every path from the root
# it was handed; the alternatives here are the two ways out of that -- stay relative to a
# directory fd, and spread the subtrees over threads.


def wipe_walk(root: Path) -> None:
    """os.walk bottom-up, unlinking by full path: what fs::remove_all does."""
    for base, dirs, files in os.walk(root, topdown=False):
        for name in files:
            os.unlink(os.path.join(base, name))
        for name in dirs:
            os.rmdir(os.path.join(base, name))
    os.rmdir(root)


def wipe_fd(root: Path) -> None:
    """openat/unlinkat, relative to each directory: one resolution per component, once."""

    def drop(fd: int) -> None:
        for entry in os.scandir(fd):
            if entry.is_dir(follow_symlinks=False):
                child = os.open(entry.name, os.O_RDONLY | os.O_DIRECTORY, dir_fd=fd)
                try:
                    drop(child)
                finally:
                    os.close(child)
                os.rmdir(entry.name, dir_fd=fd)
            else:
                os.unlink(entry.name, dir_fd=fd)

    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        drop(fd)
    finally:
        os.close(fd)
    os.rmdir(root)


def wipe_parallel(root: Path, threads: int) -> None:
    """Unlink every file across a pool, then rmdir deepest-first.

    Two passes for the same reason the copy has two: a directory cannot go until its
    children have, and rediscovering that ordering per worker costs more than the one
    sorted pass it replaces.
    """
    dirs, files = [], []
    for base, subdirs, names in os.walk(root):
        dirs.append(base)
        files.extend(os.path.join(base, n) for n in names)

    if files:
        with ThreadPoolExecutor(max_workers=threads) as pool:
            list(pool.map(os.unlink, files, chunksize=64))
    for base in sorted(dirs, key=len, reverse=True):
        os.rmdir(base)


def wipe_external(root: Path) -> None:
    subprocess.run(["rm", "-rf", str(root)], check=True, capture_output=True)


def wipe_strategies(threads: list[int]) -> list[tuple[str, Callable]]:
    out: list[tuple[str, Callable]] = [
        ("py-walk", wipe_walk),
        ("py-fd", wipe_fd),
    ]
    for n in threads:
        if n > 1:
            out.append((f"py-parallel-{n}", lambda r, n=n: wipe_parallel(r, n)))
    out.append(("rm -rf", wipe_external))
    return out


def sweep_wipe(profiles, root: Path, threads: list[int], reps: int, verbose: bool):
    """Build a tree the cheap way (clone), then time tearing it down."""
    records = []
    scratch = root / "vendor-wipe"
    for profile in profiles:
        corpus = generate(profile, root, verbose)
        layout = walk_corpus(corpus)

        for name, run in wipe_strategies(threads):
            samples = []
            for _ in range(reps):
                shutil.rmtree(scratch, ignore_errors=True)
                try:
                    copy_tree(corpus, scratch, layout, clone_file, max(threads))
                except OSError:  # no clone here; the copy is setup, not the measurement
                    copy_tree(corpus, scratch, layout, shutil.copyfile, max(threads))
                started = time.perf_counter()
                run(scratch)
                samples.append(time.perf_counter() - started)
            shutil.rmtree(scratch, ignore_errors=True)

            records.append(
                {
                    "profile": profile.name,
                    "strategy": name,
                    "files": len(layout[1]),
                    "dirs": len(layout[0]),
                    "min_s": min(samples),
                    "median_s": statistics.median(samples),
                    "files_per_s": len(layout[1]) / min(samples),
                }
            )
    return records


def wipe_table(records) -> str:
    baseline = {r["profile"]: r["min_s"] for r in records if r["strategy"] == "py-walk"}
    lines = [
        "| profile | strategy | files | dirs | min s | files/s | vs py-walk |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in records:
        speedup = baseline.get(r["profile"], 0) / r["min_s"] if r["min_s"] else 0
        lines.append(
            f"| {r['profile']} | {r['strategy']} | {r['files']} | {r['dirs']} | "
            f"{r['min_s']:.3f} | {r['files_per_s']:.0f} | {speedup:.2f}x |"
        )
    return "\n".join(lines)


# -- hashing a tree that was cloned, against one that was copied ------------------------
#
# The drift check hashes the destination on every run, and envy hashes far more often
# than it vendors, so a copy strategy that made later hashes slower would be a bad trade
# however fast the copy got. A clone writes no bytes, so the destination is only in the
# page cache if the source was -- this separates that one cold read from any standing
# cost of reading shared extents.


def hash_tree(envy: Path, root: Path, target: Path) -> int:
    """`envy hash --tree`, whose duration_ms times the hash and not the process."""
    result = subprocess.run(
        [str(envy), "hash", "--tree", "--json", str(target)],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise SystemExit(f"envy hash failed ({result.returncode}):\n{result.stderr}")
    return json.loads(result.stdout)[0]["duration_ms"]


def sweep_hash_after(envy, profiles, root: Path, reps: int, verbose: bool):
    """Hash the same tree repeatedly, once built by a clone and once by a byte copy.

    The first hash and the ones after it are reported separately: if a clone costs
    anything beyond its own cold read, the later hashes are where it would show.
    """
    records = []
    scratch = root / "vendor-hashafter"
    for profile in profiles:
        corpus = generate(profile, root, verbose)
        layout = walk_corpus(corpus)

        for how, copy in (("clone", clone_file), ("copy", shutil.copyfile)):
            firsts, laters, digests = [], [], set()
            for _ in range(reps):
                shutil.rmtree(scratch, ignore_errors=True)
                try:
                    copy_tree(corpus, scratch, layout, copy, 1)
                except OSError:
                    firsts = []
                    break
                firsts.append(hash_tree(envy, root, scratch))
                laters.extend(hash_tree(envy, root, scratch) for _ in range(2))
            shutil.rmtree(scratch, ignore_errors=True)
            if not firsts:
                records.append({"profile": profile.name, "built": how, "unsupported": 1})
                continue

            records.append(
                {
                    "profile": profile.name,
                    "built": how,
                    "files": len(layout[1]),
                    "first_ms": min(firsts),
                    "later_ms": min(laters),
                }
            )
    return records


def hash_after_table(records) -> str:
    lines = [
        "| profile | built by | files | first hash ms | later hash ms |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for r in records:
        if "unsupported" in r:
            lines.append(f"| {r['profile']} | {r['built']} | | unsupported | |")
            continue
        lines.append(
            f"| {r['profile']} | {r['built']} | {r['files']} | {r['first_ms']} "
            f"| {r['later_ms']} |"
        )
    return "\n".join(lines)


def sweep_probe(profiles, root: Path, threads: list[int], reps: int, verbose: bool):
    records = []
    scratch = root / "vendor-probe"
    for profile in profiles:
        corpus = generate(profile, root, verbose)
        layout = walk_corpus(corpus)
        total_bytes = sum(
            (corpus / rel).stat().st_size for rel in layout[1]
        )  # the corpus sentinel counts; it is part of the fixed shape

        for name, run in strategies(threads):
            samples, failure = [], None
            for _ in range(reps):
                shutil.rmtree(scratch, ignore_errors=True)
                started = time.perf_counter()
                try:
                    run(corpus, scratch, layout)
                except (OSError, subprocess.CalledProcessError) as exc:
                    failure = f"{type(exc).__name__}: {exc}"
                    break
                samples.append(time.perf_counter() - started)
            shutil.rmtree(scratch, ignore_errors=True)

            if failure:
                if verbose:
                    print(f"  {profile.name}/{name}: {failure}", file=sys.stderr)
                records.append(
                    {"profile": profile.name, "strategy": name, "unsupported": failure}
                )
                continue

            best = min(samples)
            records.append(
                {
                    "profile": profile.name,
                    "strategy": name,
                    "files": len(layout[1]),
                    "bytes": total_bytes,
                    "min_s": best,
                    "median_s": statistics.median(samples),
                    "mb_per_s": total_bytes / MIB / best,
                    "files_per_s": len(layout[1]) / best,
                }
            )
    return records


# -- reporting -------------------------------------------------------------------------


def envy_table(records) -> str:
    lines = [
        "| profile | scenario | threads | action | files | MiB | hash ms | wipe ms "
        "| copy ms | total ms | copy MiB/s | copy files/s |",
        "| --- | --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: "
        "| ---: |",
    ]
    for r in records:
        lines.append(
            f"| {r['profile']} | {r['scenario']} | {r['threads']} | "
            f"{r['action']} | {r['files']} | "
            f"{r['bytes'] / MIB:.0f} | {r['hash_ms']} | {r['wipe_ms']} | "
            f"{r['copy_ms']} | {r['min_ms']} | {r['copy_mb_per_s']:.0f} | "
            f"{r['copy_files_per_s']:.0f} |"
        )
    return "\n".join(lines)


def probe_table(records) -> str:
    baseline = {
        r["profile"]: r["min_s"]
        for r in records
        if r["strategy"] == "py-serial" and "min_s" in r
    }
    lines = [
        "| profile | strategy | files | MiB | min s | MiB/s | files/s | vs py-serial |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in records:
        if "unsupported" in r:
            lines.append(
                f"| {r['profile']} | {r['strategy']} | | | unsupported | | | |"
            )
            continue
        speedup = baseline.get(r["profile"], 0) / r["min_s"] if r["min_s"] else 0
        lines.append(
            f"| {r['profile']} | {r['strategy']} | {r['files']} | "
            f"{r['bytes'] / MIB:.0f} | {r['min_s']:.3f} | {r['mb_per_s']:.0f} | "
            f"{r['files_per_s']:.0f} | {speedup:.2f}x |"
        )
    return "\n".join(lines)


def verdict(probe) -> str:
    """Per shape, the fastest strategy and what it beat. The point of the exercise."""
    lines = ["| profile | fastest | vs py-serial |", "| --- | --- | ---: |"]
    by_profile: dict[str, list[dict]] = {}
    for r in probe:
        if "min_s" in r:
            by_profile.setdefault(r["profile"], []).append(r)
    for name, rows in by_profile.items():
        best = min(rows, key=lambda r: r["min_s"])
        serial = next((r for r in rows if r["strategy"] == "py-serial"), None)
        ratio = serial["min_s"] / best["min_s"] if serial and best["min_s"] else 0
        lines.append(f"| {name} | {best['strategy']} | {ratio:.2f}x |")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--envy", type=Path, default=ROOT / "out" / "build" / "envy")
    parser.add_argument(
        "--profile", action="append", choices=list(PROFILES_BY_NAME), default=None
    )
    parser.add_argument("--threads", help="comma-separated counts (default: 1,2,4,8,hw)")
    parser.add_argument("--reps", type=int, default=3)
    parser.add_argument(
        "--scale",
        type=int,
        default=8,
        help="divide every profile by this (vendoring writes the corpus three times)",
    )
    parser.add_argument("--bench-root", type=Path, default=DEFAULT_BENCH_ROOT)
    parser.add_argument("--skip-envy", action="store_true", help="only probe strategies")
    parser.add_argument("--skip-probe", action="store_true", help="only measure envy")
    parser.add_argument("--wipe", action="store_true", help="also probe wipe strategies")
    parser.add_argument(
        "--hash-after",
        action="store_true",
        help="also compare hashing a cloned tree against a copied one",
    )
    parser.add_argument("--json", type=Path, help="write records here")
    parser.add_argument("--summary", type=Path, help="append the tables here")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    envy = args.envy
    if sys.platform == "win32" and envy.suffix != ".exe":
        envy = envy.with_suffix(".exe")
    if not args.skip_envy and not envy.is_file():
        raise SystemExit(f"envy not found at {envy}; run ./build.sh first")

    selected = [
        PROFILES_BY_NAME[name] for name in (args.profile or list(PROFILES_BY_NAME))
    ]
    selected = [p.scaled(args.scale) for p in selected]
    args.bench_root.mkdir(parents=True, exist_ok=True)

    started = time.monotonic()
    verbose = not args.quiet
    counts = thread_counts(args.threads)
    envy_records = (
        []
        if args.skip_envy
        else sweep_envy(envy, selected, args.bench_root, args.reps, verbose, counts)
    )
    probe_records = (
        []
        if args.skip_probe
        else sweep_probe(selected, args.bench_root, counts, args.reps, verbose)
    )

    wipe_records = (
        sweep_wipe(selected, args.bench_root, counts, args.reps, verbose)
        if args.wipe
        else []
    )

    hash_records = (
        sweep_hash_after(envy, selected, args.bench_root, args.reps, verbose)
        if args.hash_after
        else []
    )

    parts = []
    if envy_records:
        parts.append("#### what envy does now\n\n" + envy_table(envy_records))
    if probe_records:
        parts.append("#### copy strategies on the same corpus\n\n" + probe_table(probe_records))
        parts.append("#### fastest per shape\n\n" + verdict(probe_records))
    if wipe_records:
        parts.append("#### wipe strategies on the same tree\n\n" + wipe_table(wipe_records))
    if hash_records:
        parts.append(
            "#### hashing a cloned tree vs a copied one\n\n"
            + hash_after_table(hash_records)
        )
    rendered = "\n\n".join(parts)

    if not args.quiet:
        print(rendered)
        print(
            f"\nswept {len(envy_records) + len(probe_records) + len(wipe_records) + len(hash_records)} "
            f"configurations in "
            f"{time.monotonic() - started:.1f}s; warm page cache"
        )
    if args.json:
        args.json.write_text(
            json.dumps(
                {
                    "envy": envy_records,
                    "probe": probe_records,
                    "wipe": wipe_records,
                    "hash_after": hash_records,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    if args.summary:
        with args.summary.open("a", encoding="utf-8") as handle:
            handle.write("### vendor benchmark\n\n" + rendered + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
