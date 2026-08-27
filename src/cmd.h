#pragma once

#include "util.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace envy {

class cache;
struct manifest;

struct subprocess_exit {  // for commands that proxy subprocess exit codes
  int code;
};

// Shared command prologue: load manifest (throws "<cmd_name>: could not load
// manifest"), re-exec if the manifest pins a different envy version, then
// self-deploy into the cache. subproject enables nearest-manifest discovery.
struct cmd_startup {
  std::unique_ptr<manifest> m;
  std::unique_ptr<cache> c;
};

// Anchor precedence: manifest_path, then project_dir (the global --project, which a bin
// dir's launcher injects so its own project outranks the caller's CWD), then the CWD.
cmd_startup cmd_startup_load(
    std::string_view cmd_name,
    std::optional<std::filesystem::path> const &manifest_path,
    std::optional<std::filesystem::path> const &cli_cache_root,
    bool subproject = false,
    std::optional<std::filesystem::path> const &project_dir = std::nullopt);

// --subproject means "the manifest nearest to where I stand", so it anchors on the CWD
// even under an injected --project -- a launcher's bin dir is the wrong "here".
inline std::optional<std::filesystem::path> subproject_anchor(
    bool subproject,
    std::optional<std::filesystem::path> const &project_dir) {
  return subproject ? std::nullopt : project_dir;
}

class cmd : unmovable {
 public:
  using ptr_t = std::unique_ptr<cmd>;

  virtual ~cmd() = default;
  virtual void execute() = 0;

  template <typename config>
  static ptr_t create(config const &cfg,
                      std::optional<std::filesystem::path> const &cli_cache_root);

 protected:
  cmd() = default;
};

// Command configs inherit from this for factory creation.
template <typename command>
struct cmd_cfg {
  using cmd_t = command;
};

// Opt-in for the global --project: cli_parse writes the anchor into every config deriving
// from this. Explicit, so cmd_init's unrelated project-dir is not mistaken for one.
struct cmd_project_anchor {
  std::optional<std::filesystem::path> project_dir;
};

template <typename config>
cmd::ptr_t cmd::create(config const &cfg,
                       std::optional<std::filesystem::path> const &cli_cache_root) {
  return std::make_unique<typename config::cmd_t>(cfg, cli_cache_root);
}

}  // namespace envy
