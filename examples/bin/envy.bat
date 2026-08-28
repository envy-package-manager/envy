@echo off
REM envy-managed bootstrap script - do not edit
setlocal EnableDelayedExpansion

set "DEFAULT_MIRROR=https://github.com/envy-package-manager/envy/releases/download"
set "LATEST_URL=https://github.com/envy-package-manager/envy/releases/latest"
set "ENV_MIRROR="
if defined ENVY_MIRROR set "ENV_MIRROR=%ENVY_MIRROR%"
set "FALLBACK_VERSION=0.0.17"
set "MIN_DIRECTIVE_VERSION=0.2.0"

set "MANIFEST="
set "CANDIDATE="
set "DIR=%~dp0"
if "!DIR:~-1!"=="\" set "DIR=!DIR:~0,-1!"
:findloop
if exist "!DIR!\envy.lua" (
    call :read_root "!DIR!\envy.lua"
    if "!IS_ROOT!"=="true" (
        set "MANIFEST=!DIR!\envy.lua"
        goto :found
    ) else (
        set "CANDIDATE=!DIR!\envy.lua"
    )
)
REM A repository is a hard boundary, matching discover() in src/manifest.cpp. Without it a
REM `root "false"` project inside a checkout kept walking into the parent tree, and this
REM script and the binary picked different manifests -- so different caches. The trailing
REM backslash makes `if exist` test for a directory, as discover()'s is_directory does.
if exist "!DIR!\.git\" (
    if defined CANDIDATE (
        set "MANIFEST=!CANDIDATE!"
        goto :found
    )
    echo ERROR: envy.lua not found >&2 & exit /b 1
)
for %%I in ("!DIR!\..") do set "PARENT=%%~fI"
if "!PARENT!"=="!DIR!" (
    if defined CANDIDATE (
        set "MANIFEST=!CANDIDATE!"
        goto :found
    )
    echo ERROR: envy.lua not found >&2 & exit /b 1
)
set "DIR=!PARENT!"
goto :findloop
:found

set "VERSION="
set "CACHE_LOCAL="
set "CACHE_MODE="
set "STATE_DIR_REL="
set "MANIFEST_MIRROR="
set "SUMS_PIN="

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
for /f "usebackq tokens=1,2,3,* eol=" %%a in ("!MANIFEST!") do (
    set "TOK=%%a"
    if not "!TOK:~0,2!"=="--" goto :done_parse
    if "%%a"=="--" if "%%b"=="@envy" (
        set "KEY=%%c"
        set "RAW=%%d"
        if defined RAW (
            call :quoted_value
            if defined VAL (
                if "!KEY!"=="version" set "VERSION=!VAL!"
                if "!KEY!"=="cache-local" set "CACHE_LOCAL=!VAL!"
                if "!KEY!"=="cache-mode" set "CACHE_MODE=!VAL!"
                if "!KEY!"=="state-dir" set "STATE_DIR_REL=!VAL!"
                if "!KEY!"=="mirror" set "MANIFEST_MIRROR=!VAL!"
                if "!KEY!"=="sha256sums" set "SUMS_PIN=!VAL!"
            )
        )
    )
)
:done_parse

REM A sums pin names one release's checksum file, so it is meaningless against a resolved
REM or stamped-fallback version. Captured before the resolution chain overwrites VERSION.
set "PINNED_VERSION=!VERSION!"

REM Fail closed before any network: a pin that silently stops verifying is worse than none,
REM since the manifest still advertises attestation.
if defined SUMS_PIN if not defined PINNED_VERSION (
    echo ERROR: '@envy sha256sums' requires '@envy version' in !MANIFEST! >&2
    exit /b 1
)

