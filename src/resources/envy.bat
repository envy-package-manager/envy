@echo off
REM envy-managed bootstrap script - do not edit
setlocal EnableDelayedExpansion

set "ENVY_DEFAULT_MIRROR=@@DOWNLOAD_URL@@"
set "ENVY_LATEST_URL=@@LATEST_URL@@"
set "ENVY_ENV_MIRROR="
if defined ENVY_MIRROR set "ENVY_ENV_MIRROR=%ENVY_MIRROR%"
set "ENVY_FALLBACK_VERSION=@@ENVY_VERSION@@"
set "ENVY_MIN_DIRECTIVE_VERSION=@@MIN_DIRECTIVE_VERSION@@"

set "ENVY_MANIFEST="
set "ENVY_CANDIDATE="
set "ENVY_DIR=%~dp0"
if "!ENVY_DIR:~-1!"=="\" set "ENVY_DIR=!ENVY_DIR:~0,-1!"
:findloop
if exist "!ENVY_DIR!\envy.lua" (
    call :read_root "!ENVY_DIR!\envy.lua"
    if "!ENVY_IS_ROOT!"=="true" (
        set "ENVY_MANIFEST=!ENVY_DIR!\envy.lua"
        goto :found
    ) else (
        set "ENVY_CANDIDATE=!ENVY_DIR!\envy.lua"
    )
)
REM A repository is a hard boundary, matching discover() in src/manifest.cpp. Without it a
REM `root "false"` project inside a checkout kept walking into the parent tree, and this
REM script and the binary picked different manifests -- so different caches. The trailing
REM backslash makes `if exist` test for a directory, as discover()'s is_directory does.
if exist "!ENVY_DIR!\.git\" (
    if defined ENVY_CANDIDATE (
        set "ENVY_MANIFEST=!ENVY_CANDIDATE!"
        goto :found
    )
    echo ERROR: envy.lua not found >&2 & exit /b 1
)
for %%I in ("!ENVY_DIR!\..") do set "ENVY_PARENT=%%~fI"
if "!ENVY_PARENT!"=="!ENVY_DIR!" (
    if defined ENVY_CANDIDATE (
        set "ENVY_MANIFEST=!ENVY_CANDIDATE!"
        goto :found
    )
    echo ERROR: envy.lua not found >&2 & exit /b 1
)
set "ENVY_DIR=!ENVY_PARENT!"
goto :findloop
:found

set "ENVY_VERSION="
set "ENVY_CACHE_LOCAL="
set "ENVY_CACHE_MODE="
set "ENVY_STATE_DIR_REL="
set "ENVY_MANIFEST_MIRROR="
set "ENVY_SUMS_PIN="

REM Header only, stopping at the first line of code, matching parse_envy_meta in
REM src/manifest.cpp. No line cap: a cap both drops directives under a long preamble -- the
REM launcher would then resolve a version or mirror the re-exec'd binary ignores -- and
REM honors a directive-shaped comment sitting in the body under the cap. `for /f` skips
REM blank lines on its own. No `delims=` override, so the default space+tab set applies:
REM `for /f` strips leading delimiters, and naming space alone would leave a tab-indented
REM comment's tab in %%a -- ending the header at a line parse_envy_meta reads straight past.
REM `eol=` clears the default `;` comment character, which skipped a `;`-led line instead of
REM ending the header on it; parse_envy_meta stops there like it does at any other code line.
REM It comes last because `eol=` takes the next character as its value, so an option behind
REM it would donate its separating space as the marker.
for /f "usebackq tokens=1,2,3,* eol=" %%a in ("!ENVY_MANIFEST!") do (
    set "ENVY_TOK=%%a"
    if not "!ENVY_TOK:~0,2!"=="--" goto :done_parse
    if "%%a"=="--" if "%%b"=="@envy" (
        set "ENVY_KEY=%%c"
        set "ENVY_RAW=%%d"
        if defined ENVY_RAW (
            call :quoted_value
            if defined ENVY_VAL (
                if "!ENVY_KEY!"=="version" set "ENVY_VERSION=!ENVY_VAL!"
                if "!ENVY_KEY!"=="cache-local" set "ENVY_CACHE_LOCAL=!ENVY_VAL!"
                if "!ENVY_KEY!"=="cache-mode" set "ENVY_CACHE_MODE=!ENVY_VAL!"
                if "!ENVY_KEY!"=="state-dir" set "ENVY_STATE_DIR_REL=!ENVY_VAL!"
                if "!ENVY_KEY!"=="mirror" set "ENVY_MANIFEST_MIRROR=!ENVY_VAL!"
                if "!ENVY_KEY!"=="sha256sums" set "ENVY_SUMS_PIN=!ENVY_VAL!"
            )
        )
    )
)
:done_parse

