#include "lua_error_formatter.h"

#include "engine.h"
#include "lua_envy.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "sol_util.h"

#include "doctest.h"

#include <memory>

namespace envy {

// Extern declarations for testing internal functions (not in public API)
extern std::optional<int> extract_line_number(std::string const &error_msg);
extern std::vector<pkg_cfg const *> build_provenance_chain(pkg_cfg const *cfg);

namespace {

// Helper fixture for creating test packages
struct formatter_test_fixture {
  pkg_cfg *cfg;
  std::unique_ptr<pkg> p;

  formatter_test_fixture(std::string identity = "test.package@v1",
                         std::string options = "{}",
                         std::filesystem::path declaring_path = {},
                         pkg_cfg *parent_cfg = nullptr) {
    cfg = pkg_cfg::pool()->emplace(std::move(identity),
                                   pkg_cfg::weak_ref{},
                                   std::move(options),
                                   std::nullopt,
                                   parent_cfg,
                                   nullptr,
                                   std::vector<pkg_cfg *>{},
                                   std::nullopt,
                                   std::move(declaring_path));

    auto lua_state = sol_util_make_lua_state();
    lua_envy_install(*lua_state);

    p = std::unique_ptr<pkg>(new pkg{ .key = pkg_key(*cfg),
                                      .cfg = cfg,
                                      .cache_ptr = nullptr,
                                      .eng = nullptr,
                                      .lua = std::move(lua_state),
                                      .lock = nullptr,
                                      .canonical_identity_hash = {},
                                      .pkg_path = std::filesystem::path{},
                                      .spec_file_path = std::nullopt,
                                      .result_hash = {},
                                      .type = pkg_type::UNKNOWN,
                                      .declared_dependencies = {},
                                      .owned_dependency_cfgs = {},
                                      .dependencies = {},
                                      .product_dependencies = {},
                                      .weak_references = {},
                                      .products = {},
                                      .resolved_weak_dependency_keys = {} });
  }
};

}  // namespace

// ============================================================================
// extract_line_number() tests
// ============================================================================

TEST_CASE("extract_line_number extracts line from standard Lua error") {
  std::string error_msg = "/path/to/spec.lua:42: assertion failed";
  auto line_num = extract_line_number(error_msg);
  REQUIRE(line_num.has_value());
  CHECK(*line_num == 42);
}

TEST_CASE("extract_line_number handles multi-digit line numbers") {
  std::string error_msg = "spec.lua:1234: some error";
  auto line_num = extract_line_number(error_msg);
  REQUIRE(line_num.has_value());
  CHECK(*line_num == 1234);
}

TEST_CASE("extract_line_number returns nullopt when no .lua: pattern") {
  std::string error_msg = "generic error message";
  auto line_num = extract_line_number(error_msg);
  CHECK_FALSE(line_num.has_value());
}

TEST_CASE("extract_line_number returns nullopt when no colon after line number") {
  std::string error_msg = "spec.lua:42";
  auto line_num = extract_line_number(error_msg);
  CHECK_FALSE(line_num.has_value());
}

TEST_CASE("extract_line_number returns nullopt for non-numeric line number") {
  std::string error_msg = "spec.lua:abc: error";
  auto line_num = extract_line_number(error_msg);
  CHECK_FALSE(line_num.has_value());
}

TEST_CASE("extract_line_number handles line number 1") {
  std::string error_msg = "spec.lua:1: error at top of file";
  auto line_num = extract_line_number(error_msg);
  REQUIRE(line_num.has_value());
  CHECK(*line_num == 1);
}

// ============================================================================
// build_provenance_chain() tests
// ============================================================================

TEST_CASE("build_provenance_chain returns single element for package without parent") {
  formatter_test_fixture f;
  auto chain = build_provenance_chain(f.cfg);
  REQUIRE(chain.size() == 1);
  CHECK(chain[0] == f.cfg);
}

TEST_CASE("build_provenance_chain builds chain with parent") {
  formatter_test_fixture parent{ "parent.package@v1" };
  formatter_test_fixture child{ "child.package@v1", "{}", {}, parent.cfg };

  auto chain = build_provenance_chain(child.cfg);
  REQUIRE(chain.size() == 2);
  CHECK(chain[0] == child.cfg);
  CHECK(chain[1] == parent.cfg);
}

TEST_CASE("build_provenance_chain builds chain with grandparent") {
  formatter_test_fixture grandparent{ "grandparent.package@v1" };
  formatter_test_fixture parent{ "parent.package@v1", "{}", {}, grandparent.cfg };
  formatter_test_fixture child{ "child.package@v1", "{}", {}, parent.cfg };

  auto chain = build_provenance_chain(child.cfg);
  REQUIRE(chain.size() == 3);
  CHECK(chain[0] == child.cfg);
  CHECK(chain[1] == parent.cfg);
  CHECK(chain[2] == grandparent.cfg);
}

TEST_CASE("build_provenance_chain handles nullptr") {
  auto chain = build_provenance_chain(nullptr);
  CHECK(chain.empty());
}

// ============================================================================
// format_lua_error() tests
// ============================================================================

TEST_CASE("format_lua_error includes identity in header") {
  formatter_test_fixture f{ "my.package@v1.2.3" };

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Lua error in my.package@v1.2.3") != std::string::npos);
}

TEST_CASE("format_lua_error includes error message") {
  formatter_test_fixture f;

  lua_error_context ctx{ .lua_error_message = "assertion failed: version required",
                         .r = f.p.get(),
                         .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("assertion failed: version required") != std::string::npos);
}

TEST_CASE("format_lua_error includes spec_file_path when present") {
  formatter_test_fixture f;
  f.p->spec_file_path = std::filesystem::path("/home/user/.envy/specs/test.lua");

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Spec file: /home/user/.envy/specs/test.lua") != std::string::npos);
}

TEST_CASE("format_lua_error includes line number when extractable") {
  formatter_test_fixture f;
  f.p->spec_file_path = std::filesystem::path("/path/to/spec.lua");

  lua_error_context ctx{ .lua_error_message = "spec.lua:42: assertion failed",
                         .r = f.p.get(),
                         .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Spec file: /path/to/spec.lua:42") != std::string::npos);
}

TEST_CASE("format_lua_error omits spec_file_path when not present") {
  formatter_test_fixture f;
  // spec_file_path not set

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Spec file:") == std::string::npos);
}

TEST_CASE("format_lua_error includes declaring_file_path") {
  formatter_test_fixture f{ "test.package@v1",
                            "{}",
                            std::filesystem::path("/path/to/manifest.lua") };

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Declared in: /path/to/manifest.lua") != std::string::npos);
}

TEST_CASE("format_lua_error includes phase when provided") {
  formatter_test_fixture f;

  lua_error_context ctx{ .lua_error_message = "test error",
                         .r = f.p.get(),
                         .phase = "build" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Phase: build") != std::string::npos);
}

TEST_CASE("format_lua_error omits phase when empty") {
  formatter_test_fixture f;

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Phase:") == std::string::npos);
}

TEST_CASE("format_lua_error includes serialized options") {
  formatter_test_fixture f{ "test.package@v1", R"({"version":"3.13.9"})" };

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find(R"(Options: {"version":"3.13.9"})") != std::string::npos);
}

TEST_CASE("format_lua_error includes options in header when non-empty") {
  formatter_test_fixture f{ "test.package@v1", R"({"version":"3.13.9"})" };

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find(R"(Lua error in test.package@v1{"version":"3.13.9"})") !=
        std::string::npos);
}

TEST_CASE("format_lua_error omits provenance chain for single package") {
  formatter_test_fixture f;

  lua_error_context ctx{ .lua_error_message = "test error", .r = f.p.get(), .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Provenance chain:") == std::string::npos);
}

TEST_CASE("format_lua_error includes provenance chain for nested dependencies") {
  formatter_test_fixture parent{ "parent.package@v1",
                                 "{}",
                                 std::filesystem::path("manifest.lua") };
  formatter_test_fixture child{ "child.package@v1",
                                "{}",
                                std::filesystem::path("parent.lua"),
                                parent.cfg };

  lua_error_context ctx{ .lua_error_message = "test error",
                         .r = child.p.get(),
                         .phase = "" };

  std::string result = format_lua_error(ctx);
  CHECK(result.find("Provenance chain:") != std::string::npos);
  CHECK(result.find("child.package@v1") != std::string::npos);
  CHECK(result.find("parent.package@v1") != std::string::npos);
  CHECK(result.find("parent.lua") != std::string::npos);
  CHECK(result.find("manifest.lua") != std::string::npos);
}

TEST_CASE("format_lua_error full example with all context") {
  formatter_test_fixture parent{ "test.python@r3.13",
                                 "{}",
                                 std::filesystem::path("/home/user/manifest.lua") };
  formatter_test_fixture child{ "test.ninja@r1.11.1",
                                R"({"version":"1.11.1"})",
                                std::filesystem::path("/home/user/.envy/specs/python.lua"),
                                parent.cfg };

  child.p->spec_file_path = std::filesystem::path("/home/user/.envy/specs/ninja.lua");

  lua_error_context ctx{ .lua_error_message =
                             "ninja.lua:42: assertion failed: version mismatch",
                         .r = child.p.get(),
                         .phase = "build" };

  std::string result = format_lua_error(ctx);

  // Verify all components present
  CHECK(result.find("Lua error in test.ninja@r1.11.1") != std::string::npos);
  CHECK(result.find(R"({"version":"1.11.1"})") != std::string::npos);
  CHECK(result.find("assertion failed: version mismatch") != std::string::npos);
  CHECK(result.find("Spec file: /home/user/.envy/specs/ninja.lua:42") !=
        std::string::npos);
  CHECK(result.find("Declared in: /home/user/.envy/specs/python.lua") !=
        std::string::npos);
  CHECK(result.find("Phase: build") != std::string::npos);
  CHECK(result.find("Provenance chain:") != std::string::npos);
  CHECK(result.find("test.ninja@r1.11.1") != std::string::npos);
  CHECK(result.find("test.python@r3.13") != std::string::npos);
}

}  // namespace envy
