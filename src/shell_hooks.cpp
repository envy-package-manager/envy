#include "shell_hooks.h"

#include "embedded_init_resources.h"
#include "tui.h"
#include "util.h"
#include "version.h"

#include <fstream>
#include <iterator>
#include <string>
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

struct hook_stamp {
  std::string_view writer;  // the envy version that wrote the file
  std::string_view digest;  // the hook resource that file came from
};

// Line 2 of every hook, in each shell's assignment syntax: <version>:<digest>. A file from
// the retired integer scheme, an unstamped one and a missing one all read as {}.
hook_stamp stamp_of(std::string_view content) {
  constexpr std::string_view kKey{ "_ENVY_HOOK_STAMP" };
  auto const key{ content.find(kKey) };
  if (key == std::string_view::npos) { return {}; }
  auto const begin{ content.find_first_not_of("= \"", key + kKey.size()) };
  if (begin == std::string_view::npos) { return {}; }
  auto const end{ content.find_first_of("\"\r\n \t", begin) };
  auto const token{ content.substr(begin, end - begin) };  // npos-safe: substr clamps
  auto const colon{ token.find(':') };
  return colon == std::string_view::npos
             ? hook_stamp{}
             : hook_stamp{ token.substr(0, colon), token.substr(colon + 1) };
}

std::string read_text(std::filesystem::path const &path) {
  std::ifstream in{ path, std::ios::binary };
  return { std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
}

}  // namespace

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
    try {
      std::string const want{ util_inflate_resource(h.res) };
      std::string const have{ read_text(hook_path) };
      if (have == want) { continue; }

      // Binaries live in <root>/envy/<version>/, yet every one writes this one file: a
      // pinned project and a newer envy meet here, and must not trade writes forever.
      hook_stamp const mine{ stamp_of(want) }, theirs{ stamp_of(have) };
      if (theirs.writer != mine.writer) {  // our own version's copy we always repair
        if (theirs.digest == mine.digest) { continue; }  // same hook, another one's label
        if (version_is_newer(theirs.writer, mine.writer)) { continue; }  // a newer envy's
      }

      bool const was_update{ fs::exists(hook_path) };
      util_write_file(hook_path, want);
      ++written;
      if (was_update) { tui::info("Shell hook updated (%s) — restart your shell", h.ext); }
    } catch (std::exception const &e) {
      tui::warn("Failed to write shell hook (%s): %s", h.ext, e.what());
    }
  }

  return written;
}

}  // namespace envy::shell_hooks
