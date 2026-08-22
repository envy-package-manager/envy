#include "phase_spec_fetch.h"

#include "bundle.h"
#include "engine.h"
#include "extract.h"
#include "fetch.h"
#include "lua_ctx/lua_envy_options.h"
#include "lua_ctx/lua_phase_context.h"
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "manifest.h"
#include "pkg.h"
#include "sha256.h"
#include "sol_util.h"
#include "trace.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include <chrono>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace envy {

namespace {

bool resolve_user_managed(sol::state_view lua, std::string const &identity) {
  sol::object obj{ lua["USER_MANAGED"] };
  if (!obj.valid() || obj.get_type() == sol::type::lua_nil) { return false; }

  if (obj.get_type() == sol::type::boolean) { return obj.as<bool>(); }

  if (obj.is<sol::protected_function>()) {
    if (sol::protected_function_result result{ obj.as<sol::protected_function>()() };
        !result.valid()) {
      sol::error err = result;
      throw std::runtime_error("USER_MANAGED function failed for " + identity + ": " +
                               std::string{ err.what() });
    } else {
      sol::object ret{ result };
      if (ret.get_type() != sol::type::boolean) {
        throw std::runtime_error(
            std::string{ "USER_MANAGED function must return a boolean (got " } +
            sol::type_name(lua.lua_state(), ret.get_type()) + ") for " + identity);
      }
      return ret.as<bool>();
    }
  }

  throw std::runtime_error(
      std::string{ "USER_MANAGED must be a boolean or function returning a boolean "
                   "(got " } +
      sol::type_name(lua.lua_state(), obj.get_type()) + ") for " + identity);
}

void validate_phases(sol::state_view lua,
                     std::string const &identity,
                     bool user_managed,
                     bool has_setup) {
  bool const has_fetch{ lua["FETCH"].is<sol::protected_function>() ||
                        lua["FETCH"].is<std::string>() || lua["FETCH"].is<sol::table>() };
  bool const has_install{ lua["INSTALL"].is<sol::protected_function>() ||
                          lua["INSTALL"].is<std::string>() };

  if (sol::object check_obj{ lua["CHECK"] };
      check_obj.valid() && check_obj.get_type() != sol::type::lua_nil) {
    throw std::runtime_error("Spec " + identity +
                             " defines top-level CHECK; CHECK/INSTALL pairs belong in "
                             "SETUP entries (SETUP = { name = { CHECK=..., "
                             "INSTALL=... } })");
  }

  if (user_managed) {
    if (!has_setup) {
      throw std::runtime_error("User-managed spec must define at least one SETUP pair: " +
                               identity);
    }
    std::pair<char const *, bool> const forbidden[]{
      { "FETCH", has_fetch },
      { "STAGE",
        lua["STAGE"].is<sol::protected_function>() || lua["STAGE"].is<std::string>() ||
            lua["STAGE"].is<sol::table>() },
      { "BUILD",
        lua["BUILD"].is<sol::protected_function>() || lua["BUILD"].is<std::string>() },
      { "INSTALL", has_install },
    };
    for (auto const &[verb, present] : forbidden) {
      if (present) {
        throw std::runtime_error(
            "Spec " + identity + " is user-managed (USER_MANAGED=true) but declares " +
            verb +
            ". User-managed packages define only SETUP pairs; remove the phase verb "
            "or set USER_MANAGED=false.");
      }
    }
  } else {
    if (!has_fetch) { throw std::runtime_error("Spec must define 'FETCH': " + identity); }
  }
}

// Parse and validate the SETUP global:
// { name = { CHECK, INSTALL, PLATFORMS?, DEPENDS? } }.
// DEPENDS names sibling pairs that must complete first; validated here for
// unknown targets and cycles so downstream selection/scheduling can trust it.
std::map<std::string, pkg::setup_pair_decl> parse_setup_table(
    sol::state_view lua,
    std::string const &identity) {
  std::map<std::string, pkg::setup_pair_decl> pairs;

  sol::object setup_obj{ lua["SETUP"] };
  if (!setup_obj.valid() || setup_obj.get_type() == sol::type::lua_nil) { return pairs; }
  if (setup_obj.get_type() != sol::type::table) {
    throw std::runtime_error("SETUP must be a table in spec '" + identity + "'");
  }

  auto const is_verb{ [](sol::object const &o) {
    return o.is<sol::protected_function>() || o.is<std::string>();
  } };

  auto const parse_string_array{
    [&identity](sol::object const &obj, std::string const &name, char const *field) {
      std::vector<std::string> values;
      if (!obj.valid() || obj.get_type() == sol::type::lua_nil) { return values; }
      if (obj.get_type() != sol::type::table) {
        throw std::runtime_error("SETUP entry '" + name + "' " + field +
                                 " must be a table in spec '" + identity + "'");
      }
      sol::table t{ obj.as<sol::table>() };
      for (size_t i{ 1 }; i <= t.size(); ++i) {
        sol::object elem{ t[i] };
        if (!elem.is<std::string>() || elem.as<std::string>().empty()) {
          throw std::runtime_error("SETUP entry '" + name + "' " + field +
                                   " entries must be non-empty strings in spec '" +
                                   identity + "'");
        }
        values.push_back(elem.as<std::string>());
      }
      return values;
    }
  };

  sol::table setup_table{ setup_obj.as<sol::table>() };
  for (auto const &[key, value] : setup_table) {
    sol::object const key_obj(key);
    sol::object const val_obj(value);

    if (!key_obj.is<std::string>() || key_obj.as<std::string>().empty()) {
      throw std::runtime_error("SETUP keys must be non-empty strings in spec '" +
                               identity + "'");
    }
    std::string const name{ key_obj.as<std::string>() };

    // Pair names become engine-node key suffixes ("<canonical>#setup:<name>");
    // restrict the charset so they can never collide with real identities.
    if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "abcdefghijklmnopqrstuvwxyz0123456789_.-") !=
        std::string::npos) {
      throw std::runtime_error("SETUP pair name '" + name + "' in spec '" + identity +
                               "' has invalid characters (allowed: alphanumerics, "
                               "'_', '.', '-')");
    }

    if (!val_obj.is<sol::table>()) {
      throw std::runtime_error("SETUP entry '" + name + "' must be a table in spec '" +
                               identity + "'");
    }
    sol::table pair{ val_obj.as<sol::table>() };

    for (auto const &[pair_key, _] : pair) {  // Reject unknown pair fields
      sol::object const pair_key_obj(pair_key);
      std::string const k{ pair_key_obj.is<std::string>() ? pair_key_obj.as<std::string>()
                                                          : std::string{} };
      if (k != "CHECK" && k != "INSTALL" && k != "PLATFORMS" && k != "DEPENDS") {
        throw std::runtime_error("SETUP entry '" + name + "' has unknown field '" + k +
                                 "' in spec '" + identity +
                                 "' (valid: CHECK, INSTALL, PLATFORMS, DEPENDS)");
      }
    }

    if (!is_verb(pair["CHECK"])) {
      throw std::runtime_error("SETUP entry '" + name +
                               "' must define CHECK (function or string) in spec '" +
                               identity + "'");
    }
    if (!is_verb(pair["INSTALL"])) {
      throw std::runtime_error("SETUP entry '" + name +
                               "' must define INSTALL (function or string) in spec '" +
                               identity + "'");
    }

