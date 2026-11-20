#include "lua_envy_file_ops.h"

#include "lua_phase_context.h"
#include "tree_hash.h"
#include "util.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace envy {

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
}

}  // namespace envy
