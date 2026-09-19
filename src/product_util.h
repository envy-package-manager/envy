#pragma once

#include <string>

namespace envy {

class cache;
struct pkg;
struct product_info;

// Compute the rendered product value for a provider pkg.
// Returns pkg_path/value for cache-managed providers, raw value for user-managed.
//
// spec_fetch writes the package type before it writes the products, so every product
// has a provider that is one or the other; any other type means the spec never finished
// loading, and both of these throw naming it rather than guess. Treating "not
// user-managed" as "cache-managed" is what printed an absolute path into a user-managed
// package's non-existent payload.
std::string product_util_resolve(pkg *provider, std::string const &product_name);

// What a listing prints, before anything is installed: the raw value for a user-managed
// provider, else where the payload *will* land, predicted from the provider's cache key.
// `envy product` runs after resolution but before any payload phase, so there is no
// pkg_path to read yet -- which is why this cannot be product_util_resolve.
std::string product_util_predict(product_info const &pi, pkg *provider, cache &c);

}  // namespace envy