    pairs.emplace(
        name,
        pkg::setup_pair_decl{
            .platforms = parse_string_array(pair["PLATFORMS"], name, "PLATFORMS"),
            .depends = parse_string_array(pair["DEPENDS"], name, "DEPENDS") });
  }

  for (auto const &[name, decl] : pairs) {  // DEPENDS targets must exist
    for (auto const &dep : decl.depends) {
      if (!pairs.contains(dep)) {
        throw std::runtime_error("SETUP entry '" + name + "' DEPENDS on unknown pair '" +
                                 dep + "' in spec '" + identity + "'");
      }
    }
  }

  {  // Cycle detection: iterative DFS, white/gray/black, deterministic (sorted map)
    std::unordered_map<std::string, int> color;
    for (auto const &[root, _] : pairs) {
      if (color[root]) { continue; }
      std::vector<std::pair<std::string, size_t>> stack{ { root, 0 } };
      color[root] = 1;
      while (!stack.empty()) {
        std::string const cur{ stack.back().first };
        auto const &deps{ pairs.at(cur).depends };
        if (stack.back().second == deps.size()) {
          color[cur] = 2;
          stack.pop_back();
          continue;
        }
        std::string const &next{ deps[stack.back().second++] };
        if (color[next] == 1) {  // gray = on stack = cycle; report the loop path
          std::ostringstream oss;
          oss << "SETUP DEPENDS cycle in spec '" << identity << "': ";
          bool started{ false };  // don't overload path.empty() as loop state
          for (auto const &[frame, _] : stack) {
            if (!started && frame != next) { continue; }
            started = true;
            oss << frame << " -> ";
          }
          oss << next;
          throw std::runtime_error(oss.str());
        }
        if (color[next] == 0) {
          color[next] = 1;
          stack.emplace_back(next, 0);
        }
      }
    }
  }

  return pairs;
}

sol_state_ptr create_lua_state() {
  auto lua{ sol_util_make_lua_state() };
  lua_envy_install(*lua);
  return lua;
}

int load_spec_script(sol::state &lua,
                     std::filesystem::path const &spec_path,
                     std::string const &identity) {
  auto const content{ util_load_file(spec_path) };
  auto const meta{ parse_envy_meta(
      { reinterpret_cast<char const *>(content.data()), content.size() }) };

  std::string const script{ reinterpret_cast<char const *>(content.data()),
                            content.size() };
  std::string const chunk_name{ "@" + spec_path.string() };
  sol::protected_function_result result{
    lua.safe_script(script, sol::script_pass_on_error, chunk_name)
  };
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error("Failed to load spec: " + identity + ": " + err.what());
  }

  return meta.schema;
}

// Canonical description of where a spec's or bundle's bytes come from. This is the
// only input to its cache entry key besides identity, so every field a fetch
// actually reads belongs here: change the URL, the git ref, or the path and you get
// a different entry rather than yesterday's content under today's declaration.
//
// A custom fetch function is the exception -- a Lua closure has no fingerprint, so
// its entries key on the file that declares it. Editing the function body in place
// still reuses the entry; move it, or bump the identity, to force a refetch.
std::string source_key(pkg_cfg::remote_source const &r) {
  return "remote\n" + r.url + "\n" + r.sha256;
}

std::string source_key(pkg_cfg::git_source const &g) {
  return "git\n" + g.url + "\n" + g.ref;
}

std::string source_key(pkg_cfg::local_source const &l) {
  return "local\n" + l.file_path.generic_string();
}

std::string custom_fetch_source_key(pkg_cfg const &cfg) {
  return "fetch\n" + cfg.declaring_file_path.generic_string() + "\n" + cfg.identity;
}

// Every download this phase makes — a spec file, a git spec repo, a bundle payload —
// goes through here, so it draws with the same tracker the package fetch phase uses
// and lands on the requesting package's row. Throws on failure; `what` names the
// source kind in the message.
void fetch_with_progress(fetch_request req,
                         pkg const *p,
                         std::string const &url,
                         char const *what) {
  std::string const &identity{ p->cfg->identity };
  tui_actions::fetch_all_progress_tracker tracker{ p->tui_section,
                                                   identity,
                                                   { uri_extract_filename(url) },
                                                   "fetch" };
  std::visit([&](auto &r) { r.progress = tracker.make_callback(0); }, req);

  auto const results{ fetch({ std::move(req) }, identity) };
  if (results.empty() || std::holds_alternative<std::string>(results[0])) {
    throw std::runtime_error(
        std::string{ "Failed to fetch " } + what + ": " +
        (results.empty() ? "no results" : std::get<std::string>(results[0])));
  }
}

// A fetched spec together with the cache entry lock that still guards it, if the
// fetch was a cache miss. The lock is deliberately *not* released here: an entry
// carrying `envy-complete` is trusted forever after, so it may only be finalized
// once the fetched bytes are proven to be a loadable spec of the expected
// identity. run_spec_fetch_phase owns that decision; see the comment there.
struct spec_fetch_result {
  std::filesystem::path spec_path;
  cache::scoped_entry_lock::ptr_t lock;
};

spec_fetch_result fetch_local_source(pkg_cfg const &cfg) {
  auto const *local_src{ std::get_if<pkg_cfg::local_source>(&cfg.source) };
  return { local_src->file_path, nullptr };  // in-situ: never enters the cache
}

spec_fetch_result fetch_remote_source(pkg_cfg const &cfg, pkg *p) {
  auto const *remote_src{ std::get_if<pkg_cfg::remote_source>(&cfg.source) };
  auto cache_result{ p->cache_ptr->ensure_spec(cfg.identity, source_key(*remote_src)) };

  if (cache_result.lock) {
    tui::debug("spec: source %s", remote_src->url.c_str());
    std::filesystem::path fetch_dest{ cache_result.lock->install_dir() / "spec.lua" };

    fetch_with_progress(fetch_request_from_url(remote_src->url, fetch_dest),
                        p,
                        remote_src->url,
                        "spec");

    if (!remote_src->sha256.empty()) {
      sha256_verify(remote_src->sha256, sha256(fetch_dest));
    }
  }

  return { cache_result.pkg_path / "spec.lua", std::move(cache_result.lock) };
}

spec_fetch_result fetch_git_source(pkg_cfg const &cfg, pkg *p) {
  auto const *git_src{ std::get_if<pkg_cfg::git_source>(&cfg.source) };
  auto cache_result{ p->cache_ptr->ensure_spec(cfg.identity, source_key(*git_src)) };

  if (cache_result.lock) {
    tui::debug("spec: from git %s @ %s", git_src->url.c_str(), git_src->ref.c_str());

    std::filesystem::path install_dir{ cache_result.lock->install_dir() };
    auto const info{ uri_classify(git_src->url) };
    fetch_with_progress(fetch_request_git{ .source = git_src->url,
                                           .destination = install_dir,
                                           .ref = git_src->ref,
                                           .scheme = info.scheme },
                        p,
                        git_src->url,
                        "git spec");
  }

  return { cache_result.pkg_path / "spec.lua", std::move(cache_result.lock) };
}

// The bundle is materialized by its own BUNDLE_ONLY package, which this spec lists
// as a source dependency — so by the time spec_fetch runs the bundle is registered
// and all that is left is resolving the spec's path inside it.
spec_fetch_result resolve_spec_from_bundle(pkg_cfg const &cfg, engine &eng) {
  auto const *bundle_src{ std::get_if<pkg_cfg::bundle_source>(&cfg.source) };
  std::string const &bundle_id{ bundle_src->bundle_identity };

  bundle *b{ eng.find_bundle(bundle_id) };
  if (!b) {
    throw std::runtime_error("Bundle '" + bundle_id +
                             "' was not materialized before spec '" + cfg.identity +
                             "' (missing bundle source dependency)");
  }

  std::filesystem::path spec_path{ b->resolve_spec_path(cfg.identity) };
  if (spec_path.empty()) {
    throw std::runtime_error("Spec '" + cfg.identity + "' not found in bundle '" +
                             bundle_id + "'");
  }

  return { std::move(spec_path), nullptr };  // the bundle owns the cache entry
}

