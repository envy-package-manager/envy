#include "package_depot.h"

#include "cache.h"
#include "fetch.h"
#include "tui.h"
#include "tui_actions.h"
#include "util.h"

#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace envy {

namespace {

bool is_hex_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string lowercase_hex(std::string_view hex) {
  std::string result{ hex };
  for (auto &c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

bool is_valid_sha256_hex(std::string_view hex) {
  if (hex.size() != 64) { return false; }
  for (char const c : hex) {
    if (!is_hex_char(c)) { return false; }
  }
  return true;
}

// Extract the archive filename stem (filename minus .tar.zst) from a URL.
// Returns nullopt (with a warning naming `context`) when the URL has no
// .tar.zst extension or the stem doesn't parse as an archive filename.
std::optional<std::string> stem_from_url(std::string_view url, std::string_view context) {
  auto const slash_pos{ url.rfind('/') };
  std::string_view filename{ (slash_pos != std::string_view::npos &&
                              slash_pos + 1 < url.size())
                                 ? url.substr(slash_pos + 1)
                                 : url };

  std::string_view stem{ filename };
  if (stem.size() > 8 && stem.substr(stem.size() - 8) == ".tar.zst") {
    stem = stem.substr(0, stem.size() - 8);
  } else {
    tui::warn("depot: skipping entry without .tar.zst extension: %s",
              std::string(context).c_str());
    return std::nullopt;
  }

  if (!util_parse_archive_filename(stem)) {
    tui::warn("depot: skipping unparseable entry: %s", std::string(context).c_str());
    return std::nullopt;
  }

  return std::string{ stem };
}

// Parse a single manifest text into a map of filename stem → depot entry.
// Supports two line formats:
//   <URL>                           (plain URL, no hash — rejected when require_sha256)
//   <64hex>  <URL>                  (SHA256 hash + two spaces + URL)
std::unordered_map<std::string, depot_entry> parse_manifest_text(std::string_view text,
                                                                 bool require_sha256) {
  std::unordered_map<std::string, depot_entry> entries;

  std::istringstream stream{ std::string{ text } };
  std::string line;

  while (std::getline(stream, line)) {
    // Trim trailing \r for Windows line endings
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }

    // Skip blank lines and comments
    if (line.empty()) { continue; }
    if (line[0] == '#') { continue; }

    // Detect SHA256 prefix: exactly 64 hex chars followed by two spaces
    std::optional<std::string> sha256_hash;
    std::string_view url_part{ line };

    if (line.size() > 66 && line[64] == ' ' && line[65] == ' ' &&
        is_valid_sha256_hex(std::string_view{ line }.substr(0, 64))) {
      sha256_hash = lowercase_hex(std::string_view{ line }.substr(0, 64));
      url_part = std::string_view{ line }.substr(66);
    }

    auto stem{ stem_from_url(url_part, line) };
    if (!stem) { continue; }

    if (require_sha256 && !sha256_hash) {
      tui::warn("depot: skipping line without SHA256: %s", line.c_str());
      continue;
    }

    entries.try_emplace(std::move(*stem),
                        depot_entry{ std::string(url_part), std::move(sha256_hash) });
  }

  return entries;
}

}  // namespace

package_depot_index package_depot_index::build(std::vector<std::string> const &depot_urls,
                                               std::filesystem::path const &tmp_dir) {
  if (depot_urls.empty()) { return {}; }

  // Download all depot manifests in parallel
  struct manifest_download {
    std::string url;
    std::filesystem::path dest;
    std::string content;
    bool ok{ false };
  };

  std::vector<manifest_download> downloads;
  downloads.reserve(depot_urls.size());

  std::vector<fetch_request> requests;
  requests.reserve(depot_urls.size());

  // Maps each request index back to its download index (skipped URLs produce no request).
  std::vector<size_t> request_to_download;
  request_to_download.reserve(depot_urls.size());

  for (size_t i{ 0 }; i < depot_urls.size(); ++i) {
    auto &dl{ downloads.emplace_back() };
    dl.url = depot_urls[i];
    dl.dest = tmp_dir / ("depot-manifest-" + std::to_string(i) + ".txt");

    try {
      requests.push_back(fetch_request_from_url(dl.url, dl.dest));
      request_to_download.push_back(i);
    } catch (std::exception const &) {
      tui::warn("depot: unsupported scheme for depot manifest: %s", dl.url.c_str());
    }
  }

  if (!requests.empty()) {
    auto const labels{ [&] {
      std::vector<std::string> l;
      l.reserve(requests.size());
      for (auto idx : request_to_download) {
        auto const &url{ downloads[idx].url };
        auto const slash{ url.rfind('/') };
        l.push_back(slash != std::string::npos ? url.substr(slash + 1) : url);
      }
      return l;
    }() };

    auto const results{
      tui_actions::fetch_tracked(std::move(requests), "depot", labels, "#depot")
    };

    for (size_t req_idx{ 0 }; req_idx < results.size(); ++req_idx) {
      auto const dl_idx{ request_to_download[req_idx] };
      auto &dl{ downloads[dl_idx] };

      auto const *result{ std::get_if<fetch_result>(&results[req_idx]) };
      if (result) {
        try {
          auto const data{ util_load_file(dl.dest) };
          dl.content.assign(reinterpret_cast<char const *>(data.data()), data.size());
          dl.ok = true;
        } catch (std::exception const &e) {
          tui::warn("depot: failed to read manifest %s: %s", dl.url.c_str(), e.what());
        }
      } else {
        auto const *error{ std::get_if<std::string>(&results[req_idx]) };
        tui::warn("depot: failed to fetch manifest %s: %s",
                  dl.url.c_str(),
                  error ? error->c_str() : "unknown error");
      }
    }
  }

  // Parse all downloaded manifests into one merged index
  package_depot_index index;
  for (auto const &dl : downloads) {
    if (!dl.ok) { continue; }
    index.merge(build_from_text(dl.content, true));
  }

  return index;
}

package_depot_index package_depot_index::build_from_contents(
    std::vector<std::string> const &manifest_contents) {
  package_depot_index index;
  for (auto const &content : manifest_contents) {
    index.merge(build_from_text(content, true));
  }
  return index;
}

package_depot_index package_depot_index::build_from_text(std::string_view text,
                                                         bool require_sha256) {
  package_depot_index index;
  index.entries_ = parse_manifest_text(text, require_sha256);
  return index;
}

package_depot_index package_depot_index::build_from_entries(
    std::vector<depot_entry> const &entries) {
  package_depot_index index;

  for (auto const &entry : entries) {
    if (entry.url.empty()) {
      throw std::runtime_error("depot entry requires a non-empty 'url'");
    }

    std::optional<std::string> sha256_hash;
    if (entry.sha256) {
      if (!is_valid_sha256_hex(*entry.sha256)) {
        throw std::runtime_error("depot entry has malformed sha256 for url: " + entry.url);
      }
      sha256_hash = lowercase_hex(*entry.sha256);
    }

    auto stem{ stem_from_url(entry.url, entry.url) };
    if (!stem) { continue; }

    index.entries_.try_emplace(std::move(*stem),
                               depot_entry{ entry.url, std::move(sha256_hash) });
  }

  return index;
}

package_depot_index package_depot_index::build_from_directory(
    std::filesystem::path const &dir) {
  return build_from_directory(dir, {});
}

package_depot_index package_depot_index::build_from_directory(
    std::filesystem::path const &dir,
    std::unordered_map<std::string, std::string> const &checksums) {
  namespace fs = std::filesystem;

  package_depot_index index;

  for (auto const &e : fs::directory_iterator(dir)) {
    if (!e.is_regular_file()) { continue; }
    auto const &p{ e.path() };
    if (p.extension() != ".zst") { continue; }
    auto stem_path{ p.stem() };  // strips .zst
    if (stem_path.extension() != ".tar") { continue; }
    std::string const stem{ stem_path.stem().string() };

    if (!util_parse_archive_filename(stem)) {
      tui::warn("depot: skipping unrecognized file %s", p.filename().string().c_str());
      continue;
    }

    std::string const filename{ p.filename().string() };
    auto const it{ checksums.find(filename) };
    std::optional<std::string> sha256_hash;
    if (it != checksums.end()) { sha256_hash = it->second; }

    index.entries_.try_emplace(
        stem,
        depot_entry{ fs::absolute(p).string(), std::move(sha256_hash) });
  }

  return index;
}

void package_depot_index::merge(package_depot_index other) {
  for (auto &[stem, entry] : other.entries_) {
    auto const [it, inserted]{ entries_.try_emplace(stem, std::move(entry)) };
    if (!inserted && it->second.sha256 != entry.sha256) {
      tui::warn("depot: duplicate entry %s with differing SHA256; keeping first",
                stem.c_str());
    }
  }
}

std::optional<depot_entry> package_depot_index::find(std::string_view identity,
                                                     std::string_view platform,
                                                     std::string_view arch,
                                                     std::string_view hash_prefix) const {
  auto const it{ entries_.find(cache::key(identity, platform, arch, hash_prefix)) };
  return it != entries_.end() ? std::optional{ it->second } : std::nullopt;
}

bool package_depot_index::empty() const { return entries_.empty(); }

}  // namespace envy
