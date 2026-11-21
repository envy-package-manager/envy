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

TEST_CASE("shell_quote_argv: POSIX quotes everything outside a bare set") {
  using envy::shell_choice;
  using envy::shell_quote_argv;
  for (char c{ 0x21 }; c < 0x7f; ++c) {  // every printable but space
    std::string const arg{ 'a', c };
    bool const bare{ (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') ||
                     std::string_view{ "_-.:+/@," }.find(c) != std::string_view::npos };
    std::string const quoted{ c == '\'' ? std::string{ "'a'\\'''" } : "'" + arg + "'" };
    CAPTURE(arg);
    CHECK(shell_quote_argv({ "/x", arg }, shell_choice::bash) ==
          "/x " + (bare ? arg : quoted));
  }
  CHECK(shell_quote_argv({ "if", "x" }, shell_choice::bash) == "'if' x");  // a program
  CHECK(shell_quote_argv({ "time" }, shell_choice::sh) == "'time'");
  CHECK(shell_quote_argv({ "/opt/node", "a b", "it's", "", "-x=1" }, shell_choice::sh) ==
        "/opt/node 'a b' 'it'\\''s' '' '-x=1'");
}

TEST_CASE("shell_quote_argv: PowerShell") {
  using envy::shell_choice;
  using envy::shell_quote_argv;
  auto const q{ [](std::vector<std::string> const &argv) {
    return shell_quote_argv(argv, shell_choice::powershell);
  } };

  CHECK(q({ "C:\\n\\node.exe", "a b", "it's", "50%", "/x" }) ==
        "& C:\\n\\node.exe 'a b' 'it''s' '50%' /x");
  CHECK(q({ "C:\\Program Files\\x.exe" }) == "& 'C:\\Program Files\\x.exe'");
  CHECK(q({ "x", "-Djava.awt.headless", "@args", "$HOME", "a`b" }) ==
        "& x '-Djava.awt.headless' '@args' '$HOME' 'a`b'");
  CHECK(q({ "x",
            "O\xE2\x80\x99"
            "Brien" }) ==
        "& x 'O\xE2\x80\x99\xE2\x80\x99"
        "Brien'");
  CHECK(q({ "x", "C:\\no-space\\" }) == "& x C:\\no-space\\");

  CHECK_THROWS_WITH(q({ "x", "" }),
                    "PowerShell cannot quote '': powershell.exe drops an empty argument");
  CHECK_THROWS_WITH(q({ "x", "say \"hi\"" }),
                    doctest::Contains("passes '\"' to a program unescaped"));
  CHECK_THROWS_WITH(q({ "x", "C:\\a b\\" }),
                    doctest::Contains("turns a trailing '\\' into an escaped quote"));
  CHECK_THROWS_WITH(q({ "x", "a\nb" }), doctest::Contains("a line break"));

  // A .bat/.cmd runs through cmd, which PowerShell hands `a&b` bare: data, not a command.
  CHECK(q({ "C:\\w\\gradlew.bat", "build", "a b" }) == "& C:\\w\\gradlew.bat build 'a b'");
  for (char const *const meta : { "a&b", "a|b", "a<b", "a>b", "a^b", "50%", "wow!" }) {
    CAPTURE(meta);
    CHECK_THROWS_WITH(q({ "C:\\w\\gradlew.CMD", meta }),
                      doctest::Contains("hands a .bat/.cmd its arguments through cmd"));
    CHECK_NOTHROW(q({ "C:\\w\\node.exe", meta }));  // a native program gets it intact
  }
}

TEST_CASE("shell_quote_argv: cmd") {
  using envy::shell_choice;
  using envy::shell_quote_argv;
  auto const q{ [](std::vector<std::string> const &argv) {
    return shell_quote_argv(argv, shell_choice::cmd);
  } };

  CHECK(q({ "C:\\n\\node.exe", "a b", "50%", "-x=1", "C:/fwd" }) ==
        "C:\\n\\node.exe \"a b\" \"50%%\" \"-x=1\" \"C:/fwd\"");
  CHECK(q({ "x", "C:\\a b\\", "a b\\\\" }) == "\"x\" \"C:\\a b\\\\\" \"a b\\\\\\\\\"");
  CHECK(q({ "C:\\w\\gradlew.BAT", "build", "a b" }) ==
        "call C:\\w\\gradlew.BAT build \"a b\"");
  CHECK(q({ "g.exe", "a^b" }) == "\"g.exe\" \"a^b\"");  // not a builtin either
  CHECK(q({ "start" }) == "\"start\"");

  CHECK_THROWS_WITH(q({ "say \"hi\"" }), doctest::Contains("cmd cannot quote"));
  CHECK_THROWS_WITH(q({ "wow!" }), doctest::Contains("cmd cannot quote 'wow!'"));
  CHECK_THROWS_WITH(q({ "x", "a\r\nb" }), doctest::Contains("a line break"));
  CHECK_THROWS_WITH(q({ "g.cmd", "50%" }), doctest::Contains("'call' expands '%'"));
  CHECK_THROWS_WITH(q({ "g.cmd", "a^b" }), doctest::Contains("'call' expands '%'"));
}
