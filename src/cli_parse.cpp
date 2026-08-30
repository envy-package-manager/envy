#include "cli_parse.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <system_error>

namespace envy {

namespace {

constexpr std::size_t kDescCol{ 30 };

// "-q,--quiet" -> "--quiet"; "--manifest" -> "--manifest"; "archive" -> "archive". The
// long spelling is what every diagnostic names, matching what a user typed or read.
std::string primary_name(char const *names) {
  std::string_view const all{ names };
  std::string_view first{ all.substr(0, all.find(',')) };
  for (std::string_view rest{ all }; !rest.empty();) {
    auto const comma{ rest.find(',') };
    auto const tok{ rest.substr(0, comma) };
    if (tok.starts_with("--")) { return std::string{ tok }; }
    rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);
  }
  return std::string{ first };
}

bool has_long(char const *names, std::string_view want) {
  for (std::string_view rest{ names }; !rest.empty();) {
    auto const comma{ rest.find(',') };
    auto const tok{ rest.substr(0, comma) };
    if (tok.starts_with("--") && tok.substr(2) == want) { return true; }
    rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);
  }
  return false;
}

bool has_short(char const *names, char want) {
  for (std::string_view rest{ names }; !rest.empty();) {
    auto const comma{ rest.find(',') };
    auto const tok{ rest.substr(0, comma) };
    if (tok.size() == 2 && tok[0] == '-' && tok[1] == want) { return true; }
    rest = (comma == std::string_view::npos) ? std::string_view{} : rest.substr(comma + 1);
  }
  return false;
}

bool is_vector(cli_dest k) { return k == cli_dest::VEC_STRING || k == cli_dest::VEC_PATH; }

bool takes_value(cli_dest k) { return k != cli_dest::FLAG && k != cli_dest::HELP; }

// A leading '-' introduces an option unless it is a lone dash, the "--" terminator, or a
// negative number. A leading '/' never does, so POSIX paths stay positional on Windows.
bool is_option_token(std::string_view a) {
  return a.size() >= 2 && a[0] == '-' && a != "--" && !(a[1] >= '0' && a[1] <= '9');
}

bool member_of(char const *csv, std::string_view want) {
  for (std::string_view rest{ csv }; !rest.empty();) {
    auto const comma{ rest.find(',') };
    if (rest.substr(0, comma) == want) { return true; }
    if (comma == std::string_view::npos) { break; }
    rest = rest.substr(comma + 1);
  }
  return false;
}

