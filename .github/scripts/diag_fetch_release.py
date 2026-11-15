"""TEMPORARY (#401): A/B a released envy against this build on a GH Windows runner.

The x.jlink shape from the issue -- 7zr.exe unpacks 7z.exe in STAGE, 7z.exe opens a big
fetched archive in INSTALL -- beside siblings that spawn shells while it downloads. Each
run gets a fresh cache. Prints exit codes, "still locked"/"could not remove" warnings, and
any fetch_release_wait events (with holders). Diagnostic only: always exits 0.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path

U7 = "https://github.com/ip7z/7zip/releases/download/26.00/"
CMAKE_ZIP = (
    "https://github.com/Kitware/CMake/releases/download/v4.1.2/"
    "cmake-4.1.2-windows-x86_64.zip"
)
NODE_ZIP = "https://nodejs.org/dist/v22.11.0/node-v22.11.0-win-x64.zip"

SEVENZIP = f"""IDENTITY = "diag.sevenzip@r1"
FETCH = {{
  {{ source = "{CMAKE_ZIP}" }},
  {{ source = "{U7}7zr.exe",
    sha256 = "4bec0bc59836a890a11568b58bd12a3e7b23a683557340562da211b6088058ba" }},
  {{ source = "{U7}7z2600-x64.exe",
    sha256 = "6fe18d5b3080e39678cabfa6cef12cfb25086377389b803a36a3c43236a8a82c" }},
}}
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts)
  envy.run('& "' .. fetch_dir .. '7zr.exe" x "' .. fetch_dir .. '7z2600-x64.exe" "-o' ..
           stage_dir .. '7z" 7z.exe 7z.dll -y')
end
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  envy.run('& "' .. stage_dir .. '7z\\\\7z.exe" x "' .. fetch_dir ..
           'cmake-4.1.2-windows-x86_64.zip" "-o' .. install_dir .. 'cmake" -aoa -y -bd')
end
"""

NODE = f"""IDENTITY = "diag.node@r1"
FETCH = {{ source = "{NODE_ZIP}" }}
STAGE = function(fetch_dir, stage_dir, tmp_dir, opts) end
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  -- FileShare.Read, as 7-Zip opens its input: fails while anyone holds it for write.
  local zip = fetch_dir .. "node-v22.11.0-win-x64.zip"
  envy.run("[IO.File]::Open('" .. zip .. "', 'Open', 'Read', 'Read').Close(); " ..
           "tar -xf '" .. zip .. "' -C '" .. install_dir .. "'")
end
"""

# Shells born while the others download: each one used to inherit their write handles.
SHELLS = """IDENTITY = "diag.shells@r1"
FETCH = function(tmp_dir, opts) end
INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, opts)
  for i = 1, 12 do envy.run("Start-Sleep -Milliseconds 2500") end
end
"""


def defender_status() -> None:
    script = (
        "$s = Get-MpComputerStatus; $p = Get-MpPreference; "
        "[pscustomobject]@{ RealTime=$s.RealTimeProtectionEnabled; "
        "OnAccess=$s.OnAccessProtectionEnabled; Ioav=$s.IoavProtectionEnabled; "
        "Mode=$s.AMRunningMode; Exclusions=($p.ExclusionPath -join ';') } | Format-List"
    )
    r = subprocess.run(
        ["pwsh", "-NoProfile", "-Command", script], capture_output=True, text=True
    )
    print("== Defender ==")
    print((r.stdout + r.stderr).strip() or "(no output)")


def released_envy(tag: str, root: Path) -> Path:
    arch = "arm64" if platform.machine().upper() == "ARM64" else "x86_64"
    url = (
        "https://github.com/envy-package-manager/envy/releases/download/"
        f"{tag}/envy-windows-{arch}.zip"
    )
    dest = root / f"envy-{tag}"
    dest.mkdir(parents=True, exist_ok=True)
    archive = dest / "envy.zip"
    urllib.request.urlretrieve(url, archive)
    with zipfile.ZipFile(archive) as z:
        z.extractall(dest)
    return next(dest.rglob("envy.exe"))


def run_once(envy: Path, root: Path, label: str, n: int) -> dict:
    work = root / f"{label}-{n}"
    cache = work / "cache"
    work.mkdir(parents=True)
    for name, body in (
        ("sevenzip.lua", SEVENZIP),
        ("node.lua", NODE),
        ("shells.lua", SHELLS),
    ):
        (work / name).write_text(body, encoding="utf-8")
    entries = "\n".join(
        f'  {{ spec = "{ident}", source = "{(work / f).as_posix()}" }},'
        for ident, f in (
            ("diag.shells@r1", "shells.lua"),
            ("diag.sevenzip@r1", "sevenzip.lua"),
            ("diag.node@r1", "node.lua"),
        )
    )
    manifest = work / "envy.lua"
    manifest.write_text(
        f'-- @envy bin "envy-bin"\nPACKAGES = {{\n{entries}\n}}\n', encoding="utf-8"
    )
    trace = work / "trace.jsonl"
    env = dict(os.environ, ENVY_NO_REEXEC="1")
    start = time.monotonic()
    r = subprocess.run(
        [
            str(envy),
            "--cache-root",
            str(cache),
            f"--trace=file:{trace}",
            "install",
            "--manifest",
            str(manifest),
        ],
        capture_output=True,
        text=True,
        env=env,
        cwd=work,
        timeout=900,
    )
    waits = []
    if trace.exists():
        for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                ev = json.loads(line)
            except json.JSONDecodeError:
                continue
            if ev.get("event") == "fetch_release_wait":
                waits.append(
                    f"{Path(ev['path']).name} {ev['duration_ms']}ms {ev['holders']}"
                )
    out = r.stdout + r.stderr
    result = {
        "rc": r.returncode,
        "secs": round(time.monotonic() - start, 1),
        "still_locked": out.count("still locked"),
        "could_not_remove": out.count("could not remove"),
        "in_use": out.count("being used by another process"),
        "release_waits": waits,
    }
    if r.returncode != 0:
        result["tail"] = out[-1500:]
    shutil.rmtree(work, ignore_errors=True)
    return result


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--candidate", required=True, help="path to this build's envy.exe")
    ap.add_argument("--baseline-tag", default="v0.7.3")
    ap.add_argument("--runs", type=int, default=3)
    args = ap.parse_args()

    defender_status()
    root = Path(os.environ.get("RUNNER_TEMP", tempfile.gettempdir())) / "diag401"
    shutil.rmtree(root, ignore_errors=True)
    root.mkdir(parents=True)
    try:
        baseline = released_envy(args.baseline_tag, root)
    except Exception as e:  # noqa: BLE001 - diagnostic
        print(f"could not fetch {args.baseline_tag}: {e}")
        baseline = None

    subjects = [("candidate", Path(args.candidate).resolve())]
    if baseline:
        subjects.insert(0, (args.baseline_tag, baseline))
    for n in range(args.runs):
        for label, envy in subjects:
            try:
                res = run_once(envy, root, label, n)
            except Exception as e:  # noqa: BLE001 - diagnostic
                res = {"error": repr(e)}
            print(f"== {label} run {n}: {json.dumps(res, indent=1)}", flush=True)


if __name__ == "__main__":
    main()
