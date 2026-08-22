#include "manifest.h"

#include "bundle.h"
#include "engine.h"
#include "envy_release.h"
#include "lua_envy.h"
#include "lua_shell.h"
#include "shell.h"
#include "sol_util.h"
#include "tui.h"
#include "util.h"

#include <cstring>
#include <stdexcept>

namespace envy {

namespace {

size_t skip_whitespace(std::string_view s, size_t pos) {
  while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) { ++pos; }
  return pos;
}

std::string_view parse_identifier(std::string_view s, size_t &pos) {
  size_t const start{ pos };
  while ((pos < s.size()) && (std::isalnum(static_cast<unsigned char>(s[pos])) ||
                              s[pos] == '_' || s[pos] == '-')) {
    ++pos;
  }
  return s.substr(start, pos - start);
}

std::optional<bool> parse_bool_value(std::string_view value) {
  if (value == "true") { return true; }
  if (value == "false") { return false; }
  return std::nullopt;
}

// Expects pos to be at opening quote, advances pos past closing quote
std::optional<std::string> parse_quoted_value(std::string_view s, size_t &pos) {
  if (pos >= s.size() || s[pos] != '"') { return std::nullopt; }
  ++pos;  // skip opening quote

  std::string result;
  while (pos < s.size() && s[pos] != '"') {
    if (s[pos] == '\\' && pos + 1 < s.size()) {
      char const next{ s[pos + 1] };
      if (next == '"' || next == '\\') {
        result += next;
        pos += 2;
        continue;
      }
    }

    result += s[pos];
    ++pos;
  }

  if (pos >= s.size() || s[pos] != '"') { return std::nullopt; }
  ++pos;  // skip closing quote
  return result;
}

// One directive line's contents, plus where its value's bytes sit within the line. The
// offsets exist so a rewriter can splice a new value in without re-deriving the grammar.
struct directive_line {
  std::string_view key;
  std::string value;
  size_t value_begin{};  // first byte inside the opening quote
  size_t value_end{};    // the closing quote
};

// Parse a single line for @envy directive
// Returns key, value and value span if found, nullopt otherwise
std::optional<directive_line> parse_directive_line(std::string_view line) {
  size_t pos{ 0 };

  pos = skip_whitespace(line, pos);

  // Must start with "--"
  if (pos + 2 > line.size() || line[pos] != '-' || line[pos + 1] != '-') {
    return std::nullopt;
  }
  pos += 2;

  pos = skip_whitespace(line, pos);

  // Must have "@envy"
  if (pos + 5 > line.size()) { return std::nullopt; }
  if (line.substr(pos, 5) != "@envy") { return std::nullopt; }
  pos += 5;

  if (pos >= line.size() || (line[pos] != ' ' && line[pos] != '\t')) {
    return std::nullopt;
  }
  pos = skip_whitespace(line, pos);

  auto const key{ parse_identifier(line, pos) };
  if (key.empty()) { return std::nullopt; }

  pos = skip_whitespace(line, pos);

  // Taken before the parse: parse_quoted_value unescapes as it goes, so its result can be
  // shorter than the bytes it consumed. Meaningless if the parse fails, discarded with it.
  size_t const value_begin{ pos + 1 };
  auto value{ parse_quoted_value(line, pos) };
  if (!value) { return std::nullopt; }

  return directive_line{ key, std::move(*value), value_begin, pos - 1 };
}

// The one walk over a manifest's '@envy' header. Reading and rewriting both go through it,
// so a rewriter cannot edit a line the parser does not read.
template <typename fn>
void scan_envy_header(std::string_view content, fn const &on_directive) {
  for (size_t line_start{ 0 }; line_start < content.size();) {
    size_t const nl{ content.find('\n', line_start) };
    size_t const line_end{ nl == std::string_view::npos ? content.size() : nl };
    auto const line{ content.substr(line_start, line_end - line_start) };

    if (auto const d{ parse_directive_line(line) }) {
      on_directive(d->key,
                   d->value,
                   envy_directive_span{ .line_begin = line_start,
                                        .line_end = line_end,
                                        .value_begin = line_start + d->value_begin,
                                        .value_end = line_start + d->value_end });
    } else if (auto const body{ line.find_first_not_of(" \t\r") };
               body != std::string_view::npos && line.compare(body, 2, "--") != 0) {
      break;  // first line of code ends the header; a directive below it is not one
    }

    if (nl == std::string_view::npos) { break; }
    line_start = nl + 1;
  }
}

using bundle_alias_map = std::unordered_map<std::string, pkg_cfg::bundle_source>;
using bundle_pkg_map = std::unordered_map<std::string, pkg_cfg *>;

// Parse optional `setup` field: array of SETUP pair names to select.
// Selection is explicit-only and unions across referrers (manifest entries here;
// dependency entries parse theirs in phase_spec_fetch).
void parse_setup_field(sol::table const &table, pkg_cfg *cfg) {
  sol::object setup_obj{ table["setup"] };
  if (!setup_obj.valid() || setup_obj.get_type() == sol::type::lua_nil) { return; }
  if (setup_obj.get_type() != sol::type::table) {
    throw std::runtime_error("Package 'setup' field must be a table of pair names");
  }

  std::vector<std::string> names;
  sol::table t{ setup_obj.as<sol::table>() };
  for (size_t i{ 1 }; i <= t.size(); ++i) {
    sol::object elem{ t[i] };
    if (!elem.is<std::string>() || elem.as<std::string>().empty()) {
      throw std::runtime_error("Package 'setup' entries must be non-empty strings");
    }
    names.push_back(elem.as<std::string>());
  }
  cfg->setup = std::move(names);
}

// Parse a single package entry that may reference a bundle
pkg_cfg *parse_package_entry(sol::object const &entry,
                             std::filesystem::path const &manifest_path,
                             bundle_alias_map const &bundles,
                             bundle_pkg_map &bundle_pkgs) {
  // For non-table entries (strings) - use standard parsing (no platforms possible)
  if (!entry.is<sol::table>()) { return pkg_cfg::parse(entry, manifest_path); }

  sol::table table{ entry.as<sol::table>() };

  // Check for bundle field
  sol::object bundle_obj{ table["bundle"] };
  if (!bundle_obj.valid() || bundle_obj.get_type() == sol::type::lua_nil) {
    // No bundle field - use standard pkg_cfg::parse, then add platforms
    pkg_cfg *cfg{ pkg_cfg::parse(entry, manifest_path) };
    sol::object platforms_obj{ table["platforms"] };
    if (platforms_obj.valid() && platforms_obj.get_type() != sol::type::lua_nil) {
      if (platforms_obj.get_type() != sol::type::table) {
        throw std::runtime_error("platforms must be a table");
      }
      sol::table plat_table{ platforms_obj.as<sol::table>() };
      for (size_t j{ 1 }; j <= plat_table.size(); ++j) {
        auto elem{ plat_table[j] };
        if (!elem.template is<std::string>()) {
          throw std::runtime_error("platforms entries must be strings");
        }
        cfg->platforms.push_back(elem.template get<std::string>());
      }
    }
    parse_setup_field(table, cfg);
    return cfg;
  }

  // Has bundle field - need to handle bundle reference
  std::string const spec_identity{ [&] {
    auto opt{ sol_util_get_optional<std::string>(table, "spec", "Package") };
    if (!opt.has_value() || opt->empty()) {
      throw std::runtime_error("Package with 'bundle' field requires 'spec' field");
    }
    return std::move(*opt);
  }() };

  // Check for source field - can't have both source and bundle
  if (sol::object source_obj{ table["source"] };
      source_obj.valid() && source_obj.get_type() != sol::type::lua_nil) {
    throw std::runtime_error("Package cannot specify both 'source' and 'bundle' fields");
  }

  pkg_cfg::bundle_source const bundle_src{ [&]() -> pkg_cfg::bundle_source {
    if (bundle_obj.is<std::string>()) {
      std::string const &alias{ bundle_obj.as<std::string>() };
      auto it{ bundles.find(alias) };
      if (it == bundles.end()) {
        throw std::runtime_error("Bundle alias '" + alias +
                                 "' not found in BUNDLES table for spec '" +
                                 spec_identity + "'");
      }
      return it->second;
    }
    if (bundle_obj.is<sol::table>()) {
      return bundle::parse_inline(bundle_obj.as<sol::table>(), manifest_path);
    }
    throw std::runtime_error("Package 'bundle' field must be string (alias) or table");
  }() };

  // Parse optional fields
  std::string serialized_options{ "{}" };
  sol::object options_obj{ table["options"] };
  if (options_obj.valid() && options_obj.get_type() == sol::type::table) {
    serialized_options = pkg_cfg::serialize_option_table(options_obj);
  }

  std::optional<pkg_phase> needed_by;
  auto needed_by_str{ sol_util_get_optional<std::string>(table, "needed_by", "Package") };
  if (needed_by_str.has_value()) {
    needed_by = pkg_phase_parse_needed_by(*needed_by_str, "Package");
  }

  std::optional<std::string> product{
    sol_util_get_optional<std::string>(table, "product", "Package")
  };

  std::string const bundle_identity{ bundle_src.bundle_identity };

  // The bundle itself is a package; depend on it so it materializes (and reports
  // its own row) before this spec's spec_fetch reads a spec out of it.
  std::vector<pkg_cfg *> source_deps{
    bundle::ensure_pkg_cfg(bundle_src, manifest_path, nullptr, bundle_pkgs)
  };

  // Create pkg_cfg with bundle source
  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(spec_identity,
                                         std::move(bundle_src),
                                         std::move(serialized_options),
                                         needed_by,
                                         nullptr,  // parent
                                         nullptr,  // weak
                                         std::move(source_deps),
                                         std::move(product),
                                         manifest_path) };

