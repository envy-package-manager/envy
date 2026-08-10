#include "pkg_cfg.h"

#include "sol_util.h"
#include "uri.h"
#include "util.h"

#include <algorithm>
#include <charconv>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace envy {

namespace {

pkg_cfg_pool g_default_pkg_cfg_pool;

bool parse_identity(std::string const &identity,
                    std::string &out_namespace,
                    std::string &out_name,
                    std::string &out_version) {
  if (!util_is_safe_path_component(identity)) { return false; }

  auto const at_pos{ identity.find('@') };
  if (at_pos == std::string::npos || at_pos == 0 || at_pos == identity.size() - 1) {
    return false;
  }

  auto const dot_pos{ identity.find('.') };
  if (dot_pos == std::string::npos || dot_pos == 0 || dot_pos >= at_pos) { return false; }

  out_namespace = identity.substr(0, dot_pos);
  out_name = identity.substr(dot_pos + 1, at_pos - dot_pos - 1);
  out_version = identity.substr(at_pos + 1);

  return !out_namespace.empty() && !out_name.empty() && !out_version.empty();
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

// Parse source table (custom source fetch with dependencies)
pkg_cfg::source_t parse_source_table(sol::table const &source_table,
                                     std::filesystem::path const &base_path,
                                     std::vector<pkg_cfg *> &out_dependencies,
                                     bool allow_weak_without_source) {
  // Check for dependencies field (need to parse as array)
  bool has_dependencies{ false };
  sol::object deps_obj{ source_table["dependencies"] };
  if (deps_obj.valid()) {
    if (deps_obj.is<sol::table>()) {
      sol::table deps_table{ deps_obj.as<sol::table>() };
      has_dependencies = true;
      for (size_t i{ 1 }, n{ deps_table.size() }; i <= n; ++i) {
        pkg_cfg *dep_cfg{ pkg_cfg::parse(deps_table[i], base_path, true) };
        out_dependencies.push_back(dep_cfg);
      }
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
        "source table must have either URL string or dependencies+fetch function");
  }

  return pkg_cfg::fetch_function{};  // Custom source fetch - no URL-based source
}

// Parse source string (URI-based sources)
pkg_cfg::source_t parse_source_string(std::string const &source_uri,
                                      sol::table const &table,
                                      std::filesystem::path const &base_path) {
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
        p = base_path.parent_path() / p;
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
                     ? (base_path.parent_path() / info.canonical).lexically_normal()
                     : std::filesystem::path{ info.canonical }
  };
}

}  // namespace

pkg_cfg_pool *pkg_cfg::pool_ = &g_default_pkg_cfg_pool;

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

void pkg_cfg::set_pool(pkg_cfg_pool *pool) {
  pool_ = pool ? pool : &g_default_pkg_cfg_pool;
}

pkg_cfg_pool *pkg_cfg::pool() { return pool_; }

