#include "cmd_use.h"

#include "manifest.h"

#include "doctest.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace {

constexpr std::string_view kPin{
  "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
};
constexpr std::string_view kPin2{
  "a3bf4f1b2b0b822cd15d6c15b0f00a089f86d081884c7d659a2feaa0c55ad015"
};

std::string rewrite(std::string_view content,
                    std::string_view version,
                    std::optional<std::string> const &sums = std::nullopt) {
  return envy::use_rewrite_header(content, version, sums);
}

// A rewritten manifest is only correct if every reader agrees with it, so each case checks the
// parser's view of the result rather than only its bytes.
envy::envy_meta meta_of(std::string const &content) {
  return envy::parse_envy_meta(content);
}

}  // namespace

TEST_CASE("use_rewrite_header retargets the version and leaves the rest alone") {
  auto const out{ rewrite(
      "-- envy.lua - Project manifest\n"
      "-- @envy schema \"1\"\n"
      "-- @envy version \"0.1.5\"\n"
      "-- @envy bin \"tools\"\n"
      "PACKAGES = {}\n",
      "0.1.6") };

  CHECK(out ==
        "-- envy.lua - Project manifest\n"
        "-- @envy schema \"1\"\n"
        "-- @envy version \"0.1.6\"\n"
        "-- @envy bin \"tools\"\n"
        "PACKAGES = {}\n");
}

TEST_CASE("use_rewrite_header replaces an existing pin alongside the version") {
  auto const out{ rewrite("-- @envy version \"0.1.5\"\n"
                          "-- @envy sha256sums \"" +
                              std::string{ kPin } +
                              "\"\n"
                              "PACKAGES = {}\n",
                          "0.1.6",
                          std::string{ kPin2 }) };

  auto const meta{ meta_of(out) };
  CHECK(*meta.version == "0.1.6");
  CHECK(*meta.sha256sums == kPin2);
}

TEST_CASE("use_rewrite_header rewrites a pin sitting above the version it attests") {
  // Both spans shift relative to each other, so this is the case an edit applied in source
  // order would corrupt. Order is not a style choice here.
  auto const out{ rewrite("-- @envy sha256sums \"" + std::string{ kPin } +
                              "\"\n"
                              "-- @envy version \"0.1.5\"\n"
                              "PACKAGES = {}\n",
                          "0.1.6",
                          std::string{ kPin2 }) };

  CHECK(out ==
        "-- @envy sha256sums \"" + std::string{ kPin2 } +
            "\"\n"
            "-- @envy version \"0.1.6\"\n"
            "PACKAGES = {}\n");
}

TEST_CASE("use_rewrite_header inserts a missing pin below the version line") {
  auto const out{ rewrite("-- @envy version \"0.1.5\"\n"
                          "-- @envy bin \"tools\"\n"
                          "PACKAGES = {}\n",
                          "0.1.6",
                          std::string{ kPin }) };

  CHECK(out ==
        "-- @envy version \"0.1.6\"\n"
        "-- @envy sha256sums \"" +
            std::string{ kPin } +
            "\"\n"
            "-- @envy bin \"tools\"\n"
            "PACKAGES = {}\n");
}

TEST_CASE("use_rewrite_header inserts a pin inside the header, not below the code") {
  // Placed anywhere under the first line of code the directive is read by nothing, so the
  // manifest would bootstrap unattested while looking pinned.
  auto const out{ rewrite("-- @envy version \"0.1.5\"\nPACKAGES = {}\n",
                          "0.1.6",
                          std::string{ kPin }) };

  auto const meta{ meta_of(out) };
  REQUIRE(meta.sha256sums.has_value());
  CHECK(*meta.sha256sums == kPin);
}

TEST_CASE("use_rewrite_header matches the version line's indentation when inserting") {
  auto const out{ rewrite("\t  -- @envy version \"0.1.5\"\nPACKAGES = {}\n",
                          "0.1.6",
                          std::string{ kPin }) };

  CHECK(out ==
        "\t  -- @envy version \"0.1.6\"\n\t  -- @envy sha256sums \"" + std::string{ kPin } +
            "\"\nPACKAGES = {}\n");
}

