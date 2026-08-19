---@meta
-- lua_ls type definitions for envy
-- Generated for envy @@ENVY_VERSION@@

--------------------------------------------------------------------------------
-- Platform Constants
--------------------------------------------------------------------------------

---@class envy
---@field PLATFORM "darwin"|"linux"|"windows" Current operating system
---@field ARCH "arm64"|"x86_64" CPU architecture
---@field PLATFORM_ARCH string Platform-architecture pair (e.g., "darwin-arm64")
---@field EXE_EXT ""|".exe" Executable extension ("" on Unix, ".exe" on Windows)
envy = {}

--------------------------------------------------------------------------------
-- Logging
--------------------------------------------------------------------------------

---Emit debug-level log message
---@param msg string
function envy.debug(msg) end

---Emit info-level log message
---@param msg string
function envy.info(msg) end

---Emit warning-level log message
---@param msg string
function envy.warn(msg) end

---Emit error-level log message
---@param msg string
function envy.error(msg) end

---Write directly to stdout (bypasses TUI)
---@param msg string
function envy.stdout(msg) end

--------------------------------------------------------------------------------
-- String Templates
--------------------------------------------------------------------------------

---Replace `{{key}}` placeholders with values from table
---@param str string Template string with `{{key}}` placeholders
---@param values table<string, any> Key-value pairs for substitution
---@return string
function envy.template(str, values) end

---Extend target array with items from one or more source arrays
---@param target any[] Target array to extend (modified in-place)
---@param ... any[] Source arrays to append
---@return any[] target The same target array (for chaining)
function envy.extend(target, ...) end

--------------------------------------------------------------------------------
-- Path Operations
--------------------------------------------------------------------------------

---@class envy.path
envy.path = {}

---Join path components with platform-appropriate separator
---@param ... string Path components
---@return string
function envy.path.join(...) end

---Extract filename with extension from path
---@param path string
---@return string
function envy.path.basename(path) end

---Extract parent directory from path
---@param path string
---@return string
function envy.path.dirname(path) end

---Extract filename without extension from path
---@param path string
---@return string
function envy.path.stem(path) end

---Extract file extension including leading dot
---@param path string
---@return string
function envy.path.extension(path) end

--------------------------------------------------------------------------------
-- File Operations
--------------------------------------------------------------------------------

---Copy file or directory (recursive). Relative paths resolve from stage_dir.
---@param src string Source path
---@param dst string Destination path
function envy.copy(src, dst) end

---Move/rename file or directory. Relative paths resolve from stage_dir.
---@param src string Source path
---@param dst string Destination path
function envy.move(src, dst) end

---Delete file or directory recursively. Relative paths resolve from stage_dir.
---@param path string Path to remove
function envy.remove(path) end

---Check if path exists
---@param path string
---@return boolean
function envy.exists(path) end

---Check if path is a regular file
---@param path string
---@return boolean
function envy.is_file(path) end

---Check if path is a directory
---@param path string
---@return boolean
function envy.is_dir(path) end

--------------------------------------------------------------------------------
-- Shell Execution
--------------------------------------------------------------------------------

---@class envy.run_opts
---@field cwd? string Working directory (absolute or relative to stage_dir)
---@field env? table<string, string> Environment variables (merged with inherited)
---@field shell? integer|envy.shell_config Shell choice (ENVY_SHELL.*) or config table
---@field quiet? boolean Suppress output (default: false)
---@field capture? boolean Capture stdout/stderr in result (default: false)
---@field check? boolean Throw on non-zero exit code (default: true)
---@field interactive? boolean Enable interactive mode for user input (default: false)

---@class envy.run_result
---@field exit_code integer Process exit code
---@field stdout? string Captured stdout (only if capture=true)
---@field stderr? string Captured stderr (only if capture=true)

---Execute shell script
---@param script string|string[] Shell script (string or array of lines)
---@param opts? envy.run_opts Execution options
---@return envy.run_result
function envy.run(script, opts) end

--------------------------------------------------------------------------------
-- Archive Extraction
--------------------------------------------------------------------------------

---@class envy.extract_opts
---@field strip? integer Strip leading path components (default: 0)
---@field only? string[] Extract only these archive-relative paths or globs, matched after
--- strip; a directory takes its whole subtree (default: everything). Globs: '*'/'?' within
--- one component, '**' across components, '[a-z]'/'[!a-z]' classes. An entry that matches
--- nothing is an error.