  // Set bundle-related fields
  cfg->bundle_identity = bundle_identity;
  // bundle_path will be resolved later when the bundle is fetched and parsed

  // Parse optional platforms field
  sol::object platforms_obj{ table["platforms"] };
  if (platforms_obj.valid() && platforms_obj.get_type() != sol::type::lua_nil) {
    if (platforms_obj.get_type() != sol::type::table) {
      throw std::runtime_error("platforms must be a table");
    }
    sol::table plat_table{ platforms_obj.as<sol::table>() };
    for (size_t j{ 1 }; j <= plat_table.size(); ++j) {
      auto elem{ plat_table[j] };
      if (!elem.template is<std::string>()) {
        throw std::runtime_error("platforms entries must be strings");
      }
      cfg->platforms.push_back(elem.template get<std::string>());
    }
  }

  parse_setup_field(table, cfg);
  return cfg;
}

// Parse the optional PACKAGE_DEPOTS global: list of URI strings and/or
// { DEPENDS = {...}, FETCH = function } tables.
std::vector<manifest::depot_source> parse_package_depots(sol::object const &depots_obj) {
  std::vector<manifest::depot_source> depots;

  if (!depots_obj.valid() || depots_obj.get_type() == sol::type::lua_nil) {
    return depots;
  }
  if (depots_obj.get_type() != sol::type::table) {
    throw std::runtime_error(
        "PACKAGE_DEPOTS must be a table of URI strings or {DEPENDS, FETCH} tables");
  }

  sol::table t{ depots_obj.as<sol::table>() };
  for (size_t i{ 1 }; i <= t.size(); ++i) {
    std::string const label{ "PACKAGE_DEPOTS[" + std::to_string(i) + "]" };
    sol::object entry{ t[i] };

    if (entry.is<std::string>()) {
      std::string url{ entry.as<std::string>() };
      if (url.empty()) {
        throw std::runtime_error(label + " must be a non-empty URI string");
      }
      depots.push_back(manifest::depot_uri{ std::move(url) });
      continue;
    }

    if (entry.get_type() != sol::type::table) {
      throw std::runtime_error(label +
                               " must be a URI string or a {DEPENDS, FETCH} table");
    }

    sol::table et{ entry.as<sol::table>() };
    if (sol::object fetch_obj{ et["FETCH"] };
        !fetch_obj.valid() || fetch_obj.get_type() != sol::type::function) {
      throw std::runtime_error(label + " requires a FETCH function");
    }

    manifest::depot_fetch_fn fn{ .depends = {}, .lua_index = i };
    if (sol::object dep_obj{ et["DEPENDS"] };
        dep_obj.valid() && dep_obj.get_type() != sol::type::lua_nil) {
      if (dep_obj.get_type() != sol::type::table) {
        throw std::runtime_error(label + " DEPENDS must be a table of package identities");
      }
      sol::table dt{ dep_obj.as<sol::table>() };
      for (size_t j{ 1 }; j <= dt.size(); ++j) {
        sol::object d{ dt[j] };
        if (!d.is<std::string>() || d.as<std::string>().empty()) {
          throw std::runtime_error(label + " DEPENDS entries must be non-empty strings");
        }
        fn.depends.push_back(d.as<std::string>());
      }
    }
    depots.push_back(std::move(fn));
  }

  return depots;
}

}  // namespace

