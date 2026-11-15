# Test Archives

This directory contains test archives used by extract unit tests and stage phase functional tests.

## Structure

- `source/root/` - Source files used to create the test archives
- `source/bare/` - Source for bare single-stream compression fixtures
- `*.tar*` - Various tar-based archive formats
- `test.zip` - ZIP archive format
- `hello.txt.{gz,bz2,xz,zst,lzma}` - Bare single-stream compressed fixtures
- `corrupt.gz` - Garbage bytes with .gz suffix; used by error-path tests
- `stated.tar.zst` - `test.tar.zst` plus envy's totals trailer (5 files, 93 bytes)

## Regenerating Archives

To regenerate the test archives (e.g., after modifying source files):

```bash
./create_archives.sh
```

This script:
1. Sets `COPYFILE_DISABLE=1` to prevent macOS extended attributes from being included
2. Creates archives from the `source/` directory
3. Generates all supported archive formats

**Important**: After regenerating archives, you must update the SHA256 hashes in all spec files that reference `test.tar.gz`:
- `test_data/specs/stage_default.lua`
- `test_data/specs/stage_declarative_strip.lua`
- `test_data/specs/stage_imperative.lua`
- `test_data/specs/stage_extract_single.lua`

To get the new SHA256 hash:
```bash
shasum -a 256 test.tar.gz
```

## Why Clean Archives?

The test archives must not contain macOS metadata files like:
- AppleDouble files (`._*`)
- `.DS_Store` files
- `__MACOSX/` directories

These files can cause test failures on Linux systems where they appear as unexpected entries in the extracted directory structure.
