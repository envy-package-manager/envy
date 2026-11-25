#include "product_util.h"

#include "engine.h"  // pkg_type
#include "lua_envy.h"
#include "lua_error_formatter.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_env.h"
#include "shell.h"
#include "util.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace envy {

namespace {

// A product whose provider has neither renderable type cannot exist -- spec_fetch sets
// the type, and only afterwards parses PRODUCTS -- so reaching this is a spec that
// never finished loading, reported through a reader that saw half of it.
void require_renderable_type(pkg_type type,
                             std::string const &product_name,
                             std::string const &provider) {
  if (type == pkg_type::USER_MANAGED || type == pkg_type::CACHE_MANAGED) { return; }
  throw_runtime_error({ "Product '",
                        product_name,
                        "' provider '",
                        provider,
                        "' has no resolved package type; its spec did not finish "
                        "loading" });
}

// A copy, so rendering never holds deps_mutex across a callback or a recursion.
product_entry entry_of(pkg *p, std::string const &name) {
  std::lock_guard const deps_lock(p->deps_mutex);
  auto const it{ p->products.find(name) };
  if (it == p->products.end()) {
    throw_runtime_error(
        { "Product '", name, "' not found in provider '", p->cfg->identity, "'" });
  }
  return it->second;
}

bool entry_declared(pkg *p, std::string const &name) {
  std::lock_guard const deps_lock(p->deps_mutex);
  return p->products.contains(name);
}

std::string validated_key(pkg *p, std::string const &name) {
  return p->key.canonical() + '\0' + name;
}

// A cache-managed provider's payload per `pkg_path_of`; nullopt when user-managed.
template <typename PkgPathOf>
std::optional<std::filesystem::path> provider_dir(pkg *provider,
                                                  std::string const &name,
                                                  PkgPathOf pkg_path_of) {
  require_renderable_type(provider->type, name, provider->cfg->identity);
  if (provider->type == pkg_type::USER_MANAGED) { return std::nullopt; }
  std::filesystem::path pkg_path{ pkg_path_of(provider) };
  if (pkg_path.empty()) {
    throw_runtime_error({ "Product '",
                          name,
                          "' provider '",
                          provider->cfg->identity,
                          "' missing pkg path" });
  }
  return pkg_path;
}

// What `name`'s command function returns, called as ENV is and held to a table's rules.
std::vector<std::string> call_command_fn(pkg *provider,
                                         std::string const &name,
                                         product_render const &r) {
  auto const dir{ provider_dir(provider, name, r.pkg_path) };
  std::string const state_dir{ util_path_with_separator(
      pkg_state_dir(provider, r.state_root)) };
  std::string const &id{ provider->cfg->identity };
  auto const lua_acc{ provider->lua.lock() };
  if (!lua_acc) {
    throw_runtime_error({ "Product '", name, "' provider '", id, "' has no spec state" });
  }
  sol::state_view lua{ *lua_acc };
  sol::protected_function const fn{ lua.registry()[ENVY_PRODUCT_COMMANDS_RIDX][name] };
  sol::object const pkg_dir{ dir ? sol::make_object(lua, util_path_with_separator(*dir))
                                 : sol::make_object(lua, sol::lua_nil) };
  sol::object const opts{ lua.registry()[ENVY_OPTIONS_RIDX] };
  sol::object const result{ call_lua_function_with_enriched_errors(
      provider,
      "PRODUCTS['" + name + "'].command",
      [&] { return fn(pkg_dir, state_dir, opts); }) };
  return product_util_parse_command(
      result, name, id, provider->type == pkg_type::USER_MANAGED);
}

struct argv_builder {
  product_render const &render;
  std::unordered_set<std::string> *validated;        // set: check only, each command once
  std::vector<std::pair<pkg *, std::string>> stack;  // products being rendered: cycles
  std::vector<std::string> out;
  std::string program;  // the product that rendered out[0]

  void push(std::string arg, pkg *provider, std::string const &from) {
    if (arg.find('\0') != std::string::npos) {
      throw_runtime_error({ "Product '",
                            from,
                            "' in spec '",
                            provider->cfg->identity,
                            "' renders an argument holding a NUL, which no program "
                            "receives intact" });
    }
    if (out.empty()) { program = from; }
    out.push_back(std::move(arg));
  }

