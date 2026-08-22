#include "cmd_product.h"

#include "blake3_util.h"
#include "engine.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "platform.h"
#include "product_util.h"
#include "reexec.h"
#include "self_deploy.h"
#include "tui.h"
#include "util.h"

#include "CLI11.hpp"
#include "picojson.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>

namespace envy {

void cmd_product::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand(
      "product",
      "Query product value or list all products from manifest") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("product", cfg_ptr->product_name, "Product name (omit to list all)");
  sub->add_option("--manifest", cfg_ptr->manifest_path, "Path to envy.lua manifest");
  sub->add_flag("--json", cfg_ptr->json, "Output as JSON (to stdout)");
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_product::cmd_product(cfg cfg,
                         std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

namespace {

void print_products_json(engine &eng, cache &c) {
  auto const products{ eng.collect_all_products() };

  picojson::object obj;
  for (auto const &pi : products) {
    std::string const resolved{ [&] {
      if (pi.type == pkg_type::USER_MANAGED) { return pi.value; }
      pkg *provider{ eng.find_product_provider(pi.product_name) };
      std::ostringstream key;
      key << provider->cfg->format_key();
      for (auto const &wk : provider->resolved_weak_dependency_keys) { key << '|' << wk; }
      auto const key_for_hash{ key.str() };
      auto const digest{ blake3_hash(key_for_hash.data(), key_for_hash.size()) };
      std::string const hash_prefix{ util_bytes_to_hex(digest.data(), 8) };
      auto const pkg_path{ c.compute_pkg_path(provider->cfg->identity,
                                              platform::os_name(),
                                              platform::arch_name(),
                                              hash_prefix) };
      return util_normalized_path(pkg_path / pi.value);
    }() };
    obj[pi.product_name] = picojson::value(resolved);
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

}  // namespace

void cmd_product::execute() {
  auto const [m, c]{ cmd_startup_load("product", cfg_.manifest_path, cli_cache_root_) };
  engine eng{ *c, m.get() };

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
    throw std::runtime_error("product: '" + cfg_.product_name +
                             "' has no provider in resolved dependency graph");
  }

  eng.extend_dependencies_to_completion(provider);
  eng.wait_for_completion(provider->key);

  std::string const rendered_value{ product_util_resolve(provider, cfg_.product_name) };
  tui::print_stdout("%s\n", rendered_value.c_str());
}

}  // namespace envy
