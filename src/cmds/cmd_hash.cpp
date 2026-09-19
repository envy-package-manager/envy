#include "cmd_hash.h"

#include "sha256.h"
#include "tree_hash.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"
#include "vendor.h"

#include "cli_parse.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace envy {

namespace {

void hash_one_file(std::filesystem::path const &file,
                   std::optional<std::string> const &prefix) {
  // A big archive is a real wait, so it draws; the bar then commits above the digest it
  // was drawn for.
  auto const section{ tui::section_create() };
  auto const hash{ tui_actions::sha256_tracked(file, section, "hash") };
  tui::section_commit(section);
  auto const hex{ util_bytes_to_hex(hash.data(), hash.size()) };
  auto const name{ file.filename().string() };

  if (prefix) {
    tui::print_stdout("%s  %s%s\n", hex.c_str(), prefix->c_str(), name.c_str());
  } else {
    tui::print_stdout("%s  %s\n", hex.c_str(), name.c_str());
  }
}

// Stage times are summed across workers, so read the share, not the total. These are
// log lines, so stderr; stdout carries the digest alone and stays pipeable.
void print_stats(tree_hash_stats const &s, tree_hash_result const &result) {
  auto const busy{ s.scan_ns + s.read_ns + s.hash_ns + s.wait_ns };
  auto const pct{ [busy](std::uint64_t ns) {
    return busy ? 100.0 * static_cast<double>(ns) / static_cast<double>(busy) : 0.0;
  } };
  auto const ms{ [](std::uint64_t ns) { return static_cast<double>(ns) / 1e6; } };

  tui::info("  threads %u  dirs %llu  files %llu  bytes %llu  wall %.1fms",
            s.threads,
            static_cast<unsigned long long>(s.dirs),
            static_cast<unsigned long long>(result.files),
            static_cast<unsigned long long>(result.bytes),
            ms(s.wall_ns));
  tui::info("  scan %8.1fms %5.1f%%   read %8.1fms %5.1f%%",
            ms(s.scan_ns),
            pct(s.scan_ns),
            ms(s.read_ns),
            pct(s.read_ns));
  tui::info("  hash %8.1fms %5.1f%%   wait %8.1fms %5.1f%%   fold %8.1fms",
            ms(s.hash_ns),
            pct(s.hash_ns),
            ms(s.wait_ns),
            pct(s.wait_ns),
            ms(s.fold_ns));

  // Spread is the story, so print that rather than N numbers. Bytes matter as much as
  // files: one huge file among a thousand small ones balances by count and not by work.
  if (s.threads && !s.files_per_worker.empty()) {
    auto const [flo, fhi]{ std::ranges::minmax(s.files_per_worker) };
    auto const [blo, bhi]{ std::ranges::minmax(s.bytes_per_worker) };
    tui::info(
        "  files/worker %llu..%llu (ideal %llu)   bytes/worker %llu..%llu "
        "(ideal %llu)",
        static_cast<unsigned long long>(flo),
        static_cast<unsigned long long>(fhi),
        static_cast<unsigned long long>(result.files / s.threads),
        static_cast<unsigned long long>(blo),
        static_cast<unsigned long long>(bhi),
        static_cast<unsigned long long>(result.bytes / s.threads));
  }
}

void print_stats_json(tree_hash_stats const &s) {
  tui::print_stdout(
      ", \"dirs\": %llu, \"scan_ns\": %llu, \"read_ns\": %llu, \"hash_ns\": %llu, "
      "\"wait_ns\": %llu, \"fold_ns\": %llu",
      static_cast<unsigned long long>(s.dirs),
      static_cast<unsigned long long>(s.scan_ns),
      static_cast<unsigned long long>(s.read_ns),
      static_cast<unsigned long long>(s.hash_ns),
      static_cast<unsigned long long>(s.wait_ns),
      static_cast<unsigned long long>(s.fold_ns));

  tui::print_stdout(", \"files_per_worker\": [");
  for (std::size_t i{ 0 }; i < s.files_per_worker.size(); ++i) {
    tui::print_stdout("%s%llu",
                      i ? ", " : "",
                      static_cast<unsigned long long>(s.files_per_worker[i]));
  }
  tui::print_stdout("]");
}

void hash_one_tree(std::filesystem::path const &dir,
                   tree_filter const &filter,
                   cmd_hash::cfg const &cfg,
                   bool first) {
  // Timed around tree_hash alone; every command self-deploys, so process time lies.
  auto const start{ std::chrono::steady_clock::now() };
  tree_hash_stats stats;
  auto const result{ tree_hash(dir,
                               filter,
                               static_cast<unsigned>(cfg.threads),
                               cfg.stats ? &stats : nullptr) };
  auto const duration_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count() };

  auto const hex{ util_bytes_to_hex(result.digest.data(), result.digest.size()) };
  if (!cfg.json) {
    tui::print_stdout("%s  %s\n", hex.c_str(), dir.string().c_str());
    if (cfg.stats) { print_stats(stats, result); }
    return;
  }

  tui::print_stdout(
      "%s  {\"path\": \"%s\", \"digest\": \"%s\", \"files\": %llu, \"bytes\": %llu, "
      "\"threads\": %d, \"duration_ms\": %lld",
      first ? "[" : ",\n",
      util_escape_json_string(dir.generic_string()).c_str(),
      hex.c_str(),
      static_cast<unsigned long long>(result.files),
      static_cast<unsigned long long>(result.bytes),
      cfg.threads,
      static_cast<long long>(duration_ms));
  if (cfg.stats) { print_stats_json(stats); }
  tui::print_stdout("}");
}

}  // namespace

cli_cmd &cmd_hash::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("hash", "Compute SHA256 hash of files, or BLAKE3 of a directory") };
  sub.pos("paths", c.paths, "Files and/or directories to hash").required();
  auto const prefix{ sub.opt("--prefix", c.prefix, "URL prefix for output lines") };
  // --prefix decorates depot manifest lines, which subtree mode does not produce.
  sub.flag("--tree", c.tree, "Hash each directory as a whole subtree (BLAKE3)")
      .excludes(prefix);
  sub.opt("--only", c.only, "Subtree selector; '!' prefix excludes (repeatable)");
  sub.opt("--threads", c.threads, "Hashing threads for --tree (0 = hardware concurrency)");
  sub.flag("--json", c.json, "Emit machine-readable results for --tree");
  sub.flag("--stats", c.stats, "Report per-stage timings and per-worker balance");
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
  if (!cfg_.tree && (!cfg_.only.empty() || cfg_.threads || cfg_.json || cfg_.stats)) {
    throw std::runtime_error(
        "hash: --only, --threads, --json and --stats apply to --tree; per-file hashing "
        "takes neither selectors nor a thread count");
  }
  if (cfg_.threads < 0) { throw std::runtime_error("hash: --threads cannot be negative"); }

  if (cfg_.tree) {
    auto const filter{ vendor_parse_selectors(cfg_.only, "hash --only") };
    for (size_t i{ 0 }; i < cfg_.paths.size(); ++i) {
      auto const &path{ cfg_.paths[i] };
      if (!fs::is_directory(path)) {
        throw std::runtime_error("hash: --tree needs a directory: " + path.string());
      }
      hash_one_tree(path, filter, cfg_, i == 0);
    }
    if (cfg_.json) { tui::print_stdout("\n]\n"); }
    return;
  }

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
        hash_one_file(p, cfg_.prefix);
      }
    } else {
      hash_one_file(path, cfg_.prefix);
    }
  }
}

}  // namespace envy
