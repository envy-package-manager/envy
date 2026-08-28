#include "phase_fetch.h"

#include "cache.h"
#include "engine.h"
#include "fetch.h"
#include "lua_ctx/lua_phase_context.h"
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "pkg.h"
#include "sha256.h"
#include "sol_util.h"
#include "trace.h"
#include "tui.h"
#include "tui_actions.h"
#include "uri.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace envy {

// Not in anonymous namespace so tests can call it
fetch_request url_to_fetch_request(std::string const &url,
                                   std::filesystem::path const &dest,
                                   std::optional<std::string> const &ref,
                                   std::optional<std::string> const &post_data,
                                   std::string const &context,
                                   std::optional<std::filesystem::path> const &file_root) {
  auto const info{ uri_classify(url) };

  if (post_data && info.scheme != uri_scheme::HTTP && info.scheme != uri_scheme::HTTPS) {
    throw std::runtime_error("post_data only valid for HTTP/HTTPS in " + context);
  }

  switch (info.scheme) {
    case uri_scheme::HTTP:
      return fetch_request_http{ .source = url,
                                 .destination = dest,
                                 .post_data = post_data };
    case uri_scheme::HTTPS:
      return fetch_request_https{ .source = url,
                                  .destination = dest,
                                  .post_data = post_data };
    case uri_scheme::FTP: return fetch_request_ftp{ .source = url, .destination = dest };
    case uri_scheme::FTPS: return fetch_request_ftps{ .source = url, .destination = dest };
    case uri_scheme::S3:
      return fetch_request_s3{ .source = url, .destination = dest };
      // TODO: region support
    case uri_scheme::LOCAL_FILE_ABSOLUTE:
    case uri_scheme::LOCAL_FILE_RELATIVE:
      return fetch_request_file{ .source = url,
                                 .destination = dest,
                                 .file_root =
                                     file_root.value_or(std::filesystem::path{}) };
    case uri_scheme::GIT:
    case uri_scheme::GIT_HTTPS:
      if (!ref.has_value() || ref->empty()) {
        throw std::runtime_error("Git URLs require 'ref' field in " + context);
      }
      return fetch_request_git{ .source = info.canonical,
                                .destination = dest,
                                .ref = *ref,
                                .scheme = info.scheme };
    default: throw std::runtime_error("Unsupported URL scheme in " + context + ": " + url);
  }
}

namespace {

// Helper to extract destination from fetch_request variant
std::filesystem::path const &get_destination(fetch_request const &req) {
  return std::visit(
      [](auto const &r) -> std::filesystem::path const & { return r.destination; },
      req);
}

// Helper to extract source from fetch_request variant
std::string const &get_source(fetch_request const &req) {
  return std::visit([](auto const &r) -> std::string const & { return r.source; }, req);
}

struct fetch_spec {
  fetch_request request;
  std::string sha256;
};

struct table_entry {
  std::string url;
  std::string sha256;
  std::optional<std::string> ref;
  std::optional<std::string> post_data;
  std::optional<std::string> dest;
};

std::vector<fetch_spec> parse_fetch_field(sol::object const &fetch_obj,
                                          std::filesystem::path const &fetch_dir,
                                          std::filesystem::path const &stage_dir,
                                          std::string const &key);
std::vector<size_t> determine_downloads_needed(std::vector<fetch_spec> const &specs,
                                               std::string const &identity);
void execute_downloads(std::vector<fetch_spec> const &specs,
                       std::vector<size_t> const &to_download_indices,
                       std::string const &key,
                       tui::section_handle section);

bool run_programmatic_fetch(sol::protected_function fetch_func,
                            cache::scoped_entry_lock *lock,
                            std::string const &identity,
                            engine &eng,
                            pkg *p) {
  tui::debug("fetch: running fetch function");

  std::filesystem::path const tmp_dir{ lock->tmp_dir() };

  // Set up Lua registry context for envy.* functions (run_dir = tmp_dir, lock for
  // commit_fetch)
  phase_context_guard ctx_guard{ &eng, p, fetch_func.lua_state(), tmp_dir, lock };

  sol::state_view lua{ fetch_func.lua_state() };
  sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };

  sol::protected_function_result result{ call_lua_function_with_enriched_errors(
      p,
      "FETCH",
      [&]() { return fetch_func(util_path_with_separator(tmp_dir), opts); }) };

  bool should_mark_complete{ true };
  sol::object return_value{ result };

