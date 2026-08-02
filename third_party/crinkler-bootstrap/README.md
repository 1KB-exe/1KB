# 1KB.exe bootstrap Crinkler

`Win32/Crinkler.exe` is an altered build of Crinkler `master` commit
`afc007676157e216bf06227c021f7c2a88da9448` (3.0b, 2026-07-30).

`import20_1k.asm` preserves `PEB.ProcessParameters` while walking the PEB and
enters the linked bootstrap with `EAX=0` and nonvolatile
`EBX=ProcessParameters`. It also uses a patched signed-byte hash multiplier.
`bootstrap-hash.patch` specializes Crinkler's collision search for the exact
GUI and console import sets (multipliers 67 and 91) and patches that byte. This
removes three raw resolver bytes and improves the final compressed cores by
3 GUI bytes and 1 console byte. The generated PE format is otherwise the
upstream zero-section tiny-header format. These are private bootstrap
contracts, not upstream Crinkler contracts.

The upstream zlib/third-party license notice is preserved verbatim in
`LICENSE.txt`. The checked-in executable keeps normal builds independent of
NASM and of rebuilding Crinkler.

To reproduce it, clone <https://github.com/runestubbe/Crinkler>, check out the
commit above, apply `bootstrap-hash.patch`, replace
`source/Crinkler/modules/import20_1k.asm` with this directory's file, and build
`Crinkler.sln` as `Release|Win32` with Visual Studio 2022 and NASM 2.16.03 on
`PATH`. If MFC's `afxres.h` is unavailable, changing
`#include "afxres.h"` to `#include <windows.h>` in `Crinkler.rc` affects only
the tool's resources and reproduces the launcher format unchanged.