TEST_CASE("use_rewrite_header inserts with CRLF endings in a CRLF manifest") {
  // A lone '\n' in an otherwise-CRLF header leaves the value's trailing '\r' in the directive
  // the batch launcher parses, which then appends a carriage return to a download URL.
  auto const out{ rewrite("-- @envy version \"0.1.5\"\r\nPACKAGES = {}\r\n",
                          "0.1.6",
                          std::string{ kPin }) };

  CHECK(out ==
        "-- @envy version \"0.1.6\"\r\n-- @envy sha256sums \"" + std::string{ kPin } +
            "\"\r\nPACKAGES = {}\r\n");
  CHECK(*meta_of(out).sha256sums == kPin);
}

TEST_CASE("use_rewrite_header inserts a pin below a header that runs to end of file") {
  auto const out{  // no terminator to insert after, so the new line brings its own break
    rewrite("-- @envy version \"0.1.5\"", "0.1.6", std::string{ kPin })
  };

  CHECK(out ==
        "-- @envy version \"0.1.6\"\n-- @envy sha256sums \"" + std::string{ kPin } + "\"");
  CHECK(*meta_of(out).sha256sums == kPin);
}

TEST_CASE("use_rewrite_header drops a pin line whole, terminator included") {
  auto const out{ rewrite("-- @envy version \"0.1.5\"\n"
                          "-- @envy sha256sums \"" +
                              std::string{ kPin } +
                              "\"\n"
                              "-- @envy bin \"tools\"\n"
                              "PACKAGES = {}\n",
                          "0.1.6") };

  CHECK(out ==
        "-- @envy version \"0.1.6\"\n"
        "-- @envy bin \"tools\"\n"
        "PACKAGES = {}\n");
  CHECK_FALSE(meta_of(out).sha256sums.has_value());
}

TEST_CASE("use_rewrite_header drops a pin that is the unterminated last line") {
  auto const out{ rewrite("-- @envy version \"0.1.5\"\n-- @envy sha256sums \"" +
                              std::string{ kPin } + "\"",
                          "0.1.6") };

  CHECK(out == "-- @envy version \"0.1.6\"\n");
}

TEST_CASE("use_rewrite_header edits the last of a repeated directive") {
  // parse_envy_meta's last-match-wins is what every launcher does too, so editing any earlier
  // line would change a value nothing reads.
  auto const out{ rewrite("-- @envy version \"0.0.1\"\n"
                          "-- @envy version \"0.1.5\"\n"
                          "PACKAGES = {}\n",
                          "0.1.6") };

  CHECK(out ==
        "-- @envy version \"0.0.1\"\n"
        "-- @envy version \"0.1.6\"\n"
        "PACKAGES = {}\n");
  CHECK(*meta_of(out).version == "0.1.6");
}

TEST_CASE("use_rewrite_header ignores a version directive below the first code line") {
  CHECK_THROWS_AS(rewrite("PACKAGES = {}\n-- @envy version \"0.1.5\"\n", "0.1.6"),
                  std::runtime_error);
}

TEST_CASE("use_rewrite_header preserves odd spacing and trailing comments") {
  auto const out{ rewrite("--\t@envy   version\t\"0.1.5\"  -- pinned deliberately\n"
                          "PACKAGES = {}\n",
                          "0.1.6") };

  CHECK(out ==
        "--\t@envy   version\t\"0.1.6\"  -- pinned deliberately\n"
        "PACKAGES = {}\n");
}

TEST_CASE("use_rewrite_header handles a replacement of a different length") {
  auto const out{ rewrite("-- @envy version \"0.1.10-rc.1\"\n-- @envy bin \"tools\"\n",
                          "1.0") };

  CHECK(out == "-- @envy version \"1.0\"\n-- @envy bin \"tools\"\n");
}

TEST_CASE("use_rewrite_header edits a directive inside a block comment") {
  // Pinned as the deliberate consequence of matching on the comment marker alone: the parser
  // reads this line, so a rewriter that skipped it would disagree with every reader.
  auto const out{ rewrite("--[[\n-- @envy version \"0.1.5\"\n]]\nPACKAGES = {}\n",
                          "0.1.6") };

  CHECK(*meta_of(out).version == "0.1.6");
}

TEST_CASE("use_rewrite_header throws when the header has no version directive") {
  CHECK_THROWS_AS(rewrite("-- @envy bin \"tools\"\nPACKAGES = {}\n", "0.1.6"),
                  std::runtime_error);
}
