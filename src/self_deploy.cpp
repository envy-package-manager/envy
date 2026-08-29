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
  std::error_code ec;

  // Self-copy: ensure_envy gates on binary *and* types, so a version directory that lost
  // its envy.lua asks for a deploy whose source is already the destination. The rename
  // would then overwrite a running image -- which fails outright on Windows, where the
  // launcher spawns rather than execs and the file stays locked. The early return on
  // failure would skip writing the very types this deploy exists to restore, so the warning
  // repeats forever. equivalent() reports false (and sets ec) when dst is absent, which is
  // the ordinary case.
  if (std::filesystem::equivalent(src, dst, ec)) { return true; }
  ec.clear();

  // Atomic deploy: copy to temp, set permissions, then rename. Avoids ETXTBSY on
  // Linux when another process is executing the destination binary concurrently.
  auto const tmp{ dst.parent_path() / (".envy-tmp-" + dst.filename().string()) };

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

std::unique_ptr<cache> self_deploy::ensure(path const &root, cache_mode mode) {
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

    // A project on its own cache tree takes no part in shell integration. The profile
    // sources the user-wide hook, so a copy here is never read and `rm -rf` on the build
    // root takes it with it. Redirecting the write to the user-wide tree instead would be
    // worse: a local cache exists so that running the project touches nothing outside it,
    // and conjuring ~/Library/Caches/envy to hold a hook breaks exactly that promise. So
    // neither is written, and `envy shell` says why.
    if (mode != cache_mode::LOCAL) { shell_hooks::ensure(c->root()); }
  } catch (std::exception const &e) { tui::warn("self-deploy: failed: %s", e.what()); }

  return c;
}

}  // namespace envy
