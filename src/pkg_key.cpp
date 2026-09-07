#include "pkg_key.h"
#include "pkg_cfg.h"

#include <stdexcept>
#include <utility>

namespace envy {

pkg_key::pkg_key(pkg_cfg const &cfg) : canonical_(cfg.format_key()) { parse_components(); }

pkg_key::pkg_key(std::string_view canonical_or_identity)
    : canonical_(canonical_or_identity) {
  parse_components();
}

pkg_key::pkg_key(pkg_key const &other) : canonical_(other.canonical_) {
  parse_components();
}

pkg_key::pkg_key(pkg_key &&other) noexcept : canonical_(std::move(other.canonical_)) {
  parse_components();
}

pkg_key &pkg_key::operator=(pkg_key const &other) {
  if (this != &other) {
    canonical_ = other.canonical_;
    parse_components();
  }
  return *this;
}

pkg_key &pkg_key::operator=(pkg_key &&other) noexcept {
  if (this != &other) {
    canonical_ = std::move(other.canonical_);
    parse_components();
  }
  return *this;
}

void pkg_key::parse_components() {
  size_t const options_start{ canonical_.find('{') };
  identity_ = (options_start != std::string::npos)
                  ? std::string_view(canonical_).substr(0, options_start)
                  : std::string_view(canonical_);

  size_t const dot{ identity_.find('.') };  // namespace: everything before first '.'
  if (dot == std::string_view::npos) {
    throw std::runtime_error("Invalid identity (missing namespace): " +
                             std::string(identity_));
  }

  // Parse name and revision: "name@revision"
  std::string_view const name_and_revision{ identity_.substr(dot + 1) };
  size_t at{ name_and_revision.find('@') };
  if (at == std::string_view::npos) {
    throw std::runtime_error("Invalid identity (missing revision): " +
                             std::string(identity_));
  }

  ns_ = identity_.substr(0, dot);
  name_ = name_and_revision.substr(0, at);
  revision_ = name_and_revision.substr(at);  // Includes '@'
  hash_ = std::hash<std::string>{}(canonical_);
}

bool pkg_key::matches(std::string_view query) const {
  if (query == canonical_) return true;
  if (query == identity_) return true;

  // Parse query to determine what we're matching against
  // Query forms:
  //   "name"               -> match any namespace/revision
  //   "namespace.name"     -> match any revision
  //   "name@revision"      -> match any namespace (unusual but valid)
  //   "namespace.name@rev" -> exact identity match (already checked above)

  size_t const query_at{ query.find('@') };
  bool const query_has_revision{ (query_at != std::string_view::npos) };

  // Only a dot before '@' is the namespace separator: a dotted revision ("gcc@13.2.0")
  // is not a namespace, and reading it as one made the query match nothing at all.
  size_t const query_dot{ query.substr(0, query_at).find('.') };
  bool const query_has_namespace{ (query_dot != std::string_view::npos) };

  if (!query_has_namespace && !query_has_revision) {
    return query == name_;  // Query is just "name"
  }

  if (query_has_namespace && !query_has_revision) {  // Query is "namespace.name"
    std::string_view const query_namespace{ query.substr(0, query_dot) };
    std::string_view const query_name{ query.substr(query_dot + 1) };
    return (query_namespace == ns_) && (query_name == name_);
  }

  if (!query_has_namespace && query_has_revision) {  // Query is "name@revision"
    std::string_view const query_name{ query.substr(0, query_at) };
    std::string_view const query_revision{ query.substr(query_at) };
    return (query_name == name_) && (query_revision == revision_);
  }

  return false;
}

}  // namespace envy