bool parse_bool(std::string_view v, bool &out) {
  std::string s{ v };
  for (auto &c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  if (s == "1" || s == "t" || s == "y" || s == "+" || s == "true" || s == "on" ||
      s == "yes" || s == "enable") {
    return out = true;
  }
  if (s == "0" || s == "f" || s == "n" || s == "-" || s == "false" || s == "off" ||
      s == "no" || s == "disable") {
    out = false;
    return true;
  }
  return false;
}

bool parse_int(std::string_view v, int &out) {
  std::string const s{ v };
  char *end{};
  long const n{ std::strtol(s.c_str(), &end, 10) };
  if (s.empty() || *end != '\0') { return false; }
  out = static_cast<int>(n);
  return true;
}

std::string validate(cli_option const &o, std::string_view v) {
  if (o.check == cli_check::ONE_OF) {
    return member_of(o.allowed, v) ? std::string{}
                                   : primary_name(o.names) + ": " + std::string{ v } +
                                         " not in {" + o.allowed + "}";
  }
  if (o.check == cli_check::NONE) { return {}; }

  std::error_code ec;
  std::filesystem::path const p{ v };
  bool const is_dir{ std::filesystem::is_directory(p, ec) };
  bool const exists{ std::filesystem::exists(p, ec) };
  std::string const who{ primary_name(o.names) + ": " };
  if (o.check == cli_check::EXISTING_FILE) {
    if (!exists) { return who + "File does not exist: " + std::string{ v }; }
    if (is_dir) { return who + "File is actually a directory: " + std::string{ v }; }
    return {};
  }
  if (!exists) { return who + "Directory does not exist: " + std::string{ v }; }
  if (!is_dir) { return who + "Directory is actually a file: " + std::string{ v }; }
  return {};
}

// The whole point: every string -> value conversion in the program lives here.
std::string store(cli_option &o, std::string_view v) {
  if (auto err{ validate(o, v) }; !err.empty()) { return err; }
  auto const bad{ [&] {
    return "Could not convert: " + primary_name(o.names) + " = " + std::string{ v };
  } };
  switch (o.kind) {
    case cli_dest::HELP: break;
    case cli_dest::FLAG: {
      bool b{};
      if (!parse_bool(v, b)) { return bad(); }
      *static_cast<bool *>(o.dest) = b;
      break;
    }
    case cli_dest::INT: {
      int n{};
      if (!parse_int(v, n)) { return bad(); }
      *static_cast<int *>(o.dest) = n;
      break;
    }
    case cli_dest::STRING: *static_cast<std::string *>(o.dest) = v; break;
    case cli_dest::PATH: *static_cast<std::filesystem::path *>(o.dest) = v; break;
    case cli_dest::OPT_BOOL: {
      bool b{};
      if (!parse_bool(v, b)) { return bad(); }
      *static_cast<std::optional<bool> *>(o.dest) = b;
      break;
    }
    case cli_dest::OPT_STRING:
      *static_cast<std::optional<std::string> *>(o.dest) = std::string{ v };
      break;
    case cli_dest::OPT_PATH:
      *static_cast<std::optional<std::filesystem::path> *>(o.dest) =
          std::filesystem::path{ v };
      break;
    case cli_dest::VEC_STRING:
      static_cast<std::vector<std::string> *>(o.dest)->emplace_back(v);
      break;
    case cli_dest::VEC_PATH:
      static_cast<std::vector<std::filesystem::path> *>(o.dest)->emplace_back(v);
      break;
  }
  return {};
}

char const *placeholder(cli_dest k) {
  switch (k) {
    case cli_dest::INT: return " INT";
    case cli_dest::OPT_BOOL: return " BOOLEAN";
    case cli_dest::VEC_STRING:
    case cli_dest::VEC_PATH: return " TEXT ...";
    case cli_dest::HELP:
    case cli_dest::FLAG: return "";
    default: return " TEXT";
  }
}

void add_row(std::string &out, std::string const &label, char const *desc) {
  out += "  ";
  out += label;
  out.append(label.size() + 2 < kDescCol ? kDescCol - 2 - label.size() : 1, ' ');
  out += desc;
  out += "\n";
}

}  // namespace

cli_option &cli_opt::option() const {
  return cmd_->options()[static_cast<std::size_t>(idx_)];
}

cli_opt cli_opt::required() const { return option().required = true, *this; }
cli_opt cli_opt::envname(char const *var) const { return option().env = var, *this; }
cli_opt cli_opt::take_last() const { return option().take_last = true, *this; }
cli_opt cli_opt::optional_value() const { return option().value_optional = true, *this; }
cli_opt cli_opt::one_of(char const *csv) const {
  return option().allowed = csv, option().check = cli_check::ONE_OF, *this;
}
cli_opt cli_opt::check_file() const {
  return option().check = cli_check::EXISTING_FILE, *this;
}
cli_opt cli_opt::check_dir() const {
  return option().check = cli_check::EXISTING_DIR, *this;
}
cli_opt cli_opt::excludes(cli_opt other) const {
  option().excludes |= uint64_t{ 1 } << other.index();
  return *this;
}
uint32_t cli_opt::count() const { return option().count; }

cli_cmd::cli_cmd(cli_cmd *parent, char const *name, char const *desc)
    : parent_{ parent }, name_{ name }, desc_{ desc } {
  add("-h,--help", nullptr, cli_dest::HELP, "Print this help message and exit");
}

cli_opt cli_cmd::add(char const *names,
                     void *dest,
                     cli_dest kind,
                     char const *desc,
                     bool positional) {
  cli_option o{};
  o.names = names;
  o.desc = desc;
  o.dest = dest;
  o.kind = kind;
  o.positional = positional;
  options_.push_back(o);
  return cli_opt{ this, static_cast<int>(options_.size()) - 1 };
}

cli_cmd &cli_cmd::sub(char const *name, char const *desc) {
  subs_.push_back(std::make_unique<cli_cmd>(this, name, desc));
  return *subs_.back();
}

cli_cmd *cli_cmd::find_sub(std::string_view name) const {
  for (auto const &s : subs_) {
    if (name == s->name_) { return s.get(); }
  }
  return nullptr;
}

cli_option *cli_cmd::find_long(std::string_view name) {
  for (auto &o : options_) {
    if (!o.positional && has_long(o.names, name)) { return &o; }
  }
  return nullptr;
}

