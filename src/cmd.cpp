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
  auto m{ manifest::find_and_load(manifest_path, subproject, project_dir) };
  if (!m) {
    throw std::runtime_error(std::string{ cmd_name } + ": could not load manifest");
  }

  auto const manifest_dir{ m->manifest_path.parent_path() };

  // Resolved once here and threaded onward: reexec's fast path, self-deploy and every
  // command downstream must agree, and re-resolving per consumer let a concurrent
  // `envy cache --local` split one run across two trees.
  auto const resolved{ resolve_cache_root(
      m->meta.cache_request(cli_cache_root, manifest_dir)) };

  // Throws reexec_request when it re-execs, so everything below runs only on the proceed
  // path -- which is what keeps the first-run notice from printing twice per invocation.
  reexec_if_needed(m->meta, resolved.root, manifest_dir);

  cache_announce_root_once(resolved, m->meta.bin);

  auto c{ self_deploy::ensure(resolved.root) };
  return { std::move(m), std::move(c) };
}

}  // namespace envy
