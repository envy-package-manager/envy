#include "product_expand.h"

#include "cache.h"
#include "engine.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_env.h"
#include "pkg_key.h"
#include "util.h"

#include "doctest.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace envy {

namespace {

#ifdef _WIN32
std::filesystem::path const kRoot{ "C:\\c" };
#else
std::filesystem::path const kRoot{ "/c" };
#endif

cache &test_cache() {  // computes paths only
  static cache c{ kRoot / "cache" };
  return c;
}

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
                                       .cache_ptr = &test_cache(),
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

// A cache-managed package installed at kRoot/<dir>.
std::unique_ptr<pkg> installed(std::string identity, std::string const &dir) {
  auto p{ make_pkg(std::move(identity), pkg_type::CACHE_MANAGED) };
  p->pkg_path = kRoot / dir;
  return p;
}

product_entry path(std::string value) {
  return product_entry{ .value = std::move(value), .script = false };
}

product_entry command(std::vector<std::string> argv) {
  return product_entry{ .value = std::move(argv) };
}

// `consumer` depends on `provider` for `name`, as a product dependency.
void provide(pkg *consumer,
             pkg *provider,
             std::string const &name,
             pkg_phase needed_by = pkg_phase::pkg_build) {
  consumer->dependencies[provider->cfg->identity] = { .p = provider,
                                                      .needed_by = needed_by };
  consumer->product_dependencies[name] = { .name = name,
                                           .needed_by = needed_by,
                                           .provider = provider,
                                           .constraint_identity = {} };
}

std::string expand(pkg *p,
                   std::string_view text,
                   resolved_shell const &shell,
                   pkg_phase phase = pkg_phase::pkg_build) {
  shell_env_t const env;
  std::filesystem::path const cwd{ "." };
  std::filesystem::path const root{ "scratch" };
  return product_expand(text,
                        { .p = p,
                          .phase = phase,
                          .what = "BUILD",
                          .shell = shell,
                          .env = env,
                          .cwd = cwd,
                          .state_root = root });
}

std::string native(std::filesystem::path const &p) {
  return util_path_to_utf8(std::filesystem::path{ p }.make_preferred());
}

#ifdef _WIN32
shell_choice const kShell{ shell_choice::powershell };
#else
shell_choice const kShell{ shell_choice::bash };
#endif

struct fixture {
  std::unique_ptr<pkg> c{ installed("local.c@v1", "c") };
  std::unique_ptr<pkg> node{ installed("local.node@v1", "node") };
  std::unique_ptr<pkg> yarn{ installed("local.yarn@v1", "yarn") };

  fixture() {
    node->products["node"] = path("bin/node");
    node->products["npm"] = path("npm.cmd");
    node->products["NPM"] = path("NPM.Bat");
    yarn->products["yarn.cjs"] = path("lib/yarn.cjs");
    yarn->products["yarn"] = command({ "{{node}}", "{{yarn.cjs}}" });
    provide(yarn.get(), node.get(), "node");
  }

  // What `{{yarn}}` renders as, quoted for `shell`.
  std::string yarn_line(shell_choice shell) const {
    return shell_quote_argv(
        { native(kRoot / "node" / "bin/node"), native(kRoot / "yarn" / "lib/yarn.cjs") },
        shell);
  }
};

}  // namespace

TEST_CASE_FIXTURE(fixture,
                  "product_expand: a path product is its text, wherever it sits") {
  provide(c.get(), node.get(), "node");
  std::string const n{ native(kRoot / "node" / "bin/node") };
  CHECK(expand(c.get(), "\"{{node}}\" --n={{node}} '{{{{x}}}}'", kShell) ==
        "\"" + n + "\" --n=" + n + " '{{x}}'");
}

TEST_CASE_FIXTURE(fixture, "product_expand: a directory loses its trailing separator") {
  node->products["share"] = path("share/");
  provide(c.get(), node.get(), "share");
  CHECK(expand(c.get(), "\"-DS={{share}}\"", kShell) ==
        "\"-DS=" + native(kRoot / "node" / "share") + "\"");
}

