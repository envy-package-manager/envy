# envy shell hook — managed by envy; do not edit
_ENVY_HOOK_STAMP=@@ENVY_HOOK_WRITER@@:@@ENVY_RESOURCE_HASH@@

case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
  *[Uu][Tt][Ff]-8*|*[Uu][Tt][Ff]8*) _ENVY_UTF8=1; _ENVY_DASH="—" ;;
  *) _ENVY_UTF8=; _ENVY_DASH="--" ;;
esac

# One directive's value out of a manifest's header -- blank lines and comments up to the
# first code line, the rule parse_envy_meta and src/resources/envy both apply. No line cap.
# Read whole: a repeated directive resolves to the last, as it does there.
_envy_header_directive() {
  local line found=""
  local re='^[[:space:]]*--[[:space:]]*@envy[[:space:]]+'"$2"'[[:space:]]+"(([^"\\]|\\.)*)"'
  while IFS= read -r line || [ -n "$line" ]; do  # `||`: no final newline
    if [[ "$line" =~ ^[[:space:]]*$ ]]; then continue; fi
    if [[ ! "$line" =~ ^[[:space:]]*-- ]]; then break; fi
    if [[ "$line" =~ $re ]]; then found="${BASH_REMATCH[1]}"; fi
  done < "$1"
  printf '%s\n' "$found"
}

_envy_find_manifest() {
  local d="$PWD"
  while [ "$d" != / ]; do
    if [ -f "$d/envy.lua" ]; then
      if [ "$(_envy_header_directive "$d/envy.lua" root)" != "false" ]; then
        echo "$d"
        return 0
      fi
    fi
    d="${d%/*}"
    d="${d:-/}"
  done
  return 1
}

_envy_parse_bin() {
  _envy_header_directive "$1/envy.lua" bin
}

_envy_remove_from_path() {
  local remove="$1"
  local IFS=':'
  local new_path=""
  # shellcheck disable=SC2086
  for p in $PATH; do
    if [ "$p" = "$remove" ]; then continue; fi
    new_path="${new_path:+$new_path:}$p"
  done
  echo "$new_path"
}

_envy_set_prompt() {
  if [ "${ENVY_SHELL_NO_ICON:-}" = "1" ]; then return; fi
  if [ "${_ENVY_UTF8:-}" != "1" ]; then return; fi
  if [ "${_ENVY_PROMPT_ACTIVE:-}" = "1" ]; then return; fi
  _ENVY_ORIG_PS1="$PS1"
  PS1="🦝 $PS1"
  _ENVY_PROMPT_ACTIVE=1
}

_envy_unset_prompt() {
  if [ "${_ENVY_PROMPT_ACTIVE:-}" != "1" ]; then return; fi
  PS1="$_ENVY_ORIG_PS1"
  unset _ENVY_ORIG_PS1
  unset _ENVY_PROMPT_ACTIVE
}

# A pin below 0.6.0 resolves an envy with no `activate`; running it just dumps usage.
_envy_predates_activate() {
  local v
  v=$(_envy_header_directive "$1/envy.lua" version)
  [[ "$v" =~ ^0\.[0-5]\.[0-9]+$ && "$v" != 0.0.0 ]]  # 0.0.0: a dev build
}

# The project's bin-dir envy prints the activate/deactivate script and its status lines.
# Locals on the eval's stack are ENVY_-prefixed: scoping is dynamic and ENV can't use one.
_envy_activate() {
  local ENVY_HOOK_OUT
  if [ ! -x "$2/envy" ] || _envy_predates_activate "$1"; then return 1; fi
  if ! ENVY_HOOK_OUT=$(ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE="${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" \
                       "$2/envy" activate bash) || [ -z "$ENVY_HOOK_OUT" ]; then
    return 1
  fi
  eval "$ENVY_HOOK_OUT"
  ENVY_ACTIVATE_KEY="$1"
  export ENVY_ACTIVATE_KEY
}

_envy_deactivate() {
  local ENVY_HOOK_OUT
  if [ -x "${ENVY_ACTIVATE_EXE:-}" ] &&
     ENVY_HOOK_OUT=$(ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE="${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" \
                     "$ENVY_ACTIVATE_EXE" deactivate bash); then
    eval "$ENVY_HOOK_OUT"
  else
    printf 'envy: cannot restore the environment (%s is gone); restart your shell\n' \
      "${ENVY_ACTIVATE_EXE:-envy}" >&2
    unset $(compgen -v ENVY_ACTIVATE_)
  fi
  unset ENVY_ACTIVATE_KEY
}

# A project pinned to an envy without `activate` just gets its bin dir on PATH.
_envy_legacy_enter() {
  if [ "$2" != "${_ENVY_BIN_DIR:-}" ]; then
    PATH="$2:$PATH"
    export PATH
    _ENVY_BIN_DIR="$2"
    if [ "${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" != "1" ]; then
      printf 'envy: entering %s %s tools added to PATH\n' "${1##*/}" "$_ENVY_DASH" >&2
    fi
  fi
  ENVY_PROJECT_ROOT="$1"
  export ENVY_PROJECT_ROOT
}

_envy_legacy_leave() {
  if [ "${ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE:-}" != "1" ]; then
    printf 'envy: leaving %s %s PATH restored\n' "${ENVY_PROJECT_ROOT##*/}" "$_ENVY_DASH" >&2
  fi
  PATH=$(_envy_remove_from_path "$_ENVY_BIN_DIR")
  export PATH
  unset _ENVY_BIN_DIR
  unset ENVY_PROJECT_ROOT
}

_envy_hook() {
  if [ "${ENVY_SHELL_HOOK_DISABLE:-}" = "1" ]; then return; fi

  # `envy install` bumps the project's generation file; the next prompt re-activates.
  local ENVY_HOOK_REFRESH="" ENVY_HOOK_GEN=""
  if [ -n "${ENVY_ACTIVATE_KEY:-}" ] && [ -f "${ENVY_ACTIVATE_GENFILE:-}" ]; then
    read -r ENVY_HOOK_GEN < "$ENVY_ACTIVATE_GENFILE" || true  # `||`: no final newline
    if [ "${ENVY_HOOK_GEN:-0}" != "${ENVY_ACTIVATE_GEN:-}" ]; then ENVY_HOOK_REFRESH=1; fi
  fi
  if [ -z "$ENVY_HOOK_REFRESH" ] && [ "$PWD" = "${_ENVY_LAST_PWD:-}" ]; then return; fi
  _ENVY_LAST_PWD="$PWD"

  local ENVY_HOOK_MANIFEST ENVY_HOOK_BIN_VAL ENVY_HOOK_BIN=""
  ENVY_HOOK_MANIFEST=$(_envy_find_manifest 2>/dev/null) || true
  if [ -n "$ENVY_HOOK_MANIFEST" ]; then
    ENVY_HOOK_BIN_VAL=$(_envy_parse_bin "$ENVY_HOOK_MANIFEST")
    if [ -n "$ENVY_HOOK_BIN_VAL" ]; then
      ENVY_HOOK_BIN="$(cd "$ENVY_HOOK_MANIFEST/$ENVY_HOOK_BIN_VAL" 2>/dev/null && pwd)" ||
        ENVY_HOOK_BIN=""
    fi
  fi

  if [ -n "$ENVY_HOOK_BIN" ]; then
    if [ -n "$ENVY_HOOK_REFRESH" ] ||
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

if [[ "${PROMPT_COMMAND:-}" != *"_envy_hook"* ]]; then
  PROMPT_COMMAND="_envy_hook${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
fi

_envy_hook