REM Precedence: ENVY_MIRROR env > @envy mirror directive > envy upstream, matching the
REM runtime resolver in src/reexec.cpp. DEFAULT_MIRROR is always envy's own release URL,
REM never a copy of this project's mirror: deleting the directive must not resolve this
REM script and the re-exec'd binary to different mirrors.
if defined ENV_MIRROR (
    set "ENVY_MIRROR=!ENV_MIRROR!"
) else if defined MANIFEST_MIRROR (
    set "ENVY_MIRROR=!MANIFEST_MIRROR!"
) else (
    set "ENVY_MIRROR=!DEFAULT_MIRROR!"
)

REM A trailing slash would produce ".../releases//v1.2.3/...", a distinct and nonexistent
REM s3:// key.
:striptrail
if "!ENVY_MIRROR:~-1!"=="/" (
    set "ENVY_MIRROR=!ENVY_MIRROR:~0,-1!"
    goto :striptrail
)

set "MIRROR_IS_S3="
if /i "!ENVY_MIRROR:~0,5!"=="s3://" set "MIRROR_IS_S3=1"

REM Probe bare `aws`, not `aws.exe`: PATHEXT also resolves the aws.cmd/aws.bat shims. The
REM curl.exe/tar.exe probes below name the exe deliberately, to stay policy-proof.
if not defined MIRROR_IS_S3 goto :mirror_ok
where /q aws && goto :mirror_ok
echo ERROR: mirror "!ENVY_MIRROR!" is an s3:// URI but the aws CLI was not found on PATH. >&2
echo        Install AWS CLI v2, or use an https:// mirror. >&2
exit /b 1
:mirror_ok

set "MANIFEST_DIR="
for %%I in ("!MANIFEST!") do set "MANIFEST_DIR=%%~dpI"
if "!MANIFEST_DIR:~-1!"=="\" set "MANIFEST_DIR=!MANIFEST_DIR:~0,-1!"

REM Tiers, in order, matching resolve_cache_root() in src/cache.cpp. No expansion of any
REM kind: every value is either a literal from the manifest or a path this script joins, so
REM there is no grammar for this script and the binary to disagree about.
REM An override short-circuits every project tier, and is rejected rather than absolutized:
REM the binary used to anchor a relative one to its own cwd while this script took it
REM verbatim, so one invocation named two different trees. Flat `goto`, not an if/else block,
REM because every comment below would otherwise sit inside parentheses -- where cmd re-parses
REM REM lines and a stray `(`, `&` or `>` in prose silently breaks the block.
if not defined ENVY_CACHE_ROOT goto :resolve_project_cache
set "CACHE=!ENVY_CACHE_ROOT!"
call :require_absolute
if errorlevel 1 exit /b 1
goto :cache_resolved

:resolve_project_cache
REM @envy state-dir, else the manifest's own directory -- never `.envy`, which is inside the
REM default local tree `.envy\cache` and would let a cache wipe erase the marker.
set "STATE_DIR=!MANIFEST_DIR!"
if defined STATE_DIR_REL set "STATE_DIR=!MANIFEST_DIR!\!STATE_DIR_REL!"

REM Existence is the whole signal, so there is no file content for this script, bash and C++
REM to read three different ways. Both markers at once is a state envy never writes.
if exist "!STATE_DIR!\.envy-cache-local" if exist "!STATE_DIR!\.envy-cache-shared" (
    echo ERROR: both .envy-cache-local and .envy-cache-shared exist in !STATE_DIR! >&2
    echo        envy never writes both. Delete one. >&2
    exit /b 1
)

REM Naming a tree is asking for it; a cache-local needing a second directive to take effect
REM would sit in a manifest doing nothing.
set "MODE="
if exist "!STATE_DIR!\.envy-cache-local" set "MODE=local"
if not defined MODE if exist "!STATE_DIR!\.envy-cache-shared" set "MODE=shared"
if not defined MODE if defined CACHE_MODE set "MODE=!CACHE_MODE!"
if not defined MODE if defined CACHE_LOCAL set "MODE=local"
if not defined MODE set "MODE=shared"

