#!/usr/bin/env python3
"""Tests for embed_resource.py — verifies CRLF normalization and substitution."""

import lzma
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).parent / "embed_resource.py"


class EmbedRunner:
    """Drives embed_resource.py over a temp file and reads back the emitted bytes."""

    def _run_embed(self, resource_bytes: bytes, defines=None,
                   normalize_eol: bool = True, compress: bool = False) -> str:
        """Write resource_bytes to a temp file, run embed_resource, return header text."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            res_file = tmp / "resource.txt"
            res_file.write_bytes(resource_bytes)
            out_file = tmp / "out.h"

            cmd = [sys.executable, str(SCRIPT), str(out_file)]
            if normalize_eol:
                cmd.append("--normalize-eol")
            if compress:
                cmd.append("--compress")
            for d in (defines or []):
                cmd.extend(["-D", d])
            cmd.append(f"test_resource={res_file}")

            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return out_file.read_text()

    def _extract_bytes(self, header: str, symbol: str = "kTestResource") -> bytes:
        """Parse hex byte literals from the generated C++ array initializer."""
        m = re.search(re.escape(symbol) + r"\[\]\s*=\s*\{([^}]+)\}", header)
        assert m, f"Could not find array in header:\n{header}"
        hex_vals = [tok.strip() for tok in m.group(1).split(",") if tok.strip()]
        return bytes(int(h, 16) for h in hex_vals)


class TestCRLFNormalization(EmbedRunner, unittest.TestCase):
    """--normalize-eol strips \\r from text resources; omitting it preserves bytes."""

    def test_lf_passthrough(self):
        """LF-only input is preserved unchanged."""
        data = b"line1\nline2\nline3\n"
        header = self._run_embed(data)
        embedded = self._extract_bytes(header)
        self.assertEqual(embedded, data)

    def test_crlf_normalized_to_lf(self):
        """CRLF input is normalized to LF."""
        data = b"line1\r\nline2\r\nline3\r\n"
        header = self._run_embed(data)
        embedded = self._extract_bytes(header)
        self.assertNotIn(b"\r", embedded)
        self.assertEqual(embedded, b"line1\nline2\nline3\n")

    def test_mixed_line_endings_normalized(self):
        """Mixed CR, CRLF, LF all become LF."""
        data = b"crlf\r\nlf\ncr\rend\n"
        header = self._run_embed(data)
        embedded = self._extract_bytes(header)
        self.assertNotIn(b"\r", embedded)
        self.assertEqual(embedded, b"crlf\nlf\ncr\nend\n")

    def test_bare_cr_normalized(self):
        """Bare CR (old Mac) is normalized to LF."""
        data = b"a\rb\rc\r"
        header = self._run_embed(data)
        embedded = self._extract_bytes(header)
        self.assertEqual(embedded, b"a\nb\nc\n")

    def test_substitution_after_normalization(self):
        """Substitutions work correctly on CRLF-normalized content."""
        data = b"@@KEY@@\r\nrest\r\n"
        header = self._run_embed(data, defines=["KEY=VALUE"])
        embedded = self._extract_bytes(header)
        self.assertNotIn(b"\r", embedded)
        self.assertEqual(embedded, b"VALUE\nrest\n")

    def test_binary_data_preserved_without_flag(self):
        """Without --normalize-eol, 0x0d bytes survive unchanged."""
        data = b"\x00\x0d\x01\x02\r\n\xff\x0d\xfe\n"
        header = self._run_embed(data, normalize_eol=False)
        embedded = self._extract_bytes(header)
        self.assertEqual(embedded, data)

    def test_no_normalization_without_flag(self):
        """CRLF text is preserved when --normalize-eol is not passed."""
        data = b"line1\r\nline2\r\n"
        header = self._run_embed(data, normalize_eol=False)
        embedded = self._extract_bytes(header)
        self.assertEqual(embedded, data)


class TestCompression(EmbedRunner, unittest.TestCase):
    """--compress emits one xz stream whose slices unpack to the exact raw bytes."""

    def _unpack(self, header: str) -> dict[str, bytes]:
        """Decode the pack and cut it into its packed_resource slices."""
        packs = re.findall(r"unsigned char (k\w+Xz)\[\]", header)
        self.assertEqual(len(packs), 1, f"expected one pack in header:\n{header}")
        xz = self._extract_bytes(header, packs[0])
        self.assertEqual(xz[:6], b"\xfd7zXZ\x00", "missing xz magic")
        raw = lzma.decompress(xz)
        slices = re.findall(r"packed_resource (k\w+)\{ (\w+), sizeof\((\w+)\), (\d+), (\d+) \}",
                            header)
        self.assertTrue(slices, f"Could not find packed_resource in header:\n{header}")
        out = {}
        for name, pack, sized, offset, size in slices:
            self.assertEqual((pack, sized), (packs[0], packs[0]))
            out[name] = raw[int(offset):int(offset) + int(size)]
        self.assertEqual(sum(len(v) for v in out.values()), len(raw), "slices must tile")
        return out

    def _round_trip(self, data: bytes, defines=None, normalize_eol: bool = True) -> bytes:
        header = self._run_embed(data, defines=defines, normalize_eol=normalize_eol,
                                 compress=True)
        return self._unpack(header)["kTestResource"]

    def test_round_trip_is_byte_identical(self):
        """Compressed text unpacks to exactly the normalized, substituted bytes."""
        self.assertEqual(self._round_trip(b"alpha\nbeta\ngamma\n"),
                         b"alpha\nbeta\ngamma\n")

    def test_round_trip_after_normalization_and_substitution(self):
        """Compression happens last: EOL normalization and -D still apply."""
        self.assertEqual(self._round_trip(b"@@K@@\r\ntail\r\n", defines=["K=V"]),
                         b"V\ntail\n")

    def test_round_trip_binary(self):
        """Arbitrary bytes survive the xz container unchanged."""
        data = bytes(range(256)) * 4
        self.assertEqual(self._round_trip(data, normalize_eol=False), data)

    def test_empty_resource(self):
        """A zero-byte resource still produces a valid, decodable stream."""
        self.assertEqual(self._round_trip(b""), b"")

    def test_compression_shrinks_repetitive_text(self):
        """The whole point: a compressible resource gets smaller."""
        data = b"envy is a freeform package manager\n" * 200
        header = self._run_embed(data, compress=True)
        self.assertLess(len(self._extract_bytes(header, "kOutXz")), len(data))

    def test_resources_share_one_pack_in_order(self):
        """Every resource slices the one stream, in command-line order."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "a.txt").write_bytes(b"first\n")
            (tmp / "b.txt").write_bytes(b"second\n")
            out_file = tmp / "out.h"
            cmd = [sys.executable, str(SCRIPT), str(out_file), "--compress",
                   f"res_a={tmp / 'a.txt'}", f"res_b={tmp / 'b.txt'}"]
            result = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            header = out_file.read_text()
        self.assertEqual(self._unpack(header), {"kResA": b"first\n", "kResB": b"second\n"})
        self.assertIn("kResA{ kOutXz, sizeof(kOutXz), 0, 6 }", header)
        self.assertIn("kResB{ kOutXz, sizeof(kOutXz), 6, 7 }", header)

    def test_header_includes_util(self):
        """Compressed headers need packed_resource and util_unpack_resource."""
        self.assertIn('#include "util.h"', self._run_embed(b"x\n", compress=True))


if __name__ == "__main__":
    unittest.main()
