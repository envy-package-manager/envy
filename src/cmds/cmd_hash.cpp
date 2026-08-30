#include "cmd_hash.h"

#include "sha256.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include "cli_parse.h"

#include <filesystem>
#include <memory>
#include <stdexcept>

namespace envy {

namespace {

void hash_one_file(std::filesystem::path const &file,
                   std::optional<std::string> const &prefix,
                   tui::section_handle section) {
  auto const hash{ tui_actions::sha256_tracked(file, section, "hash") };
  auto const hex{ util_bytes_to_hex(hash.data(), hash.size()) };
  auto const name{ file.filename().string() };

  if (prefix) {
    tui::print_stdout("%s  %s%s\n", hex.c_str(), prefix->c_str(), name.c_str());
  } else {
    tui::print_stdout("%s  %s\n", hex.c_str(), name.c_str());
  }
}

}  // namespace

cli_cmd &cmd_hash::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("hash", "Compute SHA256 hash of files") };
  sub.pos("paths", c.paths, "Files and/or directories to hash").required();
  sub.opt("--prefix", c.prefix, "URL prefix for output lines");
  return sub;
}

cmd_hash::cmd_hash(cmd_hash::cfg cfg,
                   std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_hash::execute() {
  namespace fs = std::filesystem;

  if (cfg_.paths.empty()) {
    throw std::runtime_error("hash: at least one path is required");
  }

  // One row for the command, reused per file: a big archive is a real wait, and the
  // digests themselves go to stdout.
  auto const section{ tui::section_create() };

  for (auto const &path : cfg_.paths) {
    if (!fs::exists(path)) {
      throw std::runtime_error("hash: path does not exist: " + path.string());
    }

    if (fs::is_directory(path)) {
      for (auto const &e : fs::directory_iterator(path)) {
        if (!e.is_regular_file()) { continue; }
        auto const &p{ e.path() };
        if (p.extension() != ".zst") { continue; }
        auto stem_path{ p.stem() };
        if (stem_path.extension() != ".tar") { continue; }
        hash_one_file(p, cfg_.prefix, section);
      }
    } else {
      hash_one_file(path, cfg_.prefix, section);
    }
  }
}

}  // namespace envy
