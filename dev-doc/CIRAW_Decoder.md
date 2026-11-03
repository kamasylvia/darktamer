# The CIRAW Decoder

An Apple-only build option (`--enable-ciraw`, `USE_CIRAW`) that replaces
every raw decoder with Apple Core Image RAW **v9**
(`CIRAWDecoderVersion9`, macOS 27+). There is no fallback: on a CIRAW
build, a camera outside the v9 list fails to open loudly. Default builds
behave exactly as before.

## Why a full replacement

Three decisions, taken together:

| Decision | Outcome |
|----------|---------|
| Scope | CIRAW v9 is the only raw loader on Apple builds that enable it |
| Pipeline | A new "Apple RAW" mode, not a reuse of `DT_IMAGE_S_RAW` |
| Coverage | Forced v9: cameras outside the list are an error, never a downgrade |

CIRAW v9 output is developed RGB, not sensor data: `meta.mm` probing of
`CIRAWFilter.properties` shows no `AsShotNeutral`, no color matrices, no
black/white level, no CFA pattern. White balance happens inside the
filter (`wb.mm`: `neutralTemperature` moves the channel ratios). Carrying
that output through the `S_RAW` path would hit four traps
(`src/common/image.c`, `src/iop/temperature.c`, `src/iop/rawprepare.c`,
`src/iop/colorin.c`, `src/develop/develop.c`): automatic white balance on
top of developed pixels, black/white rescaling of normalized data, a
camera matrix applied to RGB that already went through one, and the raw
exposure/filmic presets. So the build carries its own flag instead.

## The flag

`DT_IMAGE_CIRAW` (`src/common/image.h`, `1 << 23`): produced by an Apple
Core Image RAW decode, fully developed RGB with no sensor data and no
camera matrix. The loader clears `DT_IMAGE_RAW`/`DT_IMAGE_LDR` and sets
`DT_IMAGE_CIRAW`/`DT_IMAGE_HDR`.

The flag is deliberately absent from three predicates in
`src/common/image.c`, the master switch for automatic raw-module
enabling:

- `dt_image_is_raw`
- `dt_image_is_rawprepare_supported`
- `dt_image_is_matrix_correction_supported`

With those three false, `rawprepare`, `temperature`, `highlights` and
`demosaic` stay off by default and the raw exposure (+0.7EV) and
`FOR_MATRIX` (channelmixerrgb) presets never apply. The view transform
presets (`FOR_RAW|FOR_MATRIX|FOR_CIRAW` on sigmoid, filmicrgb, agx,
spektrafilm) do apply: `_dev_auto_apply_presets`
(`src/develop/develop.c`) sets `FOR_CIRAW` (`src/gui/presets.h`, `1 <<
6`) on CIRAW images, and `dt_presets_get_filter`
(`src/common/presets.c`) matches it symmetric extension with the matrix rule.
`src/common/image_cache.c` has an explicit CIRAW branch for the
`buf_dsc` rebuild (`channels=4`, `TYPE_FLOAT`, `IOP_CS_RGB`); with HDR
set, the existing HDR non-raw branch would rebuild the same descriptor,
so the branch is defensive rather than corrective.

History replay needs one more gate. `_dev_pixelpipe_synch`
(`src/develop/pixelpipe_hb.c`) writes each history entry's `enabled`
back onto the pipe, and its force-off list only names demosaic,
rawprepare, rawdenoise, hotpixels and cacorrect. An old library, a
cross-machine XMP or a pasted style carrying `temperature enabled=1`
would otherwise replay a second white balance onto developed pixels.
The fix force-disables `temperature` and `highlights` on CIRAW images
only, gated on `dt_image_is_ciraw`, so non-raw images in any build keep
their `sRGB`-fallback behavior and non-CIRAW builds are untouched. The
existing trouble-message mechanism tells the user the module was
disabled as not appropriate. Stored history rows keep `enabled=1`: on a
rawspeed build the same row is still correct.

## The loader

`src/imageio/imageio_ciraw.mm`, behind `HAVE_CIRAW`. The contract, in
short:

1. `[CIRAWFilter filterWithImageURL:]`; nil means
   `DT_IMAGEIO_UNSUPPORTED_FORMAT`.
2. `supportedDecoderVersions` must contain v9 (`CIRAWDecoderVersion9`,
   or `CIRAWDecoderVersion9DNG` for DNG); otherwise
   `DT_IMAGEIO_UNSUPPORTED_CAMERA`, never a downgrade.
