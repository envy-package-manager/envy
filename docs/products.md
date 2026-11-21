# Products

A spec advertises entry points as `PRODUCTS`, a name→value map; consumers ask for the
name instead of an identity, so a manifest can swap providers without editing specs.
For a cache-managed package the value is a path relative to the installed package; for a
user-managed one it is an arbitrary string (typically a command the SETUP pairs put on
`PATH`). `envy product` queries the resolved graph.

## Declaring

```lua
PRODUCTS = {                        -- or function(options) returning the same table
  jf = "bin/jf" .. envy.EXE_EXT,    -- string shorthand for { value = ..., script = true }
  clangd = { value = "bin/clangd", script = false, platforms = { "linux-x86_64" } },
}
```

Keys are printable ASCII minus shell metacharacters (`" ' $ \` % \ !`) — they become
deployed script names. Values are non-empty, and for a cache-managed spec must be a
relative path with no `..` component. `platforms` narrows one product; the effective
constraint is its intersection with the spec's resolved platforms (spec `PLATFORMS` and
the manifest entry's `platforms`). An OS covers its arches, so granularity may differ:
`{ "linux" }` and `{ "linux-x86_64" }` meet at `linux-x86_64`; disjoint lists deploy no
script. `script = false`
suppresses the deployed wrapper (see `docs/commands.md` for `deploy`).

## Commands

A product may be an argv instead of a path — a script run by another product's
interpreter, or a tool that needs fixed flags. Every `{{name}}` is a product:

```lua
DEPENDENCIES = { { product = "node" }, { product = "java" } }
PRODUCTS = {
  ["yarn.cjs"]   = { value = "lib/yarn.cjs", script = false },   -- a file the command runs
  yarn           = { command = { "{{node}}", "{{yarn.cjs}}" } },
  ["gradle.jar"] = { value = "lib/gradle.jar", script = false },
  gradle         = { command = { "{{java}}", "-Xmx2g", "-jar", "{{gradle.jar}}" } },
  ci             = { command = { "{{yarn}}", "install", "--frozen-lockfile" } },  -- splices
}
```

- `command` excludes `value`; `script` and `platforms` apply as before. A list with holes
  or non-string entries is an error.
- `{{name}}` is a product of the provider or its dependency closure; the registry admits
  one provider per name, so it names exactly one. In the graph but outside the closure is
  an error, checked once resolution converges (`validate_product_commands`) for every
  command whose `platforms` admit the host. A command's own files are path products of
  its package. An edge that exists only for a command can say `needed_by = "install"`.
- A path product may sit inside an element (`--node={{node}}`); a command must fill a whole
  element, and its argv splices in. Cycles are errors. `{{` has no escape.
- Rendered at each use, never at install: nothing lands in the provider's payload, and
  its cache root can move. A consumer that bakes a command's output into its own payload
  is keyed only by its own references (see Cache key), as with any transitive tool.
- The program: absolute as is; bare is searched on `PATH` — so user-managed products take
  arguments too, and a PowerShell alias (`curl`) never shadows the program. A literal
  relative program is refused in a cache-managed spec (name the file as a path product);
  a user-managed relative value anchors at the project root under `--run`.
- `{{ }}` echoes `envy.template`, but these names are products filled in at use; build a
  command with `..`, not `envy.template`, which rejects names it was not given.

## Depending on a product

A dependency entry carries `product =`; the entry's `spec` (when present) pins which
provider is acceptable.

```lua
DEPENDENCIES = {
  { product = "jf", spec = "corp.jfrog@r1", source = "jfrog.lua" },  -- strong
  { product = "python3", weak = { spec = "corp.py@r3", source = "py.lua" } },  -- weak
  { product = "cmake" },                                             -- reference-only
}
```

- **Strong** (has a `source`): wired at parse like any other edge; the registry is never
  consulted, and `envy.product("jf")` resolves through that edge.
- **Weak**: resolved from the registry; if nobody provides the name, the fallback is
  interned and started. A fallback must provide the product itself or transitively —
  `validate_product_fallbacks` walks it after convergence and fails the run otherwise.
- **Reference-only** (no `source`, no `weak`): must be satisfied by some provider in the
  graph; still unresolved after convergence is an error naming the consumer.

Two entries may not name one product in a single spec (duplicate product dependency),
and a resolved provider must match the entry's `spec` if it gave one.

## Registry and collisions

`engine::register_products` publishes a package's names from its own worker as
spec_fetch completes — eager, not batched at the resolution barrier, so a consumer whose
edge already dragged the provider through export always sees the entry. Names are
snapshotted under `deps_mutex` and published under the engine `mutex_`, sequentially, so
the resolution loop's `mutex_` → `deps_mutex` order is never inverted. A second provider
for a registered name throws from that worker; the message sorts the two identities,
because which one registered first is scheduling-dependent. Every read goes through the
locked `find_product_provider`.

## Resolution and access

`resolve_product_ref` takes the registry's answer as an argument (never traversing the
map itself), checks the identity constraint, then wires the edge through
`engine::wire_dependency` like any other — cycles refused, `needed_by` min-merged. The
outcome is traced `product_resolved{product, provider, via}` with `via` = `registry` or
`fallback`.

`envy.product(name)` requires a *direct* edge to the provider whose `needed_by` the
current phase has reached; a provider reachable only transitively is refused. The value
is the provider's package path joined with the product value (cache-managed) or the raw
string (user-managed); a command product is refused. `envy.product_command(name)`
authorizes the same way and returns the rendered argv quoted for the phase's shell — a
path product as one element — so consumers run any product without knowing its shape. Allowed or not, the attempt is traced
`lua_ctx_product_access{target, provider, current_phase, needed_by, allowed, reason}`.

## Cache key

A resolved weak or reference-only edge decides *which* provider a package was built
against, so it belongs in that package's hash: `BLAKE3("identity{opts}|key1|key2|…")`
over the provider keys, sorted, appended as each reference resolves (not swept
afterwards, so a closure member that hashes mid-resolution already has them). Strong
edges contribute nothing — the entry itself named the provider, and it is already in the
spec text.

## `envy product`

`envy product <name>` resolves the graph, extends the provider's closure to completion,
and prints the rendered value to stdout — a command quoted for the platform's shell, for
`eval`/`Invoke-Expression`; scripts wanting an argv read `--json`, or run it with `--run`. A cache hit logs at DEBUG, so `$(envy product x)`
stays quiet; a lazy install still reports. `--run` executes the product instead, in the
project's activation — what deployed scripts call (see `docs/commands.md`). With no argument it lists every product:
aligned `name  value  provider (user-managed)` rows on stderr (a command as declared), or
with `--json` a `{ name: resolved_value }` object on stdout, a command as its argv array —
which needs no install, since paths are recomputed from the providers' cache keys.
