#include "vendor.h"

#include "blake3_util.h"
#include "cache.h"
#include "glob.h"
#include "util.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace envy {

namespace {

namespace fs = std::filesystem;

constexpr char kVendorEntry[]{ "VENDOR entry" };

// How much of an identity a derived name spells out. Each level is a superset of the one
// above, so raising a colliding group separates it eventually.
enum class name_level : int { NAME = 0, NAMESPACED = 1, REVISIONED = 2, HASHED = 3 };
constexpr int kMaxLevel{ static_cast<int>(name_level::HASHED) };

std::string derive_leaf(pkg_key const &key, int level) {
  switch (static_cast<name_level>(level)) {
    case name_level::NAME: return std::string{ key.name() };
    case name_level::NAMESPACED:
      return std::string{ key.namespace_() } + "." + std::string{ key.name() };
    case name_level::REVISIONED: return std::string{ key.identity() };
    case name_level::HASHED: {
      // Whatever follows the identity in the canonical key. Hashing keeps the name a
      // legal path component whatever the author wrote.
      std::string_view const options{ std::string_view{ key.canonical() }.substr(
          key.identity().size()) };
      auto const digest{ blake3_hash(options.data(), options.size()) };
      return std::string{ key.identity() } + "-" + util_bytes_to_hex(digest.data(), 8);
    }
  }
  return std::string{ key.identity() };
}

void reject_bad_path(std::string_view value, std::string const &what) {
  if (auto const problem{ validate_project_relative_path(value) }) {
    throw std::runtime_error(what + " must be a path inside the project: " + *problem +
                             " (\"" + std::string{ value } + "\")");
  }
}

fs::path anchored(fs::path const &project_root, std::string_view relative) {
  return (project_root / fs::path{ relative }).lexically_normal();
}

// Componentwise, so "third_party/lib" is not a parent of "third_party/lib-extra".
bool contains_path(fs::path const &outer, fs::path const &inner) {
  auto const rel{ inner.lexically_relative(outer) };
  return !rel.empty() && *rel.begin() != "..";
}

}  // namespace

vendor_plan vendor_resolve(std::vector<vendor_request> const &requests,
                           std::optional<std::string> const &vendor_root,
                           fs::path const &project_root) {
  if (requests.empty()) { return {}; }

  // Canonical order: two runs of one manifest escalate the same names and name the same
  // pair in a collision message.
  auto sorted{ requests };
  std::ranges::sort(sorted, {}, [](vendor_request const &r) -> std::string const & {
    return r.key.canonical();
  });

  std::optional<fs::path> root_dir;
  if (vendor_root) {
    reject_bad_path(*vendor_root, "VENDOR_ROOT");
    root_dir = anchored(project_root, *vendor_root);
  }

  std::vector<int> level(sorted.size(), 0);
  std::vector<fs::path> dest(sorted.size());

  for (size_t i{ 0 }; i < sorted.size(); ++i) {
    auto const &req{ sorted[i] };
    if (!req.path_override) {
      if (!root_dir) {
        throw std::runtime_error(
            "package '" + std::string{ req.key.identity() } +
            "' asks to be vendored, but the manifest sets no VENDOR_ROOT; add "
            "VENDOR_ROOT = \"<dir>\" or give this entry an explicit vendor = \"<dir>\"");
      }
      continue;
    }
    reject_bad_path(*req.path_override,
                    "vendor path for '" + std::string{ req.key.identity() } + "'");
    dest[i] = anchored(project_root, *req.path_override);
    level[i] = -1;  // fixed; only derived names escalate
  }

  // To a fixpoint: raising one group can collide it with another.
  for (bool bumped{ true }; bumped;) {
    bumped = false;
    for (size_t i{ 0 }; i < sorted.size(); ++i) {
      if (level[i] >= 0) { dest[i] = *root_dir / derive_leaf(sorted[i].key, level[i]); }
    }

    std::unordered_map<std::string, std::vector<size_t>> groups;
    for (size_t i{ 0 }; i < sorted.size(); ++i) {
      groups[dest[i].generic_string()].push_back(i);
    }
    for (auto const &[_, members] : groups) {
      if (members.size() < 2) { continue; }
      for (size_t const i : members) {
        if (level[i] >= 0 && level[i] < kMaxLevel) {
          ++level[i];
          bumped = true;
        }
      }
    }
  }

  // What survived escalation must be unique and non-nested. Two colliding overrides land
  // here, since overrides never escalate.
  for (size_t i{ 0 }; i < sorted.size(); ++i) {
    for (size_t j{ i + 1 }; j < sorted.size(); ++j) {
      auto const &a{ sorted[i].key };
      auto const &b{ sorted[j].key };
      if (dest[i] == dest[j]) {
        throw std::runtime_error(
            "vendor destination collision: '" + std::string{ a.identity() } + "' and '" +
            std::string{ b.identity() } + "' both vendor to " + dest[i].string());
      }
      // Vendoring wipes before copying, so an outer package would delete an inner one.
      if (contains_path(dest[i], dest[j]) || contains_path(dest[j], dest[i])) {
        throw std::runtime_error(
            "nested vendor destinations: '" + std::string{ a.identity() } +
            "' vendors to " + dest[i].string() + " and '" + std::string{ b.identity() } +
            "' vendors to " + dest[j].string() + "; one would be erased by the other");
      }
    }
  }

  vendor_plan plan;
  plan.project_root = project_root;
  for (size_t i{ 0 }; i < sorted.size(); ++i) {
    plan.dirs.emplace(
        sorted[i].key,
        vendor_destination{ .dir = dest[i],
                            .overridden = sorted[i].path_override.has_value() });
  }
  return plan;
}

