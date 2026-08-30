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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
  // Subcommands print in registration order, so only the register_cmds list keeps
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

  SUBCASE("bare subcommand reports") {
    std::vector<std::string> args{ "envy", "cache" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    auto const *cfg{ std::get_if<envy::cmd_cache::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->act == envy::cmd_cache::cfg::action::REPORT);
  }

  SUBCASE("--root selects PRINT_ROOT") {
    std::vector<std::string> args{ "envy", "cache", "--root" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    auto const *cfg{ std::get_if<envy::cmd_cache::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->act == envy::cmd_cache::cfg::action::PRINT_ROOT);
  }

  SUBCASE("--user-wide-root selects PRINT_USER_WIDE_ROOT") {
    std::vector<std::string> args{ "envy", "cache", "--user-wide-root" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    auto const *cfg{ std::get_if<envy::cmd_cache::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->act == envy::cmd_cache::cfg::action::PRINT_USER_WIDE_ROOT);
  }

  SUBCASE("--local selects SET_LOCAL") {
    std::vector<std::string> args{ "envy", "cache", "--local" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    auto const *cfg{ std::get_if<envy::cmd_cache::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->act == envy::cmd_cache::cfg::action::SET_LOCAL);
  }

  SUBCASE("--shared selects SET_SHARED") {
    std::vector<std::string> args{ "envy", "cache", "--shared" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    auto const *cfg{ std::get_if<envy::cmd_cache::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    CHECK(cfg->act == envy::cmd_cache::cfg::action::SET_SHARED);
  }

  // One action per invocation: combining a report with a mutation would have to pick an
  // order, and there is no sensible one.
  SUBCASE("--local and --shared are mutually exclusive") {
    std::vector<std::string> args{ "envy", "cache", "--local", "--shared" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--root excludes --local") {
    std::vector<std::string> args{ "envy", "cache", "--root", "--local" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--root excludes --user-wide-root") {
    std::vector<std::string> args{ "envy", "cache", "--root", "--user-wide-root" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--user-wide-root excludes --local") {
    std::vector<std::string> args{ "envy", "cache", "--user-wide-root", "--local" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
  }

  SUBCASE("--root excludes --shared") {
    std::vector<std::string> args{ "envy", "cache", "--root", "--shared" };
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
    auto const anchor_of{
      [](std::vector<std::string> args) -> std::optional<std::filesystem::path> {
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
      }
    };

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
    std::vector<std::string> args{ "envy", "--project", "..",  "--project",
                                   ".",    "product",   "tool" };
    auto argv{ make_argv(args) };

    auto parsed{ envy::cli_parse(static_cast<int>(args.size()), argv.data()) };

    REQUIRE(parsed.cmd_cfg.has_value());
    auto const *cfg{ std::get_if<envy::cmd_product::cfg>(&*parsed.cmd_cfg) };
    REQUIRE(cfg != nullptr);
    REQUIRE(cfg->project_dir.has_value());
    CHECK(*cfg->project_dir == std::filesystem::path("."));
  }

  SUBCASE("coexists with --manifest, which outranks it") {
    std::vector<std::string> args{ "envy",       "--project",    ".", "product", "tool",
                                   "--manifest", "/tmp/envy.lua" };
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

// ===========================================================================
// Exhaustive characterization of the parser's cross-cutting behavior. Every
// registered option and flag gets a value-lands check here; every rule that can
// reject an argv gets a rejection check. This is the contract the parser owes
// its callers, independent of which library implements it.
// ===========================================================================

namespace {

std::string joined(std::vector<std::string> const &args) {
  std::string out;
  for (auto const &a : args) { out += a + " "; }
  return out;
}

envy::cli_args parse_argv(std::vector<std::string> args) {
  auto argv{ make_argv(args) };
  return envy::cli_parse(static_cast<int>(args.size()), argv.data());
}

// Every rejection looks the same to main(): no config to run, something to print.
void rejects(std::vector<std::string> args) {
  std::string const argv_text{ joined(args) };
  INFO("argv: ", argv_text);
  auto const parsed{ parse_argv(std::move(args)) };
  CHECK_FALSE(parsed.cmd_cfg.has_value());
  CHECK_FALSE(parsed.cli_output.empty());
}

// Parses, selects the expected command, and prints nothing.
template <typename cfg_t>
cfg_t accepts(std::vector<std::string> args) {
  std::string const argv_text{ joined(args) };
  INFO("argv: ", argv_text);
  auto const parsed{ parse_argv(std::move(args)) };
  REQUIRE(parsed.cmd_cfg.has_value());
  CHECK(parsed.cli_output.empty());
  auto const *cfg{ std::get_if<cfg_t>(&*parsed.cmd_cfg) };
  REQUIRE(cfg != nullptr);
  return *cfg;
}

// Help is the other non-command outcome: something to print, nothing to run.
void prints_help(std::vector<std::string> args, std::string_view needle) {
  std::string const argv_text{ joined(args) };
  INFO("argv: ", argv_text);
  auto const parsed{ parse_argv(std::move(args)) };
  CHECK_FALSE(parsed.cmd_cfg.has_value());
  REQUIRE_FALSE(parsed.cli_output.empty());
  CHECK(parsed.cli_output.find(needle) != std::string::npos);
  CHECK(parsed.cli_output.find("Error: ") == std::string::npos);
}

void put_env(char const *name, char const *value) {
#ifdef _WIN32
  // Windows has no unset; an empty value erases the variable, which is exactly the
  // distinction ENVY_IGNORE_DEPOT does not draw anyway.
  _putenv_s(name, value ? value : "");
#else
  if (value) {
    setenv(name, value, 1);
  } else {
    unsetenv(name);
  }
#endif
}

// One env var set for the length of a scope, restoring whatever was inherited.
class env_scope {
 public:
  env_scope(char const *name, char const *value) : name_{ name } {
    if (char const *prev{ std::getenv(name) }) { saved_ = prev; }
    put_env(name, value);
  }
  ~env_scope() { put_env(name_, saved_ ? saved_->c_str() : nullptr); }
  env_scope(env_scope const &) = delete;
  env_scope &operator=(env_scope const &) = delete;

 private:
  char const *name_;
  std::optional<std::string> saved_;
};

// In-repo fixtures: unit tests never create files, so validators need real ones.
constexpr char const *kFile{ "test_data/lua/simple.lua" };
constexpr char const *kOtherFile{ "test_data/merge_depot/valid_two_entries.txt" };
constexpr char const *kDir{ "test_data/lua" };
constexpr char const *kMissing{ "test_data/no/such/entry" };

}  // namespace

TEST_CASE("cli_parse: --name=value form lands the value for every option kind") {
  SUBCASE("global options") {
    CHECK(*parse_argv({ "envy", "--cache-root=/tmp/c", "version" }).cache_root ==
          std::filesystem::path{ "/tmp/c" });
    CHECK(*accepts<envy::cmd_product::cfg>({ "envy", "--project=.", "product" })
               .project_dir == std::filesystem::path{ "." });
    CHECK(parse_argv({ "envy", "--trace=stderr", "version" }).trace_outputs.size() == 1);
  }

  SUBCASE("deploy") {
    auto const cfg{ accepts<envy::cmd_deploy::cfg>(
        { "envy", "deploy", "--manifest=/m/envy.lua", "--platform=posix" }) };
    CHECK(*cfg.manifest_path == std::filesystem::path{ "/m/envy.lua" });
    CHECK(cfg.platform_flag == "posix");
  }

  SUBCASE("export") {
    auto const cfg{ accepts<envy::cmd_export::cfg>({ "envy",
                                                     "export",
                                                     "--output-dir=/out",
                                                     "--manifest=/m/envy.lua",
                                                     "--depot-prefix=https://d/" }) };
    CHECK(*cfg.output_dir == std::filesystem::path{ "/out" });
    CHECK(*cfg.manifest_path == std::filesystem::path{ "/m/envy.lua" });
    CHECK(*cfg.depot_prefix == "https://d/");
  }

  SUBCASE("extract") {
    auto const cfg{ accepts<envy::cmd_extract::cfg>(
        { "envy", "extract", kFile, "--only=bin/tool" }) };
    REQUIRE(cfg.only.size() == 1);
    CHECK(cfg.only[0] == "bin/tool");
  }

  SUBCASE("fetch") {
    auto const cfg{ accepts<envy::cmd_fetch::cfg>({ "envy",
                                                    "fetch",
                                                    "https://x/y",
                                                    "out.bin",
                                                    "--manifest-root=/r",
                                                    "--ref=main" }) };
    CHECK(*cfg.manifest_root == std::filesystem::path{ "/r" });
    CHECK(*cfg.ref == "main");
  }

  SUBCASE("hash") {
    CHECK(*accepts<envy::cmd_hash::cfg>({ "envy", "hash", "a", "--prefix=https://p/" })
               .prefix == "https://p/");
  }

  SUBCASE("import") {
    auto const cfg{ accepts<envy::cmd_import::cfg>(
        { "envy",
          "import",
          std::string{ "--dir=" } + kDir,
          "--manifest=/m/envy.lua",
          std::string{ "--checksums=" } + kOtherFile }) };
    CHECK(*cfg.dir == std::filesystem::path{ kDir });
    CHECK(*cfg.manifest_path == std::filesystem::path{ "/m/envy.lua" });
    CHECK(*cfg.checksums_path == std::filesystem::path{ kOtherFile });
  }

  SUBCASE("init") {
    auto const cfg{ accepts<envy::cmd_init::cfg>({ "envy",
                                                   "init",
                                                   "p",
                                                   "b",
                                                   "--mirror=https://m/",
                                                   "--envy-version=1.2.3",
                                                   "--deploy=false",
                                                   "--root=false",
                                                   "--platform=all" }) };
    CHECK(*cfg.mirror == "https://m/");
    CHECK(*cfg.envy_version == "1.2.3");
    CHECK(cfg.deploy == std::optional<bool>{ false });
    CHECK(cfg.root == std::optional<bool>{ false });
    CHECK(cfg.platform_flag == "all");
  }

  SUBCASE("install") {
    CHECK(*accepts<envy::cmd_install::cfg>({ "envy", "install", "--manifest=/m/envy.lua" })
               .manifest_path == std::filesystem::path{ "/m/envy.lua" });
  }

  SUBCASE("merge-depot") {
    auto const cfg{ accepts<envy::cmd_merge_depot::cfg>({ "envy",
                                                          "merge-depot",
                                                          kOtherFile,
                                                          "--existing=e.txt",
                                                          "--retain=r.txt",
                                                          "--retain-prefix=pkgs/" }) };
    CHECK(*cfg.existing_path == "e.txt");
    REQUIRE(cfg.retain.has_value());
    CHECK(cfg.retain->path == "r.txt");
    CHECK(cfg.retain->fmt == envy::cmd_merge_depot::retain_format::PLAIN);
    CHECK(*cfg.retain_prefix == "pkgs/");
  }

  SUBCASE("merge-depot --retain-s3-ls") {
    auto const cfg{ accepts<envy::cmd_merge_depot::cfg>(
        { "envy", "merge-depot", kOtherFile, "--retain-s3-ls=ls.txt" }) };
    REQUIRE(cfg.retain.has_value());
    CHECK(cfg.retain->path == "ls.txt");
    CHECK(cfg.retain->fmt == envy::cmd_merge_depot::retain_format::S3_LS);
  }

  SUBCASE("mirror-envy") {
    CHECK(accepts<envy::cmd_mirror_envy::cfg>(
              { "envy", "mirror-envy", "1.2.3", "/dst", "--from=https://f/" })
              .from == "https://f/");
  }

  SUBCASE("package") {
    CHECK(*accepts<envy::cmd_package::cfg>(
               { "envy", "package", "a.b@v1", "--manifest=/m/envy.lua" })
               .manifest_path == std::filesystem::path{ "/m/envy.lua" });
  }

  SUBCASE("product") {
    CHECK(*accepts<envy::cmd_product::cfg>(
               { "envy", "product", "tool", "--manifest=/m/envy.lua" })
               .manifest_path == std::filesystem::path{ "/m/envy.lua" });
  }

  SUBCASE("sync") {
    auto const cfg{ accepts<envy::cmd_sync::cfg>(
        { "envy", "sync", "--manifest=/m/envy.lua", "--platform=windows" }) };
    CHECK(*cfg.manifest_path == std::filesystem::path{ "/m/envy.lua" });
    CHECK(cfg.platform_flag == "windows");
  }

  SUBCASE("use") {
    auto const cfg{ accepts<envy::cmd_use::cfg>(
        { "envy", "use", "1.2.3", "--manifest=/m/envy.lua", "--mirror=https://m/" }) };
    CHECK(*cfg.manifest_path == std::filesystem::path{ "/m/envy.lua" });
    CHECK(*cfg.mirror == "https://m/");
  }
}

TEST_CASE("cli_parse: --name value form lands the same values") {
  SUBCASE("global options") {
    CHECK(*parse_argv({ "envy", "--cache-root", "/tmp/c", "version" }).cache_root ==
          std::filesystem::path{ "/tmp/c" });
    CHECK(*accepts<envy::cmd_product::cfg>({ "envy", "--project", ".", "product" })
               .project_dir == std::filesystem::path{ "." });
    CHECK(parse_argv({ "envy", "--trace", "stderr", "version" }).trace_outputs.size() ==
          1);
  }

  SUBCASE("deploy") {
    auto const cfg{ accepts<envy::cmd_deploy::cfg>(
        { "envy", "deploy", "--manifest", "/m/envy.lua", "--platform", "posix" }) };
    CHECK(*cfg.manifest_path == std::filesystem::path{ "/m/envy.lua" });
    CHECK(cfg.platform_flag == "posix");
  }

  SUBCASE("export") {
    auto const cfg{ accepts<envy::cmd_export::cfg>(
        { "envy", "export", "--output-dir", "/out", "--depot-prefix", "https://d/" }) };
    CHECK(*cfg.output_dir == std::filesystem::path{ "/out" });
    CHECK(*cfg.depot_prefix == "https://d/");
  }

  SUBCASE("extract") {
    auto const cfg{ accepts<envy::cmd_extract::cfg>(
        { "envy", "extract", kFile, "--only", "bin/tool" }) };
    REQUIRE(cfg.only.size() == 1);
    CHECK(cfg.only[0] == "bin/tool");
  }

  SUBCASE("fetch") {
    auto const cfg{ accepts<envy::cmd_fetch::cfg>({ "envy",
                                                    "fetch",
                                                    "https://x/y",
                                                    "out.bin",
                                                    "--manifest-root",
                                                    "/r",
                                                    "--ref",
                                                    "main" }) };
    CHECK(*cfg.manifest_root == std::filesystem::path{ "/r" });
    CHECK(*cfg.ref == "main");
  }

  SUBCASE("import") {
    auto const cfg{ accepts<envy::cmd_import::cfg>(
        { "envy", "import", "--dir", kDir, "--checksums", kOtherFile }) };
    CHECK(*cfg.dir == std::filesystem::path{ kDir });
    CHECK(*cfg.checksums_path == std::filesystem::path{ kOtherFile });
  }

  SUBCASE("init") {
    auto const cfg{ accepts<envy::cmd_init::cfg>({ "envy",
                                                   "init",
                                                   "p",
                                                   "b",
                                                   "--mirror",
                                                   "https://m/",
                                                   "--envy-version",
                                                   "1.2.3",
                                                   "--deploy",
                                                   "false",
                                                   "--root",
                                                   "false",
                                                   "--platform",
                                                   "all" }) };
    CHECK(*cfg.mirror == "https://m/");
    CHECK(*cfg.envy_version == "1.2.3");
    CHECK(cfg.deploy == std::optional<bool>{ false });
    CHECK(cfg.root == std::optional<bool>{ false });
    CHECK(cfg.platform_flag == "all");
  }

  SUBCASE("merge-depot") {
    auto const cfg{ accepts<envy::cmd_merge_depot::cfg>({ "envy",
                                                          "merge-depot",
                                                          kOtherFile,
                                                          "--existing",
                                                          "e.txt",
                                                          "--retain-s3-ls",
                                                          "ls.txt",
                                                          "--retain-prefix",
                                                          "pkgs/" }) };
    CHECK(*cfg.existing_path == "e.txt");
    REQUIRE(cfg.retain.has_value());
    CHECK(cfg.retain->path == "ls.txt");
    CHECK(cfg.retain->fmt == envy::cmd_merge_depot::retain_format::S3_LS);
    CHECK(*cfg.retain_prefix == "pkgs/");
  }

  SUBCASE("mirror-envy") {
    CHECK(accepts<envy::cmd_mirror_envy::cfg>(
              { "envy", "mirror-envy", "1.2.3", "/dst", "--from", "https://f/" })
              .from == "https://f/");
  }

  SUBCASE("use") {
    CHECK(
        *accepts<envy::cmd_use::cfg>({ "envy", "use", "1.2.3", "--mirror", "https://m/" })
             .mirror == "https://m/");
  }
}

TEST_CASE("cli_parse: short option forms") {
  SUBCASE("-o takes the next argument") {
    CHECK(*accepts<envy::cmd_export::cfg>({ "envy", "export", "-o", "/out" }).output_dir ==
          std::filesystem::path{ "/out" });
  }

  SUBCASE("-o takes an attached value") {
    CHECK(*accepts<envy::cmd_export::cfg>({ "envy", "export", "-o/out" }).output_dir ==
          std::filesystem::path{ "/out" });
  }

  SUBCASE("'=' is not special after a short option") {
    // Attached-value form wins, so the '=' is part of the value rather than a separator.
    CHECK(*accepts<envy::cmd_export::cfg>({ "envy", "export", "-o=/out" }).output_dir ==
          std::filesystem::path{ "=/out" });
  }

  SUBCASE("-o without a value is rejected") { rejects({ "envy", "export", "-o" }); }

  SUBCASE("-q is --quiet") {
    CHECK(parse_argv({ "envy", "-q", "version" }).verbosity == envy::tui::level::TUI_WARN);
  }

  SUBCASE("-v selects the version command") {
    CHECK(std::holds_alternative<envy::cmd_version::cfg>(
        *parse_argv({ "envy", "-v" }).cmd_cfg));
  }

  SUBCASE("short flags bundle") {
    auto const parsed{ parse_argv({ "envy", "-qv" }) };
    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(std::holds_alternative<envy::cmd_version::cfg>(*parsed.cmd_cfg));
    CHECK(parsed.verbosity == envy::tui::level::TUI_WARN);
    CHECK_FALSE(parsed.decorated_logging);
  }

  SUBCASE("bundled help wins over the version alias") {
    // Help short-circuits the parse, so -v never gets to select a command.
    prints_help({ "envy", "-vh" }, "SUBCOMMANDS:");
  }

  SUBCASE("-h is --help") { prints_help({ "envy", "-h" }, "SUBCOMMANDS:"); }
}

TEST_CASE("cli_parse: '--' ends option parsing for the selected command") {
  SUBCASE("a query that would otherwise look like nothing at all") {
    auto const cfg{ accepts<envy::cmd_install::cfg>({ "envy", "install", "--", "foo" }) };
    REQUIRE(cfg.queries.size() == 1);
    CHECK(cfg.queries[0] == "foo");
  }

  SUBCASE("an option name becomes a plain positional") {
    auto const cfg{ accepts<envy::cmd_install::cfg>(
        { "envy", "install", "--", "--manifest" }) };
    REQUIRE(cfg.queries.size() == 1);
    CHECK(cfg.queries[0] == "--manifest");
  }

  SUBCASE("a path that looks like a negative number") {
    auto const cfg{ accepts<envy::cmd_hash::cfg>({ "envy", "hash", "--", "-5" }) };
    REQUIRE(cfg.paths.size() == 1);
    CHECK(cfg.paths[0] == std::filesystem::path{ "-5" });
  }

  SUBCASE("fills the next positional slot") {
    CHECK(accepts<envy::cmd_extract::cfg>({ "envy", "extract", kFile, "--", "dest" })
              .destination == std::filesystem::path{ "dest" });
  }

  SUBCASE("rejected once every positional slot has been fed") {
    // Characterized, not designed: with nothing left to place, '--' has no meaning
    // and the argv is a usage error.
    rejects({ "envy", "install", "foo", "--", "bar" });
    rejects({ "envy", "--", "version" });
  }
}

TEST_CASE("cli_parse: arguments that look like options") {
  SUBCASE("a leading-dash number is a positional") {
    CHECK(accepts<envy::cmd_hash::cfg>({ "envy", "hash", "-5" }).paths[0] ==
          std::filesystem::path{ "-5" });
    CHECK(accepts<envy::cmd_mirror_envy::cfg>({ "envy", "mirror-envy", "-1.2.3", "/dst" })
              .version == "-1.2.3");
  }

  SUBCASE("an option value may start with a dash") {
    CHECK(*accepts<envy::cmd_fetch::cfg>({ "envy", "fetch", "s", "d", "--ref", "-abc" })
               .ref == "-abc");
    CHECK(
        *accepts<envy::cmd_fetch::cfg>({ "envy", "fetch", "s", "d", "--ref=-abc" }).ref ==
        "-abc");
  }

  SUBCASE("a leading-dash word is not a positional") {
    rejects({ "envy", "package", "-foo" });
  }

  SUBCASE("a leading slash never introduces an option") {
    // allow_windows_style_options(false): POSIX absolute paths stay positional on Windows.
    CHECK(accepts<envy::cmd_hash::cfg>({ "envy", "hash", "/tmp/x" }).paths[0] ==
          std::filesystem::path{ "/tmp/x" });
    CHECK(accepts<envy::cmd_run::cfg>({ "envy", "run", "/usr/bin/env" }).command[0] ==
          "/usr/bin/env");
  }
}

TEST_CASE("cli_parse: unknown options and subcommands are rejected") {
  rejects({ "envy", "--bogus", "version" });
  rejects({ "envy", "bogus" });
  rejects({ "envy", "version", "--bogus" });
  rejects({ "envy", "install", "--bogus" });
  rejects({ "envy", "install", "-Z" });
  rejects({ "envy", "help" });
  rejects({ "envy", "" });
  SUBCASE("subcommand names are case-sensitive and never abbreviated") {
    rejects({ "envy", "CACHE" });
    rejects({ "envy", "ca" });
    rejects({ "envy", "vers" });
  }
}

TEST_CASE("cli_parse: options that need a value must get one") {
  rejects({ "envy", "--cache-root" });
  rejects({ "envy", "--project" });
  rejects({ "envy", "package", "a", "--manifest" });
  rejects({ "envy", "install", "--manifest" });
  rejects({ "envy", "deploy", "--platform" });
  rejects({ "envy", "init", "a", "b", "--deploy" });
  rejects({ "envy", "merge-depot", kOtherFile, "--retain" });
  SUBCASE("an empty '=' value counts as no value at all") {
    rejects({ "envy", "install", "--manifest=" });
    rejects({ "envy", "deploy", "--platform=" });
  }
  SUBCASE("an empty argument is a value, and validators still see it") {
    rejects({ "envy", "deploy", "--platform", "" });
    rejects({ "envy", "shell", "" });
  }
}

TEST_CASE("cli_parse: required arguments are enforced for every command") {
  rejects({ "envy", "extract" });
  rejects({ "envy", "fetch" });
  rejects({ "envy", "fetch", "src" });
  rejects({ "envy", "git-resolve" });
  rejects({ "envy", "git-resolve", "url" });
  rejects({ "envy", "hash" });
  rejects({ "envy", "init" });
  rejects({ "envy", "init", "proj" });
  rejects({ "envy", "lua" });
  rejects({ "envy", "merge-depot" });
  rejects({ "envy", "mirror-envy" });
  rejects({ "envy", "mirror-envy", "1.2.3" });
  rejects({ "envy", "package" });
  rejects({ "envy", "shell" });
  rejects({ "envy", "use" });
}

TEST_CASE("cli_parse: a positional with nowhere to go is rejected") {
  rejects({ "envy", "cache", "extra" });
  rejects({ "envy", "extract", kFile, "dest", "extra" });
  rejects({ "envy", "fetch", "s", "d", "extra" });
  rejects({ "envy", "git-resolve", "u", "r", "extra" });
  rejects({ "envy", "init", "p", "b", "extra" });
  rejects({ "envy", "lua", kFile, "extra" });
  rejects({ "envy", "mirror-envy", "1.2.3", "/dst", "extra" });
  rejects({ "envy", "package", "a.b@v1", "extra" });
  rejects({ "envy", "product", "tool", "extra" });
  rejects({ "envy", "shell", "bash", "zsh" });
  rejects({ "envy", "use", "1.2.3", "extra" });
  rejects({ "envy", "version", "extra" });
  SUBCASE("a single-value option does not swallow a second argument") {
    rejects({ "envy", "extract", kFile, "dest", "--only", "a", "b" });
  }
}

TEST_CASE("cli_parse: repeated options") {
  SUBCASE("a second value for a single-value option is an error") {
    rejects({ "envy", "--cache-root", "a", "--cache-root", "b", "version" });
    rejects({ "envy", "--trace=stderr", "--trace=stderr", "version" });
    rejects({ "envy", "package", "a", "--manifest", "x", "--manifest", "y" });
    rejects({ "envy", "deploy", "--platform", "posix", "--platform", "all" });
    rejects({ "envy", "export", "-o", "a", "-o", "b" });
    rejects({ "envy", "fetch", "s", "d", "--ref", "a", "--ref", "b" });
    rejects({ "envy", "init", "a", "b", "--deploy", "true", "--deploy", "false" });
    rejects({ "envy", "use", "1.2.3", "--mirror", "a", "--mirror", "b" });
    rejects({ "envy", "merge-depot", kOtherFile, "--retain", "a", "--retain", "b" });
  }

  SUBCASE("--project takes the last, so an injected anchor can be overridden") {
    CHECK(*accepts<envy::cmd_product::cfg>(
               { "envy", "--project", "..", "--project", ".", "product" })
               .project_dir == std::filesystem::path{ "." });
  }

  SUBCASE("flags are idempotent") {
    CHECK(
        accepts<envy::cmd_version::cfg>({ "envy", "version", "--licenses", "--licenses" })
            .show_licenses);
    CHECK(accepts<envy::cmd_install::cfg>(
              { "envy", "install", "--ignore-depot", "--ignore-depot" })
              .ignore_depot);
    CHECK(accepts<envy::cmd_deploy::cfg>({ "envy", "deploy", "--strict", "--strict" })
              .strict);
    CHECK(parse_argv({ "envy", "--verbose", "--verbose", "version" }).verbosity ==
          envy::tui::level::TUI_DEBUG);
  }

  SUBCASE("list-valued options and positionals accumulate") {
    CHECK(accepts<envy::cmd_extract::cfg>(
              { "envy", "extract", kFile, "d", "--only", "a", "--only", "b" })
              .only.size() == 2);
    CHECK(accepts<envy::cmd_hash::cfg>({ "envy", "hash", "a", "b", "c" }).paths.size() ==
          3);
    CHECK(
        accepts<envy::cmd_install::cfg>({ "envy", "install", "a", "b" }).queries.size() ==
        2);
    CHECK(accepts<envy::cmd_deploy::cfg>({ "envy", "deploy", "a", "b", "c" })
              .identities.size() == 3);
  }
}

TEST_CASE("cli_parse: options may follow and interleave with positionals") {
  SUBCASE("an option splits a positional list without ending it") {
    auto const cfg{ accepts<envy::cmd_install::cfg>(
        { "envy", "install", "a", "--manifest", "m", "b" }) };
    REQUIRE(cfg.queries.size() == 2);
    CHECK(cfg.queries[0] == "a");
    CHECK(cfg.queries[1] == "b");
    CHECK(*cfg.manifest_path == std::filesystem::path{ "m" });
  }

  SUBCASE("a positional after an option still fills the next slot") {
    auto const cfg{ accepts<envy::cmd_extract::cfg>(
        { "envy", "extract", kFile, "--only", "x", "dest" }) };
    CHECK(cfg.destination == std::filesystem::path{ "dest" });
    REQUIRE(cfg.only.size() == 1);
    CHECK(cfg.only[0] == "x");
  }

  SUBCASE("hash accumulates around --prefix") {
    auto const cfg{ accepts<envy::cmd_hash::cfg>(
        { "envy", "hash", "a", "--prefix", "p", "b" }) };
    CHECK(cfg.paths.size() == 2);
    CHECK(*cfg.prefix == "p");
  }
}

TEST_CASE("cli_parse: global options must precede the subcommand") {
  // No fallthrough: a global option written after the subcommand is simply unknown there.
  rejects({ "envy", "version", "-q" });
  rejects({ "envy", "version", "--cache-root", "/x" });
  rejects({ "envy", "install", "--verbose" });
  rejects({ "envy", "product", "--project", "." });
  rejects({ "envy", "install", "--trace=stderr" });
}

TEST_CASE("cli_parse: --trace takes an optional, comma-separated spec") {
  auto const outputs{ [](std::vector<std::string> args) {
    return parse_argv(std::move(args)).trace_outputs;
  } };

  SUBCASE("bare --trace means stderr") {
    auto const out{ outputs({ "envy", "--trace", "version" }) };
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == envy::tui::trace_output_type::std_err);
    CHECK_FALSE(out[0].file_path.has_value());
  }

  SUBCASE("an empty '=' value also means stderr") {
    REQUIRE(outputs({ "envy", "--trace=", "version" }).size() == 1);
  }

  SUBCASE("stderr, either spelling") {
    CHECK(outputs({ "envy", "--trace=stderr", "version" }).size() == 1);
    CHECK(outputs({ "envy", "--trace", "stderr", "version" }).size() == 1);
  }

  SUBCASE("file:<path>") {
    auto const out{ outputs({ "envy", "--trace=file:/tmp/t.jsonl", "version" }) };
    REQUIRE(out.size() == 1);
    CHECK(out[0].type == envy::tui::trace_output_type::file);
    CHECK(*out[0].file_path == std::filesystem::path{ "/tmp/t.jsonl" });
  }

  SUBCASE("comma-separated destinations, in order") {
    auto const out{ outputs({ "envy", "--trace=stderr,file:/tmp/t.jsonl", "version" }) };
    REQUIRE(out.size() == 2);
    CHECK(out[0].type == envy::tui::trace_output_type::std_err);
    CHECK(out[1].type == envy::tui::trace_output_type::file);
    CHECK(*out[1].file_path == std::filesystem::path{ "/tmp/t.jsonl" });
  }

  SUBCASE("two files") {
    auto const out{ outputs({ "envy", "--trace=file:/a,file:/b", "version" }) };
    REQUIRE(out.size() == 2);
    CHECK(*out[0].file_path == std::filesystem::path{ "/a" });
    CHECK(*out[1].file_path == std::filesystem::path{ "/b" });
  }

  SUBCASE("empty tokens are dropped, leaving no destination at all") {
    auto const parsed{ parse_argv({ "envy", "--trace=,,", "version" }) };
    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(parsed.trace_outputs.empty());
  }

  SUBCASE("an unusable spec cancels the whole run") {
    for (char const *spec : { "--trace=bogus", "--trace=file:", "--trace=stderr,bogus" }) {
      INFO("spec: ", spec);
      auto const parsed{ parse_argv({ "envy", spec, "version" }) };
      CHECK_FALSE(parsed.cmd_cfg.has_value());
      CHECK_FALSE(parsed.cli_output.empty());
      CHECK(parsed.trace_outputs.empty());
    }
  }

  SUBCASE("the value is not taken from a subcommand name or another option") {
    CHECK(std::holds_alternative<envy::cmd_version::cfg>(
        *parse_argv({ "envy", "--trace", "version" }).cmd_cfg));
    CHECK(std::holds_alternative<envy::cmd_version::cfg>(
        *parse_argv({ "envy", "--trace", "--version" }).cmd_cfg));
  }

  SUBCASE("trace is configured even when no command is selected") {
    auto const parsed{ parse_argv({ "envy", "--trace" }) };
    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK_FALSE(parsed.cli_output.empty());
    CHECK(parsed.trace_outputs.size() == 1);
  }

  SUBCASE("no --trace means no destinations") {
    CHECK(outputs({ "envy", "version" }).empty());
  }
}

TEST_CASE("cli_parse: environment variables stand in for absent flags") {
  SUBCASE("ENVY_CACHE_ROOT fills --cache-root") {
    env_scope const env{ "ENVY_CACHE_ROOT", "/env/cache" };
    CHECK(*parse_argv({ "envy", "version" }).cache_root ==
          std::filesystem::path{ "/env/cache" });
  }

  SUBCASE("an explicit --cache-root outranks the environment") {
    env_scope const env{ "ENVY_CACHE_ROOT", "/env/cache" };
    CHECK(*parse_argv({ "envy", "--cache-root=/flag", "version" }).cache_root ==
          std::filesystem::path{ "/flag" });
  }

  SUBCASE("no ENVY_CACHE_ROOT leaves the override unset") {
    env_scope const env{ "ENVY_CACHE_ROOT", nullptr };
    CHECK_FALSE(parse_argv({ "envy", "version" }).cache_root.has_value());
  }

  SUBCASE("ENVY_IGNORE_DEPOT reaches every command that declares it") {
    env_scope const env{ "ENVY_IGNORE_DEPOT", "1" };
    CHECK(accepts<envy::cmd_export::cfg>({ "envy", "export" }).ignore_depot);
    CHECK(accepts<envy::cmd_install::cfg>({ "envy", "install" }).ignore_depot);
    CHECK(accepts<envy::cmd_package::cfg>({ "envy", "package", "a" }).ignore_depot);
    CHECK(accepts<envy::cmd_sync::cfg>({ "envy", "sync" }).ignore_depot);
  }

  SUBCASE("a false-y value leaves the flag off") {
    for (char const *v : { "0", "false", "" }) {
      INFO("ENVY_IGNORE_DEPOT=", v);
      env_scope const env{ "ENVY_IGNORE_DEPOT", v };
      CHECK_FALSE(accepts<envy::cmd_install::cfg>({ "envy", "install" }).ignore_depot);
    }
  }

  SUBCASE("an unparseable value is an error") {
    env_scope const env{ "ENVY_IGNORE_DEPOT", "bogus" };
    rejects({ "envy", "install" });
  }

  SUBCASE("unset leaves the flag off") {
    env_scope const env{ "ENVY_IGNORE_DEPOT", nullptr };
    CHECK_FALSE(accepts<envy::cmd_install::cfg>({ "envy", "install" }).ignore_depot);
  }
}

TEST_CASE("cli_parse: every mutually exclusive pair rejects both orders") {
  SUBCASE("global verbosity") {
    rejects({ "envy", "--verbose", "--quiet", "version" });
    rejects({ "envy", "--quiet", "--verbose", "version" });
  }

  SUBCASE("cache actions") {
    char const *acts[]{ "--root", "--user-wide-root", "--local", "--shared" };
    for (auto const *a : acts) {
      for (auto const *b : acts) {
        if (a != b) { rejects({ "envy", "cache", a, b }); }
      }
    }
  }

  SUBCASE("--subproject and --manifest") {
    rejects({ "envy", "deploy", "--subproject", "--manifest", "m" });
    rejects({ "envy", "deploy", "--manifest", "m", "--subproject" });
    rejects({ "envy", "sync", "--subproject", "--manifest", "m" });
    rejects({ "envy", "sync", "--manifest", "m", "--subproject" });
    rejects({ "envy", "use", "1.2.3", "--subproject", "--manifest", "m" });
    rejects({ "envy", "use", "1.2.3", "--manifest", "m", "--subproject" });
  }

  SUBCASE("use --pin-sums and --no-pin-sums") {
    rejects({ "envy", "use", "1.2.3", "--pin-sums", "--no-pin-sums" });
    rejects({ "envy", "use", "1.2.3", "--no-pin-sums", "--pin-sums" });
  }

  SUBCASE("merge-depot retain formats") {
    rejects({ "envy", "merge-depot", kOtherFile, "--retain", "a", "--retain-s3-ls", "b" });
    rejects({ "envy", "merge-depot", kOtherFile, "--retain-s3-ls", "b", "--retain", "a" });
  }

  SUBCASE("import takes an archive or a directory, never both and never neither") {
    rejects({ "envy", "import", kFile, "--dir", kDir });
    rejects({ "envy", "import", "--dir", kDir, kFile });
    rejects({ "envy", "import" });
  }
}

TEST_CASE("cli_parse: value validators") {
  SUBCASE("an existing file is required") {
    struct {
      std::vector<std::string> ok, missing, directory;
    } const cases[]{
      { { "envy", "extract", kFile },
        { "envy", "extract", kMissing },
        { "envy", "extract", kDir } },
      { { "envy", "import", kFile },
        { "envy", "import", kMissing },
        { "envy", "import", kDir } },
      { { "envy", "import", "--checksums", kOtherFile, "--dir", kDir },
        { "envy", "import", "--checksums", kMissing, "--dir", kDir },
        { "envy", "import", "--checksums", kDir, "--dir", kDir } },
      { { "envy", "lua", kFile }, { "envy", "lua", kMissing }, { "envy", "lua", kDir } },
      { { "envy", "merge-depot", kOtherFile },
        { "envy", "merge-depot", kMissing },
        { "envy", "merge-depot", kDir } },
    };
    for (auto const &c : cases) {
      INFO("argv: ", joined(c.ok));
      CHECK(parse_argv(c.ok).cmd_cfg.has_value());
      rejects(c.missing);
      rejects(c.directory);
    }
  }

  SUBCASE("an existing directory is required") {
    CHECK(parse_argv({ "envy", "--project", ".", "product" }).cmd_cfg.has_value());
    rejects({ "envy", "--project", kMissing, "product" });
    rejects({ "envy", "--project", kFile, "product" });
    CHECK(parse_argv({ "envy", "import", "--dir", kDir }).cmd_cfg.has_value());
    rejects({ "envy", "import", "--dir", kMissing });
    rejects({ "envy", "import", "--dir", kFile });
  }

  SUBCASE("--platform must name a known script platform") {
    for (char const *cmd : { "deploy", "sync" }) {
      for (char const *p : { "posix", "windows", "all" }) {
        INFO(cmd, " --platform ", p);
        CHECK(parse_argv({ "envy", cmd, "--platform", p }).cmd_cfg.has_value());
      }
      rejects({ "envy", cmd, "--platform", "linux" });
      rejects({ "envy", cmd, "--platform", "POSIX" });
    }
    for (char const *p : { "posix", "windows", "all" }) {
      CHECK(parse_argv({ "envy", "init", "p", "b", "--platform", p }).cmd_cfg.has_value());
    }
    rejects({ "envy", "init", "p", "b", "--platform", "linux" });
    rejects({ "envy", "init", "p", "b", "--platform", "All" });
  }

  SUBCASE("shell must name a supported shell") {
    for (char const *s : { "bash", "zsh", "fish", "powershell" }) {
      INFO("shell ", s);
      CHECK(accepts<envy::cmd_shell::cfg>({ "envy", "shell", s }).shell == s);
    }
    rejects({ "envy", "shell", "csh" });
    rejects({ "envy", "shell", "Bash" });
  }
}

TEST_CASE("cli_parse: help is available everywhere and runs nothing") {
  SUBCASE("the root") {
    prints_help({ "envy", "--help" }, "SUBCOMMANDS:");
    prints_help({ "envy", "-h" }, "SUBCOMMANDS:");
  }

  SUBCASE("bare envy prints the same listing") {
    auto const parsed{ parse_argv({ "envy" }) };
    CHECK_FALSE(parsed.cmd_cfg.has_value());
    CHECK(parsed.cli_output.find("SUBCOMMANDS:") != std::string::npos);
  }

  SUBCASE("every subcommand") {
    for (char const *name :
         { "cache",       "deploy",      "export",      "extract", "fetch",
           "git-resolve", "hash",        "import",      "init",    "install",
           "lua",         "merge-depot", "mirror-envy", "package", "product",
           "run",         "shell",       "sync",        "use",     "version" }) {
      prints_help({ "envy", name, "--help" }, std::string{ "envy " } + name);
      prints_help({ "envy", name, "-h" }, std::string{ "envy " } + name);
    }
  }
}

TEST_CASE("cli_parse: cmd_init boolean directives") {
  SUBCASE("both default to an engaged true") {
    auto const cfg{ accepts<envy::cmd_init::cfg>({ "envy", "init", "p", "b" }) };
    CHECK(cfg.deploy == std::optional<bool>{ true });
    CHECK(cfg.root == std::optional<bool>{ true });
  }

  SUBCASE("truthy and falsey spellings") {
    for (char const *t : { "true", "1", "yes", "on" }) {
      INFO("--deploy ", t);
      CHECK(accepts<envy::cmd_init::cfg>({ "envy", "init", "p", "b", "--deploy", t })
                .deploy == std::optional<bool>{ true });
    }
    for (char const *f : { "false", "0", "no", "off" }) {
      INFO("--root ", f);
      CHECK(accepts<envy::cmd_init::cfg>({ "envy", "init", "p", "b", "--root", f }).root ==
            std::optional<bool>{ false });
    }
  }

  SUBCASE("anything else is an error") {
    rejects({ "envy", "init", "p", "b", "--deploy", "bogus" });
    rejects({ "envy", "init", "p", "b", "--root", "maybe" });
  }
}

TEST_CASE("cli_parse: cmd_trace_schema") {
  CHECK(std::holds_alternative<envy::cmd_trace_schema::cfg>(
      *parse_argv({ "envy", "trace-schema" }).cmd_cfg));
  rejects({ "envy", "trace-schema", "extra" });
  prints_help({ "envy", "trace-schema", "--help" }, "envy trace-schema");
}

TEST_CASE("cli_parse: cmd_run passes everything after the subcommand through") {
  SUBCASE("envy's own global options are not intercepted") {
    auto const cfg{ accepts<envy::cmd_run::cfg>(
        { "envy", "run", "tool", "--verbose", "--quiet", "--project", "." }) };
    REQUIRE(cfg.command.size() == 5);
    CHECK(cfg.command[1] == "--verbose");
    CHECK(cfg.command[2] == "--quiet");
    CHECK(cfg.command[4] == ".");
  }

  SUBCASE("a leading option starts the passthrough") {
    auto const cfg{ accepts<envy::cmd_run::cfg>({ "envy", "run", "--verbose", "ls" }) };
    REQUIRE(cfg.command.size() == 2);
    CHECK(cfg.command[0] == "--verbose");
    CHECK(cfg.command[1] == "ls");
  }

  SUBCASE("globals ahead of the subcommand still apply") {
    auto const parsed{ parse_argv({ "envy", "--verbose", "run", "ls" }) };
    REQUIRE(parsed.cmd_cfg.has_value());
    CHECK(parsed.verbosity == envy::tui::level::TUI_DEBUG);
    CHECK(std::get<envy::cmd_run::cfg>(*parsed.cmd_cfg).command.size() == 1);
  }

  SUBCASE("--help is still envy's") {
    prints_help({ "envy", "run", "--help" }, "envy run");
  }
}

TEST_CASE("cli_parse: --project reaches only the commands that anchor on a project") {
  SUBCASE("shell") {
    CHECK(accepts<envy::cmd_shell::cfg>({ "envy", "--project", ".", "shell", "bash" })
              .project_dir.has_value());
  }

  SUBCASE("a command with its own unrelated project directory is skipped") {
    // cmd_init's project-dir positional is not the anchor, so init never receives one.
    CHECK(std::holds_alternative<envy::cmd_init::cfg>(
        *parse_argv({ "envy", "--project", ".", "init", "p", "b" }).cmd_cfg));
    CHECK(std::holds_alternative<envy::cmd_lua::cfg>(
        *parse_argv({ "envy", "--project", ".", "lua", kFile }).cmd_cfg));
  }
}

#ifdef ENVY_FUNCTIONAL_TESTER

TEST_CASE("cli_parse: cache-test parent and children") {
  SUBCASE("the parent alone runs nothing and lists its children") {
    prints_help({ "envy", "cache-test" }, "ensure-package");
    prints_help({ "envy", "cache-test", "--help" }, "ensure-spec");
  }

  SUBCASE("children have their own help") {
    prints_help({ "envy", "cache-test", "ensure-package", "--help" },
                "envy cache-test ensure-package");
    prints_help({ "envy", "cache-test", "ensure-spec", "--help" },
                "envy cache-test ensure-spec");
  }

  SUBCASE("an unknown child is rejected") {
    rejects({ "envy", "cache-test", "bogus" });
    rejects({ "envy", "cache-test", "ensure-package", "--bogus", "a", "b", "c", "d" });
  }

  SUBCASE("extra positionals are rejected") {
    rejects({ "envy", "cache-test", "ensure-package", "a", "b", "c", "d", "e" });
    rejects({ "envy", "cache-test", "ensure-spec", "a", "b" });
  }

  SUBCASE("every choreography option, in the space-separated form") {
    auto const cfg{ accepts<envy::cmd_cache_ensure_package::cfg>(
        { "envy",
          "cache-test",
          "ensure-package",
          "gcc",
          "darwin",
          "arm64",
          "deadbeef",
          "--test-id",
          "t",
          "--barrier-dir",
          "/b",
          "--barrier-signal",
          "s",
          "--barrier-wait",
          "w",
          "--barrier-signal-after",
          "sa",
          "--barrier-wait-after",
          "wa",
          "--crash-after",
          "42",
          "--fail-before-complete" }) };
    CHECK(cfg.identity == "gcc");
    CHECK(cfg.platform == "darwin");
    CHECK(cfg.arch == "arm64");
    CHECK(cfg.hash_prefix == "deadbeef");
    CHECK(cfg.test_id == "t");
    CHECK(cfg.barrier_dir == std::filesystem::path{ "/b" });
    CHECK(cfg.barrier_signal == "s");
    CHECK(cfg.barrier_wait == "w");
    CHECK(cfg.barrier_signal_after == "sa");
    CHECK(cfg.barrier_wait_after == "wa");
    CHECK(cfg.crash_after_ms == 42);
    CHECK(cfg.fail_before_complete);
  }

  SUBCASE("--crash-after wants a number") {
    rejects({ "envy",
              "cache-test",
              "ensure-package",
              "a",
              "b",
              "c",
              "d",
              "--crash-after",
              "soon" });
    CHECK(accepts<envy::cmd_cache_ensure_spec::cfg>(
              { "envy", "cache-test", "ensure-spec", "a", "--crash-after", "-1" })
              .crash_after_ms == -1);
  }

  SUBCASE("--crash-after rejects values outside int rather than truncating") {
    // strtol widens to long, so these parse cleanly but do not fit an int.
    rejects({ "envy", "cache-test", "ensure-spec", "a", "--crash-after", "2147483648" });
    rejects({ "envy", "cache-test", "ensure-spec", "a", "--crash-after", "-2147483649" });
    rejects({ "envy", "cache-test", "ensure-spec", "a", "--crash-after",
              "99999999999999999999" });
    CHECK(accepts<envy::cmd_cache_ensure_spec::cfg>({ "envy", "cache-test", "ensure-spec",
                                                      "a", "--crash-after", "2147483647" })
              .crash_after_ms == 2147483647);
  }

  SUBCASE("ensure-spec --source, both forms") {
    CHECK(accepts<envy::cmd_cache_ensure_spec::cfg>(
              { "envy", "cache-test", "ensure-spec", "a", "--source=s" })
              .source == "s");
    CHECK(accepts<envy::cmd_cache_ensure_spec::cfg>(
              { "envy", "cache-test", "ensure-spec", "a", "--source", "s" })
              .source == "s");
  }
}

#endif  // ENVY_FUNCTIONAL_TESTER
