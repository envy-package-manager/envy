#pragma once

#include "manifest.h"
#include "util.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

struct product_info;

inline constexpr int kProductScriptVersion = 2;

// project_root_rel is the bin dir -> manifest dir hop, generic-separator: relative so a
// re-cloned tree still resolves, stamped because '@envy bin' can name any depth.
std::string deploy_stamp_product_script(std::string_view product_name,
                                        platform_id platform,
                                        std::string_view project_root_rel);

void deploy_product_scripts(std::filesystem::path const &bin_dir,
                            std::vector<product_info> const &products,
                            bool strict,
                            std::vector<platform_id> const &platforms,
                            std::string_view project_root_rel);

// Refuse a bin dir whose own upward walk does not land on the manifest that owns it: the
// scripts written there resolve their project that way, so a layout that cannot
// round-trip deploys tools answering for a different project. Throws with the mismatch.
// Skipped for '@envy root "false"', which declares that the walk continues past this
// manifest -- and whose deployed files are byte-identical wherever the tree sits.
void deploy_verify_bin_dir(std::filesystem::path const &bin_dir,
                           std::filesystem::path const &manifest_path,
                           envy_meta const &meta);

// Shared tail of `sync` and `deploy`: refresh the bootstrap script for each platform,
// then either deploy product scripts ('@envy deploy') or explain how to turn deployment
// on. manifest_path names the file in the disabled hint; meta supplies the '@envy version'
// the stamp is checked against and the '@envy root' the project-root stamp depends on.
void deploy_finalize(std::filesystem::path const &bin_dir,
                     std::vector<product_info> const &products,
                     std::vector<platform_id> const &platforms,
                     bool strict,
                     std::filesystem::path const &manifest_path,
                     envy_meta const &meta);

}  // namespace envy
