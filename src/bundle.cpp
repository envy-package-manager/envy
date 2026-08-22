#include "bundle.h"

#include "manifest.h"
#include "sol_util.h"
#include "spec_util.h"
#include "tui.h"
#include "uri.h"
#include "util.h"

#include <optional>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

namespace envy {

namespace {

// Internal representation of a bundle declaration (not exposed in header)
struct bundle_decl {
  std::string identity;

  struct remote_source {
    std::string url;
    std::string sha256;
  };
  struct local_source {
    std::filesystem::path file_path;
  };
  struct git_source {
    std::string url;
    std::string ref;
  };
  struct custom_fetch_source {
    std::vector<pkg_cfg *> dependencies;  // Parsed dependencies
  };

  using source_t =
      std::variant<remote_source, local_source, git_source, custom_fetch_source>;
  source_t source;
};

// Parse source table for custom fetch: { fetch = function, dependencies = {...} }
bundle_decl::source_t parse_source_table_for_bundle(
    sol::table const &source_table,
    std::filesystem::path const &base_path) {
  sol::object fetch_obj{ source_table["fetch"] };
  if (!fetch_obj.valid() || !fetch_obj.is<sol::function>()) {
    throw std::runtime_error("Bundle source table requires 'fetch' function");
  }

  bundle_decl::custom_fetch_source result;

  sol::object deps_obj{ source_table["dependencies"] };
  if (deps_obj.valid() && deps_obj.get_type() != sol::type::lua_nil) {
    if (!deps_obj.is<sol::table>()) {
      throw std::runtime_error("Bundle source.dependencies must be array (table)");
    }
    sol::table deps_table{ deps_obj.as<sol::table>() };
    for (size_t i{ 1 }, n{ deps_table.size() }; i <= n; ++i) {
      result.dependencies.push_back(
          pkg_cfg::parse_fetch_dependency(deps_table[i], base_path));
    }
  }

  return result;
}

bundle_decl parse_decl(sol::table const &table, std::filesystem::path const &base_path) {
  bundle_decl decl;

  // Required: identity
  auto identity_opt{ sol_util_get_optional<std::string>(table, "identity", "Bundle") };
  if (!identity_opt.has_value() || identity_opt->empty()) {
    throw std::runtime_error("Bundle declaration missing required 'identity' field");
  }
  decl.identity = std::move(*identity_opt);

  // Required: source (string or table)
  sol::object source_obj{ table["source"] };
  if (!source_obj.valid() || source_obj.get_type() == sol::type::lua_nil) {
    throw std::runtime_error("Bundle declaration missing required 'source' field");
  }

  // Handle source as table (custom fetch with dependencies)
  if (source_obj.is<sol::table>()) {
    decl.source = parse_source_table_for_bundle(source_obj.as<sol::table>(), base_path);
    return decl;
  }

  // Handle source as string (URL/path)
  if (!source_obj.is<std::string>()) {
    throw std::runtime_error("Bundle 'source' must be string (URL/path) or table");
  }

  std::string const source_uri{ source_obj.as<std::string>() };
  if (source_uri.empty()) {
    throw std::runtime_error("Bundle 'source' string cannot be empty");
  }

  auto const info{ uri_classify(source_uri) };

  if (info.scheme == uri_scheme::GIT || info.scheme == uri_scheme::GIT_HTTPS) {
    auto ref_opt{ sol_util_get_optional<std::string>(table, "ref", "Bundle") };
    if (!ref_opt.has_value() || ref_opt->empty()) {
      throw std::runtime_error("Bundle with git source requires 'ref' field");
    }
    decl.source =
        bundle_decl::git_source{ .url = info.canonical, .ref = std::move(*ref_opt) };
  } else if (info.scheme == uri_scheme::LOCAL_FILE_RELATIVE ||
             info.scheme == uri_scheme::LOCAL_FILE_ABSOLUTE) {
    std::filesystem::path resolved{ info.canonical };
    if (info.scheme == uri_scheme::LOCAL_FILE_RELATIVE) {
      resolved = (base_path.parent_path() / resolved).lexically_normal();
    }
    decl.source = bundle_decl::local_source{ .file_path = std::move(resolved) };
  } else {
    auto sha256_opt{ sol_util_get_optional<std::string>(table, "sha256", "Bundle") };
    decl.source = bundle_decl::remote_source{ .url = info.canonical,
                                              .sha256 = sha256_opt.value_or("") };
  }

  return decl;
}

// Convert internal bundle_decl to public pkg_cfg::bundle_source
pkg_cfg::bundle_source decl_to_source(bundle_decl const &decl) {
  return std::visit(
      match{
          [&](bundle_decl::remote_source const &s) -> pkg_cfg::bundle_source {
            return { .bundle_identity = decl.identity,
                     .fetch_source =
                         pkg_cfg::remote_source{ .url = s.url, .sha256 = s.sha256 } };
          },
          [&](bundle_decl::local_source const &s) -> pkg_cfg::bundle_source {
            return { .bundle_identity = decl.identity,
                     .fetch_source = pkg_cfg::local_source{ .file_path = s.file_path } };
          },
          [&](bundle_decl::git_source const &s) -> pkg_cfg::bundle_source {
            return { .bundle_identity = decl.identity,
                     .fetch_source = pkg_cfg::git_source{ .url = s.url, .ref = s.ref } };
          },
          [&](bundle_decl::custom_fetch_source const &s) -> pkg_cfg::bundle_source {
            return { .bundle_identity = decl.identity,
                     .fetch_source =
                         pkg_cfg::custom_fetch_source{ .dependencies = s.dependencies } };
          },
      },
      decl.source);
}

}  // namespace

// bundle methods

std::filesystem::path bundle::resolve_spec_path(std::string const &spec_identity) const {
  auto const it{ specs.find(spec_identity) };
  if (it == specs.end()) { return {}; }
  return cache_path / it->second;
}

bundle bundle::from_path(std::filesystem::path const &cache_path) {
  std::filesystem::path const manifest_path{ cache_path / "envy-bundle.lua" };

  if (!std::filesystem::exists(manifest_path)) {
    throw std::runtime_error("Bundle manifest not found: " + manifest_path.string());
  }

  auto const content{ util_load_file(manifest_path) };
  auto const meta{ parse_envy_meta(
      { reinterpret_cast<char const *>(content.data()), content.size() }) };

  auto lua{ sol_util_make_lua_state() };
  std::string const script{ reinterpret_cast<char const *>(content.data()),
                            content.size() };
  std::string const chunk_name{ "@" + manifest_path.string() };
  sol::protected_function_result result{
    lua->safe_script(script, sol::script_pass_on_error, chunk_name)
  };

  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error("Failed to parse bundle manifest " + manifest_path.string() +
                             ": " + err.what());
  }