cli_option *cli_cmd::find_short(char c) {
  for (auto &o : options_) {
    if (!o.positional && has_short(o.names, c)) { return &o; }
  }
  return nullptr;
}

// Slots fill in declaration order; a list slot never fills, so it takes the tail.
cli_option *cli_cmd::next_positional() {
  for (auto &o : options_) {
    if (o.positional && (is_vector(o.kind) || o.count == 0)) { return &o; }
  }
  return nullptr;
}

bool cli_cmd::has_empty_positional() const {
  for (auto const &o : options_) {
    if (o.positional && o.count == 0) { return true; }
  }
  return false;
}

bool cli_cmd::owns_option(std::string_view arg) {
  if (!is_option_token(arg)) { return false; }
  if (arg.starts_with("--")) {
    auto const eq{ arg.find('=') };
    return find_long(arg.substr(2, eq == std::string_view::npos ? eq : eq - 2)) != nullptr;
  }
  return find_short(arg[1]) != nullptr;
}

std::string cli_cmd::full_name() const {
  return parent_ ? parent_->full_name() + " " + name_ : std::string{ name_ };
}

std::string cli_cmd::help() const {
  std::string out{ desc_ };
  out += "\n\nUSAGE:\n  ";
  out += full_name();
  if (options_.size() > 1) { out += " [OPTIONS]"; }
  for (auto const &o : options_) {
    if (!o.positional) { continue; }
    out += o.required ? " " : " [";
    out += o.names;
    if (is_vector(o.kind)) { out += "..."; }
    if (!o.required) { out += "]"; }
  }
  if (!subs_.empty()) { out += " [SUBCOMMANDS]"; }
  out += "\n";

  bool positionals{ false };
  for (auto const &o : options_) { positionals = positionals || o.positional; }
  if (positionals) {
    out += "\nPOSITIONALS:\n";
    for (auto const &o : options_) {
      if (!o.positional) { continue; }
      std::string label{ o.names };
      label += placeholder(o.kind);
      if (o.required) { label += " REQUIRED"; }
      add_row(out, label, o.desc);
    }
  }

  out += "\nOPTIONS:\n";
  for (auto const &o : options_) {
    if (o.positional) { continue; }
    std::string_view const names{ o.names };
    auto const comma{ names.find(',') };
    // Short and long spellings share a column, so a long-only option indents past it.
    std::string label{ comma != std::string_view::npos
                           ? std::string{ names.substr(0, comma) } + ", " +
                                 std::string{ names.substr(comma + 1) }
                       : names.starts_with("--") ? std::string{ "    " } + o.names
                                                 : std::string{ o.names } };
    label += placeholder(o.kind);
    add_row(out, label, o.desc);
  }

  if (!subs_.empty()) {
    out += "\nSUBCOMMANDS:\n";
    for (auto const &s : subs_) { add_row(out, s->name_, s->desc_); }
  }
  return out;
}

