# envy shell hook — managed by envy; do not edit
_ENVY_HOOK_VERSION=@@ENVY_HOOK_VERSION@@

case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
  *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) _ENVY_UTF8=1; _ENVY_DASH="—" ;;
  *) _ENVY_UTF8=; _ENVY_DASH="--" ;;
esac

_ENVY_PROMPT_PREFIX="%{🦝%2G%} "  # %2G: declare the width, don't let zsh measure it

# p10k custom segment — called by p10k if registered; harmless when p10k absent.
prompt_envy() {
  emulate -L zsh
  [[ "${_ENVY_PROMPT_ACTIVE:-}" = "1" ]] || return
  p10k segment -f 208 -t '🦝'
}

# One directive's value into REPLY -- blank lines and comments up to the first code line,
# the rule parse_envy_meta and src/resources/envy both apply. No line cap. Read whole: a
# repeated directive resolves to the last, as it does there. Pure zsh read+regex, no
# head|grep fork per directory; REPLY avoids a $() subshell at the call site.
_envy_header_directive() {
  emulate -L zsh
  REPLY=""
  local line="" re=""  # explicit: NO_TYPESET_SILENT would print a prior value
  re='^[[:space:]]*--[[:space:]]*@envy[[:space:]]+'"$2"'[[:space:]]+"(([^"\\]|\\.)*)"'
  while IFS= read -r line || [[ -n "$line" ]]; do  # `||`: no final newline
    if [[ "$line" =~ '^[[:space:]]*$' ]]; then continue; fi
    if [[ ! "$line" =~ '^[[:space:]]*--' ]]; then break; fi
    if [[ "$line" =~ $re ]]; then REPLY="${match[1]}"; fi
  done < "$1"
}

_envy_find_manifest() {
  emulate -L zsh
  local d="$PWD"
  while [ "$d" != / ]; do
    if [ -f "$d/envy.lua" ]; then
      _envy_header_directive "$d/envy.lua" root
      if [ "$REPLY" != "false" ]; then
        REPLY="$d"
        return 0
      fi
    fi
    d="${d%/*}"
    d="${d:-/}"
  done
  REPLY=""
  return 1
}

_envy_parse_bin() {
  emulate -L zsh
  _envy_header_directive "$1/envy.lua" bin
}

_envy_remove_from_path() {
  emulate -L zsh
  path=("${(@)path:#${(b)1}}")  # in-place; zsh auto-syncs $path to $PATH
}

_envy_set_prompt() {
  emulate -L zsh
  if [ "${ENVY_SHELL_NO_ICON:-}" = "1" ]; then return; fi
  if [ "${_ENVY_UTF8:-}" != "1" ]; then return; fi
  if [ "${_ENVY_PROMPT_ACTIVE:-}" = "1" ]; then return; fi
  _ENVY_PROMPT_ACTIVE=1
  (( ${+functions[p10k]} )) && return  # p10k renders via the prompt_envy() segment
  if [[ "$PROMPT" != *"${_ENVY_PROMPT_PREFIX}"* ]]; then
    PROMPT="${_ENVY_PROMPT_PREFIX}${PROMPT}"
  fi
}

# Every icon goes, wherever it sits: another decorator may sit ahead of one.
_envy_unset_prompt() {
  emulate -L zsh
  if [ "${_ENVY_PROMPT_ACTIVE:-}" != "1" ]; then return; fi
  if ! (( ${+functions[p10k]} )); then
    PROMPT="${PROMPT//"${_ENVY_PROMPT_PREFIX}"/}"
  fi
  unset _ENVY_PROMPT_ACTIVE
}

