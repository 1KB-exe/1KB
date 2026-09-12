# Bootstrap size pass

Measured on Windows 11 x64 10.0.26200.9168 (WOW64), VS 2022 tools, production
`https://r.2v2.me`, checked-in Crinklers, `/ORDERTRIES:1000`. Physical EOF bytes,
not instruction counts. This is a measured local search, **not a global lower
bound or a multi-Windows-version compatibility claim**.

## Candidates

Columns: GUI / console / GUI icon-capable / console icon-capable. Icons and
identity are excluded here. `tools/sweep-bootstrap.py` preserves every generated
source, object, executable and link log; `tests/bootstrap-size-baseline.asm` is
the incoming, working ExitProcess baseline. Over 100 candidates were assembled
and compressed, including coarse/fine virtual-address sweeps.

| Candidate | Bytes | Decision / evidence |
|---|---|---|
| Incoming baseline | 403 / 420 / 457 / 474 | Direct-core tests pass |
| **Production: GUI status stays in EBP; failure falls through** | **398 / 420 / 452 / 474** | Saves 5 / 0 / 5 / 0; full direct-core matrix passes |
| Fallthrough but retain GUI XCHG | 400 / 420 / 454 / 474 | Inferior |
| Tail JMP ExitProcess, original layout | 403 / 421 / 457 / 475 | No saving; not adopted |
| Stack exit argument initialized from process handle | 398 / 421 / 452 / 475 | Preserves query-failure value, larger console |
| Stack exit argument initialized to zero | 398 / 419 / 452 / 473 | Matrix passes, but query failure would falsely return success; reject 1-byte trade |
| Stack exit argument initialized to one | 398 / 420 / 452 / 474 | Tie; keep simpler existing console implementation |
| Push exit address + RET | 402 / 424 / 456 / 478 | Larger |
| Literal zero pushes | 403 / 423 / 457 / 477 | Larger despite same semantics |
| Carry / sign retry variants | 399 / 419 / 453 / 473; 402 / 419 / 456 / 473 | Larger GUI; these use rejected zero-status console |
| Adjacent GUI scratch | 399 / 419 / 453 / 473 | Larger GUI, cache output can overwrite scratch; reject without execution |
| Initialize aliased STARTUPINFO.cb | 402 / 424 / 456 / 478 | Fails corrupt-cache/404 GUI case (returns 0, no capture output); not a compatibility fix |
| Separate, initialized STARTUPINFO | 406 / 425 / 460 / 479 | Same observed failure, packed and conventional; cause unresolved |
| Production, ORDERTRIES:10000 | 398 / 420 / 452 / 474 | No improvement |
| **Unsafe RET-only, split exits** | **396 / 414 / 450 / 468** | Full recovery retained, but all four threaded controls hang and remain write-locked |
| Cache-only, ExitProcess retained | 358 / 376 / 412 / 430 | Cached handoff works; successful recovery test fails: corrupt/missing runtime cannot launch |
| **Smallest measured: cache-only + RET** | **355 / 373 / 409 / 427** | Both sacrifices; deterministic lingering-thread failure |
| Conventional unpacked adapter | 3584 / 3584 / — / — | Normal imports, ASLR, DEP, no executable writable sections; direct tests pass with original cb=0 semantics |

Coarse offsets 0..496 step 16 and fine offsets 240..288 did not improve the
selected defaults. DEC retry ties SUB; OR in place of TEST worsens console.
RET-only now saves just **2 GUI / 6 console bytes** versus production. No unsafe
build option is exposed: this saving does not justify locked executables.
Cache-only saves 40/44 bytes but removes the central first-install/recovery
feature, not incidental compatibility. Unsafe files exist only in sweep outputs.

## Complete generated launchers

`capture-runtime.c` fixtures; icon variants use `src/1KB-logo.ico` through the
actual builder's optimizer. Each column is baseline → production.

| Identity | GUI | Console | GUI icon | Console icon |
|---|---:|---:|---:|---:|
| `gh:1kb-exe/1kb` | 405→400 | 422→422 | 730→725 | 747→747 |
| `https://example.com/1KB.ini#capture-runtime` | 424→419 | 441→441 | 749→744 | 766→766 |

The icon fixture adds a 271-byte carrier (171 icon + 20 group + 80 metadata).
All files also have the builder's one-byte decoder read-ahead guard. The GitHub
identity is the one-byte built-in identity; the URL identity uses 20 bytes.
Do not remove the guard based on a single identity's successful execution.
All 16 baseline/production complete files passed cached Unicode handoff,
GUI detachment, console waiting/exit 37 and image-write-access checks.

## Verification and limitations

- Incoming and production direct matrices: **72 cases each**, all four cores,
  with/without a suspended surviving thread, valid/corrupt/missing canonical
  runtime against valid/invalid/404 downloads, Unicode argv, Unicode 256-char
  temp paths. Enhanced checks demonstrate GUI detachment and console waiting.
- Positive/negative termination control: all four production cores terminate
  and become writable; all four RET cores still live and reject write access
  after the capture child completes. Test cleanup kills/reaps unsafe processes.
