#include "product_util.h"

#include "engine.h"
#include "pkg.h"
#include "util.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace envy {

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

  if (provider->type == pkg_type::USER_MANAGED) { return value; }

  if (provider->pkg_path.empty()) {
    throw std::runtime_error("Product '" + product_name + "' provider '" +
                             provider->cfg->identity + "' missing pkg path");
  }

  return util_normalized_path(provider->pkg_path / value);
}

}  // namespace envy
