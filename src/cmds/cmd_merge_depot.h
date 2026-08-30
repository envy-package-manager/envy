#pragma once

#include "cmd.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace envy {

class cli_cmd;

struct depot_manifest_entry {
  std::string hash;  // lowercase 64-char hex
  std::string path;
};

std::vector<depot_manifest_entry> parse_depot_manifest(std::filesystem::path const &file);
std::unordered_set<std::string> parse_s3_ls_lines(std::istream &in);

class cmd_merge_depot : public cmd {
 public:
  enum class retain_format { PLAIN, S3_LS };

  struct retain_source {
    std::string path;
    retain_format fmt{ retain_format::PLAIN };
  };

  struct cfg : cmd_cfg<cmd_merge_depot> {
    std::vector<std::filesystem::path> depot_manifests;
    std::optional<std::string> existing_path;
    std::optional<retain_source> retain;
    std::optional<std::string> retain_prefix;
    bool strict{ false };
    // Parse scratch: the two retain spellings differ only in format, so they share a
    // destination and are folded into `retain` once argv parses.
    std::optional<std::string> retain_plain, retain_s3_ls;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_merge_depot(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
