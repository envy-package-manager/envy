#pragma once

#include "util.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace envy {

class cache;
struct manifest;

// A command's last act: become `argv` in environment `to` (`from` now). Thrown so main
// runs it after the tui and libraries are down -- no thread races the env, no row lingers.
struct exec_request {
  using env_t = std::unordered_map<std::string, std::string>;  // shell_env_t; see below
  std::vector<std::string> argv;
  env_t from;
  env_t to;
};
// shell.h stays out of this header: its sol/forward.hpp ahead of a TU's later sol.hpp,
// with a library's __has_builtin shim between them, is a macro redefinition under MSVC.

// Shared command prologue: load manifest (throws "<cmd_name>: could not load
// manifest"), re-exec if the manifest pins a different envy version, then
// self-deploy into the cache. subproject enables nearest-manifest discovery.
struct cmd_startup {
  std::unique_ptr<manifest> m;
  std::unique_ptr<cache> c;
};

// Anchor precedence: manifest_path, then project_dir (the global --project, which a bin
// dir's launcher injects so its own project outranks the caller's CWD), then the CWD.
// read_only_cache: activation's prompt path, which must neither fetch nor announce a tree.
cmd_startup cmd_startup_load(
    std::string_view cmd_name,
    std::optional<std::filesystem::path> const &manifest_path,
    std::optional<std::filesystem::path> const &cli_cache_root,
    bool subproject = false,
    std::optional<std::filesystem::path> const &project_dir = std::nullopt,
    bool read_only_cache = false);

// --subproject means "the manifest nearest to where I stand", so it anchors on the CWD
// even under an injected --project -- a launcher's bin dir is the wrong "here".
inline std::optional<std::filesystem::path> subproject_anchor(
    bool subproject,
    std::optional<std::filesystem::path> const &project_dir) {
  return subproject ? std::nullopt : project_dir;
}

// One line of a size report: the tree at `path` measured, else `bytes` as given; `note`
// prints after the size.
struct cmd_size_row {
  std::string label;
  std::filesystem::path path;
  std::uint64_t bytes{ 0 };
  std::string note{};
};

struct cmd_size_section {
  char const *title;  // nullptr prints the rows alone
  std::vector<cmd_size_row> rows;
};

// Measures every row behind a spinner labeled `label`, counting files and bytes as it
// walks, then prints each section largest first and a TOTAL line.
void cmd_print_sizes(char const *label, std::vector<cmd_size_section> const &sections);

// Points fd 1 at stderr until restore(), so nothing a command doesn't mean to print -- a
// spec's envy.stdout, queued tui output -- reaches stdout.
class stdout_to_stderr : unmovable {
 public:
  stdout_to_stderr();
  ~stdout_to_stderr();

  void restore();                        // the real stdout back on fd 1
  void write(std::string_view s) const;  // to the real stdout

 private:
  int saved_;
};

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
