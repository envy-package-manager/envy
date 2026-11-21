#include <string>
#include <unordered_map>
#include <vector>
#include "doctest.h"
#include "shell.h"

TEST_CASE("shell_getenv captures PATH") {
  auto env{ envy::shell_getenv() };
  REQUIRE(!env.empty());
#if defined(_WIN32)
  bool has_path_variant =
      (env.find("Path") != env.end()) || (env.find("PATH") != env.end());
  CHECK(has_path_variant);
#else
  CHECK(env.find("PATH") != env.end());
#endif
}

TEST_CASE("shell_quote_argv quotes only what each shell needs") {
  using envy::shell_choice;
  using envy::shell_quote_argv;
  std::vector<std::string> const argv{ "/opt/node", "a b", "it's", "", "-x=1", "50%" };

  CHECK(shell_quote_argv(argv, shell_choice::bash) ==
        "/opt/node 'a b' 'it'\\''s' '' '-x=1' '50%'");
  CHECK(shell_quote_argv(argv, shell_choice::sh) ==
        shell_quote_argv(argv, shell_choice::bash));
  CHECK(shell_quote_argv(argv, shell_choice::powershell) ==
        "& /opt/node 'a b' 'it''s' '' '-x=1' '50%'");
  CHECK(shell_quote_argv({ "C:\\n\\node.exe", "a b", "", "-x=1", "50%" },
                         shell_choice::cmd) ==
        "C:\\n\\node.exe \"a b\" \"\" \"-x=1\" \"50%%\"");
  CHECK(shell_quote_argv({ "C:\\n\\node.exe" }, shell_choice::powershell) ==
        "& C:\\n\\node.exe");

  CHECK_THROWS_WITH(shell_quote_argv({ "say \"hi\"" }, shell_choice::cmd),
                    "cmd cannot quote 'say \"hi\"'");
  CHECK_THROWS_WITH(shell_quote_argv({ "wow!" }, shell_choice::cmd),
                    "cmd cannot quote 'wow!'");
}
