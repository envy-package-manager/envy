#include "cmd.h"

#include "doctest.h"

#include <filesystem>
#include <optional>

namespace {

class test_cmd : public envy::cmd {
 public:
  struct cfg : envy::cmd_cfg<test_cmd> {};
  test_cmd(cfg, std::optional<std::filesystem::path> const & /*cli_cache_root*/) {}
  void execute() override { ++executions; }
  static inline int executions{ 0 };
};

}  // namespace

TEST_CASE("cmd_cfg exposes cmd_t alias") {
  using config_type = test_cmd::cfg;
  using expected_command = test_cmd;
  using actual_command = config_type::cmd_t;
  CHECK(std::is_same_v<actual_command, expected_command>);
}

TEST_CASE("cmd factory creates command from cfg") {
  test_cmd::cfg cfg{};
  std::optional<std::filesystem::path> cli_cache_root{ std::nullopt };
  auto cmd{ envy::cmd::create(cfg, cli_cache_root) };
  REQUIRE(cmd);
  int const before{ test_cmd::executions };
  cmd->execute();  // envy builds without RTTI; the override running proves the type
  CHECK(test_cmd::executions == before + 1);
}
