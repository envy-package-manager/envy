#include "cmd_init.h"

#include "bootstrap.h"
#include "cache.h"
#include "embedded_init_resources.h"  // Generated from cmake/EmbedResource.cmake
#include "envy_release.h"
#include "fetch.h"
#include "luarc.h"
#include "manifest.h"
#include "platform.h"
#include "reexec.h"
#include "sha256.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include "CLI11.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace envy {

namespace fs = std::filesystem;

void cmd_init::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("init",
                                "Initialize envy project with bootstrap scripts") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("project-dir", cfg_ptr->project_dir, "Project directory for manifest")
      ->required();
  sub->add_option("bin-dir", cfg_ptr->bin_dir, "Directory for bootstrap scripts")
      ->required();
  sub->add_option("--mirror", cfg_ptr->mirror, "Override download mirror URL");
  sub->add_option("--envy-version",
                  cfg_ptr->envy_version,
                  "Initialize the project at this envy version instead of this binary's, "
                  "re-execing into it (downloading it if the cache lacks it)");
  sub->add_flag("--pin-sums",
                cfg_ptr->pin_sums,
                "Fetch this release's SHA256SUMS and pin its hash in @envy sha256sums, so "
                "bootstrap attests every envy binary it downloads");
  sub->add_option("--deploy", cfg_ptr->deploy, "Set @envy deploy directive (true/false)");
  sub->add_option("--root", cfg_ptr->root, "Set @envy root directive (true/false)");
  sub->add_option("--platform",
                  cfg_ptr->platform_flag,
                  "Script platform: posix, windows, or all (default: current OS)")
      ->check(CLI::IsMember({ "posix", "windows", "all" }));
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

namespace {

std::string_view get_manifest_template() {
  return { reinterpret_cast<char const *>(embedded::kManifestTemplate),
           embedded::kManifestTemplateSize };
}

void replace_all(std::string &s, std::string_view from, std::string_view to) {
  size_t pos{ 0 };
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.length(), to);
    pos += to.length();
  }
}

// Download this release's SHA256SUMS and return its own sha256, the value that
// `@envy sha256sums` pins. Fetched rather than computed locally: the pin has to describe
// the published file byte-for-byte, and this binary has no copy of it.
std::string fetch_sums_pin(std::optional<std::string> const &mirror) {
  std::string_view const base{ mirror ? std::string_view{ *mirror }
                                      : kEnvyReleaseDownloadUrl };
  auto const url{ envy_release_sums_url(base, ENVY_VERSION_STR) };

  auto const tmp{ platform::create_unique_temp_dir("envy-init-sums") };
  scoped_path_cleanup const cleanup{ tmp };
  auto const dest{ tmp / std::string{ kEnvyReleaseSumsFile } };

  tui::info("Fetching %s for %s",
            std::string{ kEnvyReleaseSumsFile }.c_str(),
            url.c_str());
  std::vector<std::string> const labels{ std::string{ kEnvyReleaseSumsFile } };
  auto const results{
    tui_actions::fetch_tracked({ fetch_request_from_url(url, dest) }, "init", labels)
  };
  if (results.empty()) {
    throw std::runtime_error("init: --pin-sums: failed to download " + url +
                             ": unknown error");
  }
  if (auto const *err{ std::get_if<std::string>(&results[0]) }) {
    throw std::runtime_error("init: --pin-sums: failed to download " + url + ": " + *err);
  }

  auto const hash{ sha256(dest) };
  return util_bytes_to_hex(hash.data(), hash.size());
}

std::string stamp_manifest_placeholders(std::string_view content,
                                        std::optional<std::string> const &mirror,
                                        std::string_view bin_dir,
                                        std::optional<bool> deploy,
                                        std::optional<bool> root,
                                        std::optional<std::string> const &sums_pin) {
  std::string result{ content };
  replace_all(result, "@@ENVY_VERSION@@", ENVY_VERSION_STR);
  replace_all(result, "@@BIN_DIR@@", bin_dir);

  if (sums_pin) {
    replace_all(result,
                "@@SHA256SUMS_DIRECTIVE@@",
                "-- @envy sha256sums \"" + *sums_pin + "\"\n");
  } else {
    replace_all(result, "@@SHA256SUMS_DIRECTIVE@@", "");
  }

  // The manifest is the only place the mirror lands: the bootstrap scripts carry no
  // project configuration and parse `@envy mirror` back out at run time.
  if (mirror.has_value() && !mirror->empty()) {
    replace_all(result, "@@MIRROR_DIRECTIVE@@", "-- @envy mirror \"" + *mirror + "\"\n");
  } else {
    replace_all(result, "@@MIRROR_DIRECTIVE@@", "");
  }

  // Add deploy directive if specified
  if (deploy.has_value()) {
    replace_all(result,
                "@@DEPLOY_DIRECTIVE@@",
                *deploy ? "-- @envy deploy \"true\"\n" : "-- @envy deploy \"false\"\n");
  } else {
    replace_all(result, "@@DEPLOY_DIRECTIVE@@", "");
  }

  // Add root directive if specified
  if (root.has_value()) {
    replace_all(result,
                "@@ROOT_DIRECTIVE@@",
                *root ? "-- @envy root \"true\"\n" : "-- @envy root \"false\"\n");
  } else {
    replace_all(result, "@@ROOT_DIRECTIVE@@", "");
  }

  return result;
}

