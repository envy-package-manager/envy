#include "cmd_fetch.h"

#include "fetch.h"
#include "phases/phase_fetch.h"
#include "tui.h"
#include "tui_actions.h"
#include "uri.h"

#include "CLI11.hpp"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace envy {

void cmd_fetch::register_cli(CLI::App &app, std::function<void(cfg)> on_selected) {
  auto *sub{ app.add_subcommand("fetch", "Download resource to local file") };
  auto cfg_ptr{ std::make_shared<cfg>() };
  sub->add_option("source", cfg_ptr->source, "Source URI (http/https/git/etc.)")
      ->required();
  sub->add_option("destination", cfg_ptr->destination, "Destination file path")
      ->required();
  sub->add_option("--manifest-root",
                  cfg_ptr->manifest_root,
                  "Manifest root for resolving relative file URIs");
  sub->add_option("--ref", cfg_ptr->ref, "Git ref (branch/tag/SHA) for git sources");
  sub->callback(
      [cfg_ptr, on_selected = std::move(on_selected)] { on_selected(*cfg_ptr); });
}

cmd_fetch::cmd_fetch(cmd_fetch::cfg cfg,
                     std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_fetch::execute() {
  if (cfg_.source.empty()) { throw std::runtime_error("fetch: source URI is empty"); }
  if (cfg_.destination.empty()) {
    throw std::runtime_error("fetch: destination path is empty");
  }

  auto const scheme{ uri_classify(cfg_.source).scheme };
  if ((scheme == uri_scheme::GIT || scheme == uri_scheme::GIT_HTTPS) &&
      (!cfg_.ref.has_value() || cfg_.ref->empty())) {
    throw std::runtime_error("fetch: git sources require --ref <branch|tag|sha>");
  }

  auto req{ url_to_fetch_request(cfg_.source,
                                 cfg_.destination,
                                 cfg_.ref,
                                 std::nullopt,
                                 "fetch",
                                 cfg_.manifest_root) };

  // The whole job of this command is a download, so it draws a bar like any other.
  auto const results{ tui_actions::fetch_tracked({ std::move(req) },
                                                 "fetch",
                                                 { cfg_.source }) };
  if (results.empty()) { throw std::runtime_error("fetch: no result returned"); }

  if (std::holds_alternative<std::string>(results[0])) {
    throw std::runtime_error("fetch: " + std::get<std::string>(results[0]));
  }

  auto const &result{ std::get<fetch_result>(results[0]) };
  tui::debug("Fetched %s -> %s",
             result.resolved_source.string().c_str(),
             result.resolved_destination.string().c_str());
}

}  // namespace envy
