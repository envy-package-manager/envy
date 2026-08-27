#include "cli.h"
#include "cmds/cmd_cache.h"
#include "cmds/cmd_deploy.h"
#include "cmds/cmd_export.h"
#include "cmds/cmd_extract.h"
#include "cmds/cmd_fetch.h"
#include "cmds/cmd_git_resolve.h"
#include "cmds/cmd_hash.h"
#include "cmds/cmd_import.h"
#include "cmds/cmd_init.h"
#include "cmds/cmd_install.h"
#include "cmds/cmd_lua.h"
#include "cmds/cmd_merge_depot.h"
#include "cmds/cmd_mirror_envy.h"
#include "cmds/cmd_package.h"
#include "cmds/cmd_product.h"
#include "cmds/cmd_run.h"
#include "cmds/cmd_shell.h"
#include "cmds/cmd_sync.h"
#include "cmds/cmd_use.h"
#include "cmds/cmd_version.h"
#include "envy_release.h"

#include "doctest.h"

#include <algorithm>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace {

// Helper to convert vector of strings to argc/argv
std::vector<char *> make_argv(std::vector<std::string> &args) {
  std::vector<char *> argv;
  for (auto &arg : args) { argv.push_back(arg.data()); }
  argv.push_back(nullptr);
  return argv;
}

}  // anonymous namespace

TEST_CASE("cli_parse: no arguments") {
  std::vector<std::string> args{ "envy" };
  auto argv{ make_argv(args) };

  auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

  // With no arguments, help text returned and no command configuration.
  CHECK_FALSE(parsed.cmd_cfg.has_value());
}

TEST_CASE("cli_parse: help lists subcommands in sorted order") {
  // CLI11 prints subcommands in registration order, so only the register_cmds list keeps
  // `envy --help` alphabetical. This notices a new command appended out of order.
  std::vector<std::string> args{ "envy", "--help" };
  auto argv{ make_argv(args) };

  auto const parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };
  REQUIRE_FALSE(parsed.cli_output.empty());

  auto const names{ [&parsed] {
    std::vector<std::string> found;
    std::istringstream in{ parsed.cli_output };
    bool subcommands{ false };
    for (std::string line; std::getline(in, line);) {
      if (line.starts_with("SUBCOMMANDS:")) {
        subcommands = true;
      } else if (subcommands && line.starts_with("  ") && line[2] != ' ') {
        // A row starts with exactly two spaces; a wrapped description is indented to the
        // description column, and OPTIONS rows are above the header.
        found.push_back(line.substr(2, line.find(' ', 2) - 2));
      }
    }
    return found;
  }() };

  REQUIRE(names.size() >= 20);
  CHECK(std::ranges::is_sorted(names));
  CHECK(names.front() == "cache");
  CHECK(names.back() == "version");
}

TEST_CASE("cli_parse: cmd_version") {
  SUBCASE("-v flag") {
    std::vector<std::string> args{ "envy", "-v" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(std::holds_alternative<envy::cmd_version::cfg>(*parsed.cmd_cfg));
  }

  SUBCASE("--version flag") {
    std::vector<std::string> args{ "envy", "--version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(std::holds_alternative<envy::cmd_version::cfg>(*parsed.cmd_cfg));
  }

  SUBCASE("version subcommand without --licenses") {
    std::vector<std::string> args{ "envy", "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_version::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->show_licenses);
  }

  SUBCASE("version --licenses flag") {
    std::vector<std::string> args{ "envy", "version", "--licenses" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_version::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->show_licenses);
  }
}

TEST_CASE("cli_parse: cmd_extract") {
  SUBCASE("archive and destination") {
    // Create temporary test archive
    auto temp_archive{ std::filesystem::temp_directory_path() /
                       "cli_test_archive.tar.gz" };
    auto temp_dest{ std::filesystem::temp_directory_path() / "cli_test_dest" };
    {
      std::ofstream temp_file{ temp_archive };
      temp_file << "fake archive\n";
    }

    std::vector<std::string> args{ "envy",
                                   "extract",
                                   temp_archive.string(),
                                   temp_dest.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    // Clean up temp file
    std::filesystem::remove(temp_archive);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_extract::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->archive_path == temp_archive);
    CHECK(cfg->destination == temp_dest);
  }

  SUBCASE("archive without destination") {
    // Create temporary test archive
    auto temp_archive{ std::filesystem::temp_directory_path() /
                       "cli_test_archive2.tar.gz" };
    {
      std::ofstream temp_file{ temp_archive };
      temp_file << "fake archive\n";
    }

    std::vector<std::string> args{ "envy", "extract", temp_archive.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    // Clean up temp file
    std::filesystem::remove(temp_archive);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_extract::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->archive_path == temp_archive);
    CHECK(cfg->destination.empty());
  }

  SUBCASE("bare single-stream compressed archive parses") {
    auto temp_archive{ std::filesystem::temp_directory_path() / "cli_test_bare.gz" };
    std::ofstream{ temp_archive } << "fake gz\n";

    std::vector<std::string> args{ "envy", "extract", temp_archive.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_archive);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_extract::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->archive_path == temp_archive);
  }

  SUBCASE("only defaults to empty") {
    auto temp_archive{ std::filesystem::temp_directory_path() /
                       "cli_test_no_only.tar.gz" };
    std::ofstream{ temp_archive } << "fake archive\n";

    std::vector<std::string> args{ "envy", "extract", temp_archive.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_archive);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_extract::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->only.empty());
  }

  SUBCASE("repeated --only accumulates and leaves the destination positional") {
    auto temp_archive{ std::filesystem::temp_directory_path() / "cli_test_only.tar.gz" };
    auto temp_dest{ std::filesystem::temp_directory_path() / "cli_test_only_dest" };
    std::ofstream{ temp_archive } << "fake archive\n";

    std::vector<std::string> args{ "envy",      "extract",          temp_archive.string(),
                                   "--only",    "bin/clang-format", "--only",
                                   "lib/clang", temp_dest.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_archive);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_extract::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->archive_path == temp_archive);
    CHECK(cfg->destination == temp_dest);
    REQUIRE(cfg->only.size() == 2);
    CHECK(cfg->only[0] == "bin/clang-format");
    CHECK(cfg->only[1] == "lib/clang");
  }
}

TEST_CASE("cli_parse: cmd_fetch") {
  SUBCASE("basic fetch command") {
    std::vector<std::string> args{ "envy",
                                   "fetch",
                                   "https://example.com/archive.tar.gz",
                                   "/tmp/local.tar.gz" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_fetch::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->source == "https://example.com/archive.tar.gz");
    CHECK(cfg->destination == std::filesystem::path("/tmp/local.tar.gz"));
    CHECK_FALSE(cfg->manifest_root.has_value());
  }

  SUBCASE("fetch with manifest root") {
    std::vector<std::string> args{ "envy",
                                   "fetch",
                                   "file://relative/path/tool.tar.gz",
                                   "/tmp/tool.tar.gz",
                                   "--manifest-root",
                                   "/workspace" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_fetch::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->source == "file://relative/path/tool.tar.gz");
    CHECK(cfg->destination == std::filesystem::path("/tmp/tool.tar.gz"));
    REQUIRE(cfg->manifest_root.has_value());
    CHECK(*cfg->manifest_root == std::filesystem::path("/workspace"));
  }
}

