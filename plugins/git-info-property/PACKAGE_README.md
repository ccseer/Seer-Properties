# Git Info Property

**Git Info Property** is an official Canonical v1 Property plugin for [Seer](https://1218.io/), the file preview tool for Windows. It reports repository information for the currently previewed folder by running the Git installation found on the machine.

## Options & Arguments

| Option | Values | Default | Purpose / Description |
|---|---|---|---|
| `--input <directory>` | an existing folder | required | The folder to inspect. Populated automatically by Seer via `${input_file}`. |
| `--output <output-base>` | a path inside the request directory | required | Base path of the result; the helper publishes `<output-base>.json`. Populated automatically by Seer via `${output_file}`. |
| `--output-dir <request-directory>` | an existing directory | required | The host-assigned request directory; the result is only written inside it. Populated automatically by Seer via `${output_dir}`. |
| `--git <path-to-git.exe>` | an absolute path to an existing executable | absent | Use this `git.exe` instead of searching the `PATH` entries. |
