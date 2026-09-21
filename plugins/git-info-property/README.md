# Git Info Property Plugin

This standalone package is a Canonical v1 process plugin that inspects a folder
and reports key repository information using Git itself. Its manifest declares
a Canonical Property invocation using `result_schema: 1` and produces a "Git"
subgroup describing the repository state.

The package interface is defined by `plugin.json`:

- backend: `process`
- capability: `property`
- extensions: `${type_folder}`
- command: package-relative `git_info.exe`
- arguments: `--input ${input_file} --output ${output_file} --output-dir ${output_dir}`
- output: `${output_file}.json` with `result_schema: 1`

The packaged `git_info.exe` is the manifest command, never `git.exe`. A missing
Git installation therefore cannot invalidate the plugin: it still publishes a
valid result whose `Reason` row states that no usable `git.exe` was found.

## Git discovery

- An optional `--git <absolute-path-to-git.exe>` selects a specific executable;
  only an absolute path to an existing executable is accepted.
- Otherwise discovery searches absolute, existing, non-empty `PATH` directories
  for `git.exe` only (`git.cmd` / `git.bat` cannot be launched through
  `CreateProcessW`). Relative and empty entries resolve against the working
  directory and are skipped, and the inspected folder never supplies the
  executable used to inspect it. Candidates are resolved to absolute paths
  before execution.
- The child environment drops inherited Git location, namespace, configuration
  and helper overrides that would redirect inspection or trigger external
  helpers: `GIT_DIR`, `GIT_WORK_TREE`, `GIT_COMMON_DIR`, `GIT_INDEX_FILE`,
  `GIT_OBJECT_DIRECTORY`, `GIT_ALTERNATE_OBJECT_DIRECTORIES`, `GIT_NAMESPACE`,
  `GIT_CEILING_DIRECTORIES`, `GIT_SHALLOW_FILE`, `GIT_CONFIG`,
  `GIT_CONFIG_PARAMETERS`, `GIT_CONFIG_COUNT`,
  `GIT_CONFIG_KEY_*`, `GIT_CONFIG_VALUE_*`, `GIT_EXEC_PATH`,
  `GIT_EXTERNAL_DIFF`, `GIT_DIFF_OPTS`, `GIT_SSH*`, `GIT_ASKPASS`,
  `SSH_ASKPASS`, `GIT_EDITOR`, `GIT_SEQUENCE_EDITOR`, `GIT_PAGER`, `PAGER`,
  `GIT_ALLOW_PROTOCOL`, `GIT_PROTOCOL_FROM_USER`, the `GIT_TRACE*` family and
  friends. Configuration *location* variables (`GIT_CONFIG_GLOBAL`,
  `GIT_CONFIG_SYSTEM`, `GIT_CONFIG_NOSYSTEM`) are deliberately kept: they
  select which configuration the user normally runs with, and dropping them can
  turn a working setup into an "unsafe ownership" refusal because a
  `safe.directory` exception would silently stop applying.
- `GIT_OPTIONAL_LOCKS=0` and `GIT_TERMINAL_PROMPT=0` are always injected.

## Visible outcomes

- Git unavailable: still valid Property JSON with exit code 0, carrying a
  `Reason` row instead of repository rows.
- Confirmed non-repository: `Repository = "not"`.
- Repository found: `Repository = "yes"`, followed by the available key
  information.
- Unsafe ownership (`unsafe-ownership`), permission errors (`permission`),
  corruption (`corrupt`), unsupported Git versions and timeouts (`timeout`) are
  separate query states; they are never collapsed into "not".
- A repository whose layout or work tree root query fails reports the failure in
  `Query Error` and omits `Repository Type` / `Repository Root` rather than
  claiming `worktree` without evidence.
- Exit code 0 is reserved for expected domain outcomes. Nonzero means a
  malformed invocation or a failure that prevents a meaningful result.

## Result rows

Every row is published inside one `Git` subgroup, whose value is an **array of
one-key fields in the plugin's own order** (an object would impose key order),
and the Inspector renders the array in that sequence. The plugin never reports
the path of the `git.exe` it used, nor its version: both are implementation
details of the inspection rather than facts about the inspected folder, so the plugin
does not publish them.

Rows in render order for a normal worktree: `HEAD State` (`attached`,
`detached`, `unborn`), `Branch`, `HEAD` (abbreviated), `Upstream` (`(none)`
without one), `Ahead`, `Behind`, `Repository Type` (`worktree`, `worktree
subdirectory`, `linked-worktree`, `bare`), `Repository Root`, `Staged Changes`,
`Worktree Changes`, `Conflicts`, `Untracked Entries`, `Untracked Mode`, and
`Query Error` when the query could not complete. A healthy repository publishes
no `Repository` row: the rows above already imply it. `Repository` appears only
for degraded states (`unsafe-ownership`, `permission`, `corrupt`, `timeout`,
`query-error`) and reads `not a git repository` when the inspected folder is
not one. Bare repositories replace the change counts with `Worktree
Statistics`; a missing usable Git collapses the group to `Reason`.

