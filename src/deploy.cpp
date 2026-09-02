#include "deploy.h"

#include "bootstrap.h"
#include "embedded_init_resources.h"
#include "engine.h"
#include "manifest.h"
#include "platform.h"
#include "reexec.h"
#include "trace.h"
#include "tui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace envy {

namespace fs = std::filesystem;

namespace {

std::string get_product_script_template(platform_id platform) {
  switch (platform) {
    case platform_id::POSIX: return util_inflate_resource(embedded::kProductScriptPosix);
    case platform_id::WINDOWS:
      return util_inflate_resource(embedded::kProductScriptWindows);
    default:
      throw std::logic_error("unhandled platform_id in get_product_script_template");
  }
}

void replace_all(std::string &s, std::string_view from, std::string_view to) {
  size_t pos{ 0 };
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.length(), to);
    pos += to.length();
  }
}

std::string read_file_content(fs::path const &path) {
  if (!fs::exists(path)) { return {}; }
  std::ifstream in{ path, std::ios::binary };
  if (!in) { return {}; }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool has_envy_marker(fs::path const &path) {
  std::string const content{ read_file_content(path) };
  return content.find("envy-managed") != std::string::npos;
}

fs::path product_script_path(fs::path const &bin_dir,
                             std::string_view product_name,
                             platform_id platform) {
  return (platform == platform_id::WINDOWS)
             ? bin_dir / (std::string(product_name) + ".bat")
             : bin_dir / product_name;
}

}  // namespace

std::string deploy_stamp_product_script(std::string_view product_name,
                                        platform_id platform,
                                        std::string_view project_root_rel) {
  std::string result{ get_product_script_template(platform) };
  replace_all(result, "@@PRODUCT_NAME@@", product_name);
  replace_all(result, "@@PROJECT_ROOT_REL@@", project_root_rel);
  util_apply_script_eol(result, platform);
  return result;
}

void deploy_verify_bin_dir(fs::path const &bin_dir,
                           fs::path const &manifest_path,
                           envy_meta const &meta) {
  // '@envy root "false"' *declares* that a walk continues past this manifest, so its own
  // bin dir resolving the enclosing project is the stated design, not a layout error. The
  // files deployed there name no path outside the bin dir and carry no project-root stamp,
  // so a nested deploy writes exactly what a standalone one does -- which is what lets a
  // superproject checkout restamp a submodule's committed bin dir in place.
  if (meta.root.has_value() && !*meta.root) {
    tui::debug(
        "deploy: %s declares '@envy root \"false\"'; its scripts resolve whichever "
        "project encloses %s",
        manifest_path.filename().string().c_str(),
        bin_dir.string().c_str());
    return;
  }

  auto const owner{ util_canonical_path(manifest_path) };
  auto const found{ manifest::discover(false, util_canonical_path(bin_dir)) };
  if (found && util_canonical_path(found->path) == owner) { return; }

  std::string const mismatch{ "scripts in " + bin_dir.string() + " resolve " +
                              (found ? found->path.string()
                                     : std::string{ "no manifest" }) +
                              ", not " + owner.string() };

  // Discovery only ever looks for 'envy.lua', so a variant manifest ('--manifest ci.lua')
  // can never be what the scripts resolve -- inherent to that workflow, and no bin dir
  // placement changes it. Finding nothing at all is inert too: those scripts fail loudly,
  // naming the anchor. Either way a warning, not a refusal.
  if (owner.filename() != "envy.lua") {
    tui::warn(
        "deploy: %s; a manifest not named envy.lua is invisible to the upward walk "
        "those scripts do, so they answer for whatever they find instead",
        mismatch.c_str());
    return;
  }
  if (!found) {
    tui::warn(
        "deploy: %s; they cannot resolve this project until the bin directory sits "
        "under a discoverable envy.lua",
        mismatch.c_str());
    return;
  }

  // Resolving a *different* envy.lua is the dangerous case, and the only one a layout fix
  // addresses: the bin dir quietly hands out another project's tools.
  throw std::runtime_error(
      "deploy: " + mismatch +
      ".\n"
      "       A root manifest's bin directory has to sit under it: no '.git' between\n"
      "       them, no '..' in '@envy bin', and a '--manifest' inside the bin dir's own\n"
      "       tree. A nested tree that means to defer to its parent says so with\n"
      "       '@envy root \"false\"'.");
}

