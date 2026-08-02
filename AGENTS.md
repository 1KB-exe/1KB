This project is currently pre-release (`version 0`) and has no users.

- Do not preserve backwards compatibility with existing builds, launchers, configuration formats, caches, or internal contracts.
- Prefer removing or replacing obsolete code over adding migrations, fallbacks, legacy parsers, or compatibility branches.
- Do not bump internal format or contract versions for breaking changes yet; keep/reset them to version `1` where a version field is required.
- Optimize for the simplest clean implementation intended for the first public release.
- Keep `README.md` minimal and user-facing. Keep `docs/` concise and agent-facing; source and tests are authoritative.
- For small, simple changes, inspect only the directly relevant files. Do not read documentation, rebuild binaries, or run tests unless the change affects serious behavior, or verification is genuinely needed.
- Keep simple tasks lightweight: avoid broad repository exploration and full test suites when a targeted edit is sufficient.

The selling point is the smallest possible bootstrap launcher file size on disk.
