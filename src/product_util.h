#pragma once

#include "util.h"

#include "sol/sol.hpp"

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
// `pkg_path` maps a provider to its payload dir; default: the installed one.
std::string product_util_resolve(
    pkg *provider,
    std::string const &product_name,
    function_ref<std::filesystem::path(pkg *provider)> pkg_path = {});

// A path product's value as UTF-8: user-managed raw, else under `pkg_path(provider)`.
std::string product_util_path_utf8(
    pkg *provider,
    std::string const &name,
    function_ref<std::filesystem::path(pkg *provider)> pkg_path);

// Whether `provider` declares `name` as a command rather than a path.
bool product_util_is_command(pkg *provider, std::string const &name);

// One `{{ }}` and its span: a product name, or an escape emitting `literal` as is.
struct product_placeholder {
  std::string name;     // empty for an escape
  std::string literal;  // an escape's text
  std::size_t begin;
  std::size_t end;

  bool escape() const { return name.empty(); }
};

// Whether `c` is allowed in a product key.
bool product_util_key_char(char c);

// Finds `{{ }}` in `text`: braces pair by depth within a line; `{{{{a}}}}` escapes
// `{{a}}`; an odd brace run's first `{` is text. Throws on unterminated, empty or invalid
// names.
std::vector<product_placeholder> product_util_placeholders(std::string_view text);

// The package in `root`'s dependency closure, `root` included, whose PRODUCTS declares
// `name`; null if none. The product registry admits one provider per name, so at most one.
pkg *product_util_closure_provider(pkg *root, std::string const &name);

// The package whose PRODUCTS declares `name` for `consumer`. A product dependency pins it
// to its provider, which must declare it -- unless a weak fallback resolved it, which may
// provide it transitively. Unpinned, `consumer`'s closure decides. Null if nothing does;
// throws "not found in provider" when a pin names a package that does not declare it.
pkg *product_util_owner(pkg *consumer, std::string const &name);

// A PRODUCTS command as argv: strings, a program first, placeholders parsed; `what` names
// it in errors. A table is held to this at load, a function's result at each call.
std::vector<std::string> product_util_parse_command(sol::object const &obj,
                                                    std::string const &key,
                                                    std::string const &id,
                                                    bool user_managed,
                                                    std::string_view what = "command");

// What running, predicting and validating differ in: each payload's dir, pkg_state_dir's
// root, and -- for validation and tests -- a stand-in for path products.
struct product_render {
  function_ref<std::filesystem::path(pkg *provider)> pkg_path;  // installed or predicted
  std::filesystem::path state_root;  // the project, a phase's scratch; empty: none
  function_ref<std::string(pkg *provider, std::string const &name)> leaf{};
  function_ref<pkg *(pkg *provider, std::string const &name)> owner{};  // default: closure
  pkg *held_pkg{ nullptr };    // package whose Lua state is `held`
  lua_State *held{ nullptr };  // locked by the caller
};

// `name` of `provider` as argv: a path product is one element; a command renders each
// element, every `{{name}}` resolved by product_util_owner, splicing a command that fills
// a whole element. Throws on a name nothing provides, a cycle, a command embedded in part
// of an element, or a NUL, which no exec carries. `program`, if given, receives the
// product whose value or command put argv[0] there -- what a PATH search must not find
// as its own wrapper.
std::vector<std::string> product_util_argv(pkg *provider,
                                           std::string const &name,
                                           product_render const &render,
                                           std::string *program = nullptr);

// What product_util_argv checks, without expanding: each command walks once, however often
// spliced. `validated` carries the sound ones across calls; functions see predicted paths.
void product_util_validate(pkg *provider,
                           std::string const &name,
                           std::filesystem::path const &state_root,
                           std::unordered_set<std::string> &validated);

// product_util_argv over installed paths, as UTF-8: the encoding of every Lua string, of
// envy.run's script, and of what `--run` hands shell_exec.
std::vector<std::string> product_util_run_argv(pkg *provider,
                                               std::string const &name,
                                               std::filesystem::path const &state_root,
                                               std::string *program = nullptr);

// product_util_argv before anything is installed, as UTF-8 for JSON: a user-managed value
// raw, a cache-managed path where the payload *will* land, predicted from the provider's
// cache key. `envy product` lists after resolution but before any payload phase, so no
// pkg_path exists yet.
std::vector<std::string> product_util_predict_argv(
    pkg *provider,
    std::string const &name,
    std::filesystem::path const &state_root);

// What `name`'s command function returns at the predicted path, placeholders intact.
std::vector<std::string> product_util_predict_function(
    pkg *provider,
    std::string const &name,
    std::filesystem::path const &state_root);

// Where a bare `program` runs from, UTF-8 in and out: searched on `path_list`, relative
// entries anchored at `base` (where it will run), past `bin_dir` when `program` names
// `supplier` -- the product's own deployed wrapper, which would only run envy again.
std::optional<std::string> product_util_find_program(std::string const &program,
                                                     std::string const &supplier,
                                                     std::string const &path_list,
                                                     std::filesystem::path const &base,
                                                     std::filesystem::path const *bin_dir);

}  // namespace envy