- Normal-import conventional GUI/console: **36 cases pass**. This still retains
  cb=0 and other assembly assumptions; it is not a fully documented bootstrap.
- API sets, hash multiplier, resolver and Crinkler binaries are unchanged.
  Executed import paths pass on this host; future DLL export additions remain
  a known tiny-hash risk. No cross-version import proof was attempted.
- Rebuilt builder/runtime and ran the broader bootstrap suite. Overlay identity
  and payload-crypto tests pass. **Runtime integration does not pass**: incoming
  baseline initially failed the first-install assertion; repeat baseline and
  final production both time out there after 40 seconds and leave `console/r`
  locked during cleanup. Later runtime update/restart cases remain unverified.
  Complete-file capture tests do not substitute for that integration suite.
- Initial cb=68/separate-structure experiments returned zero with no child
  capture and no HTTP request in the corrupt-cache case. A short-path standalone
  CreateProcess probe returned error 193 for cb=0 and cb=68. Cause unresolved;
  neither experiment is shipped or advertised as reliable.

## Compatibility / trust evidence

Live `VirtualQueryEx` finds production image mappings with protections **0x40
(PAGE_EXECUTE_READWRITE)** and **0x80 (PAGE_EXECUTE_WRITECOPY)**. The conventional
adapter instead has R/RX/RW/write-copy mappings, no writable executable mappings,
and was relocated away from 0x400000. Its size cost is +3186/+3164 bytes over
production before identity/icons. Standard layout is a substantial, legitimate
compatibility option, but not a size optimization; its section ordering has not
been minimized.

Zero sections, overlapping DOS/PE/decompressor fields, private hashed imports,
self-modifying decompression and no ASLR/DEP flags remain in tiny production.
The runtime remains a conventional **168448-byte** PE with ASLR/DEP, RX code,
RW data, and a 2048-byte raw metadata/resource section. Neither measured image
has a certificate table. Signed runtime/builder publication is preferable to
stripping those protections or metadata. Sign final bytes, timestamp and verify;
bootstrap currently does **not** authenticate the executable it downloads into
writable temporary/cache storage. Moving trust verification into the downloaded
runtime cannot authenticate that runtime before its first execution.

No scanner acceptance tests, signing credentials or certificate-size measurements
were available. No antivirus improvement is claimed. Authenticode cost depends
on the certificate chain/timestamp; tiny iconless headers lack the ordinary
security-directory layout, so signing is not merely appending a blob. Adding
signature enforcement, normal imports, conventional headers and W^X deserves
separate measured designs, not scanner-evasion tricks.

## Reproduce

From a VS x86 tools prompt (output directories must be new):

```cmd
py -3 tools\sweep-bootstrap.py --out ignore\size-sweep --standard
py -3 tools\sweep-bootstrap.py --out ignore\order10000 --filter direct-status --ordertries 10000
set KEEP_BOOTSTRAP_TESTS=1
set KEEP_BOOTSTRAP_TEMPLATES=ignore\production-cores
tests\test-bootstrap.cmd
py -3 tests\test_bootstrap_termination.py --capture-runtime .1KB-test\capture-runtime.exe
py -3 tests\test_bootstrap_termination.py --capture-runtime .1KB-test\capture-runtime.exe --source ignore\size-sweep\unsafe-ret-split\bootstrap.asm --expect-hang
py -3 tests\test_bootstrap_cores.py --capture-runtime .1KB-test\capture-runtime.exe --standard
py -3 tools\measure-bootstrap-launchers.py --builder final=1KB.exe --fixtures .1KB-test --out ignore\complete
py -3 tests\test_generated_bootstrap.py --measurements ignore\complete --capture-runtime .1KB-test\capture-runtime.exe
py -3 tools\probe-bootstrap-memory.py --capture-runtime .1KB-test\capture-runtime.exe ignore\production-cores\console-crinkler.exe ignore\size-sweep\direct-status\console-standard.exe
```

Use `pe-size-report.py` on the resulting files for resource/identity/protection
accounting. Repeat `--builder baseline=...` to compare a saved incoming builder.
Session evidence/snapshots are retained under `ignore/size-pass/` (not committed);
production measurements are in `production-cores/`, `production-launchers/` and
`production-*.log`. The broader suite's expected current failure still returns
nonzero; `KEEP_BOOTSTRAP_TESTS=1` only retains fixtures, not suppresses failures.

Most promising unimplemented directions: redesign resolver and decompressor
**together** to reuse their dead storage; jointly sweep signed multipliers,
import ordering and BSS addresses against several Windows export sets; compare
an unpacked tiny normal-import PE rather than assuming compression always wins;
redesign the 54-byte icon-capable header premium while preserving loader and
resource-walker behavior. Direct-download-to-canonical-file, ANSI-only paths and
fixed syscall numbers were not implemented: their concurrency/Unicode/platform
sacrifices need isolated tests before any claimed saving. Crinkler binaries did
not need rebuilding; the builder/runtime were rebuilt for production changes.
