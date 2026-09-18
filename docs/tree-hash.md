# Subtree Hashing

One digest for a whole directory tree, used by vendoring to tell an untouched copy from an
edited one. `envy hash --tree <dir>` prints it; `src/tree_hash.h` is the single entry
point.

## Digest

Folded over every selected entry in sorted relative-path order:

```
<relpath> NUL <kind> <exec> <payload>
```

`kind` is `f`/`d`/`l`; `exec` is the owner-execute bit (always `0` on Windows, which has
no such concept). Payload is the file's BLAKE3 for `f`, the target string plus a NUL for
`l`, and nothing for `d`. Directories fold in, so an added or removed empty directory is a
difference. Symlinks are hashed by their *stored* target and never followed, so a digest
does not depend on where the tree sits.

A selection always contains the directories holding its entries, even when the filter does
not name them. `--only '**/*.h'` selects two headers *and* the `include/` they live in,
because a copy of that selection has to create it. This is what makes the digest of a
selection equal the digest of a copy of it, which is the whole basis of the vendor
up-to-date check: a destination is hashed whole and compared against the digest its
package recorded, with no state kept on the project side.

These bytes are the stamped format—append to it, never renumber. A change invalidates
every vendor stamp in every cache, which is what the known-answer test in
`src/tree_hash_tests.cpp` exists to make deliberate.

## Selectors

`--only`, a spec's `VENDOR`, and `envy extract --only` are one selector language matched
by one implementation (`src/glob.h`: `glob_filter`, `glob_parse_filter`, `glob_match`) —
`*`, `?`, `**`, `[a-z]`, `[!a-z]`, literal-names-a-subtree, with a leading `!` to exclude. An entry is
selected when the include list is empty or one pattern matches, and no exclude pattern
does. The walk still descends into an unselected directory, so `!build` does not hide a
selected `build/keep.txt`.

## Implementation

`tree_hash.cpp` is the portable driver; `tree_hash_posix.cpp` and `tree_hash_win.cpp`
supply native traversal. Same split `platform.h` uses for `dir_sizes`.

Nothing here owns a hasher or a reader of its own. BLAKE3 lives once in `blake3_util.h`
as `blake3_stream`, with the one-shot `blake3_hash()` built on it; reading a file's bytes
lives once in `file_read.h` as `file_read_chunks`, with `file_read_posix.cpp` /
`file_read_win.cpp` behind it. The per-file digest (`sha256`) reads through exactly that,
so read sizing, readahead hints and short-read handling are decided in one place for both
digests.

Workers drain a LIFO queue of directories and BLAKE3 whole files on the thread that read
them, so throughput scales with cores rather than funnelling chunks through one hasher.
Results accumulate per worker and merge once at the end; the shared lock is taken once per
directory. Reads use a per-worker buffer—a fresh megabyte per file costs more to zero than
a small file costs to read—with `F_RDAHEAD` (macOS), `posix_fadvise` (Linux), or a pair of
overlapped reads (Windows). The hook boundary is where an io_uring backend drops in
without touching the driver.

Strict where `dir_sizes` is best-effort: an unreadable entry throws. A wrong size is
cosmetic; a digest that silently omitted data would report "unchanged" for a tree it never
read.

Concurrency budget: every package worker is its own thread and any of them may be hashing,
so a call with `threads == 0` takes `hardware_concurrency / <calls in flight>`.

## Profiling

`envy hash --tree --stats <dir>` reports where the time went and how evenly it was
spread. **The report goes to stderr; stdout stays the digest line alone**, so a hash still
pipes into a checksum file or a diff with `--stats` on. Under `--json` the same numbers
ride inside the JSON object on stdout, where a consumer parsing that object wants them.

Stage times are summed across workers, so they exceed wall time on a parallel run; the
ratio is the useful part.

