# darktamer

darktamer is a macOS-only fork of
[darktable](https://github.com/darktable-org/darktable), the open source
photography workflow application and non-destructive raw developer.

Everything that is not raw decoding is darktable. What darktamer changes is
one thing: on builds that enable it, raw files are decoded by Apple's
Core Image RAW **v9** pipeline instead of rawspeed.

Sibling project: [Lightamer](https://github.com/kamasylvia/Lightamer).

**Status: experimental.** The feature is off by default and Apple-only
(`--enable-ciraw`, cmake `-DUSE_CIRAW=ON`). Default builds behave exactly
like darktable. The decoder needs macOS 27 or newer; Core Image is
weak-linked so the app still starts on older systems.

## How a CIRAW build differs from darktable

### One raw decoder, and a hard camera allowlist

CIRAW is the only raw loader, and v9 is the preferred decoder: a camera
on Apple's v9 decoder list decodes with v9, and a camera outside the
list downgrades to the newest legacy version it offers (logged at open;
the decode stays inside the CIRAW pipeline either way). Only a camera
offering no CIRAW decoder version at all fails to open with an explicit
error, never a downgrade to another decoder. The class-level v9 list
currently covers 69 camera bodies (about 5% of the cameras rawspeed
supports: Canon 19, Fujifilm 2, Leica 1, Nikon 5, Panasonic 21, Sigma 3,
Sony 18), but a list entry does not guarantee a v9 decode: some entries
are named in a style no real body reports in EXIF. The Sony A7R V is
one such entry: the list has "Sony Alpha ILCE-7R V" while the camera
reports "ILCE-7RM5" in EXIF, so macOS never offers v9 for its files
(measured on macOS 27.0.1 and 27.3 beta 1; forcing v9 silently decodes
with v8) and the fork decodes it with v8. Flagship bodies such as the
Nikon Z8/Z9, Sony A1/A9 and Canon R5 Mark II are not on the list at all.
DNG files decode through the v9 DNG decoder when offered. Camera
resources for newly released bodies may need a one-time online download
before first decode.

### Developed RGB, not sensor data

Core Image RAW v9 hands over fully developed RGB: white balance happens
inside the filter, and there is no black/white level, no color matrix and
no CFA pattern to work with. Feeding that through darktable's raw modules
would double-process the frame, so a CIRAW build takes a different path:

- `rawprepare`, `temperature`, `highlights` and `demosaic` never run on
  these images, and the raw exposure and camera-matrix presets do not apply
- history replay force-disables `temperature` and `highlights`, so a stale
  XMP or pasted style cannot replay a second white balance onto developed
  pixels
- the scene-referred view transform presets (sigmoid, filmicrgb, agx,
  spektrafilm) do apply

### Apple decodes, darktable presents

Every Apple development knob is pinned to neutral (denoise, sharpening,
contrast, gamut, lens correction, and the whole tone-curve group). Two
exceptions, both deliberate:

- `highlightRecoveryEnabled` stays on: it is out-of-range hue
  reconstruction, not a curve, and turning it off renders clipped
  highlights magenta
- `extendedDynamicRangeAmount` stays at 2.0: an output-range declaration
  that keeps HDR headroom, which is why these images open flagged as HDR

The tone presentation is dt's view transform, so the image you see is one
Apple decode plus one darktable curve, not two curves in series.

### Compatibility

- macOS only. Linux, FreeBSD and Windows builds cannot enable the feature
  and are unaffected
- the version string carries a `+ciraw` suffix
- a given raw renders differently than in darktable, and XMP/history for
  CIRAW images is not interchangeable between the two
- neural restore (AI denoise) rejects CIRAW images explicitly

## Everything else

Module set, lighttable, darkroom workflow, database, export, Lua and
shortcuts are darktable, unchanged. Documentation lives upstream:
the [user manual](https://github.com/darktable-org/dtdocs) and the in-tree
developer docs in [`dev-doc/`](dev-doc/README.md), which include
[`dev-doc/CIRAW_Decoder.md`](dev-doc/CIRAW_Decoder.md) for the decoder's
design and contracts.

## Building

Same as darktable, plus:

```sh
./build.sh --enable-ciraw            # CIRAW build (macOS only)
./build.sh                           # default build, identical to darktable
```

Requires macOS 27+ SDK at build time for the decoder paths; the deployment
floor stays at 13.5.

## Upstream

This fork tracks `darktable-org/darktable` and intends to keep merging it.
Bugs in anything that is not the CIRAW decoder belong in the
[upstream tracker](https://github.com/darktable-org/darktable/issues);
CIRAW-specific issues go to this fork's tracker.

Sync is one-way: updates flow from upstream into this fork, and no code
from this fork is ever contributed back upstream.
