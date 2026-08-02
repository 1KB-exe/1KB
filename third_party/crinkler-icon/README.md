# Resource-capable 1KB.exe Crinkler

`Win32/Crinkler.exe` is an altered build of Crinkler `master` commit
`afc007676157e216bf06227c021f7c2a88da9448` (3.0b, 2026-07-30).

Two included module sources differ from upstream:

- `header22_1k.asm` reserves PE resource-directory fields, moves code behind
  insertion offset 148, and exposes the crossing references patched by
  1KB.exe's icon packer.
- `import20_1k.asm` preserves `PEB.ProcessParameters`, enters the bootstrap
  with `EAX=0` and nonvolatile `EBX=ProcessParameters`, and uses a patched
  signed-byte hash multiplier.
- `bootstrap-hash.patch` specializes Crinkler's collision search for the exact
  GUI and console import sets (multipliers 67 and 91) and patches that byte.

The header retains Crinkler's zero-section tiny-header decompressor. 1KB.exe
inserts its icon carrier at offset 148; this is not an upstream
Crinkler format.

The upstream zlib/third-party license notice is preserved verbatim in
`LICENSE.txt`. The checked-in executable keeps normal builds independent of
NASM and of rebuilding Crinkler.

To reproduce it, clone <https://github.com/runestubbe/Crinkler>, check out the
commit above, apply `bootstrap-hash.patch`, replace
`source/Crinkler/modules/header22_1k.asm` and
`source/Crinkler/modules/import20_1k.asm` with this directory's files, and build
`Crinkler.sln` as `Release|Win32` with Visual Studio 2022 and NASM 2.16.03 on
`PATH`. If MFC's `afxres.h` is unavailable, changing `#include "afxres.h"` to
`#include <windows.h>` in `Crinkler.rc` affects only the tool's resources and
reproduces the launcher format unchanged.
