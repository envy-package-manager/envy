# Shell Integration

Envy shell hooks auto-manage `PATH` and `ENVY_PROJECT_ROOT` as you navigate envy projects. No envy binary required at shell startup—hooks are pure shell.

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
4. Compute absolute bin path: `$manifest_dir/$bin_value`
5. Manage `PATH`—prepend new bin dir, remove old one on project switch or exit
6. Export `ENVY_PROJECT_ROOT` (or unset when leaving)

## Auto-Update

Hook files carry `_ENVY_HOOK_VERSION=N`. Any envy command checks the stamp and refreshes stale hooks automatically. Restart your shell after an update.

## Environment Variables

| Variable | Effect |
|---|---|
| `ENVY_SHELL_HOOK_DISABLE=1` | Disable the hook entirely—no PATH changes, no messages, no icon |
| `ENVY_SHELL_NO_ENTER_EXIT_ANNOUNCE=1` | Suppress "entering/leaving" status messages; PATH and icon still work |
| `ENVY_SHELL_NO_ICON=1` | Suppress the raccoon prompt icon; messages and PATH still work |

All three are independent—combine as needed.

## Troubleshooting

**Verify hook is loaded:** `type _envy_hook` (bash/zsh) or `functions _envy_hook` (fish).

**Temporarily disable:** `export ENVY_SHELL_HOOK_DISABLE=1` (unset to re-enable).

**Force refresh:** Delete `<user-wide cache>/shell/` and run any envy command in a project that is *not* on a local cache tree.

**Missing raccoon (Windows/PowerShell):** The icon needs a UTF-8 console; PowerShell defaults to the legacy OEM code page (e.g. 437). Add `[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()` *above* the line that dot-sources the hook (`. "..."`) in `$PROFILE`. The hook nudges once per session when the icon is wanted but the console isn't UTF-8—silenced by `ENVY_SHELL_NO_ICON=1`.