  bundle b;
  b.cache_path = cache_path;
  b.schema = meta.schema;

  // Required: BUNDLE identity
  sol::object bundle_obj{ (*lua)["BUNDLE"] };
  if (!bundle_obj.valid() || !bundle_obj.is<std::string>()) {
    throw std::runtime_error("Bundle manifest missing required 'BUNDLE' field: " +
                             manifest_path.string());
  }
  b.identity = bundle_obj.as<std::string>();
  if (b.identity.empty()) {
    throw std::runtime_error("Bundle manifest 'BUNDLE' field cannot be empty: " +
                             manifest_path.string());
  }

  // Required: SPECS table
  sol::object specs_obj{ (*lua)["SPECS"] };
  if (!specs_obj.valid() || !specs_obj.is<sol::table>()) {
    throw std::runtime_error("Bundle manifest missing required 'SPECS' table: " +
                             manifest_path.string());
  }

  sol::table specs_table{ specs_obj.as<sol::table>() };
  for (auto const &[key, value] : specs_table) {
    if (!key.is<std::string>()) {
      throw std::runtime_error("SPECS key must be string in bundle: " +
                               manifest_path.string());
    }
    if (!value.is<std::string>()) {
      throw std::runtime_error("SPECS value must be string (relative path) in bundle: " +
                               manifest_path.string());
    }

    std::string spec_identity{ key.as<std::string>() };
    std::string relative_path{ value.as<std::string>() };

    if (spec_identity.empty()) {
      throw std::runtime_error("SPECS key cannot be empty in bundle: " +
                               manifest_path.string());
    }
    if (relative_path.empty()) {
      throw std::runtime_error("SPECS path cannot be empty for '" + spec_identity +
                               "' in bundle: " + manifest_path.string());
    }

    std::filesystem::path p{ relative_path };
    if (p.is_absolute()) {
      throw std::runtime_error("SPECS path must be relative, got absolute path for '" +
                               spec_identity + "' in bundle: " + manifest_path.string());
    }

    b.specs.emplace(std::move(spec_identity), std::move(relative_path));
  }

  if (b.specs.empty()) {
    throw std::runtime_error("Bundle SPECS table cannot be empty: " +
                             manifest_path.string());
  }

  return b;
}

void bundle::configure_package_path(sol::state &lua) const {
  std::string const bundle_root{ cache_path.string() };
  sol::table package_table = lua["package"];
  std::string const current_path{ package_table["path"].get_or<std::string>("") };
  package_table["path"] =
      bundle_root + "/?.lua;" + bundle_root + "/?/init.lua;" + current_path;
}

