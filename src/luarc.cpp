#include "luarc.h"

#include "cache.h"

#include "embedded_init_resources.h"
#include "envy_release.h"
#include "tui.h"
#include "util.h"

#include "lua.h"
#include "picojson.h"
#include "semver.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#ifndef ENVY_VERSION_STR
#error "ENVY_VERSION_STR must be defined by the build system"
#endif

namespace envy {

namespace fs = std::filesystem;

std::string make_portable_path(fs::path const &path) {
#ifdef _WIN32
  char const *home{ std::getenv("USERPROFILE") };
  char const env_var[]{ "${env:USERPROFILE}" };
  char const sep{ '\\' };
#else
  char const *home{ std::getenv("HOME") };
  char const env_var[]{ "${env:HOME}" };
  char const sep{ '/' };
#endif
  if (!home) {
    std::string result{ path.string() };
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
  }

  std::string const path_str{ path.string() };
  std::string const home_str{ home };

  if (path_str == home_str) { return env_var; }

  if (path_str.starts_with(home_str + sep)) {
    std::string result{ env_var + path_str.substr(home_str.size()) };
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
  }

  std::string result{ path_str };
  std::replace(result.begin(), result.end(), '\\', '/');
  return result;
}

namespace {

void replace_all(std::string &s, std::string_view from, std::string_view to) {
  size_t pos{ 0 };
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.length(), to);
    pos += to.length();
  }
}

std::string stamp_placeholders(std::string_view content, std::string_view download_url) {
  std::string result{ content };
  replace_all(result, "@@ENVY_VERSION@@", ENVY_VERSION_STR);
  replace_all(result, "@@DOWNLOAD_URL@@", download_url);
  return result;
}

// Check if a path ends with /envy/<valid-semver>
bool is_envy_semver_path(std::string_view s) {
  // Find last /envy/ segment
  auto const marker{ s.rfind("/envy/") };
  if (marker == std::string_view::npos) { return false; }

  auto const ver_start{ marker + 6 };  // skip "/envy/"
  if (ver_start >= s.size()) { return false; }

  // The version part is everything after /envy/ (to end, or before trailing /)
  auto ver_str{ s.substr(ver_start) };
  if (!ver_str.empty() && ver_str.back() == '/') {
    ver_str = ver_str.substr(0, ver_str.size() - 1);
  }
  if (ver_str.empty()) { return false; }

  semver::version<> v;
  return semver::parse(ver_str, v);
}

}  // namespace

std::vector<std::string> compute_canonical_luarc_paths(envy_meta const &meta) {
  std::vector<std::string> paths;
  std::string const ver{ ENVY_VERSION_STR };

  // A union, not a choice. `.luarc.json` is committed, so it has to resolve on every
  // developer's machine, and the cache root is now per-user state: the same manifest is
  // local for whoever ran `envy cache --local` and shared for everyone else. Listing both
  // costs a dead entry lua-language-server ignores; listing one breaks half the team.
  if (meta.cache_local) {
    // Relative, deliberately: an absolute path would name the machine that generated the
    // file. lua-language-server resolves workspace.library against the workspace root, and
    // is_envy_semver_path's "/envy/" search is position-independent.
    std::string p{ *meta.cache_local };
    std::replace(p.begin(), p.end(), '\\', '/');
    paths.push_back(p + "/envy/" + ver);
  } else {
    paths.push_back(std::string{ kDefaultCacheLocal } + "/envy/" + ver);
  }

  // Platform defaults for whoever is on the shared tree.
  paths.push_back("~/Library/Caches/envy/envy/" + ver);
  paths.push_back("~/.cache/envy/envy/" + ver);
  paths.push_back("${env:USERPROFILE}/AppData/Local/envy/envy/" + ver);

  return paths;
}

std::optional<std::string> rewrite_luarc_types_path(
    std::string_view content,
    std::vector<std::string> const &canonical_paths) {
  picojson::value root;
  std::string const json_str{ content };
  std::string err{ picojson::parse(root, json_str) };
  if (!err.empty() || !root.is<picojson::object>()) { return std::nullopt; }

  auto &obj{ root.get<picojson::object>() };
  auto it{ obj.find("workspace.library") };
  if (it == obj.end() || !it->second.is<picojson::array>()) { return std::nullopt; }

  auto &library{ it->second.get<picojson::array>() };

  // Partition: keep non-envy entries, remove envy entries
  std::vector<picojson::value> kept;
  for (auto const &entry : library) {
    if (entry.is<std::string>() && is_envy_semver_path(entry.get<std::string>())) {
      continue;  // remove envy entry
    }
    kept.push_back(entry);
  }

  // Append canonical paths
  for (auto const &p : canonical_paths) { kept.push_back(picojson::value(p)); }

  if ([&] {
        if (kept.size() != library.size()) { return false; }
        for (size_t i{ 0 }; i < kept.size(); ++i) {
          if (kept[i].serialize() != library[i].serialize()) { return false; }
        }
        return true;
      }()) {
    return std::nullopt;
  }

  library = std::move(kept);
  return root.serialize(true);
}

void update_luarc_types_path(fs::path const &project_dir, envy_meta const &meta) {
  fs::path const luarc_path{ project_dir / ".luarc.json" };
  if (!fs::exists(luarc_path)) { return; }

  auto const canonical{ compute_canonical_luarc_paths(meta) };

  auto const bytes{ util_load_file(luarc_path) };
  std::string const content{ bytes.begin(), bytes.end() };

  auto result{ rewrite_luarc_types_path(content, canonical) };
  if (!result) { return; }

  util_write_file(luarc_path, *result);
  tui::info("Updated .luarc.json types paths");
}

fs::path extract_lua_ls_types(fs::path const &cache_root) {
  fs::path const types_dir{ cache_root / "envy" / ENVY_VERSION_STR };
  fs::path const types_path{ types_dir / "envy.lua" };

  if (fs::exists(types_path)) { return types_dir; }

  std::error_code ec;
  fs::create_directories(types_dir, ec);
  if (ec) {
    throw std::runtime_error("init: failed to create types directory " +
                             types_dir.string() + ": " + ec.message());
  }

  auto const types{ stamp_placeholders(util_inflate_resource(embedded::kTypeDefinitions),
                                       kEnvyReleaseDownloadUrl) };
  util_write_file(types_path, types);

  tui::info("Extracted type definitions to %s", types_path.string().c_str());
  return types_dir;
}

void write_luarc(fs::path const &project_dir, envy_meta const &meta) {
  fs::path const luarc_path{ project_dir / ".luarc.json" };

  auto const canonical{ compute_canonical_luarc_paths(meta) };

  if (fs::exists(luarc_path)) {
    tui::info("");
    tui::info(".luarc.json already exists at %s", luarc_path.string().c_str());
    tui::info("To enable envy autocompletion, add the following to workspace.library:");
    for (auto const &p : canonical) { tui::info("  \"%s\"", p.c_str()); }
    return;
  }

  // Build comma-separated quoted entries for template substitution
  std::string entries;
  for (size_t i{ 0 }; i < canonical.size(); ++i) {
    if (i > 0) { entries += ",\n    "; }
    entries += "\"" + canonical[i] + "\"";
  }

  std::string content{ util_inflate_resource(embedded::kLuarcTemplate) };
  replace_all(content, "@@LUA_VERSION@@", LUA_VERSION);
  replace_all(content, "\"@@TYPES_DIR@@\"", entries);

  util_write_file(luarc_path, content);

  tui::info("Created %s", luarc_path.string().c_str());
}

}  // namespace envy