REM A sums pin names one release's checksum file, so it is meaningless against a resolved
REM or stamped-fallback version. Captured before the resolution chain overwrites ENVY_VERSION.
set "ENVY_PINNED_VERSION=!ENVY_VERSION!"

REM Fail closed before any network: a pin that silently stops verifying is worse than none,
REM since the manifest still advertises attestation.
if defined ENVY_SUMS_PIN if not defined ENVY_PINNED_VERSION (
    echo ERROR: '@envy sha256sums' requires '@envy version' in !ENVY_MANIFEST! >&2
    exit /b 1
)

REM Precedence: ENVY_MIRROR env > @envy mirror directive > envy upstream, matching the
REM runtime resolver in src/reexec.cpp. ENVY_DEFAULT_MIRROR is always envy's own release ENVY_URL,
REM never a copy of this project's mirror: deleting the directive must not resolve this
REM script and the re-exec'd binary to different mirrors.
if defined ENVY_ENV_MIRROR (
    set "ENVY_MIRROR=!ENVY_ENV_MIRROR!"
) else if defined ENVY_MANIFEST_MIRROR (
    set "ENVY_MIRROR=!ENVY_MANIFEST_MIRROR!"
) else (
    set "ENVY_MIRROR=!ENVY_DEFAULT_MIRROR!"
)

REM A trailing slash would produce ".../releases//v1.2.3/...", a distinct and nonexistent
REM s3:// key.
:striptrail
if "!ENVY_MIRROR:~-1!"=="/" (
    set "ENVY_MIRROR=!ENVY_MIRROR:~0,-1!"
    goto :striptrail
)

set "ENVY_MIRROR_IS_S3="
if /i "!ENVY_MIRROR:~0,5!"=="s3://" set "ENVY_MIRROR_IS_S3=1"

REM Probe bare `aws`, not `aws.exe`: PATHEXT also resolves the aws.cmd/aws.bat shims. The
REM curl.exe/tar.exe probes below name the exe deliberately, to stay policy-proof.
if not defined ENVY_MIRROR_IS_S3 goto :mirror_ok
where /q aws && goto :mirror_ok
echo ERROR: mirror "!ENVY_MIRROR!" is an s3:// URI but the aws CLI was not found on PATH. >&2
echo        Install AWS CLI v2, or use an https:// mirror. >&2
exit /b 1
:mirror_ok

set "ENVY_MANIFEST_DIR="
for %%I in ("!ENVY_MANIFEST!") do set "ENVY_MANIFEST_DIR=%%~dpI"
if "!ENVY_MANIFEST_DIR:~-1!"=="\" set "ENVY_MANIFEST_DIR=!ENVY_MANIFEST_DIR:~0,-1!"

REM Cleared ahead of the tiers, not inside :resolve_project_cache, which the override path
REM below skips outright. setlocal copies the parent environment, so an exported ENVY_MODE
REM would otherwise reach the candidate selection with a value this script never chose --
REM and `ENVY_MODE=local` plus ENVY_CACHE_ROOT would make an explicit root borrow a binary
REM from somewhere else.
set "ENVY_MODE="
set "ENVY_SHARED_CACHE="
set "ENVY_MODE_FROM_MARKER="

REM Tiers, in order, matching resolve_cache_root() in src/cache.cpp. No expansion of any
REM kind: every value is either a literal from the manifest or a path this script joins, so
REM there is no grammar for this script and the binary to disagree about.
REM An override short-circuits every project tier, and is rejected rather than absolutized:
REM the binary used to anchor a relative one to its own cwd while this script took it
REM verbatim, so one invocation named two different trees. Flat `goto`, not an if/else block,
REM because every comment below would otherwise sit inside parentheses -- where cmd re-parses
REM REM lines and a stray `(`, `&` or `>` in prose silently breaks the block.
if not defined ENVY_CACHE_ROOT goto :resolve_project_cache
set "ENVY_CACHE=!ENVY_CACHE_ROOT!"
call :require_absolute
if errorlevel 1 exit /b 1
goto :cache_resolved

:resolve_project_cache
REM The user's own cache root. Shared mode *is* this path, and a local tree borrows an envy
REM binary from it rather than downloading a second copy of one already on disk. Read-only --
REM nothing is written here on a local project's behalf. Left empty under ENVY_CACHE_ROOT,
REM which never reaches this label: an explicit root names exactly one tree.
REM
REM The USERPROFILE tier matches platform_win.cpp; without it this script and the binary name
REM different user-wide roots on a box with no LOCALAPPDATA (a service account, a stripped
REM container profile). Guarded, because an unguarded join yields "\envy" -- which is
REM *defined*, so `if exist` treats the drive root as a real cache, and the stock C:\ ACL lets
REM any authenticated user create it. Empty means there is no such root.
if defined LOCALAPPDATA set "ENVY_SHARED_CACHE=!LOCALAPPDATA!\envy"
if not defined ENVY_SHARED_CACHE if defined USERPROFILE set "ENVY_SHARED_CACHE=!USERPROFILE!\AppData\Local\envy"

