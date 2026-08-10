#include "lua_envy_fetch.h"

#include "fetch.h"
#include "lua_phase_context.h"
#include "phases/phase_fetch.h"
#include "pkg.h"
#include "sha256.h"
#include "sol_util.h"
#include "tui.h"
#include "tui_actions.h"
#include "uri.h"

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace envy {
namespace {

struct fetch_item {
  std::string source;
  std::optional<std::string> sha256;
  std::optional<std::string> ref;
  std::optional<std::string> post_data;
  std::optional<std::string> dest;
};

// Parse envy.fetch() url_or_spec argument
std::pair<std::vector<fetch_item>, bool> parse_fetch_args(sol::object const &arg) {
  std::vector<fetch_item> items;
  bool is_array{ false };

  if (arg.is<std::string>()) {
    items.push_back({ arg.as<std::string>(), std::nullopt, std::nullopt, std::nullopt });
  } else if (arg.is<sol::table>()) {
    sol::table tbl{ arg.as<sol::table>() };
    sol::object first_elem{ tbl[1] };

    if (first_elem.get_type() == sol::type::lua_nil) {
      std::string source{
        sol_util_get_required<std::string>(tbl, "source", "envy.fetch")
      };

      auto sha256{ sol_util_get_optional<std::string>(tbl, "sha256", "envy.fetch") };
      auto ref{ sol_util_get_optional<std::string>(tbl, "ref", "envy.fetch") };
      auto post_data{ sol_util_get_optional<std::string>(tbl, "post_data", "envy.fetch") };
      auto dest{ sol_util_get_optional<std::string>(tbl, "dest", "envy.fetch") };
      items.push_back({ source, sha256, ref, post_data, dest });
    } else if (first_elem.is<std::string>()) {
      is_array = true;

      for (auto const &[key, value] : tbl) {
        if (!value.is<std::string>()) {
          throw std::runtime_error("envy.fetch: array elements must be strings");
        }

        items.push_back(
            { value.as<std::string>(), std::nullopt, std::nullopt, std::nullopt });
      }
    } else if (first_elem.is<sol::table>()) {
      is_array = true;
      for (auto const &[key, value] : tbl) {
        if (!value.is<sol::table>()) {
          throw std::runtime_error("envy.fetch: array elements must be tables");
        }

        sol::table item_tbl{ value.as<sol::table>() };
        std::string source{ sol_util_get_required<std::string>(
            item_tbl,
            "source",
            "envy.fetch array element") };
        auto sha256{
          sol_util_get_optional<std::string>(item_tbl, "sha256", "envy.fetch")
        };
        auto ref{ sol_util_get_optional<std::string>(item_tbl, "ref", "envy.fetch") };
        auto post_data{
          sol_util_get_optional<std::string>(item_tbl, "post_data", "envy.fetch")
        };
        auto dest{ sol_util_get_optional<std::string>(item_tbl, "dest", "envy.fetch") };

        items.push_back({ source, sha256, ref, post_data, dest });
      }
    } else {
      throw std::runtime_error("envy.fetch: invalid array element type");
    }
  } else {
    throw std::runtime_error("envy.fetch: argument must be string or table");
  }

  return { std::move(items), is_array };
}

struct commit_entry {
  std::string filename;
  std::string sha256;  // empty = no verification
};

// Parse envy.commit_fetch() arguments
std::vector<commit_entry> parse_commit_fetch_args(sol::object const &arg) {
  std::vector<commit_entry> entries;

  if (arg.is<std::string>()) {
    entries.push_back({ arg.as<std::string>(), "" });
  } else if (arg.is<sol::table>()) {
    sol::table tbl{ arg.as<sol::table>() };
    sol::object first_elem{ tbl[1] };

    if (first_elem.get_type() == sol::type::lua_nil) {
      // Single table: {filename="...", sha256="..."}
      std::string filename{
        sol_util_get_required<std::string>(tbl, "filename", "envy.commit_fetch")
      };
      auto sha256{
        sol_util_get_optional<std::string>(tbl, "sha256", "envy.commit_fetch")
      };
      entries.push_back({ filename, sha256.value_or("") });
    } else if (first_elem.is<std::string>()) {
      // Array of strings: {"file1", "file2"}
      for (auto const &[key, value] : tbl) {
        if (!value.is<std::string>()) {
          throw std::runtime_error("envy.commit_fetch: array elements must be strings");
        }
        entries.push_back({ value.as<std::string>(), "" });
      }
    } else if (first_elem.is<sol::table>()) {
      // Array of tables: {{filename="...", sha256="..."}, {...}}
      for (auto const &[key, value] : tbl) {
        if (!value.is<sol::table>()) {
          throw std::runtime_error("envy.commit_fetch: array elements must be tables");
        }
        sol::table item_tbl{ value.as<sol::table>() };
        std::string filename{ sol_util_get_required<std::string>(
            item_tbl,
            "filename",
            "envy.commit_fetch array element") };
        auto sha256{
          sol_util_get_optional<std::string>(item_tbl, "sha256", "envy.commit_fetch")
        };
        entries.push_back({ filename, sha256.value_or("") });
      }
    } else {
      throw std::runtime_error("envy.commit_fetch: invalid array element type");
    }
  } else {
    throw std::runtime_error("envy.commit_fetch: argument must be string or table");
  }

  return entries;
}

void commit_files(std::vector<commit_entry> const &entries,
                  std::filesystem::path const &tmp_dir,
                  std::filesystem::path const &fetch_dir) {
  std::vector<std::string> errors;

  for (auto const &entry : entries) {
    std::filesystem::path const src{ tmp_dir / entry.filename };
    std::filesystem::path const dest{ fetch_dir / entry.filename };

    if (!std::filesystem::exists(src)) {
      errors.push_back(entry.filename + ": file not found in tmp directory");
      continue;
    }

    if (!entry.sha256.empty()) {
      try {
        tui::debug("envy.commit_fetch: verifying SHA256 for %s", entry.filename.c_str());
        sha256_verify(entry.sha256, sha256(src));
      } catch (std::exception const &e) {
        errors.push_back(entry.filename + ": " + e.what());
        continue;
      }
    }

    try {
      std::filesystem::rename(src, dest);
      tui::debug("envy.commit_fetch: moved %s to fetch_dir", entry.filename.c_str());
    } catch (std::exception const &e) {
      errors.push_back(entry.filename + ": failed to move: " + e.what());
    }
  }

  if (!errors.empty()) {
    std::ostringstream oss;
    oss << "envy.commit_fetch failed:\n";
    for (auto const &err : errors) { oss << "  " << err << "\n"; }
    throw std::runtime_error(oss.str());
  }
}

}  // namespace

void lua_envy_fetch_install(sol::table &envy_table) {
  // envy.fetch(source_or_spec, opts) - Download files with explicit destination
  envy_table["fetch"] =
      [](sol::object arg, sol::table opts, sol::this_state L) -> sol::object {
    sol::state_view lua{ L };

    // dest is required
    std::string dest_str{ sol_util_get_required<std::string>(opts, "dest", "envy.fetch") };
    std::filesystem::path const dest_dir{ dest_str };

    auto [items, is_array] = parse_fetch_args(arg);

    std::vector<std::string> sources, basenames;
    std::vector<fetch_request> requests;

    // Track used basenames for deduplication
    std::unordered_set<std::string> used_basenames;

    phase_context const *ctx{ lua_phase_context_get(L) };
    pkg *p{ ctx ? ctx->p : nullptr };

    for (auto const &item : items) {
      std::string basename;
      if (item.dest) {
        if (item.dest->empty()) {
          throw std::runtime_error("envy.fetch: dest cannot be empty for source: " +
                                   item.source);
        }
        if (item.dest->find_first_of("/\\") != std::string::npos || *item.dest == ".." ||
            *item.dest == ".") {
          throw std::runtime_error(
              "envy.fetch: dest must be a plain filename (no path separators): " +
              *item.dest);
        }
        basename = *item.dest;
      } else {
        basename = uri_extract_filename(item.source);
        if (basename.empty()) {
          throw std::runtime_error("envy.fetch: cannot extract filename from source: " +
                                   item.source);
        }
      }

      // Deduplicate basenames
      std::string final_basename{ basename };
      int suffix{ 2 };
      while (used_basenames.contains(final_basename)) {
        size_t const dot_pos{ basename.find_last_of('.') };
        if (dot_pos != std::string::npos) {
          final_basename = basename.substr(0, dot_pos) + "-" + std::to_string(suffix) +
                           basename.substr(dot_pos);
        } else {
          final_basename = basename + "-" + std::to_string(suffix);
        }
        ++suffix;
      }
      used_basenames.insert(final_basename);
      basenames.push_back(final_basename);
      sources.push_back(item.source);

      std::filesystem::path const file_dest{ dest_dir / final_basename };
      requests.push_back(url_to_fetch_request(item.source,
                                              file_dest,
                                              item.ref,
                                              item.post_data,
                                              "envy.fetch"));
    }

    tui::debug("envy.fetch: downloading %zu file(s) to %s",
               sources.size(),
               dest_dir.string().c_str());

    // Every download in the process reports through this one tracker, whatever the
    // count or scheme (see tui_actions::fetch_all_progress_tracker).
    std::optional<tui_actions::fetch_all_progress_tracker> tracker;
    if (p && p->tui_section) {
      tracker.emplace(p->tui_section, p->cfg->identity, basenames, "fetch");
      for (size_t i{ 0 }; i < requests.size(); ++i) {
        auto cb{ tracker->make_callback(i) };
        std::visit([&](auto &rq) { rq.progress = std::move(cb); }, requests[i]);
      }
    }

    auto const results{ fetch(requests, p->cfg->identity) };

    // Check for errors and verify SHA256 if provided
    std::vector<std::string> errors;
    for (size_t i = 0; i < results.size(); ++i) {
      if (auto const *err{ std::get_if<std::string>(&results[i]) }) {
        errors.push_back(sources[i] + ": " + *err);
      } else if (items[i].sha256) {
        // Verify SHA256 if specified
        std::filesystem::path const file_path{ dest_dir / basenames[i] };
        try {
          sha256_verify(*items[i].sha256, sha256(file_path));
        } catch (std::exception const &e) {
          errors.push_back(sources[i] + ": " + e.what());
        }
      }
    }

    if (!errors.empty()) {
      std::ostringstream oss;
      oss << "envy.fetch failed:\n";
      for (auto const &err : errors) { oss << "  " << err << "\n"; }
      throw std::runtime_error(oss.str());
    }

    // Return basename(s) to Lua
    if (is_array || sources.size() > 1) {
      sol::table result{ lua.create_table(static_cast<int>(basenames.size()), 0) };
      for (size_t i = 0; i < basenames.size(); ++i) { result[i + 1] = basenames[i]; }
      return result;
    } else {
      return sol::make_object(lua, basenames[0]);
    }
  };

  // envy.commit_fetch(files) - Atomically move verified files from tmp_dir to fetch_dir
  envy_table["commit_fetch"] = [](sol::object arg, sol::this_state L) {
    phase_context const *ctx{ lua_phase_context_get(L) };
    if (!ctx || !ctx->lock) {
      throw std::runtime_error(
          "envy.commit_fetch: can only be called from FETCH phase with cache lock active");
    }
    commit_files(parse_commit_fetch_args(arg),
                 ctx->lock->tmp_dir(),
                 ctx->lock->fetch_dir());
  };

  // envy.verify_hash(file_path, expected_sha256) - Verify file hash
  envy_table["verify_hash"] = [](std::string const &file_path_str,
                                 std::string const &expected_sha256) -> bool {
    std::filesystem::path const file_path{ file_path_str };

    if (!std::filesystem::exists(file_path)) {
      throw std::runtime_error("envy.verify_hash: file not found: " + file_path.string());
    }

    try {
      sha256_verify(expected_sha256, sha256(file_path));
      return true;
    } catch (std::exception const &) { return false; }
  };
}

}  // namespace envy