void vendor_validate_destination(fs::path const &dest,
                                 fs::path const &project_root,
                                 std::string_view identity) {
  // lexically_normal cannot see a symlinked component, and the next step is remove_all.
  std::error_code ec;
  auto const real_dest{ fs::weakly_canonical(dest, ec) };
  if (ec) {
    throw std::runtime_error("vendor: cannot resolve " + dest.string() + ": " +
                             ec.message());
  }
  auto const real_root{ fs::weakly_canonical(project_root, ec) };
  if (ec) {
    throw std::runtime_error("vendor: cannot resolve project root " +
                             project_root.string() + ": " + ec.message());
  }
  if (!contains_path(real_root, real_dest)) {
    throw std::runtime_error("vendor: '" + std::string{ identity } + "' resolves to " +
                             real_dest.string() + ", outside the project at " +
                             real_root.string() +
                             "; a symlinked path component would "
                             "put a wipe-and-recopy somewhere it does not belong");
  }
}

tree_filter vendor_parse_selectors(std::vector<std::string> const &raw,
                                   std::string_view context) {
  return glob_parse_filter(raw, context, kVendorEntry);
}

std::string vendor_filter_key(tree_filter const &filter) {
  std::string key;
  for (auto const &p : filter.include) { key += p + "\n"; }
  key += "!\n";  // the lists are already canonical, so a separator is all they need
  for (auto const &p : filter.exclude) { key += p + "\n"; }
  return key;
}

std::string vendor_pristine_hash(fs::path const &pkg_path, tree_filter const &filter) {
  auto const key{ vendor_filter_key(filter) };
  auto const key_digest{ blake3_hash(key.data(), key.size()) };
  fs::path const stamp{ pkg_path.parent_path() /
                        ("envy-vendor-" + util_bytes_to_hex(key_digest.data(), 8)) };

  // A completed entry is immutable, so a digest found here was computed over these exact
  // bytes and needs no revalidation.
  std::error_code ec;
  if (fs::is_regular_file(stamp, ec) && !ec) {
    try {
      auto const bytes{ util_load_file(stamp) };
      std::string digest{ bytes.begin(), bytes.end() };
      if (auto const nl{ digest.find('\n') }; nl != std::string::npos) {
        digest.resize(nl);
      }
      if (digest.size() == 64) { return digest; }
    } catch (std::exception const &) {
      // Unreadable or truncated: recompute rather than trust it.
    }
  }

  auto const result{ tree_hash(pkg_path, filter) };
  auto const digest{ util_bytes_to_hex(result.digest.data(), result.digest.size()) };
  try {
    util_write_file(stamp, digest + "\n");
  } catch (std::exception const &) {
    // Backfilling is an optimization; losing it costs the next run one more walk.
  }
  return digest;
}

}  // namespace envy
