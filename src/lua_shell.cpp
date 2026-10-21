#include "lua_shell.h"

#include "util.h"

#include "sol/sol.hpp"

#include <stdexcept>
#include <string>

namespace envy {

resolved_shell parse_shell_config_from_lua(sol::object const &obj, char const *context) {
  // ENVY_SHELL constant (stored as number)
  if (obj.get_type() == sol::type::number) {
    int const value{ obj.as<int>() };
    auto const choice{ static_cast<shell_choice>(value) };

    // Validate it's one of the known shell choices
    bool const valid_choice{ choice == shell_choice::bash || choice == shell_choice::sh ||
                             choice == shell_choice::cmd ||
                             choice == shell_choice::powershell };

    if (!valid_choice) {
      throw_runtime_error({ context, ": invalid ENVY_SHELL constant" });
    }

    // Platform-specific shell validation
#if defined(_WIN32)
    if (choice == shell_choice::bash || choice == shell_choice::sh) {
      throw_runtime_error({ context, ": BASH/SH shells are only available on Unix" });
    }
#else
    if (choice == shell_choice::cmd || choice == shell_choice::powershell) {
      throw_runtime_error(
          { context, ": CMD/POWERSHELL shells are only available on Windows" });
    }
#endif

    return choice;
  }

  // Custom shell table
  if (obj.is<sol::table>()) {
    try {
      custom_shell custom{ shell_parse_custom_from_lua(obj.as<sol::table>()) };
      shell_validate_custom(custom);

      // Unpack custom_shell variant (variant<file, inline>) into return variant
      resolved_shell result;
      envy::visit([&result](auto &&custom_cfg) { result = custom_cfg; }, custom);
      return result;
    } catch (std::exception const &e) { throw_runtime_error({ context, ": ", e.what() }); }
  }

  // Unsupported type
  throw_runtime_error({ context,
                        ": shell must be ENVY_SHELL constant or table {file=..., "
                        "ext=...} or {inline=...}" });
}

}  // namespace envy