REM @envy state-dir, else the manifest's own directory -- never `.envy`, which is inside the
REM default local tree `.envy\cache` and would let a cache wipe erase the marker.
set "ENVY_STATE_DIR=!ENVY_MANIFEST_DIR!"
if defined ENVY_STATE_DIR_REL set "ENVY_STATE_DIR=!ENVY_MANIFEST_DIR!\!ENVY_STATE_DIR_REL!"

REM Existence is the whole signal, so there is no file content for this script, bash and C++
REM to read three different ways. Both markers at once is a state envy never writes.
if exist "!ENVY_STATE_DIR!\.envy-cache-local" if exist "!ENVY_STATE_DIR!\.envy-cache-shared" (
    echo ERROR: both .envy-cache-local and .envy-cache-shared exist in !ENVY_STATE_DIR! >&2
    echo        envy never writes both. Delete one. >&2
    exit /b 1
)

REM Naming a tree is asking for it; a cache-local needing a second directive to take effect
REM would sit in a manifest doing nothing.
REM A marker is recorded, not declared, so it leaves no directive in the manifest -- and
REM an envy that predates the markers cannot see it either. Remembered here so the version
REM guard below can refuse that downgrade the same way it refuses a directive's.
if exist "!ENVY_STATE_DIR!\.envy-cache-local" set "ENVY_MODE=local"
if not defined ENVY_MODE if exist "!ENVY_STATE_DIR!\.envy-cache-shared" set "ENVY_MODE=shared"
if defined ENVY_MODE set "ENVY_MODE_FROM_MARKER=1"
if not defined ENVY_MODE if defined ENVY_CACHE_MODE set "ENVY_MODE=!ENVY_CACHE_MODE!"
if not defined ENVY_MODE if defined ENVY_CACHE_LOCAL set "ENVY_MODE=local"
if not defined ENVY_MODE set "ENVY_MODE=shared"

if "!ENVY_MODE!"=="local" goto :cache_local
REM Said rather than inferred: the platform default is what shared mode *is*, so with no
REM LOCALAPPDATA and no USERPROFILE there is no root to use. The bare join this replaces
REM produced "\envy" and carried on against the drive root.
if not defined ENVY_SHARED_CACHE (
    echo ERROR: cannot determine a cache root: neither LOCALAPPDATA nor USERPROFILE is set. >&2
    echo        Set ENVY_CACHE_ROOT, or give the project a local cache. >&2
    exit /b 1
)
set "ENVY_CACHE=!ENVY_SHARED_CACHE!"
goto :cache_resolved

:cache_local
REM Anchored to the manifest's directory, never the caller's cwd: one manifest names one tree
REM from every working directory. %%~fI then normalizes, which is what keeps this equal to the
REM binary's lexically_normal/make_preferred -- a cache-local of `out/.envy` joins as
REM `...\out/.envy` until it runs.
set "ENVY_CACHE=!ENVY_MANIFEST_DIR!\.envy\cache"
if defined ENVY_CACHE_LOCAL set "ENVY_CACHE=!ENVY_MANIFEST_DIR!\!ENVY_CACHE_LOCAL!"
for %%I in ("!ENVY_CACHE!") do set "ENVY_CACHE=%%~fI"

:cache_resolved

if "!ENVY_VERSION!"=="" (
    set "ENVY_LATEST_FILE=!ENVY_CACHE!\envy\latest"
    if exist "!ENVY_LATEST_FILE!" (
        set /p ENVY_LATEST_VER=<"!ENVY_LATEST_FILE!"
        if defined ENVY_LATEST_VER (
            if exist "!ENVY_CACHE!\envy\!ENVY_LATEST_VER!\envy.exe" set "ENVY_VERSION=!ENVY_LATEST_VER!"
        )
    )
)
if not "!ENVY_VERSION!"=="" goto :version_resolved