**Release prerequisite.** `appMinVersion` is still `4.5.10`, which predates the
host's subgroup-array support. It must be raised to the first host release that
carries it before this plugin is published; on an older host the whole group collapses
into a single empty text row.

## Queries

Git performs repository discovery and the machine-readable queries, always with
structured arguments (`-C <input-directory>`, never shell concatenation) and a
sanitized command line:

- `rev-parse --git-dir` as the repository probe: pure plumbing, never writes,
  with a stable diagnostic outside a repository.
- `rev-parse --is-bare-repository --absolute-git-dir --git-common-dir` for the
  repository layout in one invocation; `--git-common-dir` is resolved against
  the inspected directory because Git reports it relative to the current
  directory.
- `status --porcelain=v2 --branch -z` for branch, upstream, ahead/behind,
  staged changes, worktree changes, conflicts, and untracked entries.
  NUL-delimited records keep renames and file names containing spaces intact,
  and branch information is read from the leading header records only.
- `symbolic-ref` and `rev-parse` for branch, detached and unborn state when
  worktree statistics do not apply.
- `rev-parse --show-toplevel` for the work tree root.

`--no-optional-locks` is passed from Git 2.15; `GIT_OPTIONAL_LOCKS=0` is always
set. Repository-configured helpers are disabled explicitly
(`core.fsmonitor=false`, `diff.external=`, `credential.helper=`).

## Safety and limits

- Read-only and local: no fetch, pull, submodule update, config write,
  repository init, or authentication. Credentials embedded in remote URLs are
  never exposed (no remote is reported).
- Per-query limits: manifest `timeout_ms` 30000, internal deadline 12000 ms for
  the status query and 5000 ms for the plumbing probes, sharing a total budget
  of 25000 ms for the whole invocation, 4 MiB stdout and
  256 KiB stderr caps enforced while reading rather than after exit; exceeding
  a cap ends the query immediately instead of waiting for the deadline. On the
  internal deadline - and whenever a cap is exceeded - the child and its
  descendants are terminated and reaped before the result is published, so a
  forced host cancellation cannot leave query workers running. Descendants are
  killed through a Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), which
  cannot miss a re-parented process the way a parent-pid scan can.
- Untracked entries are counted as reported by `git status` with the default
  untracked mode: a collapsed directory counts as one entry, not as every file
  below it, and `Untracked Mode` states this.
- Porcelain v2 needs Git 2.11; older versions still report the repository but
  explain that the change counts are unavailable.
- Only the supplied request directory is written, containment is validated
  against canonical paths, reparse-point escapes are rejected, and the result
  JSON is published last through a temporary file plus an atomic move.

## Build and test

Dependencies: the C++ standard library, the vendored single-header
[nlohmann/json](../../third_party), the shared
[propertycommon.h](../common/propertycommon.h) (path containment, `.json`
suffix, atomic JSON publication — shared with `digital-signature-property`), and
the Win32 API. No Qt, no Git SDK, and no other third-party code. The helper
links the static CRT, so the built executable is self-contained.

```powershell
cd plugins/git-info-property
cmake --preset default            # Ninja, Release; binaryDir under C:/Dev/build_output/Seer-Properties
cmake --build --preset default
ctest --preset default
```

Test targets:

- `git_info_runner_test` - process plumbing: argument quoting, environment
  sanitization, incremental output caps, internal deadline and reaping.
- `git_info_discovery_test` - override validation and PATH discovery rules.
- `git_info_output_test` - every result state against the git test double,
  including the flat-row host contract and the escaping round trip.
- `git_info_real_git_test` - integration against the real Git found on PATH
  (clean, staged, untracked, detached, unborn, bare, linked worktree,
  subdirectory, non-repository) including a before/after check that the
  repository, index and configuration content is unchanged. Reports SKIP when
  no Git is installed.
- `git_info_manifest_test` - manifest contract plus a staged-helper run.

## Packaging

Run `scripts/package-plugins.ps1` from the repository root: it builds every
package, runs its tests, installs the flat package tree, deploys the Qt runtime
for the image package, and writes one ZIP per package
`plugins/<pkg>/<pkg>-<version>.zip`. Every archive entry is verified against
the installed tree and the SHA-256 is printed; no sidecar checksum file is
written.

The archive contains exactly one package-root `plugin.json` and the helper
executable; no build or test tree is included.
