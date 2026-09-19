#include "cmd_vendor.h"

#include "engine.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_cfg.h"
#include "pkg_key.h"
#include "reexec.h"
#include "self_deploy.h"
#include "tui.h"
#include "util.h"
#include "vendor.h"

#include "cli_parse.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace envy {

cli_cmd &cmd_vendor::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("vendor", "Restore vendored package copies in the project tree") };
  sub.pos("queries", c.queries, "Package queries to vendor");
  sub.flag("--all", c.all, "Vendor every package the manifest vendors");
  sub.flag("--force", c.force, "Repair even where vendor.auto_sync = false");
  sub.flag("--dry-run", c.dry_run, "Print what would happen; write nothing");
  sub.opt("--threads", c.threads, "Copy worker threads (0 = performance-core count)");
  sub.opt("--manifest", c.manifest_path, "Path to envy.lua manifest");
  sub.finalize(
      [](void *p) -> char const * {
        auto const &sel{ *static_cast<cfg *>(p) };
        if (sel.all && !sel.queries.empty()) {
          return "Cannot specify both package queries and --all";
        }
        if (!sel.all && sel.queries.empty()) {
          return "Must specify package queries or --all";
        }
        return nullptr;
      },
      &c);
  return sub;
}

cmd_vendor::cmd_vendor(cfg cfg, std::optional<std::filesystem::path> const &cli_cache_root)
    : cfg_{ std::move(cfg) }, cli_cache_root_{ cli_cache_root } {}

void cmd_vendor::execute() {
  auto const [m, c]{ cmd_startup_load("vendor",
                                      cfg_.manifest_path,
                                      cli_cache_root_,
                                      false,
                                      cfg_.project_dir) };

  // The whole manifest, so a collision between a target and a package nobody named is
  // still an error here rather than a wipe that lands on top of another destination.
  auto plan{ m->resolve_vendor_plan() };
  if (plan.empty()) {
    throw std::runtime_error("vendor: no package in this manifest asks to be vendored");
  }

  // One entry per package however it was reached: envy.import can splice two
  // declarations of one key, and two queries can resolve to one package. The engine
  // treats those as one package, so the report must not name it twice.
  auto const targets{ [&] {
    auto const candidates{
      cfg_.all ? std::vector<pkg_cfg const *>{ m->packages.begin(), m->packages.end() }
               : engine_resolve_targets(m->packages, cfg_.queries, "vendor")
    };

    std::vector<pkg_cfg const *> chosen;
    std::unordered_set<pkg_key> seen;
    for (auto const *cfg : candidates) {
      pkg_key key{ *cfg };
      if (!plan.find(key)) {
        // --all is "every package that vendors", so one that does not is simply not a
        // target; one named outright is a mistake worth reporting.
        if (cfg_.all) { continue; }
        throw std::runtime_error("vendor: '" + cfg->identity +
                                 "' is not vendored; give its PACKAGES entry a "
                                 "'vendor' field to copy it into the project");
      }
      if (seen.insert(std::move(key)).second) { chosen.push_back(cfg); }
    }
    return chosen;
  }() };

  // A vendored dependency of a target was not asked for, so it keeps whatever it has.
  std::unordered_set<pkg_key> const wanted{ [&] {
    std::unordered_set<pkg_key> keys;
    for (auto const *cfg : targets) { keys.emplace(*cfg); }
    return keys;
  }() };
  std::erase_if(plan.dirs,
                [&](auto const &entry) { return !wanted.contains(entry.first); });

  // --force is exactly "read every destination as auto_sync = true": the exemption is
  // the only thing standing between a mismatch and its repair.
  if (cfg_.force) {
    for (auto &[_, destination] : plan.dirs) { destination.auto_sync = true; }
  }
  plan.dry_run = cfg_.dry_run;
  plan.threads = static_cast<unsigned>(std::max(0, cfg_.threads));

  engine eng{ *c, m.get() };
  eng.set_vendor_plan(std::move(plan));
  eng.run_full(targets);  // Throws on any failure

  // The phase's own row is overwritten by the completion row, and a dry run draws none
  // at all, so the report is the command's: sections down first, then target order.
  for (auto const *cfg : targets) {
    if (pkg *p{ eng.find_exact(pkg_key{ *cfg }) }; p && p->tui_section) {
      tui::section_delete(p->tui_section);
    }
  }
  for (auto const *cfg : targets) {
    pkg const *p{ eng.find_exact(pkg_key{ *cfg }) };
    if (p && !p->vendor_outcome.empty()) {
      tui::info("[%s] %s", cfg->identity.c_str(), p->vendor_outcome.c_str());
    }
  }
}

}  // namespace envy