pkg_cfg *pkg_cfg::parse(sol::object const &lua_val,
                        std::filesystem::path const &base_path,
                        bool const allow_weak_without_source) {
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
    if (allow_weak_without_source && product.has_value()) {
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

  bool const allow_missing_source{ allow_weak_without_source && !has_source };

  std::string ns, name, ver;
  if (!allow_missing_source) {
    if (!identity.empty() && !parse_identity(identity, ns, name, ver)) {
      throw std::runtime_error("Invalid spec identity format: " + identity);
    }
  }

  source_t source;
  // Check if source is a table (custom source fetch with dependencies)
  if (has_source) {
    if (source_obj.is<sol::table>()) {
      sol::table source_table{ source_obj.as<sol::table>() };
      source = parse_source_table(source_table,
                                  base_path,
                                  source_dependencies,
                                  allow_weak_without_source);
    } else if (source_obj.is<std::string>()) {
      std::string source_uri{ source_obj.as<std::string>() };
      source = parse_source_string(source_uri, table, base_path);
    } else {
      throw std::runtime_error("Spec 'source' field must be string or table");
    }
  } else {
    if (!allow_weak_without_source) {
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
    pkg_cfg *weak_cfg{ pkg_cfg::parse(weak_obj, base_path, false) };
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
                                  base_path);
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

  if (val.is<std::string>()) {
    auto const &str{ val.as<std::string>() };
    std::string result;
    result.reserve(str.size() + 2);
    result += '"';
    for (char c : str) {
      if (c == '"' || c == '\\') { result += '\\'; }
      result += c;
    }
    result += '"';
    return result;
  }

  if (val.is<sol::table>()) {
    sol::table table{ val.as<sol::table>() };
    if (table.empty()) { return "{}"; }

    // Collect all key-value pairs
    std::vector<std::pair<sol::object, sol::object>> entries;
    for (auto const &[key, value] : table) { entries.emplace_back(key, value); }

    // Check if this is an array (all keys are consecutive integers 1..n)
    bool is_array{ !entries.empty() };
    if (is_array) {
      // Verify all keys are integers
      for (auto const &entry : entries) {
        if (!entry.first.is<lua_Integer>()) {
          is_array = false;
          break;
        }
      }

      if (is_array) {  // Sort by numeric key
        std::ranges::sort(entries,
                          [](std::pair<sol::object, sol::object> const &a,
                             std::pair<sol::object, sol::object> const &b) {
                            return a.first.template as<lua_Integer>() <
                                   b.first.template as<lua_Integer>();
                          });

        for (size_t i = 0; i < entries.size(); ++i) {  // Verify keys are contiguous
          if (entries[i].first.as<lua_Integer>() != static_cast<lua_Integer>(i + 1)) {
            is_array = false;
            break;
          }
        }
      }
    }

    if (is_array) {  // Serialize as array {val1,val2,val3} - maintain numeric order
      std::ostringstream oss;
      oss << '{';
      bool first{ true };
      for (auto const &entry : entries) {
        if (!first) { oss << ','; }
        oss << serialize_option_table(entry.second);
        first = false;
      }
      oss << '}';
      return oss.str();
    } else {  // Serialize as table {key1=val1,key2=val2} - sort by string key
      std::vector<std::pair<std::string, std::string>> sorted;
      for (auto const &entry : entries) {
        if (entry.first.is<std::string>()) {
          sorted.emplace_back(entry.first.as<std::string>(),
                              pkg_cfg::serialize_option_table(entry.second));
        }
      }
      std::ranges::sort(sorted);

      std::ostringstream oss;
      oss << '{';
      bool first{ true };
      for (auto const &[key, serialized_val] : sorted) {
        if (!first) { oss << ','; }
        oss << key << '=' << serialized_val;
        first = false;
      }
      oss << '}';
      return oss.str();
    }
  }

  throw std::runtime_error("Unsupported Lua type in serialize_option_table");
}

std::string pkg_cfg::format_key(std::string const &identity,
                                std::string const &serialized_options) {
  if (serialized_options.empty() || serialized_options == "{}") { return identity; }
  return identity + serialized_options;
}

std::string pkg_cfg::format_key() const {
  return format_key(identity, serialized_options);
}

pkg_cfg *pkg_cfg::parse_from_stack(sol::state_view lua,
                                   int index,
                                   std::filesystem::path const &base_path,
                                   bool const allow_weak_without_source) {
  sol::stack_object stack_obj{ lua, index };
  sol::object cfg_val{ stack_obj };
  return parse(cfg_val, base_path, allow_weak_without_source);
}

std::optional<sol::protected_function> pkg_cfg::get_source_fetch(
    sol::state_view lua,
    std::string const &dep_identity) {
  sol::object deps_obj{ lua["DEPENDENCIES"] };
  if (!deps_obj.valid() || !deps_obj.is<sol::table>()) { return std::nullopt; }

  sol::table deps_table{ deps_obj.as<sol::table>() };

  for (size_t i{ 1 }, n{ deps_table.size() }; i <= n; ++i) {
    if (!deps_table[i].is<sol::table>()) { continue; }
    sol::table dep_table{ deps_table.get<sol::table>(i) };

    sol::object spec_obj{ dep_table["spec"] };
    if (!spec_obj.valid() || !spec_obj.is<std::string>()) { continue; }

    if (spec_obj.as<std::string>() == dep_identity) {
      sol::object source_obj{ dep_table["source"] };
      if (!source_obj.valid() || !source_obj.is<sol::table>()) { return std::nullopt; }

      sol::table source_table{ source_obj.as<sol::table>() };
      sol::object fetch_obj{ source_table["fetch"] };
      if (!fetch_obj.valid() || !fetch_obj.is<sol::function>()) { return std::nullopt; }

      return fetch_obj.as<sol::protected_function>();
    }
  }

  return std::nullopt;
}

std::optional<sol::protected_function> pkg_cfg::get_bundle_fetch(
    sol::state_view lua,
    std::string const &bundle_identity) {
  sol::object deps_obj{ lua["DEPENDENCIES"] };
  if (!deps_obj.valid() || !deps_obj.is<sol::table>()) { return std::nullopt; }

  sol::table deps_table{ deps_obj.as<sol::table>() };

  for (size_t i{ 1 }, n{ deps_table.size() }; i <= n; ++i) {
    if (!deps_table[i].is<sol::table>()) { continue; }
    sol::table dep_table{ deps_table.get<sol::table>(i) };

    sol::object bundle_obj{ dep_table["bundle"] };
    if (!bundle_obj.valid() || !bundle_obj.is<std::string>()) { continue; }

    if (bundle_obj.as<std::string>() == bundle_identity) {
      sol::object source_obj{ dep_table["source"] };
      if (!source_obj.valid() || !source_obj.is<sol::table>()) { return std::nullopt; }

      sol::table source_table{ source_obj.as<sol::table>() };
      sol::object fetch_obj{ source_table["fetch"] };
      if (!fetch_obj.valid() || !fetch_obj.is<sol::function>()) { return std::nullopt; }

      return fetch_obj.as<sol::protected_function>();
    }
  }

  return std::nullopt;
}

std::filesystem::path pkg_cfg::compute_project_root(pkg_cfg const *cfg) {
  while (cfg && cfg->parent) { cfg = cfg->parent; }

  if (cfg && !cfg->declaring_file_path.empty()) {
    std::filesystem::path const abs{ std::filesystem::absolute(cfg->declaring_file_path) };
    std::error_code ec;
    std::filesystem::path const canonical{ std::filesystem::weakly_canonical(abs, ec) };
    return (ec ? abs : canonical).parent_path();
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

bundle_source_match bundle_source_compare(pkg_cfg::bundle_source const &lhs,
                                          pkg_cfg::bundle_source const &rhs) {
  if (lhs.bundle_identity != rhs.bundle_identity) {
    return bundle_source_match::DIFFERENT;
  }
  if (lhs.fetch_source.index() != rhs.fetch_source.index()) {
    return bundle_source_match::DIFFERENT;
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
    return equal ? bundle_source_match::SAME : bundle_source_match::DIFFERENT;
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
    // Two fetch closures are opaque: not the same, not provably different.
    default: return bundle_source_match::INCOMPARABLE;
  }
}

}  // namespace envy
