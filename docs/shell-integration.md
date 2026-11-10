# Shell Integration

Envy shell hooks activate a project's environment as you `cd` into it and restore your own as you leave. Shell startup runs no envy binary; entering a project runs that project's launcher.

## Setup

Run `envy shell <shell>` to get the source line for your profile:

```bash
# Bash
envy shell bash    # → add to ~/.bashrc

# Zsh
envy shell zsh     # → add to ~/.zshrc

# Fish
envy shell fish    # → add to ~/.config/fish/config.fish

# PowerShell
envy shell powershell  # → add to $PROFILE
```

Hook files live at `<user-wide cache>/shell/hook.{bash,zsh,fish,ps1}` and are written automatically during self-deploy. `envy shell` just prints the `source` line. Under `--cache-root`/`ENVY_CACHE_ROOT` envy warns that moving or deleting that cache will break shell integration.

**Hooks are a user-wide-cache feature.** A profile sources one path for every directory the shell ever visits, so no project tier moves it: the hook root is `--cache-root`/`ENVY_CACHE_ROOT`, else the platform default. A project on its own cache tree (`@envy cache-local`, `envy cache --local`) does not merely resolve elsewhere — it writes **no** hooks at all. A copy inside the project would never be the one the shell loads, and `rm -rf` on the build root would take it; writing to the user-wide tree instead would break the one promise a local cache makes, which is that running the project touches nothing outside it. So a user whose only projects are local has no hooks, and `envy shell` says so rather than suggesting a command that cannot produce them.

## Behavior

On every directory change:

1. Walk up from `$PWD` looking for `envy.lua`
2. Respect `@envy root "false"` (continue upward) vs default `root=true` (stop)
3. Parse `@envy bin` from the manifest header—blank lines and comments up to the first line of code, same rule as the launcher and the runtime
4. Entering a project (or switching to another): eval `<bin>/envy activate <shell>`—the bin dir on `PATH`, `ENVY_PROJECT_ROOT`, and every installed package's `ENV` (see `docs/architecture.md`)
5. Leaving all projects: eval `envy deactivate <shell>`, which restores every variable activation touched

`envy install` bumps a per-project generation file; at the next prompt every shell activated in that project re-activates, picking up what was just installed. The check is a builtin `read`, no fork.

A project pinned to an envy that predates `activate` (its launcher fails) falls back to the old behavior: the bin dir on `PATH` and `ENVY_PROJECT_ROOT`, managed by the hook itself.

Without hooks, the same commands work by hand: `eval "$(envy activate zsh)"`, `eval "$(envy deactivate zsh)"`. Activating always deactivates first, from any project.

## Activation State

Activation is modal and lives in exported variables, so nested shells, tmux panes and IDEs launched from the shell inherit it rather than re-applying it.

| Variable | Meaning |
|---|---|
| `ENVY_ACTIVATE_VARS` | Names activation touched |
| `ENVY_ACTIVATE_ORIG__<NAME>` | `<NAME>`'s value before activation; absent means it was unset |
| `ENVY_ACTIVATE_ROOT` | Activated project (canonical); `ENVY_ACTIVATE_KEY` is the hook's spelling of it |
| `ENVY_ACTIVATE_EXE` | Binary that wrote the state; the hook deactivates with it |
| `ENVY_ACTIVATE_GEN` / `_GENFILE` | Generation in effect / the file `envy install` bumps |
| `ENVY_ACTIVATE_SCHEMA` | State format; a mismatch says to restart the shell |

Restore is wholesale: a variable activation touched goes back to its pre-activate value on leave, including edits you made to it inside the project. Envy never applies activation to its own phases—`envy install` from an activated shell builds exactly what it builds in CI.

## Auto-Update

Hook files carry `_ENVY_HOOK_STAMP=<envy version>:<digest>`—which envy wrote the file, and a digest of the hook's own content. Any envy command rewrites a hook that differs from its own copy, so a hook edit always ships; it leaves a hook a *newer* envy wrote alone, so a version-pinned project and a newer envy sharing the cache don't trade writes. Restart your shell after an update.

## Environment Variables

| Variable | Effect |
|---|---|
| `ENVY_SHELL_HOOK_DISABLE=1` | Disable the hook entirely—no activation, no messages, no icon |
| `ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1` | Suppress "entering/leaving" status messages; activation and icon still work |
| `ENVY_SHELL_NO_ICON=1` | Suppress the raccoon prompt icon; messages and PATH still work |

All three are independent—combine as needed.

## Troubleshooting

**Verify hook is loaded:** `type _envy_hook` (bash/zsh) or `functions _envy_hook` (fish).

**Temporarily disable:** `export ENVY_SHELL_HOOK_DISABLE=1` (unset to re-enable).

**Force refresh:** Delete `<user-wide cache>/shell/` and run any envy command in a project that is *not* on a local cache tree.

**Missing raccoon (Windows/PowerShell):** The icon needs a UTF-8 console; PowerShell defaults to the legacy OEM code page (e.g. 437). Add `[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()` *above* the line that dot-sources the hook (`. "..."`) in `$PROFILE`. The hook nudges once per session when the icon is wanted but the console isn't UTF-8—silenced by `ENVY_SHELL_NO_ICON=1`.
