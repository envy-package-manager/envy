#include "product_expand.h"

#include "engine.h"
#include "env_ops.h"
#include "manifest.h"
#include "pkg.h"
#include "pkg_env.h"
#include "product_util.h"
#include "trace.h"
#include "tui_actions.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <variant>

namespace envy {

namespace {

// Builds a refusal error, tracing it when `traced`.
struct product_access {
  pkg *consumer;
  std::string const &name;
  std::string_view what;
  pkg_phase phase;
  bool traced;
  product_grant grant{ nullptr };

  std::runtime_error refuse(std::string const &why) const {
    std::string msg{ std::string{ what } + ": " + why };
    if (traced) { product_trace(consumer, name, phase, grant, false, msg); }
    return std::runtime_error{ std::move(msg) };
  }
};

std::vector<product_placeholder> placeholders(std::string_view text,
                                              std::string_view what) {
  try {
    return product_util_placeholders(text);
  } catch (std::exception const &e) {
    throw_runtime_error({ what, ": ", e.what() });
  }
}

// The target shell's rules; UNKNOWN while a DEFAULT_SHELL function hasn't run.
enum class dialect : std::uint8_t { POSIX, POWERSHELL, CMD, CUSTOM, UNKNOWN };

dialect dialect_of(resolved_shell const &shell) {
  return envy::visit(match{ [](shell_choice c) {
                             return c == shell_choice::powershell ? dialect::POWERSHELL
                                    : c == shell_choice::cmd      ? dialect::CMD
                                                                  : dialect::POSIX;
                           },
                            [](custom_shell_file const &) { return dialect::CUSTOM; },
                            [](custom_shell_inline const &) { return dialect::CUSTOM; } },
                     shell);
}

// Custom shells use the host's strictest path rules: POSIX, or cmd on Windows.
dialect path_rules(dialect d) {
  if (d != dialect::CUSTOM) { return d; }
  bool const windows{ std::filesystem::path::preferred_separator == '\\' };
  return windows ? dialect::CMD : dialect::POSIX;
}

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Whether s[i] starts a UTF-8 char PowerShell treats as a quote, dash or space.
bool pwsh_syntax(std::string_view s, std::size_t i) {
  auto const b{ [&](std::size_t k) -> unsigned {
    return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0u;
  } };
  if (b(0) == 0xC2) { return b(1) == 0x85 || b(1) == 0xA0; }  // NEL, no-break space
  if (b(0) == 0xE3) { return b(1) == 0x80 && b(2) == 0x80; }  // ideographic space
  if (b(0) != 0xE2 || b(1) != 0x80) { return false; }
  unsigned const c{ b(2) };  // U+20xx: spaces, dashes, curly quotes, line separators
  return c <= 0x8A || (c >= 0x93 && c <= 0x95) || (c >= 0x98 && c <= 0x9E) || c == 0xA8 ||
         c == 0xA9;
}

// Whether a path is safe unquoted anywhere: bare, in quotes, in a heredoc.
bool unquotable(std::string_view text, dialect d) {
  dialect const rules{ path_rules(d) };
  bool const posix{ rules == dialect::POSIX };
  for (std::size_t i{ 0 }; i < text.size(); ++i) {
    char const c{ text[i] };
    if (static_cast<unsigned char>(c) >= 0x80) {  // cmd decodes scripts in the OEM page
      if (rules == dialect::CMD ||
          (rules == dialect::POWERSHELL && pwsh_syntax(text, i))) {
        return false;
      }
      continue;
    }
    bool const safe{ util_ascii_is_alnum(c) ||
                     std::string_view{ "/:._+@-" }.find(c) != std::string_view::npos ||
                     (c == '~' && !(posix && i > 0 && text[i - 1] == ':')) ||
                     (c == '\\' && !posix) };  // POSIX `X=a:~b` tilde-expands
    if (!safe) { return false; }
  }
  return true;
}

std::string_view allowed_characters(dialect d) {
  dialect const rules{ path_rules(d) };
  if (rules == dialect::POSIX) { return "letters, digits and / : . _ + @ ~ -"; }
  return rules == dialect::POWERSHELL ? "letters, digits and \\ / : . _ + @ ~ -"
                                      : "ASCII letters, digits and \\ / : . _ + @ ~ -";
}

// Strips trailing separators, so cmd's `"{{dir}}"` can't end in `\"`.
std::string without_trailing_separator(std::string s) {
  while (s.size() > 1 && (s.back() == '/' || s.back() == '\\') && s[s.size() - 2] != ':') {
    s.pop_back();
  }
  return s;
}

// The last non-blank character before `at` on its line and its index; 0 at the start.
std::pair<char, std::size_t> previous_on_line(std::string_view text, std::size_t at) {
  while (at > 0) {
    char const c{ text[--at] };
    if (c == '\n' || c == '\r') { return { 0, at }; }
    if (c != ' ' && c != '\t') { return { c, at }; }
  }
  return { 0, 0 };
}

// The word of letters just before `at`, past blanks, lowercased.
std::string word_before(std::string_view text, std::size_t at) {
  while (at > 0 && (text[at - 1] == ' ' || text[at - 1] == '\t')) { --at; }
  std::size_t const end{ at };
  while (at > 0 && util_ascii_is_alpha(text[at - 1])) { --at; }
  std::string w{ text.substr(at, end - at) };
  std::ranges::transform(w, w.begin(), ascii_lower);
  return w;
}

// Whether `at` begins a cmd command, where a batch file needs `call`: line start, or after
// `&`, `|`, a block's `(`, `do`, `else`; skips a leading `@` or `"`.
bool cmd_command_start(std::string_view text, std::size_t at) {
  if (at > 0 && text[at - 1] == '"') { --at; }
  if (at > 0 && text[at - 1] == '@') { --at; }
  auto const [c, i]{ previous_on_line(text, at) };
  if (c == 0 || c == '&' || c == '|') { return true; }
  if (c == '(') {  // a block, unless `in (` or `echo(`
    std::string const w{ word_before(text, i) };
    return w != "in" && w != "echo";
  }
  std::string const w{ word_before(text, at) };
  return w == "do" || w == "else";
}

bool is_batch(std::string_view path) {
  if (path.size() < 4) { return false; }
  std::string ext{ path.substr(path.size() - 4) };
  std::ranges::transform(ext, ext.begin(), ascii_lower);
  return ext == ".bat" || ext == ".cmd";
}

// A command's argv must fill whole words: whitespace or shell punctuation on both sides.
void require_whole_word(std::string_view text,
                        product_placeholder const &h,
                        dialect d,
                        std::string_view what) {
  std::string_view const delims{ d == dialect::POSIX        ? ";&|()<>`"
                                 : d == dialect::POWERSHELL ? ";|{}(),"
                                                            : "&|()<>" };
  auto const bounds{ [&](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           delims.find(c) != std::string_view::npos;
  } };
  if ((h.begin == 0 || bounds(text[h.begin - 1])) &&
      (h.end == text.size() || bounds(text[h.end]))) {
    return;
  }
  throw_runtime_error({ what,
                        ": '{{",
                        h.name,
                        "}}' is a command product: its argv fills whole words, never part "
                        "of one or a quoted string" });
}

// POSIX: whether `at` is in quotes or a heredoc body; `$( )` and backticks are code again.
bool posix_quoted_at(std::string_view t, std::size_t at) {
  enum class frame : std::uint8_t { CODE, DOUBLE, SUBST, BACKTICK };
  struct heredoc {
    std::string word;
    bool tabs;  // `<<-` strips leading tabs
  };
  constexpr auto npos{ std::string_view::npos };
  std::vector<std::pair<frame, int>> stack{ { frame::CODE, 0 } };  // frame, paren depth
  std::vector<heredoc> pending;
  auto const word_start{ [&](std::size_t i) {
    return i == 0 || std::string_view{ " \t\n;&|()" }.find(t[i - 1]) != npos;
  } };
  for (std::size_t i{ 0 }; i < at;) {
    auto &[top, depth]{ stack.back() };
    char const c{ t[i] };
    if (top == frame::DOUBLE) {
      if (c == '\\') {
        i += 2;
      } else if (c == '"') {
        stack.pop_back();
        ++i;
      } else if (t.substr(i, 2) == "$(") {
        stack.emplace_back(frame::SUBST, 0);
        i += 2;
      } else {
        if (c == '`') { stack.emplace_back(frame::BACKTICK, 0); }
        ++i;
      }
      continue;
    }
    if (c == '\\') {
      i += 2;
    } else if (c == '\'') {
      std::size_t const close{ t.find('\'', i + 1) };
      if (close == npos || close >= at) { return true; }
      i = close + 1;
    } else if (c == '"') {
      stack.emplace_back(frame::DOUBLE, 0);
      ++i;
    } else if (c == '`') {
      if (top == frame::BACKTICK) {
        stack.pop_back();
      } else {
        stack.emplace_back(frame::BACKTICK, 0);
      }
      ++i;
    } else if (t.substr(i, 2) == "$(") {
      stack.emplace_back(frame::SUBST, 0);
      i += 2;
    } else if (top == frame::SUBST && (c == '(' || c == ')')) {
      if (c == '(') {
        ++depth;
      } else if (depth-- == 0) {
        stack.pop_back();
      }
      ++i;
    } else if (c == '#' && word_start(i)) {  // a comment, to the end of its line
      std::size_t const eol{ t.find('\n', i) };
      if (eol == npos || eol >= at) { return false; }
      i = eol;
    } else if (t.substr(i, 2) == "<<" && t.substr(i, 3) != "<<<" && depth == 0) {
      i += 2;
      bool const tabs{ i < t.size() && t[i] == '-' };
      i += tabs;
      while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) { ++i; }
      std::string word;
      std::string_view const ends{ " \t\r\n;&|<>()" };
      for (; i < t.size() && ends.find(t[i]) == npos; ++i) {
        if (t[i] != '\'' && t[i] != '"' && t[i] != '\\') { word += t[i]; }
      }
      pending.push_back({ std::move(word), tabs });
    } else if (c == '\n' && !pending.empty()) {  // heredoc bodies start on the next line
      std::size_t pos{ i + 1 };
      for (auto const &h : pending) {
        std::size_t const start{ pos };
        std::size_t end{ t.size() };
        while (pos < t.size()) {
          std::size_t const eol{ std::min(t.find('\n', pos), t.size()) };
          std::string_view line{ t.substr(pos, eol - pos) };
          if (h.tabs) {
            line.remove_prefix(std::min(line.find_first_not_of('\t'), line.size()));
          }
          if (line.ends_with('\r')) { line.remove_suffix(1); }
          std::size_t const line_start{ pos };
          pos = std::min(eol + 1, t.size());
          if (line == h.word) {
            end = line_start;
            break;
          }
        }
        if (at >= start && at < end) { return true; }
      }
      pending.clear();
      i = pos;
    } else {
      ++i;
    }
  }
  return stack.back().first == frame::DOUBLE;
}

// PowerShell: whether `at` is in a string or here-string; `$( )` is code again.
bool pwsh_quoted_at(std::string_view t, std::size_t at) {
  enum class frame : std::uint8_t { CODE, DOUBLE, HERE, SUBEXPR };
  constexpr auto npos{ std::string_view::npos };
  std::vector<std::pair<frame, int>> stack{ { frame::CODE, 0 } };  // frame, paren depth
  auto const token_start{ [&](std::size_t i) {
    return i == 0 || std::string_view{ " \t\r\n;{}()|" }.find(t[i - 1]) != npos;
  } };
  auto const opens_here{ [&](std::size_t i) {  // `@'` or `@"` ending its line
    return t.substr(i + 2, 1) == "\n" || t.substr(i + 2, 2) == "\r\n";
  } };
  for (std::size_t i{ 0 }; i < at;) {
    auto &[top, depth]{ stack.back() };
    char const c{ t[i] };
    if (top == frame::DOUBLE || top == frame::HERE) {
      if (c == '`') {
        i += 2;
      } else if (top == frame::DOUBLE && c == '"') {
        if (t.substr(i, 2) == "\"\"") {
          i += 2;
        } else {
          stack.pop_back();
          ++i;
        }
      } else if (top == frame::HERE && t.substr(i, 2) == "\"@" && i > 0 &&
                 t[i - 1] == '\n') {
        stack.pop_back();
        i += 2;
      } else if (t.substr(i, 2) == "$(") {
        stack.emplace_back(frame::SUBEXPR, 0);
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (c == '`') {
      i += 2;
    } else if (t.substr(i, 2) == "@'" && opens_here(i)) {
      std::size_t const close{ t.find("\n'@", i) };
      if (close == npos || close >= at) { return true; }
      i = close + 3;
    } else if (t.substr(i, 2) == "@\"" && opens_here(i)) {
      stack.emplace_back(frame::HERE, 0);
      i += 2;
    } else if (c == '\'') {
      std::size_t close{ i + 1 };
      while (close < t.size() && (t[close] != '\'' || t.substr(close, 2) == "''")) {
        close += t[close] == '\'' ? 2 : 1;
      }
      if (close >= at) { return true; }
      i = close + 1;
    } else if (c == '"') {
      stack.emplace_back(frame::DOUBLE, 0);
      ++i;
    } else if (t.substr(i, 2) == "<#") {  // a block comment
      std::size_t const close{ t.find("#>", i + 2) };
      if (close == npos || close >= at) { return false; }
      i = close + 2;
    } else if (c == '#' && token_start(i)) {  // a comment, to the end of its line
      std::size_t const eol{ t.find('\n', i) };
      if (eol == npos || eol >= at) { return false; }
      i = eol;
    } else if (t.substr(i, 2) == "$(") {
      stack.emplace_back(frame::SUBEXPR, 0);
      i += 2;
    } else {
      if (top == frame::SUBEXPR && c == '(') { ++depth; }
      if (top == frame::SUBEXPR && c == ')' && depth-- == 0) { stack.pop_back(); }
      ++i;
    }
  }
  auto const top{ stack.back().first };
  return top == frame::DOUBLE || top == frame::HERE;
}

// cmd: whether `at` is inside `"..."` on its line; `rem` and `::` lines are comments.
bool cmd_quoted_at(std::string_view t, std::size_t at) {
  bool quoted{ false };
  for (std::size_t i{ 0 }; i < at;) {
    if (!quoted && (i == 0 || t[i - 1] == '\n')) {
      std::size_t const j{ std::min(t.find_first_not_of(" \t@", i), t.size()) };
      std::string w{ t.substr(j, 4) };
      std::ranges::transform(w, w.begin(), ascii_lower);
      if (w.starts_with("::") || w == "rem" || w == "rem " || w == "rem\t" ||
          w == "rem\r" || w == "rem\n") {
        std::size_t const eol{ t.find('\n', i) };
        if (eol == std::string_view::npos || eol >= at) { return false; }
        i = eol;
        continue;
      }
    }
    char const c{ t[i] };
    if (c == '\n') {
      quoted = false;
    } else if (!quoted && c == '^') {
      ++i;
    } else if (c == '"') {
      quoted = !quoted;
    }
    ++i;
  }
  return quoted;
}

// Whether `at` is in quoted text (quotes, a heredoc, a here-string) rather than code.
bool quoted_at(std::string_view text, std::size_t at, dialect d) {
  switch (d) {
    case dialect::POSIX: return posix_quoted_at(text, at);
    case dialect::POWERSHELL: return pwsh_quoted_at(text, at);
    case dialect::CMD: return cmd_quoted_at(text, at);
    case dialect::CUSTOM:
    case dialect::UNKNOWN: return false;
  }
  return false;
}

// Authorizes what an own table command splices, recursing into own commands; function
// commands are checked when they run.
void check_own_command(pkg *p,
                       std::string const &name,
                       pkg_phase phase,
                       std::unordered_set<std::string> &seen) {
  if (!seen.insert(name).second) { return; }  // cycles: validate_product_commands
  auto const argv{ [&]() -> std::optional<std::vector<std::string>> {
    std::lock_guard const deps_lock(p->deps_mutex);
    auto const it{ p->products.find(name) };
    auto const *const v{ it == p->products.end()
                             ? nullptr
                             : std::get_if<std::vector<std::string>>(&it->second.value) };
    return v ? std::optional{ *v } : std::nullopt;
  }() };
  if (!argv) { return; }
  std::string const splicer{ "'{{" + name + "}}'" };
  for (auto const &element : *argv) {
    for (auto const &h : product_util_placeholders(element)) {  // validated at load
      if (h.escape()) { continue; }
      if (product_authorize(p, phase, h.name, splicer, false).provider == p) {
        check_own_command(p, h.name, phase, seen);
      }
    }
  }
}

// argv for `provider`'s `name` in `consumer`'s phase; own commands use `held` and the
// phase's edges. A bare program resolves on `path()`, past its bin-dir wrapper, or stays.
std::vector<std::string> phase_argv(pkg *consumer,
                                    pkg *provider,
                                    std::string const &name,
                                    pkg_phase phase,
                                    std::filesystem::path const &state_root,
                                    lua_State *held,
                                    function_ref<std::string()> path,
                                    std::filesystem::path const &cwd) {
  auto const dirs{ [&](pkg *q) { return product_payload_dir(consumer, q); } };
  std::string const splicer{ "'{{" + name + "}}'" };
  auto const owner{ [&](pkg *of, std::string const &n) {  // own: authorized by the phase
    return of == consumer ? product_authorize(consumer, phase, n, splicer, false).provider
                          : product_util_owner(of, n);
  } };
  std::string program;
  auto argv{ product_util_argv(provider,
                               name,
                               { .pkg_path = dirs,
                                 .state_root = state_root,
                                 .owner = owner,
                                 .held_pkg = consumer,
                                 .held = held },
                               &program) };
  if (util_path_from_utf8(argv.front()).has_parent_path()) { return argv; }

  // Resolve as `--run` does: past aliases, builtins and the product's own wrapper.
  manifest const *const m{ consumer->eng ? consumer->eng->get_manifest() : nullptr };
  std::filesystem::path const bin{ [&]() -> std::filesystem::path {
    if (!m || !m->meta.bin) { return {}; }
    std::error_code ec;
    auto dir{ std::filesystem::canonical(consumer->eng->project_root() / *m->meta.bin,
                                         ec) };
    return ec ? std::filesystem::path{} : dir;
  }() };
  if (auto found{ product_util_find_program(
          argv.front(), program, path(), cwd, bin.empty() ? nullptr : &bin) }) {
    argv.front() = std::move(*found);
  }
  return argv;
}

// Renders one placeholder; with `render` false, only checks it.
std::string expand_one(std::string_view text,
                       product_placeholder const &h,
                       product_expand_cfg const &cfg,
                       dialect d,
                       bool render) {
  if (!cfg.p) {
    throw_runtime_error({ cfg.what,
                          ": '{{",
                          h.name,
                          "}}' names a product, but no package's phase runs this" });
  }
  std::string const hint{ "; for the braces themselves write '{{{{" + h.name + "}}}}'" };
  product_grant const grant{
    product_authorize(cfg.p, cfg.phase, h.name, cfg.what, render, hint)
  };
  pkg *const q{ grant.provider };
  try {
    if (product_util_is_command(q, h.name)) {
      if (d == dialect::CUSTOM) {
        throw_runtime_error({ cfg.what,
                              ": '{{",
                              h.name,
                              "}}' is a command product, and a custom shell has no quoting "
                              "envy knows; only path products expand there" });
      }
      if (d != dialect::UNKNOWN) { require_whole_word(text, h, d, cfg.what); }
      if (quoted_at(text, h.begin, d)) {
        throw_runtime_error({ cfg.what,
                              ": '{{",
                              h.name,
                              "}}' is a command product inside quoted text; its argv is "
                              "quoted for shell code, so place it outside quotes, heredocs "
                              "and here-strings" });
      }
      if (!render) {
        std::unordered_set<std::string> seen;
        try {
          if (q == cfg.p) { check_own_command(cfg.p, h.name, cfg.phase, seen); }
        } catch (std::exception const &e) {
          throw_runtime_error({ cfg.what, ": ", e.what() });
        }
        return {};
      }
      shell_choice const shell{ std::get<shell_choice>(cfg.shell) };  // not custom here
      std::string line{ [&] {
        try {
          return shell_quote_argv(phase_argv(
                                      cfg.p,
                                      q,
                                      h.name,
                                      cfg.phase,
                                      cfg.state_root,
                                      cfg.held,
                                      [&] {
                                        std::string const *const path{ env_get(cfg.env,
                                                                               "PATH") };
                                        return path ? *path : std::string{};
                                      },
                                      cfg.cwd),
                                  shell);
        } catch (std::exception const &e) {
          throw_runtime_error({ cfg.what, ": ", e.what() });
        }
      }() };
      auto const [before, at]{ previous_on_line(text, h.begin) };
      if (d == dialect::POWERSHELL && line.starts_with("& ") && before == '&' &&
          (at == 0 || text[at - 1] != '&')) {
        line.erase(0, 2);  // the author's `& {{yarn}}` already calls it
      }
      product_trace(cfg.p, h.name, cfg.phase, grant, true, line);
      return line;
    }

    auto const dirs{ [&](pkg *r) {  // checking: predicted paths
      return render ? product_payload_dir(cfg.p, r) : pkg_predicted_path(r);
    } };
    std::string const value{ without_trailing_separator(
        product_util_path_utf8(q, h.name, dirs)) };
    if (d != dialect::UNKNOWN && q->type == pkg_type::CACHE_MANAGED &&
        !unquotable(value, d)) {
      bool const root{ !unquotable(util_path_to_utf8(dirs(q).make_preferred()), d) };
      throw_runtime_error({ cfg.what,
                            ": '{{",
                            h.name,
                            "}}' is '",
                            value,
                            root ? "', under a cache root" : "', a product value",
                            " a shell would misread; envy inserts paths unquoted, so they "
                            "hold only ",
                            allowed_characters(d),
                            root ? " (set --cache-root or ENVY_CACHE_ROOT to one)" : "" });
    }
    if (d == dialect::CMD && is_batch(value) && cmd_command_start(text, h.begin)) {
      throw_runtime_error({ cfg.what,
                            ": '{{",
                            h.name,
                            "}}' is a batch file; write 'call {{",
                            h.name,
                            "}}' so the script resumes after it" });
    }
    if (render) { product_trace(cfg.p, h.name, cfg.phase, grant, true, value); }
    return value;
  } catch (std::exception const &e) {
    if (render) { product_trace(cfg.p, h.name, cfg.phase, grant, false, e.what()); }
    throw;
  }
}

std::string expand(std::string_view text,
                   product_expand_cfg const &cfg,
                   dialect d,
                   bool render) {
  std::string out;
  std::size_t at{ 0 };
  for (auto const &h : placeholders(text, cfg.what)) {
    out.append(text, at, h.begin - at);
    at = h.end;
    out += h.escape() ? h.literal : expand_one(text, h, cfg, d, render);
  }
  out.append(text.substr(at));
  return out;
}

}  // namespace

void product_trace(pkg *consumer,
                   std::string const &name,
                   pkg_phase phase,
                   product_grant const &grant,
                   bool allowed,
                   std::string const &reason) {
  ENVY_TRACE(lua_ctx_product_access,
             consumer->cfg->identity,
             .target = name,
             .provider = grant.provider ? grant.provider->cfg->identity : std::string{},
             .current_phase = phase,
             .needed_by = grant.needed_by,
             .allowed = allowed,
             .reason = reason);
}

product_grant product_authorize(pkg *consumer,
                                pkg_phase phase,
                                std::string const &name,
                                std::string_view what,
                                bool trace,
                                std::string_view hint) {
  if (name.empty()) { throw_runtime_error({ what, ": product name cannot be empty" }); }
  product_access a{
    .consumer = consumer, .name = name, .what = what, .phase = phase, .traced = trace
  };

  bool const own{ [&] {
    std::lock_guard const deps_lock(consumer->deps_mutex);
    return consumer->products.contains(name);
  }() };
  if (own) { return { consumer }; }  // spec_fetch rejects depending on it too

  pkg::product_dependency const dep{ [&] {
    {  // a declared product dependency first; copied, as resolution writes it
      std::lock_guard const deps_lock(consumer->deps_mutex);
      auto const it{ consumer->product_dependencies.find(name) };
      if (it != consumer->product_dependencies.end()) { return it->second; }
    }

    // Else the registry's provider, through a direct edge (which drove its install).
    pkg *const provider{ consumer->eng ? consumer->eng->find_product_provider(name)
                                       : nullptr };
    if (!provider) {
      throw a.refuse("pkg '" + consumer->cfg->identity +
                     "' does not declare product dependency on '" + name + "'" +
                     std::string{ hint });
    }

    // The edge must reach the registry's exact package; option variants share an identity.
    auto const edge{ [&]() -> std::optional<pkg_phase> {
      std::lock_guard const deps_lock(consumer->deps_mutex);
      auto const it{ consumer->dependencies.find(provider->cfg->identity) };
      return it == consumer->dependencies.end() || it->second.p != provider
                 ? std::nullopt
                 : std::optional{ it->second.needed_by };
    }() };
    if (!edge) {
      a.grant.provider = provider;
      throw a.refuse("'" + provider->cfg->identity + "' provides product '" + name +
                     "', but pkg '" + consumer->cfg->identity +
                     "' does not depend on it — declare it as a dependency: "
                     "{ product = \"" +
                     name + "\" }");
    }
    return pkg::product_dependency{ .name = name,
                                    .needed_by = *edge,
                                    .provider = provider };
  }() };

  a.grant = { dep.provider, dep.needed_by };

  if (phase < dep.needed_by) {
    throw a.refuse("product '" + name + "' needed_by '" +
                   std::string(pkg_phase_name(dep.needed_by)) + "' but accessed during '" +
                   std::string(pkg_phase_name(phase)) + "'");
  }
  if (!dep.provider) {
    throw a.refuse("product '" + name + "' provider not resolved for pkg '" +
                   consumer->cfg->identity + "'");
  }
  if (!dep.constraint_identity.empty() &&
      dep.provider->cfg->identity != dep.constraint_identity) {
    throw a.refuse("product '" + name + "' must come from '" + dep.constraint_identity +
                   "', but provider is '" + dep.provider->cfg->identity + "'");
  }

  // The declaring package: the edge's, or a weak fallback's closure member.
  try {
    if (pkg *const owner{ product_util_owner(consumer, name) }) {
      a.grant.provider = owner;
    }
  } catch (std::exception const &e) { throw a.refuse(e.what()); }
  return a.grant;
}

std::filesystem::path product_payload_dir(pkg *consumer, pkg *provider) {
  return provider == consumer && provider->pkg_path.empty() ? pkg_predicted_path(provider)
                                                            : provider->pkg_path;
}

std::string product_expand(std::string_view text, product_expand_cfg const &cfg) {
  return expand(text, cfg, dialect_of(cfg.shell), true);
}

void product_expand_check(pkg *p,
                          pkg_phase phase,
                          std::string_view text,
                          std::string_view what,
                          std::optional<resolved_shell> const &shell) {
  resolved_shell const sh{ shell.value_or(kShellPlatformDefault) };  // stand-in if UNKNOWN
  shell_env_t const env;
  std::filesystem::path const none;
  expand(text,
         { .p = p,
           .phase = phase,
           .what = what,
           .shell = sh,
           .env = env,
           .cwd = none,
           .state_root = none },
         shell ? dialect_of(*shell) : dialect::UNKNOWN,
         false);
}

void product_run_phase_script(pkg *p,
                              pkg_phase phase,
                              std::string_view verb,
                              std::string_view label,
                              std::string_view script,
                              std::filesystem::path const &cwd,
                              std::filesystem::path const &state_root,
                              lua_State *held,
                              tui::section_handle section,
                              std::filesystem::path const &cache_root) {
  resolved_shell const shell{ pkg_default_shell(p) };
  shell_env_t env{ pkg_env_for_phase(p, phase, state_root) };
  std::string const what{ p->cfg->identity + " " + std::string{ verb } };
  std::string const text{ product_expand(script,
                                         { .p = p,
                                           .phase = phase,
                                           .what = what,
                                           .shell = shell,
                                           .env = env,
                                           .cwd = cwd,
                                           .state_root = state_root,
                                           .held = held }) };
  tui_actions::run_phase_shell_script(
      text, label, cwd, p->cfg->identity, shell, section, cache_root, std::move(env));
}

}  // namespace envy
