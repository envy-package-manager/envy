# envy shell hook — managed by envy; do not edit
$global:_ENVY_HOOK_STAMP = "@@ENVY_HOOK_WRITER@@:@@ENVY_RESOURCE_HASH@@"

# Re-evaluated every prompt (cheap), so flipping the console encoding needs no re-source.
function _envy_detect_utf8 {
    $global:_ENVY_UTF8 = (($env:LC_ALL + $env:LC_CTYPE + $env:LANG) -match '[Uu][Tt][Ff]-?8') -or
        $(try { [Console]::OutputEncoding.WebName -eq 'utf-8' } catch { $false })
    $global:_ENVY_DASH = if ($global:_ENVY_UTF8) { "`u{2014}" } else { "--" }
}
_envy_detect_utf8

# One directive's value out of a manifest's header -- blank lines and comments up to the
# first code line, the rule parse_envy_meta and src/resources/envy both apply. No line cap.
# Read whole: a repeated directive resolves to the last, as it does there.
#
# A StreamReader, not `foreach ($line in Get-Content)`: foreach runs its collection to
# completion first, materializing the whole manifest twice per directory change. Disposed
# explicitly -- foreach need not dispose a ReadLines() enumerator, and a leak denies writes
# to envy.lua until GC. Both call sites pass the absolute path .NET requires.
function _envy_header_directive($manifest, $key) {
    $re = '^\s*--\s*@envy\s+' + $key + '\s+"([^"\\]*(?:\\.[^"\\]*)*)"'
    $found = $null
    $reader = $null
    try {
        $reader = [System.IO.File]::OpenText($manifest)
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line -match '^\s*$') { continue }
            if ($line -notmatch '^\s*--') { break }
            if ($line -match $re) { $found = $Matches[1] }
        }
    } catch {
        return $null
    } finally {
        if ($reader) { $reader.Dispose() }
    }
    return $found
}

function _envy_find_manifest {
    $d = (Get-Location).Path
    while ($d -ne [System.IO.Path]::GetPathRoot($d)) {
        $manifest = Join-Path $d "envy.lua"
        if (Test-Path $manifest -PathType Leaf) {
            if ((_envy_header_directive $manifest "root") -ne "false") { return $d }
        }
        $d = Split-Path $d -Parent
        if (-not $d) { break }
    }
    return $null
}

function _envy_parse_bin($manifestDir) {
    return (_envy_header_directive (Join-Path $manifestDir "envy.lua") "bin")
}

# The project's bin-dir envy prints the activate/deactivate script and its status lines.
# It writes UTF-8; an OEM console code page would mangle every non-ASCII path in it.
function _envy_native_utf8($exe) {
    $saved = [Console]::OutputEncoding
    try {
        try { [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new() } catch {}
        & $exe @args
    } finally {
        try { [Console]::OutputEncoding = $saved } catch {}
    }
}

# A pin below 0.6.0 resolves an envy with no `activate`; running it just dumps usage.
function _envy_predates_activate($manifestDir) {
    $v = _envy_header_directive (Join-Path $manifestDir "envy.lua") "version"
    return ($v -match '^0\.[0-5]\.[0-9]+$' -and $v -ne '0.0.0')  # 0.0.0: a dev build
}

function _envy_activate($manifestDir, $binDir) {
    $launcher = Join-Path $binDir $(if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'envy.bat' } else { 'envy' })
    if (-not (Test-Path $launcher -PathType Leaf) -or (_envy_predates_activate $manifestDir)) { return $false }
    $out = _envy_native_utf8 $launcher activate powershell
    if ($LASTEXITCODE -ne 0 -or -not $out) { return $false }
    ($out -join "`n") | Invoke-Expression
    $env:ENVY_ACTIVATE_KEY = $manifestDir
    return $true
}

function _envy_deactivate {
    $exe = $env:ENVY_ACTIVATE_EXE
    $out = $null
    if ($exe -and (Test-Path $exe -PathType Leaf)) { $out = _envy_native_utf8 $exe deactivate powershell }
    if ($exe -and $LASTEXITCODE -eq 0 -and $null -ne $out) {
        ($out -join "`n") | Invoke-Expression
    } else {
        [Console]::Error.WriteLine("envy: cannot restore the environment ($exe is gone); restart your shell")
        Get-ChildItem Env:ENVY_ACTIVATE_* | Remove-Item -ErrorAction SilentlyContinue
    }
    Remove-Item Env:\ENVY_ACTIVATE_KEY -ErrorAction SilentlyContinue
}