  // The package providing `name` for `provider`'s product `of`; throws if none.
  pkg *owner(pkg *provider, std::string const &of, std::string const &name) {
    if (pkg *const q{ product_util_owner(provider, name) }) { return q; }
    throw_runtime_error({ "Product '",
                          of,
                          "' in spec '",
                          provider->cfg->identity,
                          "' runs '{{",
                          name,
                          "}}', but nothing in its dependency closure provides '",
                          name,
                          "'" });
  }

  void append(pkg *provider, std::string const &name) {
    product_entry const entry{ entry_of(provider, name) };
    if (!entry.is_command()) {
      push(render.leaf(provider, name), provider, name);
      return;
    }
    if (validated && validated->contains(validated_key(provider, name))) { return; }

    std::pair<pkg *, std::string> self{ provider, name };
    if (std::ranges::find(stack, self) != stack.end()) {
      std::string chain;
      for (auto const &[_, n] : stack) { chain += n + " -> "; }
      throw_runtime_error({ "Product '",
                            name,
                            "' in spec '",
                            provider->cfg->identity,
                            "' is a command cycle: ",
                            chain,
                            name });
    }
    stack.push_back(std::move(self));

    auto const command{ entry.command_fn ? call_command_fn(provider, name, render)
                                         : entry.command };
    for (auto const &element : command) {
      auto const holes{ product_util_placeholders(element) };
      if (holes.size() == 1 && holes[0].begin == 0 && holes[0].end == element.size()) {
        append(owner(provider, name, holes[0].name), holes[0].name);  // splices
        continue;
      }

      std::string rendered;
      std::size_t at{ 0 };
      for (auto const &h : holes) {
        pkg *const q{ owner(provider, name, h.name) };
        if (entry_of(q, h.name).is_command()) {
          throw_runtime_error({ "Product '",
                                name,
                                "' in spec '",
                                provider->cfg->identity,
                                "' embeds command product '",
                                h.name,
                                "' in '",
                                element,
                                "'; a command can only fill a whole element" });
        }
        rendered.append(element, at, h.begin - at);
        rendered += render.leaf(q, h.name);
        at = h.end;
      }
      rendered.append(element, at);
      push(std::move(rendered), provider, name);
    }
    stack.pop_back();
    if (validated) { validated->insert(validated_key(provider, name)); }
  }
};

// A path product's value: a user-managed one raw (Lua's UTF-8), else the payload's path
// under `pkg_path_of(provider)` -- asked only then -- rendered by `spell`.
template <typename PkgPathOf, typename Spell>
std::string path_value(pkg *provider,
                       std::string const &name,
                       PkgPathOf pkg_path_of,
                       Spell spell) {
  product_entry const entry{ entry_of(provider, name) };
  if (entry.is_command()) {
    throw_runtime_error({ "Product '",
                          name,
                          "' in provider '",
                          provider->cfg->identity,
                          "' is a command, not a path" });
  }
  if (entry.value.empty()) {
    throw_runtime_error(
        { "Product '", name, "' is empty in provider '", provider->cfg->identity, "'" });
  }
  auto const dir{ provider_dir(provider, name, pkg_path_of) };
  return dir ? spell(*dir / entry.value) : entry.value;
}

std::filesystem::path installed_path(pkg *p) { return p->pkg_path; }

// Lambdas, not the functions: a function_ref holds a callable that outlives it.
auto const kInstalled{ [](pkg *p) { return installed_path(p); } };
auto const kPredicted{ [](pkg *p) { return pkg_predicted_path(p); } };
auto const kNoLeaf{ [](pkg *, std::string const &) { return std::string{}; } };

std::string utf8_preferred(std::filesystem::path path) {
  return util_path_to_utf8(path.make_preferred());
}

bool same_program(std::string_view a, std::string_view b) {
#ifdef _WIN32
  auto const lower{ [](char c) {
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
  } };
  return std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
#else
  return a == b;
#endif
}

}  // namespace