3. `downloadResourcesWithTimeout:` (10 s) first: camera resources for new
   bodies may need an OTA download. Failure is `UNSUPPORTED_CAMERA` with
   a log line saying the machine needs to go online.
4. Development knobs split (`_ciraw_neutralize`): the detail stages dt
   owns (denoise, sharpening, contrast, gamut, lens) stay neutral, the
   tone/exposure group keeps its Apple defaults (`boostAmount`,
   `shadowBias`, `highlightRecoveryEnabled`, plus the always-default
   `baselineExposure`). A zeroed boost is a pure linear response, which
   the LDR pipe shows unmapped, dark and hazy; the tone curve is what
   makes first-open brightness sane. The probe scripts and this list
   agree exactly; regressions compare against it.
5. `orientation` set to Up, dimensions taken from the resulting
   `outputImage.extent`, not `nativeSize` (whether `nativeSize` includes
   the sensor crop is unverified; the extent always matches the buffer
   being allocated).
6. Rendered with `CIRenderDestination` (`kCIFormatRGBAf`) straight into
   the `dt_mipmap_cache_alloc` pointer: a 60 MP RGBA float frame is
   ~0.9 GB, a temp copy would double the peak. Alpha pinned to 0.
7. One process-wide `CIContext`, behind a mutex. `CIContext` thread
   safety is assumed from Apple's documentation and unverified; the
   lock is the backstop. Renders also take the lock.
8. `img->colorspace` left unset: an `SRGB` tag would make `colorin` pick
   the nonlinear `DT_COLORSPACE_SRGB` ahead of every other branch.
9. Fields: `channels=4`, `TYPE_FLOAT`, `filters=0`, `IOP_CS_RGB`,
   `raw_white_point=1`, `processed_maximum=1.0`, `LOADER_CIRAW` (18).
10. Probe call (`mbuf == NULL`): metadata only, no allocation.
11. Undecodable means `DT_IMAGEIO_UNSUPPORTED_FORMAT`.
12. Every function under `@autoreleasepool`; every v9 symbol under
    `if(@available(macOS 27.0, *))` (the deployment floor is 13.5).

`colorin` (`src/iop/colorin.c`) maps CIRAW images to
`DT_COLORSPACE_LIN_REC709` ahead of the DNG matrix branches. Without it,
a DNG's `d65_color_matrix` (filled independently by exiv2 at import,
not through the loader) would match `EMBEDDED_MATRIX` first and multiply
developed RGB by a sensor matrix. The same pollution source reaches
`temperature`'s fallback chain, so `_calculate_bogus_daylight_wb`
returns early on CIRAW images and a manually enabled temperature falls
back to the preset search instead of deriving coefficients from a DNG
matrix. Neural restore needs no change: its `RAW|S_RAW` gate in
`src/common/ai/restore.c` already rejects CIRAW images (neither bit is
set) with a clean unsupported verdict.

## The wiring

`src/imageio/imageio.c`, three changes under `HAVE_CIRAW`:

- `_should_decode_ciraw()`: extension gate. `dng` returns TRUE
  explicitly (it is absent from `_supported_raw[]`, so the list alone
  would miss it); everything else goes through
  `dt_imageio_is_raw_by_extension`.
- Magic bypass: in `_open_by_magic_number()`, raw signatures (rawspeed,
  libraw, the try-tiff-then-fallback entry) return
  `DT_IMAGEIO_UNRECOGNIZED` for CIRAW targets, handing the file to the
  fallback chain untouched. The 27-entry signature table itself is not
  modified.
- Fallback bisection: a CIRAW target goes to `dt_imageio_open_ciraw`
  only, with no tiff attempt, no rawspeed, no libraw, no exotic rescue;
  anything but `OK`/`CACHE_FULL`/`UNSUPPORTED_FEATURE` is normalized to
  terminal `UNSUPPORTED_FORMAT`, except `UNSUPPORTED_CAMERA`, which is
  kept for its distinct user-facing message. A non-raw target keeps the
  upstream tiff attempt plus exotic rescue plus the terminal line, minus
  the rawspeed/libraw segments, which have no legitimate target left in
  this build.

`dt_imageio_lookup_makermodel()` skips the rawspeed/libraw alias tables
under `HAVE_CIRAW`: camera names come straight from EXIF.

## Known boundaries