TEST_CASE_FIXTURE(fixture,
                  "product_expand: a payload path a shell could misread is refused") {
  provide(c.get(), node.get(), "node");
  for (std::string const dir : { "my node",
                                 "a$b",
                                 "a'b",
                                 "a\"b",
                                 "a(b",
                                 "a&b",
                                 "a;b",
                                 "a%b",
                                 "a!b",
                                 "a,b",
                                 "a=b",
                                 "a#b",
                                 "a*b",
                                 "a?b" }) {
    CAPTURE(dir);
    node->pkg_path = kRoot / dir;
    CHECK_THROWS_WITH_AS(expand(c.get(), "{{node}}", kShell),
                         doctest::Contains("under a cache root"),
                         std::runtime_error);
  }
  for (std::string const dir : { "cach\xC3\xA9", "a@r0~label", "x+y", "a.b_c-d" }) {
    CAPTURE(dir);
    node->pkg_path = kRoot / util_path_from_utf8(dir);
    CHECK_NOTHROW(expand(c.get(), "{{node}}", kShell));
  }
  node->pkg_path = kRoot / "node";  // safe root, unsafe product value
  node->products["app"] = path("My App.app/bin/x");
  provide(c.get(), node.get(), "app");
  CHECK_THROWS_WITH_AS(expand(c.get(), "{{app}}", kShell),
                       doctest::Contains("a product value a shell would misread"),
                       std::runtime_error);
}

#ifndef _WIN32
TEST_CASE_FIXTURE(fixture, "product_expand: POSIX refuses a backslash and a `:~`") {
  node->products["bs"] = path("a\\b");
  node->products["tilde"] = path("a:~b");
  provide(c.get(), node.get(), "bs");
  provide(c.get(), node.get(), "tilde");
  CHECK_THROWS(expand(c.get(), "{{bs}}", shell_choice::bash));
  CHECK_THROWS(expand(c.get(), "X={{tilde}}", shell_choice::bash));
}
#endif

TEST_CASE_FIXTURE(fixture, "product_expand: non-ASCII PowerShell and cmd would misread") {
  provide(c.get(), node.get(), "node");
  node->pkg_path = kRoot / util_path_from_utf8(
                               "D\xE2\x80\x99"
                               "Angelo");  // U+2019: a quote
  CHECK_THROWS(expand(c.get(), "{{node}}", shell_choice::powershell));
  node->pkg_path = kRoot / util_path_from_utf8("J\xC3\xB6rg");  // cmd reads an OEM page
  CHECK_NOTHROW(expand(c.get(), "{{node}}", shell_choice::powershell));
  CHECK_THROWS(expand(c.get(), "{{node}}", shell_choice::cmd));
}

TEST_CASE_FIXTURE(fixture, "product_expand: a user-managed value is inserted as written") {
  auto py{ make_pkg("local.py@v1", pkg_type::USER_MANAGED) };
  py->products["py"] = path("py -3");
  provide(c.get(), py.get(), "py");
  CHECK(expand(c.get(), "{{py}} x.py", kShell) == "py -3 x.py");
}

TEST_CASE_FIXTURE(fixture, "product_expand: a command product fills a whole word") {
  provide(c.get(), yarn.get(), "yarn");
  std::string const posix{ yarn_line(shell_choice::bash) };
  CHECK(expand(c.get(), "{{yarn}} install", shell_choice::bash) == posix + " install");
  CHECK(expand(c.get(), "v=$({{yarn}} -v);{{yarn}}|`{{yarn}}`", shell_choice::bash) ==
        "v=$(" + posix + " -v);" + posix + "|`" + posix + "`");
  for (std::string_view const text : { "X={{yarn}} make", "{{yarn}}=1", "\"{{yarn}}\"" }) {
    CAPTURE(text);
    CHECK_THROWS_WITH_AS(expand(c.get(), text, shell_choice::bash),
                         doctest::Contains("fills whole words"),
                         std::runtime_error);
  }
  CHECK_NOTHROW(expand(c.get(), "$(& {{yarn}}),{{yarn}}", shell_choice::powershell));
  CHECK_THROWS(expand(c.get(), "--y={{yarn}}", shell_choice::powershell));
  CHECK_NOTHROW(expand(c.get(), "a&{{yarn}}|b", shell_choice::cmd));
  CHECK_THROWS(expand(c.get(), "x;{{yarn}}", shell_choice::cmd));  // `;` is text to cmd
}