  if (return_value.get_type() == sol::type::none ||
      return_value.get_type() == sol::type::lua_nil) {
    // Imperative fetch: the function did its own downloading.
  } else if (return_value.is<std::string>() || return_value.is<sol::table>()) {
    auto const fetch_specs{
      parse_fetch_field(return_value, lock->fetch_dir(), lock->stage_dir(), identity)
    };

    if (!fetch_specs.empty()) {
      auto const to_download{ determine_downloads_needed(fetch_specs, identity) };
      execute_downloads(fetch_specs, to_download, identity, p->tui_section);

      bool const has_git_repos =
          std::any_of(fetch_specs.begin(), fetch_specs.end(), [](auto const &spec) {
            return std::holds_alternative<fetch_request_git>(spec.request);
          });

      if (has_git_repos) {
        tui::debug("fetch: git repos present — result not cacheable");
        should_mark_complete = false;
      }
    }
  } else {
    throw std::runtime_error("Fetch function for " + identity +
                             " must return nil, string, or table (got " +
                             sol::type_name(lua, return_value.get_type()) + ")");
  }

  // tmp_dir cleanup handled by lock destructor
  return should_mark_complete;
}

table_entry parse_table_entry(sol::table const &tbl, std::string const &context) {
  sol::optional<sol::object> source_obj = tbl["source"];
  if (!source_obj || !source_obj->valid() ||
      source_obj->get_type() == sol::type::lua_nil) {
    throw std::runtime_error(context + ": FETCH table missing 'source' key; got " +
                             sol_util_dump_table(tbl));
  }
  if (!source_obj->is<std::string>()) {
    throw std::runtime_error(context + ": FETCH table 'source' must be a string; got " +
                             sol::type_name(tbl.lua_state(), source_obj->get_type()));
  }
  std::string url{ source_obj->as<std::string>() };
  if (url.empty()) {
    throw std::runtime_error(context + ": FETCH table 'source' cannot be empty");
  }

  table_entry entry;
  entry.url = std::move(url);

  if (auto sha{ sol_util_get_optional<std::string>(tbl, "sha256", context) }) {
    entry.sha256 = std::move(*sha);
  }

  if (auto ref{ sol_util_get_optional<std::string>(tbl, "ref", context) }) {
    entry.ref = std::move(*ref);
  }

  if (auto pd{ sol_util_get_optional<std::string>(tbl, "post_data", context) }) {
    entry.post_data = std::move(*pd);
  }

  if (auto dst{ sol_util_get_optional<std::string>(tbl, "dest", context) }) {
    entry.dest = std::move(*dst);
  }

  return entry;
}

fetch_spec create_fetch_spec(std::string url,
                             std::string sha256,
                             std::optional<std::string> ref,
                             std::optional<std::string> post_data,
                             std::optional<std::string> dest_name,
                             std::filesystem::path const &fetch_dir,
                             std::filesystem::path const &stage_dir,
                             std::unordered_set<std::string> &basenames,
                             std::string const &context) {
  std::string basename;
  if (dest_name) {
    if (dest_name->empty()) {
      throw std::runtime_error("Empty dest for URL: " + url + " in " + context);
    }
    if (dest_name->find_first_of("/\\") != std::string::npos || *dest_name == ".." ||
        *dest_name == ".") {
      throw std::runtime_error("dest must be a plain filename (no path separators): " +
                               *dest_name + " in " + context);
    }
    basename = std::move(*dest_name);
  } else {
    basename = uri_extract_filename(url);
    if (basename.empty()) {
      throw std::runtime_error("Cannot extract filename from URL: " + url + " in " +
                               context);
    }
  }

  if (basenames.contains(basename)) {
    throw std::runtime_error("Fetch filename collision: " + basename + " in " + context);
  }
  basenames.insert(basename);

  auto const info{ uri_classify(url) };

  std::filesystem::path const dest{ (info.scheme == uri_scheme::GIT ||
                                     info.scheme == uri_scheme::GIT_HTTPS)
                                        ? stage_dir / basename
                                        : fetch_dir / basename };

  return { .request = url_to_fetch_request(url, dest, ref, post_data, context),
           .sha256 = std::move(sha256) };
}

