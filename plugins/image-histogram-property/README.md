# Image Scopes Property Plugin

This standalone package is a Canonical v1 process plugin that computes RGB
histograms, a luma waveform, and a Cb/Cr vectorscope for supported image
formats in a single invocation. Its manifest declares a Canonical Property
invocation using `result_schema: 1` and produces exposure summary metrics plus
three attached rendered graphs (`rgb-histogram.png`, `waveform.png`, and
`vectorscope.png`).

The package interface is defined by `plugin.json`:

- backend: `process`
- capability: `property`
- extensions: `png`, `jpg`, `jpeg`, `bmp`, `webp`, `tif`, `tiff`
- command: package-relative `image_histogram.exe`
- arguments: `--input ${input_file} --output ${output_file} --output-dir ${output_dir}`
- output: `${output_file}.json` with `result_schema: 1`, plus three PNG
  attachments in `${output_dir}`
- Y-axis scale: `log1p`

## Scopes

By default all three scopes are produced. An optional `--scopes` selector
accepts a comma-separated subset of `histogram`, `waveform`, `vectorscope`.
Unknown scope names, duplicate entries, or an empty selection are rejected.
Existing argument overrides remain valid because the default covers all scopes.

- **RGB Histogram**: 256 bins per channel, per-channel log1p normalization.
- **Luma Waveform**: horizontal axis is the source image's horizontal position;
  vertical axis is luma intensity (black at the bottom). Density is accumulated
  on a fixed 256x256 grid (log1p-scaled), preserving spatial structure.
- **Vectorscope**: fixed Cb/Cr coordinates with BT.709 coefficients. Cb is
  horizontal, Cr vertical, neutral at the center. The per-image chroma extent is
  never rescaled to fill the plot, so low- and high-saturation images remain
  distinguishable. Red, green, blue, cyan, magenta, and yellow reference markers
  are drawn at their fixed projections.

## Analysis convention

This is an 8-bit decoded-RGB analysis, not an HDR, scene-linear, or fully
color-managed measurement. Inputs are decoded once and explicitly converted to
non-premultiplied 8-bit RGB(A). Luma uses BT.709 coefficients
(`Y' = 0.2126R' + 0.7152G' + 0.0722B'`) and Cb/Cr use
`Cb = (B'-Y')/1.8556`, `Cr = (R'-Y')/1.5748`. For animations, the first frame is
analyzed; EXIF orientation is preserved by the decoder.

## Result shape

One plugin, one group: every row (dimensions, means, clipping, shadow,
`Histogram Y Scale`, the analysis convention, and `Status`/`Reason` for domain
outcomes) **and** the three chart fields are published inside the single `Image
Scopes` subgroup, so the Inspector shows the plugin's whole result in one
section and `data` carries exactly one key.

The subgroup value is an **array of one-key fields** published in the plugin's
own order - statistics, `Histogram Y Scale`, then the three charts - and the
Inspector renders the array in that sequence rather than in the alphabetic order
of the keys. (JSON objects are key-sorted and cannot carry an order, which is why
the fields are published as an array.) Each chart is
`{"type":"image","value":"<file>.png"}`. This requires a host whose property
parser resolves typed values inside a subgroup and preserves array order
(`PropertyCapabilityHandler::parseSchema1`); older hosts collapse the whole group
into a single empty text row, losing the statistics and the charts.

**Release prerequisite.** `appMinVersion` is still `4.5.10`, which predates that
host support. It must be raised to the first host release that carries the
subgroup-chart change before 1.1.0 is published; the number cannot be written
earlier, and publishing to an older host loses the charts silently while the
statistics keep rendering (tracked in `docs/RESULTS.md` section 5, item 2).

## Domain outcomes

A missing, unsupported, undecodable or oversized input is an expected domain
outcome: the helper exits `0` and publishes `result_schema: 1` JSON whose only
rows are `Status` and `Reason` inside the `Image Scopes` subgroup, so the
Inspector shows an explanation instead of nothing. `Status` is `ReadError`
(missing or not a regular file),
`UnsupportedOrMalformed` (unsupported extension, undecodable or dimension-less
image), or `QueryError` (over the pixel/byte budget, or over the internal
deadline). Exit nonzero is reserved for a malformed invocation and for a
publication failure, such as an attachment that cannot be written - in that case
no result JSON is published at all.

## Resource safety

Inputs larger than 100,000,000 pixels or whose estimated allocation exceeds
512 MiB are rejected before decoding. The fixed waveform and vectorscope grids
are bounded independently of the input size. The analysis also runs under an
internal deadline of 20000 ms, below the manifest `timeout_ms` of 30000; when it
is exceeded the analysis is abandoned at the next step boundary (Qt offers no
way to interrupt a decoder) and a `QueryError` result is published.

## Dependencies

Qt 6.8 Core/Gui is the only dependency: it provides the image codecs the
manifest declares (including WebP and TIFF) and the rasterizer for the three
charts. JSON output is produced with `QJsonDocument`. The three analyses share
one decoded image, so no additional full-resolution copy is created per scope.

## Tests

- `image_histogram_test` - per-pixel histogram statistics, including a
  256-step gray-ramp baseline and premultiplied input handling.
- `image_histogram_analysis_test` - waveform and vectorscope grids: spatial
  tracking, neutral center, primary/secondary color positions, fixed chroma
  extent, premultiplied and 16-bit normalization, and agreement between the
  shared analysis entry point and the single-scope helpers.
- `image_histogram_renderer_test`, `image_histogram_scope_renderer_test` -
  chart dimensions, determinism, and pixel-level orientation checks that keep
  the density plot and the reference markers consistent.
- `image_histogram_output_test` - CLI contract, attachment publication,
  request-directory containment, `<output-base>.*` discovery, attachment
  failure handling, the domain `Status`/`Reason` outcomes, the internal
  deadline, and gray-ramp/alpha/high-bit-depth end-to-end fixtures.
- `image_histogram_manifest_test` - manifest contract plus a staged-helper run.

## Build and test

The build needs Qt 6.8 MSVC x64. A machine-local `CMakeUserPresets.json` in
this package directory already points the build to
`C:/Dev/build_output/Seer-Properties/image-histogram-property`
(Ninja, Release) and supplies `CMAKE_PREFIX_PATH` for the local Qt install.
Run the commands from inside the package directory; the Ninja generator needs
the MSVC environment on PATH (see
`<build-root>/msvcenv.sh`):

```powershell
cd plugins/image-histogram-property
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Without the local preset file, configure with an explicit out-of-repo `-B`
directory and `-DCMAKE_PREFIX_PATH=<Qt install>`.

## Packaging

Run `scripts/package-plugins.ps1` from the repository root. It installs the flat
package tree, runs `windeployqt` restricted to the codecs the manifest declares,
bundles `vc_redist.x64.exe` (the helper links the dynamic CRT, exactly as the
1.0.0 package did) and writes `image-histogram-property-<version>.zip`. Every
archive entry is verified against the installed tree and the SHA-256 is
printed; no sidecar checksum file is written. The equivalent manual deployment
step is:

```powershell
& "<qt-prefix>/bin/windeployqt.exe" --release --no-translations `
  --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw --no-ffmpeg `
  --no-network --no-svg --no-pdf `
  --exclude-plugins qpdf,qsvg,qicns,qwbmp,qtga,qsvgicon `
  --dir plugins/image-histogram-property/dist `
  plugins/image-histogram-property/dist/image_histogram.exe
```

The final package contains a single package-root `plugin.json`, the helper
executable, and the required Qt runtime assets (including image-format plugins
for WebP and TIFF) under the configured installation root.