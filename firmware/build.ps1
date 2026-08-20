<#
.SYNOPSIS
    Build pico_programmer.uf2 for one or both Pico boards.

.DESCRIPTION
    Wraps the manual cmake/ninja steps, using the SDK and toolchain the
    "Raspberry Pi Pico" VS Code extension installs under
    %USERPROFILE%\.pico-sdk. Run the extension's Import Project step once
    first (see README.md) so that directory exists; after that this script
    needs nothing set up in the shell.

    Builds into firmware\build_pico1 and firmware\build_pico2 - separate
    directories because CMake caches -DPICO_BOARD per directory. Both are
    gitignored (build*/), so nothing here needs cleaning up afterward.

.PARAMETER Board
    Which board(s) to build: pico, pico2, or both. Default: both.

.PARAMETER Release
    After a successful build, copy the .uf2 over the tracked release file
    (pico1_programmer.uf2 / pico2_programmer.uf2). Off by default - that
    file is what students flash from, so overwriting it is a deliberate
    choice, not a side effect of building.

.EXAMPLE
    .\build.ps1
    Builds both boards into build_pico1\ and build_pico2\.

.EXAMPLE
    .\build.ps1 -Board pico -Release
    Builds only the Pico (RP2040) and, if it succeeds, updates
    pico1_programmer.uf2 with the result.
#>
param(
    [ValidateSet("pico", "pico2", "both")]
    [string]$Board = "both",
    [switch]$Release
)

$ErrorActionPreference = "Stop"
$firmwareDir = $PSScriptRoot

# ---- locate the extension's toolchain --------------------------------

$sdkRoot = "$env:USERPROFILE\.pico-sdk"
if (-not (Test-Path $sdkRoot)) {
    Write-Error (
        "No Pico SDK found at $sdkRoot.`n" +
        "Install the 'Raspberry Pi Pico' VS Code extension, open this " +
        "firmware folder, and run its 'Import Project' step once - that " +
        "downloads the SDK, toolchain, CMake and Ninja this script uses."
    )
    exit 1
}

# Component folders hold one version subdirectory each, e.g.
# sdk\2.3.0, toolchain\15_2_Rel1. Picking the newest by name rather than
# hardcoding a version means this script keeps working after the extension
# updates itself.
function Get-SdkComponent([string]$name) {
    $dirs = @(Get-ChildItem (Join-Path $sdkRoot $name) -Directory -ErrorAction SilentlyContinue)
    if ($dirs.Count -eq 0) {
        Write-Error "Could not find $name under $sdkRoot - re-run the extension's Import Project step."
        exit 1
    }
    # Sort on the numeric runs in the name, zero-padded, so 2.10.0 beats
    # 2.9.0 and 15_2_Rel1 beats 9_2_Rel1. A plain string sort gets both of
    # those backwards and would silently build against the older install.
    $dir = $dirs | Sort-Object @{ Expression = {
        ([regex]::Matches($_.Name, '\d+') |
            ForEach-Object { $_.Value.PadLeft(6, '0') }) -join '.'
    }} -Descending | Select-Object -First 1
    return $dir.FullName
}

$sdkPath   = Get-SdkComponent "sdk"
$toolchain = Get-SdkComponent "toolchain"
$cmakeDir  = Get-SdkComponent "cmake"
$ninjaDir  = Get-SdkComponent "ninja"

$env:PICO_SDK_PATH = $sdkPath
$env:PATH = "$toolchain\bin;$cmakeDir\bin;$ninjaDir;$env:PATH"

Write-Host "Using SDK:       $sdkPath"
Write-Host "Using toolchain: $toolchain"

# ---- build ------------------------------------------------------------

$targets = @()
if ($Board -eq "pico" -or $Board -eq "both") {
    $targets += [pscustomobject]@{ Board = "pico";  Dir = "build_pico1"; Release = "pico1_programmer.uf2" }
}
if ($Board -eq "pico2" -or $Board -eq "both") {
    $targets += [pscustomobject]@{ Board = "pico2"; Dir = "build_pico2"; Release = "pico2_programmer.uf2" }
}

foreach ($t in $targets) {
    Write-Host "`n=== Building for $($t.Board) ===" -ForegroundColor Cyan

    $buildDir = Join-Path $firmwareDir $t.Dir
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    # Delete any previous .uf2 first, so its existence afterwards proves THIS
    # build produced it. Without that, a build that fails while an older file
    # is still lying around would sail past the check below and -Release would
    # publish a stale binary as if it were current.
    $uf2 = Join-Path $buildDir "pico_programmer.uf2"
    Remove-Item $uf2 -Force -ErrorAction SilentlyContinue

    Push-Location $buildDir
    try {
        cmake .. -G Ninja "-DPICO_BOARD=$($t.Board)"
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $($t.Board)" }

        cmake --build . -j4
        if ($LASTEXITCODE -ne 0) { throw "build failed for $($t.Board)" }
    } finally {
        Pop-Location
    }

    if (-not (Test-Path $uf2)) { throw "Expected $uf2 but the build didn't produce it" }
    Write-Host "Built: $uf2" -ForegroundColor Green

    if ($Release) {
        $dest = Join-Path $firmwareDir $t.Release
        Copy-Item $uf2 $dest -Force
        Write-Host "Copied to $dest (tracked release file)" -ForegroundColor Yellow
    }
}

Write-Host "`nDone." -ForegroundColor Green