std::optional<std::string> const &envy_meta::cache_for_platform() const {
#ifdef _WIN32
  return cache_win;
#else
  return cache_posix;
#endif
}

envy_meta parse_envy_meta(std::string_view content) {
  envy_meta result;

  scan_envy_header(
      content,
      [&result](std::string_view key,
                std::string const &value,
                envy_directive_span const &) {
        if (key == "version") {
          result.version = value;
        } else if (key == "cache-posix") {
          result.cache_posix = value;
        } else if (key == "cache-win") {
          result.cache_win = value;
        } else if (key == "mirror") {
          result.mirror = value;
        } else if (key == "sha256sums") {
          if (!envy_release_sha256_hex_is_valid(value)) {
            throw std::runtime_error(
                "'@envy sha256sums' must be exactly 64 hex digits (the sha256 "
                "of the release's SHA256SUMS file), got: '" +
                value + "'");
          }
          result.sha256sums = value;
        } else if (key == "bin" || key == "bin-dir") {
          result.bin = value;
        } else if (key == "schema") {
          try {
            if (int const v{ std::stoi(value) }; v >= 1) { result.schema = v; }
          } catch (...) {}
        } else if (key == "deploy") {
          result.deploy = parse_bool_value(value);
        } else if (key == "root") {
          result.root = parse_bool_value(value);
        } else if (key == "package-depot") {
          throw std::runtime_error(
              "'@envy package-depot' directive removed; declare a "
              "PACKAGE_DEPOTS global in the manifest instead, e.g.: "
              "PACKAGE_DEPOTS = { \"" +
              value + "\" }");
        }
      });

  // A sums pin names one release's checksum file, so it is meaningless without the version
  // that selects that release: with the version resolved dynamically (cache `latest`, the
  // mirror's `latest`, GitHub's redirect, or the script's stamped fallback) the pin would
  // describe a different release than the one being downloaded. Fail closed rather than
  // silently skip verification -- a pin that quietly stops verifying is worse than none.
  if (result.sha256sums && !result.version) {
    throw std::runtime_error(
        "'@envy sha256sums' requires '@envy version': a sums pin identifies one release, "
        "so the version cannot be left to dynamic resolution");
  }

  // Deliberately no envy_release_validate_mirror here. That check belongs to the write
  // side, `envy init --mirror`, where a user-supplied value first enters the manifest.
  // Enforcing it on read rejects manifests that already work: a Windows `file://C:\...` or
  // UNC mirror carries backslashes, and refusing those breaks every manifest-aware command
  // for that project. The newline case the character set exists for cannot occur here
  // anyway -- directives are matched per line, so a parsed value never contains one.

  return result;
}

