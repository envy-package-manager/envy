#pragma once

#include "util.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

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
std::string product_util_predict(product_info const &pi, pkg *provider);

// Whether `provider` declares `name` as a command rather than a path.
bool product_util_is_command(pkg *provider, std::string const &name);

// A `{{name}}` in a command element: the trimmed name and the span it replaces.
struct product_placeholder {
  std::string name;
  std::size_t begin;
  std::size_t end;
};

// Placeholders in one command element, in order. Throws on an unterminated `{{` or an
// empty name; a stray `}}` is literal text.
std::vector<product_placeholder> product_util_placeholders(std::string_view element);

// The package in `root`'s dependency closure, `root` included, whose PRODUCTS declares
// `name`; null if none. The product registry admits one provider per name, so at most one.
pkg *product_util_closure_provider(pkg *root, std::string const &name);

// How a path product renders as a command leaf. Running, predicting and validating
// differ only here.
using product_render = function_ref<std::string(pkg *provider, std::string const &name)>;

// `name` of `provider` as argv: a path product is one element; a command renders each
// element, every `{{name}}` a product of the provider or its closure, splicing a command
// that fills a whole element. Throws on a name the closure lacks, a cycle, or a command
// embedded in part of an element.
std::vector<std::string> product_util_argv(pkg *provider,
                                           std::string const &name,
                                           product_render render);

// product_util_argv over installed paths (product_util_resolve).
std::vector<std::string> product_util_run_argv(pkg *provider, std::string const &name);

// product_util_argv over predicted paths, as product_util_predict renders a path product.
std::vector<std::string> product_util_predict_argv(pkg *provider, std::string const &name);

}  // namespace envy
