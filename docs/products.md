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
constraint is its intersection with the spec's resolved platforms. `script = false`
suppresses the deployed wrapper (see `docs/commands.md` for `deploy`).

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
string (user-managed). Allowed or not, the attempt is traced
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
and prints the rendered value to stdout. With no argument it lists every product:
aligned `name  value  provider (user-managed)` rows on stderr, or with `--json` a
`{ name: resolved_value }` object on stdout — which needs no install, since the path is
recomputed from the provider's cache key.