std::string product_util_resolve(pkg *provider, std::string const &product_name) {
  if (!provider) {
    throw_runtime_error({ "Product '", product_name, "' has no provider" });
  }
  return path_value(provider, product_name, installed_path, util_normalized_path);
}

bool product_util_is_command(pkg *provider, std::string const &name) {
  std::lock_guard const deps_lock(provider->deps_mutex);
  auto const it{ provider->products.find(name) };
  return it != provider->products.end() && it->second.is_command();
}

std::vector<product_placeholder> product_util_placeholders(std::string_view element) {
  std::vector<product_placeholder> out;
  for (std::size_t at{ element.find("{{") }; at != std::string_view::npos;
       at = element.find("{{", at)) {
    std::size_t const close{ element.find("}}", at + 2) };
    if (close == std::string_view::npos) {
      throw_runtime_error({ "unterminated '{{' in '", element, "'" });
    }
    std::string_view name{ element.substr(at + 2, close - at - 2) };
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) {
      name.remove_prefix(1);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
      name.remove_suffix(1);
    }
    if (name.empty()) { throw_runtime_error({ "empty placeholder in '", element, "'" }); }
    out.push_back({ .name = std::string{ name }, .begin = at, .end = close + 2 });
    at = close + 2;
  }
  return out;
}

pkg *product_util_closure_provider(pkg *root, std::string const &name) {
  std::vector<pkg *> work{ root };
  std::unordered_set<pkg const *> visited{ root };

  while (!work.empty()) {
    pkg *const p{ work.back() };
    work.pop_back();

    std::lock_guard const deps_lock(p->deps_mutex);
    if (p->products.contains(name)) { return p; }
    for (auto const &[_, dep_info] : p->dependencies) {
      if (visited.insert(dep_info.p).second) { work.push_back(dep_info.p); }
    }
  }

  return nullptr;
}

pkg *product_util_owner(pkg *consumer, std::string const &name) {
  auto const [pinned, via_fallback]{ [&]() -> std::pair<pkg *, bool> {
    std::lock_guard const deps_lock(consumer->deps_mutex);
    auto const it{ consumer->product_dependencies.find(name) };
    if (it == consumer->product_dependencies.end() || !it->second.provider) {
      return { nullptr, false };
    }
    pkg *const p{ it->second.provider };
    return { p, std::ranges::any_of(consumer->weak_references, [&](auto const &wr) {
               return wr.is_product && wr.fallback && wr.query == name && wr.resolved == p;
             }) };
  }() };
  if (!pinned) { return product_util_closure_provider(consumer, name); }
  if (via_fallback) { return product_util_closure_provider(pinned, name); }
  if (entry_declared(pinned, name)) { return pinned; }
  throw_runtime_error(  // as entry_of words it: the pin names the provider
      { "Product '", name, "' not found in provider '", pinned->cfg->identity, "'" });
}

std::vector<std::string> product_util_parse_command(sol::object const &obj,
                                                    std::string const &key,
                                                    std::string const &id,
                                                    bool user_managed) {
  auto const fail{ [&](std::string_view why) {
    throw_runtime_error(
        { "PRODUCTS entry '", key, "' command ", why, " in spec '", id, "'" });
  } };
  if (obj.get_type() != sol::type::table) { fail("must be a list of strings"); }
  sol::table const t{ obj.as<sol::table>() };
  std::size_t entries{ 0 };
  for (auto const &kv : t) {
    if (!kv.first.is<std::size_t>() || !kv.second.is<std::string>()) {
      fail("must be a list of strings");
    }
    ++entries;
  }
  if (entries != t.size()) { fail("must not have holes"); }

  std::vector<std::string> out;
  out.reserve(entries);
  for (std::size_t i{ 1 }; i <= entries; ++i) {
    std::string s{ t.get<std::string>(i) };
    try {
      product_util_placeholders(s);
    } catch (std::exception const &e) { fail(e.what()); }
    out.push_back(std::move(s));
  }
  if (out.empty() || out.front().empty()) {
    fail("must be a non-empty list starting with a program");
  }
  // A literal relative program names nothing every surface agrees on: `--run` anchors it
  // at the project root, a phase at its run dir. A cache-managed spec's own are pkg_dir's.
  std::filesystem::path const program{ out.front() };
  if (!user_managed && product_util_placeholders(out.front()).empty() &&
      program.has_parent_path() && !program.has_root_directory()) {  // `/usr/bin/env`
                                                                     // on Windows too
    fail("starts with relative path '" + out.front() +
         "'; join it to a command function's pkg_dir");
  }
  return out;
}

