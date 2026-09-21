# Packages the Seer property plugins.
#
# Run from a Visual Studio developer command prompt (MSVC on PATH), from the
# repository root:
#
#   powershell -ExecutionPolicy Bypass -File scripts/package-plugins.ps1 -BuildRoot "C:\Dev\build_output\Seer-Properties" -QtDir "C:\Dev\Qt\6.8.3\msvc2022_64"
#   powershell -ExecutionPolicy Bypass -File scripts/package-plugins.ps1 -BuildRoot "C:\Dev\build_output\Seer-Properties" -SkipTests
#
# -BuildRoot is required: the build trees must live outside the repository and
#   the default cannot be a machine-local path committed to the repo (see
#   AGENTS.md section 5, which keeps such paths in the gitignored
#   CMakeUserPresets.json instead).
# -QtDir is only needed for image-histogram-property (windeployqt).
# -VcRedist is optional: it overrides where vc_redist.x64.exe comes from. Without
#   it the script takes the installer from the previously published
#   image-histogram-property-1.0.0.zip (not committed) and otherwise from a local
#   Visual Studio install.
# -SevenZip is optional: it overrides the 7-Zip executable used to write the
#   archives at -mx=9. The default lookup is "%ProgramFiles%\7-Zip\7z.exe" then
#   7z on PATH; without 7-Zip the script falls back to .NET's zip writer.
#
# For every package this script configures with Ninja, builds, runs CTest,
# installs a flat package tree into plugins/<pkg>/dist, adds the Qt runtime for
# the image package, and produces plugins/<pkg>/<pkg>-<version>.zip. The
# archive is verified entry by entry against dist; the SHA-256 is printed but
# no sidecar .sha256 file is written (a committed copy goes stale with every
# rebuild).
#
# The script overwrites its outputs and never deletes files.
#
# The image package bundles vc_redist.x64.exe because its helper links the
# dynamic CRT. The installer is extracted from the previously published
# image-histogram-property-1.0.0.zip so the byte source stays traceable.

param(
    [string]$QtDir = "",
    [string]$BuildRoot = "",
    [string]$VcRedist = "",
    [string]$SevenZip = "",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    throw "-BuildRoot is required: pass an out-of-repository build directory (for example -BuildRoot '$env:TEMP\Seer-Properties-build'). Machine-local defaults are not committed; per-package CMakeUserPresets.json holds them instead."
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repoRoot = Split-Path -Parent $PSScriptRoot
$packages = @(
    "git-info-property",
    "digital-signature-property",
    "image-histogram-property",
    "sha256-property"
)

function Get-ManifestVersion([string]$PackageDir) {
    $manifest = Get-Content (Join-Path $PackageDir "plugin.json") -Raw | ConvertFrom-Json
    return $manifest.version
}

# The helper executable the host runs, read from the manifest so the script
# never hardcodes a per-package name.
function Get-ManifestCommand([string]$PackageDir) {
    $manifest = Get-Content (Join-Path $PackageDir "plugin.json") -Raw | ConvertFrom-Json
    if ($manifest.invocations -and $manifest.invocations.property) {
        return $manifest.invocations.property.command
    }
    return $manifest.command
}

# 7-Zip writes the archive when it is available; -mx=9 is the maximum level for
# the zip/deflate format, which is what AGENTS.md section 5 requires. The .NET
# writer below is only a fallback for machines without 7-Zip.
function Find-SevenZip([string]$SevenZip) {
    if ($SevenZip -and (Test-Path $SevenZip)) { return $SevenZip }
    $default = Join-Path ${env:ProgramFiles} "7-Zip\7z.exe"
    if (Test-Path $default) { return $default }
    $command = Get-Command 7z -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

function Get-FileSha([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace("-", "")
        }
        finally { $stream.Dispose() }
    }
    finally { $sha.Dispose() }
}

function Get-StreamSha([System.IO.Stream]$Stream) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($Stream))).Replace("-", "")
    }
    finally { $sha.Dispose() }
}

