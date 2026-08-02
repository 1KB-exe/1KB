# Tiny icon PNG optimizer

The builder selects one icon image under the final 1024-byte launcher limit. Its exact RT_ICON budget is:

```text
1024 - selected resource-capable Crinkler core
     - 100-byte single-image resource carrier
     - 1 decoder-safety byte
     - encoded identity/encryption overlay
```

The finished launcher is checked again after packing and overlay append. The compact resource carrier remains one RT_GROUP_ICON entry and one RT_ICON image.

## Candidate selection

All authored ICO frames are decoded through WIC. Authored 48×48 and 32×32 frames are preserved rather than recreated from the largest frame. If either size is absent, a larger square authored frame may generate that size by downscaling only. Confident pixel art uses nearest-neighbor; other artwork uses WIC Fant. Generated upscales are never stored.

Candidates use fast original/WIC encodings for feasibility and ranking. Ranking strongly prefers lossless pixels, then authored over generated pixels, useful desktop dimensions, palette fidelity, and alpha fidelity. In particular, an authored lossless 48×48 image wins when it fits, an authored lossless 32×32 image is next, compact authored 32×32 is preferred to lossy 48×48, and 16×16 is a last resort. If no acceptable image fits, resource changes are discarded and the iconless core is used.

Exact candidates retain every alpha value and every visible RGB value. Lossy compact candidates use a separate policy: RGB is clustered without alpha, pixels below the transparency threshold map to one fully transparent palette entry, and all visible entries are opaque. This prevents scarce entries from being consumed by transparent-black alpha variants and prevents ghosted dark edges. Alongside ordinary RGB palettes, the search includes a saturated fixed-palette candidate derived from the earlier compact-icon path; it preserves strong black/gray/white and primary/dark colors for high-contrast artwork. Lossy candidates are compared using palette capacity plus measured premultiplied-color and alpha error, not encoded size or resolution alone.

The exhaustive `OptimizeTinyPng` serializer does not run for every frame, resize, and palette level. It runs once for the selected pixels. At most two higher-ranked candidates whose fast encoding is within 128 bytes of the budget may also receive exhaustive serialization when that can change fit.

## Lossless serializer

`src/icon-png-optimizer.cpp` finds the smallest complete PNG among its bounded representation, palette-order, filter, and ordinary-zlib candidates. “Smallest” applies to that enumerated set, not to every possible PNG encoder. A result is accepted only after WIC decoding confirms the intended dimensions and exact alpha/visible RGB pixels; RGB beneath alpha zero is insignificant.

The bounded search includes:

- exact grayscale at the smallest legal 1/2/4/8-bit depth;
- indexed color at the smallest legal 1/2/4/8-bit depth, with minimal PLTE and trailing opaque `tRNS` entries omitted;
- first-occurrence, alpha, frequency, and numeric palette orders;
- exhaustive palette permutations for at most eight colors, with only the best bounded subset receiving the full compression search;
- fixed None/Sub/Up/Average/Paeth filters and a per-row minimum-sum sequence;
- zlib levels 1/6/9, default/filtered/RLE strategies, and window sizes 9/10/11/15;
- one IDAT and minimal PNG framing.

Zopfli is not used.

## Measured examples

With the test identities used during implementation:

| Source | Build time | Launcher | Selected RT_ICON | Result |
|---|---:|---:|---:|---|
| Desktop Honey | 0.14 s | 857 bytes | 269 bytes, 48×48 | authored, lossless |
| DevilutionX/Diablo | 0.32 s | 927 bytes | 339 bytes, 32×32 | authored, lossy, 4-bit indexed |

The Diablo result has seven palette entries: transparent, `#000000`, `#808080`, `#C0C0C0`, `#FFFFFF`, `#800000`, and `#FF0000`. Transparency is binary, so it has no partially transparent black fringe and does not use muted RGBA centroids.