spec_fetch_result fetch_custom_function(pkg_cfg const &cfg, pkg *p, engine &eng) {
  if (!cfg.parent) {
    throw std::runtime_error("Custom fetch function spec has no parent: " + cfg.identity);
  }

  pkg *parent{ eng.find_exact(pkg_key(*cfg.parent)) };
  if (!parent) {
    throw std::runtime_error("Custom fetch function spec parent not found: " +
                             cfg.identity);
  }

  auto cache_result{ p->cache_ptr->ensure_spec(cfg.identity,
                                               custom_fetch_source_key(cfg)) };

  if (cache_result.lock) {
    tui::debug("spec: custom fetch function");

    // Set up paths for custom fetch function
    std::filesystem::path const tmp_dir{ cache_result.lock->work_dir() / "tmp" };
    std::filesystem::create_directories(tmp_dir);

    auto const parent_acc{ parent->lua.lock() };

    if (!parent_acc) {
      throw std::runtime_error("Custom fetch function spec has no parent Lua state: " +
                               cfg.identity);
    }

    {
      sol::state_view parent_lua_view{ *parent_acc };

      auto fetch_func_opt{ pkg_cfg::get_source_fetch(parent_lua_view, cfg.identity) };
      if (!fetch_func_opt) {
        throw std::runtime_error("Failed to lookup fetch function for: " + cfg.identity);
      }

      sol::object options_obj{ parent_lua_view.registry()[ENVY_OPTIONS_RIDX] };

      // Set up phase context with lock so envy.commit_fetch can access paths.
      // The inline source.fetch runs in parent's Lua state, so we pass the lock
      // explicitly rather than through parent->lock.
      phase_context_guard ctx_guard{ &eng,
                                     parent,
                                     parent_lua_view.lua_state(),
                                     tmp_dir,
                                     cache_result.lock.get() };

      sol::protected_function_result fetch_result{ (
          *fetch_func_opt)(util_normalized_path(tmp_dir), options_obj) };

      if (!fetch_result.valid()) {
        sol::error err = fetch_result;
        throw std::runtime_error("Fetch function failed for " + cfg.identity + ": " +
                                 err.what());
      }
    }

    // Custom fetch creates spec.lua in fetch_dir via envy.commit_fetch.
    // The lock destructor will clean up fetch_dir, so move spec.lua to install_dir.
    std::filesystem::path const fetch_dir{ cache_result.lock->fetch_dir() };
    std::filesystem::path const install_dir{ cache_result.lock->install_dir() };
    std::filesystem::path const spec_src{ fetch_dir / "spec.lua" };
    std::filesystem::path const spec_dst{ install_dir / "spec.lua" };

    if (!std::filesystem::exists(spec_src)) {
      throw std::runtime_error("Custom fetch did not create spec.lua for: " +
                               cfg.identity);
    }

    std::filesystem::rename(spec_src, spec_dst);

    return { spec_dst, std::move(cache_result.lock) };
  }

  // Cache was already complete - spec.lua should exist in pkg_path
  std::filesystem::path spec_path{ cache_result.pkg_path / "spec.lua" };
  if (!std::filesystem::exists(spec_path)) {
    throw std::runtime_error("Custom fetch did not create spec.lua for: " + cfg.identity);
  }

  return { std::move(spec_path), nullptr };
}

std::unordered_map<std::string, product_entry> parse_products_table(pkg_cfg const &cfg,
                                                                    sol::state &lua,
                                                                    pkg *p) {
  std::unordered_map<std::string, product_entry> parsed_products;
  sol::object products_obj{ lua["PRODUCTS"] };
  std::string const &id{ cfg.identity };

  if (!products_obj.valid()) { return parsed_products; }

  sol::table products_table;

  // Handle programmatic products: function that takes options, returns table
  if (products_obj.get_type() == sol::type::function) {
    sol::function products_fn{ products_obj.as<sol::function>() };

    // Deserialize options from cfg to pass to products function
    std::string const opts_str{ "return " + cfg.serialized_options };
    auto opts_result{ lua.safe_script(opts_str, sol::script_pass_on_error) };
    if (!opts_result.valid()) {
      sol::error err = opts_result;
      throw std::runtime_error("Failed to deserialize options for PRODUCTS function: " +
                               std::string(err.what()));
    }
    sol::object options{ opts_result.get<sol::object>() };

    // Call products(options) with enriched error handling
    sol::protected_function_result result{ call_lua_function_with_enriched_errors(
        p,
        "PRODUCTS",
        [&]() { return sol::protected_function_result{ products_fn(options) }; }) };

    sol::object result_obj{ result };
    if (result_obj.get_type() != sol::type::table) {
      throw std::runtime_error("PRODUCTS function must return table in spec '" + id + "'");
    }
    products_table = result_obj.as<sol::table>();
  } else if (products_obj.get_type() == sol::type::table) {
    products_table = products_obj.as<sol::table>();
  } else {
    throw std::runtime_error("PRODUCTS must be table or function in spec '" + id + "'");
  }
  // User-managed product values name host state (not cache paths); skip path safety.
  bool const user_managed{ p->type == pkg_type::USER_MANAGED };

  for (auto const &[key, value] : products_table) {
    sol::object key_obj(key);
    sol::object val_obj(value);

    if (!key_obj.is<std::string>()) {
      throw std::runtime_error("PRODUCTS key must be string in spec '" + id + "'");
    }

    std::string key_str{ key_obj.as<std::string>() };
    std::string val_str;
    bool script{ true };
    std::vector<std::string> entry_platforms;

    if (val_obj.is<std::string>()) {
      val_str = val_obj.as<std::string>();
    } else if (val_obj.is<sol::table>()) {
      sol::table t{ val_obj.as<sol::table>() };
      sol::object val_field{ t["value"] };
      if (!val_field.is<std::string>()) {
        throw std::runtime_error("PRODUCTS table entry '" + key_str +
                                 "' must have string 'value' field in spec '" + id + "'");
      }
      val_str = val_field.as<std::string>();
      sol::object script_field{ t["script"] };
      if (script_field.valid() && script_field.is<bool>()) {
        script = script_field.as<bool>();
      }
      sol::object platforms_field{ t["platforms"] };
      if (platforms_field.valid() && platforms_field.get_type() != sol::type::lua_nil) {
        if (platforms_field.get_type() != sol::type::table) {
          throw std::runtime_error("PRODUCTS entry '" + key_str +
                                   "' platforms must be a table in spec '" + id + "'");
        }
        sol::table plat_table{ platforms_field.as<sol::table>() };
        for (size_t i{ 1 }; i <= plat_table.size(); ++i) {
          sol::object elem{ plat_table[i] };
          if (!elem.is<std::string>()) {
            throw std::runtime_error("PRODUCTS entry '" + key_str +
                                     "' platforms entries must be strings in spec '" + id +
                                     "'");
          }
          std::string plat_str{ elem.as<std::string>() };
          if (plat_str.empty()) {
            throw std::runtime_error("PRODUCTS entry '" + key_str +
                                     "' platforms entry cannot be empty in spec '" + id +
                                     "'");
          }
          entry_platforms.push_back(std::move(plat_str));
        }
      }
    } else {
      throw std::runtime_error("PRODUCTS value must be string or table in spec '" + id +
                               "'");
    }

    if (key_str.empty()) {
      throw std::runtime_error("PRODUCTS key cannot be empty in spec '" + id + "'");
    }

    for (char c : key_str) {
      bool const dangerous{ c < 0x21 || c > 0x7e || c == '"' || c == '\'' || c == '$' ||
                            c == '`' || c == '%' || c == '\\' || c == '!' };
      if (dangerous) {
        throw std::runtime_error("PRODUCTS key '" + key_str +
                                 "' contains shell-unsafe character in spec '" + id + "'");
      }
    }

    if (val_str.empty()) {
      throw std::runtime_error("PRODUCTS value cannot be empty in spec '" + id + "'");
    }

    if (!user_managed) {  // Validate path safety for cached packages
      std::filesystem::path product_path{ val_str };

      if (product_path.is_absolute() || (!val_str.empty() && val_str[0] == '/')) {
        throw std::runtime_error("PRODUCTS value '" + val_str +
                                 "' cannot be absolute path in spec '" + id + "'");
      }

      for (auto const &component : product_path) {  // Check for path traversal components
        if (component == "..") {
          throw std::runtime_error("PRODUCTS value '" + val_str +
                                   "' cannot contain path traversal (..) in spec '" + id +
                                   "'");
        }
      }
    }

    parsed_products[std::move(key_str)] =
        product_entry{ std::move(val_str), script, std::move(entry_platforms) };
  }

  return parsed_products;
}

