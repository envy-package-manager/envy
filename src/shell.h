#pragma once

#include "sol/forward.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace envy {

using shell_env_t = std::unordered_map<std::string, std::string>;

struct shell_result {
  int exit_code;
  std::optional<int> signal;
};

enum class shell_stream { std_out, std_err };
enum class shell_choice { bash, sh, cmd, powershell };

// Custom shell configuration for file mode (script written to temp file)
struct custom_shell_file {
  std::vector<std::string> argv;  // first element is shell executable path
  std::string ext;                // Required, e.g. ".tcl", ".sh"
};

// Custom shell configuration for inline mode (script passed as command-line argument)
struct custom_shell_inline {
  std::vector<std::string> argv;  // first element is shell executable path
};

using custom_shell = std::variant<custom_shell_file, custom_shell_inline>;
using resolved_shell = std::variant<shell_choice, custom_shell_file, custom_shell_inline>;

// Manifest DEFAULT_SHELL value (resolved to constant or custom shell)
using default_shell_value =
    std::variant<shell_choice,  // Built-in: ENVY_SHELL.BASH, etc.
                 custom_shell   // Custom: {file = ..., ext = ...} or {inline = ...}
                 >;

// Manifest DEFAULT_SHELL configuration (optional, nullopt if not specified)
using default_shell_cfg_t = std::optional<default_shell_value>;

// DEFAULT_SHELL as declared, parsed but not necessarily evaluated. A value form
// resolves at manifest load; a function form cannot, because naming an envy-managed
// interpreter needs `depends` installed first — so it is evaluated on first use.
struct default_shell_decl {
  std::vector<std::string> depends;  // package identities, resolved against PACKAGES
  bool is_function{ false };
  default_shell_cfg_t value;  // set only when !is_function
};

// Resolve manifest default shell (or platform default if unset)
resolved_shell shell_resolve_default(default_shell_cfg_t const *cfg);

struct shell_run_cfg {
  std::function<void(std::string_view)> on_output_line;
  std::function<void(std::string_view)> on_stdout_line;
  std::function<void(std::string_view)> on_stderr_line;
  std::optional<std::filesystem::path> cwd;
  shell_env_t env;
  resolved_shell shell{ shell_resolve_default(nullptr) };
  bool check{ true };
};

shell_choice shell_parse_choice(std::optional<std::string_view> value);

// Validate custom shell configuration (checks executable exists and is accessible)
void shell_validate_custom(custom_shell const &cfg);

// Parse custom shell from sol2 table
custom_shell shell_parse_custom_from_lua(sol::table const &tbl);

// Initialize shell subsystem (must be called early in main before any shell_run calls)
void shell_init();

shell_env_t shell_getenv();
shell_result shell_run(std::string_view script, shell_run_cfg const &cfg);

}  // namespace envy