void write_manifest(fs::path const &project_dir,
                    fs::path const &bin_dir,
                    std::optional<std::string> const &mirror,
                    std::optional<bool> deploy,
                    std::optional<bool> root,
                    std::optional<std::string> const &sums_pin) {
  fs::path const manifest_path{ project_dir / "envy.lua" };

  if (fs::exists(manifest_path)) {
    tui::info("Manifest already exists: %s", manifest_path.string().c_str());
    return;
  }

  // Compute relative path from project_dir to bin_dir
  auto const abs_project{ fs::absolute(project_dir) };
  auto const abs_bin{ fs::absolute(bin_dir) };
  auto const relative_bin{ fs::relative(abs_bin, abs_project) };

  std::string const content{ stamp_manifest_placeholders(get_manifest_template(),
                                                         mirror,
                                                         relative_bin.string(),
                                                         deploy,
                                                         root,
                                                         sums_pin) };
  util_write_file(manifest_path, content);

  tui::info("Created %s", manifest_path.string().c_str());
}

}  // namespace

cmd_init::cmd_init(cmd_init::cfg cfg,
                   std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_init::execute() {
  // Before creating any directories: the mirror is written verbatim into a quoted manifest
  // directive that both bootstrap scripts parse back out.
  if (cfg_.mirror) { envy_release_validate_mirror(*cfg_.mirror, "init"); }

  // Every version this command writes -- the manifest directive, the bootstrap fallback,
  // the cached types -- is the running binary's, so a requested one is honored by handing
  // the whole init to that binary rather than by stamping a number this one is not.
  if (cfg_.envy_version) {
    if (!envy_release_version_is_valid(*cfg_.envy_version)) {
      throw std::runtime_error("init: invalid version string: " + *cfg_.envy_version);
    }
    // The child inherits this process's argv, so the flag itself must not travel: it is a
    // parent-side instruction, and every release predating it rejects the unknown option.
    reexec_if_needed(envy_meta{ .version = cfg_.envy_version, .mirror = cfg_.mirror },
                     cli_cache_root_,
                     cfg_.project_dir,
                     { "--envy-version" });
    if (*cfg_.envy_version != ENVY_VERSION_STR) {  // no re-exec, or it landed elsewhere
      tui::warn("init: not re-execing into envy %s; stamping %s instead",
                cfg_.envy_version->c_str(),
                ENVY_VERSION_STR);
    }
  }

  // Also before creating anything: --pin-sums needs the network, and failing after having
  // written a manifest and two scripts would leave a project half-initialized and
  // unattested. A dev build (0.0.0) has no published release, so this is where that
  // says so.
  std::optional<std::string> sums_pin;
  if (cfg_.pin_sums) { sums_pin = fetch_sums_pin(cfg_.mirror); }

  auto c{ std::make_unique<cache>(cli_cache_root_) };
  std::error_code ec;

  if (!fs::exists(cfg_.project_dir)) {
    fs::create_directories(cfg_.project_dir, ec);
    if (ec) {
      throw std::runtime_error("init: failed to create project directory " +
                               cfg_.project_dir.string() + ": " + ec.message());
    }
  }

  if (!fs::exists(cfg_.bin_dir)) {
    fs::create_directories(cfg_.bin_dir, ec);
    if (ec) {
      throw std::runtime_error("init: failed to create bin directory " +
                               cfg_.bin_dir.string() + ": " + ec.message());
    }
  }

  auto const platforms{ util_parse_platform_flag(cfg_.platform_flag) };
  for (auto const plat : platforms) {
    bootstrap_write_script(cfg_.bin_dir, plat);
    auto const name{ (plat == platform_id::WINDOWS) ? "envy.bat" : "envy" };
    tui::info("Created %s", (cfg_.bin_dir / name).string().c_str());
  }

  write_manifest(cfg_.project_dir,
                 cfg_.bin_dir,
                 cfg_.mirror,
                 cfg_.deploy,
                 cfg_.root,
                 sums_pin);
  extract_lua_ls_types(c->root());
  write_luarc(cfg_.project_dir, envy_meta{});

  tui::info("");
  tui::info("Initialized envy project.");
  tui::info("Next steps:");
  tui::info("  1. Edit %s to add packages",
            (cfg_.project_dir / "envy.lua").string().c_str());
  auto const native_name{ (platform::native() == platform_id::WINDOWS) ? "envy.bat"
                                                                       : "envy" };
  tui::info("  2. Run %s sync", (cfg_.bin_dir / native_name).string().c_str());
}

}  // namespace envy