if "!MODE!"=="local" goto :cache_local
set "CACHE=!LOCALAPPDATA!\envy"
goto :cache_resolved

:cache_local
REM Anchored to the manifest's directory, never the caller's cwd: one manifest names one tree
REM from every working directory. %%~fI then normalizes, which is what keeps this equal to the
REM binary's lexically_normal/make_preferred -- a cache-local of `out/.envy` joins as
REM `...\out/.envy` until it runs.
set "CACHE=!MANIFEST_DIR!\.envy\cache"
if defined CACHE_LOCAL set "CACHE=!MANIFEST_DIR!\!CACHE_LOCAL!"
for %%I in ("!CACHE!") do set "CACHE=%%~fI"

:cache_resolved

if "!VERSION!"=="" (
    set "LATEST_FILE=!CACHE!\envy\latest"
    if exist "!LATEST_FILE!" (
        set /p LATEST_VER=<"!LATEST_FILE!"
        if defined LATEST_VER (
            if exist "!CACHE!\envy\!LATEST_VER!\envy.exe" set "VERSION=!LATEST_VER!"
        )
    )
)
if not "!VERSION!"=="" goto :version_resolved

REM Ask the mirror first: 'envy mirror-envy' writes a `latest` file at the mirror root, so
REM a private or air-gapped mirror answers for itself.
set "LATEST_TMP=!TEMP!\envy-latest-%RANDOM%%RANDOM%.txt"
set "GOT="
if defined MIRROR_IS_S3 (
    call aws s3 cp --only-show-errors "!ENVY_MIRROR!/latest" "!LATEST_TMP!" >nul 2>&1 && set "GOT=1"
) else (
    where /q curl.exe && (curl.exe -fsSL --connect-timeout 10 --max-time 300 "!ENVY_MIRROR!/latest" -o "!LATEST_TMP!" >nul 2>&1 && set "GOT=1")
)
if not defined GOT goto :latest_cleanup
REM Trim with an unquoted for /f (a literal string, not a filename -- no usebackq), staging
REM through RAW so a whitespace-only file leaves VERSION empty.
set "RAW_VERSION="
set /p RAW_VERSION=<"!LATEST_TMP!"
for /f "tokens=1" %%v in ("!RAW_VERSION!") do set "VERSION=%%v"
set "VERSION_SRC=!ENVY_MIRROR!/latest"
call :check_version
:latest_cleanup
del "!LATEST_TMP!" 2>nul
if not "!VERSION!"=="" goto :version_resolved

REM GitHub serves no `latest` object, so fall back to its redirect. Skipped for s3://
REM mirrors, which are never github.
if defined MIRROR_IS_S3 goto :version_fallback

REM Prefer native curl.exe (policy-resistant); parse the tag from the end of the redirect
REM chain, not hop 1: a repo rename inserts a hop whose last segment is `latest`. To a file
REM behind && rather than a `for /f` backquote: --fail still writes the -w output on an
REM HTTP error. Timeouts bound a blackholed connect.
set "EFF_TMP=!TEMP!\envy-effective-%RANDOM%%RANDOM%.txt"
set "EFFECTIVE="
set "TAG="
set "GOT="
where /q curl.exe && (curl.exe -fsSL -o nul -w "%%{url_effective}" --connect-timeout 5 --max-time 15 "!LATEST_URL!" >"!EFF_TMP!" 2>nul && set "GOT=1")
REM goto, not `if defined GOT set /p ...`: cmd applies the redirection whether or not the
REM `if` body runs, and EFF_TMP is absent when curl.exe is.
if not defined GOT goto :effective_cleanup
set /p EFFECTIVE=<"!EFF_TMP!"
:effective_cleanup
del "!EFF_TMP!" 2>nul
if defined EFFECTIVE set "EFFECTIVE=!EFFECTIVE:/=\!"
if defined EFFECTIVE for %%a in ("!EFFECTIVE!") do set "TAG=%%~nxa"
if defined TAG set "VERSION=!TAG!"
if defined TAG if "!TAG:~0,1!"=="v" set "VERSION=!TAG:~1!"
REM PowerShell fallback for a box without curl.exe. AllowAutoRedirect defaults on, so
REM ResponseUri is the end of the chain.
if "!VERSION!"=="" (
    for /f "tokens=*" %%u in ('powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; try { $resp=[System.Net.WebRequest]::Create('!LATEST_URL!').GetResponse(); $u=$resp.ResponseUri.AbsoluteUri; $resp.Close(); ($u -split '/')[-1] -replace '^v','' } catch {}" 2^>nul') do set "VERSION=%%u"
)
set "VERSION_SRC=!LATEST_URL!"
call :check_version

