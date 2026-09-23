# Image Scopes Property

**Image Scopes Property** is an official Canonical v1 Property plugin for [Seer](https://1218.io/), the file preview tool for Windows. It renders an RGB histogram, a luma waveform, and a Cb/Cr vectorscope with exposure statistics for the currently previewed image.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <file>` | an existing image file | required | The image to analyse. Populated automatically by Seer via `${input_file}`. |
| `--output <output-base>` | a path inside the request directory | required | Base path of the result; the helper publishes `<output-base>.json`. Populated automatically by Seer via `${output_file}`. |
| `--output-dir <request-directory>` | an existing directory | required | The host-assigned request directory; the result and the three PNG charts are only written inside it. Populated automatically by Seer via `${output_dir}`. |
| `--scopes <list>` | comma-separated `histogram`, `waveform`, `vectorscope` | all three | Which charts to render. An unknown name, a duplicate entry or an empty selection is rejected. |
