#include "pkg_state.h"

#include "cache.h"
#include "engine.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "tui.h"
#include "util.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace envy {

namespace {

std::string ascii_lower(std::string_view s) {
  std::string out{ s };
  for (char &c : out) {
    if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
  }
  return out;
}

}  // namespace

std::filesystem::path pkg_state_root(std::filesystem::path const &project_root) {
  return project_root / ".envy" / "state";
}

std::string_view pkg_state_key(std::string_view identity) {
  return identity.substr(0, identity.find('@'));
}

std::filesystem::path pkg_state_dir(pkg const *p, std::filesystem::path const &root) {
  return pkg_state_root(root) / pkg_state_key(p->cfg->identity);
}

std::vector<pkg_state_entry> pkg_state_entries(std::filesystem::path const &project_root,
                                               std::error_code &ec) {
  namespace fs = std::filesystem;
  std::vector<pkg_state_entry> entries;
  fs::directory_iterator it{ pkg_state_root(project_root), ec };
  if (ec == std::errc::no_such_file_or_directory) { ec.clear(); }
  for (fs::directory_iterator const end; !ec && it != end; it.increment(ec)) {
    std::error_code type_ec;
    auto const type{ it->symlink_status(type_ec).type() };
    if (type_ec == std::errc::no_such_file_or_directory) { continue; }  // gone since
    if (type_ec) {
      ec = type_ec;
      break;
    }
    entries.push_back({ util_path_to_utf8(it->path().filename()), it->path(), type });
  }
  util_sort(entries, [](pkg_state_entry const &a, pkg_state_entry const &b) {
    return a.name < b.name;
  });
  return entries;
}

std::vector<pkg_state_entry> pkg_state_entries(std::filesystem::path const &project_root) {
  std::error_code ec;
  auto entries{ pkg_state_entries(project_root, ec) };
  if (ec) {
    throw_runtime_error(
        { "cannot read ", pkg_state_root(project_root), ": ", ec.message() });
  }
  return entries;
}

std::vector<std::string> pkg_state_unused(engine const &eng,
                                          std::filesystem::path const &project_root,
                                          std::vector<pkg_state_entry> const &entries) {
  std::unordered_set<std::string> used;
  std::unordered_map<std::string, std::string> by_lower;  // lowercased key -> key
  for (pkg const *p : eng.packages()) {
    std::string key{ pkg_state_key(p->cfg->identity) };
    by_lower.emplace(ascii_lower(key), key);
    used.insert(std::move(key));
  }
  auto const root{ pkg_state_root(project_root) };
  std::vector<std::string> unused;
  for (auto const &e : entries) {
    if (used.contains(e.name)) { continue; }
    auto const variant{ by_lower.find(ascii_lower(e.name)) };
    std::error_code ec;  // an unresolvable spelling is a distinct entry
    if (variant == by_lower.end() ||
        !std::filesystem::equivalent(e.path, root / variant->second, ec)) {
      unused.push_back(e.name);
    }
  }
  return unused;
}

void pkg_state_report_unused(engine const &eng, manifest const &m) {
  std::error_code ec;
  auto const entries{ pkg_state_entries(m.project_root, ec) };
  if (ec) {
    tui::warn("Package state: cannot read %s: %s",
              pkg_state_root(m.project_root).string().c_str(),
              ec.message().c_str());
    return;
  }
  auto const unused{ pkg_state_unused(eng, m.project_root, entries) };
  if (unused.empty()) { return; }
  tui::info("Unused package state: %s (%s state clean <name> removes one)",
            util_join(unused).c_str(),
            project_launcher(m.meta.bin).c_str());
}

}  // namespace envy
