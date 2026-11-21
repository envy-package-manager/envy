#include "product_util.h"

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

TEST_CASE("product_util_predict_argv prints a user-managed value verbatim") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::USER_MANAGED) };
  provider->products["tool"] = product_entry{ "programmatic-tool", true };

  CHECK(product_util_predict_argv(provider.get(), "tool") ==
        std::vector<std::string>{ "programmatic-tool" });
}

TEST_CASE("product_util_predict_argv refuses a product whose type is unresolved") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::UNKNOWN) };
  provider->products["tool"] = product_entry{ "programmatic-tool", true };

  CHECK_THROWS_WITH_AS(product_util_predict_argv(provider.get(), "tool"),
                       "Product 'tool' provider 'local.provider@v1' has no resolved "
                       "package type; its spec did not finish loading",
                       std::runtime_error);
}

// Commands render through a stand-in leaf -- `identity:name` for a path product -- so
// these pin the walk, not any platform's path spelling.

namespace {

std::vector<std::string> render(pkg *provider, std::string const &name) {
  return product_util_argv(provider, name, [](pkg *p, std::string const &n) {
    return p->cfg->identity + ":" + n;
  });
}

product_entry command(std::vector<std::string> argv) {
  return product_entry{ .value = {}, .script = true, .platforms = {}, .command = argv };
}

product_entry path(std::string value) {
  return product_entry{ .value = std::move(value), .script = false };
}

void depend(pkg *from, pkg *to) {
  from->dependencies[to->cfg->identity] = { .p = to, .needed_by = pkg_phase::pkg_build };
}

}  // namespace