using bundle_alias_map = std::unordered_map<std::string, pkg_cfg::bundle_source>;
using bundle_pkg_map = std::unordered_map<std::string, pkg_cfg *>;

// Parse a pure bundle dependency: {bundle = "identity", source = "...", ref = "..."}
// Returns bundle_source if this is a pure bundle dep, nullopt otherwise
std::optional<pkg_cfg::bundle_source> try_parse_pure_bundle_dep(
    sol::table const &table,
    std::filesystem::path const &spec_path) {
  sol::object bundle_obj{ table["bundle"] };
  sol::object source_obj{ table["source"] };

  // Pure bundle dep requires both bundle and source fields
  if (!bundle_obj.valid() || bundle_obj.get_type() == sol::type::lua_nil) {
    return std::nullopt;
  }
  if (!source_obj.valid() || source_obj.get_type() == sol::type::lua_nil) {
    return std::nullopt;  // No source = not a pure bundle dep (might be spec-from-bundle)
  }

  // bundle field must be string (the bundle identity)
  if (!bundle_obj.is<std::string>()) {
    throw std::runtime_error(
        "Pure bundle dependency 'bundle' field must be string (identity)");
  }
  std::string bundle_identity{ bundle_obj.as<std::string>() };
  if (!util_is_safe_path_component(bundle_identity)) {
    throw std::runtime_error(
        "Pure bundle dependency 'bundle' field is not a valid "
        "identity: '" +
        bundle_identity + "'");
  }

  // Parse source: string (URL/path) or table { fetch = function, dependencies = {} }
  if (source_obj.is<sol::table>()) {  // Table: custom fetch with optional dependencies
    sol::table source_table{ source_obj.as<sol::table>() };
    sol::object fetch_obj{ source_table["fetch"] };
    if (!fetch_obj.valid() || !fetch_obj.is<sol::function>()) {
      throw std::runtime_error("Bundle source table requires 'fetch' function");
    }

    pkg_cfg::custom_fetch_source custom;

    sol::object deps_obj{ source_table["dependencies"] };
    if (deps_obj.valid() && deps_obj.get_type() != sol::type::lua_nil) {
      if (!deps_obj.is<sol::table>()) {
        throw std::runtime_error("Bundle source.dependencies must be array (table)");
      }
      sol::table deps_table{ deps_obj.as<sol::table>() };
      for (size_t i{ 1 }, n{ deps_table.size() }; i <= n; ++i) {
        custom.dependencies.push_back(
            pkg_cfg::parse_fetch_dependency(deps_table[i], spec_path));
      }
    }

    return pkg_cfg::bundle_source{ .bundle_identity = std::move(bundle_identity),
                                   .fetch_source = std::move(custom) };
  }

  if (!source_obj.is<std::string>()) {
    throw std::runtime_error(
        "Pure bundle dependency 'source' field must be string or table");
  }
  std::string const source_uri{ source_obj.as<std::string>() };
  auto const info{ uri_classify(source_uri) };

  if (info.scheme == uri_scheme::GIT || info.scheme == uri_scheme::GIT_HTTPS) {
    auto ref_opt{ sol_util_get_optional<std::string>(table, "ref", "Bundle dependency") };
    if (!ref_opt.has_value() || ref_opt->empty()) {
      throw std::runtime_error("Bundle dependency with git source requires 'ref' field");
    }
    return pkg_cfg::bundle_source{ .bundle_identity = std::move(bundle_identity),
                                   .fetch_source =
                                       pkg_cfg::git_source{ .url = info.canonical,
                                                            .ref = std::move(*ref_opt) } };
  }

  if (info.scheme == uri_scheme::LOCAL_FILE_RELATIVE ||
      info.scheme == uri_scheme::LOCAL_FILE_ABSOLUTE) {
    std::filesystem::path resolved{ info.canonical };
    if (info.scheme == uri_scheme::LOCAL_FILE_RELATIVE) {
      resolved = (spec_path.parent_path() / resolved).lexically_normal();
    }
    return pkg_cfg::bundle_source{ .bundle_identity = std::move(bundle_identity),
                                   .fetch_source = pkg_cfg::local_source{
                                       .file_path = std::move(resolved) } };
  }

  // Remote source
  auto sha256_opt{
    sol_util_get_optional<std::string>(table, "sha256", "Bundle dependency")
  };
  return pkg_cfg::bundle_source{ .bundle_identity = std::move(bundle_identity),
                                 .fetch_source = pkg_cfg::remote_source{
                                     .url = info.canonical,
                                     .sha256 = sha256_opt.value_or("") } };
}

// Parse a dependency entry that has a bundle field (spec-from-bundle)
pkg_cfg *parse_spec_from_bundle_dep(sol::table const &table,
                                    std::filesystem::path const &spec_path,
                                    bundle_alias_map const &aliases,
                                    bundle_alias_map const &declared_bundles,
                                    pkg_cfg const *declaring_spec,
                                    bundle_pkg_map &bundle_pkgs) {
  // Get spec identity (required for spec-from-bundle)
  std::string const spec_identity{ [&] {
    auto opt{ sol_util_get_optional<std::string>(table, "spec", "Dependency") };
    if (!opt.has_value() || opt->empty()) {
      throw std::runtime_error(
          "Dependency with 'bundle' field (without 'source') requires 'spec' field");
    }
    return std::move(*opt);
  }() };

  sol::object bundle_obj{ table["bundle"] };

  // Resolve bundle source
  pkg_cfg::bundle_source const bundle_src{ [&]() -> pkg_cfg::bundle_source {
    if (bundle_obj.is<std::string>()) {
      std::string const &ref{ bundle_obj.as<std::string>() };

      // First check BUNDLES aliases
      if (auto it{ aliases.find(ref) }; it != aliases.end()) { return it->second; }

      // Then check declared bundle dependencies (by identity)
      if (auto it{ declared_bundles.find(ref) }; it != declared_bundles.end()) {
        return it->second;
      }

      throw std::runtime_error(
          "Bundle reference '" + ref +
          "' not found in BUNDLES table or prior DEPENDENCIES for spec '" + spec_identity +
          "'");
    }

    if (bundle_obj.is<sol::table>()) {
      return bundle::parse_inline(bundle_obj.as<sol::table>(), spec_path);
    }

    throw std::runtime_error("Dependency 'bundle' field must be string or table");
  }() };

  // Parse optional fields
  std::string serialized_options{ "{}" };
  sol::object options_obj{ table["options"] };
  if (options_obj.valid() && options_obj.get_type() == sol::type::table) {
    serialized_options = pkg_cfg::serialize_option_table(options_obj);
  }

  std::optional<pkg_phase> needed_by;
  auto needed_by_str{
    sol_util_get_optional<std::string>(table, "needed_by", "Dependency")
  };
  if (needed_by_str.has_value()) {
    needed_by = pkg_phase_parse_needed_by(*needed_by_str, "Dependency");
  }

  std::optional<std::string> product{
    sol_util_get_optional<std::string>(table, "product", "Dependency")
  };

  std::string const bundle_identity{ bundle_src.bundle_identity };

  // The bundle is its own package; depending on it blocks this spec's spec_fetch
  // until the bundle is materialized and registered.
  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(
      spec_identity,
      pkg_cfg::bundle_source{ bundle_src },
      std::move(serialized_options),
      needed_by,
      nullptr,
      nullptr,
      std::vector<pkg_cfg *>{
          bundle::ensure_pkg_cfg(bundle_src, spec_path, declaring_spec, bundle_pkgs) },
      std::move(product),
      spec_path) };

  cfg->bundle_identity = bundle_identity;
  return cfg;
}