TEST_CASE_FIXTURE(fixture, "product_expand: a command product stays out of quoted text") {
  provide(c.get(), yarn.get(), "yarn");
  auto const refused{ [&](std::string_view text, shell_choice shell) {
    CAPTURE(text);
    CHECK_THROWS_WITH_AS(expand(c.get(), text, shell),
                         doctest::Contains("inside quoted text"),
                         std::runtime_error);
  } };
  auto const allowed{ [&](std::string_view text, shell_choice shell) {
    CAPTURE(text);
    CHECK_NOTHROW(expand(c.get(), text, shell));
  } };
  auto const bash{ shell_choice::bash };
  refused("printf '%s\\n' \" {{yarn}} \"", bash);
  refused("echo ' {{yarn}} '", bash);
  refused("cat <<EOF\n {{yarn}}\nEOF\n", bash);
  refused("cat <<-'EOF'\n\t{{yarn}}\n\tEOF\n", bash);
  refused("echo \"$(echo \") {{yarn}} \"", bash);  // the `"` inside `$( )` nests
  allowed("v=\"$( {{yarn}} -v )\"", bash);
  allowed("v=\"` {{yarn}} -v `\"", bash);
  allowed("echo \"a\" {{yarn}} 'b'", bash);
  allowed("# don't\n{{yarn}}", bash);  // a comment's quote opens nothing
  allowed("echo \\' {{yarn}}", bash);
  allowed("cat <<EOF\n{{{{x}}}}\nEOF\n{{yarn}}", bash);
  allowed("echo $((1 + 2)) {{yarn}}", bash);

  auto const pwsh{ shell_choice::powershell };
  refused("Write-Output \" {{yarn}} \"", pwsh);
  refused("Write-Output ' {{yarn}} '", pwsh);
  refused("$s = @\"\n {{yarn}}\n\"@", pwsh);
  refused("$s = @'\n {{yarn}}\n'@", pwsh);
  allowed("$v = \"$( {{yarn}} -v )\"", pwsh);
  allowed("Write-Output 'it''s' ; {{yarn}}", pwsh);
  allowed("<# don't #> {{yarn}}", pwsh);
  allowed("# don't\n{{yarn}}", pwsh);

  auto const cmd{ shell_choice::cmd };
  refused("echo \" {{yarn}} \"", cmd);
  allowed("echo \"a\" & {{yarn}}", cmd);
  allowed("rem don't \"\n{{yarn}}", cmd);
}

TEST_CASE_FIXTURE(fixture,
                  "product_expand: an author's own call operator is not doubled") {
  provide(c.get(), yarn.get(), "yarn");
  std::string const line{ yarn_line(shell_choice::powershell) };
  REQUIRE(line.starts_with("& "));
  CHECK(expand(c.get(), "{{yarn}} -v", shell_choice::powershell) == line + " -v");
  CHECK(expand(c.get(), "& {{yarn}} -v", shell_choice::powershell) ==
        "& " + line.substr(2) + " -v");
  CHECK(expand(c.get(), "a && {{yarn}}", shell_choice::powershell) == "a && " + line);
}

TEST_CASE_FIXTURE(fixture, "product_expand: cmd runs a batch file only behind call") {
  provide(c.get(), node.get(), "npm");
  provide(c.get(), node.get(), "NPM");
  for (std::string_view const text : { "{{npm}} ci",
                                       "cd x && {{npm}} ci",
                                       "\"{{npm}}\" ci",
                                       "@{{npm}} ci",
                                       "if x (\n  {{npm}} ci\n)",
                                       "for %%f in (a) do {{npm}} ci",
                                       ") else {{npm}} ci",
                                       "{{NPM}} ci" }) {
    CAPTURE(text);
    CHECK_THROWS_WITH_AS(expand(c.get(), text, shell_choice::cmd),
                         doctest::Contains("so the script resumes"),
                         std::runtime_error);
  }
  std::string const npm{ native(kRoot / "node" / "npm.cmd") };
  CHECK(expand(c.get(), "call {{npm}} ci", shell_choice::cmd) == "call " + npm + " ci");
  CHECK(expand(c.get(), "copy {{npm}} out", shell_choice::cmd) == "copy " + npm + " out");
  CHECK_NOTHROW(expand(c.get(), "for %%f in ({{npm}}) do echo %%f", shell_choice::cmd));
  CHECK_NOTHROW(expand(c.get(), "echo({{npm}}", shell_choice::cmd));
}

TEST_CASE_FIXTURE(fixture, "product_expand: a custom shell takes paths and escapes") {
  resolved_shell const tcl{ custom_shell_inline{ .argv = { "tclsh" } } };
  provide(c.get(), node.get(), "node");
  provide(c.get(), yarn.get(), "yarn");
  CHECK(expand(c.get(), "exec {{node}} {{{{a b}}}}", tcl) ==
        "exec " + native(kRoot / "node" / "bin/node") + " {{a b}}");
  CHECK_THROWS_WITH_AS(expand(c.get(), "exec {{yarn}}", tcl),  // unknown quoting
                       doctest::Contains("a custom shell has no quoting"),
                       std::runtime_error);
  CHECK_THROWS_WITH_AS(expand(c.get(), "set l {{a b} {c d}}", tcl),  // expanded first
                       doctest::Contains("write '{{{{a b} {c d}}}}'"),
                       std::runtime_error);
}