void bundle::validate() const {
  struct validation_result {
    std::string spec_key;
    std::optional<std::string> error;
  };

  std::vector<validation_result> results(specs.size());
  std::vector<std::thread> threads;
  threads.reserve(specs.size());

  size_t i{ 0 };
  for (auto const &[expected_id, relative_path] : specs) {
    results[i].spec_key = expected_id;
    threads.emplace_back([this, expected_id, relative_path, &r = results[i]] {
      std::filesystem::path const spec_path{ cache_path / relative_path };

      if (!std::filesystem::exists(spec_path)) {
        r.error = "file not found: " + spec_path.string();
        return;
      }

      try {  // Execute spec and verify IDENTITY matches key
        std::string const actual_id{ extract_spec_identity(spec_path, cache_path) };
        if (actual_id != expected_id) {
          r.error =
              "IDENTITY mismatch: expected '" + expected_id + "', got '" + actual_id + "'";
        }
      } catch (std::exception const &e) { r.error = e.what(); }
    });
    ++i;
  }

  for (auto &t : threads) { t.join(); }

  for (auto const &r : results) {
    if (r.error) {
      throw std::runtime_error("bundle '" + identity + "' spec '" + r.spec_key +
                               "': " + *r.error);
    }
  }
}

std::unordered_map<std::string, pkg_cfg::bundle_source> bundle::parse_aliases(
    sol::object const &bundles_obj,
    std::filesystem::path const &base_path) {
  std::unordered_map<std::string, pkg_cfg::bundle_source> result;

  if (!bundles_obj.valid() || bundles_obj.get_type() == sol::type::lua_nil) {
    return result;
  }

  if (!bundles_obj.is<sol::table>()) {
    throw std::runtime_error("BUNDLES must be a table");
  }

  sol::table bundles_lua{ bundles_obj.as<sol::table>() };

  for (auto const &[key, value] : bundles_lua) {
    if (!key.is<std::string>()) { throw std::runtime_error("BUNDLES key must be string"); }
    std::string alias{ key.as<std::string>() };

    if (!value.is<sol::table>()) {
      throw std::runtime_error("BUNDLES['" + alias + "'] must be a table");
    }

    bundle_decl decl{ parse_decl(value.as<sol::table>(), base_path) };
    result.emplace(std::move(alias), decl_to_source(decl));
  }

  return result;
}

pkg_cfg::bundle_source bundle::parse_inline(sol::table const &table,
                                            std::filesystem::path const &base_path) {
  bundle_decl decl{ parse_decl(table, base_path) };
  return decl_to_source(decl);
}

pkg_cfg *bundle::ensure_pkg_cfg(pkg_cfg::bundle_source const &src,
                                std::filesystem::path const &decl_path,
                                pkg_cfg const *declared_by,
                                std::unordered_map<std::string, pkg_cfg *> &memo) {
  if (auto const it{ memo.find(src.bundle_identity) }; it != memo.end()) {
    // One declaring scope, so this is the only cfg that will ever exist for the
    // identity — engine::validate_bundle_redeclaration compares cfgs and would never
    // see the loser. Reject a disagreeing redeclaration here instead; the engine
    // still catches the cross-scope case (manifest versus spec, spec versus spec).
    auto const &existing{ std::get<pkg_cfg::bundle_source>(it->second->source) };
    switch (bundle_source_compare(existing, src)) {
      case bundle_source_match::SAME: break;

      case bundle_source_match::INCOMPARABLE:
        // Two fetch closures: undecidable, so keep the first and say so.
        tui::warn(
            "bundle '%s' is declared with more than one custom fetch function in %s; "
            "the first will run",
            src.bundle_identity.c_str(),
            decl_path.string().c_str());
        break;

      case bundle_source_match::DIFFERENT:
        throw std::runtime_error("bundle '" + src.bundle_identity +
                                 "' is declared more than once in " + decl_path.string() +
                                 " with conflicting sources; a bundle identity must "
                                 "name one payload");
    }
    return it->second;
  }

  // A custom-fetch bundle's own dependencies must land before its fetch runs, so
  // they ride along as the bundle package's source dependencies.
  auto const *custom{ std::get_if<pkg_cfg::custom_fetch_source>(&src.fetch_source) };

  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(
      src.bundle_identity,  // identity == bundle identity marks it BUNDLE_ONLY
      pkg_cfg::bundle_source{ src },
      "{}",
      std::nullopt,  // needed_by: source dependencies always block spec_fetch
      declared_by,   // parent: where a custom fetch function is looked up
      nullptr,       // weak
      custom ? custom->dependencies : std::vector<pkg_cfg *>{},
      std::nullopt,  // product
      decl_path) };

  cfg->bundle_identity = src.bundle_identity;
  memo.emplace(src.bundle_identity, cfg);
  return cfg;
}

}  // namespace envy
