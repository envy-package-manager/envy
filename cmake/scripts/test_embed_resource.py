#!/usr/bin/env python3
"""Tests for embed_resource.py — verifies CRLF normalization and substitution."""

import gzip
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
    """--compress emits a gzip stream that inflates back to the exact raw bytes."""

    def _round_trip(self, data: bytes, defines=None, normalize_eol: bool = True) -> bytes:
        header = self._run_embed(data, defines=defines, normalize_eol=normalize_eol,
                                 compress=True)
        gz = self._extract_bytes(header, "kTestResourceGz")
        self.assertEqual(gz[:3], b"\x1f\x8b\x08", "missing gzip magic")
        self.assertEqual(gz[4:8], b"\x00\x00\x00\x00", "mtime must be zeroed")
        m = re.search(r"gz_resource kTestResource\{ kTestResourceGz, (\d+), (\d+) \}",
                      header)
        self.assertIsNotNone(m, f"Could not find gz_resource in header:\n{header}")
        self.assertEqual(int(m.group(1)), len(gz))
        inflated = gzip.decompress(gz)
        self.assertEqual(int(m.group(2)), len(inflated))
        return inflated

    def test_round_trip_is_byte_identical(self):
        """Compressed text inflates to exactly the normalized, substituted bytes."""
        self.assertEqual(self._round_trip(b"alpha\nbeta\ngamma\n"),
                         b"alpha\nbeta\ngamma\n")

    def test_round_trip_after_normalization_and_substitution(self):
        """Compression happens last: EOL normalization and -D still apply."""
        self.assertEqual(self._round_trip(b"@@K@@\r\ntail\r\n", defines=["K=V"]),
                         b"V\ntail\n")

    def test_round_trip_binary(self):
        """Arbitrary bytes survive the gzip container unchanged."""
        data = bytes(range(256)) * 4
        self.assertEqual(self._round_trip(data, normalize_eol=False), data)

    def test_empty_resource(self):
        """A zero-byte resource still produces a valid, inflatable stream."""
        self.assertEqual(self._round_trip(b""), b"")

    def test_compression_shrinks_repetitive_text(self):
        """The whole point: a compressible resource gets smaller."""
        data = b"envy is a freeform package manager\n" * 200
        header = self._run_embed(data, compress=True)
        self.assertLess(len(self._extract_bytes(header, "kTestResourceGz")), len(data))

    def test_header_includes_util(self):
        """Compressed headers need gz_resource and util_inflate_resource."""
        self.assertIn('#include "util.h"', self._run_embed(b"x\n", compress=True))


if __name__ == "__main__":
    unittest.main()
