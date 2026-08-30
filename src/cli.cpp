#include "cli.h"
#include "cli_parse.h"
#include "tui.h"

#include <concepts>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace envy {

namespace {

// Every command's config lives for the whole parse, so registration can hand the parser a
// pointer to the field it fills. The tuple mirrors the variant, alternative for
// alternative, which is what lets a selected command be looked up by index.
template <typename>
struct cfg_store;
template <typename... Ts>
struct cfg_store<std::variant<Ts...>> {
  using type = std::tuple<Ts...>;
};
using cfg_store_t = cfg_store<cli_args::cmd_cfg_t>::type;

template <typename T, std::size_t I = 0>
constexpr int cfg_index() {
  if constexpr (std::is_same_v<T, std::variant_alternative_t<I, cli_args::cmd_cfg_t>>) {
    return static_cast<int>(I);
  } else {
    return cfg_index<T, I + 1>();
  }
}

}  // namespace

cli_args cli_parse(int argc, char **argv) {
  // A leading '/' never introduces an option, so absolute POSIX-style paths like
  // "/tmp/file" are treated as positional arguments on Windows too.
  cli_cmd app{ nullptr, "envy", "envy - freeform package manager" };

  bool verbose{ false };
  auto const verbose_flag{ app.flag(
      "--verbose",
      verbose,
      "Verbose logging: per-package decision narrative, decorated with timestamp and "
      "level") };

  bool quiet{ false };
  app.flag("-q,--quiet", quiet, "Quiet logging: warnings and errors only")
      .excludes(verbose_flag);

  std::optional<std::filesystem::path> cache_root;
  app.opt("--cache-root", cache_root, "Cache root directory (overrides default)")
      .envname("ENVY_CACHE_ROOT");

  // take_last, not the default reject-duplicates: a bin dir's launcher injects this ahead
  // of the user's own argv, so a hand-typed --project has to be able to override it.
  std::optional<std::filesystem::path> project_dir;
  app.opt("--project",
          project_dir,
          "Operate on the project containing DIR (walks up from DIR for envy.lua) "
          "instead of the one containing the current directory")
      .check_dir()
      .take_last();

  std::string trace_spec;
  auto const trace_option{
    app.opt("--trace",
            trace_spec,
            "Enable trace logging. Provide a comma-separated list: 'stderr' for "
            "human-readable stderr and/or 'file:<path>' for JSONL file output. Defaults "
            "to stderr if no value provided.")
        .optional_value()
  };

  // Support version flags (-v / --version) triggering version command directly.
  bool version_flag_short{ false };
  bool version_flag_long{ false };
  app.flag("-v", version_flag_short, "Show version information (alias for version "
                                     "subcommand)");
  app.flag("--version",
           version_flag_long,
           "Show version information (alias for version subcommand)");

  cfg_store_t store{};

  auto const register_cmds{ [&]<typename... Ts>(cli_cmd &parent) {
    ((void)Ts::register_cli(parent, std::get<typename Ts::cfg>(store))
         .set_id(cfg_index<typename Ts::cfg>()),
     ...);
  } };

  // Subcommands are listed in registration order, so this list is sorted by subcommand
  // name; split around 'cache-test' to keep it in place. cli_tests enforces the order.
  register_cmds.operator()<cmd_cache>(app);

#ifdef ENVY_FUNCTIONAL_TESTER
  // Test-only cache drivers get their own parent so the production "cache"
  // command never has to reason about child subcommands.
  register_cmds.operator()<cmd_cache_ensure_package, cmd_cache_ensure_spec>(
      app.sub("cache-test", "Drive cache primitives directly (test only)"));
#endif

  register_cmds.operator()<cmd_deploy,
                           cmd_export,
                           cmd_extract,
                           cmd_fetch,
                           cmd_git_resolve,
                           cmd_hash,
                           cmd_import,
                           cmd_init,
                           cmd_install,
                           cmd_lua,
                           cmd_merge_depot,
                           cmd_mirror_envy,
                           cmd_package,
                           cmd_product,
                           cmd_run,
                           cmd_shell,
                           cmd_sync,
#ifdef ENVY_FUNCTIONAL_TESTER
                           cmd_trace_schema,
#endif
                           cmd_use,
                           cmd_version>(app);

  cli_args args{};
  std::optional<cli_args::cmd_cfg_t> cmd_cfg;

  auto const parsed{ cli_run(app, argc, argv) };
  if (!parsed.error.empty()) {
    args.cli_output = parsed.help_text + "Error: " + parsed.error + "\n";
  } else if (!parsed.help_requested && parsed.selected_id >= 0) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      ((parsed.selected_id == static_cast<int>(I) ? void(cmd_cfg = std::get<I>(store))
                                                  : void()),
       ...);
    }(std::make_index_sequence<std::variant_size_v<cli_args::cmd_cfg_t>>{});
  }

  // Handle trace logging: --trace defaults to stderr if no value provided
  bool const trace_requested{ trace_option.count() > 0 };
  auto const trace_specs_tokens{ [&] {
    std::vector<std::string> tokens;
    if (trace_requested) {
      if (trace_spec.empty()) {
        tokens.push_back("stderr");
      } else {
        for (std::string_view sv{ trace_spec }; !sv.empty();) {
          auto const pos{ sv.find(',') };
          auto const token{ sv.substr(0, pos) };
          if (!token.empty()) { tokens.emplace_back(token); }
          sv = (pos == std::string_view::npos) ? std::string_view{} : sv.substr(pos + 1);
        }
      }
    }
    return tokens;
  }() };

  // Trace output is a sink configuration, orthogonal to log verbosity: enabling
  // --trace no longer changes the log level or decoration. The log level is set
  // solely by --verbose / --quiet / (default).
  for (auto const &spec : trace_specs_tokens) {
    if (spec.empty() || spec == "stderr") {
      args.trace_outputs.push_back({ tui::trace_output_type::std_err, std::nullopt });
    } else if (spec.rfind("file:", 0) == 0 && spec.size() > 5) {
      args.trace_outputs.push_back(
          { tui::trace_output_type::file, std::filesystem::path{ spec.substr(5) } });
    } else {
      args.cli_output = "Invalid trace output spec: " + spec;
      args.trace_outputs.clear();
      args.cmd_cfg.reset();
      cmd_cfg.reset();
      break;
    }
  }
  if (!trace_specs_tokens.empty() && args.trace_outputs.empty() &&
      args.cli_output.empty()) {
    args.trace_outputs.push_back({ tui::trace_output_type::std_err, std::nullopt });
  }

  if (verbose) {
    args.verbosity = tui::level::TUI_DEBUG;
    args.decorated_logging = true;
  } else if (quiet) {
    args.verbosity = tui::level::TUI_WARN;
    args.decorated_logging = false;
  } else {
    args.verbosity = tui::level::TUI_INFO;
    args.decorated_logging = false;
  }

  if (parsed.error.empty() && !parsed.help_requested &&
      (version_flag_short || version_flag_long)) {
    args.cmd_cfg = cmd_version::cfg{};
    args.cache_root = cache_root;
    return args;
  }

  if (cmd_cfg) {
    args.cmd_cfg = *cmd_cfg;
    // One global option, distributed to whichever config was selected; a command that
    // never loads a manifest does not derive from the anchor base and is skipped.
    if (project_dir) {
      std::visit(
          [&](auto &c) {
            if constexpr (std::derived_from<std::decay_t<decltype(c)>,
                                            cmd_project_anchor>) {
              c.project_dir = project_dir;
            }
          },
          *args.cmd_cfg);
    }
  } else if (args.cli_output.empty()) {
    args.cli_output = parsed.help_text;
  }

  args.cache_root = cache_root;
  return args;
}

}  // namespace envy
