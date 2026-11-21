#include "lua_envy_file_ops.h"

#include "lua_phase_context.h"
#include "sol_util.h"
#include "tree_hash.h"
#include "util.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace envy {
namespace {

constexpr std::string_view kWriteOptions[]{ "executable" };

// envy.write(path, content, opts?) - Atomic binary write; parents created, handle closed
void envy_write(sol::this_state L,
                std::string_view path,
                sol::stack_object content,
                sol::optional<sol::object> opts) {
  bool const executable{ [&] {
    if (!opts || !opts->valid()) { return false; }  // absent or nil
    if (!opts->is<sol::table>()) {
      throw_runtime_error({ "envy.write: opts must be a table" });
    }
    sol::table const t{ opts->as<sol::table>() };
    sol_util_reject_unknown_keys(t, kWriteOptions, "envy.write");
    return sol_util_get_optional<bool>(t, "executable", "envy.write").value_or(false);
  }() };
  lua_phase_context_write_file(L,
                               path,
                               sol_util_string_arg(content, "envy.write"),
                               "envy.write",
                               executable);
}

// envy.mkdir(path) - create_directories; an existing directory is not an error
void envy_mkdir(sol::this_state L, std::string_view path) {
  std::filesystem::path const p{ lua_phase_context_resolve(L, std::string{ path }) };
  std::error_code create_ec;
  std::filesystem::create_directories(p, create_ec);
  std::error_code stat_ec;
  if (std::filesystem::is_directory(p, stat_ec)) { return; }
  std::error_code const &ec{ create_ec ? create_ec : stat_ec };
  std::string const why{ ec ? ec.message() : "not a directory" };
  throw_runtime_error({ "envy.mkdir: cannot create ", p, ": ", why });
}

}  // namespace

void lua_envy_file_ops_install(sol::table &envy_table) {
  // envy.copy(src, dst) - Copy file or directory (relative paths anchored to stage_dir)
  envy_table["copy"] =
      [](std::string const &src_str, std::string const &dst_str, sol::this_state L) {
        std::filesystem::path const src{ lua_phase_context_resolve(L, src_str) };
        std::filesystem::path dst{ lua_phase_context_resolve(L, dst_str) };

        if (!std::filesystem::exists(src)) {
          throw_runtime_error({ "envy.copy: source not found: ", src });
        }

        // If copying file to directory, append filename
        if (std::filesystem::is_regular_file(src) && std::filesystem::is_directory(dst)) {
          dst = dst / src.filename();
        }

        // Both branches: std::filesystem::copy creates only dst itself, so a directory
        // destination needs its ancestors too.
        if (dst.has_parent_path()) {
          std::filesystem::create_directories(dst.parent_path());
        }

        if (std::filesystem::is_directory(src)) {
          std::filesystem::copy(src,
                                dst,
                                std::filesystem::copy_options::recursive |
                                    std::filesystem::copy_options::overwrite_existing);
        } else {
          std::filesystem::copy_file(src,
                                     dst,
                                     std::filesystem::copy_options::overwrite_existing);
        }
      };

  // envy.move(src, dst) - Move/rename file or directory
  envy_table["move"] =
      [](std::string const &src_str, std::string const &dst_str, sol::this_state L) {
        std::filesystem::path const src{ lua_phase_context_resolve(L, src_str) };
        std::filesystem::path dst{ lua_phase_context_resolve(L, dst_str) };

        if (!std::filesystem::exists(src)) {
          throw_runtime_error({ "envy.move: source not found: ", src });
        }

        // If moving file to directory, append filename
        if (std::filesystem::is_regular_file(src) && std::filesystem::is_directory(dst)) {
          dst = dst / src.filename();
        }

        if (dst.has_parent_path()) {
          std::filesystem::create_directories(dst.parent_path());
        }

        std::filesystem::rename(src, dst);
      };

  // envy.remove(path) - Delete file or directory recursively
  envy_table["remove"] = [](std::string const &path_str, sol::this_state L) {
    std::filesystem::path const path{ lua_phase_context_resolve(L, path_str) };
    // A spec still learns when a tree it asked to delete is still there.
    if (auto const ec{ tree_remove_insisting(path) }) {
      throw std::filesystem::filesystem_error("envy.remove", path, ec);
    }
  };

  // envy.exists(path) - Check if path exists
  envy_table["exists"] = [](std::string const &path_str, sol::this_state L) -> bool {
    return std::filesystem::exists(lua_phase_context_resolve(L, path_str));
  };

  // envy.is_file(path) - Check if path is regular file
  envy_table["is_file"] = [](std::string const &path_str, sol::this_state L) -> bool {
    return std::filesystem::is_regular_file(lua_phase_context_resolve(L, path_str));
  };

  // envy.is_dir(path) - Check if path is directory
  envy_table["is_dir"] = [](std::string const &path_str, sol::this_state L) -> bool {
    return std::filesystem::is_directory(lua_phase_context_resolve(L, path_str));
  };

  envy_table.set_function("write", envy_write);  // set_function keeps pcall's trampoline
  envy_table.set_function("mkdir", envy_mkdir);
}

}  // namespace envy