// Parse the fetch field from Lua into a vector of fetch_specs.
std::vector<fetch_spec> parse_fetch_field(sol::object const &fetch_obj,
                                          std::filesystem::path const &fetch_dir,
                                          std::filesystem::path const &stage_dir,
                                          std::string const &key) {
  sol::type const fetch_type{ fetch_obj.get_type() };

  switch (fetch_type) {
    case sol::type::string: {
      std::string const url{ fetch_obj.as<std::string>() };
      std::string basename{ uri_extract_filename(url) };
      if (basename.empty()) {
        throw std::runtime_error("Cannot extract filename from URL: " + url + " in " +
                                 key);
      }
      std::filesystem::path dest{ fetch_dir / basename };

      return { { .request =
                     url_to_fetch_request(url, dest, std::nullopt, std::nullopt, key),
                 .sha256 = "" } };
    }

    case sol::type::table: {
      std::vector<fetch_spec> specs;
      std::unordered_set<std::string> basenames;

      sol::table tbl{ fetch_obj.as<sol::table>() };

      sol::object first_elem{ tbl[1] };
      sol::type first_elem_type{ first_elem.get_type() };

      auto process_table_entry{ [&](sol::table const &entry_tbl) {
        auto entry{ parse_table_entry(entry_tbl, key) };
        specs.push_back(create_fetch_spec(std::move(entry.url),
                                          std::move(entry.sha256),
                                          std::move(entry.ref),
                                          std::move(entry.post_data),
                                          std::move(entry.dest),
                                          fetch_dir,
                                          stage_dir,
                                          basenames,
                                          key));
      } };

      if (first_elem_type == sol::type::none || first_elem_type == sol::type::lua_nil) {
        process_table_entry(tbl);
      } else if (first_elem_type == sol::type::string) {
        size_t const len{ tbl.size() };
        for (size_t i = 1; i <= len; ++i) {
          sol::object elem{ tbl[i] };
          if (!elem.is<std::string>()) {
            throw std::runtime_error("Array element " + std::to_string(i) +
                                     " must be string in " + key);
          }
          std::string url{ elem.as<std::string>() };

          specs.push_back(create_fetch_spec(std::move(url),
                                            "",
                                            std::nullopt,
                                            std::nullopt,
                                            std::nullopt,
                                            fetch_dir,
                                            stage_dir,
                                            basenames,
                                            key));
        }
      } else if (first_elem_type == sol::type::table) {
        size_t const len{ tbl.size() };
        for (size_t i = 1; i <= len; ++i) {
          sol::object elem_obj{ tbl[i] };
          if (!elem_obj.is<sol::table>()) {
            throw std::runtime_error(
                "FETCH for " + key + ": array element [" + std::to_string(i) +
                "] must be a table (got " +
                std::string(sol::type_name(tbl.lua_state(), elem_obj.get_type())) +
                "); each element needs a 'source' key");
          }
          process_table_entry(elem_obj.as<sol::table>());
        }
      } else {
        throw std::runtime_error("Invalid fetch array element type in " + key);
      }

      return specs;
    }

    default:
      throw std::runtime_error("Fetch field must be string, table, or function in " + key);
  }
}

// Check cache and determine which files need downloading.
std::vector<size_t> determine_downloads_needed(std::vector<fetch_spec> const &specs,
                                               std::string const &identity) {
  std::vector<size_t> to_download;

  for (size_t i = 0; i < specs.size(); ++i) {
    auto const &spec{ specs[i] };
    auto const &dest{ get_destination(spec.request) };

    if (!std::filesystem::exists(dest)) {  // File doesn't exist: download
      to_download.push_back(i);
      continue;
    }

    if (spec.sha256.empty()) {  // No SHA256: always re-download (no cache trust)
      tui::debug("fetch: %s has no sha — always downloads",
                 dest.filename().string().c_str());
      std::filesystem::remove(dest);
      to_download.push_back(i);
      continue;
    }

    // File exists with SHA256 - verify cached version
    try {
      sha256_verify(spec.sha256, sha256(dest));
      tui::debug("fetch: %s cached (sha ok)", dest.filename().string().c_str());
      ENVY_TRACE(download_skipped,
                 identity,
                 .url = get_source(spec.request),
                 .reason = "cached, sha256 verified");
    } catch (std::exception const &e) {  // hash mismatch, delete and re-download
      tui::debug("fetch: %s sha mismatch — re-downloading",
                 dest.filename().string().c_str());
      std::filesystem::remove(dest);
      to_download.push_back(i);
    }
  }

  return to_download;
}

std::string fetch_item_label(fetch_request const &req) {
  return get_destination(req).filename().string();
}

