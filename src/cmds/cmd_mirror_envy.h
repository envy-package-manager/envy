#pragma once

#include "cmd.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

class cli_cmd;

// One release archive to mirror.
struct mirror_envy_item {
  std::string source_url;  // <from>/v<version>/<asset>
  std::string relpath;     // v<version>/<asset>, forward slashes on every platform
};

// Where a mirror-envy run reads from and writes to. Pure data so the naming rules are
// testable without touching the network or the filesystem.
struct mirror_envy_plan {
  bool dest_is_s3{ false };
  std::string bucket;               // s3 dest only
  std::string prefix;               // s3 dest only; normalized, no leading/trailing slash
  std::filesystem::path local_dir;  // local dest only
  std::vector<mirror_envy_item> items;
  // Which entry of `items` is the checksum manifest rather than an archive. Named rather
  // than positional so the verify pass cannot drift out of sync with the item ordering.
  std::string sums_relpath;
};

// Throws std::runtime_error on an invalid version, an unusable destination, or an empty
// source mirror. from_mirror and dest may carry trailing slashes; those are normalized
// away.
mirror_envy_plan mirror_envy_make_plan(std::string_view version,
                                       std::string_view dest,
                                       std::string_view from_mirror);

// s3://<bucket>[/<prefix>], no trailing slash: exactly the value an @envy mirror directive
// wants.
std::string mirror_envy_s3_root(mirror_envy_plan const &plan);

// s3://<bucket>/[<prefix>/]<relpath>, never with a double slash -- S3 keys are opaque byte
// strings, so "a//b" is a different object from "a/b".
std::string mirror_envy_s3_uri(mirror_envy_plan const &plan, std::string_view relpath);

// Name of the mirror-root file recording the newest mirrored version, so a bootstrap
// script can resolve "latest" from the mirror instead of from github.com.
inline constexpr std::string_view kMirrorLatestFile{ "latest" };

class cmd_mirror_envy : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_mirror_envy> {
    std::string version;
    std::string dest;
    std::string from;
  };

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_mirror_envy(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;

 private:
  cfg cfg_;
};

}  // namespace envy
