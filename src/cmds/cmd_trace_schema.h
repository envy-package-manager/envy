#pragma once

#include "cmd.h"

namespace envy {

class cli_cmd;

// Functional-tester-only: dump the trace event registry as JSON so Python tests
// can verify the parser registry matches the binary.
class cmd_trace_schema : public cmd {
 public:
  struct cfg : cmd_cfg<cmd_trace_schema> {};

  static cli_cmd &register_cli(cli_cmd &app, cfg &c);

  cmd_trace_schema(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root);

  void execute() override;
};

}  // namespace envy
