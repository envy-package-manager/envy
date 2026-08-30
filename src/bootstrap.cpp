#include "bootstrap.h"

#include "embedded_init_resources.h"
#include "envy_release.h"
#include "platform.h"
#include "tui.h"
#include "util.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace envy {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kEnvyManagedMarker{ "envy-managed" };

std::string get_bootstrap_template(platform_id platform) {
  switch (platform) {
    case platform_id::POSIX: return util_inflate_resource(embedded::kBootstrapPosix);
    case platform_id::WINDOWS: return util_inflate_resource(embedded::kBootstrapWindows);
    default: throw std::logic_error("unhandled platform_id in get_bootstrap_template");
  }
}

void replace_all(std::string &s, std::string_view from, std::string_view to) {
  size_t pos{ 0 };
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.length(), to);
    pos += to.length();
  }
}

std::string stamp_bootstrap(platform_id platform) {
  std::string result{ get_bootstrap_template(platform) };
  replace_all(result, "@@ENVY_VERSION@@", ENVY_VERSION_STR);
  // Both URLs are envy's own upstream, never a project's mirror: the script resolves
  // `@envy mirror` out of the manifest at run time and only falls back to these. Stamped
  // rather than hardcoded in the scripts so relocating the project stays a one-line edit
  // in envy_release.h.
  replace_all(result, "@@DOWNLOAD_URL@@", kEnvyReleaseDownloadUrl);
  replace_all(result, "@@LATEST_URL@@", kEnvyReleaseLatestUrl);
  replace_all(result, "@@MIN_DIRECTIVE_VERSION@@", kEnvyMinDirectiveVersion);

  // cmd.exe seeks `goto`/`call :label` by offsets that assume CRLF, so an LF batch drifts
  // a byte per line until the search misses. Converted here, not in the repo.
  if (platform == platform_id::WINDOWS) { replace_all(result, "\n", "\r\n"); }
  return result;
}

std::string read_file_content(fs::path const &path) {
  if (!fs::exists(path)) { return {}; }
  std::ifstream in{ path, std::ios::binary };
  if (!in) { return {}; }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

fs::path bootstrap_script_path(fs::path const &bin_dir, platform_id platform) {
  return (platform == platform_id::WINDOWS) ? bin_dir / "envy.bat" : bin_dir / "envy";
}

void set_executable(fs::path const &path, platform_id platform) {
  if (platform == platform_id::WINDOWS) { return; }
#ifndef _WIN32
  std::error_code ec;
  fs::permissions(path,
                  fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                  fs::perm_options::add,
                  ec);
  if (ec) {
    tui::warn("Failed to set executable bit on %s: %s",
              path.string().c_str(),
              ec.message().c_str());
  }
#else
  (void)path;
#endif
}

}  // namespace

bool bootstrap_is_envy_managed(fs::path const &path) {
  std::string const content{ read_file_content(path) };
  return content.find(kEnvyManagedMarker) != std::string::npos;
}

bool bootstrap_write_script(fs::path const &bin_dir, platform_id platform) {
  fs::path const script_path{ bootstrap_script_path(bin_dir, platform) };

  // Check if existing file is envy-managed
  if (fs::exists(script_path) && !bootstrap_is_envy_managed(script_path)) {
    throw std::runtime_error(
        "bootstrap: file '" + script_path.string() +
        "' exists but is not envy-managed. Remove manually to allow envy to manage it.");
  }

  // Generate new content
  std::string const new_content{ stamp_bootstrap(platform) };

  // Compare with existing
  std::string const existing_content{ read_file_content(script_path) };
  if (new_content == existing_content) { return false; }

  // Write atomically
  util_write_file(script_path, new_content);
  set_executable(script_path, platform);

  return true;
}

}  // namespace envy
