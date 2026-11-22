#include "lua_error_formatter.h"

#include "pkg_cfg.h"

#include <algorithm>
#include <sstream>
#include <string_view>

namespace envy {

// Extract line number from Lua error message (for unit testing)
// Example: "spec.lua:42: assertion failed" -> 42
std::optional<int> extract_line_number(std::string const &error_msg) {
  // Look for pattern ":line_number:"
  size_t pos{ error_msg.find(".lua:") };
  if (pos == std::string::npos) { return std::nullopt; }

  pos += 5;  // Skip ".lua:"
  size_t const end_pos{ error_msg.find(':', pos) };
  if (end_pos == std::string::npos) { return std::nullopt; }

  try {
    return std::stoi(error_msg.substr(pos, end_pos - pos));
  } catch (...) { return std::nullopt; }
}

// Build provenance chain by walking parent pointers (for unit testing)
std::vector<pkg_cfg const *> build_provenance_chain(pkg_cfg const *spec) {
  std::vector<pkg_cfg const *> chain;
  while (spec) {
    chain.push_back(spec);
    spec = spec->parent;
  }
  return chain;
}

struct parsed_lua_error {
  std::string headline;                   // First line of error
  std::vector<std::string> stack_frames;  // Cleaned stack frames
};

parsed_lua_error parse_lua_error(std::string const &msg) {
  parsed_lua_error parsed;
  std::istringstream iss{ msg };
  std::string line;
  bool in_stack{ false };
  bool stack_seen{ false };

  while (std::getline(iss, line)) {
    if (!in_stack) {
      if (line.rfind("stack traceback:", 0) == 0) {
        in_stack = true;
        stack_seen = true;
        continue;
      }
      if (parsed.headline.empty() && !line.empty()) { parsed.headline = line; }
      continue;
    }

    // Skip duplicate stack traceback headers
    if (line.rfind("stack traceback:", 0) == 0) {
      if (stack_seen) { break; }
      stack_seen = true;
      continue;
    }

    // Trim leading whitespace
    auto start{ line.find_first_not_of(" \t") };
    if (start == std::string::npos) { continue; }
    line = line.substr(start);

    // Drop noisy frames that don't help users: C functions and envy's own Lua, whose chunk
    // names CMakeLists.txt gives envy_luac.
    constexpr std::string_view kEnvyChunks[]{ "envy.prelude:",
                                              "envy.extend:",
                                              "envy.template:" };
    if (line.rfind("[C]:", 0) == 0) { continue; }
    if (std::ranges::any_of(kEnvyChunks,
                            [&](std::string_view c) { return line.starts_with(c); })) {
      continue;
    }

    parsed.stack_frames.push_back(line);
  }

  return parsed;
}

std::string format_lua_error(lua_error_context const &ctx) {
  std::ostringstream oss;

  parsed_lua_error parsed{ parse_lua_error(ctx.lua_error_message) };

  // Header: identity with options
  oss << "Lua error in " << ctx.r->cfg->identity;
  if (!ctx.r->cfg->serialized_options.empty() && ctx.r->cfg->serialized_options != "{}") {
    oss << ctx.r->cfg->serialized_options;
  }
  oss << ":\n  " << (parsed.headline.empty() ? ctx.lua_error_message : parsed.headline)
      << "\n";

  if (!parsed.stack_frames.empty()) {
    oss << "Stack traceback:\n";
    for (auto const &frame : parsed.stack_frames) { oss << "  " << frame << "\n"; }
  }

  oss << "\n";

  // Spec file path with line number
  if (ctx.r->spec_file_path) {
    oss << "Spec file: " << ctx.r->spec_file_path->string();
    if (auto line_num = extract_line_number(ctx.lua_error_message)) {
      oss << ":" << *line_num;
    }
    oss << "\n";
  }

  // Declared in (provenance)
  if (!ctx.r->cfg->declaring_file_path.empty()) {
    oss << "Declared in: " << ctx.r->cfg->declaring_file_path.string() << "\n";
  }

  // Phase
  if (!ctx.phase.empty()) { oss << "Phase: " << ctx.phase << "\n"; }

  // Options
  if (!ctx.r->cfg->serialized_options.empty()) {
    oss << "Options: " << ctx.r->cfg->serialized_options << "\n";
  }

  // Provenance chain (if nested dependencies)
  auto chain{ build_provenance_chain(ctx.r->cfg) };
  if (chain.size() > 1) {
    oss << "\nProvenance chain:\n";
    for (size_t i{ 0 }; i < chain.size(); ++i) {
      oss << "  ";
      for (size_t j{ 0 }; j < i; ++j) { oss << "  "; }
      oss << "<- " << chain[i]->identity;
      if (!chain[i]->declaring_file_path.empty()) {
        oss << " (declared in " << chain[i]->declaring_file_path.filename().string()
            << ")";
      }
      oss << "\n";
    }
  }

  return oss.str();
}

}  // namespace envy
