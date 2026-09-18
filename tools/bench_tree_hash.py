#!/usr/bin/env python3
"""Benchmark `envy hash --tree` across corpus shapes and thread counts.

Corpus generation and the sweep live here rather than in the shipped binary: they are dev
tooling, not product surface. What is measured is `duration_ms` out of
`envy hash --tree --json`, which times the hash itself -- every envy command self-deploys
on startup, so timing the subprocess would fold that in.

Corpora are deterministic (content is derived from each file's relative path), so a
profile hashes to the same digest on every platform and run. The sweep asserts that,
which turns the benchmark into a correctness check that happens to report throughput.

    python3 tools/bench_tree_hash.py                 # full sweep, human table
    python3 tools/bench_tree_hash.py --quick         # what CI runs
    python3 tools/bench_tree_hash.py --json out.json --summary "$GITHUB_STEP_SUMMARY"

Caches are warm: no CI runner lets you drop them portably, and a cold-cache number nobody
can reproduce is worse than a warm one everybody can.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CORPUS_ROOT = ROOT / "out" / "bench"


@dataclass(frozen=True)
class Profile:
    """One corpus shape. `layers` is (count, depth, size_bytes) per group of files."""

    name: str
    layers: tuple[tuple[int, int, int], ...]
    fanout: int = 8
    quick: bool = False

    def scaled(self, divisor: int) -> "Profile":
        """Same shape at roughly 1/divisor the files *and* bytes.

        Scaling the file count alone would leave `few-large` generating gigabytes: eight
        files divided by anything is still one file, and that file was 512 MiB. Scaling it
        to one file instead makes per-file parallelism unmeasurable, so a layer keeps at
        least four files (or all of them, if it had fewer).
        """
        if divisor <= 1:
            return self
        layers = []
        for count, depth, size in self.layers:
            new_count = max(min(count, 4), count // divisor)
            new_size = max(1, (count * size // divisor) // new_count)
            layers.append((new_count, depth, new_size))
        return Profile(self.name, tuple(layers), self.fanout, self.quick)

    @property
    def total_bytes(self) -> int:
        return sum(count * size for count, _, size in self.layers)

    @property
    def total_files(self) -> int:
        return sum(count for count, _, _ in self.layers)


KIB = 1024
MIB = 1024 * 1024

PROFILES = (
    # Walk and syscall rate: the per-file overhead dominates, not the hashing.
    Profile("many-small", ((50_000, 3, 4 * KIB),), quick=True),
    # Read bandwidth and BLAKE3 throughput, with almost no walk at all.
    Profile("few-large", ((8, 1, 512 * MIB),), quick=True),
    # A realistic toolchain payload: a long tail of headers under a few big archives.
    Profile("mixed", ((20_000, 4, 8 * KIB), (40, 2, 8 * MIB), (4, 1, 256 * MIB))),
    # Directory-queue contention: many directories, almost no bytes.
    Profile("deep", ((10_000, 5, 64),), fanout=2),
    # A single enormous directory, where batched enumeration is the whole story.
    Profile("wide", ((100_000, 1, 256),), fanout=1_000_000),
)

PROFILES_BY_NAME = {p.name: p for p in PROFILES}


BLOCK = 64 * 1024


def content_for(rel: str, size: int) -> bytes:
    """Deterministic bytes for one path: a SHAKE-256 block keyed by the path, tiled.

    Same corpus on every platform and every run, which is what lets the sweep assert the
    digest instead of merely timing it. Tiled rather than streamed because generating a
    gigabyte a word at a time in Python takes longer than the benchmark it feeds; the
    hasher is content-blind, so a repeating block measures exactly what a random one does.
    """
    if not size:
        return b""
    block = hashlib.shake_256(rel.encode()).digest(min(size, BLOCK))
    if size <= len(block):
        return block[:size]
    return (block * (size // len(block) + 1))[:size]


def corpus_dir(profile: Profile, root: Path) -> Path:
    # The shape is in the directory name, so a rescaled profile never reuses the corpus
    # of a differently-sized one.
    shape = hashlib.sha256(repr((profile.layers, profile.fanout)).encode()).hexdigest()[:12]
    return root / f"{profile.name}-{shape}"


def generate(profile: Profile, root: Path, verbose: bool) -> Path:
    path = corpus_dir(profile, root)
    sentinel = path / ".complete"
    if sentinel.exists():
        if verbose:
            print(f"  reusing corpus {path}", file=sys.stderr)
        return path

    if path.exists():
        shutil.rmtree(path)
    if verbose:
        print(
            f"  generating {profile.name}: {profile.total_files} files, "
            f"{profile.total_bytes / MIB:.0f} MiB",
            file=sys.stderr,
        )

    index = 0
    for count, depth, size in profile.layers:
        for i in range(count):
            parts = []
            branch = i
            for level in range(depth - 1):
                parts.append(f"d{level}_{branch % profile.fanout}")
                branch //= max(1, profile.fanout)
            parts.append(f"f{index:07d}.bin")
            rel = "/".join(parts)
            target = path / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content_for(rel, size))
            index += 1

    sentinel.write_text("generated by tools/bench_tree_hash.py\n", encoding="utf-8")
    return path


def hash_once(envy: Path, corpus: Path, threads: int, stats: bool = False) -> dict:
    result = subprocess.run(
        [str(envy), "hash", "--tree", "--json", "--threads", str(threads)]
        + (["--stats"] if stats else [])
        + [str(corpus)],
        capture_output=True,
        text=True,
        encoding="utf-8",
    )
    if result.returncode != 0:
        raise SystemExit(f"envy hash failed ({result.returncode}):\n{result.stderr}")
    records = json.loads(result.stdout)
    return records[0]


def thread_counts(requested: str | None) -> list[int]:
    if requested:
        return [int(n) for n in requested.split(",")]
    hw = os.cpu_count() or 4
    counts = [n for n in (1, 2, 4, 8) if n <= hw]
    if hw not in counts:
        counts.append(hw)
    return counts


# Stage times, summed across workers, so they exceed wall time; the ratio says whether a
# tree is bound by directory reads, file reads, the hasher, or its queue.
STAGE_KEYS = ("dirs", "scan_ns", "read_ns", "hash_ns", "wait_ns", "fold_ns")


def sweep(
    envy: Path,
    profiles,
    root: Path,
    threads: list[int],
    reps: int,
    verbose: bool,
    stats: bool = False,
):
    records = []
    for profile in profiles:
        corpus = generate(profile, root, verbose)
        # The corpus's own sentinel is inside it, so the digest covers one extra tiny
        # file; that is fine, it is part of the fixed shape being compared.
        digest = None
        for n in threads:
            hash_once(envy, corpus, n)  # warm-up; the first read is the only cold one
            samples = []
            for _ in range(reps):
                record = hash_once(envy, corpus, n)
                samples.append(record["duration_ms"])
                if digest is None:
                    digest = record["digest"]
                elif digest != record["digest"]:
                    raise SystemExit(
                        f"{profile.name}: digest changed with thread count "
                        f"({digest} vs {record['digest']}) -- the hasher is not "
                        f"deterministic"
                    )
            best = min(samples) or 1
            row = {
                "profile": profile.name,
                "threads": n,
                "files": record["files"],
                "bytes": record["bytes"],
                "min_ms": min(samples),
                "median_ms": statistics.median(samples),
                "mb_per_s": record["bytes"] / MIB / (best / 1000.0),
                "files_per_s": record["files"] / (best / 1000.0),
                "digest": digest,
            }
            if stats:
                # One extra untimed run, so the stage breakdown never taxes the number
                # being reported.
                detail = hash_once(envy, corpus, n, stats=True)
                row.update({k: detail[k] for k in STAGE_KEYS})
                row["files_per_worker"] = detail["files_per_worker"]
            records.append(row)
    return records


def table(records) -> str:
    lines = [
        "| profile | threads | files | MiB | min ms | median ms | MiB/s | files/s |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for r in records:
        lines.append(
            f"| {r['profile']} | {r['threads']} | {r['files']} | "
            f"{r['bytes'] / MIB:.0f} | {r['min_ms']} | {r['median_ms']:.0f} | "
            f"{r['mb_per_s']:.0f} | {r['files_per_s']:.0f} |"
        )
    if all("read_ns" in r for r in records) and records:
        lines.append("")
        lines.append("| profile | threads | scan | read | hash | wait |")
        lines.append("| --- | ---: | ---: | ---: | ---: | ---: |")
        for r in records:
            busy = r["scan_ns"] + r["read_ns"] + r["hash_ns"] + r["wait_ns"] or 1
            share = lambda k: 100.0 * r[k] / busy  # noqa: E731
            lines.append(
                f"| {r['profile']} | {r['threads']} | {share('scan_ns'):.0f}% | "
                f"{share('read_ns'):.0f}% | {share('hash_ns'):.0f}% | "
                f"{share('wait_ns'):.0f}% |"
            )
    lines.append("")
    lines.append("Warm page cache; `duration_ms` excludes process startup.")
    return "\n".join(lines)


# Deliberately far below what any working implementation reaches: this catches a
# serialized queue or a lock in the inner loop, not a regression worth arguing about.
FLOOR_MB_PER_S = 200.0
SCALING_RATIO = 1.5


def check_guards(records) -> int:
    """Hard floor fails; the scaling check only annotates. Shared runners are noisy."""
    failures = 0
    large = [r for r in records if r["profile"] == "few-large"]
    if not large:
        return 0

    fastest = max(large, key=lambda r: r["threads"])
    if fastest["mb_per_s"] < FLOOR_MB_PER_S:
        print(
            f"::error::tree-hash throughput {fastest['mb_per_s']:.0f} MiB/s is below the "
            f"{FLOOR_MB_PER_S:.0f} MiB/s floor",
            file=sys.stderr,
        )
        failures += 1

    single = next((r for r in large if r["threads"] == 1), None)
    hw = os.cpu_count() or 1
    if single and hw >= 4 and fastest["threads"] > 1:
        ratio = fastest["mb_per_s"] / max(single["mb_per_s"], 1e-9)
        if ratio < SCALING_RATIO:
            print(
                f"::warning::tree-hash scaled {ratio:.2f}x from 1 to "
                f"{fastest['threads']} threads (expected >= {SCALING_RATIO}x)",
                file=sys.stderr,
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--envy", type=Path, default=ROOT / "out" / "build" / "envy")
    parser.add_argument(
        "--profile", action="append", choices=list(PROFILES_BY_NAME), default=None
    )
    parser.add_argument("--threads", help="comma-separated counts (default: 1,2,4,8,hw)")
    parser.add_argument("--reps", type=int, default=5)
    parser.add_argument(
        "--quick",
        action="store_true",
        help="the two cheapest profiles at 1/32 scale, for CI",
    )
    parser.add_argument("--corpus-root", type=Path, default=DEFAULT_CORPUS_ROOT)
    parser.add_argument("--json", type=Path, help="write records here")
    parser.add_argument("--summary", type=Path, help="append the table here")
    parser.add_argument(
        "--stats",
        action="store_true",
        help="also record the per-stage breakdown (an extra untimed run per row)",
    )
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    envy = args.envy
    if sys.platform == "win32" and envy.suffix != ".exe":
        envy = envy.with_suffix(".exe")
    if not envy.is_file():
        raise SystemExit(f"envy not found at {envy}; run ./build.sh first")

    selected = (
        [PROFILES_BY_NAME[name] for name in args.profile]
        if args.profile
        else [p for p in PROFILES if p.quick]
        if args.quick
        else list(PROFILES)
    )
    if args.quick:
        selected = [p.scaled(32) for p in selected]

    args.corpus_root.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    records = sweep(
        envy,
        selected,
        args.corpus_root,
        thread_counts(args.threads),
        args.reps,
        not args.quiet,
        args.stats,
    )

    rendered = table(records)
    if not args.quiet:
        print(rendered)
        print(f"\nswept {len(records)} configurations in {time.monotonic() - started:.1f}s")
    if args.json:
        args.json.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    if args.summary:
        with args.summary.open("a", encoding="utf-8") as handle:
            handle.write("### tree-hash benchmark\n\n" + rendered + "\n")

    return 1 if check_guards(records) else 0


if __name__ == "__main__":
    sys.exit(main())
