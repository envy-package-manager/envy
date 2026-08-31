#include "cmd_mirror_envy.h"

#include "aws_util.h"
#include "envy_release.h"
#include "fetch.h"
#include "platform.h"
#include "sha256.h"
#include "tui.h"
#include "tui_actions.h"
#include "uri.h"
#include "util.h"

#include "cli_parse.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace envy {

namespace fs = std::filesystem;

namespace {

std::string_view strip_trailing_slashes(std::string_view s) {
  while (s.ends_with('/')) { s.remove_suffix(1); }
  return s;
}

// Owns a scratch tree for the duration of the run. util.h's scoped_path_cleanup removes a
// single entry, which is not enough here -- the staging directory has archives in it.
class scoped_temp_dir : unmovable {
 public:
  explicit scoped_temp_dir(std::filesystem::path path) : path_{ std::move(path) } {}

  ~scoped_temp_dir() {
    if (auto const ec{ platform::remove_all_with_retry(path_) }) {
      tui::warn("failed to remove staging directory %s: %s",
                path_.string().c_str(),
                ec.message().c_str());
    }
  }

  std::filesystem::path const &path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// A destination is a local directory or an s3:// URI. Everything else -- git, ssh, http --
// is rejected here rather than surfacing later as a confusing fetch error.
void validate_dest(std::string_view dest) {
  if (dest.empty()) { throw std::runtime_error("mirror-envy: destination is empty"); }

  // "s3:/bucket" has no "://", so uri_classify would call it a relative path and we would
  // silently stage into a local directory named "s3:".
  bool const looks_s3{ dest.size() >= 3 && (dest[0] == 's' || dest[0] == 'S') &&
                       dest[1] == '3' && dest[2] == ':' };
  if (looks_s3 && !(dest.size() >= 5 && dest.substr(3, 2) == "//")) {
    throw std::runtime_error("mirror-envy: malformed S3 destination '" +
                             std::string{ dest } + "' (expected s3://bucket/prefix)");
  }
}

// Progress rows are labeled by basename: every relpath under a run shares the same
// "vX.Y.Z/" prefix, which would just eat row width without distinguishing anything.
std::vector<std::string> progress_labels(std::vector<std::string> const &relpaths) {
  std::vector<std::string> labels;
  labels.reserve(relpaths.size());
  for (auto const &rel : relpaths) {
    labels.push_back(fs::path{ rel }.filename().string());
  }
  return labels;
}

}  // namespace

std::string mirror_envy_s3_root(mirror_envy_plan const &plan) {
  std::ostringstream ss;
  ss << "s3://" << plan.bucket;
  if (!plan.prefix.empty()) { ss << '/' << plan.prefix; }
  return ss.str();
}

std::string mirror_envy_s3_uri(mirror_envy_plan const &plan, std::string_view relpath) {
  std::ostringstream ss;
  ss << mirror_envy_s3_root(plan) << '/' << relpath;
  return ss.str();
}

mirror_envy_plan mirror_envy_make_plan(std::string_view version,
                                       std::string_view dest,
                                       std::string_view from_mirror) {
  if (!envy_release_version_is_valid(version)) {
    throw std::runtime_error("mirror-envy: invalid version string: " +
                             std::string{ version });
  }

  auto const from{ strip_trailing_slashes(from_mirror) };
  if (from.empty()) { throw std::runtime_error("mirror-envy: source mirror is empty"); }

  validate_dest(dest);

  mirror_envy_plan plan{};

  switch (auto const info{ uri_classify(dest) }; info.scheme) {
    case uri_scheme::S3: {
      auto const parts{ aws_s3_parse_uri(info.canonical, "mirror-envy") };
      plan.dest_is_s3 = true;
      plan.bucket = parts.bucket;
      plan.prefix = parts.key;  // already stripped of trailing slashes
      break;
    }
    case uri_scheme::LOCAL_FILE_ABSOLUTE:
    case uri_scheme::LOCAL_FILE_RELATIVE: plan.local_dir = info.canonical; break;
    default:
      throw std::runtime_error(
          "mirror-envy: destination must be a local directory or an s3:// URI (got '" +
          std::string{ dest } + "')");
  }

  plan.items.reserve(kEnvyReleaseTargets.size() + 1);
  for (auto const &target : kEnvyReleaseTargets) {
    std::ostringstream rel;
    rel << 'v' << version << '/' << envy_release_archive_name(target.os, target.arch);
    plan.items.push_back(mirror_envy_item{
        .source_url = envy_release_url(from, version, target.os, target.arch),
        .relpath = rel.str() });
  }

  // Last, so the archive rows keep their stable order in the progress display. Copied
  // verbatim rather than regenerated from the bytes we just fetched: a project's
  // `@envy sha256sums` pins this file's own hash, so regenerating it here -- even with
  // identical digests -- would change that hash on any formatting or ordering difference
  // and make the pin mirror-specific.
  std::ostringstream sums_rel;
  sums_rel << 'v' << version << '/' << kEnvyReleaseSumsFile;
  plan.sums_relpath = sums_rel.str();
  plan.items.push_back(
      mirror_envy_item{ .source_url = envy_release_sums_url(from, version),
                        .relpath = plan.sums_relpath });

  return plan;
}

cli_cmd &cmd_mirror_envy::register_cli(cli_cmd &app, cfg &c) {
  auto &sub{ app.sub("mirror-envy",
                     "Mirror an envy release for all platforms to a directory or S3 "
                     "prefix") };
  c.from = std::string{ kEnvyReleaseDownloadUrl };

  sub.pos("version", c.version, "Envy version to mirror (e.g. 1.2.3)").required();
  sub.pos("destination", c.dest, "Local directory or s3://bucket/prefix to mirror into")
      .required();
  sub.opt("--from",
          c.from,
          "Source mirror to read the release from (default: envy's GitHub releases)");
  return sub;
}

cmd_mirror_envy::cmd_mirror_envy(cmd_mirror_envy::cfg cfg,
                                 std::optional<fs::path> const & /*cli_cache_root*/)
    : cfg_{ std::move(cfg) } {}

void cmd_mirror_envy::execute() {
  auto const plan{ mirror_envy_make_plan(cfg_.version, cfg_.dest, cfg_.from) };

  // For an S3 destination the archives still have to land on disk first, because the AWS
  // upload API takes a file. That scratch tree must be uniquely created rather than named
  // predictably -- otherwise another user in a shared temp dir could pre-create the path
  // and see, or substitute, what gets uploaded -- and it must not outlive the run: six
  // release archives per invocation adds up. Use a local destination instead to keep the
  // staged bytes.
  std::optional<scoped_temp_dir> scratch;
  if (plan.dest_is_s3) {
    scratch.emplace(platform::create_unique_temp_dir("envy-mirror"));
  }
  auto const &staging{ scratch ? scratch->path() : plan.local_dir };

  std::error_code ec;
  fs::create_directories(staging / ("v" + cfg_.version), ec);
  if (ec) {
    throw std::runtime_error("mirror-envy: failed to create " + staging.string() + ": " +
                             ec.message());
  }
  if (plan.dest_is_s3) {
    // Debug, not info: the tree is transient scratch that is removed before we return.
    tui::debug("staging in %s", staging.string().c_str());
  }

  auto const item_relpaths{ [&] {
    std::vector<std::string> v;
    v.reserve(plan.items.size());
    for (auto const &item : plan.items) { v.push_back(item.relpath); }
    return v;
  }() };

  // One progress bar per object: the downloads run concurrently, so a single spinner
  // would say nothing about which one is stuck.
  std::vector<fetch_request> requests;
  requests.reserve(plan.items.size());
  for (auto const &item : plan.items) {
    requests.push_back(fetch_request_from_url(item.source_url, staging / item.relpath));
  }

  auto const results{ tui_actions::fetch_tracked(std::move(requests),
                                                 "mirror-envy",
                                                 progress_labels(item_relpaths)) };

  if (results.size() != plan.items.size()) {
    throw std::runtime_error("mirror-envy: fetch returned " +
                             std::to_string(results.size()) + " results for " +
                             std::to_string(plan.items.size()) + " requests");
  }

  size_t failed{ 0 };
  for (size_t i{ 0 }; i < results.size(); ++i) {
    if (auto const *error{ std::get_if<std::string>(&results[i]) }) {
      tui::error("%s: %s", plan.items[i].source_url.c_str(), error->c_str());
      ++failed;
    } else {
      tui::debug("fetched %s", plan.items[i].relpath.c_str());
    }
  }
  if (failed > 0) {
    throw std::runtime_error("mirror-envy: " + std::to_string(failed) + " of " +
                             std::to_string(plan.items.size()) +
                             " archives failed to download");
  }

  // Attest every archive against the checksum manifest we just fetched from the same
  // source, before anything is republished. A mirror that passes on corrupt bytes turns
  // one bad upstream fetch into a bad artifact for every consumer downstream, and a
  // project pinning this SHA256SUMS would then be attesting garbage as authentic.
  auto const sums_text{
    envy_release_load_sums(staging / plan.sums_relpath, std::nullopt, "mirror-envy")
  };
  auto const attest_section{ tui::section_create() };
  for (auto const &item : plan.items) {
    if (item.relpath == plan.sums_relpath) { continue; }
    auto const name{ fs::path{ item.relpath }.filename().string() };
    envy_release_verify_artifact(
        staging / item.relpath,
        name,
        sums_text,
        "mirror-envy",
        tui_actions::byte_progress_bar(attest_section, "mirror-envy", "attesting", name));
  }
  // One reused row over N archives, so no per-item line to commit it above: the row goes
  // and the line below is the step's record, at INFO because it is the only one.
  tui::section_delete(attest_section);
  tui::info("attested %zu archives against %s",
            plan.items.size() - 1,
            std::string{ kEnvyReleaseSumsFile }.c_str());

  // The pin a consuming project puts in its manifest. Computed from the mirrored file, so
  // it is the value that will actually verify against this mirror -- and, because the file
  // is copied verbatim, against upstream too.
  auto const sums_hash{ sha256(staging / plan.sums_relpath) };
  auto const sums_hex{ util_bytes_to_hex(sums_hash.data(), sums_hash.size()) };

  // Mirror-root "latest" so a bootstrap script can resolve the newest version from the
  // mirror rather than probing github.com. Format matches the cache's own latest file:
  // the bare version, no trailing newline.
  util_write_file(staging / kMirrorLatestFile, cfg_.version);

  if (!plan.dest_is_s3) {
    tui::info("staged envy %s (%zu objects) in %s",
              cfg_.version.c_str(),
              plan.items.size(),
              staging.string().c_str());
    tui::info("attest against it with:");
    tui::info("  -- @envy sha256sums \"%s\"", sums_hex.c_str());
    return;
  }

  // Thread per object, matching fetch()'s shape. Errors are collected so one bad key does
  // not hide the others.
  auto const relpaths{ [&] {
    std::vector<std::string> v{ item_relpaths };
    v.emplace_back(kMirrorLatestFile);
    return v;
  }() };

  auto const upload_section{ tui::section_create() };
  tui_actions::fetch_all_progress_tracker upload_tracker{ upload_section,
                                                          "mirror-envy",
                                                          progress_labels(relpaths),
                                                          "upload" };

  std::vector<std::string> errors(relpaths.size());
  {
    std::vector<std::thread> workers;
    workers.reserve(relpaths.size());
    for (size_t i{ 0 }; i < relpaths.size(); ++i) {
      workers.emplace_back([&, i] {
        auto const uri{ mirror_envy_s3_uri(plan, relpaths[i]) };
        try {
          aws_s3_upload(s3_upload_request{ .source = staging / relpaths[i],
                                           .uri = uri,
                                           .progress = upload_tracker.make_callback(i) });
          tui::debug("uploaded %s", uri.c_str());
        } catch (std::exception const &e) {
          errors[i] = uri + ": " + e.what();
        } catch (...) { errors[i] = uri + ": unknown error during upload"; }
      });
    }
    for (auto &t : workers) { t.join(); }
  }

  // Same bargain as a download row: a finished upload commits its full bar, a failed one
  // yields the row to the error text below.
  if (std::ranges::all_of(errors, [](std::string const &e) { return e.empty(); })) {
    upload_tracker.finish();
    tui::section_commit(upload_section);
  } else {
    tui::section_delete(upload_section);
  }

  size_t upload_failures{ 0 };
  for (auto const &error : errors) {
    if (error.empty()) { continue; }
    tui::error("%s", error.c_str());
    ++upload_failures;
  }
  if (upload_failures > 0) {
    throw std::runtime_error("mirror-envy: " + std::to_string(upload_failures) + " of " +
                             std::to_string(relpaths.size()) + " uploads failed");
  }

  auto const root{ mirror_envy_s3_root(plan) };
  tui::info("mirrored envy %s (%zu objects) to %s",
            cfg_.version.c_str(),
            relpaths.size(),
            root.c_str());
  tui::info("point envy.lua at it with:");
  tui::info("  -- @envy version \"%s\"", cfg_.version.c_str());
  tui::info("  -- @envy mirror \"%s\"", root.c_str());
  tui::info("  -- @envy sha256sums \"%s\"", sums_hex.c_str());
}

}  // namespace envy