REM Ask the mirror first: 'envy mirror-envy' writes a `latest` file at the mirror root, so
REM a private or air-gapped mirror answers for itself.
set "ENVY_LATEST_TMP=!TEMP!\envy-latest-%RANDOM%%RANDOM%.txt"
set "ENVY_GOT="
if defined ENVY_MIRROR_IS_S3 (
    call aws s3 cp --only-show-errors "!ENVY_MIRROR!/latest" "!ENVY_LATEST_TMP!" >nul 2>&1 && set "ENVY_GOT=1"
) else (
    where /q curl.exe && (curl.exe -fsSL --connect-timeout 10 --max-time 300 "!ENVY_MIRROR!/latest" -o "!ENVY_LATEST_TMP!" >nul 2>&1 && set "ENVY_GOT=1")
)
if not defined ENVY_GOT goto :latest_cleanup
REM Trim with an unquoted for /f (a literal string, not a filename -- no usebackq), staging
REM through ENVY_RAW so a whitespace-only file leaves ENVY_VERSION empty.
set "ENVY_RAW_VERSION="
set /p ENVY_RAW_VERSION=<"!ENVY_LATEST_TMP!"
for /f "tokens=1" %%v in ("!ENVY_RAW_VERSION!") do set "ENVY_VERSION=%%v"
set "ENVY_VERSION_SRC=!ENVY_MIRROR!/latest"
call :check_version
:latest_cleanup
del "!ENVY_LATEST_TMP!" 2>nul
if not "!ENVY_VERSION!"=="" goto :version_resolved

REM GitHub serves no `latest` object, so fall back to its redirect. Skipped for s3://
REM mirrors, which are never github.
if defined ENVY_MIRROR_IS_S3 goto :version_fallback

REM Prefer native curl.exe (policy-resistant); parse the tag from the end of the redirect
REM chain, not hop 1: a repo rename inserts a hop whose last segment is `latest`. To a file
REM behind && rather than a `for /f` backquote: --fail still writes the -w output on an
REM HTTP error. Timeouts bound a blackholed connect.
set "ENVY_EFF_TMP=!TEMP!\envy-effective-%RANDOM%%RANDOM%.txt"
set "ENVY_EFFECTIVE="
set "ENVY_TAG="
set "ENVY_GOT="
where /q curl.exe && (curl.exe -fsSL -o nul -w "%%{url_effective}" --connect-timeout 5 --max-time 15 "!ENVY_LATEST_URL!" >"!ENVY_EFF_TMP!" 2>nul && set "ENVY_GOT=1")
REM goto, not `if defined ENVY_GOT set /p ...`: cmd applies the redirection whether or not the
REM `if` body runs, and ENVY_EFF_TMP is absent when curl.exe is.
if not defined ENVY_GOT goto :effective_cleanup
set /p ENVY_EFFECTIVE=<"!ENVY_EFF_TMP!"
:effective_cleanup
del "!ENVY_EFF_TMP!" 2>nul
if defined ENVY_EFFECTIVE set "ENVY_EFFECTIVE=!ENVY_EFFECTIVE:/=\!"
if defined ENVY_EFFECTIVE for %%a in ("!ENVY_EFFECTIVE!") do set "ENVY_TAG=%%~nxa"
if defined ENVY_TAG set "ENVY_VERSION=!ENVY_TAG!"
if defined ENVY_TAG if "!ENVY_TAG:~0,1!"=="v" set "ENVY_VERSION=!ENVY_TAG:~1!"
REM PowerShell fallback for a box without curl.exe. AllowAutoRedirect defaults on, so
REM ResponseUri is the end of the chain.
if "!ENVY_VERSION!"=="" (
    for /f "tokens=*" %%u in ('powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; try { $resp=[System.Net.WebRequest]::Create('!ENVY_LATEST_URL!').GetResponse(); $u=$resp.ResponseUri.AbsoluteUri; $resp.Close(); ($u -split '/')[-1] -replace '^v','' } catch {}" 2^>nul') do set "ENVY_VERSION=%%u"
)
set "ENVY_VERSION_SRC=!ENVY_LATEST_URL!"
call :check_version

:version_fallback
if "!ENVY_VERSION!"=="" set "ENVY_VERSION=!ENVY_FALLBACK_VERSION!"
:version_resolved

REM An older envy silently ignores the cache directives and would resolve the *shared* cache
REM for a manifest asking for a hermetic tree, then exit 0. Refuse before downloading it.
REM 0.0.0 is a dev build, which reexec_should() in src/reexec.cpp also lets through: built
REM from a working tree, so its directive support cannot be read off its version. Both shape
REM tests matter -- an unstamped template would otherwise reach `if LSS` with
REM '0.2.0'.
set "ENVY_USES_NEW_DIRECTIVES="
if defined ENVY_CACHE_LOCAL set "ENVY_USES_NEW_DIRECTIVES=1"
if defined ENVY_CACHE_MODE set "ENVY_USES_NEW_DIRECTIVES=1"
if defined ENVY_STATE_DIR_REL set "ENVY_USES_NEW_DIRECTIVES=1"
REM A recorded mode counts as much as a declared one: `envy cache --local` leaves nothing
REM in the manifest, and a pre-marker envy resolves the shared cache and exits 0.
if defined ENVY_MODE_FROM_MARKER set "ENVY_USES_NEW_DIRECTIVES=1"
REM The errorlevel test lives inside the guard: left outside it, it ran on every path --
REM including every project using none of these directives, where the ERRORLEVEL it read was
REM whatever the preceding `set`/`if` left behind rather than the guard's own.
if defined ENVY_USES_NEW_DIRECTIVES if not "!ENVY_VERSION!"=="0.0.0" (
    call :guard_directive_version
    if errorlevel 1 exit /b 1
)

