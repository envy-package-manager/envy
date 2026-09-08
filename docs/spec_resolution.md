# Spec Resolution and Execution

How a manifest becomes a running dependency graph. Manifest and spec syntax live in
`docs/architecture.md`; this is the resolution and scheduling model.

## Core Model

**One graph.** Spec fetching and package building are the same ladder, not two passes:
`spec_fetch` discovers dependencies and spawns their tasks while other packages are
already building.

**Node identity.** A node is `(identity, options)`, named by the canonical key
`identity{["key"]=value,…}` — options serialized as a Lua table literal with keys
quoted and sorted, control bytes escaped, so the string round-trips and never collides.
Empty options drop the braces entirely. Everything user-facing (results, traces, locks,
messages) uses that string.

**Queries** match a key exactly, or by `name`, `namespace.name`, or `name@revision`.
Only a `.` *before* the `@` separates namespace from name, so `arm.gcc@13.2.0` answers
`gcc`, `arm.gcc` and `gcc@13.2.0` alike.

**Phases.** Each node runs the fixed ladder (`spec_fetch`, `check`, `import`, `fetch`,
`stage`, `build`, `install`, `setup`, `export`, completion); a phase whose verb is absent
is a no-op step. Scheduling is `task_engine`: keyed tasks, linear steps, ratcheting
target watermarks, per-step edges, tasks created at any time.

**Edges.** A dependency edge holds the dependent's `needed_by` phase until the dependency
completes `setup`, while ratcheting it through `export` — so export overlaps dependents'
builds, and host state is in place before anyone uses the payload.

## Dependency Kinds

| Kind | Written | Resolution |
|---|---|---|
| Strong | `spec` + `source` (or `bundle`) | wired and started as the declaring spec loads |
| Weak | a query plus `weak = { … }` | matched against the graph; fallback spawned only if nothing matches |
| Reference-only | a query, no `source`, no `weak` | must be satisfied by some other package |
| Product | `product = "name"` (any of the above) | see `docs/products.md` |
| Fetch prerequisite | inside `source.dependencies` | strong only; always blocks the declarer's `spec_fetch` |

`needed_by` defaults to `build`. Weak fallbacks inherit the weak entry's `needed_by` and
may not restate it.

## Graph Construction

`resolve_graph` interns every root before starting any worker (so each manifest entry's
SETUP selection is merged before a dependency thread can race it), interns
`DEFAULT_SHELL.DEPENDS` (interned only — the `#default_shell` task starts them, so a run
that never needs a shell never builds the interpreter), then starts each root toward
`spec_fetch`.

Each `spec_fetch`:
1. fetches the spec (declarative source, custom fetch, or a spec out of a bundle);
2. loads the Lua and checks `IDENTITY` against what was requested;
3. reads `PRODUCTS` (published to the registry immediately), `SETUP`, `PLATFORMS`,
   `OPTIONS`, `USER_MANAGED`;
4. parses `DEPENDENCIES` — one error context per entry (`spec '<id>': DEPENDENCIES[i]`);
5. wires each strong entry with `engine::wire_dependency` and starts it toward
   `spec_fetch`; records weak and reference-only entries for the resolution pass.

`ensure_pkg` memoizes on the canonical key: the first caller allocates, everyone else
reuses. Two cfgs on one key must agree on their source (see below).

Then the loop: wait until no `spec_fetch` is in flight, run one weak resolution pass,
repeat while it makes progress. Progress is "something resolved or a
fallback was spawned"; no progress with references left is an error listing them.
Finally `validate_product_fallbacks` and `validate_setup_selections` run once, against a
fully resolved graph.

