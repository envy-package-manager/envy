#include "product_util.h"

#include "engine.h"  // pkg_type
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_env.h"
#include "shell.h"
#include "util.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
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

struct argv_builder {
  product_render render;
  std::unordered_set<std::string> *validated;  // set: check only, each command once
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
    if (entry.command.empty()) {
      push(render(provider, name), provider, name);
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

    for (auto const &element : entry.command) {
      auto const holes{ product_util_placeholders(element) };
      if (holes.size() == 1 && holes[0].begin == 0 && holes[0].end == element.size()) {
        append(owner(provider, name, holes[0].name), holes[0].name);  // splices
        continue;
      }

      std::string rendered;
      std::size_t at{ 0 };
      for (auto const &h : holes) {
        pkg *const q{ owner(provider, name, h.name) };
        if (!entry_of(q, h.name).command.empty()) {
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
        rendered += render(q, h.name);
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
  if (!entry.command.empty()) {
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
  require_renderable_type(provider->type, name, provider->cfg->identity);
  if (provider->type == pkg_type::USER_MANAGED) { return entry.value; }
  std::filesystem::path const pkg_path{ pkg_path_of(provider) };
  if (pkg_path.empty()) {
    throw_runtime_error({ "Product '",
                          name,
                          "' provider '",
                          provider->cfg->identity,
                          "' missing pkg path" });
  }
  return spell(pkg_path / entry.value);
}

std::filesystem::path installed_path(pkg *p) { return p->pkg_path; }

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
  return it != provider->products.end() && !it->second.command.empty();
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

std::vector<std::string> product_util_argv(pkg *provider,
                                           std::string const &name,
                                           product_render render,
                                           std::string *program) {
  if (!provider) { throw_runtime_error({ "Product '", name, "' has no provider" }); }
  argv_builder b{
    .render = render, .validated = nullptr, .stack = {}, .out = {}, .program = {}
  };
  b.append(provider, name);
  if (program) { *program = std::move(b.program); }
  return std::move(b.out);
}

void product_util_validate(pkg *provider,
                           std::string const &name,
                           std::unordered_set<std::string> &validated) {
  auto const stand_in{ [](pkg *, std::string const &) { return std::string{}; } };
  argv_builder b{
    .render = stand_in, .validated = &validated, .stack = {}, .out = {}, .program = {}
  };
  b.append(provider, name);
}

std::vector<std::string> product_util_run_argv(pkg *provider,
                                               std::string const &name,
                                               std::string *program) {
  auto const installed{ [](pkg *p, std::string const &n) {
    return path_value(p, n, installed_path, utf8_preferred);
  } };
  return product_util_argv(provider, name, installed, program);
}

std::vector<std::string> product_util_predict_argv(pkg *provider,
                                                   std::string const &name) {
  return product_util_argv(provider, name, [](pkg *p, std::string const &n) {
    return path_value(p, n, pkg_predicted_path, utf8_preferred);
  });
}

std::optional<std::string> product_util_find_program(std::string const &program,
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
  auto const found{ shell_find_executable(
      util_path_from_utf8(program).string(), anchored, own ? bin_dir : nullptr) };
  return found ? std::optional{ util_path_to_utf8(*found) } : std::nullopt;
}

}  // namespace envy