std::vector<pkg_cfg *> parse_dependencies_table(sol::state &lua,
                                                std::filesystem::path const &spec_path,
                                                pkg_cfg const &cfg) {
  std::vector<pkg_cfg *> parsed_deps;

  // Parse BUNDLES table (if present) for alias resolution
  bundle_alias_map const aliases{ bundle::parse_aliases(lua["BUNDLES"], spec_path) };

  // Track declared bundle dependencies (pure bundle deps) for identity-based resolution
  bundle_alias_map declared_bundles;

  // Bundle identity → its BUNDLE_ONLY package cfg, so several specs pulled from one
  // bundle share a single bundle package (and therefore a single row).
  bundle_pkg_map bundle_pkgs;

  sol::object deps_obj{ lua["DEPENDENCIES"] };
  if (!deps_obj.valid() || deps_obj.get_type() != sol::type::table) { return parsed_deps; }

  sol::table deps_table{ deps_obj.as<sol::table>() };
  for (size_t i{ 1 }; i <= deps_table.size(); ++i) {
    sol::object entry{ deps_table[i] };

    if (!entry.is<sol::table>()) {
      // Non-table entries use standard parsing
      pkg_cfg *dep_cfg{ pkg_cfg::parse(entry, spec_path, true) };
      if (!cfg.identity.starts_with("local.") && dep_cfg->identity.starts_with("local.")) {
        throw std::runtime_error("non-local spec '" + cfg.identity +
                                 "' cannot depend on local spec '" + dep_cfg->identity +
                                 "'");
      }
      parsed_deps.push_back(dep_cfg);
      continue;
    }

    sol::table table{ entry.as<sol::table>() };

    // Optional 'setup' selection: spec authors may demand host-state pairs from
    // their dependencies. Merged (union) with all other referrers' selections.
    auto const dep_setup{ [&]() -> std::optional<std::vector<std::string>> {
      sol::object setup_obj{ table["setup"] };
      if (!setup_obj.valid() || setup_obj.get_type() == sol::type::lua_nil) {
        return std::nullopt;
      }
      if (setup_obj.get_type() != sol::type::table) {
        throw std::runtime_error(
            "Dependency 'setup' field must be a table of pair "
            "names (spec '" +
            cfg.identity + "')");
      }
      std::vector<std::string> names;
      sol::table t{ setup_obj.as<sol::table>() };
      for (size_t j{ 1 }; j <= t.size(); ++j) {
        sol::object elem{ t[j] };
        if (!elem.is<std::string>() || elem.as<std::string>().empty()) {
          throw std::runtime_error(
              "Dependency 'setup' entries must be non-empty "
              "strings (spec '" +
              cfg.identity + "')");
        }
        names.push_back(elem.as<std::string>());
      }
      return names;
    }() };

    // Check for pure bundle dependency: {bundle = "id", source = "..."}
    if (auto pure_bundle{ try_parse_pure_bundle_dep(table, spec_path) }) {
      if (dep_setup.has_value()) {
        throw std::runtime_error(
            "Bundle dependencies cannot select 'setup' pairs "
            "(spec '" +
            cfg.identity + "')");
      }
      std::string const bundle_id{ pure_bundle->bundle_identity };

      // Register this bundle for identity-based lookups
      declared_bundles[bundle_id] = *pure_bundle;

      // Parse needed_by for pure bundle deps
      std::optional<pkg_phase> needed_by;
      auto needed_by_str{
        sol_util_get_optional<std::string>(table, "needed_by", "Bundle dependency")
      };
      if (needed_by_str.has_value()) {
        needed_by = pkg_phase_parse_needed_by(*needed_by_str, "Bundle dependency");
      }

      // Same bundle package a spec-from-bundle entry would depend on, so declaring
      // both forms of the bundle in one spec still yields one package and one row.
      pkg_cfg *bundle_cfg{
        bundle::ensure_pkg_cfg(*pure_bundle, spec_path, &cfg, bundle_pkgs)
      };
      bundle_cfg->needed_by = needed_by;
      parsed_deps.push_back(bundle_cfg);
      continue;
    }

    // Both strong and weak deps may select 'setup'. Strong deps merge through
    // ensure_pkg; weak deps carry the selection on their weak_reference record
    // and merge it into whatever package they resolve to (see
    // wire_dependency_graph / engine::resolve_*_ref). Existence of the selected
    // pairs is validated post-resolution.
    auto const apply_dep_setup{ [&](pkg_cfg *dep_cfg) {
      if (!dep_setup.has_value()) { return; }
      dep_cfg->setup = dep_setup;
    } };

    // Check for spec-from-bundle: {spec = "id", bundle = "ref"}
    sol::object bundle_obj{ table["bundle"] };
    if (bundle_obj.valid() && bundle_obj.get_type() != sol::type::lua_nil) {
      pkg_cfg *dep_cfg{ parse_spec_from_bundle_dep(table,
                                                   spec_path,
                                                   aliases,
                                                   declared_bundles,
                                                   &cfg,
                                                   bundle_pkgs) };

      if (!cfg.identity.starts_with("local.") && dep_cfg->identity.starts_with("local.")) {
        throw std::runtime_error("non-local spec '" + cfg.identity +
                                 "' cannot depend on local spec '" + dep_cfg->identity +
                                 "'");
      }
      apply_dep_setup(dep_cfg);
      parsed_deps.push_back(dep_cfg);
      continue;
    }

    // Standard dependency (no bundle field)
    pkg_cfg *dep_cfg{ pkg_cfg::parse(entry, spec_path, true) };
    if (!cfg.identity.starts_with("local.") && dep_cfg->identity.starts_with("local.")) {
      throw std::runtime_error("non-local spec '" + cfg.identity +
                               "' cannot depend on local spec '" + dep_cfg->identity +
                               "'");
    }
    apply_dep_setup(dep_cfg);
    parsed_deps.push_back(dep_cfg);
  }

  return parsed_deps;
}

sol::object store_options_in_registry(sol::state &lua,
                                      std::string const &serialized_options) {
  sol::protected_function_result opts_result{
    lua.safe_script("return " + serialized_options, sol::script_pass_on_error)
  };

  if (!opts_result.valid()) {
    sol::error err = opts_result;
    throw std::runtime_error("Failed to deserialize options: " + std::string(err.what()));
  }

  return opts_result.get<sol::object>();
}