std::optional<envy_directive_span> find_envy_directive(std::string_view content,
                                                       std::string_view key) {
  std::optional<envy_directive_span>
      found;  // last match wins, as the assignments above do
  scan_envy_header(
      content,
      [&found,
       key](std::string_view k, std::string const &, envy_directive_span const &span) {
        if (k == key) { found = span; }
      });
  return found;
}

namespace {

// One read, reused: the directives are parsed out of the same bytes a caller goes on to
// hand to Lua, and the scan stops at the manifest's first line of code.
manifest::discovery read_manifest(std::filesystem::path manifest_path) {
  auto content{ util_load_file(manifest_path) };
  auto meta{ parse_envy_meta(
      { reinterpret_cast<char const *>(content.data()), content.size() }) };
  return { std::move(manifest_path), std::move(meta), std::move(content) };
}

}  // namespace

std::optional<manifest::discovery> manifest::discover(
    bool nearest,
    std::filesystem::path const &start_dir) {
  namespace fs = std::filesystem;

  // Non-root manifests encountered during the search. Each was read to reach its root
  // directive, so the winner's bytes are carried out rather than read a second time.
  std::vector<discovery> candidates;
  auto cur{ start_dir };

  auto const closest_to_root{ [&]() -> std::optional<discovery> {
    if (candidates.empty()) { return std::nullopt; }
    return std::move(candidates.back());
  } };

  for (;;) {
    auto const manifest_path{ cur / "envy.lua" };
    if (fs::exists(manifest_path)) {
      auto found{ read_manifest(manifest_path) };

      // In nearest (subproject) mode, return the first envy.lua found
      if (nearest) { return found; }

      // Default root=true (stops search); root=false continues upward
      if (!found.meta.root.has_value() || *found.meta.root) { return found; }

      // Non-root manifest: remember and continue searching
      candidates.push_back(std::move(found));
    }

    auto const git_path{ cur / ".git" };
    if (fs::exists(git_path) && fs::is_directory(git_path)) {
      return closest_to_root();  // .git boundary
    }

    auto const parent{ cur.parent_path() };
    if (parent == cur) { return closest_to_root(); }  // filesystem root

    cur = parent;
  }
}

