# Seer Property Plugin Development Specification & Reference Guide

This guide instructs AI Agents on how to develop and package custom **Property Plugins** for Seer, the Windows Quick Look tool.

---

## 🔗 Reference Links

- **Official Documentation**: [https://1218.io/docs/seer/create-plugin.html#property-plugins](https://1218.io/docs/seer/create-plugin.html#property-plugins)
- **Property Plugins Repository**: [https://github.com/ccseer/Seer-Properties](https://github.com/ccseer/Seer-Properties)
- **Official Plugin Downloads**: [https://1218.io/docs/seer/download.html#properties](https://1218.io/docs/seer/download.html#properties)

---

## 🛠️ Property Plugin Architecture Overview

Property plugins extend Seer's file inspection capabilities. When a user opens the Properties inspector window (via shortcut `Ctrl + I` or right-clicking a file), matching property plugins extract and display structured metadata, attributes, and graphical scopes/charts.

### Execution Workflow

1. **Trigger**: The user presses `Ctrl + I` on a previewed file or folder. Seer identifies matching property plugins based on file extensions or special matcher tokens (`${type_file}`, `${type_folder}`).
2. **Launch**: Seer launches the plugin process with command-line arguments:
   `--input "${input_file}" --output "${output_file}" --output-dir "${output_dir}"`
3. **Execution**:
   - The plugin inspects `${input_file}`.
   - If generating visual charts (e.g. histograms, waveforms), the plugin saves PNG images inside `${output_dir}`.
   - The plugin compiles the structured metadata into JSON according to **Result Schema 1**.
4. **Publish Output**:
   - Seer expands `${output_file}` to `<requestDir>/<md5>` **without an extension**.
   - The plugin **must** write its JSON result to `<output-base>.json` (i.e. `${output_file}.json`).
   - Write attachments first and the final `.json` last (atomically).
5. **Exit Code Policy**:
   - **Exit `0`** with valid Schema 1 JSON for all expected domain outcomes (e.g. unsigned binary, not a git repository, empty metadata, or partial result before timeout).
   - Exit non-zero **only** for invalid invocations or fatal process errors. (A non-zero exit causes Seer to wipe the output directory and discard any partial output).

> [!CAUTION]
> - `${no_cache}` is **strictly forbidden** for property plugins.
> - Always write output inside `${output_dir}` or `<output-base>.json`. Never write temporary files next to the source file.

---

## 📄 Command-Line Placeholders & Matchers

### Command-Line Placeholders

| Placeholder | Description | Example |
| :--- | :--- | :--- |
| `${input_file}` | Absolute path of the inspected file or folder. | `C:\Repo\my_project` |
| `${output_file}` | Host-assigned base file path (append `.json` when writing output). | `C:\Users\Name\...\md5_hash` |
| `${output_dir}` | Host-assigned temporary directory for attachments (charts, images). | `C:\Users\Name\...\request_dir` |
| `${use_backslash}` | Optional flag to format path separators using Windows native backslashes (`\`). | `${use_backslash}` |
| `${seer_dir}` | Directory containing `Seer.exe`. | `C:\Program Files\Seer` |
| `${seer_exe}` | Absolute path to `Seer.exe`. | `C:\Program Files\Seer\Seer.exe` |
| `${7z}` | Absolute path to `7z.exe` bundled with Seer. | `C:\Program Files\Seer\plugins\7z.exe` |

### File Matcher Tokens in `extensions`

In addition to specific file extensions (e.g. `"png"`, `"exe"`, `"dll"`), property plugins can match categories using special tokens:

| Token | Description |
| :--- | :--- |
| `${type_file}` | Matches all regular files (including extensionless files). |
| `${type_folder}` | Matches all directories and folders. |
| `${type_all}` | Matches both files and folders. |
| `${type_image}` | Matches images recognized by Seer's native image viewer. |
| `${type_media}` | Matches audio and video formats. |
| `${type_web}` | Matches HTML and Markdown web files. |
| `${type_text}` | Matches text, code, and config files. |
| `${type_pdf}` | Matches PDF documents. |
| `${type_none}` | Matches files outside the categories above (e.g. archives, executables). |

---

## 📊 Result JSON (Schema 1) Specification

The result file `${output_file}.json` must be strictly valid UTF-8 JSON matching Schema 1.

### 1. Root Structure
```json
{
  "result_schema": 1,
  "data": {
    ...
  }
}
```

### 2. Output Formats

#### Form A: Flat Key-Value Rows
Used when the entire plugin produces only one or two simple rows (e.g. `SHA-256`):
```json
{
  "result_schema": 1,
  "data": {
    "SHA-256": "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
  }
}
```

#### Form B: Subgroup Rows (Recommended)
Encapsulating output inside a subgroup titled after the plugin namespaces rows and prevents key collisions across plugins:
```json
{
  "result_schema": 1,
  "data": {
    "Git": {
      "value": {
        "Branch": "main",
        "Upstream": "origin/main",
        "Status": "Clean"
      }
    }
  }
}
```

#### Form C: Subgroup with Ordered Array of Single-Key Objects (Recommended for Strict Sequencing)
When row order in the UI must be deterministic, supply `value` as an array of single-key objects:
```json
{
  "result_schema": 1,
  "data": {
    "Git": {
      "value": [
        { "Repository": "ccseer/Seer-Properties" },
        { "Branch": "main" },
        { "Ahead / Behind": "0 / 0" },
        { "Status": "Working tree clean" }
      ]
    }
  }
}
```

#### Form D: Graphical Charts and Scopes (Image Attachments)
Render chart images (e.g. PNG) into `${output_dir}`, and declare them in the JSON using `{"type": "image", "value": "relative_filename.png"}`:
```json
{
  "result_schema": 1,
  "data": {
    "Image Scopes": {
      "value": [
        { "RGB Histogram": { "type": "image", "value": "histogram.png" } },
        { "Luma Waveform": { "type": "image", "value": "waveform.png" } },
        { "Exposure": "Normal (EV 0.0)" }
      ]
    }
  }
}
```

---

## 📋 `plugin.json` Schema Specification

Every Seer property plugin must contain a strictly valid `plugin.json` in its package root. **No comments or trailing commas.**

### Field Definitions

| Field Name | Type | Presence | Description |
| :--- | :--- | :--- | :--- |
| `schema_version` | Integer | **Required** | Must be `1`. |
| `id` | String | **Required** | Unique reverse domain ID (e.g. `"io.1218.seer.git-info"`). |
| `name` | String | **Required** | Display name of the plugin. |
| `description` | String | Optional | Short description. |
| `version` | String | **Required** | Semver string (e.g. `"1.0.0"`). |
| `appMinVersion` | String | **Required** | Minimum Seer version (e.g. `"4.5.10"`). |
| `backend` | String | **Required** | Must be `"process"`. |
| `capabilities` | Array | **Required** | Must contain `["property"]`. |
| `extensions` | Array | **Required** | Extensions or tokens (e.g. `["${type_file}"]`, `["${type_folder}"]`). |
| `invocations` | Object | **Recommended** | Declares `"property"` capability execution details. |

### `invocations.property` Properties

| Field Name | Type | Presence | Description |
| :--- | :--- | :--- | :--- |
| `command` | String | **Required** | Executable filename relative to package root. |
| `arguments` | Array | **Required** | CLI arguments array. Must include `${input_file}`, `${output_file}`, and `${output_dir}`. |
| `result_schema` | Integer | **Required** | Must be the integer `1`. |
| `timeout_ms` | Integer | Optional | Timeout budget in milliseconds (e.g. `30000`). |
| `success_exit_codes` | Array | Optional | Exit codes indicating success (default: `[0]`). |

---

## 📋 `plugin.json` Examples

### Example 1: Git Repository Inspector (`git-info`)
```json
{
  "schema_version": 1,
  "id": "io.1218.seer.git-info",
  "name": "Git Info",
  "description": "Reports repository information for the current folder using Git.",
  "version": "1.0.0",
  "appMinVersion": "4.5.10",
  "backend": "process",
  "capabilities": [
    "property"
  ],
  "extensions": [
    "${type_folder}"
  ],
  "invocations": {
    "property": {
      "command": "git_info.exe",
      "arguments": [
        "--input",
        "${input_file}",
        "--output",
        "${output_file}",
        "--output-dir",
        "${output_dir}"
      ],
      "result_schema": 1,
      "timeout_ms": 30000,
      "success_exit_codes": [
        0
      ]
    }
  }
}
```

### Example 2: Flat File Hash (`sha256-property`)
```json
{
  "schema_version": 1,
  "id": "io.1218.seer.sha256-property",
  "name": "SHA-256",
  "description": "Computes the SHA-256 hash for the current file.",
  "version": "1.0.0",
  "appMinVersion": "4.5.10",
  "backend": "process",
  "capabilities": [
    "property"
  ],
  "extensions": [
    "${type_file}"
  ],
  "command": "sha256_property.exe",
  "arguments": [
    "--input",
    "${input_file}",
    "--output",
    "${output_file}"
  ],
  "timeout_ms": 120000,
  "success_exit_codes": [
    0
  ]
}
```

---

## 🤖 AI Agent Delivery Requirements

When building a Property Plugin from a user requirement:

1. **Confirm the Target Metadata**:
   - Determine what files/directories to inspect and which fields/charts to extract.
   - Design the subgroup name (e.g. `Git`, `Digital Signature`, `EXIF`).

2. **Implement the Helper**:
   - C++17 (MSVC static CRT, Win32 API, `nlohmann/json`), or standalone CLI executable/script.
   - Produce valid UTF-8 JSON using a real serializer. Never concatenate raw strings into JSON.
   - Always write output to `<output-base>.json` (or attachments in `${output_dir}`).
   - Exit with code `0` on successful extraction or expected domain conditions (e.g. not a git repo, file unsigned).

3. **Provide `PACKAGE_README.md`**:
   - Every package must include `PACKAGE_README.md`: one short paragraph describing what the property reports, followed by an `## Options & Arguments` Markdown table detailing every option accepted by the helper (columns: `Option`, `Values`, `Default`, `Purpose / Description`).
   - The build system stages and installs `PACKAGE_README.md` as `README.md` into the package ZIP, and manifest tests assert that it exists and is non-empty.
   - If the helper needs an external runtime (PowerShell, Python, Node.js, a VC++ redistributable), state it in `PACKAGE_README.md` and ship the runtime with the package. Prefer a single self-contained executable so the end user installs nothing extra.

4. **Verify Locally**:
   - Validate `plugin.json` with strict JSON parsing: no comments, no trailing commas.
   - Confirm the `command` value matches the packaged executable or script file name **exactly**, including its extension. A name that does not exist in the package root is the most common reason a property plugin fails to start.
   - Keep every `extensions` entry lowercase and free of leading dots.
   - Run in PowerShell against test files:
     ```powershell
     # Executable helpers:
     .\my_property.exe --input "C:\Test\sample.ext" --output "C:\Temp\test_out" --output-dir "C:\Temp"
     # Verify C:\Temp\test_out.json exists and contains valid Schema 1 JSON
     Get-Content "C:\Temp\test_out.json" | ConvertFrom-Json

     # PowerShell script helpers:
     powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\my_property.ps1 --input "C:\Test\sample.ext" --output "C:\Temp\test_out" --output-dir "C:\Temp"
     Get-Content "C:\Temp\test_out.json" | ConvertFrom-Json
     ```

5. **Package into Flat ZIP**:
   - Pack the contents directly into a `.zip` archive so that `plugin.json`, the executable, and `README.md` (staged from `PACKAGE_README.md`) sit at the archive root:
     ```powershell
     7z a -tzip -mx=9 my-property-plugin-1.0.0.zip .\*
     ```
   - Canonical v1 property packages use a **flat** root. This is the opposite of legacy Convert plugins, whose online-catalog archives wrap everything in a top-level `<archive-name>/` folder; do not reuse that layout here, and never mix flat and nested entries — the host rejects an archive with an ambiguous package root.
   - Verify by importing via **Seer Settings > Properties > +**.