void run_options(pkg *p, sol::state &lua) {
  sol::table globals{ lua.globals() };
  std::string const &identity{ p->cfg->identity };

  sol::object options_obj_raw{ globals["OPTIONS"] };
  bool const has_options{ options_obj_raw.valid() &&
                          options_obj_raw.get_type() != sol::type::lua_nil };
  if (!has_options) { return; }

  sol::object opts{ lua.registry()[ENVY_OPTIONS_RIDX] };
  sol::type const options_type{ options_obj_raw.get_type() };

  if (options_type == sol::type::table) {
    validate_options_schema(options_obj_raw.as<sol::table>(), opts, identity);
  } else if (options_type == sol::type::function) {
    sol::protected_function options_fn{ options_obj_raw.as<sol::protected_function>() };

    sol::protected_function_result result{ call_lua_function_with_enriched_errors(
        p,
        "options",
        [&]() { return options_fn(opts); }) };

    sol::object ret_obj{ result };
    sol::type const ret_type{ ret_obj.get_type() };

    auto const failure_prefix{ [&]() {
      return "OPTIONS failed for " + p->cfg->format_key();
    } };

    switch (ret_type) {
      case sol::type::lua_nil: return;
      case sol::type::boolean:
        if (ret_obj.as<bool>()) { return; }
        throw std::runtime_error(failure_prefix() + " (returned false)");
      case sol::type::string:
        throw std::runtime_error(failure_prefix() + ": " + ret_obj.as<std::string>());
      default:
        throw std::runtime_error("OPTIONS must return nil/true/false/string (got " +
                                 std::string(sol::type_name(lua, ret_type)) + ") for " +
                                 p->cfg->format_key());
    }
  } else {
    throw std::runtime_error("OPTIONS must be a table or function (got " +
                             std::string(sol::type_name(lua, options_type)) + ") for " +
                             p->cfg->format_key());
  }
}

void wire_dependency_graph(pkg *p, engine &eng) {
  for (auto *dep_cfg : p->owned_dependency_cfgs) {
    // Pure bundle dep: identity == bundle_identity (bundle fetched for
    // envy.loadenv_spec()) Spec-from-bundle: identity != bundle_identity (spec resolved
    // from bundle)
    bool const is_pure_bundle_dep{ dep_cfg->bundle_identity.has_value() &&
                                   dep_cfg->identity == *dep_cfg->bundle_identity };

    engine_validate_dependency_cycle(
        dep_cfg->identity,
        p->ancestor_chain,
        p->cfg->identity,
        is_pure_bundle_dep ? "Bundle dependency" : "Dependency");

    pkg_phase const needed_by_phase{ dep_cfg->needed_by.has_value()
                                         ? static_cast<pkg_phase>(*dep_cfg->needed_by)
                                         : pkg_phase::pkg_build };
    bool const is_product_dep{ dep_cfg->product.has_value() };
    // What a weak reference for this entry resolves on: the product name when there
    // is one, else the identity.
    std::string const &query{ is_product_dep ? *dep_cfg->product : dep_cfg->identity };

    if (is_product_dep) {
      std::string const &product_name{ *dep_cfg->product };

      bool inserted{ false };
      {
        std::lock_guard const deps_lock(p->deps_mutex);
        inserted = p->product_dependencies
                       .emplace(product_name,
                                pkg::product_dependency{ .name = product_name,
                                                         .needed_by = needed_by_phase,
                                                         .provider = nullptr,
                                                         .constraint_identity =
                                                             dep_cfg->identity })
                       .second;
      }
      if (!inserted) {
        throw std::runtime_error("Duplicate product dependency '" + product_name +
                                 "' in spec '" + p->cfg->identity + "'");
      }
    }

    if (dep_cfg->is_weak_reference()) {
      // No closure overlaps the window where the weak pass can satisfy a reference:
      // depot bootstrap runs after the resolution loop has finished, a
      // source.dependencies closure runs while the barrier is held shut by the
      // consumer waiting on it, and a DEFAULT_SHELL closure is started at
      // target=completion before any worker exists, so it reaches its own string
      // verbs well ahead of the barrier. Same refusal either way, named by the
      // closure.
      //
      // Checked in the same critical section as the append, against the same mutex
      // engine::mark_closure scans under. Checking outside it would leave a window
      // where the mark sees no weak reference and this sees no membership, so the
      // package joins a closure holding a reference nothing can resolve: either the
      // mark observes this append, or this observes the mark.
      std::lock_guard const deps_lock(p->deps_mutex);
      for (auto const kind : { pkg_closure::depot_bootstrap,
                               pkg_closure::fetch,
                               pkg_closure::default_shell }) {
        if (p->in_closure(kind)) {
          throw std::runtime_error(
              std::string{ pkg_closure_name(kind) } + " must use strong dependencies: '" +
              query + "' in spec '" + p->cfg->identity + "' is a weak reference");
        }
      }
      p->weak_references.push_back(pkg::weak_reference{
          .query = query,
          .fallback = dep_cfg->weak,
          .needed_by = needed_by_phase,
          .resolved = nullptr,
          .is_product = is_product_dep,
          .constraint_identity = is_product_dep ? dep_cfg->identity : "",
          .setup = dep_cfg->setup.value_or(std::vector<std::string>{}) });
      continue;
    }

    if (is_product_dep) {
      // Strong product dependency (has source) - wire directly, no weak resolution needed
      pkg *dep{ eng.ensure_pkg(dep_cfg) };

      {
        std::lock_guard const deps_lock(p->deps_mutex);
        p->dependencies[dep_cfg->identity] = { dep, needed_by_phase };
        auto &pd{ p->product_dependencies.at(*dep_cfg->product) };
        pd.provider = dep;
        pd.constraint_identity = dep_cfg->identity;
      }
      eng.propagate_closures(p, dep);
      ENVY_TRACE(dependency_added,
                 p->cfg->identity,
                 .dependency = dep_cfg->identity,
                 .needed_by = needed_by_phase);

      std::vector<std::string> child_chain{ p->ancestor_chain };
      child_chain.push_back(p->cfg->identity);
      eng.start_pkg_thread(dep, pkg_phase::spec_fetch, std::move(child_chain));

      continue;
    }

    // Handle pure bundle dependencies specially
    if (is_pure_bundle_dep) {
      // Pure bundle deps fetch the bundle but don't execute spec phases
      pkg *dep{ eng.ensure_pkg(dep_cfg) };
      {
        std::lock_guard const deps_lock(p->deps_mutex);
        p->dependencies[dep_cfg->identity] = { dep, needed_by_phase };
      }
      eng.propagate_closures(p, dep);
      ENVY_TRACE(dependency_added,
                 p->cfg->identity,
                 .dependency = dep_cfg->identity,
                 .needed_by = needed_by_phase);

      std::vector<std::string> child_chain{ p->ancestor_chain };
      child_chain.push_back(p->cfg->identity);
      eng.start_pkg_thread(dep, pkg_phase::spec_fetch, std::move(child_chain));
      continue;
    }

    pkg *dep{ eng.ensure_pkg(dep_cfg) };

    // Store dependency info in parent's map for ctx.pkg() lookup and phase coordination
    {
      std::lock_guard const deps_lock(p->deps_mutex);
      p->dependencies[dep_cfg->identity] = { dep, needed_by_phase };
    }
    eng.propagate_closures(p, dep);
    ENVY_TRACE(dependency_added,
               p->cfg->identity,
               .dependency = dep_cfg->identity,
               .needed_by = needed_by_phase);

    std::vector<std::string> child_chain{ p->ancestor_chain };
    child_chain.push_back(p->cfg->identity);
    eng.start_pkg_thread(dep, pkg_phase::spec_fetch, std::move(child_chain));
  }
}

}  // namespace

