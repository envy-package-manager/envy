#include "cmd_merge_depot.h"

#include "fetch.h"
#include "platform.h"
#include "tui.h"
#include "tui_actions.h"
#include "uri.h"

#include "CLI11.hpp"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace envy {
namespace {

bool is_hex_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::vector<depot_manifest_entry> parse_manifest_lines(std::istream &in) {
  std::vector<depot_manifest_entry> entries;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (line.empty() || line[0] == '#') { continue; }

    if (line.size() <= 66 || line[64] != ' ' || line[65] != ' ') {
      tui::warn("merge-depot: skipping malformed line: %s", line.c_str());
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
      tui::warn("merge-depot: skipping malformed line: %s", line.c_str());
      continue;
    }

    std::string hash{ line.substr(0, 64) };
    for (auto &c : hash) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    entries.push_back(depot_manifest_entry{ std::move(hash), line.substr(66) });
  }
  return entries;
}

std::unordered_set<std::string> parse_retain_lines(std::istream &in) {
  std::unordered_set<std::string> paths;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (line.empty() || line[0] == '#') { continue; }
    paths.insert(std::move(line));
  }
  return paths;
}

}  // namespace

std::vector<depot_manifest_entry> parse_depot_manifest(std::filesystem::path const &file) {
  std::ifstream in{ file };
  if (!in) {
    throw std::runtime_error("merge-depot: cannot open depot manifest: " + file.string());
  }
  return parse_manifest_lines(in);
}

std::unordered_set<std::string> parse_s3_ls_lines(std::istream &in) {
  std::unordered_set<std::string> keys;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (line.empty()) { continue; }

    // Skip PRE (directory) lines: leading whitespace + "PRE " + prefix
    auto const pos{ line.find_first_not_of(' ') };
    if (pos != std::string::npos && line.compare(pos, 4, "PRE ") == 0) { continue; }

    // Object line format: "YYYY-MM-DD HH:MM:SS <spaces> <size> <key>"
    // Positions: 0-9 date, 10 space, 11-18 time, 19+ spaces then size then space then key
    if (line.size() < 21) {
      tui::warn("merge-depot: skipping malformed s3 ls line: %s", line.c_str());
      continue;
    }

    size_t i{ 19 };
    while (i < line.size() && line[i] == ' ') { ++i; }
    auto const size_start{ i };
    while (i < line.size() && line[i] != ' ') { ++i; }
    if (i == size_start || i >= line.size()) {
      tui::warn("merge-depot: skipping malformed s3 ls line: %s", line.c_str());
      continue;
    }
    auto const size_ok{ [&] {
      for (size_t j{ size_start }; j < i; ++j) {
        if (!std::isdigit(static_cast<unsigned char>(line[j]))) { return false; }
      }
      return true;
    }() };
    if (!size_ok) {
      tui::warn("merge-depot: skipping malformed s3 ls line: %s", line.c_str());
      continue;
    }
    ++i;  // skip single space after size
    if (i >= line.size()) {
      tui::warn("merge-depot: skipping malformed s3 ls line: %s", line.c_str());
      continue;
    }

    keys.insert(line.substr(i));
  }
  return keys;
}

void cmd_merge_depot::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("merge-depot", "Merge depot manifest files") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("depot_manifests",
                  cfg_ptr->depot_manifests,
                  "Per-OS depot manifest files to merge")
      ->required()
      ->check(CLI::ExistingFile);
  sub->add_option("--existing",
                  cfg_ptr->existing_path,
                  "Existing merged depot manifest (local path or remote URL)");
  auto retain_str{ std::make_shared<std::string>() };
  auto *retain_opt{ sub->add_option("--retain",
                                    *retain_str,
                                    "Retain list: prune entries whose path is absent") };
  auto *retain_s3_ls_opt{
    sub->add_option("--retain-s3-ls", *retain_str, "Retain list in 'aws s3 ls' format")
  };
  retain_opt->excludes(retain_s3_ls_opt);
  retain_opt->each([cfg_ptr](std::string const &val) {
    cfg_ptr->retain = retain_source{ val, retain_format::PLAIN };
  });
  retain_s3_ls_opt->each([cfg_ptr](std::string const &val) {
    cfg_ptr->retain = retain_source{ val, retain_format::S3_LS };
  });
  sub->add_option("--retain-prefix",
                  cfg_ptr->retain_prefix,
                  "Prefix to prepend to each retain entry before matching");
  sub->add_flag("--strict",
                cfg_ptr->strict,
                "Treat hash changes vs existing depot manifest as errors");
  sub->callback([cfg_ptr, retain_str, on_selected = std::move(on_selected)] {
    (void)retain_str;  // prevent -Wunused-lambda-capture; extends lifetime for CLI11
    on_selected(*cfg_ptr);
  });
}

