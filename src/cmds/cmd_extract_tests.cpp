#include "cmds/cmd_extract.h"

#include "cache.h"

#include "doctest.h"

#include <filesystem>

TEST_CASE("cmd_extract config exposes cmd_t alias") {
  using config_type = envy::cmd_extract::cfg;
  using expected_command = envy::cmd_extract;
  using actual_command = config_type::cmd_t;

  CHECK(std::is_same_v<actual_command, expected_command>);
}

TEST_CASE("cmd_extract config stores archive path") {
  envy::cmd_extract::cfg cfg;
  cfg.archive_path = "/path/to/archive.zip";
  cfg.destination = "/path/to/destination";

  CHECK(cfg.archive_path == "/path/to/archive.zip");
  CHECK(cfg.destination == "/path/to/destination");
}

TEST_CASE("cmd_extract config archive path can be relative") {
  envy::cmd_extract::cfg cfg;
  cfg.archive_path = "relative/archive.tar.xz";
  cfg.destination = "relative/destination";

  CHECK(cfg.archive_path.is_relative());
  CHECK(cfg.destination.is_relative());
}

TEST_CASE("cmd_extract config archive path can be absolute") {
  envy::cmd_extract::cfg cfg;
  // Use platform-appropriate absolute path forms.
#ifdef _WIN32
  cfg.archive_path = "C:/absolute/path/archive.7z";
  cfg.destination = "C:/absolute/path/destination";
#else
  cfg.archive_path = "/absolute/path/archive.7z";
  cfg.destination = "/absolute/path/destination";
#endif

  CHECK(cfg.archive_path.is_absolute());
  CHECK(cfg.destination.is_absolute());
}

TEST_CASE("cmd_extract config destination can be empty") {
  envy::cmd_extract::cfg cfg;
  cfg.archive_path = "archive.tar.bz2";

  CHECK(cfg.destination.empty());
}

TEST_CASE("cmd_extract config only defaults empty and holds archive-relative paths") {
  envy::cmd_extract::cfg cfg;
  CHECK(cfg.only.empty());

  cfg.only = { "bin/clang-format", "lib/clang" };
  REQUIRE(cfg.only.size() == 2);
  CHECK(cfg.only[0] == "bin/clang-format");
  CHECK(cfg.only[1] == "lib/clang");
}

TEST_CASE("cmd_extract inherits from cmd") {
  CHECK(std::is_base_of_v<envy::cmd, envy::cmd_extract>);
}

TEST_CASE("cmd_extract cfg inherits from cmd_cfg") {
  CHECK(std::is_base_of_v<envy::cmd_cfg<envy::cmd_extract>, envy::cmd_extract::cfg>);
}
