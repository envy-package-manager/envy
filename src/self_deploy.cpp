#include "self_deploy.h"

#include "embedded_init_resources.h"
#include "platform.h"
#include "shell_hooks.h"
#include "tui.h"
#include "util.h"
#include "version.h"

#include <filesystem>
#include <string_view>
#include <system_error>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

using path = std::filesystem::path;

namespace envy {

namespace {

void update_latest_if_newer(path const &envy_dir, std::string_view version) {
  auto const latest_path{ envy_dir / "latest" };
  try {
    if (std::filesystem::exists(latest_path)) {
      auto const content{ util_load_file(latest_path) };
      std::string_view const current{ reinterpret_cast<char const *>(content.data()),
                                      content.size() };
      if (!version_is_newer(version, current)) { return; }
    }
  } catch (...) {
    // TOCTOU race: file may vanish between exists() and load; treat as missing.
  }
  util_write_file(latest_path, version);
}

bool copy_binary(path const &src, path const &dst) {
  // Atomic deploy: copy to temp, set permissions, then rename. Avoids ETXTBSY on
  // Linux when another process is executing the destination binary concurrently.
  auto const tmp{ dst.parent_path() / (".envy-tmp-" + dst.filename().string()) };
  std::error_code ec;

  std::filesystem::copy_file(src,
                             tmp,
                             std::filesystem::copy_options::overwrite_existing,
                             ec);
  if (ec) {
    tui::warn("self-deploy: failed to copy binary: %s", ec.message().c_str());
    return false;
  }

#ifndef _WIN32
  std::filesystem::permissions(tmp,
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::add,
                               ec);
  if (ec) {
    tui::warn("self-deploy: failed to set executable permissions: %s",
              ec.message().c_str());
  }
#endif

  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::filesystem::remove(tmp);
    tui::warn("self-deploy: failed to install binary: %s", ec.message().c_str());
    return false;
  }

  return true;
}

}  // namespace

std::unique_ptr<cache> self_deploy::ensure(path const &root) {
  auto c{ std::make_unique<cache>(root) };

  try {
    auto result{ c->ensure_envy(ENVY_VERSION_STR) };

    if (!result.already_cached) {
      if (!copy_binary(platform::get_exe_path(), result.binary_path)) { return c; }

      std::string_view const types{ reinterpret_cast<char const *>(
                                        embedded::kTypeDefinitions),
                                    embedded::kTypeDefinitionsSize };
      util_write_file(result.types_path, types);
    }

    update_latest_if_newer(result.envy_dir.parent_path(), ENVY_VERSION_STR);
    shell_hooks::ensure(c->root());
  } catch (std::exception const &e) { tui::warn("self-deploy: failed: %s", e.what()); }

  return c;
}

}  // namespace envy
