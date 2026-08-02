# Bootstrap and icon-capable Crinkler format

Source and tests are authoritative. Measurements use `src/1KB-logo.ico`,
the optimized 171-byte 32×32 `RT_ICON`, its 20-byte group, and the fixed
`url:http://localhost:12345/1KB.ini` identity documented in `architecture.md`.

## Current sizes

| Representation | GUI | Console |
|---|---:|---:|
| Bootstrap-specific Crinkler core | **400** | **416** |
| Serialized iconless image, including decoder-safety byte | **401** | **417** |
| Resource-capable core before icon insertion | **454** | **470** |
| Final icon image before configuration, including safety byte | **726** | **742** |

Before the bootstrap audit, raw iconless cores were 497/525 bytes and final icon
images were 822/850 bytes. The current implementation therefore removes 97 GUI
bytes and 109 console bytes from iconless output, and 96/108 bytes from icon
output. The fixed icon tax against a serialized iconless image is 325 bytes.
See `icon-png-optimizer.md` for the bounded PNG search.

The final icon byte maps are:

```text
GUI                                  Console
280 header/depacker                  280 header/depacker
174 compressed payload/tiny imports 190 compressed payload/tiny imports
 80 additional resource metadata     80 additional resource metadata
171 RT_ICON                          171 RT_ICON
 20 RT_GROUP_ICON                     20 RT_GROUP_ICON
  1 decoder safety                     1 decoder safety
---                                  ---
726                                  742
```

Configuration begins at byte 726/742. Both decoders require one zero byte
between their stream and arbitrary configuration. The arithmetic decoder can
read one byte past the packed stream; without the zero, valid overlay prefixes
change that read-ahead and some recompressed overlay shapes fail. The byte is
therefore required for every iconless and icon-bearing serialization.

## Bootstrap reductions and private ABI

The x86 assembly now:

- returns through the Windows process-start thunk instead of importing
  `ExitProcess`;
- obtains the temporary directory with `GetTempPathW` and appends the one-byte
  runtime name directly into zeroed storage;
- first starts the fixed cached runtime, and only on failure asks URLMon for a
  completed cache-file path instead of downloading over the fixed path;
- places zeroed process structures after the path and aliases `STARTUPINFOW`
  with `PROCESS_INFORMATION`; GUI recovery safely reuses the failed canonical
  path buffer while console recovery uses a separate BSS cache-path buffer;
- obtains `ProcessParameters.CommandLine.Buffer` from the customized Crinkler
  resolver instead of walking the PEB itself;
- uses one scratch byte as the download/retry state; its subtract operation
  deliberately updates flags before the terminal-failure branch;
- leaves `ESI` available as zero while retaining attached-console waiting and
  exit-code forwarding.

The runtime recognizes execution from a noncanonical URLMon cache path, copies
and validates itself in a unique per-process staging file, flushes it, and
atomically replaces `%TMP%\r`. Concurrent first launches therefore race only
through atomic replacements of complete runtime files; no launcher mutex or
shared partial destination is needed. Once promoted, normal startup remains
network-independent. Runtime-update apply workers are dispatched before this
recovery promotion and retain their existing handoff protocol.

The checked-in Crinkler resolver enters the bootstrap with `EAX=0` and
nonvolatile `EBX=PEB.ProcessParameters`. Its signed-byte hash multiplier is
-117 for both import sets. Crinkler still checks each candidate
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
the entry RVA are 419 for the fixture. The packed-stream pointer is 551, so the
stream occupies 551..724 (GUI) or 551..740 (console), followed by the safety
byte.

The resource root begins unaligned at RVA 132 inside the import directory. Its
16-byte header is also the import and resource data-directory fields. The
resource directory's nominal size is `0x20000`, encoding zero named entries and
two integer-ID entries in the overlapping root bytes. The mapped tree spans RVA
132..418.

Each child directory's ignored 12-byte prefix overlaps preceding counts and
entries. Each 16-byte `IMAGE_RESOURCE_DATA_ENTRY` stores only its first eight
bytes separately; its ignored code-page/reserved half is the first eight bytes
of the corresponding icon/group data. Metadata that conventionally needs 160
bytes therefore needs only 80 additional physical bytes. The carrier at 148 is
271 bytes: 80 metadata, 171 icon, and 20 group.

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
`/OVERRIDEALIGNMENTS` for virtual-only GUI/console offsets 447/320 and
`/ORDERTRIES:1000`. These offsets improve compressed addresses without adding
physical data.

`tools/benchmark-bootstrap-cores.cmd` assembles only the two bootstrap objects,
links all four cores, emits HTML reports, and prints physical sizes. The raw
initialized payload is 229 GUI bytes and 249 console bytes: 107 resolver bytes,
84/104 bootstrap bytes, 7 DLL-name bytes, and 31 linked UTF-16 URL bytes after
trailing-zero coalescing. The reports estimate 173.59/190.73 compressed bytes.
The iconless tiny header/depacker costs 226 physical bytes;
the resource-capable one costs 280. A separate zero safety byte terminates each
serialized core before arbitrary overlay data.

### Measured hash/resolver search

Physical output, not raw instruction count, selected the result:

- The previous specialized multipliers 67/91 produced 409/427 iconless and
  463/481 resource-capable cores with the former mutex-based import sets.
- Exhaustive signed-byte measurement selected -117. A repeated search after
  replacing the mutex imports and `URLDownloadToFileW` with
  `URLDownloadToCacheFileW` kept -117 as the joint GUI/console winner, producing
  400/416 and 454/470. It is collision-free for both exact import sets.
- Encoding the two PEB list walks as shorter `[eax]` loads reduced raw code by
  two bytes in an earlier bootstrap, but worsened every compressed core.

## Local validation

Validated on x64 Windows build **10.0.26200.8875** under WOW64, Intel Core
i7-12700:

- raw GUI execution and console exit-code 37 forwarding across path lengths;
- representative overlays with the decoder-safety byte;
- missing-runtime recovery download and atomic promotion to `%TMP%\r`;
- missing-runtime download failure returning 1;
- builder generation, runtime handoff, installation, and background update;
- `LoadLibraryExW`, resource APIs, `ExtractIconExW`, and `SHGetFileInfoW` icon
  retrieval.

No other Windows build or ARM64 system was tested.
