# Development

Agent reference. Requires Windows, Python 3, Visual Studio 2022 C++ x64/x86 tools, MASM, and the checked-in UPX/Crinkler tools.

```cmd
build-1KB.cmd
```

This replaces repository-root `1KB.exe` and publishable runtime `r`.

```cmd
tests\test-bootstrap.cmd
```

The suite is intentionally limited to serious executable correctness: launcher identity decoding, payload encryption, console/GUI/iconless bootstrap handoff, installation, and update activation/failure behavior. TUI flows, publishing orchestration, and icon appearance are not covered.

After bootstrap changes, compare complete generated files and run:

```cmd
py -3 tools\pe-size-report.py file.exe
```

Keep size notes fixture-specific and concise; tests and measured artifacts are authoritative. Use `ONEKB_KEEP_CANDIDATES=1` to retain builder candidates.