cmd_merge_depot::cmd_merge_depot(
    cmd_merge_depot::cfg cfg,
    std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_merge_depot::execute() {
  std::map<std::string, std::string> merged;  // path -> hash, sorted
  std::map<std::string, std::string> existing_hashes;

  if (cfg_.existing_path) {
    std::vector<depot_manifest_entry> existing_entries;

    if (auto const info{ uri_classify(*cfg_.existing_path) };
        info.scheme == uri_scheme::LOCAL_FILE_ABSOLUTE ||
        info.scheme == uri_scheme::LOCAL_FILE_RELATIVE) {
      std::filesystem::path p{ info.canonical };
      if (!std::filesystem::exists(p)) {
        throw std::runtime_error("merge-depot: --existing file not found: " +
                                 *cfg_.existing_path);
      }
      existing_entries = parse_depot_manifest(p);
    } else {
      // Fetch remote manifest to unique temp file, read into memory, clean up, parse
      scoped_path_cleanup tmp_guard{ platform::create_unique_temp_file(
          "envy-merge-depot") };

      auto results{ tui_actions::fetch_tracked(
          { fetch_request_from_url(*cfg_.existing_path, tmp_guard.path()) },
          "merge-depot",
          { *cfg_.existing_path }) };
      if (auto const *err{ std::get_if<std::string>(&results[0]) }) {
        throw std::runtime_error("merge-depot: failed to fetch --existing: " + *err);
      }

      auto const content{ [&] {
        std::ifstream in{ tmp_guard.path() };
        if (!in) {
          throw std::runtime_error(
              "merge-depot: failed to read fetched --existing manifest");
        }
        return std::string{ std::istreambuf_iterator<char>{ in },
                            std::istreambuf_iterator<char>{} };
      }() };

      auto existing_stream{ std::istringstream{ content } };
      existing_entries = parse_manifest_lines(existing_stream);
    }

    for (auto &e : existing_entries) {
      if (auto [it, inserted]{ merged.emplace(e.path, e.hash) }; inserted) {
        existing_hashes.emplace(std::move(e.path), std::move(e.hash));
      } else {
        tui::warn("merge-depot: duplicate path in existing manifest: %s", e.path.c_str());
      }
    }
  }

  std::set<std::string> new_paths;

  for (auto const &manifest_path : cfg_.depot_manifests) {
    for (auto &e : parse_depot_manifest(manifest_path)) {
      if (auto it{ merged.find(e.path) }; it != merged.end() && it->second != e.hash) {
        if (new_paths.count(e.path) > 0) {
          // Path already modified by a prior new manifest — cross-input conflict
          throw std::runtime_error("merge-depot: conflicting hashes for " + e.path +
                                   " across input depot manifests");
        }

        // Hash changed vs existing depot manifest
        if (cfg_.strict) {
          auto ex_it{ existing_hashes.find(e.path) };
          throw std::runtime_error("merge-depot: hash changed for " + e.path +
                                   " (existing: " + ex_it->second + ", new: " + e.hash +
                                   ")");
        }
        tui::warn("merge-depot: hash changed for %s", e.path.c_str());
      }

      new_paths.insert(e.path);
      merged.insert_or_assign(std::move(e.path), std::move(e.hash));
    }
  }

  if (cfg_.retain_prefix && !cfg_.retain) {
    throw std::runtime_error(
        "merge-depot: --retain-prefix requires --retain or --retain-s3-ls");
  }

  if (cfg_.retain) {
    auto const &[source_path, fmt]{ *cfg_.retain };
    auto const is_s3_ls{ fmt == retain_format::S3_LS };
    auto const *flag_name{ is_s3_ls ? "--retain-s3-ls" : "--retain" };
    auto const retain_set{ [&]() -> std::unordered_set<std::string> {
      std::unordered_set<std::string> set;

      if (auto const info{ uri_classify(source_path) };
          info.scheme == uri_scheme::LOCAL_FILE_ABSOLUTE ||
          info.scheme == uri_scheme::LOCAL_FILE_RELATIVE) {
        std::filesystem::path const p{ info.canonical };
        if (!std::filesystem::exists(p)) {
          throw std::runtime_error(std::string("merge-depot: ") + flag_name +
                                   " file not found: " + source_path);
        }
        std::ifstream in{ p };
        if (!in) {
          throw std::runtime_error(std::string("merge-depot: cannot open ") + flag_name +
                                   " file: " + source_path);
        }
        set = is_s3_ls ? parse_s3_ls_lines(in) : parse_retain_lines(in);
      } else {
        scoped_path_cleanup tmp_guard{ platform::create_unique_temp_file(
            "envy-merge-depot-retain") };

        auto results{ tui_actions::fetch_tracked(
            { fetch_request_from_url(source_path, tmp_guard.path()) },
            "merge-depot",
            { source_path }) };
        if (auto const *err{ std::get_if<std::string>(&results[0]) }) {
          throw std::runtime_error(std::string("merge-depot: failed to fetch ") +
                                   flag_name + ": " + *err);
        }

        auto const content{ [&] {
          std::ifstream in{ tmp_guard.path() };
          if (!in) {
            throw std::runtime_error(std::string("merge-depot: failed to read fetched ") +
                                     flag_name + " list");
          }
          return std::string{ std::istreambuf_iterator<char>{ in },
                              std::istreambuf_iterator<char>{} };
        }() };

        auto retain_stream{ std::istringstream{ content } };
        set = is_s3_ls ? parse_s3_ls_lines(retain_stream)
                       : parse_retain_lines(retain_stream);
      }

      if (cfg_.retain_prefix) {
        std::unordered_set<std::string> prefixed;
        for (auto &p : set) { prefixed.insert(*cfg_.retain_prefix + p); }
        return prefixed;
      }
      return set;
    }() };

    for (auto it{ merged.begin() }; it != merged.end();) {
      if (retain_set.count(it->first) == 0 && new_paths.count(it->first) == 0) {
        it = merged.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto const &[path, hash] : merged) {
    tui::print_stdout("%s  %s\n", hash.c_str(), path.c_str());
  }
}

}  // namespace envy
