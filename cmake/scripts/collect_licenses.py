#!/usr/bin/env python3
"""Concatenate license files into one text blob.

Usage: collect_licenses.py <output.txt> <manifest-file>

Reads manifest (from find_licenses.py) with format: name|license_path
Concatenates licenses with component headers and writes plain UTF-8; embed_resource.py
--compress squeezes it alongside every other embedded resource.
"""

import argparse
import html
import re
import sys
from pathlib import Path

SEPARATOR = "=" * 80


def extract_lua_license(html_content: str) -> str:
    """Extract MIT license text from Lua's readme.html."""
    # Find the blockquote containing the license
    match = re.search(
        r'<BLOCKQUOTE[^>]*>(.*?)</BLOCKQUOTE>', html_content, re.DOTALL | re.IGNORECASE
    )
    if not match:
        return html_content  # Fallback to full content

    license_html = match.group(1)
    # Remove HTML tags, convert entities, normalize whitespace
    text = re.sub(r'<[^>]+>', ' ', license_html)
    text = html.unescape(text)
    text = re.sub(r'[ \t]+', ' ', text)
    text = re.sub(r'\n ', '\n', text)
    text = re.sub(r' \n', '\n', text)
    text = re.sub(r'\n{3,}', '\n\n', text)
    return text.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="Output .txt file path")
    parser.add_argument("manifest", help="Manifest file from find_licenses.py")
    args = parser.parse_args()

    manifest_path = Path(args.manifest)
    if not manifest_path.is_file():
        print(f"ERROR: Manifest file not found: {manifest_path}", file=sys.stderr)
        return 1

    # Read and parse manifest
    entries = []
    for line in manifest_path.read_text(encoding="utf-8").strip().split("\n"):
        if not line:
            continue
        if "|" not in line:
            print(f"ERROR: Invalid manifest line: {line}", file=sys.stderr)
            return 1
        name, license_path = line.split("|", 1)
        entries.append((name, Path(license_path)))

    # Build concatenated license text
    parts = []
    for name, license_path in entries:
        if not license_path.is_file():
            print(f"ERROR: License file not found: {license_path}", file=sys.stderr)
            return 1

        content = license_path.read_text(encoding="utf-8", errors="replace")

        # Extract license from HTML for Lua
        if "lua" in name.lower() and license_path.suffix.lower() == ".html":
            content = extract_lua_license(content)

        # Skip header for top-level envy license (first entry, content only)
        if name.lower() != "envy":
            parts.append(SEPARATOR)
            parts.append(name)
            parts.append(SEPARATOR)
            parts.append("")
        parts.append(content.rstrip())
        parts.append("")
        parts.append("")

    full_text = "\n".join(parts)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(full_text.encode("utf-8"))

    return 0


if __name__ == "__main__":
    sys.exit(main())