`run_full` adds the rest: extend every task to completion, join, then throw one error
built from *all* failures, deduplicated and sorted (a dependent re-reports its
dependency's message verbatim, so duplicates are the common case).

## Cycle Detection

**Detection strategy:** `engine::wire_dependency` is the only place an edge is added.
Before adding `parent → dep` it walks existing edges from `dep`; if that walk reaches
`parent`, the edge would close a cycle and the walk's path is the error message. The
check and the insert share the engine mutex, so two workers cannot both find no cycle and
then both add their edge. Reachability, not the spawn path: an edge is refused however
the target got back to the parent — a second manifest root, a weak fallback, a
`source.dependencies` prerequisite.

**Illegal cycle example:**
```lua
-- local.a@v1
DEPENDENCIES = { { spec = "local.b@v1", source = "b.lua", needed_by = "build" } }

-- local.b@v1
DEPENDENCIES = { { spec = "local.a@v1", source = "a.lua", needed_by = "stage" } }
```

Each waits on the other's `setup` → deadlock. Envy refuses B's edge to A, since A already
reaches B.

**Error message:** "Dependency cycle detected: B@v1 -> A@v1 -> B@v1" (`Fetch dependency` /
`Bundle dependency` prefix when the edge came from `source.dependencies` or a bundle).

**Phase-agnostic:** the check ignores `needed_by` — any cycle in the edge graph is
refused, whichever phases the edges couple.

**One edge per (parent, identity):** the map is keyed by identity, so repeat declarations
merge, keeping the *earliest* `needed_by` anyone asked for. Two option variants of one
identity in a single dependency list have no representable answer and are a parse error.

**Deadlock watchdog:** a cycle no static check can see (a Lua-level wait, a lock ordering
mistake) still ends as an error rather than a hang. Waits are timed; if no task is
running, nothing advanced for a whole interval, and no blocked wait can proceed, the
engine fails every task with a report naming each blocked waiter:

```
Deadlock: no task is running while 2 wait(s) are blocked:
  local.a@v1 step 5 waits for local.b@v1@8
  local.b@v1 step 4 waits for local.a@v1@8
```

## Spec Fetching

### Identity Declaration

Every spec declares `IDENTITY = "namespace.name@revision"`, and envy checks it against the
identity that asked for it — catching typos, stale references, copy-paste errors, and
substitution at a compromised source. No namespace exemptions; this is orthogonal to
SHA256, which is about transport trust.

### Declarative Sources

`source` on the entry names the spec file: an http/https/s3/ftp URL (optionally
`sha256`), a git URL (`ref` required), or a path. A path with no `sha256` is loaded in
place and never cached; with one it is verified and cached like a remote.

### Custom Fetch

`source = { fetch = function, dependencies = { … } }` on the entry that *depends* on the
spec — a spec's `DEPENDENCIES`, or a `BUNDLES` declaration. Not on a manifest `PACKAGES`
entry and not inside `weak = { … }`: both are rejected at parse, because nothing could
ever find the closure again to call it.

```lua
-- in corp.toolchain@r1's DEPENDENCIES
{ spec = "corp.gcc@r1", options = { channel = "beta" }, source = {
    dependencies = { { spec = "tools.jfrog-cli@r1", source = "jfrog.lua" } },
    fetch = function(tmp_dir, options)          -- options.channel == "beta"
      envy.run(envy.product("jf") .. " rt dl specs/" .. options.channel .. " " .. tmp_dir)
      envy.commit_fetch({ "spec.lua", "lib" })  -- both land in the spec directory
    end } }
```

- The closure runs in the declaring file's Lua state but *as the child package*: the
  child's `source.dependencies` are the edges `envy.package`/`envy.product` authorize
  against, and `options` is the entry's own, not the declarer's.
- `source.dependencies` are wired at `needed_by = spec_fetch` and installed first. Strong
  only: the weak pass runs at a barrier that waits for every `spec_fetch`, including that
  of the consumer waiting on this fetch, so nothing weak could be ordered in time. The
  same holds transitively — no package in the closure may hold a weak reference.
- Sibling fetches declared in one file serialize on that file's Lua lock. It cannot be
  released around the call: the closure, its upvalues and the options all live there.
- The cache entry is keyed on declaring file + identity + options, a closure having no
  fingerprint. Two option variants of one entry therefore fetch separately; editing the
  function body in place reuses the entry.
- `spec.lua` is required. Every other committed file survives beside it, so `require` and
  `envy.loadenv` reach the siblings.

### Cache Layout

```
~/.cache/envy/specs/corp.gcc@r1/blake3-{source_hash}/
├── envy-complete       # written only once the spec loads with the right IDENTITY
├── pkg/
│   ├── spec.lua        # entry point (required)
│   └── lib/…           # everything else the fetch committed
├── fetch/              # committed files land here first
└── work/tmp/           # the tmp_dir handed to the fetch function
```

## Conflicting Declarations

The key is identity plus options and holds no source, so every declaration of one key
must agree on what to fetch. This is checked for every source kind, not just bundles:

- provably different (URL, `sha256`, path, `ref`, bundle identity) → error naming both
  declaring files;
- two custom fetch closures → undecidable, so the first wins with a warning — unless both
  are copies of one declaration (an alias or a prior entry named twice), which is silent;
- a reference-only entry names no payload, so it agrees with whatever wins the key.

One dependency list naming an identity twice with *different* options is a parse error:
the map is identity-keyed and could not represent it.

**Security:** a non-`local.*` spec may not depend on a `local.*` spec; envy refuses at
parse time.

## Error Propagation

Parse and wiring errors (unknown keys, cycles, conflicting sources, `local.*` violations)
throw from the worker that found them and are reported with the entry's context.
Execution failures mark the task failed; dependents fail with the same message, and
independent branches keep running. `run_full` aggregates, deduplicates and sorts every
message into one error, so a broken shared dependency reports once.
