#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace envy {

// The closed set of destinations the parser can fill. One switch converts every string,
// so a new option costs a descriptor rather than a template instantiation.
enum class cli_dest : uint8_t {
  HELP,
  FLAG,        // bool
  INT,         // int
  STRING,      // std::string
  PATH,        // std::filesystem::path
  OPT_BOOL,    // std::optional<bool>
  OPT_STRING,  // std::optional<std::string>
  OPT_PATH,    // std::optional<std::filesystem::path>
  VEC_STRING,  // std::vector<std::string>
  VEC_PATH,    // std::vector<std::filesystem::path>
};

enum class cli_check : uint8_t { NONE, EXISTING_FILE, EXISTING_DIR, ONE_OF };

// Plain descriptor: no behavior, no ownership, no virtuals.
struct cli_option {
  char const *names{};  // "-q,--quiet"; for a positional, its display name
  char const *desc{};
  void *dest{};
  char const *env{};      // environment variable consulted when the option is absent
  char const *allowed{};  // comma-separated set for cli_check::ONE_OF
  uint64_t excludes{};    // bitmask of option indices this one cannot appear beside
  uint32_t count{};       // occurrences seen this parse
  cli_dest kind{ cli_dest::FLAG };
  cli_check check{ cli_check::NONE };
  bool required{};
  bool positional{};
  bool take_last{};
  bool value_optional{};  // may appear with no value at all
};

class cli_cmd;

// Handle to one registered option; chainable so a registration reads top to bottom.
class cli_opt {
 public:
  cli_opt(cli_cmd *cmd, int idx) : cmd_{ cmd }, idx_{ idx } {}

  cli_opt required() const;
  cli_opt envname(char const *var) const;
  cli_opt excludes(cli_opt other) const;
  cli_opt check_file() const;
  cli_opt check_dir() const;
  cli_opt one_of(char const *csv) const;
  cli_opt take_last() const;
  cli_opt optional_value() const;

  int index() const { return idx_; }
  uint32_t count() const;

 private:
  cli_option &option() const;

  cli_cmd *cmd_;
  int idx_;
};

// Cross-option validation a command owes itself: nullptr on success, else the message.
using cli_finalize_fn = char const *(*)(void *ctx);

// One command (the root included): its options, its positionals, its children.
class cli_cmd {
 public:
  cli_cmd(cli_cmd *parent, char const *name, char const *desc);

  cli_cmd(cli_cmd const &) = delete;
  cli_cmd &operator=(cli_cmd const &) = delete;

  // Registration: one overload per destination kind, each recording a pointer and a tag.
  cli_opt flag(char const *n, bool &d, char const *desc) {
    return add(n, &d, k_flag, desc);
  }
  cli_opt opt(char const *n, int &d, char const *desc) { return add(n, &d, k_int, desc); }
  cli_opt opt(char const *n, std::string &d, char const *desc) {
    return add(n, &d, k_string, desc);
  }
  cli_opt opt(char const *n, std::filesystem::path &d, char const *desc) {
    return add(n, &d, k_path, desc);
  }
  cli_opt opt(char const *n, std::optional<bool> &d, char const *desc) {
    return add(n, &d, k_opt_bool, desc);
  }
  cli_opt opt(char const *n, std::optional<std::string> &d, char const *desc) {
    return add(n, &d, k_opt_string, desc);
  }
  cli_opt opt(char const *n, std::optional<std::filesystem::path> &d, char const *desc) {
    return add(n, &d, k_opt_path, desc);
  }
  cli_opt opt(char const *n, std::vector<std::string> &d, char const *desc) {
    return add(n, &d, k_vec_string, desc);
  }

  cli_opt pos(char const *n, std::string &d, char const *desc) {
    return add(n, &d, k_string, desc, true);
  }
  cli_opt pos(char const *n, std::filesystem::path &d, char const *desc) {
    return add(n, &d, k_path, desc, true);
  }
  cli_opt pos(char const *n, std::vector<std::string> &d, char const *desc) {
    return add(n, &d, k_vec_string, desc, true);
  }
  cli_opt pos(char const *n, std::vector<std::filesystem::path> &d, char const *desc) {
    return add(n, &d, k_vec_path, desc, true);
  }

  cli_cmd &sub(char const *name, char const *desc);

  // Everything from the first argument this command does not own goes to dest, verbatim.
  void prefix_command(std::vector<std::string> &dest) { prefix_dest_ = &dest; }

  void finalize(cli_finalize_fn fn, void *ctx) { finalize_ = fn, finalize_ctx_ = ctx; }

  // Which variant alternative this command fills; -1 means "runs nothing".
  cli_cmd &set_id(int id) { return id_ = id, *this; }
  int id() const { return id_; }

  std::string help() const;
  std::string full_name() const;

  // Parsing internals, public because the engine is a free function rather than a friend.
  std::vector<cli_option> &options() { return options_; }
  std::vector<cli_option> const &options() const { return options_; }
  std::vector<std::unique_ptr<cli_cmd>> const &subs() const { return subs_; }
  cli_cmd *find_sub(std::string_view name) const;
  cli_option *find_long(std::string_view name);
  cli_option *find_short(char c);
  cli_option *next_positional();
  bool has_empty_positional() const;
  bool owns_option(std::string_view arg);
  std::vector<std::string> *prefix_dest() const { return prefix_dest_; }
  cli_finalize_fn finalize_fn() const { return finalize_; }
  void *finalize_ctx() const { return finalize_ctx_; }
  cli_cmd *parent() const { return parent_; }

 private:
  static constexpr cli_dest k_flag{ cli_dest::FLAG };
  static constexpr cli_dest k_int{ cli_dest::INT };
  static constexpr cli_dest k_string{ cli_dest::STRING };
  static constexpr cli_dest k_path{ cli_dest::PATH };
  static constexpr cli_dest k_opt_bool{ cli_dest::OPT_BOOL };
  static constexpr cli_dest k_opt_string{ cli_dest::OPT_STRING };
  static constexpr cli_dest k_opt_path{ cli_dest::OPT_PATH };
  static constexpr cli_dest k_vec_string{ cli_dest::VEC_STRING };
  static constexpr cli_dest k_vec_path{ cli_dest::VEC_PATH };

  cli_opt add(char const *names,
              void *dest,
              cli_dest kind,
              char const *desc,
              bool positional = false);

  cli_cmd *parent_{};
  char const *name_{};
  char const *desc_{};
  int id_{ -1 };
  std::vector<cli_option> options_;
  std::vector<std::unique_ptr<cli_cmd>> subs_;
  std::vector<std::string> *prefix_dest_{};
  cli_finalize_fn finalize_{};
  void *finalize_ctx_{};
};

struct cli_parse_result {
  std::string error;      // empty when the argv parsed
  std::string help_text;  // the deepest reached command's help
  int selected_id{ -1 };  // -1 when nothing is to be run
  bool help_requested{};  // -h / --help, as opposed to "nothing selected"
};

cli_parse_result cli_run(cli_cmd &root, int argc, char **argv);

}  // namespace envy