# Re-applies the icon if a theme overwrote PROMPT. Presence, not position: iTerm2/VS Code
# precmds re-prepend a mark ahead of it, which an anchored test reads as "icon gone".
_envy_precmd() {
  emulate -L zsh
  if [ "${ENVY_SHELL_NO_ICON:-}" = "1" ] || [ "${_ENVY_UTF8:-}" != "1" ]; then
    _envy_unset_prompt
    return
  fi
  if [ "${_ENVY_PROMPT_ACTIVE:-}" != "1" ]; then return; fi
  (( ${+functions[p10k]} )) && return  # p10k segment needs no PROMPT fixup
  if [[ "$PROMPT" != *"${_ENVY_PROMPT_PREFIX}"* ]]; then
    PROMPT="${_ENVY_PROMPT_PREFIX}${PROMPT}"
    # Another precmd overwrote PROMPT — run last next time
    if [[ "${precmd_functions[-1]}" != "_envy_precmd" ]]; then
      precmd_functions=("${(@)precmd_functions:#_envy_precmd}" _envy_precmd)
    fi
  fi
}

_envy_hook() {
  emulate -L zsh
  if [ "${ENVY_SHELL_HOOK_DISABLE:-}" = "1" ]; then return; fi
  if [ -n "${_ENVY_HOOK_ACTIVE:-}" ]; then return; fi  # chpwd recursion via $(cd ...)
  local _ENVY_HOOK_ACTIVE=1

  local manifest_dir bin_val bin_dir
  if _envy_find_manifest 2>/dev/null; then
    manifest_dir="$REPLY"
    _envy_parse_bin "$manifest_dir"
    bin_val="$REPLY"
    if [ -n "$bin_val" ]; then
      bin_dir="${manifest_dir}/${bin_val}"
      bin_dir="${bin_dir:A}"  # realpath via :A, no subshell
      if [ -d "$bin_dir" ]; then
        if [ "$bin_dir" != "${_ENVY_BIN_DIR:-}" ]; then
          # Leaving the old project (switching)?
          if [ -n "${_ENVY_BIN_DIR:-}" ]; then
            if [ "${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" != "1" ]; then
              printf 'envy: leaving %s %s PATH restored\n' "${ENVY_PROJECT_ROOT##*/}" "$_ENVY_DASH" >&2
            fi
            _envy_remove_from_path "$_ENVY_BIN_DIR"
          fi
          path=("$bin_dir" "${path[@]}")  # prepend via zsh path array — no subshell
          export PATH
          _ENVY_BIN_DIR="$bin_dir"
          if [ "${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" != "1" ]; then
            printf 'envy: entering %s %s tools added to PATH\n' "${manifest_dir##*/}" "$_ENVY_DASH" >&2
          fi
          _envy_set_prompt
        fi
        ENVY_PROJECT_ROOT="$manifest_dir"
        export ENVY_PROJECT_ROOT
        return
      fi
    fi
  fi

  # Left all projects or no bin — clean up
  if [ -n "${_ENVY_BIN_DIR:-}" ]; then
    if [ "${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" != "1" ]; then
      printf 'envy: leaving %s %s PATH restored\n' "${ENVY_PROJECT_ROOT##*/}" "$_ENVY_DASH" >&2
    fi
    _envy_remove_from_path "$_ENVY_BIN_DIR"
    export PATH
    unset _ENVY_BIN_DIR
    _envy_unset_prompt
  fi
  unset ENVY_PROJECT_ROOT
}

# chpwd fires only on a directory change; cheaper than precmd.
if [[ -z "${chpwd_functions[(r)_envy_hook]}" ]]; then
  chpwd_functions+=(_envy_hook)
fi
if [[ -z "${precmd_functions[(r)_envy_precmd]}" ]]; then
  precmd_functions+=(_envy_precmd)
fi

if [ "${ENVY_SHELL_NO_ICON:-}" != "1" ] && [ "${_ENVY_UTF8:-}" = "1" ] && \
   (( ${+functions[p10k]} )) && [[ -n "${POWERLEVEL9K_LEFT_PROMPT_ELEMENTS+x}" ]]; then
  if [[ -z "${POWERLEVEL9K_LEFT_PROMPT_ELEMENTS[(r)envy]}" ]]; then
    POWERLEVEL9K_LEFT_PROMPT_ELEMENTS=(envy "${POWERLEVEL9K_LEFT_PROMPT_ELEMENTS[@]}")
    p10k reload 2>/dev/null || true
  fi
fi

_envy_hook