std::filesystem::path manifest::find_manifest_path(
    std::optional<std::filesystem::path> const &explicit_path,
    bool nearest) {
  if (explicit_path) {
    auto const path{ std::filesystem::absolute(*explicit_path) };
    if (!std::filesystem::exists(path)) {
      throw std::runtime_error("manifest not found: " + path.string());
    }
    return path;
  } else {
    if (auto const discovered{ discover(nearest, std::filesystem::current_path()) }) {
      return discovered->path;
    }
    throw std::runtime_error("manifest not found (discovery failed)");
  }
}

std::unique_ptr<manifest> manifest::find_and_load(
    std::optional<std::filesystem::path> const &explicit_path,
    bool nearest) {
  if (explicit_path) { return load(find_manifest_path(explicit_path, nearest)); }

  auto const found{ discover(nearest, std::filesystem::current_path()) };
  if (!found) { throw std::runtime_error("manifest not found (discovery failed)"); }

  return load(found->content, found->path);  // discovery already read the file
}

std::unique_ptr<manifest> manifest::load(std::filesystem::path const &manifest_path) {
  tui::debug("Loading manifest from file: %s", manifest_path.string().c_str());
  return load(util_load_file(manifest_path), manifest_path);
}

std::unique_ptr<manifest> manifest::load(std::vector<unsigned char> const &content,
                                         std::filesystem::path const &manifest_path) {
  tui::debug("Loading manifest (%zu bytes)", content.size());
  // Ensure null-termination for Lua (create string with guaranteed null terminator)
  std::string const script{ reinterpret_cast<char const *>(content.data()),
                            content.size() };

  auto meta{ parse_envy_meta(script) };

  if (!meta.bin) {
    throw std::runtime_error(
        "Manifest missing required '@envy bin' directive.\n"
        "Add to manifest header, e.g.: -- @envy bin \"tools\"");
  }

  auto state{ sol_util_make_lua_state() };
  lua_envy_install(*state);

  // Use manifest path as chunk name so debug.getinfo can find it for envy.loadenv()
  std::string const chunk_name{ "@" + manifest_path.string() };
  if (sol::protected_function_result const result{
          state->safe_script(script, sol::script_pass_on_error, chunk_name) };
      !result.valid()) {
    sol::error err = result;
    throw std::runtime_error(std::string("Failed to execute manifest script: ") +
                             err.what());
  }

  auto m{ std::make_unique<manifest>() };
  m->manifest_path = manifest_path;
  m->meta = std::move(meta);
  m->lua_ = std::move(state);  // Keep lua state alive for DEFAULT_SHELL access

  auto const bundles{ bundle::parse_aliases((*m->lua_)["BUNDLES"], manifest_path) };

  // Bundles become BUNDLE_ONLY packages (see bundle::ensure_pkg_cfg). Custom-fetch
  // bundles are also roots: their fetch function lives in this manifest's BUNDLES
  // table, so they run whether or not a package references them. Every other bundle
  // is created on first reference below and pulled in as a source dependency.
  std::unordered_map<std::string, pkg_cfg *> bundle_pkgs;
  for (auto const &[alias, bundle_src] : bundles) {
    if (!std::holds_alternative<pkg_cfg::custom_fetch_source>(bundle_src.fetch_source)) {
      continue;
    }
    m->packages.push_back(
        bundle::ensure_pkg_cfg(bundle_src, manifest_path, nullptr, bundle_pkgs));
  }

  sol::object packages_obj = (*m->lua_)["PACKAGES"];
  if (!packages_obj.valid() || packages_obj.get_type() != sol::type::table) {
    throw std::runtime_error("Manifest must define 'PACKAGES' global as a table");
  }

  sol::table packages_table = packages_obj.as<sol::table>();

  for (size_t i{ 1 }; i <= packages_table.size(); ++i) {
    m->packages.push_back(
        parse_package_entry(packages_table[i], manifest_path, bundles, bundle_pkgs));
  }

  m->package_depots = parse_package_depots((*m->lua_)["PACKAGE_DEPOTS"]);

  return m;
}

