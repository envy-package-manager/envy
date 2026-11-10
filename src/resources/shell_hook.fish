# envy shell hook — managed by envy; do not edit
set -g _ENVY_HOOK_STAMP @@ENVY_HOOK_WRITER@@:@@ENVY_RESOURCE_HASH@@

if string match -qi '*utf-8*' -- $LC_ALL $LC_CTYPE $LANG
    set -g _ENVY_UTF8 1
    set -g _ENVY_DASH "—"
else
    set -g _ENVY_UTF8 0
    set -g _ENVY_DASH "--"
end

# One directive's value out of a manifest's header -- blank lines and comments up to the
# first code line, the rule parse_envy_meta and src/resources/envy both apply. No line cap;
# `q` bounds the read. `$vals[-1]`: a repeat resolves to the last, as it does there, and a
# bare substitution would hand the caller a list fish joins with a space inside quotes.
function _envy_header_directive
    set -l vals (sed -nE '
        /^[[:space:]]*$/d
        /^[[:space:]]*--/!q
        s/^[[:space:]]*--[[:space:]]*@envy[[:space:]]+'"$argv[2]"'[[:space:]]+"(([^"\\\\]|\\\\.)*)".*/\\1/p
    ' "$argv[1]")
    test (count $vals) -gt 0; and echo $vals[-1]
end

function _envy_find_manifest
    set -l d $PWD
    while test "$d" != /
        if test -f "$d/envy.lua"
            set -l root_val (_envy_header_directive "$d/envy.lua" root)
            if test "$root_val" != false
                echo "$d"
                return 0
            end
        end
        set d (string replace -r '/[^/]*$' '' "$d")
        test -z "$d"; and set d /
    end
    return 1
end

function _envy_parse_bin
    set -l bin_val (_envy_header_directive "$argv[1]/envy.lua" bin)
    test -n "$bin_val"; and echo "$bin_val"
end

function _envy_set_prompt
    test "$ENVY_SHELL_NO_ICON" = 1; and return
    test "$_ENVY_UTF8" != 1; and return
    test "$_ENVY_PROMPT_ACTIVE" = 1; and return
    if functions -q fish_prompt; and not functions -q _envy_original_fish_prompt
        functions -c fish_prompt _envy_original_fish_prompt
        function fish_prompt
            printf '🦝 '
            _envy_original_fish_prompt
        end
        set -g _ENVY_PROMPT_ACTIVE 1
    end
end

function _envy_unset_prompt
    test "$_ENVY_PROMPT_ACTIVE" != 1; and return
    if functions -q _envy_original_fish_prompt
        functions -e fish_prompt
        functions -c _envy_original_fish_prompt fish_prompt
        functions -e _envy_original_fish_prompt
    end
    set -e _ENVY_PROMPT_ACTIVE
end

# A pin below 0.6.0 resolves an envy with no `activate`; running it just dumps usage.
function _envy_predates_activate
    set -l v (_envy_header_directive "$argv[1]/envy.lua" version)
    string match -qr '^0\.[0-5]\.[0-9]+$' -- "$v"; and test "$v" != 0.0.0  # 0.0.0: a dev build
end

# The project's bin-dir envy prints the activate/deactivate script and its status lines;
# the announce switch goes along even when it is not exported.
function _envy_activate
    test -x "$argv[2]/envy"; or return 1
    _envy_predates_activate "$argv[1]"; and return 1
    set -l out (ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE="$ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE" "$argv[2]/envy" activate fish); or return 1
    test (count $out) -gt 0; or return 1
    printf '%s\n' $out | source
    set -gx ENVY_ACTIVATE_KEY "$argv[1]"
end

function _envy_deactivate
    set -l out
    if test -x "$ENVY_ACTIVATE_EXE"; and set out (ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE="$ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE" "$ENVY_ACTIVATE_EXE" deactivate fish)
        printf '%s\n' $out | source
    else
        echo "envy: cannot restore the environment ($ENVY_ACTIVATE_EXE is gone); restart your shell" >&2
        for v in (set -n | string match 'ENVY_ACTIVATE_*')
            set -e $v
        end
    end
    set -e ENVY_ACTIVATE_KEY
end

# A project pinned to an envy without `activate` just gets its bin dir on PATH.
function _envy_legacy_enter
    if test "$argv[2]" != "$_ENVY_BIN_DIR"
        set -gx PATH "$argv[2]" $PATH
        set -g _ENVY_BIN_DIR "$argv[2]"
        test "$ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE" != 1; and echo "envy: entering "(string replace -r '.*/' '' "$argv[1]")" $_ENVY_DASH tools added to PATH" >&2
    end
    set -gx ENVY_PROJECT_ROOT "$argv[1]"
end

function _envy_legacy_leave
    test "$ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE" != 1; and echo "envy: leaving "(string replace -r '.*/' '' "$ENVY_PROJECT_ROOT")" $_ENVY_DASH PATH restored" >&2
    set -l idx (contains -i -- "$_ENVY_BIN_DIR" $PATH)
    if test -n "$idx"
        set -e PATH[$idx]
    end
    set -e _ENVY_BIN_DIR
    set -e ENVY_PROJECT_ROOT
end

# $argv[1] = refresh: `envy install` bumped the generation file, so re-activate in place.
function _envy_hook --on-variable PWD
    test "$ENVY_SHELL_HOOK_DISABLE" = 1; and return
    set -l refresh (test "$argv[1]" = refresh; and echo 1)
    # --on-variable PWD also fires from a cd in a command substitution.
    test -z "$refresh"; and test "$PWD" = "$_ENVY_LAST_PWD"; and return
    set -g _ENVY_LAST_PWD "$PWD"

    set -l manifest_dir (_envy_find_manifest 2>/dev/null)
    set -l bin_dir
    if test -n "$manifest_dir"
        set -l bin_val (_envy_parse_bin "$manifest_dir")
        if test -n "$bin_val"
            set bin_dir (realpath "$manifest_dir/$bin_val" 2>/dev/null)
        end
    end

    if test -n "$bin_dir"
        if test -n "$refresh"; or begin; test "$manifest_dir" != "$ENVY_ACTIVATE_KEY"; and test "$bin_dir" != "$_ENVY_BIN_DIR"; end
            set -q _ENVY_BIN_DIR; and _envy_legacy_leave
            if not _envy_activate "$manifest_dir" "$bin_dir"
                set -q ENVY_ACTIVATE_KEY; and _envy_deactivate
                _envy_legacy_enter "$manifest_dir" "$bin_dir"
            end
        end
        _envy_set_prompt
        return
    end

    # Left all projects or no bin -- clean up
    set -q ENVY_ACTIVATE_KEY; and _envy_deactivate
    set -q _ENVY_BIN_DIR; and _envy_legacy_leave
    _envy_unset_prompt
end

# Per prompt, no fork: re-activate when `envy install` bumped this project's generation.
function _envy_refresh --on-event fish_prompt
    set -q ENVY_ACTIVATE_KEY; and test -f "$ENVY_ACTIVATE_GENFILE"; or return
    read -l gen < "$ENVY_ACTIVATE_GENFILE"
    test -z "$gen"; and set gen 0
    test "$gen" != "$ENVY_ACTIVATE_GEN"; and _envy_hook refresh
end

_envy_hook