void deploy_product_scripts(fs::path const &bin_dir,
                            std::vector<product_info> const &products,
                            bool strict,
                            std::vector<platform_id> const &platforms,
                            std::string_view project_root_rel) {
  // Check if a product is expected for a given platform
  auto const product_expected_for_platform = [&](std::string const &name,
                                                 platform_id plat) {
    for (auto const &p : products) {
      if (p.product_name == name && p.script &&
          util_platform_matches_platform_id(p.platforms, plat)) {
        return true;
      }
    }
    return false;
  };

  size_t created{ 0 };
  size_t updated{ 0 };
  size_t unchanged{ 0 };

  for (auto const &product : products) {
    if (!product.script) { continue; }
    for (auto const plat : platforms) {
      if (!util_platform_matches_platform_id(product.platforms, plat)) { continue; }
      fs::path const script_path{
        product_script_path(bin_dir, product.product_name, plat)
      };

      if (fs::exists(script_path) && !has_envy_marker(script_path)) {
        if (strict) {
          throw std::runtime_error(
              "deploy: file '" + script_path.string() +
              "' exists but is not envy-managed. Remove manually or rename product.");
        }
        continue;
      }

      auto const outcome{ util_write_script(
          script_path,
          deploy_stamp_product_script(product.product_name, plat, project_root_rel),
          plat) };

      switch (outcome) {
        case script_write::UNCHANGED: ++unchanged; break;
        case script_write::CREATED:
          ++created;
          tui::debug("Created product script: %s", script_path.string().c_str());
          break;
        case script_write::UPDATED:
          ++updated;
          tui::debug("Updated product script: %s", script_path.string().c_str());
          break;
      }
      ENVY_TRACE(deploy_script,
                 "",
                 .product = product.product_name,
                 .platform = plat == platform_id::WINDOWS ? "windows" : "posix",
                 .action = outcome == script_write::UNCHANGED ? "unchanged"
                           : outcome == script_write::CREATED ? "created"
                                                              : "updated");
    }
  }

  // Build set of platform-relevant extensions for cleanup
  bool const clean_posix{
    std::find(platforms.begin(), platforms.end(), platform_id::POSIX) != platforms.end()
  };
  bool const clean_windows{
    std::find(platforms.begin(), platforms.end(), platform_id::WINDOWS) != platforms.end()
  };

  size_t removed{ 0 };
  std::error_code ec;
  for (auto const &entry : fs::directory_iterator(bin_dir, ec)) {
    if (!entry.is_regular_file()) { continue; }

    std::string filename{ entry.path().filename().string() };
    if (filename == "envy" || filename == "envy.bat") { continue; }

    bool const is_batch{ filename.size() > 4 &&
                         filename.substr(filename.size() - 4) == ".bat" };

    if (is_batch && !clean_windows) { continue; }
    if (!is_batch && !clean_posix) { continue; }

    std::string const product_name{ is_batch ? filename.substr(0, filename.size() - 4)
                                             : filename };
    platform_id const file_plat{ is_batch ? platform_id::WINDOWS : platform_id::POSIX };

    if (!product_expected_for_platform(product_name, file_plat) &&
        has_envy_marker(entry.path())) {
      std::error_code rm_ec;
      fs::remove(entry.path(), rm_ec);
      if (rm_ec) {
        tui::warn("Failed to remove obsolete script %s: %s",
                  entry.path().string().c_str(),
                  rm_ec.message().c_str());
      } else {
        ++removed;
        tui::debug("Removed obsolete product script: %s", entry.path().string().c_str());
        ENVY_TRACE(deploy_script,
                   "",
                   .product = product_name,
                   .platform = file_plat == platform_id::WINDOWS ? "windows" : "posix",
                   .action = "removed");
      }
    }
  }

  if (ec) {
    tui::warn("Failed to iterate bin directory %s: %s",
              bin_dir.string().c_str(),
              ec.message().c_str());
    return;
  }

  if (created > 0 || updated > 0 || removed > 0) {
    size_t const script_count{ created + updated + unchanged };
    tui::info(
        "deploy: %zu product script(s) (%zu created, %zu updated, %zu unchanged, %zu "
        "removed)",
        script_count,
        created,
        updated,
        unchanged,
        removed);
  }
}

void deploy_finalize(std::filesystem::path const &bin_dir,
                     std::vector<product_info> const &products,
                     std::vector<platform_id> const &platforms,
                     bool strict,
                     std::filesystem::path const &manifest_path,
                     envy_meta const &meta) {
  // Before anything is written: the launcher deployed here injects --project with this
  // directory, so a bin dir that resolves elsewhere poisons every script in it.
  deploy_verify_bin_dir(bin_dir, manifest_path, meta);

  // The launcher stamped below hands the binary options only its own generation accepts,
  // and it execs whatever the manifest pins -- so stamping from a build the manifest does
  // not name leaves every './bin/envy' failing on an unrecognized option. Reachable only
  // where re-exec is skipped: a dev build, or ENVY_NO_REEXEC.
  if (meta.version && *meta.version != reexec_self_version()) {
    // No 'envy use <self>' suggestion: self is a dev build in the case that reaches here.
    tui::warn(
        "deploy: bin scripts stamped from envy %s, but %s pins %s -- './bin/envy' "
        "execs %s, which can reject options these scripts pass. Retarget the pin "
        "with 'envy use', or restamp by syncing as the pinned release",
        std::string{ reexec_self_version() }.c_str(),
        manifest_path.filename().string().c_str(),
        meta.version->c_str(),
        meta.version->c_str());
  }

  for (auto const plat : platforms) {
    if (bootstrap_write_script(bin_dir, plat)) { tui::info("Updated bootstrap script"); }
  }

  if (meta.deploy.has_value() && *meta.deploy) {
    // Only a root manifest gets a stamped project root. '@envy root "false"' means the
    // walk continues past this manifest, so which project the scripts resolve depends on
    // where the tree is nested -- no deploy-time constant is right in both checkouts, and
    // a wrong ENVY_PROJECT_ROOT is worse than the caller's own.
    auto const rel{ [&]() -> std::string {
      if (meta.root.has_value() && !*meta.root) { return {}; }
      auto const hop{ fs::relative(util_canonical_path(manifest_path).parent_path(),
                                   util_canonical_path(bin_dir)) };
      return hop.empty() ? "." : hop.generic_string();
    }() };
    deploy_product_scripts(bin_dir, products, strict, platforms, rel);
  } else {
    tui::warn("deployment is disabled in %s", manifest_path.string().c_str());
    tui::info("Add '-- @envy deploy \"true\"' to enable product script deployment");
  }
}

}  // namespace envy
