# envy shell hook — managed by envy; do not edit
_ENVY_HOOK_STAMP=@@ENVY_HOOK_WRITER@@:@@ENVY_RESOURCE_HASH@@

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
  _envy_refresh
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

# A pin below 0.6.0 resolves an envy with no `activate`; running it just dumps usage.
_envy_predates_activate() {
  emulate -L zsh
  _envy_header_directive "$1/envy.lua" version
  [[ "$REPLY" =~ '^0\.[0-5]\.[0-9]+$' && "$REPLY" != 0.0.0 ]]  # 0.0.0: a dev build
}

# The project's bin-dir envy prints the activate/deactivate script and its status lines.
# Locals on the eval's stack are ENVY_-prefixed: scoping is dynamic and ENV can't use one.
_envy_activate() {
  emulate -L zsh
  local ENVY_HOOK_OUT=""
  if [[ ! -x "$2/envy" ]] || _envy_predates_activate "$1"; then return 1; fi
  ENVY_HOOK_OUT="$(ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE="${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" \
                   "$2/envy" activate zsh)" || return 1
  [[ -n "$ENVY_HOOK_OUT" ]] || return 1
  eval "$ENVY_HOOK_OUT"
  export ENVY_ACTIVATE_KEY="$1"
}

_envy_deactivate() {
  emulate -L zsh
  local ENVY_HOOK_OUT=""
  if [[ -x "${ENVY_ACTIVATE_EXE:-}" ]] &&
     ENVY_HOOK_OUT="$(ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE="${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" \
                      "$ENVY_ACTIVATE_EXE" deactivate zsh)"; then
    eval "$ENVY_HOOK_OUT"
  else
    printf 'envy: cannot restore the environment (%s is gone); restart your shell\n' \
      "${ENVY_ACTIVATE_EXE:-envy}" >&2
    unset -m 'ENVY_ACTIVATE_*'
  fi
  unset ENVY_ACTIVATE_KEY
}

# A project pinned to an envy without `activate` just gets its bin dir on PATH.
_envy_legacy_enter() {
  emulate -L zsh
  if [ "$2" != "${_ENVY_BIN_DIR:-}" ]; then
    path=("$2" "${path[@]}")  # prepend via zsh path array -- no subshell
    export PATH
    _ENVY_BIN_DIR="$2"
    if [ "${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" != "1" ]; then
      printf 'envy: entering %s %s tools added to PATH\n' "${1##*/}" "$_ENVY_DASH" >&2
    fi
  fi
  export ENVY_PROJECT_ROOT="$1"
}

_envy_legacy_leave() {
  emulate -L zsh
  if [ "${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" != "1" ]; then
    printf 'envy: leaving %s %s PATH restored\n' "${ENVY_PROJECT_ROOT##*/}" "$_ENVY_DASH" >&2
  fi
  _envy_remove_from_path "$_ENVY_BIN_DIR"
  export PATH
  unset _ENVY_BIN_DIR
  unset ENVY_PROJECT_ROOT
}

# $1 = "refresh": `envy install` bumped the generation file, so re-activate in place.
_envy_hook() {
  emulate -L zsh
  if [ "${ENVY_SHELL_HOOK_DISABLE:-}" = "1" ]; then return; fi
  if [ -n "${ENVY_HOOK_ACTIVE:-}" ]; then return; fi  # chpwd recursion via $(cd ...)
  local ENVY_HOOK_ACTIVE=1

  local ENVY_HOOK_MANIFEST="" ENVY_HOOK_BIN_VAL="" ENVY_HOOK_BIN=""
  if _envy_find_manifest 2>/dev/null; then
    ENVY_HOOK_MANIFEST="$REPLY"
    _envy_parse_bin "$ENVY_HOOK_MANIFEST"
    ENVY_HOOK_BIN_VAL="$REPLY"
    if [ -n "$ENVY_HOOK_BIN_VAL" ]; then
      ENVY_HOOK_BIN="${ENVY_HOOK_MANIFEST}/${ENVY_HOOK_BIN_VAL}"
      ENVY_HOOK_BIN="${ENVY_HOOK_BIN:A}"  # realpath via :A, no subshell
      [ -d "$ENVY_HOOK_BIN" ] || ENVY_HOOK_BIN=""
    fi
  fi

  if [ -n "$ENVY_HOOK_BIN" ]; then
    if [ "${1:-}" = "refresh" ] ||
       { [ "$ENVY_HOOK_MANIFEST" != "${ENVY_ACTIVATE_KEY:-}" ] &&
         [ "$ENVY_HOOK_BIN" != "${_ENVY_BIN_DIR:-}" ]; }; then
      if [ -n "${_ENVY_BIN_DIR:-}" ]; then _envy_legacy_leave; fi
      if ! _envy_activate "$ENVY_HOOK_MANIFEST" "$ENVY_HOOK_BIN"; then
        if [ -n "${ENVY_ACTIVATE_KEY:-}" ]; then _envy_deactivate; fi
        _envy_legacy_enter "$ENVY_HOOK_MANIFEST" "$ENVY_HOOK_BIN"
      fi
    fi
    _envy_set_prompt
    return
  fi

  # Left all projects or no bin -- clean up
  if [ -n "${ENVY_ACTIVATE_KEY:-}" ]; then _envy_deactivate; fi
  if [ -n "${_ENVY_BIN_DIR:-}" ]; then _envy_legacy_leave; fi
  _envy_unset_prompt
}

# Per prompt, no fork: re-activate when `envy install` bumped this project's generation.
_envy_refresh() {
  emulate -L zsh
  [ -n "${ENVY_ACTIVATE_KEY:-}" ] && [ -f "${ENVY_ACTIVATE_GENFILE:-}" ] || return 0
  local ENVY_HOOK_GEN=""
  read -r ENVY_HOOK_GEN < "$ENVY_ACTIVATE_GENFILE" || :  # `||`: no final newline
  if [ "${ENVY_HOOK_GEN:-0}" != "${ENVY_ACTIVATE_GEN:-}" ]; then _envy_hook refresh; fi
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
