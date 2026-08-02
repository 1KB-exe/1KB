# Architecture

Agent reference. Source and tests are authoritative.

## Components

- `1KB.exe`: 64-bit deployment manager/builder containing x86 bootstrap templates.
- Generated launcher: tiny x86 bootstrap plus an app-ID overlay (and, for immutable private-launcher deployments, an 8-byte payload secret).
- `r`: extensionless x86 GUI runtime downloaded to `%TMP%\r`.
- `%LOCALAPPDATA%\1KB`: manager records, generated launchers, and shared runtime metadata. Installed app state uses publisher/host directories directly under `%LOCALAPPDATA%`.

Deployment management is linked only into the builder, never `r` or generated launchers. With no arguments the builder opens the dashboard. Passing or dropping a release EXE, ZIP, or folder enters the same remember-and-build flow as dashboard Add. Passing a generated launcher opens its remembered deployment, or the dashboard if it is not remembered locally. There are no public CLI commands.

## Deployment manager

Each deployment is under `%LOCALAPPDATA%\1KB`; GitHub IDs use `<owner>\<repository>` or `<owner>\<tag>`, while URL IDs retain their local app-name directory. Remembered app names are unique case-insensitively, but renaming one does not change its identity-based GitHub directory. `app.ini` is the single strict UTF-8 local record. It contains required canonical `app_id`, one EXE/ZIP/folder `release`, and `name`, plus optional prepared/observed `version`, `download`, remote-settings hash, repeated `extra_release` paths, and non-default `remove_icon`, immutable `encryption`, and `updates` settings. The generated launcher, payload, and uploadable `1KB.ini` are stored beside it. Writes use atomic replacement. GitHub CLI owns authentication.

Adding, editing, building, opening, and forgetting local deployments do not require GitHub CLI. Canonical `gh:` app IDs support publishing and refresh. Canonical `url:` IDs support preparing a manual upload: the manager packages or encrypts the payload, writes it and `1KB.ini` beside the launcher, and shows their target URLs without transferring them. GitHub publishing verifies that the repository exists and is public, validates the release, shows a non-mutating preview, and requires the exact target version. Executables and folders are packaged as ZIPs; existing ZIPs are copied. Repeated `extra_release` entries add sidecar GitHub assets. Publishing creates a draft semantic-version release, uploads every asset, then publishes it as latest. Publishing retains the immediately previous payload while replacing the active `1KB.ini` only after payload upload succeeds.

The bootstrap locks a global mutex, starts `%TMP%\r`, and on failure downloads `https://r.2v2.me` directly over it and retries. It forwards the original command line. App installation and all updates belong to the runtime.

## Launcher contract

Launcher identity is compact only at the overlay serialization boundary. Public and private layouts are:

```text
[complete PE][encoded identity body][typed trailer]
[complete PE][encoded identity body][8-byte random secret][typed trailer]
[complete PE][C0]  # public gh:1KB-exe/1KB only
```

The final typed byte is always EOF. Bits 7–6 select GitHub (`00`), HTTPS (`01`), HTTP (`10`), or the built-in namespace (`11`); bits 5–0 hold encoded-body lengths 1–62 for the first three kinds. For lengths 63–255 they are `0x3f`, with the exact 8-bit length immediately before the final byte (and after the private secret). There is no larger form.

`0xC0` is the sole assigned built-in value: its complete overlay is exactly that one byte and reconstructs the exact canonical identity `gh:1KB-exe/1KB`. It is public-only and has no body or extended length. A private launcher for the same identity uses the ordinary GitHub body, 8-byte secret, and typed trailer. Every other `11` value (`0xC1`–`0xFF`) is invalid. Decoding also rejects zero ordinary lengths, malformed GitHub streams, noncanonical extended lengths, and truncation.

Before body encoding, `gh:`, `url:https://`, or `url:http://` is removed. GitHub bodies use the frozen `github-owner-reference-huffman` codec: canonical Huffman coding plus compact repository references into the owner. URL bodies use the frozen trained Huffman/token/copy codec; `#` is a direct structural symbol, and lowercase `1kb` and branded `1KB` are equal-weight tokens.

The decoder reconstructs the scheme, enforces the 4096-byte canonical limit, and invokes the same authoritative app-ID parser used everywhere else. Existing callers therefore receive only canonical identities. Encoded bytes never escape overlay serialization: all runtime and builder APIs continue to use canonical identities. Private launchers retain the secret directly after the encoded body. The bootstrap reads neither overlay.

Canonical IDs:

- `gh:owner/repository` (lowercase, except branded `gh:1KB-exe/1KB`)
- `url:<absolute URL>[#app]` (normalized scheme/host; lowercase optional app tag)

The builder also accepts a GitHub repository URL or bare manifest URL. The ID permanently selects the app directory and mutexes. Filename, icon, packing, and manifest contents never affect identity. Changing a direct manifest URL changes `url:` identity.

## Application source

`gh:` reads `https://github.com/owner/repository/releases/latest/download/1KB.ini`; an app key instead selects `https://github.com/owner/repository/releases/download/app/1KB.ini`.

Manifest format is strict UTF-8 `key=value` lines:

```ini
version=1.2.3
download=example-app-v1.2.3.zip
updates=background
```

`version` and `download` are required. Encryption is immutable launcher state and is not represented in the manifest: public launchers require `.exe` or `.zip`, while private launchers require `.1KB`. `updates` is `background`, `before-launch`, or `restart` and defaults to `background`. Downloads may be relative to the manifest URL, root-relative, or absolute HTTPS URLs (HTTP only for localhost). Plaintext is at most 100 MiB; `.1KB` adds its fixed 56-byte overhead. Invalid recognized fields reject the whole manifest.