set "ENVY_BIN=!ENVY_CACHE!\envy\!ENVY_VERSION!\envy.exe"
set "ENVY_CHECK_PATH=!ENVY_BIN!"
call :usable_envy
if defined ENVY_BIN_OK goto :run

REM A local tree may borrow the user's own copy of this exact version rather than download a
REM second one. Read-only: nothing is written there, and the binary that ends up running
REM still self-deploys into *this* project's cache, so the tree stays self-contained.
REM
REM Above the "Downloading envy" banner, not below it: a probe placed with the download
REM block would announce a download it then does not perform. Skipped with a sums pin,
REM because the cache fast path never re-hashes and a pinned project must not run bytes it
REM never attested out of a tree every other project writes. ENVY_MODE is empty under
REM ENVY_CACHE_ROOT, so an explicit root still names exactly one tree. Never the other
REM direction: a clone shipping its own envy\<ver>\envy.exe would be arbitrary code
REM execution on the first run of this script.
REM Into its own variable, since the download block below reuses ENVY_BIN.
if not "!ENVY_MODE!"=="local" goto :no_shared_probe
if defined ENVY_SUMS_PIN goto :no_shared_probe
if not defined ENVY_SHARED_CACHE goto :no_shared_probe
REM `if exist X set ... & goto` would take the goto unconditionally: cmd parses `&` as a
REM statement separator, not as part of the `if` body.
set "ENVY_CHECK_PATH=!ENVY_SHARED_CACHE!\envy\!ENVY_VERSION!\envy.exe"
call :usable_envy
if not defined ENVY_BIN_OK goto :no_shared_probe
set "ENVY_BIN=!ENVY_CHECK_PATH!"
goto :run
:no_shared_probe

set "ENVY_ARCH=x86_64"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PROCESSOR_ARCHITECTURE 2>nul | findstr /i "ARM64" >nul 2>&1 && set "ENVY_ARCH=arm64"