TEST_CASE("product_expand: without a package, only escapes expand") {
  CHECK(expand(nullptr, "echo '{{{{x}}}}'", kShell) == "echo '{{x}}'");
  CHECK_THROWS_WITH_AS(expand(nullptr, "echo {{x}}", kShell),
                       "BUILD: '{{x}}' names a product, but no package's phase runs this",
                       std::runtime_error);
}

TEST_CASE_FIXTURE(fixture, "product_expand: a phase names only what its edges reached") {
  provide(c.get(), node.get(), "node", pkg_phase::pkg_install);
  CHECK_THROWS_WITH_AS(
      expand(c.get(), "{{node}}", kShell),
      "BUILD: product 'node' needed_by 'install' but accessed during 'build'",
      std::runtime_error);
  CHECK_NOTHROW(expand(c.get(), "{{node}}", kShell, pkg_phase::pkg_install));
  CHECK_THROWS_WITH_AS(expand(c.get(), "{{.ID}}", kShell),
                       "BUILD: pkg 'local.c@v1' does not declare product dependency on "
                       "'.ID'; for the braces themselves write '{{{{.ID}}}}'",
                       std::runtime_error);
}

TEST_CASE_FIXTURE(fixture, "product_expand: own products render before install") {
  c->pkg_path.clear();
  c->products["out"] = path("out.json");
  c->products["self"] = command({ "{{node}}", "{{out}}" });
  provide(c.get(), node.get(), "node", pkg_phase::pkg_install);
  std::filesystem::path const pending{ pkg_predicted_path(c.get()) };
  REQUIRE(pending.string().starts_with((kRoot / "cache").string()));

  CHECK(expand(c.get(), "{{out}}", kShell) == native(pending / "out.json"));
  CHECK_THROWS_WITH_AS(  // an own command's splice is gated like a direct `{{node}}`
      expand(c.get(), "{{self}}", kShell),
      "BUILD: '{{self}}': product 'node' needed_by 'install' but accessed during 'build'",
      std::runtime_error);
  CHECK(expand(c.get(), "{{self}}", kShell, pkg_phase::pkg_install) ==
        shell_quote_argv(
            { native(kRoot / "node" / "bin/node"), native(pending / "out.json") },
            kShell));
}

TEST_CASE_FIXTURE(fixture, "product_expand_check: refuses ahead what rendering would") {
  node->pkg_path.clear();  // not installed yet: the check reads predicted paths
  provide(c.get(), node.get(), "node");
  provide(c.get(), yarn.get(), "yarn");
  CHECK_NOTHROW(product_expand_check(c.get(),
                                     pkg_phase::pkg_build,
                                     "{{node}} '{{{{x}}}}' {{ {{y}} }}",
                                     "BUILD",
                                     kShell));
  CHECK_THROWS_WITH_AS(
      product_expand_check(c.get(), pkg_phase::pkg_build, "{{nope}}", "STAGE", kShell),
      doctest::Contains("STAGE: pkg 'local.c@v1' does not declare"),
      std::runtime_error);
  CHECK_THROWS_WITH_AS(
      product_expand_check(c.get(), pkg_phase::pkg_build, "{{node", "STAGE", kShell),
      doctest::Contains("STAGE: unterminated '{{' in '{{node'"),
      std::runtime_error);
  CHECK_THROWS_WITH_AS(product_expand_check(c.get(),
                                            pkg_phase::pkg_build,
                                            "X={{yarn}}",
                                            "BUILD",
                                            shell_choice::bash),
                       doctest::Contains("fills whole words"),
                       std::runtime_error);
  // Unknown shell (DEFAULT_SHELL function): shell rules skipped, names still checked.
  CHECK_NOTHROW(product_expand_check(c.get(),
                                     pkg_phase::pkg_build,
                                     "X={{yarn}}",
                                     "BUILD",
                                     std::nullopt));
  CHECK_THROWS(product_expand_check(c.get(),
                                    pkg_phase::pkg_build,
                                    "{{nope}}",
                                    "BUILD",
                                    std::nullopt));

  // Own table commands are checked recursively against the phase's edges.
  c->products["inner"] = command({ "{{node}}" });
  c->products["outer"] = command({ "{{inner}}", "-x" });
  provide(c.get(), node.get(), "node", pkg_phase::pkg_install);
  CHECK_THROWS_WITH_AS(
      product_expand_check(c.get(), pkg_phase::pkg_build, "{{outer}}", "BUILD", kShell),
      doctest::Contains("'{{inner}}': product 'node' needed_by 'install'"),
      std::runtime_error);
  CHECK_NOTHROW(product_expand_check(c.get(),
                                     pkg_phase::pkg_install,
                                     "{{outer}}",
                                     "INSTALL",
                                     kShell));
}

}  // namespace envy