TEST_CASE("product_util_placeholders finds names and spans") {
  auto const holes{ product_util_placeholders("--a={{node}}:{{ yarn.cjs }}}}") };
  REQUIRE(holes.size() == 2);
  CHECK(holes[0].name == "node");
  CHECK(holes[0].begin == 4);
  CHECK(holes[0].end == 12);
  CHECK(holes[1].name == "yarn.cjs");  // trimmed
  CHECK(holes[1].begin == 13);
  CHECK(holes[1].end == 27);  // the trailing stray "}}" stays literal

  CHECK(product_util_placeholders("plain").empty());
  CHECK_THROWS_WITH_AS(product_util_placeholders("x{{node"),
                       "unterminated '{{' in 'x{{node'",
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(product_util_placeholders("{{ }}"),
                       "empty placeholder in '{{ }}'",
                       std::runtime_error);
}

TEST_CASE("product_util_argv renders products of the provider and its closure") {
  auto yarn{ make_pkg("local.yarn@v1", pkg_type::CACHE_MANAGED) };
  auto node{ make_pkg("local.node@v1", pkg_type::CACHE_MANAGED) };
  depend(yarn.get(), node.get());
  node->products["node"] = path("bin/node");
  yarn->products["yarn.cjs"] = path("lib/yarn.cjs");  // its own
  yarn->products["yarn"] =
      command({ "{{node}}", "{{yarn.cjs}}", "--node={{node}}", "-q" });

  CHECK(render(yarn.get(), "yarn") ==
        std::vector<std::string>{ "local.node@v1:node",
                                  "local.yarn@v1:yarn.cjs",
                                  "--node=local.node@v1:node",
                                  "-q" });
  CHECK(render(node.get(), "node") == std::vector<std::string>{ "local.node@v1:node" });
}

TEST_CASE("product_util_argv splices a whole-element command from the closure") {
  auto app{ make_pkg("local.app@v1", pkg_type::CACHE_MANAGED) };
  auto node{ make_pkg("local.node@v1", pkg_type::CACHE_MANAGED) };
  depend(app.get(), node.get());
  node->products["node"] = path("bin/node");
  node->products["npm-cli.js"] = path("lib/npm-cli.js");
  node->products["npm"] = command({ "{{node}}", "{{npm-cli.js}}" });
  app->products["install"] = command({ "{{npm}}", "ci" });

  CHECK(
      render(app.get(), "install") ==
      std::vector<std::string>{ "local.node@v1:node", "local.node@v1:npm-cli.js", "ci" });
}

TEST_CASE("product_util_argv walks the whole closure, from each provider's own") {
  auto app{ make_pkg("local.app@v1", pkg_type::CACHE_MANAGED) };
  auto mid{ make_pkg("local.mid@v1", pkg_type::CACHE_MANAGED) };
  auto leaf{ make_pkg("local.leaf@v1", pkg_type::CACHE_MANAGED) };
  auto sibling{ make_pkg("local.sibling@v1", pkg_type::CACHE_MANAGED) };
  depend(app.get(), mid.get());
  depend(mid.get(), leaf.get());
  depend(app.get(), sibling.get());
  leaf->products["cc"] = path("bin/cc");
  sibling->products["ld"] = path("bin/ld");
  mid->products["build"] = command({ "{{cc}}", "{{cc}}:{{cc}}" });  // two levels down
  app->products["twice"] = command({ "{{build}}", "{{build}}" });   // no false cycle
  mid->products["link"] = command({ "{{ld}}" });                    // app's, not mid's

  CHECK(render(app.get(), "twice") ==
        std::vector<std::string>{ "local.leaf@v1:cc",
                                  "local.leaf@v1:cc:local.leaf@v1:cc",
                                  "local.leaf@v1:cc",
                                  "local.leaf@v1:cc:local.leaf@v1:cc" });
  CHECK_THROWS_WITH_AS(render(mid.get(), "link"),
                       "Product 'link' in spec 'local.mid@v1' runs '{{ld}}', but nothing "
                       "in its dependency closure provides 'ld'",
                       std::runtime_error);
}

TEST_CASE("product_util_argv names the product that supplied argv[0]") {
  auto app{ make_pkg("local.app@v1", pkg_type::USER_MANAGED) };
  app->products["node"] = path("node");
  app->products["node-big"] = command({ "node", "--max-old-space-size=8192" });
  app->products["yarn"] = command({ "{{node}}", "yarn.cjs" });
  app->products["ci"] = command({ "{{node-big}}", "x" });
  app->products["lit"] = command({ "lit", "{{node}}" });

  auto const program_of{ [&](std::string const &name) {
    std::string program;
    product_util_argv(
        app.get(),
        name,
        [](pkg *, std::string const &n) { return n; },
        &program);
    return program;
  } };
  CHECK(program_of("node") == "node");    // a path product supplies itself
  CHECK(program_of("yarn") == "node");    // through a placeholder
  CHECK(program_of("ci") == "node-big");  // a spliced command's literal
  CHECK(program_of("lit") == "lit");      // its own literal
}

TEST_CASE("product_util_argv refuses a self-cycle and a null provider") {
  auto p{ make_pkg("local.p@v1", pkg_type::CACHE_MANAGED) };
  p->products["a"] = command({ "{{ a }}" });

  CHECK_THROWS_WITH_AS(render(p.get(), "a"),
                       "Product 'a' in spec 'local.p@v1' is a command cycle: a -> a",
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(render(nullptr, "a"),
                       "Product 'a' has no provider",
                       std::runtime_error);
}

TEST_CASE("product_util_argv refuses a name outside the provider's closure") {
  auto yarn{ make_pkg("local.yarn@v1", pkg_type::CACHE_MANAGED) };
  auto stray{ make_pkg("local.node@v1", pkg_type::CACHE_MANAGED) };  // not a dependency
  stray->products["node"] = path("bin/node");
  yarn->products["yarn"] = command({ "{{node}}", "x" });

  CHECK_THROWS_WITH_AS(render(yarn.get(), "yarn"),
                       "Product 'yarn' in spec 'local.yarn@v1' runs '{{node}}', but "
                       "nothing in its dependency closure provides 'node'",
                       std::runtime_error);
}

TEST_CASE("product_util_argv refuses a command cycle") {
  auto p{ make_pkg("local.p@v1", pkg_type::CACHE_MANAGED) };
  p->products["a"] = command({ "{{b}}" });
  p->products["b"] = command({ "{{a}}", "x" });

  CHECK_THROWS_WITH_AS(render(p.get(), "a"),
                       "Product 'a' in spec 'local.p@v1' is a command cycle: a -> b -> a",
                       std::runtime_error);
}

TEST_CASE("product_util_argv refuses a command embedded in part of an element") {
  auto p{ make_pkg("local.p@v1", pkg_type::CACHE_MANAGED) };
  p->products["tool"] = command({ "tool" });
  p->products["wrap"] = command({ "run", "--with={{tool}}" });

  CHECK_THROWS_WITH_AS(render(p.get(), "wrap"),
                       "Product 'wrap' in spec 'local.p@v1' embeds command product "
                       "'tool' in '--with={{tool}}'; a command can only fill a whole "
                       "element",
                       std::runtime_error);
}

TEST_CASE("a command product is not a path") {
  auto p{ make_pkg("local.p@v1", pkg_type::CACHE_MANAGED) };
  p->pkg_path = std::filesystem::path("/tmp/p");
  p->products["tool"] = command({ "{{path}}" });
  p->products["path"] = path("bin/path");

  CHECK(product_util_is_command(p.get(), "tool"));
  CHECK_FALSE(product_util_is_command(p.get(), "path"));
  CHECK_FALSE(product_util_is_command(p.get(), "missing"));
  CHECK_THROWS_WITH_AS(product_util_resolve(p.get(), "tool"),
                       "Product 'tool' in provider 'local.p@v1' is a command, not a path",
                       std::runtime_error);
}

}  // namespace envy