A ZIP must yield an unambiguous app executable: only executable, only root executable, or only GUI executable. The update UI title is the launcher's filename without `.exe`; project `name` remains local builder metadata.

## Private launcher payloads

Encryption mode is selected when a deployment is created and cannot be changed. A private launcher receives one CNG-generated 8-byte secret on its initial build; rebuilds preserve that secret, and publishing only reads it from the launcher. Losing the launcher and its secure backup prevents compatible publishing. Leaking it reveals every release encrypted from that secret; recovery requires a new deployment and launcher. For each payload, PBKDF2-HMAC-SHA-256 derives a 32-byte AES key from the launcher secret using 100,000 iterations and the complete payload AAD as salt/context. The AAD includes the random nonce, canonical app ID, and release version, preventing precomputation across payloads. The canonical app ID remains the sole application identity and is not secret key material.

An `.1KB` is `header || ciphertext || tag`. Its 40-byte header is serialized little-endian: `1KPACK1\0` (8 bytes), version `u16=1`, header size `u16=40`, plaintext ZIP length `u64`, random 12-byte nonce, and eight zero reserved bytes. A 16-byte GCM tag follows ciphertext, so total overhead is 56 bytes. AES-256-GCM AAD is the complete serialized header followed by `1KAAD1\0\0`, length-prefixed UTF-8 canonical app ID, and length-prefixed UTF-8 release version. Thus a payload cannot authenticate under another application identity or manifest version without adding container bytes. Every publication uses a fresh CNG nonce while retaining the launcher secret. Files are exposed to CNG through page-backed file mappings rather than copied into whole-file heap buffers, and lengths are bounded at 100 MiB before cryptographic work.

Publishing validates and stages the ordinary ZIP, encrypts it to an exclusively created `.1KB.tmp`, flushes and validates the container, atomically renames it, deletes plaintext, uploads payload and public extra assets, and uploads the public manifest last. The private launcher is never uploaded.

The runtime obtains the launcher path through the argv/bootstrap handoff, parses the overlay directly, downloads and validates the `.1KB`, decrypts to a unique temporary ZIP, and completes GCM authentication before ZIP listing, extraction, staging, or activation. Header, ciphertext, secret, derived-key, and tag failures remove ciphertext, plaintext, and staging files while leaving the active version unchanged. Background updates use the same path without UI. Secrets and derived keys are never placed in arguments, environment variables, manifests, or logs.

## State and processes

Installed app state is outside the `%LOCALAPPDATA%\1KB` manager directory:

```text
%LOCALAPPDATA%\owner\repository\                 # gh:owner/repository
%LOCALAPPDATA%\owner\app\                        # gh:owner/repository#app
%LOCALAPPDATA%\host\app\                         # url:https://host/manifest#app
%LOCALAPPDATA%\host\<canonical-url-hash>\        # URL without an app tag
```

A non-default URL port is represented as `host@port`. URL app tags use the same lowercase app-key syntax as GitHub app keys and are removed before HTTP requests. Names must be unique within their owner or host namespace. Each app directory contains version directories, `current.txt`, and optional local `1KB.ini` state. It contains reflected non-default manifest settings such as `updates` plus accepted HTTP validators; versions and payload URLs are not cached.

The runtime reads the overlay from resolved `argv[0]`, detects the PE subsystem, and exposes `ONEKB_PATH`, `ONEKB_VERSION`, and `ONEKB_VERSION_FILE` to the app. `ONEKB_VERSION` is the running version. The version file is a read-only ASCII `major.minor.patch` line atomically replaced only after a version is fully installed and activated; an app may poll or watch it and offer a restart when its value differs from the running version. Console launchers inherit standard handles and return the app exit code.

Application manifest and runtime updates use ETag and Last-Modified conditional GETs. ETag takes precedence and Last-Modified is the fallback. Application validators are committed only when the advertised version is already active or installs successfully. A downloaded x86 GUI runtime enters private apply mode, waits for its parent, stages and validates its own image, atomically replaces `%TMP%\r`, then commits its validators. Runtime checks remain throttled independently. There is no signature, pinning, or cryptographic payload verification.

## Bootstrap packing

The builder serializes and validates exactly one production representation, then appends the compact overlay. The fixed `url:http://localhost:12345/1KB.ini` fixture exercises the canonical URL representation.

- A genuine copied icon uses the resource-capable Crinkler zero-section core. Its directly serialized three-level resource tree overlaps PE fields, ignored directory prefixes, and ignored data-entry fields before insertion ahead of the remaining depacker/stream; a decoder-safety byte follows the stream.
- `remove_icon=yes`, or an application with no genuine icon, uses the bootstrap-specific Crinkler core plus its required decoder-safety byte.
- If copied icon resources cannot satisfy the compact serializer's invariants, launcher generation fails instead of retaining a larger conventional format.
- Crinkler output is a nonstandard zero-section PE. Resource-update APIs are used only on the conventional temporary template, never on packed output.

The icon format intentionally depends on Crinkler-style loader behavior: `e_lfanew=4`, zero sections, four-byte alignments, instruction/header overlap, `TINYIMPORT`, and resource directory-header overlap. See `icon-crinkler.md` for the byte map and tested matrix.

Preserve fixed base, no relocations, GUI/console semantics, loader-visible resources, and post-selection overlay writing. Optimize complete launcher size, not compressed stream size.
