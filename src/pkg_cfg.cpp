#include "pkg_cfg.h"

#include "lua_ctx/lua_envy_import.h"
#include "sol_util.h"
#include "uri.h"
#include "util.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace envy {

namespace {

pkg_cfg_pool g_default_pkg_cfg_pool;

std::atomic<std::uint64_t> g_custom_fetch_decl_seq{ 0 };

// "namespace.name@revision", with all three parts non-empty and path-safe.
bool identity_is_valid(std::string const &identity) {
  if (!util_is_safe_path_component(identity)) { return false; }

  auto const at_pos{ identity.find('@') };
  if (at_pos == std::string::npos || at_pos == 0 || at_pos == identity.size() - 1) {
    return false;
  }

  auto const dot_pos{ identity.find('.') };
  return dot_pos != std::string::npos && dot_pos != 0 && dot_pos + 1 < at_pos;
}

bool contains_function(sol::object const &val) {
  if (val.is<sol::function>()) { return true; }
  if (val.is<sol::table>()) {
    sol::table tbl{ val.as<sol::table>() };
    for (auto const &[key, nested_val] : tbl) {
      sol::object nested_obj(nested_val);
      if (contains_function(nested_obj)) { return true; }
    }
  }
  return false;
}

// Every key one entry shape's parser reads, sorted so the error message is stable.
// A key absent here is silently inert today, which is the whole reason to reject it.
constexpr std::string_view kManifestPackageKeys[]{ kEnvyBaseKey, kEnvyBundlesKey,
                                                   "needed_by", "options",
                                                   "platforms", "product",
                                                   "ref",       "setup",
                                                   "sha256",    "source",
                                                   "spec" };
constexpr std::string_view kDependencyKeys[]{ "needed_by", "options", "product",
                                              "ref",       "setup",   "sha256",
                                              "source",    "spec",    "weak" };
constexpr std::string_view kFetchDependencyKeys[]{ "options", "product", "ref",
                                                   "sha256",  "source",  "spec",
                                                   "weak" };
constexpr std::string_view kWeakFallbackKeys[]{ "needed_by", "options", "product",
                                                "ref",       "sha256",  "source",
                                                "spec" };

// What one entry shape may say. `fetch_function_runs` is false where nothing could find
// the closure again: no parent Lua state (manifest), or no `source` to look on (weak).
struct entry_rules {
  std::span<std::string_view const> keys;
  char const *context;
  bool source_optional;
  bool fetch_function_runs;
  bool needed_by_allowed;
};

constexpr entry_rules rules_for(pkg_entry_shape shape) {
  static_assert(kPkgEntryShapeCount == 4, "add a rules_for case for the new entry shape");
  switch (shape) {
    case pkg_entry_shape::MANIFEST_PACKAGE:
      return { kManifestPackageKeys, "Package", false, false, true };
    case pkg_entry_shape::DEPENDENCY:
      return { kDependencyKeys, "Dependency", true, true, true };
    case pkg_entry_shape::FETCH_DEPENDENCY:
      return { kFetchDependencyKeys, "source.dependencies entry", true, true, false };
    case pkg_entry_shape::WEAK_FALLBACK:
      return { kWeakFallbackKeys, "weak fallback", false, false, true };
    case pkg_entry_shape::COUNT: break;
  }
  return { kDependencyKeys, "Dependency", true, true, true };  // -Wreturn-type only
}

bool has_value(sol::table const &table, char const *key) {
  sol::object const obj{ table[key] };
  return obj.valid() && obj.get_type() != sol::type::lua_nil;
}

// Parse source table (custom source fetch with dependencies)
pkg_cfg::source_t parse_source_table(sol::table const &source_table,
                                     pkg_decl_origin const &origin,
                                     std::vector<pkg_cfg *> &out_dependencies) {
  // Check for dependencies field (need to parse as array)
  bool has_dependencies{ false };
  sol::object deps_obj{ source_table["dependencies"] };
  if (deps_obj.valid()) {
    if (deps_obj.is<sol::table>()) {
      sol::table deps_table{ deps_obj.as<sol::table>() };
      has_dependencies = true;
      for (size_t i{ 1 }, n{ deps_table.size() }; i <= n; ++i) {
        out_dependencies.push_back(pkg_cfg::parse_fetch_dependency(deps_table[i], origin));
      }
      pkg_cfg_reject_option_variants(
          out_dependencies,
          "source.dependencies in " + origin.declaring_file.string());
    } else {
      throw std::runtime_error("source.dependencies must be array (table)");
    }
  }

  bool const has_fetch{ [&] {  // Check for fetch function
    sol::object fetch_obj{ source_table["fetch"] };
    if (!fetch_obj.valid()) { return false; }
    if (!fetch_obj.is<sol::function>()) {
      throw std::runtime_error("source.fetch must be a function");
    }
    return true;
  }() };

  if (has_dependencies && !has_fetch) {  // deps require fetch, fetch can exist alone
    throw std::runtime_error("source.dependencies requires source.fetch function");
  }

  if (!has_dependencies && !has_fetch) {
    throw std::runtime_error(
        "source table must declare a 'fetch' function (with optional 'dependencies'); "
        "a URL or path is written as a plain string");
  }

  return pkg_cfg::fetch_function{};  // Custom source fetch - no URL-based source
}

// Parse source string (URI-based sources)
pkg_cfg::source_t parse_source_string(std::string const &source_uri,
                                      sol::table const &table,
                                      std::filesystem::path const &anchor) {
  auto const info{ uri_classify(source_uri) };

  if (info.scheme == uri_scheme::GIT || info.scheme == uri_scheme::GIT_HTTPS) {
    std::string const ref_str{
      sol_util_get_required<std::string>(table, "ref", "Spec with git source")
    };
    if (ref_str.empty()) { throw std::runtime_error("Spec 'ref' field cannot be empty"); }

    return pkg_cfg::git_source{ .url = info.canonical, .ref = ref_str };
  }

  auto const sha256{ sol_util_get_optional<std::string>(table, "sha256", "Spec source") };

  // If SHA256 is provided, always treat as remote_source (needs verification)
  // Otherwise, local files use local_source, remote URIs use remote_source
  if (sha256.has_value() || (info.scheme != uri_scheme::LOCAL_FILE_ABSOLUTE &&
                             info.scheme != uri_scheme::LOCAL_FILE_RELATIVE)) {
    // Remote source or local file with SHA256 verification
    std::string const resolved_uri{ [&]() -> std::string {
      if (info.scheme == uri_scheme::LOCAL_FILE_RELATIVE) {
        std::filesystem::path p{ info.canonical };
        p = anchor.parent_path() / p;
        return "file://" + p.lexically_normal().string();
      } else if (info.scheme == uri_scheme::LOCAL_FILE_ABSOLUTE) {
        return "file://" + info.canonical;
      } else {
        return info.canonical;
      }
    }() };
    return pkg_cfg::remote_source{ .url = resolved_uri, .sha256 = sha256.value_or("") };
  }

  // Local file without SHA256, unverified
  return pkg_cfg::local_source{
    .file_path = (info.scheme == uri_scheme::LOCAL_FILE_RELATIVE)
                     ? (anchor.parent_path() / info.canonical).lexically_normal()
                     : std::filesystem::path{ info.canonical }
  };
}

// Lua string literal: quote, backslash, and every control byte as a three-digit decimal
// escape. The result is re-executed to rebuild options; a raw newline would not survive.
std::string quote_lua_string(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (unsigned char const c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20 || c == 0x7f) {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\%03u", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
  return out;
}

// One option value, canonically. `path` names where it sits ("options.flags[2]") so a
// rejection points at the offending key rather than at the whole table.
std::string serialize_option_value(sol::object const &val, std::string const &path) {
  sol::type const type{ val.get_type() };

  if (type == sol::type::lua_nil) { return "nil"; }
  if (type == sol::type::boolean) { return val.as<bool>() ? "true" : "false"; }

  if (type == sol::type::number) {
    lua_State *L{ val.lua_state() };
    int const stack_before{ lua_gettop(L) };
    val.push();
    bool const is_int{ lua_isinteger(L, -1) != 0 };
    lua_settop(L, stack_before);

    if (is_int) {
      return std::to_string(val.as<lua_Integer>());
    } else {  // Float value - serialize with full precision
      char buf[32];
      auto [ptr, ec] = std::to_chars(buf,
                                     buf + sizeof(buf),
                                     val.as<lua_Number>(),
                                     std::chars_format::general);
      if (ec != std::errc{}) {
        throw std::runtime_error("Failed to serialize double value");
      }
      return std::string{ buf, ptr };
    }
  }

  if (val.is<std::string>()) { return quote_lua_string(val.as<std::string>()); }

  if (val.is<sol::table>()) {
    sol::table table{ val.as<sol::table>() };
    if (table.empty()) { return "{}"; }

    std::vector<std::pair<sol::object, sol::object>> entries;
    for (auto const &[key, value] : table) { entries.emplace_back(key, value); }

    bool const all_integer_keys{ std::ranges::all_of(entries, [](auto const &e) {
      return e.first.template is<lua_Integer>();
    }) };

    if (all_integer_keys) {  // Array {v1,v2,v3}, in numeric order
      std::ranges::sort(entries, {}, [](auto const &e) {
        return e.first.template as<lua_Integer>();
      });
      for (size_t i{ 0 }; i < entries.size(); ++i) {
        if (entries[i].first.as<lua_Integer>() != static_cast<lua_Integer>(i + 1)) {
          throw std::runtime_error(
              "option '" + path +
              "': integer-keyed table must be a contiguous 1..n sequence");
        }
      }

      std::string out{ "{" };
      for (size_t i{ 0 }; i < entries.size(); ++i) {
        if (i) { out += ','; }
        out += serialize_option_value(entries[i].second,
                                      path + "[" + std::to_string(i + 1) + "]");
      }
      return out + "}";
    }

    // Otherwise every key must be a string: a mixed or non-string key has no canonical
    // spelling, and dropping it silently would collide two option sets on one cache key.
    std::vector<std::pair<std::string, std::string>> sorted;
    sorted.reserve(entries.size());
    for (auto const &[key, value] : entries) {
      if (!key.is<std::string>()) {
        throw std::runtime_error(
            "option '" + path + "': table keys must be all strings or a contiguous " +
            "1..n sequence, got a " +
            std::string{ sol::type_name(key.lua_state(), key.get_type()) } + " key");
      }
      std::string name{ key.as<std::string>() };
      std::string serialized{ serialize_option_value(value, path + "." + name) };
      sorted.emplace_back(std::move(name), std::move(serialized));
    }
    std::ranges::sort(sorted);

    std::string out{ "{" };
    for (size_t i{ 0 }; i < sorted.size(); ++i) {
      if (i) { out += ','; }
      out += "[" + quote_lua_string(sorted[i].first) + "]=" + sorted[i].second;
    }
    return out + "}";
  }

  throw std::runtime_error("Unsupported Lua type in option '" + path + "'");
}

}  // namespace

pkg_cfg::pkg_cfg(ctor_tag,
                 std::string identity,
                 source_t source,
                 std::string serialized_options,
                 std::optional<pkg_phase> needed_by,
                 pkg_cfg const *parent,
                 pkg_cfg *weak,
                 std::vector<pkg_cfg *> source_dependencies,
                 std::optional<std::string> product,
                 std::filesystem::path declaring_file_path)
    : identity(std::move(identity)),
      source(std::move(source)),
      serialized_options(std::move(serialized_options)),
      needed_by(needed_by),
      parent(parent),
      weak(weak),
      source_dependencies(std::move(source_dependencies)),
      product(std::move(product)),
      declaring_file_path(std::move(declaring_file_path)) {}

std::uint64_t next_custom_fetch_decl_id() { return ++g_custom_fetch_decl_seq; }

pkg_cfg_pool *pkg_cfg::pool() { return &g_default_pkg_cfg_pool; }

pkg_cfg *pkg_cfg::parse(sol::object const &lua_val,
                        pkg_decl_origin const &origin,
                        pkg_entry_shape const shape) {
  //  "namespace.name@version" shorthand requires url or file
  if (lua_val.is<std::string>()) {
    throw std::runtime_error(
        "Spec shorthand string syntax requires table with 'url' or 'file': " +
        lua_val.as<std::string>());
  }

  if (!lua_val.is<sol::table>()) {
    throw std::runtime_error("Spec entry must be string or table");
  }

  sol::table table{ lua_val.as<sol::table>() };
  auto const rules{ rules_for(shape) };

  // Shape rules that have advice of their own run before the catch-all sweep, so the
  // author gets the specific message rather than "unknown key".
  if (shape != pkg_entry_shape::MANIFEST_PACKAGE) {
    pkg_cfg_reject_platforms(table, rules.context);
  } else if (has_value(table, "weak")) {
    // A manifest entry must carry a source, and source + weak is refused below, so
    // the key is unusable here however it is written.
    throw std::runtime_error(
        "manifest PACKAGES entries cannot be weak; declare weak references in a "
        "spec's DEPENDENCIES");
  }
  if (!rules.needed_by_allowed && has_value(table, "needed_by")) {
    throw std::runtime_error(
        "source.dependencies entry cannot specify 'needed_by': a fetch prerequisite "
        "always blocks the parent's spec_fetch. Declare it in the spec's DEPENDENCIES "
        "to couple it to a later phase.");
  }
  sol_util_reject_unknown_keys(table, rules.keys, rules.context);

  std::string serialized_options{ "{}" };
  std::optional<pkg_phase> needed_by;
  std::vector<pkg_cfg *> source_dependencies;
  pkg_cfg *weak{ nullptr };

  std::optional<std::string> product{
    sol_util_get_optional<std::string>(table, "product", "Spec")
  };
  if (product.has_value() && product->empty()) {
    throw std::runtime_error("Spec 'product' field cannot be empty");
  }

  std::optional<std::string> identity_opt{
    sol_util_get_optional<std::string>(table, "spec", "Spec")
  };
  std::string identity;
  if (!identity_opt.has_value()) {
    if (rules.source_optional && product.has_value()) {
      identity = "";
    } else {
      throw std::runtime_error("Spec table missing required 'spec' field");
    }
  } else {
    identity = *identity_opt;
    if (identity.empty()) {
      throw std::runtime_error("Spec 'spec' field cannot be empty");
    }
  }

  sol::object weak_obj{ table["weak"] };
  bool const has_weak{ weak_obj.valid() };

  sol::object source_obj{ table["source"] };
  bool const has_source{ source_obj.valid() };

  if (has_source && has_weak) {
    throw std::runtime_error("Spec cannot specify both 'source' and 'weak' fields");
  }

  bool const allow_missing_source{ rules.source_optional && !has_source };

  if (!allow_missing_source && !identity.empty() && !identity_is_valid(identity)) {
    throw std::runtime_error("Invalid spec identity format: " + identity);
  }

  source_t source;
  // Check if source is a table (custom source fetch with dependencies)
  if (has_source) {
    if (source_obj.is<sol::table>()) {
      if (!rules.fetch_function_runs) {
        throw std::runtime_error(
            std::string{ rules.context } +
            " 'source' cannot be a { fetch = ... } table: nothing can ever call that "
            "function from here. Declare the spec in another spec's DEPENDENCIES, or "
            "the bundle in a BUNDLES table.");
      }
      sol::table source_table{ source_obj.as<sol::table>() };
      source = parse_source_table(source_table, origin, source_dependencies);
    } else if (source_obj.is<std::string>()) {
      std::string source_uri{ source_obj.as<std::string>() };
      source = parse_source_string(source_uri, table, origin.anchor);
    } else {
      throw std::runtime_error("Spec 'source' field must be string or table");
    }
  } else {
    if (!rules.source_optional) {
      throw std::runtime_error("Spec must specify 'source' field");
    }
    source = pkg_cfg::weak_ref{};
  }

  // Check if options field exists and serialize it
  sol::object options_obj{ table["options"] };
  if (options_obj.valid() && options_obj.get_type() == sol::type::table) {
    // Validate options don't contain functions
    if (contains_function(options_obj)) {
      throw std::runtime_error("Unsupported Lua type: function");
    }
    // Serialize to Lua table literal
    serialized_options = serialize_option_table(options_obj);
  } else if (options_obj.valid() && options_obj.get_type() != sol::type::lua_nil) {
    throw std::runtime_error("Spec 'options' field must be table");
  }

  auto needed_by_str{ sol_util_get_optional<std::string>(table, "needed_by", "Spec") };
  if (needed_by_str.has_value()) {
    needed_by = pkg_phase_parse_needed_by(*needed_by_str, "Spec");
  }

  if (has_weak) {
    if (!weak_obj.is<sol::table>()) {
      throw std::runtime_error("Spec 'weak' field must be table");
    }
    // Weak fallback must be a strong cfg; do not allow nested weak-without-source here
    pkg_cfg *weak_cfg{ pkg_cfg::parse(weak_obj, origin, pkg_entry_shape::WEAK_FALLBACK) };
    if (weak_cfg->needed_by.has_value()) {
      throw std::runtime_error("weak fallback must not specify 'needed_by'");
    }
    weak = weak_cfg;
  }

  return pkg_cfg::pool()->emplace(std::move(identity),
                                  std::move(source),
                                  std::move(serialized_options),
                                  needed_by,
                                  nullptr,
                                  weak,
                                  std::move(source_dependencies),
                                  std::move(product),
                                  origin.declaring_file);
}

bool pkg_cfg::is_git() const { return std::holds_alternative<git_source>(source); }
bool pkg_cfg::is_local() const { return std::holds_alternative<local_source>(source); }
bool pkg_cfg::is_remote() const { return std::holds_alternative<remote_source>(source); }

bool pkg_cfg::has_fetch_function() const {
  return holds_alternative<fetch_function>(source);
}

bool pkg_cfg::is_weak_reference() const {
  return std::holds_alternative<weak_ref>(source);
}

bool pkg_cfg::is_bundle_source() const {
  return std::holds_alternative<bundle_source>(source);
}

bool pkg_cfg::is_from_bundle() const { return bundle_identity.has_value(); }

std::string pkg_cfg::serialize_option_table(sol::object const &val) {
  return serialize_option_value(val, "options");
}

std::string pkg_cfg::format_key(std::string const &identity,
                                std::string const &serialized_options) {
  if (serialized_options.empty() || serialized_options == "{}") { return identity; }
  return identity + serialized_options;
}

std::string pkg_cfg::format_key() const {
  return format_key(identity, serialized_options);
}

pkg_cfg *pkg_cfg::parse_fetch_dependency(sol::object const &entry,
                                         pkg_decl_origin const &origin) {
  pkg_cfg *cfg{ parse(entry, origin, pkg_entry_shape::FETCH_DEPENDENCY) };
  if (cfg->is_weak_reference()) {
    // A fetch prerequisite is needed at spec_fetch, but weak references resolve at
    // a resolution barrier — which waits for every spec_fetch, including that of
    // the consumer whose fetch function is waiting on this entry. Nothing can
    // satisfy it in time, so refuse rather than order it silently wrong.
    throw std::runtime_error(
        "source.dependencies entry '" +
        (cfg->product.has_value() ? "product " + *cfg->product : cfg->identity) +
        "' must be a strong reference: give the entry a 'spec' and a 'source'");
  }
  if (cfg->identity.empty()) {
    // parse(..., true) allows a bare product entry to omit 'spec'. With a source
    // present that is not a weak reference, so it would otherwise reach pkg_key and
    // fail there as an invalid identity rather than as a non-strong prerequisite.
    throw std::runtime_error("source.dependencies product '" +
                             cfg->product.value_or(std::string{}) +
                             "' must name a 'spec'");
  }
  return cfg;
}

pkg_cfg *pkg_cfg::parse_from_stack(sol::state_view lua,
                                   int index,
                                   pkg_decl_origin const &origin,
                                   pkg_entry_shape const shape) {
  sol::stack_object stack_obj{ lua, index };
  sol::object cfg_val{ stack_obj };
  return parse(cfg_val, origin, shape);
}

namespace {

// `source = { fetch = ... }` on an entry, if it has one.
std::optional<sol::protected_function> entry_source_fetch(sol::table const &entry) {
  sol::object const source{ entry["source"] };
  if (!source.is<sol::table>()) { return std::nullopt; }
  sol::object const fn{ source.as<sol::table>()["fetch"] };
  if (!fn.is<sol::function>()) { return std::nullopt; }
  return fn.as<sol::protected_function>();
}

// A bundle declaration table -- a BUNDLES alias's value or an inline `bundle = {...}`
// -- matched on its `identity` field. Same shape either way, so one reader.
std::optional<sol::protected_function> bundle_decl_fetch(sol::table const &decl,
                                                         std::string const &identity) {
  sol::object const id{ decl["identity"] };
  if (!id.is<std::string>() || id.as<std::string>() != identity) { return std::nullopt; }
  return entry_source_fetch(decl);
}

// An entry's options as the canonical string a pkg_cfg carries, so a spec query
// discriminates two option variants of one identity.
std::string entry_options(sol::table const &entry) {
  sol::object const opts{ entry["options"] };
  return opts.is<sol::table>() ? pkg_cfg::serialize_option_table(opts) : "{}";
}

std::optional<sol::protected_function> bundles_table_fetch(sol::table const &bundles,
                                                           std::string const &identity) {
  for (auto const &[key, value] : bundles) {
    if (!value.is<sol::table>()) { continue; }
    if (auto fn{ bundle_decl_fetch(value.as<sol::table>(), identity) }) { return fn; }
  }
  return std::nullopt;
}

}  // namespace

std::optional<sol::protected_function> find_fetch_function(sol::state_view lua,
                                                           fetch_fn_query const &query) {
  bool const want_bundle{ query.what == fetch_fn_query::kind::BUNDLE };

  // A spec's `source = { fetch }` lives only in DEPENDENCIES: pkg_cfg::parse refuses it
  // on a manifest PACKAGES entry, so PACKAGES is scanned for bundles alone, where the
  // fetch hangs off an inline `bundle = {...}` table instead.
  constexpr char const *kEntryGlobals[]{ "DEPENDENCIES", "PACKAGES" };
  for (size_t g{ 0 }, gn{ want_bundle ? 2u : 1u }; g < gn; ++g) {
    sol::object const arr{ lua[kEntryGlobals[g]] };
    if (!arr.is<sol::table>()) { continue; }
    sol::table const entries{ arr.as<sol::table>() };

    for (size_t i{ 1 }, n{ entries.size() }; i <= n; ++i) {
      sol::object const value{ entries[i] };
      if (!value.is<sol::table>()) { continue; }
      sol::table const entry{ value.as<sol::table>() };

      if (want_bundle) {
        sol::object const bundle{ entry["bundle"] };
        if (bundle.is<sol::table>()) {  // inline `bundle = { identity, source }`
          if (auto fn{ bundle_decl_fetch(bundle.as<sol::table>(), query.identity) }) {
            return fn;
          }
        } else if (bundle.is<std::string>() &&
                   bundle.as<std::string>() == query.identity) {
          // Pure bundle dependency: `bundle` is the identity and `source` is its own.
          if (auto fn{ entry_source_fetch(entry) }) { return fn; }
        }
        continue;
      }

      sol::object const spec{ entry["spec"] };
      if (!spec.is<std::string>() || spec.as<std::string>() != query.identity) {
        continue;
      }
      if (entry_options(entry) != query.serialized_options) { continue; }
      if (auto fn{ entry_source_fetch(entry) }) { return fn; }
    }
  }

  if (!want_bundle) { return std::nullopt; }

  // BUNDLES aliases: this state's own, then every manifest imported into it -- an
  // imported declaration never lands in the root table. The registry key the second
  // reads is absent in a spec state, so the loop is simply empty there.
  if (sol::object const own{ lua["BUNDLES"] }; own.is<sol::table>()) {
    if (auto fn{ bundles_table_fetch(own.as<sol::table>(), query.identity) }) {
      return fn;
    }
  }
  for (sol::table const &imported : lua_envy_import_bundle_tables(lua)) {
    if (auto fn{ bundles_table_fetch(imported, query.identity) }) { return fn; }
  }

  return std::nullopt;
}

std::filesystem::path pkg_cfg::compute_project_root(pkg_cfg const *cfg) {
  while (cfg && cfg->parent) { cfg = cfg->parent; }

  if (cfg && !cfg->declaring_file_path.empty()) {
    return util_canonical_path(cfg->declaring_file_path).parent_path();
  }

  return std::filesystem::current_path();
}

bool operator==(pkg_cfg::remote_source const &lhs, pkg_cfg::remote_source const &rhs) {
  return lhs.url == rhs.url && lhs.sha256 == rhs.sha256 && lhs.subdir == rhs.subdir;
}

bool operator==(pkg_cfg::local_source const &lhs, pkg_cfg::local_source const &rhs) {
  return lhs.file_path == rhs.file_path;
}

bool operator==(pkg_cfg::git_source const &lhs, pkg_cfg::git_source const &rhs) {
  return lhs.url == rhs.url && lhs.ref == rhs.ref && lhs.subdir == rhs.subdir;
}

void pkg_cfg_reject_platforms(sol::table const &table, std::string_view context) {
  if (!has_value(table, "platforms")) { return; }
  throw std::runtime_error(std::string{ context } +
                           " cannot specify 'platforms': platform filtering is a "
                           "manifest PACKAGES field. A dependency exists because "
                           "something on this platform asked for it.");
}

void pkg_cfg_reject_option_variants(std::vector<pkg_cfg *> const &deps,
                                    std::string const &context) {
  std::unordered_map<std::string, pkg_cfg const *> seen;
  for (auto const *dep : deps) {
    if (dep->identity.empty()) { continue; }  // Bare product entry: no identity to key on
    auto const [it, inserted]{ seen.emplace(dep->identity, dep) };
    if (inserted || it->second->serialized_options == dep->serialized_options) {
      continue;
    }
    throw std::runtime_error(
        context + " names '" + dep->identity + "' twice with different options: " +
        it->second->serialized_options + " and " + dep->serialized_options);
  }
}

pkg_source_match bundle_source_compare(pkg_cfg::bundle_source const &lhs,
                                          pkg_cfg::bundle_source const &rhs) {
  if (lhs.bundle_identity != rhs.bundle_identity) {
    return pkg_source_match::DIFFERENT;
  }
  if (lhs.fetch_source.index() != rhs.fetch_source.index()) {
    return pkg_source_match::DIFFERENT;
  }

  using fetch_source_t = decltype(pkg_cfg::bundle_source::fetch_source);
  static_assert(std::is_same_v<std::variant_alternative_t<0, fetch_source_t>,
                               pkg_cfg::remote_source>);
  static_assert(std::is_same_v<std::variant_alternative_t<1, fetch_source_t>,
                               pkg_cfg::local_source>);
  static_assert(
      std::is_same_v<std::variant_alternative_t<2, fetch_source_t>, pkg_cfg::git_source>);
  static_assert(std::is_same_v<std::variant_alternative_t<3, fetch_source_t>,
                               pkg_cfg::custom_fetch_source>);
  static_assert(std::variant_size_v<fetch_source_t> == 4);

  auto const same{ [](bool equal) {
    return equal ? pkg_source_match::SAME : pkg_source_match::DIFFERENT;
  } };

  switch (lhs.fetch_source.index()) {
    case 0:
      return same(std::get<pkg_cfg::remote_source>(lhs.fetch_source) ==
                  std::get<pkg_cfg::remote_source>(rhs.fetch_source));
    case 1:
      return same(std::get<pkg_cfg::local_source>(lhs.fetch_source) ==
                  std::get<pkg_cfg::local_source>(rhs.fetch_source));
    case 2:
      return same(std::get<pkg_cfg::git_source>(lhs.fetch_source) ==
                  std::get<pkg_cfg::git_source>(rhs.fetch_source));
    // Two fetch closures are opaque unless both sides are copies of one parsed
    // declaration — an alias or a prior entry named twice is still one closure.
    default:
      return std::get<pkg_cfg::custom_fetch_source>(lhs.fetch_source).decl_id ==
                     std::get<pkg_cfg::custom_fetch_source>(rhs.fetch_source).decl_id
                 ? pkg_source_match::SAME
                 : pkg_source_match::INCOMPARABLE;
  }
}

pkg_source_match pkg_cfg_source_compare(pkg_cfg::source_t const &lhs,
                                        pkg_cfg::source_t const &rhs) {
  // A reference-only entry names no payload, so it agrees with whatever concrete
  // declaration wins the key -- that is exactly what resolving it to one means.
  if (std::holds_alternative<pkg_cfg::weak_ref>(lhs) ||
      std::holds_alternative<pkg_cfg::weak_ref>(rhs)) {
    return pkg_source_match::SAME;
  }
  if (lhs.index() != rhs.index()) { return pkg_source_match::DIFFERENT; }

  auto const same{ [](bool equal) {
    return equal ? pkg_source_match::SAME : pkg_source_match::DIFFERENT;
  } };

  return std::visit(
      match{
          [&](pkg_cfg::remote_source const &a) {
            return same(a == std::get<pkg_cfg::remote_source>(rhs));
          },
          [&](pkg_cfg::local_source const &a) {
            return same(a == std::get<pkg_cfg::local_source>(rhs));
          },
          [&](pkg_cfg::git_source const &a) {
            return same(a == std::get<pkg_cfg::git_source>(rhs));
          },
          // A Lua closure has no fingerprint; both were written to produce this spec.
          [](pkg_cfg::fetch_function const &) { return pkg_source_match::INCOMPARABLE; },
          [](pkg_cfg::weak_ref const &) { return pkg_source_match::SAME; },
          [&](pkg_cfg::bundle_source const &a) {
            return bundle_source_compare(a, std::get<pkg_cfg::bundle_source>(rhs));
          },
      },
      lhs);
}

}  // namespace envy
