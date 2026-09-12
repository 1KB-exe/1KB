# Architecture

Agent reference. Source and tests are authoritative.

## Components

- `1KB.exe`: deployment-manager TUI and launcher builder.
- Generated launcher: tiny x86 bootstrap with an app-ID overlay and, for private apps, an 8-byte package secret.
- `1KB-runtime.exe`: x86 launcher runtime, installed as `%TMP%\r`.
- Installed launchers: stable entry points in each runtime-managed application root.

There is no publishing CLI. GitHub apps publish through `gh`; direct URL apps prepare the same files beside their generated launcher for static hosting.

## Releases

The current remote `1KB.ini` is deliberately small:

```ini
version=1.2.4
download=app-v1.2.0.zip
changes=app-v1.2.4-changes.zip
updates=background
```

`changes` and `updates` are optional; update behavior defaults to `background`. Relative references resolve beside the manifest and absolute HTTPS references retain their existing meaning. GitHub's latest `1KB.ini` points directly to its snapshot and optional cumulative changes asset; clients never inspect release history.

Snapshot and update assets use a constrained standard ZIP: regular-file local records and payloads first, followed by their matching central directory and EOCD. Local headers contain canonical UTF-8 paths, methods, CRC32 values, and final sizes, with no data descriptors; ZIP64 size extras appear only when an uncompressed file exceeds 32 bits. Directories are implied. Stored, Deflate, and ZIP-LZMA payload bytes are copied from validated source ZIPs without recompression. Public assets are ordinary `.zip` files; private `.1KB` assets encrypt those same ZIP bytes. An update package contains complete replacement/addition files; deletions remain repeated `delete=path` lines in the consumed `1kb.updates.ini` entry.

The first publish is a snapshot. Later publishes default to Update, with one `Full snapshot` choice in the normal TUI flow. The manager stores the active snapshot's private hash inventory in `snapshot.idx` and cumulative changed-path history in `changes.idx`; `app.ini` contains only normal app and publishing configuration. Updates compare the current build with the snapshot and retain previously changed paths so later packages can restore snapshot bytes. A new snapshot regenerates the inventory and clears the changed paths. Public manifests contain no inventory, hashes, sizes, entry point, history, or authentication field.

Private `.1KB` packages retain authenticated AES-256-GCM encryption. The package authentication context binds the app ID and immutable package filename. Manifests do not add a second authentication protocol.

## Runtime

Installed application state is visually simple:

```text
<app-root>\
    launcher.exe
    1kb.ini
    current.txt
    <current-version>\
        <application files>
```

Publisher projects remain under `%LOCALAPPDATA%\1kb\<identity-path>`, runtime-installed applications remain in their separate existing `%LOCALAPPDATA%\<identity-path>` roots, and the executable runtime remains `%TMP%\r`. `current.txt` contains only the runnable semantic version. `1kb.ini` holds the display name, update behavior, HTTP validators, and the snapshot's `download` reference. The runtime locates an unambiguous executable in the current version directory and exports `ONEKB_VERSION_FILE` as the path to `current.txt`.

After installation, the runtime creates a per-user Start Menu shortcut and HKCU uninstall registration. Both target the stable installed `launcher.exe`, which bootstraps `%TMP%\r` and therefore retains normal update handling. The exact internal argument `__1KB_UNINSTALL__` is intercepted by the runtime; it stops the installed app when possible and removes only the shortcut, registration, and managed installation root.

If local and remote `download` values match, the runtime streams the cumulative changes package into a small `.update\changes` tree while the application runs. Once it exits, the runtime renames the version directory, overlays prepared replacement files, applies deletions, and atomically writes `current.txt`. Unchanged application bytes are neither downloaded nor copied.

If `download` changed, no installation is usable, or an earlier in-place update was interrupted, the runtime repairs forward from the one current snapshot plus the one current changes package. Full installation extracts into the target version directory, applies the overlay when present, and then activates it. There are no patch chains, historical manifests, inventories in installed state, backups, staging trees, journals, rollback records, or permanent package caches. Temporary `.update` work is removed after each attempt; obsolete version directories are removed after success.

WinHTTP feeds the sequential parser directly. Public bytes go straight to the Deflate/LZMA decoder and destination files. Private bytes first pass through Windows CNG's chained AES-GCM decryption; the one package tag is finalized before activation, and authentication failure deletes the disposable staging tree. No downloaded or decrypted archive is written. Publisher-side ZIP parsing rejects unsafe paths, links, collisions, malformed records, unsupported compression, and configured file/expanded-size limits before payload reuse; the runtime parser enforces the corresponding sequential checks per entry.

## Launcher identity

Canonical IDs are `gh:owner/repository[#app]` or `url:<absolute URL>[#app]`. They select installation directories and mutexes independently of display names. The compact launcher overlay encoding and Crinkler bootstrap layout are documented by `src/overlay-identity-model.h`, its tests, and `docs/icon-crinkler.md`.
