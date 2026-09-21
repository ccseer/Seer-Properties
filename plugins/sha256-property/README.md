# SHA-256 Property

This standalone package is a Canonical v1 process plugin that computes a
SHA-256 property for every regular file, including extensionless and Unicode
paths. Its manifest uses the host `${type_file}` matcher and writes a flat JSON
property file beside the requested output base path.

The package interface is fixed by `plugin.json`:

- backend: `process`
- capability: `property`
- file matcher: `${type_file}`
- command: package-relative `sha256_property.exe`
- arguments: `--input ${input_file} --output ${output_file}` (optional: `--case lower|upper`)
- output: `${output_file}.json`, containing `{"SHA-256":"<64 hex characters>"}` (lowercase by default)

### Output Case Customization

The helper executable accepts an optional `--case lower|upper` argument (defaulting to `lower`).
To configure uppercase hash output in Seer's plugin settings, override the capability arguments with:

```json
["--input", "${input_file}", "--output", "${output_file}", "--case", "upper"]
```

Build and validate the package with:

```powershell
cd plugins/sha256-property
cmake --preset default            # Ninja, Release; binaryDir under C:/Dev/build_output/Seer-Properties
cmake --build --preset default --target sha256_property_manifest_test
ctest --preset default -R sha256_property_manifest_test
```

Create a distributable package with `scripts/package-plugins.ps1` from the
repository root (it writes one ZIP per package
`plugins/<pkg>/<pkg>-<version>.zip` and prints its SHA-256), or package by hand
from inside the package directory:

```powershell
cmake --install "C:/Dev/build_output/Seer-Properties/sha256-property" --prefix dist
& "C:\Program Files\7-Zip\7z.exe" a -tzip -mx=9 -y sha256-property-1.0.0.zip .\dist\*
```

No `sha256-property-1.0.0.zip.sha256` is produced: the script verifies every
archive entry against `dist` and prints the checksum, so a committed sidecar
file could only go stale.

The manifest test stages both `plugin.json` and the helper, checks the exact
manifest tokens and package-relative command, then invokes the staged helper
to verify that a `base` output produces `base.json`.