```
  threads 6  dirs 73  files 50001  bytes 204800038  wall 1136.4ms
  scan    132.9ms   2.0%   read   6472.3ms  96.3%
  hash    104.7ms   1.6%   wait      9.5ms   0.1%   fold      4.1ms
  files/worker 8301..8355 (ideal 8333)   bytes/worker 34000896..34222080 (ideal 34133339)
```

- `scan` — enumerating directories (`readdir` + `fstatat`)
- `read` — `open`/`pread`/`close`, net of the hashing that interleaves with it
- `hash` — BLAKE3 over file contents
- `wait` — blocked on the work queue; a large share means workers are starving
- `fold` — sorting entries and folding them into one digest
- `files/bytes per worker` — the balance report. Bytes matter as much as files: a tree of
  one huge file and a thousand small ones balances perfectly by count while one worker
  does all the hashing.

The timers are two clock reads per file, so they are off unless asked for.

### What profiling found

**Reads dominate, and they are at the OS floor.** Across every corpus shape, `read` is
70–98% of the time, `hash` is 0.2–21%, `scan` is 1–16%, and `wait` is near zero. Per-file
cost matches a bare `open`/`read`/`close` loop in Python on the same files (~12 µs warm on
macOS/APFS), so the remaining cost is the kernel's, not this code's.

That shapes what is worth trying:

- **More threads help only when the work is syscall-bound.** `wide` (100k tiny files) runs
  1.6x faster at 12 threads than at 6; `many-small` (which has real hashing to do) runs 4x
  *slower* at 16 than at 4. The optima genuinely conflict, so the default — the
  performance-core count — is the best fixed choice, and `--threads` is there for anyone
  who knows their workload.
- **`openat` from a cached directory fd** saves 9% of open cost on a five-level tree and 2%
  on a flat one. Not worth threading a descriptor's lifetime through the work queue.
- **Longest-processing-time scheduling** (largest files popped first) improved byte balance
  but cost wall time and raised starvation, so it is not in. `mixed` balances to about
  2.2:1 as it stands, against a floor set by its largest indivisible file.
- **Allocation removal** (reused scan buffers, allocation-free selector matching,
  single-allocation path joins) measured within noise here, which is what a 96%-read
  profile predicts. It is in anyway: it costs nothing and the ratio shifts on a filesystem
  whose syscalls are cheaper.

## Benchmark

```
python3 tools/bench_tree_hash.py                  # full sweep, human table
python3 tools/bench_tree_hash.py --quick          # what CI runs, ~1/10 scale
python3 tools/bench_tree_hash.py --profile deep --threads 1,4 --reps 3
```

| profile | shape | stresses |
|---|---|---|
| `many-small` | 50,000 x 4 KiB, 3 deep | walk and syscall rate |
| `few-large` | 8 x 512 MiB | read bandwidth and BLAKE3 throughput |
| `mixed` | ~2 GiB, a long tail under a few big files | a realistic toolchain payload |
| `deep` | 10,000 dirs, 5 levels | directory-queue contention |
| `wide` | 100,000 files in one directory | batched enumeration |

Corpora are generated from each file's relative path, so a profile is byte-identical on
every platform and the sweep asserts its digest while timing it. They are cached under
`out/bench/`. Timings come from `duration_ms` in `envy hash --tree --json`, which measures
the hash alone—every envy command self-deploys on startup. The page cache is warm; no CI
runner lets you drop it portably.

CI runs `--quick --stats` on the six `main` shards, uploads the JSON — stage breakdown
included, so a regression can be attributed without a reproduction — and writes the table
to the step summary. Both guards — a 200 MiB/s floor on `few-large` and a 1.5x
single-to-many scaling check — only annotate. A runner's storage is its own business, and
a spinning disk or a throttled shared volume is not a regression in this code. The one
thing that fails the job is a digest that changed with thread count: that is correctness,
not speed.

To judge a change, diff two JSON runs from the same shard name.

The per-platform smoke test also runs `envy hash --tree --stats src` on envy's own source
and prints the breakdown. That is a look at how the hasher behaves on each runner, not a
check: it asserts nothing and names no thread count.