// Materialize a bundle into the engine's registry: the whole job of a BUNDLE_ONLY
// package's spec_fetch. Specs pulled from the bundle depend on this package, so it
// always runs first. Sets the outcome flags the completion phase renders.
void materialize_bundle(pkg_cfg const &cfg, pkg *p, engine &eng) {
  auto const *bundle_src{ std::get_if<pkg_cfg::bundle_source>(&cfg.source) };
  std::string const &bundle_id{ bundle_src->bundle_identity };

  // Another cfg for the same bundle identity already materialized it (a manifest
  // and a spec can each declare the same bundle); nothing left to do.
  if (eng.find_bundle(bundle_id)) {
    p->was_cache_hit = true;
    return;
  }

  // Local bundles (identity starts with "local.") use source directory in-situ
  if (bundle_id.starts_with("local.")) {
    auto const *local_src{ std::get_if<pkg_cfg::local_source>(&bundle_src->fetch_source) };
    if (local_src && std::filesystem::is_directory(local_src->file_path)) {
      tui::debug("spec: local bundle %s", bundle_id.c_str());

      bundle parsed{ bundle::from_path(local_src->file_path) };
      if (parsed.identity != bundle_id) {
        throw std::runtime_error("Bundle identity mismatch: expected '" + bundle_id +
                                 "' but manifest declares '" + parsed.identity + "'");
      }
      parsed.validate();
      eng.register_bundle(bundle_id, std::move(parsed.specs), local_src->file_path);
      p->bundle_in_situ = true;
      return;
    }
  }

  // Non-local bundle: fetch to cache. The completion phase turns these flags into
  // this package's outcome row, exactly as it does for any other package.
  std::string const bundle_source_key{ std::visit(
      match{
          [](pkg_cfg::remote_source const &r) { return source_key(r); },
          [](pkg_cfg::git_source const &g) { return source_key(g); },
          [](pkg_cfg::local_source const &l) { return source_key(l); },
          [&](pkg_cfg::custom_fetch_source const &) {
            return custom_fetch_source_key(cfg);
          },
      },
      bundle_src->fetch_source) };

  auto cache_result{ p->cache_ptr->ensure_spec(bundle_id, bundle_source_key) };
  p->was_cache_hit = cache_result.lock == nullptr;

  if (cache_result.lock) {
    tui::debug("spec: bundle %s", bundle_id.c_str());
    std::filesystem::path const install_dir{ cache_result.lock->install_dir() };

    // Fetch based on underlying source type
    std::visit(
        match{
            [&](pkg_cfg::remote_source const &remote) {
              std::filesystem::path fetch_dest{ cache_result.lock->fetch_dir() /
                                                uri_extract_filename(remote.url) };

              fetch_with_progress(fetch_request_from_url(remote.url, fetch_dest),
                                  p,
                                  remote.url,
                                  "bundle");

              if (!remote.sha256.empty()) {
                sha256_verify(remote.sha256, sha256(fetch_dest));
              }

              extract(fetch_dest, install_dir);
            },
            [&](pkg_cfg::local_source const &local) {  // non-local. identity
              if (std::filesystem::is_directory(local.file_path)) {
                std::filesystem::copy(
                    local.file_path,
                    install_dir,
                    std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::overwrite_existing);
              } else {
                extract(local.file_path, install_dir);
              }
            },
            [&](pkg_cfg::git_source const &git) {
              auto const git_info{ uri_classify(git.url) };
              fetch_with_progress(fetch_request_git{ .source = git.url,
                                                     .destination = install_dir,
                                                     .ref = git.ref,
                                                     .scheme = git_info.scheme },
                                  p,
                                  git.url,
                                  "git bundle");
            },
            [&](pkg_cfg::custom_fetch_source const &) {
              // Custom fetch bundle - execute fetch function
              // Function location depends on where bundle was declared:
              // - If parent is set: fetch function is in parent spec's Lua state
              // - If no parent: fetch function is in manifest's BUNDLES table

              std::filesystem::path const tmp_dir{ cache_result.lock->work_dir() / "tmp" };
              std::filesystem::create_directories(tmp_dir);

              if (cfg.parent) {
                // Bundle declared in a spec's DEPENDENCIES - use parent's Lua state
                pkg *parent{ eng.find_exact(pkg_key(*cfg.parent)) };
                if (!parent) {
                  throw std::runtime_error(
                      "Bundle custom fetch: parent spec Lua state unavailable for " +
                      bundle_id);
                }
                auto const parent_acc{ parent->lua.lock() };
                if (!parent_acc) {
                  throw std::runtime_error(
                      "Bundle custom fetch: parent spec Lua state unavailable for " +
                      bundle_id);
                }
                sol::state_view parent_lua{ *parent_acc };

                auto fetch_func_opt{ pkg_cfg::get_bundle_fetch(parent_lua, bundle_id) };
                if (!fetch_func_opt) {
                  throw std::runtime_error(
                      "Bundle custom fetch function not found in parent spec for: " +
                      bundle_id);
                }

                // Set up phase context in parent's Lua state
                phase_context_guard ctx_guard{ &eng,
                                               parent,
                                               parent_lua.lua_state(),
                                               tmp_dir,
                                               cache_result.lock.get() };

                tui::debug("spec: custom fetch for bundle %s", bundle_id.c_str());
                sol::protected_function_result result{ (*fetch_func_opt)(
                    util_normalized_path(tmp_dir)) };

                if (!result.valid()) {
                  sol::error err = result;
                  throw std::runtime_error("Bundle custom fetch function failed for " +
                                           bundle_id + ": " + err.what());
                }
              } else {
                // Bundle declared in manifest's BUNDLES table
                manifest const *m{ eng.get_manifest() };
                if (!m) {
                  throw std::runtime_error("Bundle custom fetch requires manifest: " +
                                           bundle_id);
                }

                phase_context ctx{ &eng, p, tmp_dir, cache_result.lock.get() };
                tui::debug("spec: custom fetch for bundle %s", bundle_id.c_str());

                auto err{ m->run_bundle_fetch(bundle_id, &ctx, tmp_dir) };
                if (err) {
                  throw std::runtime_error("Bundle custom fetch function failed for " +
                                           bundle_id + ": " + *err);
                }
              }

              // Custom fetch creates files in fetch_dir via envy.commit_fetch
              // Move bundle files to install_dir
              std::filesystem::path const fetch_dir{ cache_result.lock->fetch_dir() };
              std::filesystem::path const bundle_manifest{ fetch_dir / "envy-bundle.lua" };

              if (!std::filesystem::exists(bundle_manifest)) {
                throw std::runtime_error(
                    "Bundle custom fetch did not create envy-bundle.lua: " + bundle_id);
              }

              // Move all files from fetch_dir to install_dir
              for (auto const &entry : std::filesystem::directory_iterator(fetch_dir)) {
                std::filesystem::path const dest{ install_dir / entry.path().filename() };
                std::filesystem::rename(entry.path(), dest);
              }
            },
        },
        bundle_src->fetch_source);
  }

  // Parse and validate the bundle manifest while the entry is still unfinalized:
  // `envy-complete` is never revalidated, so marking first would make a malformed
  // bundle a permanent cache entry that fails identically on every later run.
  // Throwing here drops the lock uncompleted, its destructor scrubs the entry, and
  // the next run refetches.
  bundle parsed{ bundle::from_path(cache_result.pkg_path) };

  if (parsed.identity != bundle_id) {
    throw std::runtime_error("Bundle identity mismatch: expected '" + bundle_id +
                             "' but manifest declares '" + parsed.identity + "'");
  }

  parsed.validate();

  if (cache_result.lock) {
    cache_result.lock->mark_install_complete();
    cache_result.lock.reset();
  }

  // Register the bundle for envy.loadenv_spec() access
  eng.register_bundle(bundle_id, std::move(parsed.specs), cache_result.pkg_path);
}

