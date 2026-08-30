#include "shell_hooks.h"

#include "embedded_init_resources.h"
#include "tui.h"
#include "util.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>

namespace envy::shell_hooks {

namespace {

struct hook_resource {
  char const *ext;
  gz_resource res;
};

constexpr hook_resource kHooks[] = {
  { "bash", embedded::kShellHookBash },
  { "zsh", embedded::kShellHookZsh },
  { "fish", embedded::kShellHookFish },
  { "ps1", embedded::kShellHookPs1 },
};

}  // namespace

int parse_version_from_content(std::string_view content) {
  int lines_checked{ 0 };
  while (!content.empty() && lines_checked < 5) {
    auto const nl{ content.find('\n') };
    auto const line{ content.substr(0, nl) };

    constexpr std::string_view kKey{ "_ENVY_HOOK_VERSION" };
    auto const pos{ line.find(kKey) };
    if (pos != std::string_view::npos) {
      auto rest{ line.substr(pos + kKey.size()) };
      // Skip past '=' or ' = ' or ' '
      while (!rest.empty() && (rest[0] == ' ' || rest[0] == '=')) {
        rest.remove_prefix(1);
      }
      int v{ 0 };
      if (std::sscanf(std::string(rest).c_str(), "%d", &v) == 1) { return v; }
    }

    if (nl == std::string_view::npos) { break; }
    content.remove_prefix(nl + 1);
    ++lines_checked;
  }
  return 0;
}

int parse_version(std::filesystem::path const &hook_path) {
  std::ifstream in{ hook_path };
  if (!in) { return 0; }
  char line[256];
  for (int i{ 0 }; i < 5 && in.getline(line, sizeof(line)); ++i) {
    char const *p{ std::strstr(line, "_ENVY_HOOK_VERSION") };
    if (!p) { continue; }
    p += std::strlen("_ENVY_HOOK_VERSION");
    while (*p == ' ' || *p == '=') { ++p; }
    int v{ 0 };
    if (std::sscanf(p, "%d", &v) == 1) { return v; }
  }
  return 0;
}

int ensure(std::filesystem::path const &cache_root) {
  namespace fs = std::filesystem;
  fs::path const shell_dir{ cache_root / "shell" };
  int written{ 0 };

  std::error_code ec;
  fs::create_directories(shell_dir, ec);
  if (ec) {
    tui::warn("Failed to create shell hook directory %s: %s",
              shell_dir.string().c_str(),
              ec.message().c_str());
    return 0;
  }

  for (auto const &h : kHooks) {
    fs::path const hook_path{ shell_dir / ("hook." + std::string{ h.ext }) };
    if (fs::exists(hook_path) && parse_version(hook_path) >= kShellHookVersion) {
      continue;
    }

    bool const was_update{ fs::exists(hook_path) };
    try {
      util_write_file(hook_path, util_inflate_resource(h.res));
      ++written;
      if (was_update) { tui::info("Shell hook updated (%s) — restart your shell", h.ext); }
    } catch (std::exception const &e) {
      tui::warn("Failed to write shell hook (%s): %s", h.ext, e.what());
    }
  }

  return written;
}

}  // namespace envy::shell_hooks
