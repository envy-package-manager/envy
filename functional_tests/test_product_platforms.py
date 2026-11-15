"""Product scripts across manifest `platforms`, spec `PLATFORMS` and product `platforms`.

A product gets a family's script exactly when some host of that family satisfies all three
lists, absent meaning every platform. An OS entry covers each of its arches, so the lists
may name platforms at different granularity.
"""

import hashlib
import io
import tarfile
import unittest
from pathlib import Path

from . import test_config
from .env import EnvyTestCase
from .test_config import make_manifest

# Every relation the lists can stand in: absent, OS, OS-arch, a sibling arch, several OSes,
# several OS-arches, the other family, and a mix of families and granularity.
VARIANTS: dict[str, list[str] | None] = {
    "any": None,
    "linux": ["linux"],
    "linux_x64": ["linux-x86_64"],
    "linux_arm": ["linux-arm64"],
    "posix": ["darwin", "linux"],
    "posix_arch": ["darwin-arm64", "linux-x86_64"],
    "windows": ["windows"],
    "mixed": ["windows-x86_64", "linux"],
}

FAMILIES = {"posix": ("darwin", "linux"), "windows": ("windows",)}


def _matches(constraints: list[str] | None, os: str, arch: str) -> bool:
    return not constraints or any(c in (os, f"{os}-{arch}") for c in constraints)


def expected_families(lists: list[list[str] | None]) -> set[str]:
    """Script families some host satisfies every list for: the oracle, independent of
    envy's intersection. A token's arch is everything after its first '-'."""
    arches = {t.split("-", 1)[1] for lst in lists if lst for t in lst if "-" in t}
    arches.add("unnamed")
    return {
        family
        for family, oses in FAMILIES.items()
        if any(
            all(_matches(lst, os, arch) for lst in lists) for os in oses for arch in arches
        )
    }


def lua_list(values: list[str]) -> str:
    return "{ " + ", ".join(f'"{v}"' for v in values) + " }"


class TestProductPlatforms(EnvyTestCase):
    def setUp(self):
        super().setUp()
        self.test_dir = self.make_temp_dir("test_dir")
        self.specs_dir = self.make_temp_dir("specs_dir")
        archive = self.specs_dir / "payload.tar.gz"
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            info = tarfile.TarInfo(name="root/x")
            tar.addfile(info, io.BytesIO(b""))
        archive.write_bytes(buf.getvalue())
        self.archive = archive.as_posix()
        self.archive_hash = hashlib.sha256(buf.getvalue()).hexdigest()

    def _deploy(
        self, cases: list[tuple[str, list[str] | None, list[str] | None, list[str] | None]]
    ) -> dict[str, set[str]]:
        """Deploy one package per (product, manifest, spec, product) platforms case for
        every platform; return the script families each product got."""
        entries = []
        for product, manifest_plats, spec_plats, product_plats in cases:
            identity = f"local.{product}@v1"
            spec_platforms = f"PLATFORMS = {lua_list(spec_plats)}\n" if spec_plats else ""
            product_platforms = (
                f", platforms = {lua_list(product_plats)}" if product_plats else ""
            )
            spec = self.specs_dir / f"{product}.lua"
            spec.write_text(
                f'IDENTITY = "{identity}"\n'
                f"{spec_platforms}"
                f'PRODUCTS = {{ {product} = {{ value = "bin/x"{product_platforms} }} }}\n'
                f'FETCH = {{ source = "{self.archive}", sha256 = "{self.archive_hash}" }}\n'
                "INSTALL = function(install_dir, stage_dir, fetch_dir, tmp_dir, options) end\n",
                encoding="utf-8",
            )
            manifest_platforms = (
                f", platforms = {lua_list(manifest_plats)}" if manifest_plats else ""
            )
            entries.append(
                f'  {{ spec = "{identity}", source = "{spec.as_posix()}"'
                f"{manifest_platforms} }},"
            )

        manifest = self.test_dir / "envy.lua"
        manifest.write_text(
            make_manifest("PACKAGES = {\n" + "\n".join(entries) + "\n}\n", deploy=True),
            encoding="utf-8",
        )
        result = test_config.run(
            [
                str(self.envy),
                "--cache-root",
                str(self.cache_root),
                "deploy",
                "--platform",
                "all",
                "--manifest",
                str(manifest),
            ],
            cwd=self.project_root,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, f"deploy failed: {result.stderr}")

        bin_dir = self.test_dir / "envy-bin"
        return {
            product: {
                family
                for family, script in (
                    ("posix", bin_dir / product),
                    ("windows", bin_dir / f"{product}.bat"),
                )
                if script.exists()
            }
            for product, *_ in cases
        }

    def _check_matrix(self, manifest_variant: str) -> None:
        """Every spec and product variant under one manifest variant, against the oracle."""
        manifest_plats = VARIANTS[manifest_variant]
        cases = [
            (f"m_{manifest_variant}_s_{s}_p_{p}", manifest_plats, spec_plats, product_plats)
            for s, spec_plats in VARIANTS.items()
            for p, product_plats in VARIANTS.items()
        ]
        got = self._deploy(cases)
        wrong = [
            f"{product}: manifest={m} spec={s} product={p} "
            f"want={sorted(expected_families([m, s, p]))} got={sorted(got[product])}"
            for product, m, s, p in cases
            if got[product] != expected_families([m, s, p])
        ]
        self.assertEqual([], wrong, f"{len(wrong)}/{len(cases)} products scripted wrong")

    def test_manifest_any(self):
        self._check_matrix("any")

    def test_manifest_linux(self):
        self._check_matrix("linux")

    def test_manifest_linux_x64(self):
        self._check_matrix("linux_x64")

    def test_manifest_linux_arm(self):
        self._check_matrix("linux_arm")

    def test_manifest_posix(self):
        self._check_matrix("posix")

    def test_manifest_posix_arch(self):
        self._check_matrix("posix_arch")

    def test_manifest_windows(self):
        self._check_matrix("windows")

    def test_manifest_mixed(self):
        self._check_matrix("mixed")

    def test_lists_at_different_granularity_keep_their_scripts(self):
        """Written out by hand, so the oracle above is not the only witness."""
        got = self._deploy(
            [
                # The reported case: the manifest names OSes, the spec their arches.
                ("os_manifest", ["darwin", "linux"], ["darwin-arm64", "linux-x86_64"], None),
                ("arch_manifest", ["linux-x86_64"], ["linux"], None),
                ("os_product", None, ["linux-x86_64"], ["linux"]),
                ("arch_product", ["linux"], None, ["linux-arm64"]),
                ("cross_family", ["windows", "linux"], ["windows-x86_64"], None),
                ("sibling_arches", ["linux-arm64"], ["linux-x86_64"], None),
                ("other_os_arch", ["linux"], ["darwin-arm64"], None),
            ]
        )
        self.assertEqual(
            {
                "os_manifest": {"posix"},
                "arch_manifest": {"posix"},
                "os_product": {"posix"},
                "arch_product": {"posix"},
                "cross_family": {"windows"},
                "sibling_arches": set(),
                "other_os_arch": set(),
            },
            got,
        )


if __name__ == "__main__":
    unittest.main()
