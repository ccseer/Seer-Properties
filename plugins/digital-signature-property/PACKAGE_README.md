# Digital Signature Property

**Digital Signature Property** is an official Canonical v1 Property plugin for [Seer](https://1218.io/), the file preview tool for Windows. It performs a read-only Windows Authenticode query for the currently previewed `.dll` or `.exe` file and reports the verification outcome and the signer certificate metadata.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <file>` | an existing `.dll` or `.exe` file | required | The file to verify. Populated automatically by Seer via `${input_file}`. |
| `--output <output-base>` | a path inside the request directory | required | Base path of the result; the helper publishes `<output-base>.json`. Populated automatically by Seer via `${output_file}`. |
| `--output-dir <request-directory>` | an existing directory | required | The host-assigned request directory; the result is only written inside it. Populated automatically by Seer via `${output_dir}`. |
