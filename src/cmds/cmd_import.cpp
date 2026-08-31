#include "cmd_import.h"

#include "cache.h"
#include "engine.h"
#include "extract.h"
#include "manifest.h"
#include "package_depot.h"
#include "pkg_cfg.h"
#include "reexec.h"
#include "self_deploy.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include "cli_parse.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace envy {

namespace {

// `import` has two shapes: the depot path goes through cmd_startup_load, which resolves
// the cache for it, while a bare archive has no manifest and must resolve for itself.
cache_root_request cmd_import_cache_request(
    std::optional<std::filesystem::path> const &cli_cache_root,
    std::optional<std::filesystem::path> const &project_dir) {
  envy_meta meta;
  std::filesystem::path manifest_dir;
  // Skipped under an override, which already decides the root: discover() parses
  // directives and throws, so reading a manifest that cannot change the answer would let a
  // bad directive anywhere above the cwd fail an import that named its cache explicitly.
  if (!cli_cache_root) {
    if (auto const found{
            manifest::discover(false, manifest::discovery_start_dir(project_dir)) }) {
      meta = found->meta;
      manifest_dir = found->path.parent_path();
    }
  }
  return meta.cache_request(cli_cache_root, manifest_dir);
}

}  // namespace

namespace {

bool is_hex_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool directory_has_entries(std::filesystem::path const &dir) {
  std::error_code ec;
  std::filesystem::directory_iterator it{ dir, ec };
  if (ec) { return false; }
  return it != std::filesystem::directory_iterator{};
}

struct import_result {
  std::string identity;
  std::filesystem::path pkg_path;
  bool was_cached;
  bool is_fetch_only;
};

import_result import_one_archive(cache &c,
                                 std::filesystem::path const &archive_path,
                                 tui::section_handle section = tui::kInvalidSection) {
  std::string filename{ archive_path.filename().string() };

  std::string_view stem{ filename };
  if (stem.size() > 8 && stem.substr(stem.size() - 8) == ".tar.zst") {
    stem = stem.substr(0, stem.size() - 8);
  } else {
    throw std::runtime_error("import: archive must have .tar.zst extension");
  }

  auto const parsed{ util_parse_archive_filename(stem) };
  if (!parsed) {
    throw std::runtime_error(
        "import: invalid archive filename, expected "
        "<identity>@<revision>-<platform>-<arch>-blake3-<hash_prefix>.tar.zst");
  }

  std::string const label{ "[" + parsed->identity + "]" };

  auto result{ c.ensure_pkg(
      parsed->identity,
      parsed->platform,
      parsed->arch,
      parsed->hash_prefix,
      tui_actions::lock_wait_spinner(section, parsed->identity, parsed->identity)) };

  if (!result.lock) {
    if (section) {
      tui::section_set_content(
          section,
          tui::section_frame{ .label = label,
                              .content = tui::static_text_data{ .text = "cached" } });
      tui::section_set_complete(section);
    }
    return { parsed->identity, result.pkg_path, true, false };
  }

  // No pre-scan for a lone archive, so no total: the tracker spins on the running counts
  // and lands on a full bar once the archive is out.
  std::optional<tui_actions::extract_progress_tracker> tracker;
  if (section) {
    tracker.emplace(section, parsed->identity, archive_path.filename().string());
  }

  extract_options opts;
  if (tracker) { opts.progress = std::ref(*tracker); }
  extract(archive_path, result.entry_path, opts);
  if (tracker) { tracker->finish(); }

  if (directory_has_entries(result.lock->install_dir())) {
    result.lock->mark_install_complete();
    if (section) {
      tui::section_set_content(
          section,
          tui::section_frame{ .label = label,
                              .content = tui::static_text_data{ .text = "imported" } });
      tui::section_set_complete(section);
    }
    return { parsed->identity, result.pkg_path, false, false };
  }

  if (directory_has_entries(result.lock->fetch_dir())) {
    result.lock->mark_fetch_complete();
    if (section) {
      tui::section_set_content(section,
                               tui::section_frame{ .label = label,
                                                   .content = tui::static_text_data{
                                                       .text = "imported (fetch)" } });
      tui::section_set_complete(section);
    }
    return { parsed->identity, result.entry_path, false, true };
  }

  throw std::runtime_error("import: archive did not populate pkg/ or fetch/ directories");
}

// Parse a checksums file (<64hex>  <path-or-url> per line).
// Returns entries with sha256 + url.
std::vector<depot_entry> parse_checksums_file(std::filesystem::path const &path) {
  std::vector<depot_entry> entries;

  std::ifstream in{ path };
  if (!in) {
    throw std::runtime_error("import: cannot open checksums file: " + path.string());
  }

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (line.empty() || line[0] == '#') { continue; }

    // Require SHA256 prefix: 64 hex chars + two spaces
    if (line.size() <= 66 || line[64] != ' ' || line[65] != ' ') {
      tui::warn("import: skipping non-checksums line: %s", line.c_str());
      continue;
    }

    bool all_hex{ true };
    for (size_t i{ 0 }; i < 64; ++i) {
      if (!is_hex_char(line[i])) {
        all_hex = false;
        break;
      }
    }
    if (!all_hex) {
      tui::warn("import: skipping non-checksums line: %s", line.c_str());
      continue;
    }

    std::string hash{ line.substr(0, 64) };
    for (auto &c : hash) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string url{ line.substr(66) };

    entries.push_back(depot_entry{ std::move(url), std::move(hash) });
  }