TEST_CASE("cli_parse: cmd_git_resolve") {
  SUBCASE("url and ref") {
    std::vector<std::string> args{ "envy",
                                   "git-resolve",
                                   "https://chromium.googlesource.com/build",
                                   "refs/tags/siso/v1.5.23" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_git_resolve::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->repo == "https://chromium.googlesource.com/build");
    CHECK(cfg->ref == "refs/tags/siso/v1.5.23");
  }

  SUBCASE("positional order is url then ref") {
    // A bare branch name and a file:// url; verifies fields are not transposed.
    std::vector<std::string> args{ "envy", "git-resolve", "file:///srv/repo.git", "main" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_git_resolve::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->repo == "file:///srv/repo.git");
    CHECK(cfg->ref == "main");
  }

  SUBCASE("full sha ref parses verbatim") {
    std::vector<std::string> args{ "envy",
                                   "git-resolve",
                                   "git@github.com:org/repo.git",
                                   "36cc599dca99520d2a0df22d62c4a87fc5a536d1" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_git_resolve::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->repo == "git@github.com:org/repo.git");
    CHECK(cfg->ref == "36cc599dca99520d2a0df22d62c4a87fc5a536d1");
  }

  SUBCASE("missing ref rejected") {
    std::vector<std::string> args{ "envy", "git-resolve", "https://example.com/repo" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("missing url and ref rejected") {
    std::vector<std::string> args{ "envy", "git-resolve" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_cache") {
  SUBCASE("bare subcommand") {
    std::vector<std::string> args{ "envy", "cache" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(std::get_if<envy::cmd_cache::cfg>(&*parsed.cmd_cfg) != nullptr);
    CHECK_FALSE(parsed.cache_root.has_value());
  }

  SUBCASE("honors global --cache-root") {
    std::vector<std::string> args{ "envy", "--cache-root", "/tmp/envy-cli-test", "cache" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(std::get_if<envy::cmd_cache::cfg>(&*parsed.cmd_cfg) != nullptr);
    REQUIRE(parsed.cache_root.has_value());
    CHECK(*parsed.cache_root == std::filesystem::path{ "/tmp/envy-cli-test" });
  }

  SUBCASE("rejects positional arguments") {
    std::vector<std::string> args{ "envy", "cache", "extra" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_hash") {
  SUBCASE("single file") {
    auto temp_path{ std::filesystem::temp_directory_path() / "cli_test_hash.txt" };
    {
      std::ofstream temp_file{ temp_path };
      temp_file << "test content\n";
    }

    std::vector<std::string> args{ "envy", "hash", temp_path.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_path);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_hash::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->paths.size() == 1);
    CHECK(cfg->paths[0] == temp_path);
    CHECK_FALSE(cfg->prefix.has_value());
  }

  SUBCASE("multiple files") {
    auto temp_a{ std::filesystem::temp_directory_path() / "cli_test_hash_a.txt" };
    auto temp_b{ std::filesystem::temp_directory_path() / "cli_test_hash_b.txt" };
    {
      std::ofstream fa{ temp_a };
      fa << "a\n";
      std::ofstream fb{ temp_b };
      fb << "b\n";
    }

    std::vector<std::string> args{ "envy", "hash", temp_a.string(), temp_b.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_a);
    std::filesystem::remove(temp_b);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_hash::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->paths.size() == 2);
  }

  SUBCASE("missing paths rejected") {
    std::vector<std::string> args{ "envy", "hash" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("with --prefix") {
    auto temp_path{ std::filesystem::temp_directory_path() / "cli_test_hash_pfx.txt" };
    {
      std::ofstream temp_file{ temp_path };
      temp_file << "test\n";
    }

    std::vector<std::string> args{ "envy",
                                   "hash",
                                   "--prefix",
                                   "s3://bucket/",
                                   temp_path.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_path);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_hash::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->prefix.has_value());
    CHECK(*cfg->prefix == "s3://bucket/");
    CHECK(cfg->paths.size() == 1);
  }

  SUBCASE("directory accepted as path") {
    auto temp_dir{ std::filesystem::temp_directory_path() };

    std::vector<std::string> args{ "envy", "hash", temp_dir.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_hash::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->paths.size() == 1);
    CHECK(cfg->paths[0] == temp_dir);
  }
}

TEST_CASE("cli_parse: cmd_use") {
  auto const parse{ [](std::vector<std::string> args) {
    auto argv{ make_argv(args) };
    return envy::cli_parse(static_cast<int>(args.size()), argv.data());
  } };

  SUBCASE("version only") {
    auto const parsed{ parse({ "envy", "use", "0.1.6" }) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_use::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->version == "0.1.6");
    CHECK_FALSE(cfg->manifest_path.has_value());
    CHECK_FALSE(cfg->mirror.has_value());
    CHECK_FALSE(cfg->subproject);
    CHECK_FALSE(cfg->pin_sums);
    CHECK_FALSE(cfg->no_pin_sums);
    CHECK_FALSE(cfg->force);
  }

  SUBCASE("missing version rejected") {
    auto const parsed{ parse({ "envy", "use" }) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("all options") {
    auto const parsed{ parse({ "envy",
                               "use",
                               "1.2.3",
                               "--manifest",
                               "/proj/envy.lua",
                               "--mirror",
                               "s3://bucket/envy",
                               "--pin-sums" }) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_use::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->version == "1.2.3");
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path{ "/proj/envy.lua" });
    REQUIRE(cfg->mirror.has_value());
    CHECK(*cfg->mirror == "s3://bucket/envy");
    CHECK(cfg->pin_sums);
  }

  SUBCASE("--subproject") {
    auto const parsed{ parse({ "envy", "use", "0.1.6", "--subproject" }) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_use::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->subproject);
  }

  SUBCASE("--no-pin-sums and --force") {
    auto const parsed{ parse({ "envy", "use", "0.1.6", "--no-pin-sums", "--force" }) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_use::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->no_pin_sums);
    CHECK(cfg->force);
  }

  SUBCASE("--subproject excludes --manifest") {
    auto const parsed{ parse(
        { "envy", "use", "0.1.6", "--subproject", "--manifest", "/proj/envy.lua" }) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--pin-sums excludes --no-pin-sums") {
    auto const parsed{ parse({ "envy", "use", "0.1.6", "--pin-sums", "--no-pin-sums" }) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_lua") {
  SUBCASE("with script path") {
    // Create temporary test file
    auto temp_path{ std::filesystem::temp_directory_path() / "cli_test_script.lua" };
    {
      std::ofstream temp_file{ temp_path };
      temp_file << "-- test script\n";
    }

    std::vector<std::string> args{ "envy", "lua", temp_path.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    // Clean up temp file
    std::filesystem::remove(temp_path);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_lua::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->script_path == temp_path);
  }
}

// TEST_CASE removed: cmd_playground has been deleted

TEST_CASE("cli_parse: cmd_package") {
  SUBCASE("identity only") {
    std::vector<std::string> args{ "envy", "package", "vendor.gcc@v2" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_package::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->identity == "vendor.gcc@v2");
    CHECK_FALSE(cfg->manifest_path.has_value());
  }

  SUBCASE("with manifest") {
    std::vector<std::string> args{ "envy",
                                   "package",
                                   "vendor.gcc@v2",
                                   "--manifest",
                                   "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_package::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->identity == "vendor.gcc@v2");
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(cfg->manifest_path->string() == "/path/to/envy.lua");
  }

  SUBCASE("--ignore-depot flag") {
    std::vector<std::string> args{ "envy", "package", "vendor.gcc@v2", "--ignore-depot" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_package::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->ignore_depot);
  }

  SUBCASE("ignore_depot defaults to false") {
    std::vector<std::string> args{ "envy", "package", "vendor.gcc@v2" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_package::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->ignore_depot);
  }
}

TEST_CASE("cli_parse: cmd_product") {
  SUBCASE("product only") {
    std::vector<std::string> args{ "envy", "product", "tool" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->product_name == "tool");
    CHECK_FALSE(cfg->manifest_path.has_value());
    CHECK_FALSE(cfg->json);
  }

  SUBCASE("with manifest") {
    std::vector<std::string> args{ "envy",
                                   "product",
                                   "tool",
                                   "--manifest",
                                   "/tmp/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->product_name == "tool");
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path("/tmp/envy.lua"));
    CHECK_FALSE(cfg->json);
  }

  SUBCASE("no product name lists all") {
    std::vector<std::string> args{ "envy", "product" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->product_name.empty());
    CHECK_FALSE(cfg->json);
  }

  SUBCASE("json flag enabled") {
    std::vector<std::string> args{ "envy", "product", "--json" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->product_name.empty());
    CHECK(cfg->json);
  }

  SUBCASE("json with product name") {
    std::vector<std::string> args{ "envy", "product", "tool", "--json" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->product_name == "tool");
    CHECK(cfg->json);
  }
}

TEST_CASE("cli_parse: global --project") {
  SUBCASE("lands in the selected command's config") {
    std::vector<std::string> args{ "envy", "--project", ".", "product", "tool" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->product_name == "tool");
    REQUIRE(cfg->project_dir.has_value());
    CHECK(*cfg->project_dir == std::filesystem::path("."));
  }

  SUBCASE("reaches every manifest-loading command") {
    auto const anchor_of{ [](std::vector<std::string> args)
                              -> std::optional<std::filesystem::path> {
      auto argv{ make_argv(args) };
      auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };
      REQUIRE(parsed.cmd_cfg.has_value());
      return std::visit(
          [](auto const &c) -> std::optional<std::filesystem::path> {
            if constexpr (std::derived_from<std::decay_t<decltype(c)>,
                                            envy::cmd_project_anchor>) {
              return c.project_dir;
            }
            return std::nullopt;
          },
          *parsed.cmd_cfg);
    } };

    CHECK(anchor_of({ "envy", "--project", ".", "install" }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "sync" }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "deploy" }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "package", "a.b@v1" }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "export" }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "import", "--dir", "." }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "cache" }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "use", "1.2.3" }).has_value());
    CHECK(anchor_of({ "envy", "--project", ".", "run", "ls" }).has_value());
  }

  SUBCASE("a repeat wins, so an injected anchor can be overridden") {
    // The bin dir launcher injects --project ahead of the caller's argv.
    std::vector<std::string> args{ "envy",    "--project", "..",   "--project",
                                   ".",       "product",   "tool" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->project_dir.has_value());
    CHECK(*cfg->project_dir == std::filesystem::path("."));
  }

  SUBCASE("coexists with --manifest, which outranks it") {
    std::vector<std::string> args{ "envy",       "--project", ".",
                                   "product",    "tool",      "--manifest",
                                   "/tmp/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->manifest_path.has_value());
    REQUIRE(cfg->project_dir.has_value());
  }

  SUBCASE("rejects a non-directory") {
    std::vector<std::string> args{ "envy", "--project", "no/such/dir", "product" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("absent leaves the anchor unset") {
    std::vector<std::string> args{ "envy", "product", "tool" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->project_dir.has_value());
  }

  SUBCASE("a command that never loads a manifest ignores it") {
    // The visit is over every alternative, including ones with no anchor to write.
    std::vector<std::string> args{ "envy", "--project", ".", "hash", "a" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(std::get_if<envy::cmd_hash::cfg>(&*parsed.cmd_cfg) != nullptr);
  }

  SUBCASE("survives the --version early return") {
    std::vector<std::string> args{ "envy", "--project", ".", "--version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(std::get_if<envy::cmd_version::cfg>(&*parsed.cmd_cfg) != nullptr);
  }

  SUBCASE("with no subcommand prints help rather than crashing") {
    std::vector<std::string> args{ "envy", "--project", "." };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("behind a prefix_command subcommand it belongs to the child") {
    std::vector<std::string> args{ "envy", "run", "rsync", "--project", "." };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_run::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->project_dir.has_value());
    REQUIRE(cfg->command.size() == 3);
    CHECK(cfg->command[0] == "rsync");
    CHECK(cfg->command[1] == "--project");
  }
}

TEST_CASE("subproject_anchor: --subproject means nearest to the CWD") {
  std::optional<std::filesystem::path> const anchor{ std::filesystem::path{ "/bin/dir" } };
  CHECK(envy::subproject_anchor(false, anchor) == anchor);
  CHECK_FALSE(envy::subproject_anchor(true, anchor).has_value());
  CHECK_FALSE(envy::subproject_anchor(false, std::nullopt).has_value());
}

TEST_CASE("cli_parse: verbose flag") {
  std::vector<std::string> args{ "envy", "--verbose", "version" };
  auto argv{ make_argv(args) };

  auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

  REQUIRE(parsed.cmd_cfg.has_value());
  REQUIRE(parsed.verbosity.has_value());
  CHECK(parsed.verbosity == envy::tui::level::TUI_DEBUG);
  CHECK(parsed.decorated_logging);
}

TEST_CASE("cli_parse: default verbosity is info, undecorated") {
  std::vector<std::string> args{ "envy", "version" };
  auto argv{ make_argv(args) };

  auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

  REQUIRE(parsed.cmd_cfg.has_value());
  REQUIRE(parsed.verbosity.has_value());
  CHECK(parsed.verbosity == envy::tui::level::TUI_INFO);
  CHECK_FALSE(parsed.decorated_logging);
}

TEST_CASE("cli_parse: quiet flag raises threshold to warn") {
  for (auto const *flag : { "-q", "--quiet" }) {
    std::vector<std::string> args{ "envy", flag, "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    REQUIRE(parsed.verbosity.has_value());
    CHECK(parsed.verbosity == envy::tui::level::TUI_WARN);
    CHECK_FALSE(parsed.decorated_logging);
  }
}

TEST_CASE("cli_parse: verbose and quiet are mutually exclusive") {
  std::vector<std::string> args{ "envy", "--verbose", "--quiet", "version" };
  auto argv{ make_argv(args) };

  auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

  CHECK_FALSE(parsed.cmd_cfg.has_value());
  CHECK_FALSE(parsed.cli_output.empty());
}

TEST_CASE("cli_parse: trace flag enables structured outputs") {
  SUBCASE("stderr trace explicit") {
    std::vector<std::string> args{ "envy", "--trace=stderr", "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    REQUIRE(parsed.verbosity.has_value());
    // --trace is sink-only: it does not change log level or decoration.
    CHECK(parsed.verbosity == envy::tui::level::TUI_INFO);
    CHECK_FALSE(parsed.decorated_logging);
    REQUIRE(parsed.trace_outputs.size() == 1);
    CHECK(parsed.trace_outputs[0].type == envy::tui::trace_output_type::std_err);
    CHECK_FALSE(parsed.trace_outputs[0].file_path.has_value());
  }

  SUBCASE("file trace target") {
    std::filesystem::path const trace_path{ "/tmp/envy-trace.jsonl" };
    std::vector<std::string> args{ "envy",
                                   "--trace=file:" + trace_path.string(),
                                   "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    REQUIRE(parsed.verbosity.has_value());
    CHECK(parsed.verbosity == envy::tui::level::TUI_INFO);
    REQUIRE(parsed.trace_outputs.size() == 1);
    CHECK(parsed.trace_outputs[0].type == envy::tui::trace_output_type::file);
    REQUIRE(parsed.trace_outputs[0].file_path.has_value());
    CHECK(parsed.trace_outputs[0].file_path->string() == trace_path.string());
  }

  SUBCASE("multiple trace destinations") {
    std::filesystem::path const trace_path{ "/tmp/envy-trace.jsonl" };
    std::vector<std::string> args{ "envy",
                                   "--trace=stderr,file:" + trace_path.string(),
                                   "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    REQUIRE(parsed.verbosity.has_value());
    CHECK(parsed.verbosity == envy::tui::level::TUI_INFO);
    REQUIRE(parsed.trace_outputs.size() == 2);
    CHECK(parsed.trace_outputs[0].type == envy::tui::trace_output_type::std_err);
    CHECK_FALSE(parsed.trace_outputs[0].file_path.has_value());
    CHECK(parsed.trace_outputs[1].type == envy::tui::trace_output_type::file);
    REQUIRE(parsed.trace_outputs[1].file_path.has_value());
    CHECK(parsed.trace_outputs[1].file_path->string() == trace_path.string());
  }

  SUBCASE("invalid trace spec rejected") {
    std::vector<std::string> args{ "envy", "--trace=bogus", "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
    CHECK(parsed.trace_outputs.empty());
  }
}

TEST_CASE("cli_parse: global cache-root flag") {
  SUBCASE("no cache-root by default") {
    std::vector<std::string> args{ "envy", "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cache_root.has_value());
  }

  SUBCASE("cache-root with value") {
    std::vector<std::string> args{ "envy", "--cache-root", "/tmp/cache", "version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    REQUIRE(parsed.cache_root.has_value());
    CHECK(*parsed.cache_root == std::filesystem::path("/tmp/cache"));
  }

  SUBCASE("cache-root works with sync command") {
    std::vector<std::string> args{ "envy", "--cache-root", "/my/cache", "sync" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    REQUIRE(parsed.cache_root.has_value());
    CHECK(*parsed.cache_root == std::filesystem::path("/my/cache"));
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
  }
}

TEST_CASE("cli_parse: cmd_install") {
  SUBCASE("no arguments (install all)") {
    std::vector<std::string> args{ "envy", "install" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_install::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->queries.empty());
    CHECK_FALSE(cfg->manifest_path.has_value());
  }

  SUBCASE("with queries") {
    std::vector<std::string> args{ "envy", "install", "gcc", "binutils" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_install::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 2);
    CHECK(cfg->queries[0] == "gcc");
    CHECK(cfg->queries[1] == "binutils");
  }

  SUBCASE("with --manifest") {
    std::vector<std::string> args{ "envy", "install", "--manifest", "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_install::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->queries.empty());
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path("/path/to/envy.lua"));
  }

  SUBCASE("queries with --manifest") {
    std::vector<std::string> args{ "envy",
                                   "install",
                                   "gcc",
                                   "--manifest",
                                   "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_install::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 1);
    CHECK(cfg->queries[0] == "gcc");
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path("/path/to/envy.lua"));
  }

  SUBCASE("--ignore-depot flag") {
    std::vector<std::string> args{ "envy", "install", "--ignore-depot" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_install::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->ignore_depot);
  }

  SUBCASE("ignore_depot defaults to false") {
    std::vector<std::string> args{ "envy", "install" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_install::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->ignore_depot);
  }
}

TEST_CASE("cli_parse: cmd_deploy flags") {
  SUBCASE("default flags") {
    std::vector<std::string> args{ "envy", "deploy" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->strict);
    CHECK_FALSE(cfg->subproject);
  }

  SUBCASE("--strict flag") {
    std::vector<std::string> args{ "envy", "deploy", "--strict" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->strict);
  }

  SUBCASE("--subproject flag") {
    std::vector<std::string> args{ "envy", "deploy", "--subproject" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->subproject);
  }

  SUBCASE("with identities") {
    std::vector<std::string> args{ "envy", "deploy", "pkg1", "pkg2" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->identities.size() == 2);
    CHECK(cfg->identities[0] == "pkg1");
    CHECK(cfg->identities[1] == "pkg2");
  }

  SUBCASE("--manifest flag") {
    std::vector<std::string> args{ "envy", "deploy", "--manifest", "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path("/path/to/envy.lua"));
  }

  SUBCASE("--subproject with --manifest rejected") {
    std::vector<std::string> args{ "envy",
                                   "deploy",
                                   "--subproject",
                                   "--manifest",
                                   "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_shell") {
  SUBCASE("bash") {
    std::vector<std::string> args{ "envy", "shell", "bash" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_shell::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->shell == "bash");
  }

  SUBCASE("zsh") {
    std::vector<std::string> args{ "envy", "shell", "zsh" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_shell::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->shell == "zsh");
  }

  SUBCASE("fish") {
    std::vector<std::string> args{ "envy", "shell", "fish" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_shell::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->shell == "fish");
  }

  SUBCASE("powershell") {
    std::vector<std::string> args{ "envy", "shell", "powershell" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_shell::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->shell == "powershell");
  }

  SUBCASE("missing shell argument") {
    std::vector<std::string> args{ "envy", "shell" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("invalid shell rejected") {
    std::vector<std::string> args{ "envy", "shell", "csh" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_run") {
  SUBCASE("basic command") {
    std::vector<std::string> args{ "envy", "run", "ls" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_run::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->command.size() == 1);
    CHECK(cfg->command[0] == "ls");
  }

  SUBCASE("command with arguments") {
    std::vector<std::string> args{ "envy", "run", "python3", "-c", "print(1)" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_run::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->command.size() == 3);
    CHECK(cfg->command[0] == "python3");
    CHECK(cfg->command[1] == "-c");
    CHECK(cfg->command[2] == "print(1)");
  }

  SUBCASE("no command") {
    std::vector<std::string> args{ "envy", "run" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_run::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->command.empty());
  }

  SUBCASE("child flags not intercepted") {
    std::vector<std::string> args{ "envy", "run", "grep", "--version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_run::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->command.size() == 2);
    CHECK(cfg->command[0] == "grep");
    CHECK(cfg->command[1] == "--version");
  }

  SUBCASE("double-dash sentinel preserved in command vector") {
    std::vector<std::string> args{ "envy", "run", "python3", "--", "script.py" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_run::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->command.size() == 3);
    CHECK(cfg->command[0] == "python3");
    CHECK(cfg->command[1] == "--");
    CHECK(cfg->command[2] == "script.py");
  }
}

TEST_CASE("cli_parse: cmd_sync") {
  SUBCASE("no arguments (sync all)") {
    std::vector<std::string> args{ "envy", "sync" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->queries.empty());
    CHECK_FALSE(cfg->strict);
    CHECK_FALSE(cfg->subproject);
    CHECK(cfg->platform_flag.empty());
  }

  SUBCASE("with queries") {
    std::vector<std::string> args{ "envy", "sync", "gcc", "binutils" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 2);
    CHECK(cfg->queries[0] == "gcc");
    CHECK(cfg->queries[1] == "binutils");
  }

  SUBCASE("--manifest flag") {
    std::vector<std::string> args{ "envy", "sync", "--manifest", "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path("/path/to/envy.lua"));
  }

  SUBCASE("--strict flag") {
    std::vector<std::string> args{ "envy", "sync", "--strict" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->strict);
  }

  SUBCASE("--subproject flag") {
    std::vector<std::string> args{ "envy", "sync", "--subproject" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->subproject);
  }

  SUBCASE("--subproject with --manifest rejected") {
    std::vector<std::string> args{ "envy",
                                   "sync",
                                   "--subproject",
                                   "--manifest",
                                   "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--platform posix") {
    std::vector<std::string> args{ "envy", "sync", "--platform", "posix" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "posix");
  }

  SUBCASE("--platform windows") {
    std::vector<std::string> args{ "envy", "sync", "--platform", "windows" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "windows");
  }

  SUBCASE("--platform all") {
    std::vector<std::string> args{ "envy", "sync", "--platform", "all" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "all");
  }

  SUBCASE("invalid --platform value rejected") {
    std::vector<std::string> args{ "envy", "sync", "--platform", "linux" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--ignore-depot flag") {
    std::vector<std::string> args{ "envy", "sync", "--ignore-depot" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->ignore_depot);
  }

  SUBCASE("ignore_depot defaults to false") {
    std::vector<std::string> args{ "envy", "sync" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_sync::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->ignore_depot);
  }
}

TEST_CASE("cli_parse: cmd_deploy --platform") {
  SUBCASE("default (no --platform)") {
    std::vector<std::string> args{ "envy", "deploy" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag.empty());
  }

  SUBCASE("--platform posix") {
    std::vector<std::string> args{ "envy", "deploy", "--platform", "posix" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "posix");
  }

  SUBCASE("--platform windows") {
    std::vector<std::string> args{ "envy", "deploy", "--platform", "windows" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "windows");
  }

  SUBCASE("--platform all") {
    std::vector<std::string> args{ "envy", "deploy", "--platform", "all" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_deploy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "all");
  }

  SUBCASE("invalid --platform value rejected") {
    std::vector<std::string> args{ "envy", "deploy", "--platform", "linux" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_init --platform") {
  SUBCASE("default (no --platform)") {
    std::vector<std::string> args{ "envy", "init", "/tmp/proj", "/tmp/bin" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag.empty());
  }

  SUBCASE("--platform posix") {
    std::vector<std::string> args{ "envy",     "init",       "/tmp/proj",
                                   "/tmp/bin", "--platform", "posix" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "posix");
  }

  SUBCASE("--platform windows") {
    std::vector<std::string> args{ "envy",     "init",       "/tmp/proj",
                                   "/tmp/bin", "--platform", "windows" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "windows");
  }

  SUBCASE("--platform all") {
    std::vector<std::string> args{ "envy",     "init",       "/tmp/proj",
                                   "/tmp/bin", "--platform", "all" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->platform_flag == "all");
  }

  SUBCASE("invalid --platform value rejected") {
    std::vector<std::string> args{ "envy",     "init",       "/tmp/proj",
                                   "/tmp/bin", "--platform", "macos" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_init --pin-sums") {
  SUBCASE("default is off") {
    // Attestation is opt-in: `envy init` must keep working with no network reachable.
    std::vector<std::string> args{ "envy", "init", "/tmp/proj", "/tmp/bin" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->pin_sums);
  }

  SUBCASE("--pin-sums is a flag, taking no value") {
    std::vector<std::string> args{ "envy", "init", "/tmp/proj", "/tmp/bin", "--pin-sums" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->pin_sums);
  }

  SUBCASE("--pin-sums composes with --mirror") {
    // The pin is fetched from whichever mirror the project will actually bootstrap from,
    // so these two have to be usable together.
    std::vector<std::string> args{ "envy",     "init",       "/tmp/proj", "/tmp/bin",
                                   "--mirror", "s3://b/rel", "--pin-sums" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->pin_sums);
    REQUIRE(cfg->mirror.has_value());
    CHECK(*cfg->mirror == "s3://b/rel");
  }
}

TEST_CASE("cli_parse: cmd_init --envy-version") {
  SUBCASE("absent by default") {
    // Omitted means "this binary", the only version an init can stamp without re-execing.
    std::vector<std::string> args{ "envy", "init", "/tmp/proj", "/tmp/bin" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->envy_version.has_value());
  }

  SUBCASE("takes a version value") {
    std::vector<std::string> args{ "envy",     "init",           "/tmp/proj",
                                   "/tmp/bin", "--envy-version", "1.2.3" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->envy_version.has_value());
    CHECK(*cfg->envy_version == "1.2.3");
  }

  SUBCASE("composes with --mirror and --pin-sums") {
    // The requested version is downloaded from that mirror, and the child pins the sums of
    // the version it turned out to be, so all three have to be usable together.
    std::vector<std::string> args{ "envy",     "init",           "/tmp/proj",
                                   "/tmp/bin", "--envy-version", "9.8.7",
                                   "--mirror", "s3://b/rel",     "--pin-sums" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_init::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->envy_version.has_value());
    CHECK(*cfg->envy_version == "9.8.7");
    REQUIRE(cfg->mirror.has_value());
    CHECK(*cfg->mirror == "s3://b/rel");
    CHECK(cfg->pin_sums);
  }

  SUBCASE("value is required when the flag appears") {
    std::vector<std::string> args{ "envy",
                                   "init",
                                   "/tmp/proj",
                                   "/tmp/bin",
                                   "--envy-version" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_export") {
  SUBCASE("no arguments (export all)") {
    std::vector<std::string> args{ "envy", "export" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->queries.empty());
    CHECK_FALSE(cfg->output_dir.has_value());
    CHECK_FALSE(cfg->manifest_path.has_value());
  }

  SUBCASE("single query") {
    std::vector<std::string> args{ "envy", "export", "arm.gcc@r2" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 1);
    CHECK(cfg->queries[0] == "arm.gcc@r2");
  }

  SUBCASE("multiple queries") {
    std::vector<std::string> args{ "envy", "export", "gcc", "binutils" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 2);
    CHECK(cfg->queries[0] == "gcc");
    CHECK(cfg->queries[1] == "binutils");
  }

  SUBCASE("with -o output directory") {
    std::vector<std::string> args{ "envy", "export", "arm.gcc@r2", "-o", "/tmp/out" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 1);
    CHECK(cfg->queries[0] == "arm.gcc@r2");
    REQUIRE(cfg->output_dir.has_value());
    CHECK(*cfg->output_dir == std::filesystem::path("/tmp/out"));
  }

  SUBCASE("with --manifest") {
    std::vector<std::string> args{ "envy",
                                   "export",
                                   "arm.gcc@r2",
                                   "--manifest",
                                   "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 1);
    CHECK(cfg->queries[0] == "arm.gcc@r2");
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path("/path/to/envy.lua"));
  }

  SUBCASE("with --depot-prefix") {
    std::vector<std::string> args{ "envy",
                                   "export",
                                   "--depot-prefix",
                                   "s3://my-bucket/cache/" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->depot_prefix.has_value());
    CHECK(*cfg->depot_prefix == "s3://my-bucket/cache/");
  }

  SUBCASE("without --depot-prefix") {
    std::vector<std::string> args{ "envy", "export" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->depot_prefix.has_value());
  }

  SUBCASE("--depot-prefix with -o and queries") {
    std::vector<std::string> args{ "envy",
                                   "export",
                                   "arm.gcc@r2",
                                   "-o",
                                   "/tmp/out",
                                   "--depot-prefix",
                                   "https://cdn.example.com/" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->queries.size() == 1);
    CHECK(cfg->queries[0] == "arm.gcc@r2");
    REQUIRE(cfg->output_dir.has_value());
    CHECK(*cfg->output_dir == std::filesystem::path("/tmp/out"));
    REQUIRE(cfg->depot_prefix.has_value());
    CHECK(*cfg->depot_prefix == "https://cdn.example.com/");
  }

  SUBCASE("--ignore-depot flag") {
    std::vector<std::string> args{ "envy", "export", "--ignore-depot" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->ignore_depot);
  }

  SUBCASE("ignore_depot defaults to false") {
    std::vector<std::string> args{ "envy", "export" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_export::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->ignore_depot);
  }
}

TEST_CASE("cli_parse: cmd_import") {
  SUBCASE("valid archive path") {
    // Create temporary test archive
    auto temp_archive{ std::filesystem::temp_directory_path() /
                       "arm.gcc@r2-darwin-arm64-blake3-abcdef01.tar.zst" };
    {
      std::ofstream temp_file{ temp_archive };
      temp_file << "fake archive\n";
    }

    std::vector<std::string> args{ "envy", "import", temp_archive.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_archive);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_import::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->archive_path == temp_archive);
    CHECK_FALSE(cfg->dir.has_value());
  }

  SUBCASE("neither archive nor --dir rejected") {
    std::vector<std::string> args{ "envy", "import" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("nonexistent archive rejected") {
    std::vector<std::string> args{ "envy", "import", "/nonexistent/archive.tar.zst" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--dir parses") {
    auto temp_dir{ std::filesystem::temp_directory_path() / "envy-import-test-dir" };
    std::filesystem::create_directories(temp_dir);

    std::vector<std::string> args{ "envy", "import", "--dir", temp_dir.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_dir);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_import::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->dir.has_value());
    CHECK(*cfg->dir == temp_dir);
    CHECK(cfg->archive_path.empty());
  }

  SUBCASE("--dir and archive rejected") {
    auto temp_dir{ std::filesystem::temp_directory_path() / "envy-import-test-dir2" };
    std::filesystem::create_directories(temp_dir);
    auto temp_archive{ std::filesystem::temp_directory_path() /
                       "arm.gcc@r2-darwin-arm64-blake3-abcdef01.tar.zst" };
    {
      std::ofstream temp_file{ temp_archive };
      temp_file << "fake archive\n";
    }

    std::vector<std::string> args{ "envy",
                                   "import",
                                   "--dir",
                                   temp_dir.string(),
                                   temp_archive.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_dir);
    std::filesystem::remove(temp_archive);

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--dir with nonexistent directory rejected") {
    std::vector<std::string> args{ "envy", "import", "--dir", "/nonexistent/dir" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--manifest parses") {
    auto temp_dir{ std::filesystem::temp_directory_path() / "envy-import-test-dir3" };
    std::filesystem::create_directories(temp_dir);

    std::vector<std::string> args{ "envy",       "import",
                                   "--dir",      temp_dir.string(),
                                   "--manifest", "/path/to/envy.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_dir);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_import::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->dir.has_value());
    REQUIRE(cfg->manifest_path.has_value());
    CHECK(*cfg->manifest_path == std::filesystem::path("/path/to/envy.lua"));
  }

  SUBCASE(".txt manifest file accepted") {
    auto temp_txt{ std::filesystem::temp_directory_path() /
                   "envy-import-test-manifest.txt" };
    {
      std::ofstream f{ temp_txt };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
           "https://cdn/pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n";
    }

    std::vector<std::string> args{ "envy", "import", temp_txt.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_txt);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_import::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->archive_path == temp_txt);
    CHECK_FALSE(cfg->dir.has_value());
  }

  SUBCASE("--checksums with --dir parses") {
    auto temp_dir{ std::filesystem::temp_directory_path() / "envy-import-test-dir4" };
    std::filesystem::create_directories(temp_dir);
    auto temp_cksum{ std::filesystem::temp_directory_path() / "envy-import-test-ck.txt" };
    {
      std::ofstream f{ temp_cksum };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
           "pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",        "import",
                                   "--dir",       temp_dir.string(),
                                   "--checksums", temp_cksum.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_dir);
    std::filesystem::remove(temp_cksum);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_import::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->dir.has_value());
    REQUIRE(cfg->checksums_path.has_value());
    CHECK(*cfg->checksums_path == temp_cksum);
  }

  SUBCASE("--checksums with nonexistent file rejected") {
    auto temp_dir{ std::filesystem::temp_directory_path() / "envy-import-test-dir5" };
    std::filesystem::create_directories(temp_dir);

    std::vector<std::string> args{ "envy",        "import",
                                   "--dir",       temp_dir.string(),
                                   "--checksums", "/nonexistent/file.txt" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_dir);

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_mirror_envy") {
  SUBCASE("local destination") {
    std::vector<std::string> args{ "envy", "mirror-envy", "1.2.3", "./stage" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_mirror_envy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->version == "1.2.3");
    CHECK(cfg->dest == "./stage");
    // --from defaults to envy's own release URL so the common case needs no flag.
    CHECK(cfg->from == envy::kEnvyReleaseDownloadUrl);
  }

  SUBCASE("s3 destination") {
    std::vector<std::string> args{ "envy",
                                   "mirror-envy",
                                   "1.2.3",
                                   "s3://my-bucket/releases" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_mirror_envy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->dest == "s3://my-bucket/releases");
  }

  SUBCASE("--from overrides the source mirror") {
    std::vector<std::string> args{ "envy",
                                   "mirror-envy",
                                   "2.0.0",
                                   "./stage",
                                   "--from=s3://upstream/envy" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_mirror_envy::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->version == "2.0.0");
    CHECK(cfg->from == "s3://upstream/envy");
  }

  SUBCASE("version is required") {
    std::vector<std::string> args{ "envy", "mirror-envy" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("destination is required") {
    // No implicit staging directory: a bare version would otherwise have to guess.
    std::vector<std::string> args{ "envy", "mirror-envy", "1.2.3" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }
}

TEST_CASE("cli_parse: cmd_merge_depot") {
  SUBCASE("single depot manifest") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-a.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  "
           "pkg@v1-darwin-arm64-blake3-aaaa.tar.zst\n";
    }

    std::vector<std::string> args{ "envy", "merge-depot", temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->depot_manifests.size() == 1);
    CHECK(cfg->depot_manifests[0] == temp);
    CHECK_FALSE(cfg->existing_path.has_value());
    CHECK_FALSE(cfg->strict);
  }

  SUBCASE("multiple depot manifests") {
    auto temp_a{ std::filesystem::temp_directory_path() / "envy-merge-depot-a.txt" };
    auto temp_b{ std::filesystem::temp_directory_path() / "envy-merge-depot-b.txt" };
    {
      std::ofstream f{ temp_a };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ temp_b };
      f << "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  b.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   temp_a.string(),
                                   temp_b.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp_a);
    std::filesystem::remove(temp_b);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->depot_manifests.size() == 2);
  }

  SUBCASE("no arguments rejected") {
    std::vector<std::string> args{ "envy", "merge-depot" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("nonexistent file rejected") {
    std::vector<std::string> args{ "envy", "merge-depot", "/nonexistent/file.txt" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("with --existing") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-new.txt" };
    auto existing{ std::filesystem::temp_directory_path() /
                   "envy-merge-depot-existing.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ existing };
      f << "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  b.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   temp.string(),
                                   "--existing",
                                   existing.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(existing);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->existing_path.has_value());
    CHECK(*cfg->existing_path == existing.string());
  }

  SUBCASE("nonexistent --existing accepted at parse time") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-new2.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   temp.string(),
                                   "--existing",
                                   "/nonexistent/file.txt" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->existing_path.has_value());
    CHECK(*cfg->existing_path == "/nonexistent/file.txt");
  }

  SUBCASE("remote URL --existing accepted") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-remote.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   temp.string(),
                                   "--existing",
                                   "s3://my-bucket/depot/existing.txt" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->existing_path.has_value());
    CHECK(*cfg->existing_path == "s3://my-bucket/depot/existing.txt");
  }

  SUBCASE("--strict flag") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-strict.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy", "merge-depot", "--strict", temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->strict);
  }

  SUBCASE("strict defaults false") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-def.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy", "merge-depot", temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->strict);
  }

  SUBCASE("--strict and --existing combined") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-combo.txt" };
    auto existing{ std::filesystem::temp_directory_path() /
                   "envy-merge-depot-combo-ex.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ existing };
      f << "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  b.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",       "merge-depot",     "--strict",
                                   "--existing", existing.string(), temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(existing);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->strict);
    REQUIRE(cfg->existing_path.has_value());
    CHECK(*cfg->existing_path == existing.string());
    CHECK(cfg->depot_manifests.size() == 1);
  }

  SUBCASE("with --retain") {
    auto temp{ std::filesystem::temp_directory_path() /
               "envy-merge-depot-retain-new.txt" };
    auto retain{ std::filesystem::temp_directory_path() /
                 "envy-merge-depot-retain-list.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ retain };
      f << "a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   "--retain",
                                   retain.string(),
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(retain);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->retain.has_value());
    CHECK(cfg->retain->path == retain.string());
  }

  SUBCASE("remote URL --retain accepted") {
    auto temp{ std::filesystem::temp_directory_path() /
               "envy-merge-depot-retain-remote.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   "--retain",
                                   "s3://bucket/retain.txt",
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->retain.has_value());
    CHECK(cfg->retain->path == "s3://bucket/retain.txt");
  }

  SUBCASE("retain defaults empty") {
    auto temp{ std::filesystem::temp_directory_path() /
               "envy-merge-depot-retain-def.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy", "merge-depot", temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->retain.has_value());
  }

  SUBCASE("--retain and --existing and --strict combined") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-all-new.txt" };
    auto existing{ std::filesystem::temp_directory_path() /
                   "envy-merge-depot-all-ex.txt" };
    auto retain{ std::filesystem::temp_directory_path() /
                 "envy-merge-depot-all-retain.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ existing };
      f << "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  b.tar.zst\n";
    }
    {
      std::ofstream f{ retain };
      f << "a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",          "merge-depot",     "--strict",
                                   "--existing",    existing.string(), "--retain",
                                   retain.string(), temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(existing);
    std::filesystem::remove(retain);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->strict);
    REQUIRE(cfg->existing_path.has_value());
    CHECK(*cfg->existing_path == existing.string());
    REQUIRE(cfg->retain.has_value());
    CHECK(cfg->retain->path == retain.string());
    CHECK(cfg->depot_manifests.size() == 1);
  }

  SUBCASE("--retain-prefix accepted") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-rp-new.txt" };
    auto retain{ std::filesystem::temp_directory_path() /
                 "envy-merge-depot-rp-retain.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ retain };
      f << "a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",          "merge-depot",     "--retain",
                                   retain.string(), "--retain-prefix", "s3://bucket/",
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(retain);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->retain_prefix.has_value());
    CHECK(*cfg->retain_prefix == "s3://bucket/");
  }

  SUBCASE("--retain-prefix defaults empty") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-rp-def.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy", "merge-depot", temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->retain_prefix.has_value());
  }

  SUBCASE("--retain-prefix without --retain parses (runtime error)") {
    auto temp{ std::filesystem::temp_directory_path() /
               "envy-merge-depot-rp-no-retain.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   "--retain-prefix",
                                   "s3://bucket/",
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->retain_prefix.has_value());
    CHECK(*cfg->retain_prefix == "s3://bucket/");
    CHECK_FALSE(cfg->retain.has_value());
  }

  SUBCASE("--retain-prefix combined with all flags") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-rp-all.txt" };
    auto existing{ std::filesystem::temp_directory_path() /
                   "envy-merge-depot-rp-all-ex.txt" };
    auto retain{ std::filesystem::temp_directory_path() /
                 "envy-merge-depot-rp-all-retain.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ existing };
      f << "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  b.tar.zst\n";
    }
    {
      std::ofstream f{ retain };
      f << "a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",          "merge-depot",     "--strict",
                                   "--existing",    existing.string(), "--retain",
                                   retain.string(), "--retain-prefix", "s3://bucket/",
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(existing);
    std::filesystem::remove(retain);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->strict);
    REQUIRE(cfg->existing_path.has_value());
    CHECK(*cfg->existing_path == existing.string());
    REQUIRE(cfg->retain.has_value());
    CHECK(cfg->retain->path == retain.string());
    REQUIRE(cfg->retain_prefix.has_value());
    CHECK(*cfg->retain_prefix == "s3://bucket/");
    CHECK(cfg->depot_manifests.size() == 1);
  }

  SUBCASE("--retain-s3-ls accepted") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-s3ls-new.txt" };
    auto s3ls{ std::filesystem::temp_directory_path() / "envy-merge-depot-s3ls.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ s3ls };
      f << "2024-01-15 12:34:56       1234 a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   "--retain-s3-ls",
                                   s3ls.string(),
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(s3ls);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->retain.has_value());
    CHECK(cfg->retain->path == s3ls.string());
    CHECK(cfg->retain->fmt == envy::cmd_merge_depot::retain_format::S3_LS);
  }

  SUBCASE("--retain-s3-ls remote URL accepted") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-s3ls-url.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",
                                   "merge-depot",
                                   "--retain-s3-ls",
                                   "http://example.com/retain.txt",
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->retain.has_value());
    CHECK(cfg->retain->path == "http://example.com/retain.txt");
    CHECK(cfg->retain->fmt == envy::cmd_merge_depot::retain_format::S3_LS);
  }

  SUBCASE("--retain-s3-ls defaults empty") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-s3ls-def.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy", "merge-depot", temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK_FALSE(cfg->retain.has_value());
  }

  SUBCASE("--retain and --retain-s3-ls mutually exclusive") {
    auto temp{ std::filesystem::temp_directory_path() /
               "envy-merge-depot-s3ls-mutex.txt" };
    auto retain{ std::filesystem::temp_directory_path() /
                 "envy-merge-depot-s3ls-mutex-r.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ retain };
      f << "a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",          "merge-depot",    "--retain",
                                   retain.string(), "--retain-s3-ls", retain.string(),
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(retain);

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--retain-prefix with --retain-s3-ls accepted") {
    auto temp{ std::filesystem::temp_directory_path() / "envy-merge-depot-s3ls-rp.txt" };
    auto s3ls{ std::filesystem::temp_directory_path() / "envy-merge-depot-s3ls-rp-r.txt" };
    {
      std::ofstream f{ temp };
      f << "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  a.tar.zst\n";
    }
    {
      std::ofstream f{ s3ls };
      f << "2024-01-15 12:34:56       1234 a.tar.zst\n";
    }

    std::vector<std::string> args{ "envy",        "merge-depot",     "--retain-s3-ls",
                                   s3ls.string(), "--retain-prefix", "s3://bucket/",
                                   temp.string() };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    std::filesystem::remove(temp);
    std::filesystem::remove(s3ls);

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_merge_depot::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->retain.has_value());
    CHECK(cfg->retain->path == s3ls.string());
    CHECK(cfg->retain->fmt == envy::cmd_merge_depot::retain_format::S3_LS);
    REQUIRE(cfg->retain_prefix.has_value());
    CHECK(*cfg->retain_prefix == "s3://bucket/");
  }
}

// The cache-test drivers only exist in binaries built with ENVY_FUNCTIONAL_TESTER;
// the unit test target defines it so this parsing is covered like any other command.
#ifdef ENVY_FUNCTIONAL_TESTER

TEST_CASE("cli_parse: cache-test ensure-package") {
  SUBCASE("all four positionals required") {
    std::vector<std::string> args{ "envy",   "cache-test", "ensure-package", "gcc",
                                   "darwin", "arm64",      "deadbeef" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_cache_ensure_package::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->identity == "gcc");
    CHECK(cfg->platform == "darwin");
    CHECK(cfg->arch == "arm64");
    CHECK(cfg->hash_prefix == "deadbeef");
  }

  SUBCASE("missing positionals are rejected") {
    std::vector<std::string> args{ "envy", "cache-test", "ensure-package", "gcc" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK(parsed.cli_output.find("Error") != std::string::npos);
  }
}

TEST_CASE("cli_parse: cache-test ensure-spec") {
  SUBCASE("identity only leaves the source key empty") {
    // Empty means "key on the identity", resolved at execute time so the default
    // lives in one place rather than being baked into every caller.
    std::vector<std::string> args{ "envy", "cache-test", "ensure-spec", "envy.cmake@v1" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_cache_ensure_spec::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->identity == "envy.cmake@v1");
    CHECK(cfg->source.empty());
  }

  SUBCASE("--source carries the cache key input") {
    std::vector<std::string> args{ "envy",        "cache-test",
                                   "ensure-spec", "envy.cmake@v1",
                                   "--source",    "https://example.com/spec.lua" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_cache_ensure_spec::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->source == "https://example.com/spec.lua");
  }

  SUBCASE("missing identity is rejected") {
    std::vector<std::string> args{ "envy", "cache-test", "ensure-spec" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK(parsed.cli_output.find("Error") != std::string::npos);
  }
}

TEST_CASE("cli_parse: cache-test choreography options") {
  SUBCASE("barrier, crash, and failure options land on the cfg") {
    std::vector<std::string> args{ "envy",
                                   "cache-test",
                                   "ensure-spec",
                                   "envy.cmake@v1",
                                   "--test-id=abc123",
                                   "--barrier-dir=/tmp/barriers",
                                   "--barrier-signal=took-lock",
                                   "--barrier-wait=go",
                                   "--barrier-signal-after=done",
                                   "--barrier-wait-after=released",
                                   "--crash-after=250",
                                   "--fail-before-complete" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_cache_ensure_spec::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->test_id == "abc123");
    CHECK(cfg->barrier_dir == std::filesystem::path{ "/tmp/barriers" });
    CHECK(cfg->barrier_signal == "took-lock");
    CHECK(cfg->barrier_wait == "go");
    CHECK(cfg->barrier_signal_after == "done");
    CHECK(cfg->barrier_wait_after == "released");
    CHECK(cfg->crash_after_ms == 250);
    CHECK(cfg->fail_before_complete);
  }

  SUBCASE("defaults when no choreography is requested") {
    std::vector<std::string> args{ "envy",   "cache-test", "ensure-package", "gcc",
                                   "darwin", "arm64",      "deadbeef" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_cache_ensure_package::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->test_id.empty());
    CHECK(cfg->barrier_dir.empty());
    CHECK(cfg->crash_after_ms == -1);
    CHECK_FALSE(cfg->fail_before_complete);
  }
}

#endif  // ENVY_FUNCTIONAL_TESTER