:version_fallback
if "!VERSION!"=="" set "VERSION=!FALLBACK_VERSION!"
:version_resolved

REM An older envy silently ignores the cache directives and would resolve the *shared* cache
REM for a manifest asking for a hermetic tree, then exit 0. Refuse before downloading it.
REM 0.0.0 is a dev build, which reexec_should() in src/reexec.cpp also lets through: built
REM from a working tree, so its directive support cannot be read off its version. Both shape
REM tests matter -- an unstamped template would otherwise reach `if LSS` with
REM '0.2.0'.
set "USES_NEW_DIRECTIVES="
if defined CACHE_LOCAL set "USES_NEW_DIRECTIVES=1"
if defined CACHE_MODE set "USES_NEW_DIRECTIVES=1"
if defined STATE_DIR_REL set "USES_NEW_DIRECTIVES=1"
if defined USES_NEW_DIRECTIVES if not "!VERSION!"=="0.0.0" call :guard_directive_version
if errorlevel 1 exit /b 1

set "ENVY_BIN=!CACHE!\envy\!VERSION!\envy.exe"
if exist "!ENVY_BIN!" goto :run

set "ARCH=x86_64"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PROCESSOR_ARCHITECTURE 2>nul | findstr /i "ARM64" >nul 2>&1 && set "ARCH=arm64"

