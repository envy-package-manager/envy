#pragma once

#include "util.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace envy {

struct pkg;

// Compute the rendered product value for a provider pkg.
// Returns pkg_path/value for cache-managed providers, raw value for user-managed.
//
// spec_fetch writes the package type before it writes the products, so every product
// has a provider that is one or the other; any other type means the spec never finished
// loading, and both of these throw naming it rather than guess. Treating "not
// user-managed" as "cache-managed" is what printed an absolute path into a user-managed
// package's non-existent payload.
std::string product_util_resolve(pkg *provider, std::string const &product_name);

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

// The package whose PRODUCTS declares `name` for `consumer`. A product dependency pins it
// to its provider, which must declare it -- unless a weak fallback resolved it, which may
// provide it transitively. Unpinned, `consumer`'s closure decides. Null if nothing does;
// throws "not found in provider" when a pin names a package that does not declare it.
pkg *product_util_owner(pkg *consumer, std::string const &name);

// How a path product renders as a command leaf. Running, predicting and validating
// differ only here.
using product_render = function_ref<std::string(pkg *provider, std::string const &name)>;

// `name` of `provider` as argv: a path product is one element; a command renders each
// element, every `{{name}}` resolved by product_util_owner, splicing a command that fills
// a whole element. Throws on a name nothing provides, a cycle, a command embedded in part
// of an element, or a NUL, which no exec carries. `program`, if given, receives the
// product whose value or command put argv[0] there -- what a PATH search must not find
// as its own wrapper.
std::vector<std::string> product_util_argv(pkg *provider,
                                           std::string const &name,
                                           product_render render,
                                           std::string *program = nullptr);

// What product_util_argv checks, without expanding: each command walks once, however many
// commands splice it, so validation stays linear in the declarations. `validated` carries
// the commands already found sound across calls.
void product_util_validate(pkg *provider,
                           std::string const &name,
                           std::unordered_set<std::string> &validated);

// product_util_argv over installed paths, as UTF-8: the encoding of every Lua string, of
// envy.run's script, and of what `--run` hands shell_exec.
std::vector<std::string> product_util_run_argv(pkg *provider,
                                               std::string const &name,
                                               std::string *program = nullptr);

// product_util_argv before anything is installed, as UTF-8 for JSON: a user-managed value
// raw, a cache-managed path where the payload *will* land, predicted from the provider's
// cache key. `envy product` lists after resolution but before any payload phase, so no
// pkg_path exists yet.
std::vector<std::string> product_util_predict_argv(pkg *provider, std::string const &name);

// Where a bare `program` runs from, UTF-8 in and out: searched on `path_list`, relative
// entries anchored at `base` (where it will run), past `bin_dir` when `program` names
// `supplier` -- the product's own deployed wrapper, which would only run envy again.
std::optional<std::string> product_util_find_program(std::string const &program,
                                                     std::string const &supplier,
                                                     std::string const &path_list,
                                                     std::filesystem::path const &base,
                                                     std::filesystem::path const *bin_dir);

}  // namespace envy
