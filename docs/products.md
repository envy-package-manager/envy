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

Keys are printable ASCII minus whitespace, quotes, `$ % \ !` and braces — they become
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
  yarn = { command = function(pkg_dir, pkg_state_dir, options)   -- its own files
    return { "{{node}}", pkg_dir .. "lib/yarn.cjs",
             "--cache-folder=" .. envy.path.trim(pkg_state_dir) }
  end },
  ["gradle.jar"] = { value = "lib/gradle.jar", script = false },  -- public: others may name it
  gradle = { command = { "{{java}}", "-Xmx2g", "-jar", "{{gradle.jar}}" } },
  ci     = { command = { "{{yarn}}", "install", "--frozen-lockfile" } },  -- splices
}
```

- `command` excludes `value`; `script` and `platforms` apply as before. A list with holes
  or non-string entries is an error.
- A function `command` gets `ENV`'s arguments — `pkg_dir` (separator-terminated; nil when
  user-managed), `pkg_state_dir` (the project's in a shell, scratch in a phase, none in a
  `DEFAULT_SHELL` function), a fresh copy of the options — and returns the list, held to a
  table's rules. It runs at each use and each resolution (validation, listing, activation),
  often before install: build strings from its arguments; never read the payload. Its files
  stay private — no product name to collide with or list. Trim a directory that ends an
  argument: PowerShell 5.1 cannot quote a trailing `\` beside a space.
- `{{name}}` is a product of the provider or its dependency closure; the registry admits
  one provider per name, so it names exactly one. A product dependency pins the name to
  its provider, which must declare it (a weak fallback may provide it transitively). In the graph but outside the closure is
  an error, checked once resolution converges (`validate_product_commands`) for every
  command whose `platforms` admit the host. An edge that exists only for a command can
  say `needed_by = "install"`.
- A path product may sit inside an element (`--node={{node}}`); a command must fill a whole
  element, and its argv splices in. Cycles and NULs are errors. `{{{{x}}}}` is the text
  `{{x}}` (see below). Validation walks each command once, however often it is spliced.
- Rendered at each use, never at install: nothing lands in the provider's payload, and
  its cache root can move. A consumer that bakes a command's output into its own payload
  is keyed only by its own references (see Cache key), as with any transitive tool.
- The program: absolute as is; bare is searched on `PATH` (relative entries from where it
  will run), past the bin-dir wrapper of the product that supplied it — so user-managed
  products take arguments too, and a PowerShell alias (`curl`) never shadows the program. A literal
  relative program is refused in a cache-managed spec (join it to a function's `pkg_dir`);
  a user-managed relative value anchors at the project root under `--run`.

## In phase strings

STAGE/BUILD/INSTALL strings, strings a verb function returns, SETUP CHECK/INSTALL strings
and `envy.run` scripts name products too, expanded before the shell runs:

```lua
DEPENDENCIES = { { product = "yarn" }, { product = "ninja" } }
PRODUCTS = { gen = { value = "share/gen", script = false } }
BUILD = [[
{{yarn}} install --immutable
cmake -B out -G Ninja -DCMAKE_MAKE_PROGRAM={{ninja}} "-DGEN={{gen}}"
docker ps --format '{{{{.ID}}}}'
]]
```

- A name resolves as `envy.product` resolves it: a product dependency or direct edge whose
  `needed_by` the phase has reached, or the spec's own product — before INSTALL, where
  INSTALL will put it. Transitive-only is refused, naming the `{ product = "x" }` to add.
- A path product is its text, unquoted, so it reads alike in a word, quotes or a heredoc;
  a trailing separator is dropped. A payload path may hold only letters, digits and
  `/ : . _ + @ ~ -`, plus `\` outside POSIX; non-ASCII only outside cmd, never a
  PowerShell curly quote, dash or odd space; no POSIX `:~`. Else it is an error naming the
  cache root (`--cache-root`, `ENVY_CACHE_ROOT`) or the product value at fault.
- A user-managed value goes in as written: `py -3` works, `C:\Program Files\…` splits —
  give such a tool `command = { "C:\\Program Files\\…" }`.
- A command product is its argv, quoted for the shell, filling whole words: `$({{yarn}})`
  works; `X={{yarn}}`, `" {{yarn}} "`, one in a heredoc or here-string, and any under a
  custom shell (quoting unknown) are errors. A bare program not on `PATH` stays bare, so a CHECK reads "not
  satisfied". The spec's own commands splice only what the phase may.
- `{{{{x}}}}` is the text `{{x}}`; braces nest within a line, so one escape holds a Go
  template that fits on it. Three braces bind the last two: `{{{x}}}` is `{`, `x`, `}`. A
  lone `{{` has no spelling — pass such text through `envy.run`'s `env`. envy reads the
  text before any shell, so every shell gets it expanded, custom ones and comments too.
- PowerShell runs a quoted path only behind `&`; cmd returns from a `.bat`/`.cmd` only
  after `call` — starting a command without it is an error.
- On a miss, a package's string verbs are checked before anything is fetched; selected
  SETUP strings before any pair runs; the rest as they run. Hits, activation and listings
  never check. A function `DEFAULT_SHELL`'s rules wait until it runs; a bootstrap closure
  member reaches `check` before its dependencies load, so is checked only as it runs.
- Only text envy runs expands. A Makefile or script calling `yarn` finds whatever `PATH`
  holds; a provider whose tool must be found that way prepends its dir in `ENV`.
- Scope differs by place: `PRODUCTS.command` names the provider's closure; phase text its
  own products and direct edges; `ENV` reaches a phase from the whole closure.

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
current phase has reached, or names the spec's own product; a provider reachable only
transitively is refused. The value is the provider's package path joined with the product
value (cache-managed) or the raw string (user-managed); a command product is refused —
phase text runs it as `{{name}}`, authorized the same way. Allowed or not, each attempt
is traced `lua_ctx_product_access{target, provider, current_phase, needed_by, allowed,
reason}`.

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
`eval`/`Invoke-Expression`, its program resolved as `--run` resolves it; scripts wanting
an argv read `--json`, or run it with `--run`. A cache hit logs at DEBUG, so `$(envy product x)`
stays quiet; a lazy install still reports. `--run` executes the product instead, in the
project's activation — what deployed scripts call (see `docs/commands.md`). With no argument it lists every product:
aligned `name  value  provider (user-managed)` rows on stderr (a command as declared; a function's result, or `(function)` off its platforms), or
with `--json` a `{ name: resolved_value }` object on stdout, a command as its argv array
(`null` where its `platforms` exclude the host, since nothing validated what it names) —
which needs no install, since paths are recomputed from the providers' cache keys.