# A project pinned to an envy without `activate` just gets its bin dir on PATH.
function _envy_legacy_enter($manifestDir, $binDir) {
    $sep = [System.IO.Path]::PathSeparator
    if ($binDir -ne $global:_ENVY_BIN_DIR) {
        $env:PATH = "$binDir$sep$env:PATH"
        $global:_ENVY_BIN_DIR = $binDir
        if ($env:ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE -ne "1") {
            $newName = Split-Path $manifestDir -Leaf
            [Console]::Error.WriteLine("envy: entering $newName $($global:_ENVY_DASH) tools added to PATH")
        }
    }
    $env:ENVY_PROJECT_ROOT = $manifestDir
}

function _envy_legacy_leave {
    $sep = [System.IO.Path]::PathSeparator
    if ($env:ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE -ne "1") {
        $oldName = Split-Path $env:ENVY_PROJECT_ROOT -Leaf
        [Console]::Error.WriteLine("envy: leaving $oldName $($global:_ENVY_DASH) PATH restored")
    }
    $parts = $env:PATH -split [regex]::Escape($sep)
    $parts = $parts | Where-Object { $_ -ne $global:_ENVY_BIN_DIR }
    $env:PATH = $parts -join $sep
    $global:_ENVY_BIN_DIR = $null
    Remove-Item Env:\ENVY_PROJECT_ROOT -ErrorAction SilentlyContinue
}

function _envy_hook {
    if ($env:ENVY_SHELL_HOOK_DISABLE -eq "1") { return }

    _envy_detect_utf8

    # `envy install` bumps the project's generation file; the next prompt re-activates.
    $refresh = $false
    if ($env:ENVY_ACTIVATE_KEY -and $env:ENVY_ACTIVATE_GENFILE -and
        (Test-Path $env:ENVY_ACTIVATE_GENFILE -PathType Leaf)) {
        $gen = (Get-Content -LiteralPath $env:ENVY_ACTIVATE_GENFILE -TotalCount 1 -ErrorAction SilentlyContinue)
        if (-not $gen) { $gen = "0" }
        $refresh = ($gen -ne $env:ENVY_ACTIVATE_GEN)
    }

    $currentDir = (Get-Location).Path
    if (-not $refresh -and $currentDir -eq $global:_ENVY_LAST_PWD) { return }
    $global:_ENVY_LAST_PWD = $currentDir

    $binDir = $null
    $manifestDir = _envy_find_manifest
    if ($manifestDir) {
        $binVal = _envy_parse_bin $manifestDir
        if ($binVal) {
            $candidate = Join-Path $manifestDir $binVal
            if (Test-Path $candidate -PathType Container) { $binDir = (Resolve-Path $candidate).Path }
        }
    }

    if ($binDir) {
        if ($refresh -or ($manifestDir -ne $env:ENVY_ACTIVATE_KEY -and $binDir -ne $global:_ENVY_BIN_DIR)) {
            if ($global:_ENVY_BIN_DIR) { _envy_legacy_leave }
            if (-not (_envy_activate $manifestDir $binDir)) {
                if ($env:ENVY_ACTIVATE_KEY) { _envy_deactivate }
                _envy_legacy_enter $manifestDir $binDir
            }
            # One-time nudge: the icon is wanted but the console can't render it.
            if (-not $global:_ENVY_UTF8 -and
                $env:ENVY_SHELL_NO_ICON -ne "1" -and
                -not $global:_ENVY_UTF8_HINTED) {
                $global:_ENVY_UTF8_HINTED = $true
                [Console]::Error.WriteLine("envy: raccoon icon hidden -- console is not UTF-8. Add [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new() above the hook line in your profile, or set ENVY_SHELL_NO_ICON=1 to silence.")
            }
        }
        $global:_ENVY_PROMPT_ACTIVE = $true
        return
    }

    # Left all projects or no bin -- clean up
    if ($env:ENVY_ACTIVATE_KEY) { _envy_deactivate }
    if ($global:_ENVY_BIN_DIR) { _envy_legacy_leave }
    $global:_ENVY_PROMPT_ACTIVE = $false
}

$global:_ENVY_LAST_PWD = $null
$global:_ENVY_BIN_DIR = $null
$global:_ENVY_PROMPT_ACTIVE = $false
$global:_ENVY_UTF8_HINTED = $false

if (-not (Test-Path Function:\global:_envy_original_prompt)) {
    Copy-Item Function:\prompt Function:\global:_envy_original_prompt
    function global:prompt {
        _envy_hook
        if ($global:_ENVY_PROMPT_ACTIVE -and $global:_ENVY_UTF8 -and $env:ENVY_SHELL_NO_ICON -ne "1") {
            Write-Host "`u{1F99D} " -NoNewline
        }
        _envy_original_prompt
    }
}

_envy_hook
