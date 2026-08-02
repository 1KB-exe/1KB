# Bootstrap and icon-capable Crinkler format

Source and tests are authoritative. Measurements use `src/1KB-logo.ico`,
the optimized 170-byte 32×32 `RT_ICON`, its 20-byte group, and the fixed
`url:http://localhost:12345/1KB.ini` identity documented in `architecture.md`.

## Current sizes

| Representation | GUI | Console |
|---|---:|---:|
| Bootstrap-specific Crinkler core | **412** | **431** |
| Serialized iconless image, including decoder-safety byte | **413** | **432** |
| Resource-capable core before icon insertion | **466** | **485** |
| Final icon image before configuration, including safety byte | **738** | **757** |

Before the bootstrap audit, raw iconless cores were 497/525 bytes and final icon
images were 822/850 bytes. The current implementation therefore removes 85 GUI
bytes and 94 console bytes from iconless output, and 84/93 bytes from icon
output. The fixed icon tax against a serialized iconless image is 325 bytes.
See `icon-png-optimizer.md` for the bounded PNG search.

The final icon byte maps are:

```text
GUI                                  Console
280 header/depacker                  280 header/depacker
187 compressed payload/tiny imports 206 compressed payload/tiny imports
 80 additional resource metadata     80 additional resource metadata
170 RT_ICON                          170 RT_ICON
 20 RT_GROUP_ICON                     20 RT_GROUP_ICON
  1 decoder safety                     1 decoder safety
---                                  ---
738                                  757
```

Configuration begins at byte 738/757. Both decoders require one zero byte
between their stream and arbitrary configuration. Removing it worked for some
leading values but failed for valid overlay shapes after recompression, so the
builder deliberately retains it.

## Bootstrap reductions and private ABI

The x86 assembly now:

- returns through the Windows process-start thunk instead of importing
  `ExitProcess`;
- stores the URL and `%tmp%/r` directly as UTF-16 and expands the full
  path with one `ExpandEnvironmentStringsW` call;
- uses the expanded path as the mutex-independent runtime name and reuses the
  recovery URL as the mutex name;
- places zeroed process structures immediately after the path and aliases
  `STARTUPINFOW` with `PROCESS_INFORMATION`;
- obtains `ProcessParameters.CommandLine.Buffer` from the customized Crinkler
  resolver instead of walking the PEB itself;
- uses one scratch byte as the download/retry state and keeps console exit-code
  forwarding;
- keeps the console mutex handle as a stack local, reusing `ReleaseMutex`'s
  stdcall argument cleanup and leaving `ESI` available as zero.

The checked-in Crinkler resolver enters the bootstrap with `EAX=0` and
nonvolatile `EBX=PEB.ProcessParameters`. Its signed-byte hash multiplier is 67
for the GUI import set and 91 for console. Crinkler still checks each candidate
against current and known exports before linking. These are 1KB.exe private
contracts.
The assembly also intentionally relies on locally verified size-oriented
behavior: current Windows accepts a zero `STARTUPINFOW.cb`, reads the aliased
startup input before writing process output, and terminates the process when the
entry point returns. These choices are smaller than the documented defensive
forms and should be retested after platform changes.

`TINYIMPORT` itself can break if future Windows exports introduce a hash
collision; this is an upstream Crinkler tradeoff.

## Resource layout

The image retains Crinkler's zero sections, image base `0x400000`, four-byte
section/file alignment, fixed decompression target, tiny context compressor,
and `TINYIMPORT` resolver. The optional header is 120 bytes with three data
directories. An icon carrier is inserted at file/RVA 148; `SizeOfHeaders` and
the entry RVA are 418 for the fixture. The packed-stream pointer is 550, so the
stream occupies 550..736 (GUI) or 550..755 (console), followed by the safety
byte.

The resource root begins unaligned at RVA 132 inside the import directory. Its
16-byte header is also the import and resource data-directory fields. The
resource directory's nominal size is `0x20000`, encoding zero named entries and
two integer-ID entries in the overlapping root bytes. The mapped tree spans RVA
132..417.

Each child directory's ignored 12-byte prefix overlaps preceding counts and
entries. Each 16-byte `IMAGE_RESOURCE_DATA_ENTRY` stores only its first eight
bytes separately; its ignored code-page/reserved half is the first eight bytes
of the corresponding icon/group data. Metadata that conventionally needs 160
bytes therefore needs only 80 additional physical bytes. The carrier at 148 is
270 bytes: 80 metadata, 170 icon, and 20 group.

The tree exposes one integer-ID `RT_ICON`, one integer-ID `RT_GROUP_ICON`, one
language per leaf, and group-to-icon ID 1.

## Crinkler adaptations

Both tools are based on Crinkler commit
`afc007676157e216bf06227c021f7c2a88da9448` (3.0b, 2026-07-30):

- `third_party/crinkler-bootstrap/` changes `import20_1k.asm` to establish the
  private entry ABI while retaining the upstream tiny header.
- `third_party/crinkler-icon/` has the same import change and an altered
  `header22_1k.asm` with resource fields and cross-carrier trampolines.

The altered modules, source patch, upstream license, binary, provenance, and
rebuild instructions are kept together in each directory. Builds use
`/OVERRIDEALIGNMENTS` for the virtual-only `rootPath_align9_131` placement and
`/ORDERTRIES:1000`. The alignment saves one byte in each core without adding
physical data.

`tools/benchmark-bootstrap-cores.cmd` assembles only the two bootstrap objects,
links all four cores, emits HTML reports, and prints physical sizes. The raw
initialized payload is 256 GUI bytes and 285 console bytes: 107 resolver bytes,
95/124 bootstrap bytes, 7 DLL-name bytes, and 47 linked UTF-16 URL/path bytes
after trailing-zero coalescing (the two data hunks may be reordered). The
reports estimate 186.11/204.87 compressed bytes. The iconless tiny header/depacker costs 226 physical bytes;
the resource-capable one costs 280. A separate zero safety byte terminates each
serialized core before arbitrary overlay data.

### Measured hash/resolver search

Physical output, not raw instruction count, selected the result:

- Baseline 32-bit multiplier `0x0097e201`: 415/432 iconless and 469/486
  resource-capable.
- Adopted signed-byte multipliers: GUI 67 produced 412/466; console 91 produced
  431/485. They remove three raw resolver bytes and save 3/1 final bytes.
- Exhaustive signed-byte candidates found GUI ties at -63 and console ties at
  several negative values, but no smaller output. Even candidates bottomed out
  at 414/431 iconless.
- Letting the restricted search choose multiplier 71 produced 414/431 and
  468/485: useful, but two GUI bytes worse than specialization.
- Encoding the two PEB list walks as shorter `[eax]` loads reduced raw code by
  two bytes but worsened every core by one byte: 416/433 and 470/487.

## Local validation

Validated on x64 Windows build **10.0.26200.8875** under WOW64, Intel Core
i7-12700:

- raw GUI execution and console exit-code 37 forwarding across path lengths;
- representative overlays with the decoder-safety byte;
- missing-runtime download failure returning 1;
- builder generation, runtime handoff, installation, and background update;
- `LoadLibraryExW`, resource APIs, `ExtractIconExW`, and `SHGetFileInfoW` icon
  retrieval.

No other Windows build or ARM64 system was tested.