std::unique_ptr<manifest> manifest::load(char const *script,
                                         std::filesystem::path const &manifest_path) {
  tui::debug("Loading manifest from C string");
  return load(std::vector<unsigned char>(script, script + std::strlen(script)),
              manifest_path);
}

default_shell_cfg_t manifest::get_default_shell() const {
  if (!lua_) { return std::nullopt; }

  sol::object default_shell_obj{ (*lua_)["DEFAULT_SHELL"] };
  if (!default_shell_obj.valid()) { return std::nullopt; }

  // Helper to convert flat variant to nested variant structure
  auto const convert_parsed{ [](resolved_shell const &parsed) -> default_shell_value {
    return std::visit(match{ [](shell_choice c) -> default_shell_value { return c; },
                             [](custom_shell_file const &f) -> default_shell_value {
                               return custom_shell{ f };
                             },
                             [](custom_shell_inline const &i) -> default_shell_value {
                               return custom_shell{ i };
                             } },
                      parsed);
  } };

  if (default_shell_obj.is<sol::protected_function>()) {
    sol::protected_function default_shell_func{
      default_shell_obj.as<sol::protected_function>()
    };

    // DEFAULT_SHELL functions can use envy.package() directly via phase context
    sol::protected_function_result result{ default_shell_func() };
    if (!result.valid()) {
      sol::error err = result;
      throw std::runtime_error("DEFAULT_SHELL function failed: " +
                               std::string{ err.what() });
    }

    return convert_parsed(
        parse_shell_config_from_lua(result.get<sol::object>(), "DEFAULT_SHELL function"));
  }

  return convert_parsed(parse_shell_config_from_lua(default_shell_obj, "DEFAULT_SHELL"));
}

std::optional<std::string> manifest::run_bundle_fetch(
    std::string const &bundle_identity,
    void *phase_ctx,
    std::filesystem::path const &tmp_dir) const {
  std::lock_guard const lock(lua_mutex_);

  if (!lua_) { return "manifest Lua state unavailable"; }

  sol::object bundles_obj{ (*lua_)["BUNDLES"] };
  if (!bundles_obj.valid() || !bundles_obj.is<sol::table>()) {
    return "BUNDLES table not found";
  }

  sol::table bundles_table{ bundles_obj.as<sol::table>() };
  sol::protected_function const fetch_func{ [&] {
    for (auto const &[key, value] : bundles_table) {
      if (!value.is<sol::table>()) { continue; }

      sol::table bundle_entry{ value.as<sol::table>() };

      sol::object identity_obj{ bundle_entry["identity"] };
      if (!identity_obj.valid() || !identity_obj.is<std::string>()) { continue; }
      if (identity_obj.as<std::string>() != bundle_identity) { continue; }

      sol::object source_obj{ bundle_entry["source"] };
      if (!source_obj.valid() || !source_obj.is<sol::table>()) { continue; }

      sol::table source_table{ source_obj.as<sol::table>() };
      sol::object fetch_obj{ source_table["fetch"] };
      if (!fetch_obj.valid() || !fetch_obj.is<sol::function>()) { continue; }

      return fetch_obj.as<sol::protected_function>();
    }
    return sol::protected_function{};
  }() };

  if (!fetch_func.valid()) {
    return "bundle fetch function not found: " + bundle_identity;
  }

  // RAII guard to clear registry on scope exit (including exceptions)
  sol::state_view lua_view{ *lua_ };
  struct registry_guard {
    sol::state_view &lua;
    ~registry_guard() { lua.registry()[ENVY_PHASE_CTX_RIDX] = sol::lua_nil; }
  } guard{ lua_view };

  lua_view.registry()[ENVY_PHASE_CTX_RIDX] = phase_ctx;

  sol::protected_function_result result{ fetch_func(util_normalized_path(tmp_dir)) };
  if (!result.valid()) {
    sol::error err = result;
    return std::string(err.what());
  }

  return std::nullopt;
}

