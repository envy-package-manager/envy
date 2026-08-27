#include "cmd.h"

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
  reexec_if_needed(m->meta, cli_cache_root, manifest_dir);

  auto c{
    self_deploy::ensure(cli_cache_root, m->meta.cache_for_platform(), manifest_dir)
  };
  return { std::move(m), std::move(c) };
}

}  // namespace envy
