#include "cmd_use.h"

#include "envy_release.h"
#include "fetch.h"
#include "manifest.h"
#include "platform.h"
#include "sha256.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include "cli_parse.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace envy {

namespace {

// One byte range of the manifest and what replaces it. An insertion is an empty range; a
// deletion is empty text.
struct splice {
  size_t begin{};
  size_t end{};
  std::string text;
};

// A new pin goes on its own line directly below the version it attests: the header scan
// stops at the first line of code, so a directive placed anywhere lower is read by
// nothing.
splice insert_sums_line(std::string_view content,
                        envy_directive_span const &ver,
                        std::string_view hex) {
  auto const indent{ content.substr(
      ver.line_begin,
      content.find_first_not_of(" \t", ver.line_begin) - ver.line_begin) };
  auto const line{ std::string{ indent } + "-- @envy sha256sums \"" + std::string{ hex } +
                   "\"" };
  auto const eol{ (ver.line_end > ver.line_begin && content[ver.line_end - 1] == '\r')
                      ? std::string_view{ "\r\n" }
                      : std::string_view{ "\n" } };

  // A header running to EOF has no terminator to insert after, so the new line brings its
  // own leading break -- appending one would leave the pin on the version directive's
  // line.
  return ver.line_end >= content.size()
             ? splice{ content.size(), content.size(), std::string{ eol } + line }
             : splice{ ver.line_end + 1, ver.line_end + 1, line + std::string{ eol } };
}

// '(none)' rather than an empty pair of quotes: gaining and losing attestation are the two
// edits a reader most needs to see unambiguously.
std::string quoted_or_none(std::optional<std::string> const &value) {
  return value ? "\"" + *value + "\"" : "(none)";
}

// The release's SHA256SUMS, hashed -- the value '@envy sha256sums' carries. Must describe
// the published file byte-for-byte, so fetching it doubles as proof the release exists.
std::string fetch_sums_hex(std::string const &mirror, std::string const &version) {
  auto const url{ envy_release_sums_url(mirror, version) };
  auto const tmp{ platform::create_unique_temp_dir("envy-use-sums") };
  scoped_path_cleanup const cleanup{ tmp };
  auto const dest{ tmp / std::string{ kEnvyReleaseSumsFile } };

  std::vector<std::string> const labels{ std::string{ kEnvyReleaseSumsFile } };
  auto const results{
    tui_actions::fetch_tracked({ fetch_request_from_url(url, dest) }, "use", labels)
  };
  if (results.empty()) {
    throw std::runtime_error("use: failed to download " + url + ": unknown error");
  }
  if (auto const *err{ std::get_if<std::string>(&results[0]) }) {
    throw std::runtime_error("use: failed to download " + url + ": " + *err +
                             "\n       envy " + version +
                             " may not be published on that mirror.");
  }

  auto const hash{ sha256(dest) };
  return util_bytes_to_hex(hash.data(), hash.size());
}

}  // namespace

cli_cmd &cmd_use::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("use", "Retarget the manifest at an envy version") };
  sub.pos("version", c.version, "Envy version to use, e.g. 1.2.3").required();
  auto const manifest_opt{
    sub.opt("--manifest", c.manifest_path, "Path to envy.lua manifest")
  };
  sub.flag("--subproject", c.subproject, "Use nearest manifest instead of walking to root")
      .excludes(manifest_opt);
  sub.opt("--mirror",
          c.mirror,
          "Fetch SHA256SUMS from this mirror instead of the manifest's");
  auto const pin_opt{ sub.flag(
      "--pin-sums",
      c.pin_sums,
      "Add an @envy sha256sums pin even if the manifest has none") };
  sub.flag("--no-pin-sums",
           c.no_pin_sums,
           "Drop the @envy sha256sums pin, leaving downloads unattested")
      .excludes(pin_opt);
  sub.flag("--force",
           c.force,
           "Skip the SHA256SUMS fetch that proves the release exists (unpinned only)");
  return sub;
}

std::string use_rewrite_header(std::string_view content,
                               std::string_view version,
                               std::optional<std::string> const &sums_hex) {
  auto const ver{ find_envy_directive(content, "version") };
  if (!ver) {
    throw std::runtime_error(
        "manifest header has no '@envy version' directive to retarget");
  }

  auto edits{ [&, sums{ find_envy_directive(content, "sha256sums") }] {
    std::vector<splice> v{
      splice{ ver->value_begin, ver->value_end, std::string{ version } }
    };
    if (sums && sums_hex) {
      v.push_back(splice{ sums->value_begin, sums->value_end, *sums_hex });
    } else if (sums) {
      // The whole line goes, terminator included, so dropping a pin leaves no blank line
      // behind. A header that ends at EOF has no terminator to take.
      v.push_back(splice{ sums->line_begin,
                          std::min(sums->line_end + 1, content.size()),
                          std::string{} });
    } else if (sums_hex) {
      v.push_back(insert_sums_line(content, *ver, *sums_hex));
    }
    return v;
  }() };

  // Highest offset first, so an edit that shifts the bytes after it -- inserting or
  // deleting a whole line -- cannot invalidate an offset still waiting to be spliced.
  std::ranges::sort(edits, std::ranges::greater{}, &splice::begin);

  std::string out{ content };
  for (auto const &e : edits) { out.replace(e.begin, e.end - e.begin, e.text); }
  return out;
}