---Extract single archive to destination directory
---@param archive_path string Path to archive file
---@param dest_dir string Destination directory
---@param opts? envy.extract_opts Extraction options
---@return integer files_extracted Number of files extracted
function envy.extract(archive_path, dest_dir, opts) end

---Extract all archives in source directory to destination
---@param src_dir string Directory containing archives
---@param dest_dir string Destination directory
---@param opts? envy.extract_opts Extraction options
function envy.extract_all(src_dir, dest_dir, opts) end

--------------------------------------------------------------------------------
-- Fetch Operations
--------------------------------------------------------------------------------

---@class envy.fetch_spec
---@field source string URL or local path
---@field sha256? string Expected SHA256 hash for verification
---@field ref? string Git ref (branch, tag, commit) for git sources

---@class envy.fetch_opts
---@field dest string Destination directory (required)

---Download files to destination directory
---@param source string|envy.fetch_spec|string[]|envy.fetch_spec[] Source URL(s) or spec(s)
---@param opts envy.fetch_opts Fetch options (dest is required)
---@return string|string[] basename(s) Downloaded filename(s)
function envy.fetch(source, opts) end

---@class envy.commit_fetch_entry
---@field filename string File to commit
---@field sha256? string Expected SHA256 hash for verification

---Move verified files from tmp_dir to fetch_dir (FETCH phase only)
---@param files string|envy.commit_fetch_entry|string[]|envy.commit_fetch_entry[]
function envy.commit_fetch(files) end

---Verify file hash matches expected SHA256
---@param file_path string Path to file
---@param expected_sha256 string Expected SHA256 hash (hex string)
---@return boolean matches True if hash matches
function envy.verify_hash(file_path, expected_sha256) end

--------------------------------------------------------------------------------
-- Dependency Access
--------------------------------------------------------------------------------

---Get installed package path for a dependency
---@param identity string Dependency identity (e.g., "namespace.name@version")
---@return string pkg_path Absolute path to dependency's installed package
function envy.package(identity) end

---Get product value from a dependency
---@param name string Product name declared in provider's PRODUCTS
---@return string value Product value (path or arbitrary string)
function envy.product(name) end

---Load Lua file from declared dependency into sandboxed environment
---Supports fuzzy identity matching (e.g., "helpers" matches "acme.helpers@v1")
---Must be called within phase function; validates dependency graph and needed_by
---@param identity string Dependency identity (fuzzy match supported)
---@param module string Module path using Lua dot syntax (e.g., "lib.common" → lib/common.lua)
---@return table env Table containing globals defined in the loaded file
function envy.loadenv_spec(identity, module) end

---Load Lua file relative to current file into sandboxed environment
---Path is resolved relative to the file calling loadenv, not cwd
---@param module string Module path using Lua dot syntax (e.g., "lib.utils" → lib/utils.lua)
---@return table env Table containing globals defined in the loaded file
function envy.loadenv(module) end

---Validate current options against a declarative schema.
---Checks required, type (incl. semver), range, choices, then custom validators.
---Rejects unknown options not declared in the schema. Throws on validation failure.
---@param schema table<string, envy.option_constraint> Per-option constraint table
function envy.options(schema) end

--------------------------------------------------------------------------------
-- Shell Constants
--------------------------------------------------------------------------------

---@class ENVY_SHELL
---@field BASH integer Bash shell (Unix)
---@field SH integer POSIX sh shell (Unix)
---@field CMD integer Windows cmd.exe
---@field POWERSHELL integer PowerShell (Windows)
ENVY_SHELL = {}

---@class envy.shell_config
---@field choice? integer Shell choice (ENVY_SHELL.*)
---@field file? string Path to shell executable
---@field inline? string[] Command template array (use {} for script placeholder)

--------------------------------------------------------------------------------
-- Spec Globals
--------------------------------------------------------------------------------

---Spec identity in "namespace.name@version" format
---@type string
IDENTITY = ""

---Spec dependencies array
---@alias envy.dependency { spec: string, source?: string, needed_by?: "fetch"|"stage"|"build"|"install"|"check", product?: string, weak?: boolean, options?: table }
---@type envy.dependency[]
DEPENDENCIES = {}

