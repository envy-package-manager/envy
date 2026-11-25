#include "cmd_product.h"

#include "activation.h"
#include "blake3_util.h"
#include "engine.h"
#include "env_ops.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "platform.h"
#include "product_util.h"
#include "reexec.h"
#include "self_deploy.h"
#include "shell.h"
#include "tui.h"
#include "util.h"

#include "cli_parse.h"
#include "picojson.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace envy {

cli_cmd &cmd_product::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("product",
                     "Query product value or list all products from manifest") };
  sub.pos("product", c.product_name, "Product name (omit to list all)");
  sub.pos("args", c.args, "Arguments for the product, after --run's '--'");
  sub.opt("--manifest", c.manifest_path, "Path to envy.lua manifest");
  auto const json{ sub.flag("--json", c.json, "Output as JSON (to stdout)") };
  sub.flag("--run", c.run, "Run the product in the project's activated environment")
      .excludes(json);
  sub.finalize(
      [](void *p) -> char const * {
        auto const &sel{ *static_cast<cfg const *>(p) };
        if (sel.run && sel.product_name.empty()) { return "--run needs a product name"; }
        if (!sel.run && !sel.args.empty()) { return "product arguments need --run"; }
        return nullptr;
      },
      &c);
  return sub;
}

cmd_product::cmd_product(cfg cfg,
                         std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

namespace {

// A path product is a string; a command is its argv, ready to exec without re-parsing --
// or null off its platforms, where validation never checked what it names.
void print_products_json(engine &eng) {
  auto const products{ eng.collect_all_products() };

  picojson::object obj;
  for (auto const &pi : products) {
    obj[pi.product_name] = [&] {
      bool const command{ pi.is_command() };
      if (command && !util_platform_matches(pi.platforms,
                                            platform::os_name(),
                                            platform::arch_name())) {
        return picojson::value{};
      }
      auto argv{ product_util_predict_argv(eng.find_product_provider(pi.product_name),
                                           pi.product_name,
                                           eng.project_root()) };
      if (!command) { return picojson::value(std::move(argv.front())); }
      picojson::array arr;
      for (auto &arg : argv) { arr.emplace_back(std::move(arg)); }
      return picojson::value(std::move(arr));
    }();
  }

  std::string const output{ picojson::value(obj).serialize(true) };
  tui::print_stdout("%s", output.c_str());
}

// A command lists as declared, placeholders and all: a table as written, a function's
// result at the predicted path -- "(function)" off its platforms, where nothing called it.
std::string listed_value(engine &eng, product_info const &p) {
  auto const argv{ [&]() -> std::vector<std::string> {
    if (!p.command_fn) { return p.command; }
    if (!util_platform_matches(p.platforms, platform::os_name(), platform::arch_name())) {
      return { "(function)" };
    }
    return product_util_predict_command(eng.find_product_provider(p.product_name),
                                        p.product_name,
                                        eng.project_root());
  }() };
  std::string v{ p.value };  // empty for a command
  for (auto const &arg : argv) { v += (v.empty() ? "" : " ") + arg; }
  return v;
}

void print_products_aligned(engine &eng) {
  auto const products{ eng.collect_all_products() };
  if (products.empty()) {
    tui::info("No products defined");
    return;
  }

  std::vector<std::string> values;
  values.reserve(products.size());
  for (auto const &p : products) { values.push_back(listed_value(eng, p)); }

  // Calculate column widths
  size_t max_product{ 0 };
  size_t max_value{ 0 };
  for (size_t i{ 0 }; i < products.size(); ++i) {
    max_product = std::max(max_product, products[i].product_name.size());
    max_value = std::max(max_value, values[i].size());
  }

  // Print aligned rows
  for (size_t i{ 0 }; i < products.size(); ++i) {
    auto const &p{ products[i] };
    std::string const user_managed_marker{ p.type == pkg_type::USER_MANAGED
                                               ? " (user-managed)"
                                               : "" };
    std::ostringstream oss;
    oss << std::left << std::setw(max_product) << p.product_name << "  "
        << std::setw(max_value) << values[i] << "  " << p.provider_canonical
        << user_managed_marker;
    tui::info("%s", oss.str().c_str());
  }
}

// The provider of `name` in the graph `eng` resolved, its closure installed.
pkg *install_provider(engine &eng, std::string const &name) {
  pkg *const provider{ eng.find_product_provider(name) };
  if (!provider) {
    throw_runtime_error(
        { "product: '", name, "' has no provider in resolved dependency graph" });
  }
  eng.extend_dependencies_to_completion(provider);
  eng.wait_for_completion(provider->key);
  return provider;
}

// The program a product runs: an absolute path as is (every cache-managed path); a
// relative one anchors at the project root, where SETUP runs; a bare name is searched on
// `path_list` from the cwd the tool inherits. `supplier` is the product whose value or
// command put it there. UTF-8 in and out.
std::string resolve_program(std::string const &value,
                            std::string const &supplier,
                            activation_anchor const &anchor,
                            envy_meta const &meta,
                            std::string const *path_list) {
  std::filesystem::path const named{ util_path_from_utf8(value) };
  if (named.has_parent_path()) {
    return named.is_absolute() ? value : util_path_to_utf8(anchor.root / named);
  }
  std::error_code ec;
  auto const bin{ meta.bin ? std::filesystem::canonical(anchor.root / *meta.bin, ec)
                           : std::filesystem::path{} };
  auto const found{ product_util_find_program(value,
                                              supplier,
                                              path_list ? *path_list : std::string{},
                                              std::filesystem::current_path(),
                                              bin.empty() || ec ? nullptr : &bin) };
  if (!found) {
    throw_runtime_error(
        { "product: '", supplier, "' runs '", value, "', not found on PATH" });
  }
  return *found;
}

// The tool as `--run` execs it: the product rendered, its program resolved in the
// project's activation -- the one built here from the same engine.
struct tool_plan {
  std::vector<std::string> argv;  // UTF-8, as shell_exec reads it
  shell_env_t live;
  shell_env_t to;
};

tool_plan plan_tool(engine &eng,
                    manifest const &m,
                    pkg *provider,
                    std::string const &name) {
  shell_env_t live{ shell_getenv() };
  auto const anchor{ activation_anchor_read(eng.cache_root(), m.manifest_path) };
  shell_env_t to{ activation_for_tool(eng, m, anchor, live, provider) };
  std::string program;
  auto argv{ product_util_run_argv(provider, name, anchor.root, &program) };
  argv.front() =
      resolve_program(argv.front(), program, anchor, m.meta, env_get(to, "PATH"));
  return { .argv = std::move(argv), .live = std::move(live), .to = std::move(to) };
}

// Install the provider's closure, then plan the tool from that same engine: one graph.
// Host packages only, as install and activate resolve.
exec_request plan_run(cmd_product::cfg const &cfg,
                      std::optional<std::filesystem::path> const &cli_cache_root) {
  auto const [m, c]{
    cmd_startup_load("product", cfg.manifest_path, cli_cache_root, false, cfg.project_dir)
  };
  engine eng{ *c, m.get() };
  eng.set_quiet_idle_outcomes(true);  // a cache hit is no news on the tool's stderr
  eng.resolve_graph(
      engine_filter_host_platform({ m->packages.begin(), m->packages.end() }));
  auto plan{
    plan_tool(eng, *m, install_provider(eng, cfg.product_name), cfg.product_name)
  };
  for (auto const &arg : cfg.args) {  // main's argv is the native encoding
    plan.argv.push_back(util_path_to_utf8(std::filesystem::path{ arg }));
  }
  return { .argv = std::move(plan.argv),
           .from = std::move(plan.live),
           .to = std::move(plan.to) };
}

}  // namespace

// The tool owns stdout: anything envy or a spec's Lua prints meanwhile goes to stderr.
// main runs the request once the tui is down.
void cmd_product::run() const {
  auto request{ [&] {
    stdout_to_stderr redirect;
    try {
      auto r{ plan_run(cfg_, cli_cache_root_) };
      tui::flush();  // queued lines land while stderr stands in for stdout
      redirect.restore();
      return r;
    } catch (reexec_request const &) {
      redirect.restore();  // the pinned envy prints the tool's stdout itself
      throw;
    }
  }() };
  throw std::move(request);
}

void cmd_product::execute() {
  if (cfg_.run) {
    run();
    return;
  }

  auto const [m, c]{ cmd_startup_load("product",
                                      cfg_.manifest_path,
                                      cli_cache_root_,
                                      false,
                                      cfg_.project_dir) };
  engine eng{ *c, m.get() };
  eng.set_quiet_idle_outcomes(true);  // `$(envy product x)` wants the value, not news
  eng.resolve_graph({ m->packages.begin(), m->packages.end() });

  if (cfg_.product_name.empty()) {
    if (cfg_.json) {
      print_products_json(eng);
    } else {
      print_products_aligned(eng);
    }
    return;
  }

  // A path product prints as is; a command as `--run` would exec it, quoted for the
  // platform's shell.
  pkg *const provider{ install_provider(eng, cfg_.product_name) };
  std::string const rendered_value{
    product_util_is_command(provider, cfg_.product_name)
        ? shell_quote_argv(plan_tool(eng, *m, provider, cfg_.product_name).argv,
                           kShellPlatformDefault)
        : product_util_run_argv(provider, cfg_.product_name, eng.project_root()).front()
  };
  tui::print_stdout("%s\n", rendered_value.c_str());
}

}  // namespace envy
