#include "cmd.h"

#include "cache.h"
#include "manifest.h"
#include "reexec.h"
#include "self_deploy.h"
#include "util.h"

#include <cstdio>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <io.h>
#define ENVY_DUP _dup
#define ENVY_DUP2 _dup2
#define ENVY_CLOSE _close
#define ENVY_WRITE _write
#define ENVY_FILENO _fileno
#else
#include <unistd.h>
#define ENVY_DUP ::dup
#define ENVY_DUP2 ::dup2
#define ENVY_CLOSE ::close
#define ENVY_WRITE ::write
#define ENVY_FILENO ::fileno
#endif

namespace envy {

stdout_to_stderr::stdout_to_stderr()
    : saved_{ (std::fflush(stdout), ENVY_DUP(ENVY_FILENO(stdout))) } {
  ENVY_DUP2(ENVY_FILENO(stderr), ENVY_FILENO(stdout));
}

stdout_to_stderr::~stdout_to_stderr() {
  if (saved_ >= 0) { ENVY_CLOSE(saved_); }
}

void stdout_to_stderr::restore() {
  std::fflush(stdout);
  ENVY_DUP2(saved_, ENVY_FILENO(stdout));
}

void stdout_to_stderr::write(std::string_view s) const {
  while (!s.empty()) {
    auto const n{ ENVY_WRITE(saved_, s.data(), static_cast<unsigned>(s.size())) };
    if (n <= 0) { throw_runtime_error({ "failed to write stdout" }); }
    s.remove_prefix(static_cast<size_t>(n));
  }
}

cmd_startup cmd_startup_load(std::string_view cmd_name,
                             std::optional<std::filesystem::path> const &manifest_path,
                             std::optional<std::filesystem::path> const &cli_cache_root,
                             bool subproject,
                             std::optional<std::filesystem::path> const &project_dir,
                             bool read_only_cache) {
  // Bytes and '@envy' header only: everything below picks the cache, and the manifest
  // may fetch a bundle into it during its own global scope (envy.loadenv_bundle).
  auto const found{ manifest::find_and_discover(manifest_path, subproject, project_dir) };
  auto const manifest_dir{ found.path.parent_path() };

  // Resolved once here and threaded onward: reexec's fast path, self-deploy and every
  // command downstream must agree, and re-resolving per consumer let a concurrent
  // `envy cache --local` split one run across two trees.
  auto const resolved{ resolve_cache_root(
      found.meta.cache_request(cli_cache_root, manifest_dir)) };

  // Throws reexec_request when it re-execs, so everything below runs only on the proceed
  // path -- which is what keeps the first-run notice from printing twice per invocation.
  reexec_if_needed(found.meta, resolved, manifest_dir);

  if (!read_only_cache) { cache_announce_root_once(resolved, found.meta.bin); }

  auto c{ self_deploy::ensure(resolved.root, resolved.mode) };
  c->set_read_only(read_only_cache);

  auto m{ manifest::load(found.content, found.path, c.get()) };
  if (!m) { throw_runtime_error({ cmd_name, ": could not load manifest" }); }
  return { std::move(m), std::move(c) };
}

}  // namespace envy