cmd_use::cmd_use(cmd_use::cfg cfg,
                 std::optional<std::filesystem::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_use::execute() {
  if (!envy_release_version_is_valid(cfg_.version)) {
    throw std::runtime_error("use: invalid version string: " + cfg_.version);
  }
  if (cfg_.mirror) { envy_release_validate_mirror(*cfg_.mirror, "use"); }

  // Deliberately not cmd_startup_load: re-execing into the version the manifest names is
  // the one state this repairs, and reading the header leaves broken Lua below it no
  // obstacle.
  auto const path{ manifest::find_manifest_path(
      cfg_.manifest_path,
      cfg_.subproject,
      subproject_anchor(cfg_.subproject, cfg_.project_dir)) };
  auto const bytes{ util_load_file(path) };
  std::string_view const content{ reinterpret_cast<char const *>(bytes.data()),
                                  bytes.size() };
  auto const meta{ parse_envy_meta(content) };
  auto const name{ path.filename().string() };

  // Not broken, just floating to latest. Nothing to retarget, and adding a directive would
  // silently convert the project to pinned, so leave that conversion to its author.
  if (!meta.version) {
    throw std::runtime_error(
        "use: " + path.string() +
        " has no '@envy version' directive -- the project floats to the latest release.\n"
        "       'envy use' retargets a pinned version. To start pinning, add\n"
        "           -- @envy version \"" +
        cfg_.version + "\"\n       to the manifest header.");
  }

  // The manifest keeps whatever it already chose: gaining or losing attestation is never a
  // side effect of changing versions.
  auto const pin{ cfg_.pin_sums || (!cfg_.no_pin_sums && meta.sha256sums.has_value()) };
  if (pin && cfg_.force) {
    throw std::runtime_error(
        "use: --force cannot be combined with a sums pin: the pin's value comes only from "
        "the release's SHA256SUMS. Drop --force, or pass --no-pin-sums to stop "
        "attesting.");
  }

  auto const mirror{ [this, &meta]() -> std::string {
    if (cfg_.mirror) { return *cfg_.mirror; }
    if (char const *env{ std::getenv("ENVY_MIRROR") }; env && *env) { return env; }
    if (meta.mirror) { return *meta.mirror; }
    return std::string{ kEnvyReleaseDownloadUrl };
  }() };

  // Network before file, so a failed fetch leaves the manifest as it was. Worth doing with
  // no pin to write: it turns an unpublished version into an error here, not a failed
  // bootstrap.
  auto const sums_hex{ [this, &mirror, pin]() -> std::optional<std::string> {
    if (cfg_.force) { return std::nullopt; }
    auto hex{ fetch_sums_hex(mirror, cfg_.version) };
    return pin ? std::optional{ std::move(hex) } : std::nullopt;
  }() };

  auto const updated{ use_rewrite_header(content, cfg_.version, sums_hex) };
  if (updated == content) {
    tui::info("%s: already @envy version \"%s\"%s",
              name.c_str(),
              cfg_.version.c_str(),
              pin ? " with a current sums pin" : "");
    return;
  }

  util_write_file(path, updated);

  // Loud by design. The edit is a durable change to a checked-in file, not a shell-session
  // switch, and the verb invites that misreading.
  bool const version_changed{ *meta.version != cfg_.version };
  if (version_changed) {
    tui::info("%s: @envy version \"%s\" -> \"%s\"",
              name.c_str(),
              meta.version->c_str(),
              cfg_.version.c_str());
  }
  if (meta.sha256sums != sums_hex) {
    tui::info("%s: @envy sha256sums %s -> %s",
              name.c_str(),
              quoted_or_none(meta.sha256sums).c_str(),
              quoted_or_none(sums_hex).c_str());
  }

  // The bootstrap scripts and .luarc.json are stamped from the *running* binary's version,
  // so only the newly pinned envy can restamp them -- one re-exec away, on the next sync.
  if (version_changed) {
    tui::info("run 'envy sync' to restamp the bootstrap scripts and .luarc.json for %s",
              cfg_.version.c_str());
  }
}

}  // namespace envy