manifest::depot_fetch_result manifest::run_depot_fetch(
    size_t lua_index,
    void *phase_ctx,
    std::filesystem::path const &tmp_dir,
    std::vector<std::pair<std::string, std::string>> const &deps) const {
  std::lock_guard const lock(lua_mutex_);

  std::string const label{ "PACKAGE_DEPOTS[" + std::to_string(lua_index) + "]" };

  if (!lua_) { throw std::runtime_error(label + ": manifest Lua state unavailable"); }

  sol::state_view lua_view{ *lua_ };

  sol::protected_function const fetch_func{ [&] {
    sol::object depots_obj{ lua_view["PACKAGE_DEPOTS"] };
    if (!depots_obj.valid() || !depots_obj.is<sol::table>()) {
      throw std::runtime_error(label + ": PACKAGE_DEPOTS global not found");
    }
    sol::object entry{ depots_obj.as<sol::table>()[lua_index] };
    if (!entry.valid() || !entry.is<sol::table>()) {
      throw std::runtime_error(label + ": entry is not a table");
    }
    sol::object fetch_obj{ entry.as<sol::table>()["FETCH"] };
    if (!fetch_obj.valid() || !fetch_obj.is<sol::function>()) {
      throw std::runtime_error(label + ": FETCH function not found");
    }
    return fetch_obj.as<sol::protected_function>();
  }() };

  // RAII guard to clear registry on scope exit (including exceptions)
  struct registry_guard {
    sol::state_view &lua;
    ~registry_guard() { lua.registry()[ENVY_PHASE_CTX_RIDX] = sol::lua_nil; }
  } guard{ lua_view };

  lua_view.registry()[ENVY_PHASE_CTX_RIDX] = phase_ctx;

  sol::table ctx{ lua_view.create_table() };
  ctx["tmp_dir"] = util_normalized_path(tmp_dir);
  sol::table deps_table{ lua_view.create_table() };
  for (auto const &[identity, pkg_path] : deps) {
    sol::table d{ lua_view.create_table() };
    d["pkg_path"] = pkg_path;
    deps_table[identity] = d;
  }
  ctx["deps"] = deps_table;

  sol::protected_function_result result{ fetch_func(ctx) };
  if (!result.valid()) {
    sol::error err = result;
    throw std::runtime_error(label + " FETCH failed: " + std::string(err.what()));
  }

  sol::object ret{ result.get<sol::object>() };
  if (ret.is<std::string>()) { return ret.as<std::string>(); }

  if (ret.get_type() == sol::type::table) {
    std::vector<depot_entry> entries;
    sol::table rt{ ret.as<sol::table>() };
    for (size_t i{ 1 }; i <= rt.size(); ++i) {
      sol::object e{ rt[i] };
      if (e.get_type() != sol::type::table) {
        throw std::runtime_error(label + " FETCH entries must be tables");
      }
      sol::table et{ e.as<sol::table>() };
      sol::object url_obj{ et["url"] };
      if (!url_obj.is<std::string>() || url_obj.as<std::string>().empty()) {
        throw std::runtime_error(label +
                                 " FETCH entries require a non-empty 'url' string");
      }
      depot_entry de{ url_obj.as<std::string>(), std::nullopt };
      if (sol::object sha_obj{ et["sha256"] };
          sha_obj.valid() && sha_obj.get_type() != sol::type::lua_nil) {
        if (!sha_obj.is<std::string>()) {
          throw std::runtime_error(label + " FETCH entry 'sha256' must be a string");
        }
        de.sha256 = sha_obj.as<std::string>();
      }
      entries.push_back(std::move(de));
    }
    return entries;
  }

  throw std::runtime_error(
      label + " FETCH must return depot manifest text, a path to it, or an entries table");
}

}  // namespace envy
