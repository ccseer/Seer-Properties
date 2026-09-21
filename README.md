# Seer Properties

Official property plugins for [Seer](https://1218.io/).

Every package here is an independently installable Canonical v1 process plugin.
Each deployed package is flat under the configured installation root:
`<installRoot>/<package>/plugin.json`, the helper executable, and whatever
runtime assets that helper needs.

## Plugins

| Package | ID | Matches | What it reports |
|---|---|---|---|
| [Image Scopes](./plugins/image-histogram-property) | `io.1218.seer.image-histogram` | `png jpg jpeg bmp webp tif tiff` | RGB histogram, luma waveform, Cb/Cr vectorscope, exposure statistics |
| [SHA-256](./plugins/sha256-property) | `io.1218.seer.sha256-property` | every file (`${type_file}`) | SHA-256 digest |
| [Git Info](./plugins/git-info-property) | `io.1218.seer.git-info` | folders (`${type_folder}`) | repository, branch, upstream, ahead/behind, change counts |
| [Digital Signature](./plugins/digital-signature-property) | `io.1218.seer.digital-signature` | `dll exe` | Authenticode verdict, signer certificate metadata |

## Dependency policy

* `git-info-property` and `digital-signature-property` use only the C++
  standard library, the single-header [nlohmann/json](./plugins/third_party)
  vendored in this repository, and the Win32 API (Windows 10 / 11). They link
  the static CRT and ship as one self-contained executable each, so the deployed
  package has no runtime dependency of its own.
* `image-histogram-property` additionally uses Qt 6.8 (Core/Gui) because it
  decodes image formats and rasterises charts; `windeployqt` supplies the
  required runtime assets at packaging time.
* `sha256-property` uses the Win32 BCrypt API only.

## Source layout

```
plugins/<package>/
  plugin.json     manifest consumed by the host
  src/            helper sources
  test/           unit, contract and integration tests
  CMakeLists.txt  standalone CMake project with CTest tests
plugins/third_party/nlohmann/json.hpp   single-header JSON (build-time only)
```

Build and test a package with its own CMake project; see the README in each
package directory for the exact commands, packaging steps and the documented
limits of that helper. `scripts/package-plugins.ps1` builds, tests and packages
every plugin in one step (ZIP plus SHA-256 per package).