echo Downloading envy !ENVY_VERSION!... >&2
set "ENVY_ARCHIVE=envy-windows-!ENVY_ARCH!.zip"
set "ENVY_URL=!ENVY_MIRROR!/v!ENVY_VERSION!/!ENVY_ARCHIVE!"
REM Escape single quotes for PowerShell (replace ' with '')
set "ENVY_SAFE_URL=!ENVY_URL:'=''!"
REM Claim a unique temp dir via atomic mkdir (cmd's %RANDOM% can collide across
REM concurrent bootstraps; mkdir succeeds for exactly one owner of a given name).
set /a ENVY_TEMP_TRIES=0
:mktemp
set "ENVY_TEMP_DIR=!TEMP!\envy-%RANDOM%%RANDOM%"
mkdir "!ENVY_TEMP_DIR!" 2>nul && goto :gottemp
set /a ENVY_TEMP_TRIES+=1
if !ENVY_TEMP_TRIES! LSS 10 goto :mktemp
echo ERROR: Could not create a temp directory under !TEMP! >&2 & exit /b 1
:gottemp
set "ENVY_TEMP_ZIP=!ENVY_TEMP_DIR!.zip"

REM Download: prefer native curl.exe (policy-resistant), fall back to PowerShell.
set "ENVY_OK="
if defined ENVY_MIRROR_IS_S3 goto :dl_s3
goto :dl_http

:dl_s3
REM `call` so an aws resolved to a .bat/.cmd shim returns control here and ERRORLEVEL
REM survives. To a file, never piped into tar: cmd takes ERRORLEVEL from the right side of a
REM pipe only, and tar exits 0 on empty input, so a failed download would look like success.
call aws s3 cp --only-show-errors "!ENVY_URL!" "!ENVY_TEMP_ZIP!" && set "ENVY_OK=1"
goto :dl_done

:dl_http
where /q curl.exe && (curl.exe -fsSL "!ENVY_URL!" -o "!ENVY_TEMP_ZIP!" && set "ENVY_OK=1")
if not defined ENVY_OK (
    powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '!ENVY_SAFE_URL!' -OutFile '!ENVY_TEMP_ZIP!' -UseBasicParsing" && set "ENVY_OK=1"
)

:dl_done
if not defined ENVY_OK (echo ERROR: Failed to download envy from !ENVY_URL! >&2 & rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1)

REM Attest before extracting: an unattested archive must never be unpacked, or a hostile
REM mirror chooses the paths written under ENVY_TEMP_DIR, from which we run envy.
if not defined ENVY_SUMS_PIN goto :attest_done

set "ENVY_SUMS_URL=!ENVY_MIRROR!/v!ENVY_VERSION!/SHA256SUMS"
set "ENVY_SAFE_SUMS_URL=!ENVY_SUMS_URL:'=''!"
set "ENVY_SUMS_FILE=!ENVY_TEMP_DIR!\SHA256SUMS"
set "ENVY_OK="
if defined ENVY_MIRROR_IS_S3 (
    call aws s3 cp --only-show-errors "!ENVY_SUMS_URL!" "!ENVY_SUMS_FILE!" && set "ENVY_OK=1"
) else (
    where /q curl.exe && (curl.exe -fsSL "!ENVY_SUMS_URL!" -o "!ENVY_SUMS_FILE!" && set "ENVY_OK=1")
    if not defined ENVY_OK (
        powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '!ENVY_SAFE_SUMS_URL!' -OutFile '!ENVY_SUMS_FILE!' -UseBasicParsing" && set "ENVY_OK=1"
    )
)
if not defined ENVY_OK (echo ERROR: Failed to download !ENVY_SUMS_URL! >&2 & rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1)

REM Anchor the chain on the manifest's pin before trusting anything the sums file says.
set "ENVY_HASH_FILE=!ENVY_SUMS_FILE!"
call :sha256
if not defined ENVY_HASH_OUT (echo ERROR: could not compute a SHA256 of !ENVY_SUMS_FILE! >&2 & rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1)
if /i not "!ENVY_HASH_OUT!"=="!ENVY_SUMS_PIN!" (
    echo ERROR: SHA256SUMS does not match the pinned '@envy sha256sums': >&2
    echo        expected !ENVY_SUMS_PIN! >&2
    echo        got      !ENVY_HASH_OUT! >&2
    echo        The mirror is serving a different release manifest than !ENVY_MANIFEST! pinned. >&2
    echo        Update the pin deliberately; do not remove it. >&2
    rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1
)

REM Match this exact archive: keying on the hash alone accepts any platform's binary, and a
REM prefix name match accepts a longer sibling.
set "ENVY_WANT="
for /f "usebackq tokens=1,2" %%h in ("!ENVY_SUMS_FILE!") do (
    set "ENVY_NAME=%%i"
    if "!ENVY_NAME:~0,1!"=="*" set "ENVY_NAME=!ENVY_NAME:~1!"
    if /i "!ENVY_NAME!"=="!ENVY_ARCHIVE!" if not defined ENVY_WANT set "ENVY_WANT=%%h"
)
if not defined ENVY_WANT (echo ERROR: SHA256SUMS lists no entry for !ENVY_ARCHIVE! >&2 & rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1)

set "ENVY_HASH_FILE=!ENVY_TEMP_ZIP!"
call :sha256
if not defined ENVY_HASH_OUT (echo ERROR: could not compute a SHA256 of !ENVY_TEMP_ZIP! >&2 & rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1)
if /i not "!ENVY_HASH_OUT!"=="!ENVY_WANT!" (
    echo ERROR: !ENVY_ARCHIVE! failed attestation: >&2
    echo        SHA256SUMS says !ENVY_WANT! >&2
    echo        downloaded      !ENVY_HASH_OUT! >&2
    rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1
)
:attest_done

REM Extract: prefer native tar.exe (bsdtar reads zip), fall back to Expand-Archive.
set "ENVY_OK="
where /q tar.exe && (tar.exe -xf "!ENVY_TEMP_ZIP!" -C "!ENVY_TEMP_DIR!" && set "ENVY_OK=1")
if not defined ENVY_OK (
    powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Expand-Archive -Path '!ENVY_TEMP_ZIP!' -DestinationPath '!ENVY_TEMP_DIR!' -Force" && set "ENVY_OK=1"
)
if not defined ENVY_OK (echo ERROR: Failed to extract envy >&2 & rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & del "!ENVY_TEMP_ZIP!" 2>nul & exit /b 1)
del "!ENVY_TEMP_ZIP!" 2>nul
REM tar succeeds on an empty archive, so a zero-length object would fall through to :run
REM and report a missing path instead of a failed download.
if not exist "!ENVY_TEMP_DIR!\envy.exe" (echo ERROR: archive from !ENVY_URL! contained no envy binary >&2 & rmdir /s /q "!ENVY_TEMP_DIR!" 2>nul & exit /b 1)
set "ENVY_BIN=!ENVY_TEMP_DIR!\envy.exe"
goto :run

REM :read_root -- ENVY_IS_ROOT out, manifest path in %1. Reads only the manifest header: blank
REM lines and comments, stopping at the first line of code, the same rule parse_envy_meta
REM applies in src/manifest.cpp. `for /f` skips blank lines on its own, so only a code line
REM ends the scan; the default space+tab delims (no override) keep a tab-indented comment's
REM tab out of %%a, and `eol=` clears the default `;` comment character so a `;`-led line
REM ends the header rather than being skipped -- both as in the header scan above. A
REM subroutine because the early-exit `goto` has to land at this scope's top level -- inside
REM the caller's `if exist (...)` block it would tear out of the block and skip the walk's
REM own logic. Reached only by `call`.
:read_root
set "ENVY_IS_ROOT=true"
for /f "usebackq tokens=1,2,3,4 eol=" %%a in ("%~1") do (
    set "ENVY_TOK=%%a"
    if not "!ENVY_TOK:~0,2!"=="--" goto :read_root_done
    if "%%a"=="--" if "%%b"=="@envy" if "%%c"=="root" (
        set "ENVY_VAL=%%d"
        set "ENVY_VAL=!ENVY_VAL:"=!"
        if "!ENVY_VAL!"=="false" set "ENVY_IS_ROOT=false"
    )
)
:read_root_done
exit /b 0

REM :quoted_value -- ENVY_RAW in, ENVY_VAL out. ENVY_RAW is the `*` remainder of a directive line, so it
REM starts at the opening quote and may carry a trailing comment. `for /f` skips leading
REM delimiters, so with `"` as the delimiter the value is token *1*, not token 2, and it ends
REM at the closing quote either way -- matching parse_directive_line() in src/manifest.cpp.
REM The old `!ENVY_VAL:~1,-1!` trimmed one character from each end instead and folded any trailing
REM comment into the value. A `\"` inside a value still splits early --
REM already true of this parser before, and documented in docs/envy-init.md, which is why
REM envy_release_validate_mirror rejects the one directive that could carry one.
REM Options are caret-escaped and unquoted: `delims="` cannot be written inside quotes.
REM Reached only by `call`.
:quoted_value
set "ENVY_VAL="
for /f tokens^=1^ delims^=^" %%v in ("!ENVY_RAW!") do set "ENVY_VAL=%%v"
if defined ENVY_VAL set "ENVY_VAL=!ENVY_VAL:\\=\!"
exit /b 0

REM :usable_envy -- ENVY_CHECK_PATH in, ENVY_BIN_OK out. `if exist` alone is true for a
REM directory, and for a file truncated to zero that kept its name; both then fail at
REM execution instead of falling through to the next candidate. The trailing backslash is
REM cmd's directory test, the same idiom the .git boundary above uses. The size is staged
REM through a variable defaulting to 0 because %%~zI on a vanished path expands to nothing,
REM which would leave `if` comparing an empty token. Reached only by `call`.
:usable_envy
set "ENVY_BIN_OK="
if not exist "!ENVY_CHECK_PATH!" exit /b 0
if exist "!ENVY_CHECK_PATH!\" exit /b 0
set "ENVY_CHECK_SIZE=0"
for %%I in ("!ENVY_CHECK_PATH!") do set "ENVY_CHECK_SIZE=%%~zI"
if not "!ENVY_CHECK_SIZE!"=="0" set "ENVY_BIN_OK=1"
exit /b 0

REM :require_absolute -- ENVY_CACHE in; errorlevel 1 when it is not absolute. Rejected rather
REM than absolutized: the binary used to anchor a relative override to its own cwd while
REM this script took it verbatim, so one invocation named two trees. The accepted forms are
REM the ones std::filesystem calls absolute on Windows. Reached only by `call`.
:require_absolute
if "!ENVY_CACHE:~0,2!"=="\\" exit /b 0
if "!ENVY_CACHE:~0,2!"=="//" exit /b 0
if "!ENVY_CACHE:~1,2!"==":\" exit /b 0
if "!ENVY_CACHE:~1,2!"==":/" exit /b 0
echo ERROR: ENVY_CACHE_ROOT must be an absolute path: !ENVY_CACHE! >&2
exit /b 1

REM :guard_directive_version -- ENVY_VERSION and ENVY_MIN_DIRECTIVE_VERSION in; errorlevel 1 when the
REM resolved envy predates the cache directives this manifest uses. Field-wise integer
REM compare, so 0.10.0 is correctly newer than 0.2.0 where a string compare is not.
REM Reached only by `call`.
:guard_directive_version
echo(!ENVY_MIN_DIRECTIVE_VERSION!|findstr /r /x /c:"[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul 2>&1
if errorlevel 1 exit /b 0
echo(!ENVY_VERSION!|findstr /r /x /c:"[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul 2>&1
if errorlevel 1 exit /b 0
for /f "tokens=1-3 delims=." %%x in ("!ENVY_VERSION!") do (
    set "ENVY_A1=%%x" & set "ENVY_A2=%%y" & set "ENVY_A3=%%z"
)
for /f "tokens=1-3 delims=." %%x in ("!ENVY_MIN_DIRECTIVE_VERSION!") do (
    set "ENVY_B1=%%x" & set "ENVY_B2=%%y" & set "ENVY_B3=%%z"
)
set "ENVY_OLD="
if !ENVY_A1! LSS !ENVY_B1! set "ENVY_OLD=1"
if not defined ENVY_OLD if !ENVY_A1! EQU !ENVY_B1! if !ENVY_A2! LSS !ENVY_B2! set "ENVY_OLD=1"
if not defined ENVY_OLD if !ENVY_A1! EQU !ENVY_B1! if !ENVY_A2! EQU !ENVY_B2! if !ENVY_A3! LSS !ENVY_B3! set "ENVY_OLD=1"
if not defined ENVY_OLD exit /b 0
echo ERROR: !ENVY_MANIFEST! uses '@envy cache-local'/'cache-mode'/'state-dir', added in envy >&2
echo        !ENVY_MIN_DIRECTIVE_VERSION!, but resolves envy !ENVY_VERSION!. That envy ignores them >&2
echo        and would silently use the shared cache. Raise or remove the '@envy version' pin. >&2
exit /b 1

REM :check_version -- ENVY_VERSION and ENVY_VERSION_SRC in; clears ENVY_VERSION unless it is numbered
REM MAJOR.MINOR.PATCH, the only shape an envy release takes. Clearing defers to the next
REM tier, ultimately ENVY_FALLBACK_VERSION. A `vlatest/` ENVY_URL 404s, reported as a 403 by a bucket
REM without s3:ListBucket. Reached only by `call`.
REM
REM ENVY_VERSION is delayed-expanded, so an `&` in a mirror's `latest` is data to echo, not a
REM second command.
:check_version
if not defined ENVY_VERSION exit /b 0
echo(!ENVY_VERSION!|findstr /r /x /c:"[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul 2>&1
if not errorlevel 1 exit /b 0
echo WARNING: ignoring implausible envy version '!ENVY_VERSION!' from !ENVY_VERSION_SRC! >&2
set "ENVY_VERSION="
exit /b 0

REM :sha256 -- ENVY_HASH_FILE in, ENVY_HASH_OUT out; empty if no hasher on this box could produce a
REM 64-digit digest. Reached only by `call`, so control never falls into it.
REM
REM certutil first: a System32 binary, so it survives the PowerShell policy lockdowns that
REM motivate the curl.exe/tar.exe preference above. It is also a known LOLBin, so hardened
REM environments sometimes block it -- hence the Get-FileHash fallback, not a hard failure.
:sha256
set "ENVY_HASH_OUT="
where /q certutil.exe && (
    for /f "usebackq skip=1 tokens=*" %%h in (`certutil.exe -hashfile "!ENVY_HASH_FILE!" SHA256 2^>nul`) do (
        if not defined ENVY_HASH_OUT set "ENVY_HASH_OUT=%%h"
    )
)
REM Windows 7/8 certutil grouped the digest into space-separated byte pairs; Win10+ not.
if defined ENVY_HASH_OUT set "ENVY_HASH_OUT=!ENVY_HASH_OUT: =!"
call :sha256_len_ok
if not defined ENVY_HASH_OUT (
    set "ENVY_SAFE_HASH_FILE=!ENVY_HASH_FILE:'=''!"
    for /f "usebackq tokens=*" %%h in (`powershell -NoProfile -Command "try{(Get-FileHash -LiteralPath '!ENVY_SAFE_HASH_FILE!' -Algorithm SHA256).Hash}catch{}" 2^>nul`) do set "ENVY_HASH_OUT=%%h"
    call :sha256_len_ok
)
exit /b 0

REM Discard anything not exactly 64 characters: certutil writes its trailing status line to
REM stdout too, and a localized or error line would be compared against a pin as a digest.
:sha256_len_ok
if not defined ENVY_HASH_OUT exit /b 0
if "!ENVY_HASH_OUT:~63,1!"=="" set "ENVY_HASH_OUT="
if defined ENVY_HASH_OUT if not "!ENVY_HASH_OUT:~64!"=="" set "ENVY_HASH_OUT="
exit /b 0

REM --project, injected ahead of the caller's argv: this script belongs to one project, and
REM the binary must not rediscover a different one from whatever CWD invoked it. take_last
REM on the option side means a hand-typed --project still wins. The trailing dot keeps
REM %~dp0's own backslash off the closing quote.
REM envy sync may rewrite this script; single line ensures cmd.exe never reads past here.
:run
"!ENVY_BIN!" --project "%~dp0." %* & exit /b !ERRORLEVEL!
