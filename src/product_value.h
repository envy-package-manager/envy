#pragma once

#include <string>
#include <variant>
#include <vector>

namespace envy {

struct product_command_fn {};  // the argv is what the spec's function returns at each use

// A path product's value, a command's argv ({{placeholders}} rendered at each use), or a
// command function's mark; the function itself lives in its spec's Lua state.
using product_value =
    std::variant<std::string, std::vector<std::string>, product_command_fn>;

}  // namespace envy