void run_spec_fetch_phase(pkg *p, engine &eng) {
  pkg_cfg const &cfg{ *p->cfg };

  // Handle pure bundle dependencies (identity == bundle_identity)
  // These just fetch the bundle without loading a spec
  bool const is_pure_bundle_dep{
    cfg.bundle_identity.has_value() && cfg.identity == *cfg.bundle_identity &&
    std::holds_alternative<pkg_cfg::bundle_source>(cfg.source)
  };

  if (is_pure_bundle_dep) {
    phase_trace_scope const phase_scope{ cfg.identity,
                                         pkg_phase::spec_fetch,
                                         std::chrono::steady_clock::now() };
    materialize_bundle(cfg, p, eng);
    p->type = pkg_type::BUNDLE_ONLY;
    return;  // No spec to load - bundle is now available for envy.loadenv_spec()
  }

  phase_trace_scope const phase_scope{ cfg.identity,
                                       pkg_phase::spec_fetch,
                                       std::chrono::steady_clock::now() };

  // Fetch spec based on source type. A cache-miss fetch hands back its entry lock
  // still held: the entry is finalized below, only once the fetched bytes have
  // proven to be a loadable spec declaring the expected identity. Finalizing at
  // fetch time instead would bake a broken download (404 body, truncated file,
  // wrong spec) into a cache entry that is never revalidated and so fails the same
  // way forever.
  spec_fetch_result fetched{ [&] {
    if (auto const *local_src{ std::get_if<pkg_cfg::local_source>(&cfg.source) }) {
      return fetch_local_source(cfg);
    } else if (std::holds_alternative<pkg_cfg::remote_source>(cfg.source)) {
      return fetch_remote_source(cfg, p);
    } else if (std::holds_alternative<pkg_cfg::git_source>(cfg.source)) {
      return fetch_git_source(cfg, p);
    } else if (cfg.has_fetch_function()) {
      return fetch_custom_function(cfg, p, eng);
    } else if (std::holds_alternative<pkg_cfg::bundle_source>(cfg.source)) {
      return resolve_spec_from_bundle(cfg, eng);
    } else {
      throw std::runtime_error("Unsupported source type: " + cfg.identity);
    }
  }() };

  std::filesystem::path const &spec_path{ fetched.spec_path };

  if (!std::filesystem::exists(spec_path)) {
    throw std::runtime_error("Spec source not found: " + spec_path.string() +
                             " (for spec '" + cfg.identity + "')");
  }

  // Load and validate spec script
  auto lua{ create_lua_state() };

  // For specs from bundles, configure package.path for require() calls
  if (cfg.bundle_identity.has_value()) {
    if (bundle * b{ eng.find_bundle(*cfg.bundle_identity) }) {
      b->configure_package_path(*lua);
    }
  }

  p->schema = load_spec_script(*lua, spec_path, cfg.identity);

  // Store spec file path for error reporting
  p->spec_file_path = spec_path;

  std::string const declared_identity{ [&] {
    try {
      sol::object identity_obj{ (*lua)["IDENTITY"] };
      if (!identity_obj.valid() || identity_obj.get_type() != sol::type::string) {
        throw std::runtime_error("Spec must define 'IDENTITY' global as a string");
      }
      return identity_obj.as<std::string>();
    } catch (std::runtime_error const &e) {
      throw std::runtime_error(std::string(e.what()) + " (in spec: " + cfg.identity + ")");
    }
  }() };

  if (declared_identity != cfg.identity) {
    throw std::runtime_error("Identity mismatch: expected '" + cfg.identity +
                             "' but spec declares '" + declared_identity + "'");
  }

  // The fetched file is a spec for this identity: the entry is now worth keeping.
  // Everything past this point is spec semantics, which a refetch cannot repair.
  if (fetched.lock) {
    fetched.lock->mark_install_complete();
    fetched.lock.reset();
  }

  // Determine package type (user-managed or cache-managed), parse SETUP pairs,
  // and validate the phase-verb matrix.
  {
    sol::state_view lua_view{ *lua };
    bool const user_managed{ resolve_user_managed(lua_view, cfg.identity) };
    p->setup_pairs = parse_setup_table(lua_view, cfg.identity);
    validate_phases(lua_view, cfg.identity, user_managed, !p->setup_pairs.empty());
    p->type = user_managed ? pkg_type::USER_MANAGED : pkg_type::CACHE_MANAGED;
    tui::debug(user_managed ? "spec: user-managed (setup-only)" : "spec: cache-managed");
  }

  {  // deps_mutex-guarded: engine::register_products reads this from the barrier side
    auto parsed{ parse_products_table(cfg, *lua, p) };
    std::lock_guard const deps_lock(p->deps_mutex);
    p->products = std::move(parsed);
  }

  // Extract spec PLATFORMS and intersect with manifest-level platforms
  {
    std::vector<std::string> spec_platforms;
    sol::object platforms_obj{ (*lua)["PLATFORMS"] };
    if (platforms_obj.valid() && platforms_obj.get_type() != sol::type::lua_nil) {
      if (platforms_obj.get_type() != sol::type::table) {
        throw std::runtime_error("PLATFORMS must be a table in spec '" + cfg.identity +
                                 "'");
      }
      sol::table plat_table{ platforms_obj.as<sol::table>() };
      for (size_t i{ 1 }; i <= plat_table.size(); ++i) {
        sol::object elem{ plat_table[i] };
        if (!elem.is<std::string>()) {
          throw std::runtime_error("PLATFORMS entries must be strings in spec '" +
                                   cfg.identity + "'");
        }
        spec_platforms.push_back(elem.as<std::string>());
      }
    }
    auto intersected{ util_platform_intersect(cfg.platforms, spec_platforms) };
    // Disjoint non-empty inputs yield empty, but empty = "all platforms" elsewhere.
    // Use a sentinel that matches nothing so scripts are never generated.
    if (intersected.empty() && !cfg.platforms.empty() && !spec_platforms.empty()) {
      intersected.emplace_back(kPlatformNone);
    }
    std::lock_guard const deps_lock(p->deps_mutex);  // guards resolved_platforms
    p->resolved_platforms = std::move(intersected);
  }

  p->owned_dependency_cfgs = parse_dependencies_table(*lua, spec_path, cfg);

  for (auto *dep_cfg : p->owned_dependency_cfgs) { dep_cfg->parent = p->cfg; }

  try {  // Store options in Lua registry
    lua->registry()[ENVY_OPTIONS_RIDX] =
        store_options_in_registry(*lua, cfg.serialized_options);
  } catch (std::runtime_error const &e) {
    throw std::runtime_error(e.what() + std::string(" for ") + cfg.identity);
  }

  run_options(p, *lua);

  {  // Extract dependency identities for ctx.pkg() validation
    std::lock_guard const deps_lock(p->deps_mutex);
    p->declared_dependencies.reserve(p->owned_dependency_cfgs.size());
    for (auto const *dep_cfg : p->owned_dependency_cfgs) {
      p->declared_dependencies.push_back(dep_cfg->identity);
    }
  }

  p->lua.set(std::move(lua));

  wire_dependency_graph(p, eng);
}

}  // namespace envy
