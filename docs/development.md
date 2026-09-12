# Development

Agent reference. Requires Windows, Python 3, Visual Studio 2022 C++ x64/x86 tools, MASM, and the checked-in Crinkler tools.

```cmd
build-1KB.cmd
```

This replaces repository-root `1KB.exe` and publishable runtime `1KB-runtime.exe`. Both are ordinary unpacked MSVC executables; only generated bootstraps use Crinkler.

The x86 runtime builds and publishes as `1KB-runtime.exe`. Point `https://r.2v2.me` at that release asset; the installed runtime remains `%TMP%\r`. Its version metadata/icon live in `src/runtime.rc`; its non-elevating Windows 10/11 manifest lives in `src/runtime.manifest`. Keep the product release versions in those files aligned (currently `0.0.1`, encoded numerically as `0.0.1.0`). The bootstrap URL and local `%TMP%\r` contract are unchanged.

Runtime linking uses `/Brepro`, no debug information, and no generated build dates. For reproducibility, pin the MSVC/SDK versions, dependencies, environment, and `STATIC_ONEKB_RUNTIME` setting; compare SHA-256 hashes of unsigned clean builds. Cross-toolchain reproducibility is not promised. Size optimization retains `/GS`, ASLR, and DEP.

Release signing is a separate step: Authenticode-sign and timestamp the final `1KB-runtime.exe` (and builder) using the publisher's trusted certificate/service, verify the signature, then hash and publish those exact bytes. No signing credentials are stored here. Timestamped signatures are not byte-reproducible. The bootstrap does not enforce runtime signatures; signing does not guarantee antivirus acceptance.

```cmd
tests\test-updates.cmd
tests\test-bootstrap.cmd
tests\test-large-updates.cmd
```

The first suite covers the tiny manifest, snapshot comparisons, safe ZIP overlays, snapshot selection, in-place updates, and full repair. Bootstrap tests cover real launcher/runtime handoff and background/restart behavior. Direct-core tests additionally use a local HTTP server to exercise all four cores with valid/corrupt/missing canonical runtimes, successful/invalid/404 downloads, Unicode arguments, and a 256-character temporary path. Each case also runs with a test-only suspended background thread: bootstrap shutdown must call `ExitProcess`, not return and leave other threads alive. The large suite is slower and disk-intensive: it verifies a 3 GiB extracted application in a ~700 MiB ZIP plus x86 streaming encryption under a 96 MiB working-set bound. Publishing's live GitHub CLI orchestration and icon appearance are not automated.

After bootstrap changes, compare complete generated files and run:

```cmd
py -3 tools\pe-size-report.py file.exe
```

Keep size notes fixture-specific and concise; tests and measured artifacts are authoritative. Use `ONEKB_KEEP_CANDIDATES=1` to retain builder candidates.

With the production URL/toolchain and `/ORDERTRIES:1000`, GUI/console cores are 398/420 bytes; resource-capable cores are 452/474, before icon/identity insertion. GUI failure fallthrough and retaining its status in EBP save five bytes without removing `ExitProcess`. RET-only saves just 2/6 bytes and deterministically leaves surviving threads and locked images; no unsafe build mode is exposed. BSS is virtual-only: shrinking it does not itself save disk bytes.

See [bootstrap-size-pass.md](bootstrap-size-pass.md) for candidate/complete-launcher measurements, reproducible experiments, trust/compatibility costs, and the existing first-install integration failure. Set `KEEP_BOOTSTRAP_TESTS=1` to retain `.1KB-test` fixtures after the suite (including on failure).