# pkg_cfg ownership

`pkg_cfg` is parsed once and then referred to by raw pointer from packages, dependency
lists, weak fallbacks and bundle declarations. Those pointers must outlive every worker,
so ownership is deliberately trivial:

- One process-wide arena, `pkg_cfg::pool()`, backed by a mutex-guarded `std::deque`.
  Deque, not vector: emplacing never moves an existing element, so a pointer handed out
  during parsing stays valid for the run.
- `pkg_cfg` is `unmovable` and constructed only through `pkg_cfg_pool::emplace`
  (`ctor_tag` enforces it), so no caller can copy or relocate one.
- Nothing is ever freed. A run's cfgs are bounded by the manifest and the specs it
  reaches, and the arena dies with the process — reclaiming them would buy nothing and
  cost every holder a lifetime question.

There is no per-engine or per-test pool. A test that parses cfgs leaves them in the
arena; they are inert, and keying anything by pointer identity is therefore safe.
