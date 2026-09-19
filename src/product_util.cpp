#include "product_util.h"

#include "blake3_util.h"
#include "cache.h"
#include "engine.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "platform.h"
#include "util.h"

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>

namespace envy {

namespace {

// A product whose provider has neither renderable type cannot exist -- spec_fetch sets
// the type, and only afterwards parses PRODUCTS -- so reaching this is a spec that
// never finished loading, reported through a reader that saw half of it.
void require_renderable_type(pkg_type type,
                             std::string const &product_name,
                             std::string const &provider) {
  if (type == pkg_type::USER_MANAGED || type == pkg_type::CACHE_MANAGED) { return; }
  throw std::runtime_error("Product '" + product_name + "' provider '" + provider +
                           "' has no resolved package type; its spec did not finish "
                           "loading");
}

}  // namespace

std::string product_util_predict(product_info const &pi, pkg *provider, cache &c) {
  require_renderable_type(pi.type, pi.product_name, pi.provider_canonical);
  if (pi.type == pkg_type::USER_MANAGED) { return pi.value; }

  if (!provider) {
    throw std::runtime_error("Product '" + pi.product_name + "' has no provider");
  }

  // Where the payload will land: the same key the cache will hash when it installs it,
  // so the printed path is the one that appears.
  std::ostringstream key;
  key << provider->cfg->format_key();
  for (auto const &wk : provider->resolved_weak_dependency_keys) { key << '|' << wk; }
  auto const key_for_hash{ key.str() };
  auto const digest{ blake3_hash(key_for_hash.data(), key_for_hash.size()) };

  auto const pkg_path{ c.compute_pkg_path(provider->cfg->identity,
                                          platform::os_name(),
                                          platform::arch_name(),
                                          util_bytes_to_hex(digest.data(), 8)) };
  return util_normalized_path(pkg_path / pi.value);
}

std::string product_util_resolve(pkg *provider, std::string const &product_name) {
  if (!provider) {
    throw std::runtime_error("Product '" + product_name + "' has no provider");
  }

  auto const it{ provider->products.find(product_name) };
  if (it == provider->products.end()) {
    throw std::runtime_error("Product '" + product_name + "' not found in provider '" +
                             provider->cfg->identity + "'");
  }

  std::string const &value{ it->second.value };
  if (value.empty()) {
    throw std::runtime_error("Product '" + product_name + "' is empty in provider '" +
                             provider->cfg->identity + "'");
  }

  require_renderable_type(provider->type, product_name, provider->cfg->identity);
  if (provider->type == pkg_type::USER_MANAGED) { return value; }

  if (provider->pkg_path.empty()) {
    throw std::runtime_error("Product '" + product_name + "' provider '" +
                             provider->cfg->identity + "' missing pkg path");
  }

  return util_normalized_path(provider->pkg_path / value);
}

}  // namespace envy