  return entries;
}

// Parse a checksums file into a filename → sha256 map (for --dir --checksums).
std::unordered_map<std::string, std::string> parse_checksums_map(
    std::filesystem::path const &path) {
  auto const entries{ parse_checksums_file(path) };
  std::unordered_map<std::string, std::string> result;
  for (auto const &e : entries) {
    // Extract filename from URL/path
    std::string_view sv{ e.url };
    auto const slash{ sv.rfind('/') };
    std::string_view filename{ slash != std::string_view::npos ? sv.substr(slash + 1)
                                                               : sv };
    if (e.sha256) { result.try_emplace(std::string(filename), *e.sha256); }
  }
  return result;
}

}  // namespace

cli_cmd &cmd_import::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("import", "Import package archive into cache") };
  sub.pos("archive", c.archive_path, "Path to .tar.zst or .txt manifest").check_file();
  sub.opt("--dir", c.dir, "Directory of .tar.zst archives to import").check_dir();
  sub.opt("--manifest", c.manifest_path, "Path to envy.lua manifest");
  sub.opt("--checksums", c.checksums_path, "Path to checksums .txt file").check_file();
  sub.finalize(
      [](void *p) -> char const * {
        auto const &sel{ *static_cast<cfg *>(p) };
        bool const has_archive{ !sel.archive_path.empty() };
        if (has_archive && sel.dir) { return "Cannot specify both archive and --dir"; }
        if (!has_archive && !sel.dir) {
          return "Must specify either archive/manifest or --dir";
        }
        return nullptr;
      },
      &c);
  return sub;
}

cmd_import::cmd_import(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_import::execute() {
  if (!cfg_.dir) {
    // Positional arg mode: detect by extension
    auto const ext{ cfg_.archive_path.extension().string() };

    if (ext == ".txt") {
      // Depot manifest import — build index from file, let engine handle everything
      auto const [m, c]{ cmd_startup_load("import",
                                          cfg_.manifest_path,
                                          cli_cache_root_,
                                          false,
                                          cfg_.project_dir) };

      auto const data{ util_load_file(cfg_.archive_path) };
      std::string contents(reinterpret_cast<char const *>(data.data()), data.size());

      auto depot{ package_depot_index::build_from_contents({ contents }) };
      if (depot.empty()) {
        tui::warn("import: no valid entries in %s", cfg_.archive_path.string().c_str());
        return;
      }

      engine eng{ *c, m.get() };
      eng.set_depot_index(std::move(depot));

      eng.run_full({ m->packages.begin(), m->packages.end() });
      return;
    }

    if (ext == ".zst") {
      // Single archive import
      // Manifest-aware like every other path into the cache: built from the CLI override
      // alone, a single-archive import landed in the user-wide tree while the rest of the
      // project used its own.
      cache c{ resolve_cache_root(
                   cmd_import_cache_request(cli_cache_root_, cfg_.project_dir))
                   .root };
      auto const section{ tui::section_create() };
      auto result{ import_one_archive(c, cfg_.archive_path, section) };
      tui::section_commit(section);  // the outcome row belongs above the path it produced
      if (result.is_fetch_only) {
        tui::print_stdout("fetch-only import: %s\n", result.pkg_path.string().c_str());
      } else {
        tui::print_stdout("%s\n", result.pkg_path.string().c_str());
      }
      return;
    }

    throw std::runtime_error(
        "import: unrecognized file extension '" + ext +
        "' (expected .tar.zst for archives or .txt for depot manifests)");
  }

  // Directory import — build depot index from directory, let engine handle everything
  auto const [m, c]{ cmd_startup_load("import",
                                      cfg_.manifest_path,
                                      cli_cache_root_,
                                      false,
                                      cfg_.project_dir) };

  package_depot_index depot;
  if (cfg_.checksums_path) {
    auto checksums{ parse_checksums_map(*cfg_.checksums_path) };
    depot = package_depot_index::build_from_directory(*cfg_.dir, checksums);
  } else {
    depot = package_depot_index::build_from_directory(*cfg_.dir);
  }

  if (depot.empty()) {
    tui::warn("import: no .tar.zst files found in %s", cfg_.dir->string().c_str());
    return;
  }

  engine eng{ *c, m.get() };
  eng.set_depot_index(std::move(depot));

  eng.run_full({ m->packages.begin(), m->packages.end() });
}

}  // namespace envy
