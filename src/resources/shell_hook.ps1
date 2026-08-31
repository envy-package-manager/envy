# envy shell hook — managed by envy; do not edit
$global:_ENVY_HOOK_VERSION = @@ENVY_HOOK_VERSION@@

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

function _envy_hook {
    if ($env:ENVY_SHELL_HOOK_DISABLE -eq "1") { return }

    _envy_detect_utf8

    $currentDir = (Get-Location).Path
    if ($currentDir -eq $global:_ENVY_LAST_PWD) { return }
    $global:_ENVY_LAST_PWD = $currentDir

    $sep = [System.IO.Path]::PathSeparator

    $manifestDir = _envy_find_manifest
    if ($manifestDir) {
        $binVal = _envy_parse_bin $manifestDir
        if ($binVal) {
            $binDir = Join-Path $manifestDir $binVal
            if (Test-Path $binDir -PathType Container) {
                $binDir = (Resolve-Path $binDir).Path
                if ($binDir -ne $global:_ENVY_BIN_DIR) {
                    # Leaving the old project (switching)?
                    if ($global:_ENVY_BIN_DIR) {
                        $oldName = Split-Path $env:ENVY_PROJECT_ROOT -Leaf
                        if ($env:ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE -ne "1") {
                            [Console]::Error.WriteLine("envy: leaving $oldName $($global:_ENVY_DASH) PATH restored")
                        }
                        $parts = $env:PATH -split [regex]::Escape($sep)
                        $parts = $parts | Where-Object { $_ -ne $global:_ENVY_BIN_DIR }
                        $env:PATH = $parts -join $sep
                    }
                    $env:PATH = "$binDir$sep$env:PATH"
                    $global:_ENVY_BIN_DIR = $binDir
                    $newName = Split-Path $manifestDir -Leaf
                    if ($env:ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE -ne "1") {
                        [Console]::Error.WriteLine("envy: entering $newName $($global:_ENVY_DASH) tools added to PATH")
                    }
                    $global:_ENVY_PROMPT_ACTIVE = $true
                    # One-time nudge: the icon is wanted but the console can't render it.
                    if (-not $global:_ENVY_UTF8 -and
                        $env:ENVY_SHELL_NO_ICON -ne "1" -and
                        -not $global:_ENVY_UTF8_HINTED) {
                        $global:_ENVY_UTF8_HINTED = $true
                        [Console]::Error.WriteLine("envy: raccoon icon hidden -- console is not UTF-8. Add [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new() above the hook line in your profile, or set ENVY_SHELL_NO_ICON=1 to silence.")
                    }
                }
                $env:ENVY_PROJECT_ROOT = $manifestDir
                return
            }
        }
    }

    # Left all projects or no bin — clean up
    if ($global:_ENVY_BIN_DIR) {
        $oldName = Split-Path $env:ENVY_PROJECT_ROOT -Leaf
        if ($env:ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE -ne "1") {
            [Console]::Error.WriteLine("envy: leaving $oldName $($global:_ENVY_DASH) PATH restored")
        }
        $parts = $env:PATH -split [regex]::Escape($sep)
        $parts = $parts | Where-Object { $_ -ne $global:_ENVY_BIN_DIR }
        $env:PATH = $parts -join $sep
        $global:_ENVY_BIN_DIR = $null
        $global:_ENVY_PROMPT_ACTIVE = $false
    }
    Remove-Item Env:\ENVY_PROJECT_ROOT -ErrorAction SilentlyContinue
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