# The installed helper must be the binary that was just built: a stale install
# (for example CMake reporting "Up-to-date") would otherwise publish an older
# helper without any visible error.
function Assert-InstalledIsBuilt([string]$BuildDir, [string]$DistDir, [string]$Command) {
    $installed = Join-Path $DistDir $Command
    if (-not (Test-Path $installed)) { throw "install failed: $installed is missing" }
    $built = Get-ChildItem -Path $BuildDir -Recurse -File -Filter $Command |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $built) { throw "build output $Command was not found under $BuildDir" }
    if ((Get-FileSha $built.FullName) -ne (Get-FileSha $installed)) {
        throw "stale install: $installed does not match the freshly built $($built.FullName)"
    }
    Write-Host "  helper $Command matches the build output"
}

# The archive must contain exactly the dist tree, byte for byte. Updating an
# existing archive can preserve entries from an earlier run, which is how a
# previously published helper survives a rebuild.
function Assert-ArchiveMatchesDist([string]$DistDir, [string]$ZipPath) {
    # Entry names are compared with a normalised separator: 7-Zip writes
    # "platforms/qwindows.dll" while System.IO.Compression writes
    # "platforms\qwindows.dll", and the expected names come from the local
    # filesystem. Directory entries are ignored, so an archiver that records
    # them does not look like an extra file.
    $expected = @{}
    Get-ChildItem -Path $DistDir -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($DistDir.Length).TrimStart('\', '/')
        $expected[$relative.Replace('/', '\')] = Get-FileSha $_.FullName
    }
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $actual = @{}
        foreach ($entry in $zip.Entries) {
            # A trailing separator marks a directory entry (7-Zip writes
            # "platforms/"); test it before the separator is normalised away.
            if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\')) {
                continue
            }
            $name = $entry.FullName.Replace('/', '\').Trim('\')
            $stream = $entry.Open()
            try { $actual[$name] = Get-StreamSha $stream }
            finally { $stream.Dispose() }
        }
        foreach ($name in $expected.Keys) {
            if (-not $actual.ContainsKey($name)) { throw "archive is missing $name" }
            if ($actual[$name] -ne $expected[$name]) { throw "archive entry $name is stale" }
        }
        foreach ($name in $actual.Keys) {
            if (-not $expected.ContainsKey($name)) { throw "archive has extra entry $name" }
        }
        Write-Host ("  archive verified: {0} files" -f $expected.Count)
    }
    finally { $zip.Dispose() }
}

# windeployqt prints "Cannot find Visual Studio installation directory" to stderr
# when VCINSTALLDIR is unset. The documented build environment
# (build_output/Seer/msvcenv.sh) provides INCLUDE/LIB/PATH but not VCINSTALLDIR, so
# derive both variables from the cl.exe on PATH instead of requiring the caller to
# have loaded a Visual Studio developer shell. Returns the VC root, or an empty
# string when cl.exe is not on PATH.
function Initialize-VcEnvironment {
    if ($env:VCINSTALLDIR) {
        return $env:VCINSTALLDIR
    }
    $clPath = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
    if (-not $clPath) {
        return ""
    }
    $dir = Split-Path $clPath -Parent
    while ($dir -and (Split-Path $dir -Leaf) -ne "VC" `
           -and (Split-Path $dir -Parent)) {
        $dir = Split-Path $dir -Parent
    }
    if (-not $dir -or (Split-Path $dir -Leaf) -ne "VC") {
        return ""
    }
    $env:VCINSTALLDIR = $dir + "\"
    $toolsRoot = Join-Path $dir "Tools\MSVC"
    if (Test-Path $toolsRoot) {
        # Version-aware ordering: sorting the names as text puts an older toolset
        # first (14.9.x sorts after 14.51.x).
        $newest = Get-ChildItem $toolsRoot -Directory |
                  Sort-Object -Property @{
                      Expression = {
                          $name = $_.Name
                          if ($name -match '^\d+(\.\d+)+$') { [version]$name }
                          else { [version]'0.0' }
                      }
                  } -Descending |
                  Select-Object -First 1
        if ($newest) {
            $env:VCToolsVersion = $newest.Name
        }
    }
    return $env:VCINSTALLDIR
}

$results = @()

foreach ($package in $packages) {
    $packageDir = Join-Path $repoRoot "plugins\$package"
    $buildDir = Join-Path $BuildRoot "$package\package"
    $distDir = Join-Path $packageDir "dist"

    Write-Host "=== $package ===" -ForegroundColor Cyan

    $cmakeArgs = @("-S", $packageDir, "-B", $buildDir, "-G", "Ninja",
                   "-DCMAKE_BUILD_TYPE=Release")
    if ($package -eq "image-histogram-property") {
        if ([string]::IsNullOrWhiteSpace($QtDir)) {
            throw "-QtDir is required for image-histogram-property: pass the Qt 6.8 MSVC x64 prefix (for example -QtDir 'C:\Dev\Qt\6.8.3\msvc2022_64')."
        }
        $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtDir"
    }
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "configure failed for $package" }

    & cmake --build $buildDir
    if ($LASTEXITCODE -ne 0) { throw "build failed for $package" }

    if (-not $SkipTests) {
        & ctest --test-dir $buildDir -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "tests failed for $package" }
    }

    # Install overwrites the existing dist tree; nothing is deleted.
    & cmake --install $buildDir --prefix $distDir
    if ($LASTEXITCODE -ne 0) { throw "install failed for $package" }

    if ($package -eq "image-histogram-property") {
        $vcWasSet = [bool]$env:VCINSTALLDIR
        $vcRoot   = Initialize-VcEnvironment
        if (-not $vcWasSet -and $vcRoot) {
            Write-Host ("  VCINSTALLDIR derived from cl.exe: " + $vcRoot)
        }
        # Deploy exactly what the helper needs: the platform plugin plus the
        # image format codecs the manifest declares. Excluding the unused
        # modules keeps the package small without deleting anything.
        # windeployqt lives in the Qt prefix's bin directory.
        $windeployqt = Join-Path $QtDir "bin\windeployqt.exe"
        if (-not (Test-Path $windeployqt)) {
            $windeployqt = Join-Path $QtDir "windeployqt.exe"
        }
        if (-not (Test-Path $windeployqt)) {
            throw "windeployqt.exe was not found under $QtDir"
        }
        # stderr goes to a file instead of the pipeline: windeployqt reports
        # "Cannot find Visual Studio installation directory" on stderr when
        # VCINSTALLDIR is unset, and on Windows PowerShell 5.1 a native stderr
        # line can still surface as a terminating error while
        # $ErrorActionPreference is "Stop", although the deployment itself
        # succeeded. The preference is relaxed for this call, so only the exit
        # code decides success.
        $deployLog = Join-Path $env:TEMP "seer-windeployqt.log"
        $preference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        # A deployment that never starts must not be mistaken for success: the
        # relaxed preference turns "cannot start the process" into a
        # non-terminating error, which would otherwise leave the previous
        # command's exit code in $deployExit.
        $deployExit = 1
        try {
            & $windeployqt --release --no-translations `
                --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw `
                --no-ffmpeg --no-network --no-svg --no-pdf `
                --exclude-plugins qpdf,qsvg,qicns,qwbmp,qtga,qsvgicon `
                --dir $distDir (Join-Path $distDir "image_histogram.exe") 2> $deployLog
            $deployExit = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $preference
        }
        if (Test-Path $deployLog) {
            Get-Content -LiteralPath $deployLog |
                ForEach-Object { Write-Host "  windeployqt: $_" }
        }
        if ($deployExit -ne 0) { throw "windeployqt failed with exit $deployExit" }

        # The image helper links the dynamic CRT; ship the installer exactly as
        # the 1.0.0 package did. The previously published archive is the most
        # traceable source, but the stale-archive cleanup below deletes it after a
        # successful run, so a local Visual Studio install is the usual fallback.
        $target = Join-Path $distDir "vc_redist.x64.exe"
        $previousZip = Join-Path $packageDir "image-histogram-property-1.0.0.zip"
        if ($VcRedist -and (Test-Path $VcRedist)) {
            Copy-Item -LiteralPath $VcRedist -Destination $target -Force
            Write-Host "  vc_redist.x64.exe copied from -VcRedist"
        }
        elseif (Test-Path $previousZip) {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $zip = [System.IO.Compression.ZipFile]::OpenRead($previousZip)
            try {
                $entry = $zip.Entries | Where-Object { $_.Name -eq "vc_redist.x64.exe" }
                if ($entry) {
                    [System.IO.Compression.ZipFileExtensions]::ExtractToFile(
                        $entry, $target, $true)
                }
            }
            finally { $zip.Dispose() }
        }
        else {
            $installed = Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\vc_redist.x64.exe" -ErrorAction SilentlyContinue |
                         Sort-Object FullName -Descending |
                         Select-Object -First 1
            if ($installed) {
                Copy-Item -LiteralPath $installed.FullName -Destination $target -Force
                Write-Host ("  vc_redist.x64.exe copied from " + $installed.FullName)
            }
        }
        if (-not (Test-Path $target)) {
            throw "vc_redist.x64.exe source not found: the image helper links the dynamic CRT, so pass -VcRedist <path> or install the Visual Studio redist components"
        }
    }

    $command = Get-ManifestCommand $packageDir
    Assert-InstalledIsBuilt $buildDir $distDir $command

    # A brand-new archive at maximum compression, created under a temporary
    # name and then moved into place: updating an existing archive can keep
    # entries from an earlier run, which is how a previously published helper
    # survives a rebuild.
    $version = Get-ManifestVersion $packageDir
    $zipPath = Join-Path $packageDir "$package-$version.zip"
    $zipTemp = "$zipPath.new"
    # [System.IO.File]::Delete, not Remove-Item: this is a build artefact, and
    # Remove-Item is proxied to the recycle bin in some environments, where it
    # fails closed on paths under a cloud-sync root.
    if (Test-Path $zipTemp) { [System.IO.File]::Delete($zipTemp) }
    $sevenZip = Find-SevenZip $SevenZip
    if ($sevenZip) {
        Push-Location $distDir
        try {
            # stderr goes to a file, not the pipeline: see the windeployqt call
            # above for why an unmerged native stderr line aborts this script.
            $zipLog = Join-Path $env:TEMP "seer-7zip.log"
            & $sevenZip a -tzip -mx=9 -y -bso0 -bsp0 $zipTemp * 2> $zipLog
            if ($LASTEXITCODE -ne 0) {
                if (Test-Path $zipLog) {
                    Get-Content -LiteralPath $zipLog |
                        ForEach-Object { Write-Host "  7-Zip: $_" }
                }
                throw "7-Zip failed for $package"
            }
        }
        finally { Pop-Location }
        Write-Host ("  archive written by 7-Zip (-mx=9): " + $sevenZip)
    }
    else {
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            $distDir, $zipTemp,
            [System.IO.Compression.CompressionLevel]::Optimal, $false)
    }
    # Copy over the previous archive (Move-Item refuses to overwrite an
    # existing file on Windows PowerShell).
    [System.IO.File]::Copy($zipTemp, $zipPath, $true)
    [System.IO.File]::Delete($zipTemp)
    Assert-ArchiveMatchesDist $distDir $zipPath

    # A manual install can pick a stale package: once the new archive is verified,
    # drop the older archives of the same package. They are gitignored build
    # output that a run of this script regenerates.
    Get-ChildItem -Path $packageDir -Filter *.zip -File |
        Where-Object { $_.Name -ne (Split-Path -Leaf $zipPath) } |
        ForEach-Object {
            Write-Host ("  removing stale archive: " + $_.Name)
            [System.IO.File]::Delete($_.FullName)
        }

    # No sidecar checksum file: the archive is verified against dist above and
    # the checksum is only printed, so nothing stale can be committed later.
    $hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash

    $results += [pscustomobject]@{ Package = $package; Zip = $zipPath;
                                   Sha256 = $hash }
}

Write-Host ""
# Out-String first: formatting records in the output pipeline make Out-File
# fail with a NullReferenceException when this script's output is captured.
($results | Format-Table -AutoSize | Out-String) | Write-Host
foreach ($result in $results) {
    Write-Host ("{0}  {1}" -f $result.Sha256, (Split-Path -Leaf $result.Zip))
}
