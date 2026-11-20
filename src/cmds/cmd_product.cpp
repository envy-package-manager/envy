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
#include <sstream>
#include <system_error>
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

void print_products_json(engine &eng, cache &c) {
  auto const products{ eng.collect_all_products() };

  picojson::object obj;
  for (auto const &pi : products) {
    obj[pi.product_name] = picojson::value(
        product_util_predict(pi, eng.find_product_provider(pi.product_name), c));
  }

  std::string const output{ picojson::value(obj).serialize(true) };
  tui::print_stdout("%s", output.c_str());
}

void print_products_aligned(std::vector<product_info> const &products) {
  if (products.empty()) {
    tui::info("No products defined");
    return;
  }

  // Calculate column widths
  size_t max_product{ 0 };
  size_t max_value{ 0 };
  size_t max_provider{ 0 };

  for (auto const &p : products) {
    max_product = std::max(max_product, p.product_name.size());
    max_value = std::max(max_value, p.value.size());
    max_provider = std::max(max_provider, p.provider_canonical.size());
  }

  // Print aligned rows
  for (auto const &p : products) {
    std::string const user_managed_marker{ p.type == pkg_type::USER_MANAGED
                                               ? " (user-managed)"
                                               : "" };
    std::ostringstream oss;
    oss << std::left << std::setw(max_product) << p.product_name << "  "
        << std::setw(max_value) << p.value << "  " << p.provider_canonical
        << user_managed_marker;
    tui::info("%s", oss.str().c_str());
  }
}

// Everything `--run` hands the OS once envy's own work is done.
struct run_plan {
  std::vector<std::string> argv;
  shell_env_t live;    // the environment as it is now
  shell_env_t target;  // the one to run in
};

// Install the provider's closure, then activate from that same engine unless `live`
// already is this project's current activation: one graph, not two.
run_plan plan_run(cmd_product::cfg const &cfg,
                  std::optional<std::filesystem::path> const &cli_cache_root) {
  auto const [m, c]{
    cmd_startup_load("product", cfg.manifest_path, cli_cache_root, false, cfg.project_dir)
  };
  engine eng{ *c, m.get() };
  eng.set_quiet_idle_outcomes(true);  // a wrapper's stderr is its tool's
  eng.resolve_graph({ m->packages.begin(), m->packages.end() });

  pkg *provider{ eng.find_product_provider(cfg.product_name) };
  if (!provider) {
    throw_runtime_error({ "product: '",
                          cfg.product_name,
                          "' has no provider in resolved dependency graph" });
  }
  eng.extend_dependencies_to_completion(provider);
  eng.wait_for_completion(provider->key);
  std::string const value{ product_util_resolve(provider, cfg.product_name) };

  // After the install, which bumps the generation: an activation from before it lacks
  // the product's own ENV, and must not pass for current.
  shell_env_t live{ shell_getenv() };
  auto const anchor{ activation_anchor_read(eng.cache_root(), m->manifest_path) };
  shell_env_t target{
    env_activation_current(live, anchor.root.string(), anchor.gen)
        ? live
        : activation_from_engine(eng, *m, anchor, live, provider).target
  };

  // A bare user-managed value is a command, searched past the bin dir: the wrapper named
  // after it sits first on PATH and would only run envy again.
  auto program{ [&] {
    if (provider->type != pkg_type::USER_MANAGED ||
        std::filesystem::path{ value }.has_parent_path()) {
      return value;
    }
    std::error_code ec;
    auto const bin{ m->meta.bin
                        ? std::filesystem::canonical(anchor.root / *m->meta.bin, ec)
                        : std::filesystem::path{} };
    auto const *path_list{ env_get(target, "PATH") };
    auto const found{ shell_find_executable(value,
                                            path_list ? *path_list : std::string{},
                                            bin.empty() || ec ? nullptr : &bin) };
    if (!found) {
      throw_runtime_error(
          { "product: '", cfg.product_name, "' runs '", value, "', not found on PATH" });
    }
    return found->string();
  }() };

  run_plan plan{ .argv = { std::move(program) },
                 .live = std::move(live),
                 .target = std::move(target) };
  plan.argv.insert(plan.argv.end(), cfg.args.begin(), cfg.args.end());
  return plan;
}

}  // namespace

// The tool owns stdout: anything envy or a spec's Lua prints meanwhile goes to stderr.
void cmd_product::run() const {
  auto const plan{ [&] {
    stdout_to_stderr redirect;
    try {
      auto p{ plan_run(cfg_, cli_cache_root_) };
      tui::sections_clear();
      tui::flush();  // queued lines land while stderr stands in for stdout
      redirect.restore();
      return p;
    } catch (reexec_request const &) {
      redirect.restore();  // the pinned envy prints the tool's stdout itself
      throw;
    }
  }() };
  tui::trace_file_handoff();
  shell_putenv(plan.live, plan.target);
  throw subprocess_exit{ shell_exec(plan.argv) };  // POSIX: returns only by throwing
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
  eng.set_quiet_idle_outcomes(true);  // a wrapper's stderr is its tool's
  eng.resolve_graph({ m->packages.begin(), m->packages.end() });

  if (cfg_.product_name.empty()) {
    if (cfg_.json) {
      print_products_json(eng, *c);
    } else {
      print_products_aligned(eng.collect_all_products());
    }
    return;
  }

  pkg *provider{ eng.find_product_provider(cfg_.product_name) };
  if (!provider) {
    throw_runtime_error({ "product: '",
                          cfg_.product_name,
                          "' has no provider in resolved dependency graph" });
  }

  eng.extend_dependencies_to_completion(provider);
  eng.wait_for_completion(provider->key);

  std::string const rendered_value{ product_util_resolve(provider, cfg_.product_name) };
  tui::print_stdout("%s\n", rendered_value.c_str());
}

}  // namespace envy