---Spec platform constraints. Empty/absent = all platforms.
---Values: OS names ("darwin", "linux", "windows") or OS-arch combos ("darwin-arm64").
---@type string[]?
PLATFORMS = nil

---@class envy.product_entry
---@field value string Relative path to product (required)
---@field script? boolean Generate wrapper script (default: true)
---@field platforms? string[] Platform constraints for this product (empty/absent = all)

---Spec products - paths relative to install_dir, or function returning same.
---String values are shorthand for { value = "...", script = true }.
---@type table<string, string|envy.product_entry>|fun(options: table): table<string, string|envy.product_entry>
PRODUCTS = {}

--------------------------------------------------------------------------------
-- Phase Functions/Values
--------------------------------------------------------------------------------

---@alias envy.fetch_source { source: string, sha256?: string, ref?: string }

---FETCH phase: declarative source specification or function
---@type envy.fetch_source|envy.fetch_source[]|fun(tmp_dir: string, options: table): string?, string?
FETCH = {}

---@alias envy.stage_opts { strip?: integer, only?: string[] }

---STAGE phase: declarative options or function
---@type envy.stage_opts|fun(fetch_dir: string, stage_dir: string, tmp_dir: string, options: table)
STAGE = {}

---BUILD phase: shell script string or function
---@type string|fun(install_dir: string, stage_dir: string, fetch_dir: string, tmp_dir: string, options: table)
BUILD = nil

---INSTALL phase (cache-managed only): shell script string or function
---@type string|fun(install_dir: string, stage_dir: string, fetch_dir: string, tmp_dir: string, options: table)
INSTALL = nil

---@class envy.setup_pair
---@field CHECK string|fun(pkg_dir: string?, options: table): boolean|string Satisfaction probe: true/exit 0 = already satisfied. String (or returned string) runs as shell.
---@field INSTALL string|fun(pkg_dir: string?, options: table): string? Host mutation, runs with cwd=project_root when CHECK fails. Returned string runs as shell.
---@field PLATFORMS? string[] Per-pair platform constraints (empty/absent = all)

---SETUP: named host-side CHECK/INSTALL pairs, re-evaluated every run (never cached,
---never hashed into the package key). pkg_dir is the installed payload path for
---cache-managed packages, nil for user-managed. User-managed specs define ONLY
---SETUP pairs (all selected by default); cache-managed packages run pairs only when
---a manifest entry selects them via `setup = { "name", ... }`.
---@type table<string, envy.setup_pair>
SETUP = nil

---@alias envy.option_constraint { required?: boolean, type?: "string"|"int"|"float"|"boolean"|"table"|"list"|"semver", range?: string, choices?: any[], validate?: fun(value: any): nil|boolean|string }

---OPTIONS: declarative schema table or validator function
---Table form validates options against per-key constraints (required, type, range, choices, validate).
---Function form receives opts, may call envy.options(), returns nil/true/false/string.
---@type table<string, envy.option_constraint>|fun(options: table): nil|boolean|string
OPTIONS = nil

---USER_MANAGED: marks the spec as user-managed (host state only; cache holds nothing).
---Boolean or function returning a boolean (called once at spec load). Defaults to false (cache-managed).
---When true, the spec defines only SETUP pairs (all selected by default) and must NOT
---define FETCH/STAGE/BUILD/INSTALL. When false (default), FETCH must be defined and
---SETUP pairs run only when selected by a manifest entry.
---@type boolean|fun(): boolean
USER_MANAGED = nil

---EXPORTABLE: cache-managed packages with EXPORTABLE=true expose their install output for export.
---When false, only fetched bytes are preserved in the cache.
---@type boolean
EXPORTABLE = nil

--------------------------------------------------------------------------------
-- Manifest Globals
--------------------------------------------------------------------------------

---@alias envy.package_spec string|{ spec: string, source?: string, options?: table, needed_by?: string, product?: string, weak?: boolean, platforms?: string[], setup?: string[] }

---Manifest packages array
---@type envy.package_spec[]
PACKAGES = {}

---Default shell configuration for all phases
---@type integer|envy.shell_config|fun(ctx: { package: fun(identity: string): string }): integer|envy.shell_config
DEFAULT_SHELL = nil

--------------------------------------------------------------------------------
-- Built-in print override
--------------------------------------------------------------------------------

---Print values (routed through TUI)
---@param ... any Values to print
function print(...) end
