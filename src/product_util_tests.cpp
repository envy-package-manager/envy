#include "product_util.h"

#include "engine.h"
#include "lua_envy.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_env.h"
#include "pkg_key.h"
#include "sol_util.h"

#include "doctest.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>

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

  CHECK(product_util_predict_argv(provider.get(), "tool", {}) ==
        std::vector<std::string>{ "programmatic-tool" });
}

TEST_CASE("product_util_predict_argv refuses a product whose type is unresolved") {
  auto provider{ make_pkg("local.provider@v1", pkg_type::UNKNOWN) };
  provider->products["tool"] = product_entry{ "programmatic-tool", true };

  CHECK_THROWS_WITH_AS(product_util_predict_argv(provider.get(), "tool", {}),
                       "Product 'tool' provider 'local.provider@v1' has no resolved "
                       "package type; its spec did not finish loading",
                       std::runtime_error);
}

// Commands render through a stand-in leaf -- `identity:name` for a path product -- so
// these pin the walk, not any platform's path spelling.

namespace {

std::string leaf(pkg *p, std::string const &n) { return p->cfg->identity + ":" + n; }

auto const kLeaf{ [](pkg *p, std::string const &n) { return leaf(p, n); } };
auto const kPkgPath{ [](pkg *p) {
  return std::filesystem::path{ "pkgs" } / p->cfg->identity;
} };

product_render const kRender{ .leaf = kLeaf, .pkg_path = kPkgPath, .state_root = "proj" };

std::vector<std::string> render(pkg *provider, std::string const &name) {
  return product_util_argv(provider, name, kRender);
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
    auto const bare{ [](pkg *, std::string const &n) { return n; } };
    product_util_argv(app.get(),
                      name,
                      { .leaf = bare, .pkg_path = kPkgPath, .state_root = {} },
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

TEST_CASE("product_util_owner honors a pin, and only a weak fallback reaches further") {
  auto app{ make_pkg("local.app@v1", pkg_type::CACHE_MANAGED) };
  auto mid{ make_pkg("local.mid@v1", pkg_type::CACHE_MANAGED) };
  auto leaf{ make_pkg("local.leaf@v1", pkg_type::CACHE_MANAGED) };
  depend(app.get(), mid.get());
  depend(mid.get(), leaf.get());
  leaf->products["node"] = path("bin/node");
  mid->products["npm"] = path("bin/npm");

  CHECK(product_util_owner(app.get(), "node") == leaf.get());  // unpinned: the closure
  CHECK(product_util_owner(app.get(), "nope") == nullptr);

  app->product_dependencies["npm"] = { .name = "npm", .provider = mid.get() };
  CHECK(product_util_owner(app.get(), "npm") == mid.get());  // pinned, declared

  app->product_dependencies["node"] = { .name = "node", .provider = mid.get() };
  CHECK_THROWS_WITH_AS(product_util_owner(app.get(), "node"),
                       "Product 'node' not found in provider 'local.mid@v1'",
                       std::runtime_error);
  app->products["run"] = command({ "{{node}}" });  // a placeholder obeys the pin too
  CHECK_THROWS_WITH_AS(render(app.get(), "run"),
                       doctest::Contains("not found in provider 'local.mid@v1'"),
                       std::runtime_error);

  app->weak_references.push_back({ .query = "node",
                                   .fallback = mid->cfg,
                                   .resolved = mid.get(),
                                   .is_product = true });
  CHECK(product_util_owner(app.get(), "node") == leaf.get());  // the fallback's closure
}

TEST_CASE("product_util_validate walks each command once, however often spliced") {
  // c0 splices c1 twice, c1 splices c2 twice, ...: expanded, 2^64 elements.
  auto p{ make_pkg("local.p@v1", pkg_type::CACHE_MANAGED) };
  constexpr int kDepth{ 64 };
  p->products["c" + std::to_string(kDepth)] = path("bin/leaf");
  for (int i{ kDepth - 1 }; i >= 0; --i) {
    std::string const next{ "{{c" + std::to_string(i + 1) + "}}" };
    p->products["c" + std::to_string(i)] = command({ next, next });
  }

  std::unordered_set<std::string> validated;
  product_util_validate(p.get(), "c0", {}, validated);
  CHECK(validated.size() == kDepth);
  product_util_validate(p.get(), "c1", {}, validated);  // already sound: no walk at all
  CHECK(validated.size() == kDepth);
}

TEST_CASE("product_util_argv refuses a NUL, which no exec carries") {
  auto p{ make_pkg("local.p@v1", pkg_type::CACHE_MANAGED) };
  p->products["tool"] = command({ "x", std::string{ "a\0b", 3 } });

  CHECK_THROWS_WITH_AS(render(p.get(), "tool"),
                       "Product 'tool' in spec 'local.p@v1' renders an argument holding "
                       "a NUL, which no program receives intact",
                       std::runtime_error);
  std::unordered_set<std::string> validated;
  CHECK_THROWS(product_util_validate(p.get(), "tool", {}, validated));
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

namespace {

// A command function: `fns` defines PRODUCTS' functions by name in the spec's state, as
// spec_fetch stashes them, with `options` as the entry's.
void give_functions(pkg *p, char const *fns, char const *options = "{}") {
  auto lua{ sol_util_make_lua_state() };
  lua->registry()[ENVY_PRODUCT_COMMANDS_RIDX] =
      lua->safe_script(std::string{ "return " } + fns).get<sol::table>();
  lua->registry()[ENVY_OPTIONS_RIDX] =
      lua->safe_script(std::string{ "return " } + options).get<sol::table>();
  p->lua.set(std::move(lua));
}

product_entry function_command() {
  return product_entry{ .value = {}, .script = true, .platforms = {}, .command = {},
                        .command_fn = true };
}

}  // namespace

TEST_CASE("a command function gets ENV's arguments and renders like a table") {
  auto yarn{ make_pkg("local.yarn@v1", pkg_type::CACHE_MANAGED) };
  auto node{ make_pkg("local.node@v1", pkg_type::CACHE_MANAGED) };
  depend(yarn.get(), node.get());
  node->products["node"] = path("bin/node");
  yarn->products["yarn"] = function_command();
  yarn->products["ci"] = command({ "{{yarn}}", "install" });  // splices the function's
  give_functions(yarn.get(),
                 R"({ yarn = function(pkg_dir, pkg_state_dir, options)
                        return { "{{node}}", pkg_dir .. "lib/yarn.cjs",
                                 "--state=" .. pkg_state_dir, "--x=" .. options.x }
                      end })",
                 "{ x = 'y' }");

  std::string const pkg_dir{ util_path_with_separator(kPkgPath(yarn.get())) };
  std::string const state{ util_path_with_separator(pkg_state_dir(yarn.get(), "proj")) };
  CHECK(render(yarn.get(), "ci") == std::vector<std::string>{ "local.node@v1:node",
                                                              pkg_dir + "lib/yarn.cjs",
                                                              "--state=" + state,
                                                              "--x=y",
                                                              "install" });
  CHECK(product_util_is_command(yarn.get(), "yarn"));
  CHECK_THROWS_WITH_AS(
      product_util_resolve(yarn.get(), "yarn"),
      "Product 'yarn' in provider 'local.yarn@v1' is a command, not a path",
      std::runtime_error);
}

TEST_CASE("a user-managed spec's command function gets a nil pkg_dir") {
  auto um{ make_pkg("local.um@v1", pkg_type::USER_MANAGED) };
  um->products["hi"] = function_command();
  give_functions(um.get(),
                 R"({ hi = function(pkg_dir) return { tostring(pkg_dir) } end })");

  CHECK(render(um.get(), "hi") == std::vector<std::string>{ "nil" });
}

TEST_CASE("a command function's result is held to a declared table's rules") {
  auto p{ make_pkg("local.p@v1", pkg_type::CACHE_MANAGED) };
  p->products["num"] = function_command();
  p->products["rel"] = function_command();
  p->products["stray"] = function_command();
  give_functions(p.get(), R"({ num = function() return 42 end,
                               rel = function() return { "bin/tool" } end,
                               stray = function() return { "{{nope}}" } end })");

  CHECK_THROWS_WITH_AS(render(p.get(), "num"),
                       "PRODUCTS entry 'num' command must be a list of strings in spec "
                       "'local.p@v1'",
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(render(p.get(), "rel"),
                       doctest::Contains("PRODUCTS entry 'rel' command starts with "
                                         "relative path 'bin/tool'"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(render(p.get(), "stray"),
                       "Product 'stray' in spec 'local.p@v1' runs '{{nope}}', but nothing "
                       "in its dependency closure provides 'nope'",
                       std::runtime_error);
}

}  // namespace envy