// Execute downloads and verification for specs that need downloading.
void execute_downloads(std::vector<fetch_spec> const &specs,
                       std::vector<size_t> const &to_download_indices,
                       std::string const &key,
                       tui::section_handle section) {
  if (to_download_indices.empty()) { return; }

  tui::debug("fetch: downloading %zu file(s)", to_download_indices.size());

  std::vector<std::string> labels;
  labels.reserve(to_download_indices.size());
  for (auto idx : to_download_indices) {
    labels.push_back(fetch_item_label(specs[idx].request));
  }

  tui_actions::fetch_all_progress_tracker tracker{ section, key, labels, "fetch" };

  std::vector<fetch_request> requests;
  requests.reserve(to_download_indices.size());
  for (size_t req_slot{ 0 }; req_slot < to_download_indices.size(); ++req_slot) {
    auto idx = to_download_indices[req_slot];
    fetch_request req{ specs[idx].request };
    std::visit([&](auto &r) { r.progress = tracker.make_callback(req_slot); }, req);
    requests.push_back(std::move(req));
  }

  auto const results{ fetch(requests, key) };

  std::vector<bool> ok(results.size());
  for (size_t i{ 0 }; i < results.size(); ++i) {
    ok[i] = std::holds_alternative<fetch_result>(results[i]);
  }
  tracker.finish(ok);

  std::vector<std::string> errors;
  for (size_t i = 0; i < results.size(); ++i) {
    auto const spec_idx{ to_download_indices[i] };
    std::string url{ get_source(specs[spec_idx].request) };

    if (auto const *err{ std::get_if<std::string>(&results[i]) }) {
      errors.push_back(url + ": " + *err);
    } else {
      if (!specs[spec_idx].sha256.empty()) {
        try {
          auto const *result{ std::get_if<fetch_result>(&results[i]) };
          if (!result) { throw std::runtime_error("Unexpected result type"); }
          sha256_verify(specs[spec_idx].sha256,
                        tui_actions::sha256_tracked(result->resolved_destination,
                                                    section,
                                                    key,
                                                    "verifying"));
          tui::debug("fetch: %s sha256 verified",
                     result->resolved_destination.filename().string().c_str());
        } catch (std::exception const &e) {
          errors.push_back(get_source(specs[spec_idx].request) + ": " + e.what());
        }
      }
    }
  }

  if (!errors.empty()) {
    // Update TUI to show failure before throwing
    std::string status_text{ "fetch failed: " + errors.front() };
    if (errors.size() > 1) {
      status_text += " (+" + std::to_string(errors.size() - 1) + " more)";
    }
    tui::section_set_content(
        section,
        tui::section_frame{ .label = "[" + key + "]",
                            .content = tui::static_text_data{ .text = status_text } });

    std::ostringstream oss;
    oss << "Fetch failed for " << key << ":\n";
    for (auto const &err : errors) { oss << "  " << err << "\n"; }
    throw std::runtime_error(oss.str());
  }
}

// fetch = "source" or fetch = {source="..."} or fetch = {{...}}
// Returns true if fetch should be marked complete (cacheable), false otherwise
bool run_declarative_fetch(sol::object const &fetch_obj,
                           cache::scoped_entry_lock *lock,
                           std::string const &identity,
                           pkg *p) {
  // Ensure stage_dir exists (needed for git repos that clone directly there)
  std::error_code ec;
  std::filesystem::create_directories(lock->stage_dir(), ec);
  if (ec) {
    throw std::runtime_error("Failed to create stage directory: " + ec.message());
  }

  auto const fetch_specs{
    parse_fetch_field(fetch_obj, lock->fetch_dir(), lock->stage_dir(), identity)
  };
  if (fetch_specs.empty()) { return true; }  // No specs = cacheable (nothing to do)

  execute_downloads(fetch_specs,
                    determine_downloads_needed(fetch_specs, identity),
                    identity,
                    p->tui_section);

  // Check if git repos - if so, don't mark fetch complete (git clones are not cacheable)
  bool const has_git_repos{ std::any_of(fetch_specs.begin(),
                                        fetch_specs.end(),
                                        [](auto const &spec) {
                                          return std::holds_alternative<fetch_request_git>(
                                              spec.request);
                                        }) };

  if (has_git_repos) {
    tui::debug(
        "phase fetch: skipping fetch completion marker (git repos are not cacheable)");
    return false;
  }

  return true;  // Mark complete
}

}  // namespace

void run_fetch_phase(pkg *p, engine &eng) {
  phase_trace_scope const phase_scope{ p->cfg->identity,
                                       pkg_phase::pkg_fetch,
                                       std::chrono::steady_clock::now() };

  cache::scoped_entry_lock *lock = p->lock.get();
  if (!lock) { return; }  // cache hit — nothing to fetch

  if (lock->is_fetch_complete()) { return; }

  std::string const &identity{ p->cfg->identity };

  auto const lua_acc{ p->lua.lock() };
  sol::state_view lua_view{ *lua_acc };
  sol::object fetch_obj{ lua_view["FETCH"] };

  bool should_mark_complete{ true };

  if (!fetch_obj.valid()) {
    return;  // no FETCH field
  } else if (fetch_obj.is<sol::protected_function>()) {
    should_mark_complete = run_programmatic_fetch(fetch_obj.as<sol::protected_function>(),
                                                  lock,
                                                  identity,
                                                  eng,
                                                  p);
  } else if (fetch_obj.is<std::string>() || fetch_obj.is<sol::table>()) {
    should_mark_complete = run_declarative_fetch(fetch_obj, lock, identity, p);
  } else {
    throw std::runtime_error("Fetch field must be nil, string, table, or function in " +
                             identity);
  }

  if (should_mark_complete) { lock->mark_fetch_complete(); }
}

}  // namespace envy
