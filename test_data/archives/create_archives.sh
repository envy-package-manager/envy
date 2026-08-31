#!/usr/bin/env bash
# Create test archives without macOS metadata
set -Eeuo pipefail

cd "$(dirname "$0")"

# Disable macOS extended attributes and resource forks
export COPYFILE_DISABLE=1

echo "Creating test archives from source/..."

# Create tar archive
tar -cf test.tar -C source root

# Create compressed variants
gzip -c < test.tar > test.tar.gz
bzip2 -c < test.tar > test.tar.bz2
xz -c < test.tar > test.tar.xz
zstd -c < test.tar > test.tar.zst

# Create zip archive
rm -f test.zip
(cd source && zip -q -r ../test.zip root)

# Bare single-stream compressed fixtures (no tar wrapper)
gzip  -c < source/bare/hello.txt > hello.txt.gz
bzip2 -c < source/bare/hello.txt > hello.txt.bz2
xz    -c < source/bare/hello.txt > hello.txt.xz
zstd  -q -c < source/bare/hello.txt > hello.txt.zst
lzma  -c < source/bare/hello.txt > hello.txt.lzma

# Corrupt .gz: valid suffix, invalid stream — used by error-path tests.
printf 'this is not gzip data' > corrupt.gz

echo "Done! Created:"
ls -lh test.tar* test.zip hello.txt.* corrupt.gz
