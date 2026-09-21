#include "cmd.h"

#include "cache.h"
#include "manifest.h"
#include "reexec.h"
#include "self_deploy.h"

#include <stdexcept>
#include <string>

namespace envy {

cmd_startup cmd_startup_load(std::string_view cmd_name,
                             std::optional<std::filesystem::path> const &manifest_path,
                             std::optional<std::filesystem::path> const &cli_cache_root,
                             bool subproject,
                             std::optional<std::filesystem::path> const &project_dir) {
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

  cache_announce_root_once(resolved, found.meta.bin);

  auto c{ self_deploy::ensure(resolved.root, resolved.mode) };

  auto m{ manifest::load(found.content, found.path, c.get()) };
  if (!m) {
    throw std::runtime_error(std::string{ cmd_name } + ": could not load manifest");
  }
  return { std::move(m), std::move(c) };
}

}  // namespace envy
