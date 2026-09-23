# SHA-256 Property

**SHA-256 Property** is an official Canonical v1 Property plugin for [Seer](https://1218.io/), the file preview tool for Windows. It computes the SHA-256 digest of the currently previewed file.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <file>` | an existing regular file | required | The file to hash. Populated automatically by Seer via `${input_file}`. |
| `--output <output-base>` | a path inside the request directory | required | Base path of the result; the helper publishes `<output-base>.json`. Populated automatically by Seer via `${output_file}`. |
| `--case <case>` | `lower`, `upper` | `lower` | Letter case of the published hex digest. Override the capability arguments in the plugin settings to use `upper`. |