- darkroom shows its generic `UNRECOGNIZED` message only in what should
  be a bug scenario: every legitimate path in a CIRAW build either
  decodes or terminates in the fallback chain.
- HDR merge refuses CIRAW images twice, with two messages: the pre-check
  ("HDR merge aborted: ... is not a raw exposure",
  `src/control/jobs/control_jobs.c:714`) rejects non-raw, the authority
  ("exposure bracketing only works on raw images.", `:561`) rejects
  non-CFA buffers. Both fire on CIRAW.
- The tiff extension gate (`src/imageio/imageio_tiff.c`: non-`.tif`
  returns `UNSUPPORTED_FORMAT`) is what keeps the else branch safe: a
  raw file landing there fails the tiff attempt instead of being
  misread. A raw carried under a `.tif` extension stays on the upstream
  tiff path by design.
- Manually enabling `temperature` on a DNG-backed CIRAW image used to
  derive white balance from the DNG sensor matrix; the guard in
  `_calculate_bogus_daylight_wb` now forces the preset search (falling
  back to hardcoded coefficients when no preset matches). The manual
  enable itself is still allowed, and still meaningless on developed
  pixels.
- `makermodel` is not normalized (`main.cameras` groups raw EXIF names,
  so "SONY" and "Sony" split), and `wb_presets` exact matching misses
  more often, falling back to hardcoded coefficients. Both only matter
  with `temperature` manually enabled, which is off by default.
- `temperature` and `highlights` force-off on CIRAW history replay, with
  the standard "disabled as not appropriate" trouble message. The stored
  rows are untouched. A manually enabled `highlights` still runs, but
  commit clamps it to CLIP mode on any non-raw (`src/iop/highlights.c`).
- Flag lifetime: `DT_IMAGE_CIRAW` is written on first decode; until
  then, import tags raw extensions `DT_IMAGE_RAW`, so thumbnail-only
  browsing (embedded JPEG, no decode) shows the raw badge and matches
  raw collection filters. Entering darkroom forces the decode and flips
  the flag. DNG has no such window: it is absent from the extension
  table and imports unflagged.
- `fuji_rotation_pos` stays 0: the v9 Fujifilm list is X-T5/X-T50 only,
  neither needs the Super CCD rotation, so `rotatepixels` staying off
  is correct.

## Coverage and fallback

v9 covers 69 of 939 bodies (7.3%); the other 870 fail with the
unsupported-camera message. `UNSUPPORTED_CAMERA` means "supported file
type, camera not in the v9 list" (or resources need a download, or the
OS predates macOS 27), never "try another decoder": there is none.
Bit-identical repeat decodes hold within one machine and OS version
only; v7/v8/v9 pairwise means differ by 0.002-0.007, so an OS update can
change the rendering of the same file.

## Open verification

- `CIContext` thread safety under concurrent renders (lock held, Apple
  documentation assumed, not measured).
- `nativeSize` versus the sensor crop on bodies that crop.
- Pre-27 launch behavior of a CIRAW build (the `@available` gates should
  degrade every decode to `UNSUPPORTED_CAMERA`).
- The pixel integration suite does not run on CIRAW builds (see below);
  no CIRAW-specific reference images exist yet.

## Tests

`src/tests/unittests/imageio/test_ciraw.c` (cmocka, `USE_CIRAW`-gated):

- probe fills the field contract (loader, 4ch float RGB, valid extent,
  flags),
- full decode renders into a mipmap-shaped buffer with alpha at 0,
- two decodes are bit-identical,
- a non-v9 camera returns `UNSUPPORTED_CAMERA`.

No fixture ships: run cmake with
`-DCIRAW_TEST_SAMPLE=/path/to/v9-body.RAW`; without one the binary
exits 77 and ctest reports it skipped. The non-v9 default is the
integration suite's `mire1.cr2`.

The pixel integration suite (`src/tests/integration`, dE < 2 against
Bayer references) cannot pass on a CIRAW build: developed RGB through
an sRGB-neutral pipeline and sensor data through the raw pipeline are
different images by design. The integration workflow
(`.github/workflows/integration.yml`) skips the suite when
`darktable-cli --version` carries the `+ciraw` suffix (exit 0, "does
not apply" rather than failure); the select script
(`.ci/integration-select.py`) additionally maps the CIRAW-only file set
to no tests, so CIRAW-only changes select nothing even before the
runner exists. `run` itself has no guard: it lives in the integration
submodule and stays untouched.