std::vector<std::string> product_util_argv(pkg *provider,
                                           std::string const &name,
                                           product_render const &render,
                                           std::string *program) {
  if (!provider) { throw_runtime_error({ "Product '", name, "' has no provider" }); }
  argv_builder b{ .render = render,
                  .validated = nullptr,
                  .stack = {},
                  .out = {},
                  .program = {} };
  b.append(provider, name);
  if (program) { *program = std::move(b.program); }
  return std::move(b.out);
}

void product_util_validate(pkg *provider,
                           std::string const &name,
                           std::filesystem::path const &state_root,
                           std::unordered_set<std::string> &validated) {
  product_render const render{ .leaf = kNoLeaf,
                               .pkg_path = kPredicted,
                               .state_root = state_root };
  argv_builder b{ .render = render,
                  .validated = &validated,
                  .stack = {},
                  .out = {},
                  .program = {} };
  b.append(provider, name);
}

std::vector<std::string> product_util_run_argv(pkg *provider,
                                               std::string const &name,
                                               std::filesystem::path const &state_root,
                                               std::string *program) {
  auto const leaf{ [](pkg *p, std::string const &n) {
    return path_value(p, n, installed_path, utf8_preferred);
  } };
  return product_util_argv(
      provider,
      name,
      { .leaf = leaf, .pkg_path = kInstalled, .state_root = state_root },
      program);
}

std::vector<std::string> product_util_predict_argv(
    pkg *provider,
    std::string const &name,
    std::filesystem::path const &state_root) {
  auto const leaf{ [](pkg *p, std::string const &n) {
    return path_value(p, n, pkg_predicted_path, utf8_preferred);
  } };
  return product_util_argv(
      provider,
      name,
      { .leaf = leaf, .pkg_path = kPredicted, .state_root = state_root });
}

std::vector<std::string> product_util_predict_command(
    pkg *provider,
    std::string const &name,
    std::filesystem::path const &state_root) {
  product_entry entry{ entry_of(provider, name) };
  if (!entry.is_command()) {
    throw_runtime_error({ "Product '",
                          name,
                          "' in provider '",
                          provider->cfg->identity,
                          "' is a path, not a command" });
  }
  if (!entry.command_fn) { return std::move(entry.command); }
  return call_command_fn(
      provider,
      name,
      { .leaf = kNoLeaf, .pkg_path = kPredicted, .state_root = state_root });
}

std::optional<std::string> product_util_find_program(
    std::string const &program,
    std::string const &supplier,
    std::string const &path_list,
    std::filesystem::path const &base,
    std::filesystem::path const *bin_dir) {
#ifdef _WIN32
  constexpr char kSep{ ';' };
#else
  constexpr char kSep{ ':' };
#endif
  // shell_find_executable reads native strings; empty entries (the cwd) stay skipped.
  std::string anchored;
  for (std::size_t at{ 0 }; at <= path_list.size();) {
    std::size_t const end{ std::min(path_list.find(kSep, at), path_list.size()) };
    std::string_view const entry{ std::string_view{ path_list }.substr(at, end - at) };
    if (!entry.empty()) {
      auto const dir{ util_path_from_utf8(entry) };
      anchored += (dir.is_absolute() ? dir : base / dir).string();
    }
    anchored += kSep;
    at = end + 1;
  }
  bool const own{ bin_dir && same_program(program, supplier) };
  auto const found{ shell_find_executable(util_path_from_utf8(program).string(),
                                          anchored,
                                          own ? bin_dir : nullptr) };
  return found ? std::optional{ util_path_to_utf8(*found) } : std::nullopt;
}

}  // namespace envy