cli_parse_result cli_run(cli_cmd &root, int argc, char **argv) {
  cli_parse_result r;
  cli_cmd *cur{ &root };

  auto const fail{ [&](std::string msg) {
    r.error = std::move(msg);
    r.help_text = cur->help();
    return r;
  } };
  auto const unexpected{ [](std::string_view a) {
    return "The following argument was not expected: " + std::string{ a };
  } };
  // A second value for a single-value option would silently discard one of them.
  auto const bump{ [](cli_option &o) {
    if (o.count && !o.take_last && o.kind != cli_dest::FLAG && !is_vector(o.kind)) {
      return primary_name(o.names) + ": At most 1 required but received " +
             std::to_string(o.count + 1);
    }
    ++o.count;
    return std::string{};
  } };

  bool positional_only{ false };
  int i{ 1 };
  while (i < argc) {
    std::string_view const a{ argv[i] };

    if (cur->prefix_dest() && !cur->owns_option(a)) {
      for (; i < argc; ++i) { cur->prefix_dest()->emplace_back(argv[i]); }
      break;
    }

    if (!positional_only && a == "--") {
      // With every slot already fed there is nothing for the terminator to introduce.
      if (!cur->has_empty_positional()) { return fail(unexpected(a)); }
      positional_only = true;
      ++i;
      continue;
    }

    if (!positional_only && is_option_token(a)) {
      if (a.starts_with("--")) {
        auto const eq{ a.find('=') };
        bool const has_eq{ eq != std::string_view::npos };
        std::string_view const inline_val{ has_eq ? a.substr(eq + 1)
                                                  : std::string_view{} };
        cli_option *o{ cur->find_long(a.substr(2, has_eq ? eq - 2 : eq)) };
        if (!o) { return fail(unexpected(a)); }
        if (o->kind == cli_dest::HELP) {
          r.help_requested = true;
          r.help_text = cur->help();
          return r;
        }
        ++i;
        std::string_view val;
        bool have_val{ true };
        if (!takes_value(o->kind)) {
          val = inline_val.empty() ? std::string_view{ "true" } : inline_val;
        } else if (!inline_val.empty()) {
          val = inline_val;
        } else if (o->value_optional) {
          // An empty "=" value, an option, "--" and a subcommand name are all not values.
          have_val = !has_eq && i < argc && std::string_view{ argv[i] } != "--" &&
                     !is_option_token(argv[i]) && !cur->find_sub(argv[i]);
          if (have_val) { val = argv[i++]; }
        } else if (has_eq || i >= argc) {
          return fail(primary_name(o->names) + ": 1 required TEXT missing");
        } else {
          val = argv[i++];
        }
        if (auto e{ bump(*o) }; !e.empty()) { return fail(e); }
        if (have_val) {
          if (auto e{ store(*o, val) }; !e.empty()) { return fail(e); }
        }
        continue;
      }

      std::string_view const cluster{ a.substr(1) };
      for (std::size_t j{ 0 }; j < cluster.size(); ++j) {
        cli_option *o{ cur->find_short(cluster[j]) };
        if (!o) { return fail(unexpected(a)); }
        if (o->kind == cli_dest::HELP) {
          r.help_requested = true;
          r.help_text = cur->help();
          return r;
        }
        std::string_view val{ "true" };
        if (takes_value(o->kind)) {
          val = cluster.substr(j + 1);  // the rest of the cluster is the value
          if (val.empty()) {
            if (i + 1 >= argc) {
              return fail(primary_name(o->names) + ": 1 required TEXT missing");
            }
            val = argv[++i];
          }
          j = cluster.size();
        }
        if (auto e{ bump(*o) }; !e.empty()) { return fail(e); }
        if (auto e{ store(*o, val) }; !e.empty()) { return fail(e); }
      }
      ++i;
      continue;
    }

    if (!positional_only && !cur->subs().empty()) {
      cli_cmd *child{ cur->find_sub(a) };
      if (!child) { return fail(unexpected(a)); }
      cur = child;
      ++i;
      continue;
    }

    cli_option *p{ cur->next_positional() };
    if (!p) { return fail(unexpected(a)); }
    ++p->count;
    if (auto e{ store(*p, a) }; !e.empty()) { return fail(e); }
    ++i;
  }

  // Everything below walks the chain the argv actually reached: root first, so a global
  // option's environment default and exclusions are checked exactly once.
  std::vector<cli_cmd *> chain;
  for (cli_cmd *c{ cur }; c; c = c->parent()) { chain.push_back(c); }

  for (auto *c : chain) {
    for (auto &o : c->options()) {
      if (!o.env || o.count) { continue; }
      char const *v{ std::getenv(o.env) };
      if (!v || !*v) { continue; }
      ++o.count;
      if (auto e{ store(o, v) }; !e.empty()) { return fail(e); }
    }
  }

  for (auto *c : chain) {
    auto const &opts{ c->options() };
    for (std::size_t x{ 0 }; x < opts.size(); ++x) {
      if (!opts[x].count || !opts[x].excludes) { continue; }
      for (std::size_t y{ 0 }; y < opts.size(); ++y) {
        if ((opts[x].excludes >> y & 1) && opts[y].count) {
          return fail(primary_name(opts[x].names) + " excludes " +
                      primary_name(opts[y].names));
        }
      }
    }
    for (auto const &o : c->options()) {
      if (o.required && !o.count) { return fail(primary_name(o.names) + " is required"); }
    }
  }

  for (auto *c : chain) {
    if (!c->finalize_fn()) { continue; }
    if (char const *e{ c->finalize_fn()(c->finalize_ctx()) }) { return fail(e); }
  }

  // Only the outcomes that print pay for the help text.
  r.selected_id = cur->id();
  if (r.selected_id < 0) { r.help_text = cur->help(); }
  return r;
}

}  // namespace envy
