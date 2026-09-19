#include "product_util.h"

#include "cache.h"
#include "engine.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_key.h"

#include "doctest.h"

#include <filesystem>
#include <memory>

namespace envy {

namespace {

std::unique_ptr<pkg> make_pkg(std::string identity, pkg_type type) {
  pkg_cfg *cfg{ pkg_cfg::pool()->emplace(std::move(identity),
                                         pkg_cfg::weak_ref{},
                                         "{}",
                                         std::nullopt,
                                         nullptr,
                                         nullptr,
                                         std::vector<pkg_cfg *>{},
                                         std::nullopt,
                                         std::filesystem::path{}) };

  return std::unique_ptr<pkg>(new pkg{ .key = pkg_key(*cfg),
                                       .cfg = cfg,
                                       .cache_ptr = nullptr,
                                       .eng = nullptr,
                                       .tui_section = {},
                                       .lua = nullptr,
                                       .lock = nullptr,
                                       .canonical_identity_hash = {},
                                       .pkg_path = {},
                                       .spec_file_path = std::nullopt,
                                       .result_hash = {},
                                       .type = type });
}

}  // namespace

TEST_CASE("product_util_resolve returns joined path for cache-managed provider") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::CACHE_MANAGED) };
  // Drive-qualified on Windows: a leading backslash alone is drive-relative, not
  // absolute. The product value keeps the forward slash a spec author would write, so
  // this also pins that normalization reaches inside the joined-on component.
#ifdef _WIN32
  provider->pkg_path = std::filesystem::path("C:\\tmp\\provider");
#else
  provider->pkg_path = std::filesystem::path("/tmp/provider");
#endif
  provider->products["tool"] = product_entry{ "bin/tool", true };

  auto const value{ product_util_resolve(provider.get(), "tool") };
#ifdef _WIN32
  CHECK(value == "C:\\tmp\\provider\\bin\\tool");
#else
  CHECK(value == "/tmp/provider/bin/tool");
#endif
}

TEST_CASE("product_util_resolve returns raw value for user-managed provider") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::USER_MANAGED) };
  provider->products["tool"] = product_entry{ "raw-tool", true };

  auto const value{ product_util_resolve(provider.get(), "tool") };
  CHECK(value == "raw-tool");
}

TEST_CASE("product_util_resolve throws on missing provider") {
  CHECK_THROWS_WITH_AS(product_util_resolve(nullptr, "tool"),
                       "Product 'tool' has no provider",
                       std::runtime_error);
}

TEST_CASE("product_util_resolve throws on missing product entry") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::CACHE_MANAGED) };
  CHECK_THROWS_WITH_AS(product_util_resolve(provider.get(), "tool"),
                       "Product 'tool' not found in provider 'local.provider@v1'",
                       std::runtime_error);
}

TEST_CASE("product_util_resolve throws on empty product value") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::CACHE_MANAGED) };
  provider->products["tool"] = product_entry{ "", true };
  CHECK_THROWS_WITH_AS(product_util_resolve(provider.get(), "tool"),
                       "Product 'tool' is empty in provider 'local.provider@v1'",
                       std::runtime_error);
}

TEST_CASE("product_util_resolve throws on missing package path for cached provider") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::CACHE_MANAGED) };
  provider->products["tool"] = product_entry{ "bin/tool", true };
  CHECK_THROWS_WITH_AS(product_util_resolve(provider.get(), "tool"),
                       "Product 'tool' provider 'local.provider@v1' missing pkg path",
                       std::runtime_error);
}

// An unresolved type is the state the publication race used to produce: the products
// are readable, the type is not yet. Neither renderer may guess from it -- the wrong
// guess is an absolute path into a package that has no payload at all.

TEST_CASE("product_util_resolve refuses a provider whose type is unresolved") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::UNKNOWN) };
  provider->products["tool"] = product_entry{ "bin/tool", true };
  provider->pkg_path = std::filesystem::path("/tmp/provider");

  CHECK_THROWS_WITH_AS(product_util_resolve(provider.get(), "tool"),
                       "Product 'tool' provider 'local.provider@v1' has no resolved "
                       "package type; its spec did not finish loading",
                       std::runtime_error);
}

TEST_CASE("product_util_predict prints a user-managed value verbatim") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::USER_MANAGED) };
  cache c{ std::filesystem::path("/nonexistent-cache") };  // never consulted

  product_info const pi{ .product_name = "tool",
                         .value = "programmatic-tool",
                         .provider_canonical = "local.provider@v1",
                         .type = pkg_type::USER_MANAGED };

  CHECK(product_util_predict(pi, provider.get(), c) == "programmatic-tool");
}

TEST_CASE("product_util_predict refuses a product whose type is unresolved") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::UNKNOWN) };
  cache c{ std::filesystem::path("/nonexistent-cache") };

  product_info const pi{ .product_name = "tool",
                         .value = "programmatic-tool",
                         .provider_canonical = "local.provider@v1",
                         .type = pkg_type::UNKNOWN };

  CHECK_THROWS_WITH_AS(product_util_predict(pi, provider.get(), c),
                       "Product 'tool' provider 'local.provider@v1' has no resolved "
                       "package type; its spec did not finish loading",
                       std::runtime_error);
}

}  // namespace envy