echo Downloading envy !VERSION!... >&2
set "ARCHIVE=envy-windows-!ARCH!.zip"
set "URL=!ENVY_MIRROR!/v!VERSION!/!ARCHIVE!"
REM Escape single quotes for PowerShell (replace ' with '')
set "SAFE_URL=!URL:'=''!"
REM Claim a unique temp dir via atomic mkdir (cmd's %RANDOM% can collide across
REM concurrent bootstraps; mkdir succeeds for exactly one owner of a given name).
set /a TEMP_TRIES=0
:mktemp
set "TEMP_DIR=!TEMP!\envy-%RANDOM%%RANDOM%"
mkdir "!TEMP_DIR!" 2>nul && goto :gottemp
set /a TEMP_TRIES+=1
if !TEMP_TRIES! LSS 10 goto :mktemp
echo ERROR: Could not create a temp directory under !TEMP! >&2 & exit /b 1
:gottemp
set "TEMP_ZIP=!TEMP_DIR!.zip"

REM Download: prefer native curl.exe (policy-resistant), fall back to PowerShell.
set "OK="
if defined MIRROR_IS_S3 goto :dl_s3
goto :dl_http

:dl_s3
REM `call` so an aws resolved to a .bat/.cmd shim returns control here and ERRORLEVEL
REM survives. To a file, never piped into tar: cmd takes ERRORLEVEL from the right side of a
REM pipe only, and tar exits 0 on empty input, so a failed download would look like success.
call aws s3 cp --only-show-errors "!URL!" "!TEMP_ZIP!" && set "OK=1"
goto :dl_done

:dl_http
where /q curl.exe && (curl.exe -fsSL "!URL!" -o "!TEMP_ZIP!" && set "OK=1")
if not defined OK (
    powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '!SAFE_URL!' -OutFile '!TEMP_ZIP!' -UseBasicParsing" && set "OK=1"
)

:dl_done
if not defined OK (echo ERROR: Failed to download envy from !URL! >&2 & rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1)

REM Attest before extracting: an unattested archive must never be unpacked, or a hostile
REM mirror chooses the paths written under TEMP_DIR, from which we run envy.
if not defined SUMS_PIN goto :attest_done

set "SUMS_URL=!ENVY_MIRROR!/v!VERSION!/SHA256SUMS"
set "SAFE_SUMS_URL=!SUMS_URL:'=''!"
set "SUMS_FILE=!TEMP_DIR!\SHA256SUMS"
set "OK="
if defined MIRROR_IS_S3 (
    call aws s3 cp --only-show-errors "!SUMS_URL!" "!SUMS_FILE!" && set "OK=1"
) else (
    where /q curl.exe && (curl.exe -fsSL "!SUMS_URL!" -o "!SUMS_FILE!" && set "OK=1")
    if not defined OK (
        powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri '!SAFE_SUMS_URL!' -OutFile '!SUMS_FILE!' -UseBasicParsing" && set "OK=1"
    )
)
if not defined OK (echo ERROR: Failed to download !SUMS_URL! >&2 & rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1)

REM Anchor the chain on the manifest's pin before trusting anything the sums file says.
set "HASH_FILE=!SUMS_FILE!"
call :sha256
if not defined HASH_OUT (echo ERROR: could not compute a SHA256 of !SUMS_FILE! >&2 & rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1)
if /i not "!HASH_OUT!"=="!SUMS_PIN!" (
    echo ERROR: SHA256SUMS does not match the pinned '@envy sha256sums': >&2
    echo        expected !SUMS_PIN! >&2
    echo        got      !HASH_OUT! >&2
    echo        The mirror is serving a different release manifest than !MANIFEST! pinned. >&2
    echo        Update the pin deliberately; do not remove it. >&2
    rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1
)

REM Match this exact archive: keying on the hash alone accepts any platform's binary, and a
REM prefix name match accepts a longer sibling.
set "WANT="
for /f "usebackq tokens=1,2" %%h in ("!SUMS_FILE!") do (
    set "NAME=%%i"
    if "!NAME:~0,1!"=="*" set "NAME=!NAME:~1!"
    if /i "!NAME!"=="!ARCHIVE!" if not defined WANT set "WANT=%%h"
)
if not defined WANT (echo ERROR: SHA256SUMS lists no entry for !ARCHIVE! >&2 & rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1)

set "HASH_FILE=!TEMP_ZIP!"
call :sha256
if not defined HASH_OUT (echo ERROR: could not compute a SHA256 of !TEMP_ZIP! >&2 & rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1)
if /i not "!HASH_OUT!"=="!WANT!" (
    echo ERROR: !ARCHIVE! failed attestation: >&2
    echo        SHA256SUMS says !WANT! >&2
    echo        downloaded      !HASH_OUT! >&2
    rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1
)
:attest_done

REM Extract: prefer native tar.exe (bsdtar reads zip), fall back to Expand-Archive.
set "OK="
where /q tar.exe && (tar.exe -xf "!TEMP_ZIP!" -C "!TEMP_DIR!" && set "OK=1")
if not defined OK (
    powershell -NoProfile -Command "$ProgressPreference='SilentlyContinue'; Expand-Archive -Path '!TEMP_ZIP!' -DestinationPath '!TEMP_DIR!' -Force" && set "OK=1"
)
if not defined OK (echo ERROR: Failed to extract envy >&2 & rmdir /s /q "!TEMP_DIR!" 2>nul & del "!TEMP_ZIP!" 2>nul & exit /b 1)
del "!TEMP_ZIP!" 2>nul
REM tar succeeds on an empty archive, so a zero-length object would fall through to :run
REM and report a missing path instead of a failed download.
if not exist "!TEMP_DIR!\envy.exe" (echo ERROR: archive from !URL! contained no envy binary >&2 & rmdir /s /q "!TEMP_DIR!" 2>nul & exit /b 1)
set "ENVY_BIN=!TEMP_DIR!\envy.exe"
goto :run

REM :read_root -- IS_ROOT out, manifest path in %1. Reads only the manifest header: blank
REM lines and comments, stopping at the first line of code, the same rule parse_envy_meta
REM applies in src/manifest.cpp. `for /f` skips blank lines on its own, so only a code line
REM ends the scan; the default space+tab delims (no override) keep a tab-indented comment's
REM tab out of %%a, and `eol=` clears the default `;` comment character so a `;`-led line
REM ends the header rather than being skipped -- both as in the header scan above. A
REM subroutine because the early-exit `goto` has to land at this scope's top level -- inside
REM the caller's `if exist (...)` block it would tear out of the block and skip the walk's
REM own logic. Reached only by `call`.
:read_root
set "IS_ROOT=true"
for /f "usebackq tokens=1,2,3,4 eol=" %%a in ("%~1") do (
    set "TOK=%%a"
    if not "!TOK:~0,2!"=="--" goto :read_root_done
    if "%%a"=="--" if "%%b"=="@envy" if "%%c"=="root" (
        set "VAL=%%d"
        set "VAL=!VAL:"=!"
        if "!VAL!"=="false" set "IS_ROOT=false"
    )
)
:read_root_done
exit /b 0

REM :quoted_value -- RAW in, VAL out. RAW is the `*` remainder of a directive line, so it
REM starts at the opening quote and may carry a trailing comment. `for /f` skips leading
REM delimiters, so with `"` as the delimiter the value is token *1*, not token 2, and it ends
REM at the closing quote either way -- matching parse_directive_line() in src/manifest.cpp.
REM The old `!VAL:~1,-1!` trimmed one character from each end instead and folded any trailing
REM comment into the value. A `\"` inside a value still splits early --
REM already true of this parser before, and documented in docs/envy-init.md, which is why
REM envy_release_validate_mirror rejects the one directive that could carry one.
REM Options are caret-escaped and unquoted: `delims="` cannot be written inside quotes.
REM Reached only by `call`.
:quoted_value
set "VAL="
for /f tokens^=1^ delims^=^" %%v in ("!RAW!") do set "VAL=%%v"
if defined VAL set "VAL=!VAL:\\=\!"
exit /b 0

REM :require_absolute -- CACHE in; errorlevel 1 when it is not absolute. Rejected rather
REM than absolutized: the binary used to anchor a relative override to its own cwd while
REM this script took it verbatim, so one invocation named two trees. The accepted forms are
REM the ones std::filesystem calls absolute on Windows. Reached only by `call`.
:require_absolute
if "!CACHE:~0,2!"=="\\" exit /b 0
if "!CACHE:~0,2!"=="//" exit /b 0
if "!CACHE:~1,2!"==":\" exit /b 0
if "!CACHE:~1,2!"==":/" exit /b 0
echo ERROR: ENVY_CACHE_ROOT must be an absolute path: !CACHE! >&2
exit /b 1

REM :guard_directive_version -- VERSION and MIN_DIRECTIVE_VERSION in; errorlevel 1 when the
REM resolved envy predates the cache directives this manifest uses. Field-wise integer
REM compare, so 0.10.0 is correctly newer than 0.2.0 where a string compare is not.
REM Reached only by `call`.
:guard_directive_version
echo(!MIN_DIRECTIVE_VERSION!|findstr /r /x /c:"[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul 2>&1
if errorlevel 1 exit /b 0
echo(!VERSION!|findstr /r /x /c:"[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul 2>&1
if errorlevel 1 exit /b 0
for /f "tokens=1-3 delims=." %%x in ("!VERSION!") do (
    set "_A1=%%x" & set "_A2=%%y" & set "_A3=%%z"
)
for /f "tokens=1-3 delims=." %%x in ("!MIN_DIRECTIVE_VERSION!") do (
    set "_B1=%%x" & set "_B2=%%y" & set "_B3=%%z"
)
set "_OLD="
if !_A1! LSS !_B1! set "_OLD=1"
if not defined _OLD if !_A1! EQU !_B1! if !_A2! LSS !_B2! set "_OLD=1"
if not defined _OLD if !_A1! EQU !_B1! if !_A2! EQU !_B2! if !_A3! LSS !_B3! set "_OLD=1"
if not defined _OLD exit /b 0
echo ERROR: !MANIFEST! uses '@envy cache-local'/'cache-mode'/'state-dir', added in envy >&2
echo        !MIN_DIRECTIVE_VERSION!, but resolves envy !VERSION!. That envy ignores them >&2
echo        and would silently use the shared cache. Raise or remove the '@envy version' pin. >&2
exit /b 1

REM :check_version -- VERSION and VERSION_SRC in; clears VERSION unless it is numbered
REM MAJOR.MINOR.PATCH, the only shape an envy release takes. Clearing defers to the next
REM tier, ultimately FALLBACK_VERSION. A `vlatest/` URL 404s, reported as a 403 by a bucket
REM without s3:ListBucket. Reached only by `call`.
REM
REM VERSION is delayed-expanded, so an `&` in a mirror's `latest` is data to echo, not a
REM second command.
:check_version
if not defined VERSION exit /b 0
echo(!VERSION!|findstr /r /x /c:"[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul 2>&1
if not errorlevel 1 exit /b 0
echo WARNING: ignoring implausible envy version '!VERSION!' from !VERSION_SRC! >&2
set "VERSION="
exit /b 0

REM :sha256 -- HASH_FILE in, HASH_OUT out; empty if no hasher on this box could produce a
REM 64-digit digest. Reached only by `call`, so control never falls into it.
REM
REM certutil first: a System32 binary, so it survives the PowerShell policy lockdowns that
REM motivate the curl.exe/tar.exe preference above. It is also a known LOLBin, so hardened
REM environments sometimes block it -- hence the Get-FileHash fallback, not a hard failure.
:sha256
set "HASH_OUT="
where /q certutil.exe && (
    for /f "usebackq skip=1 tokens=*" %%h in (`certutil.exe -hashfile "!HASH_FILE!" SHA256 2^>nul`) do (
        if not defined HASH_OUT set "HASH_OUT=%%h"
    )
)
REM Windows 7/8 certutil grouped the digest into space-separated byte pairs; Win10+ not.
if defined HASH_OUT set "HASH_OUT=!HASH_OUT: =!"
call :sha256_len_ok
if not defined HASH_OUT (
    set "SAFE_HASH_FILE=!HASH_FILE:'=''!"
    for /f "usebackq tokens=*" %%h in (`powershell -NoProfile -Command "try{(Get-FileHash -LiteralPath '!SAFE_HASH_FILE!' -Algorithm SHA256).Hash}catch{}" 2^>nul`) do set "HASH_OUT=%%h"
    call :sha256_len_ok
)
exit /b 0

REM Discard anything not exactly 64 characters: certutil writes its trailing status line to
REM stdout too, and a localized or error line would be compared against a pin as a digest.
:sha256_len_ok
if not defined HASH_OUT exit /b 0
if "!HASH_OUT:~63,1!"=="" set "HASH_OUT="
if defined HASH_OUT if not "!HASH_OUT:~64!"=="" set "HASH_OUT="
exit /b 0

REM envy sync may rewrite this script; single line ensures cmd.exe never reads past here.
:run
"!ENVY_BIN!" %* & exit /b !ERRORLEVEL!
