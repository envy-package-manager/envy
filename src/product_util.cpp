#include "product_util.h"

#include "engine.h"  // pkg_type
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_env.h"
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

struct argv_builder {
  product_render render;
  std::vector<std::pair<pkg *, std::string>> stack;  // products being rendered: cycles
  std::vector<std::string> out;
  std::string program;  // the product that rendered out[0]

  void push(std::string arg, std::string const &from) {
    if (out.empty()) { program = from; }
    out.push_back(std::move(arg));
  }

  // The closure member providing `name` for `provider`'s product `of`; throws if none.
  pkg *closure_provider(pkg *provider, std::string const &of, std::string const &name) {
    if (pkg *const q{ product_util_closure_provider(provider, name) }) { return q; }
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
      push(render(provider, name), name);
      return;
    }

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
        append(closure_provider(provider, name, holes[0].name), holes[0].name);  // splices
        continue;
      }

      std::string rendered;
      std::size_t at{ 0 };
      for (auto const &h : holes) {
        pkg *const q{ closure_provider(provider, name, h.name) };
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
      push(std::move(rendered), name);
    }
    stack.pop_back();
  }
};

// A user-managed value is host state; a cache-managed one lands where check will put it.
std::string predicted_value(pkg *provider, std::string const &name) {
  product_entry const entry{ entry_of(provider, name) };
  require_renderable_type(provider->type, name, provider->cfg->identity);
  return provider->type == pkg_type::USER_MANAGED
             ? entry.value
             : util_normalized_path(pkg_predicted_path(provider) / entry.value);
}

}  // namespace

std::string product_util_resolve(pkg *provider, std::string const &product_name) {
  if (!provider) {
    throw_runtime_error({ "Product '", product_name, "' has no provider" });
  }

  product_entry const entry{ entry_of(provider, product_name) };
  if (!entry.command.empty()) {
    throw_runtime_error({ "Product '",
                          product_name,
                          "' in provider '",
                          provider->cfg->identity,
                          "' is a command, not a path" });
  }

  std::string const &value{ entry.value };
  if (value.empty()) {
    throw_runtime_error({ "Product '",
                          product_name,
                          "' is empty in provider '",
                          provider->cfg->identity,
                          "'" });
  }

  require_renderable_type(provider->type, product_name, provider->cfg->identity);
  if (provider->type == pkg_type::USER_MANAGED) { return value; }

  if (provider->pkg_path.empty()) {
    throw_runtime_error({ "Product '",
                          product_name,
                          "' provider '",
                          provider->cfg->identity,
                          "' missing pkg path" });
  }

  return util_normalized_path(provider->pkg_path / value);
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

std::vector<std::string> product_util_argv(pkg *provider,
                                           std::string const &name,
                                           product_render render,
                                           std::string *program) {
  if (!provider) { throw_runtime_error({ "Product '", name, "' has no provider" }); }
  argv_builder b{ .render = render, .stack = {}, .out = {}, .program = {} };
  b.append(provider, name);
  if (program) { *program = std::move(b.program); }
  return std::move(b.out);
}

std::vector<std::string> product_util_run_argv(pkg *provider,
                                               std::string const &name,
                                               std::string *program) {
  auto const resolve{ [](pkg *p, std::string const &n) {
    return product_util_resolve(p, n);
  } };
  return product_util_argv(provider, name, resolve, program);
}

std::vector<std::string> product_util_predict_argv(pkg *provider,
                                                   std::string const &name) {
  return product_util_argv(provider, name, [](pkg *p, std::string const &n) {
    return predicted_value(p, n);
  });
}

}  // namespace envy
