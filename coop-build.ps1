<#
.SYNOPSIS
    Local Windows build of CBN-Coop, same settings as the GitHub Actions "matrix" Windows job.

.DESCRIPTION
    Finds Visual Studio 2022 and vcpkg, enters the x64 developer environment,
    configures with Ninja (first run or -Reconfigure), builds cataclysm-bn-tiles
    and installs a ready-to-play game folder (default: out\game).

    See COOP_BUILD_WINDOWS.md for the one-time setup.

.EXAMPLE
    .\coop-build.ps1                 # build (incremental) and update out\game
    .\coop-build.ps1 -Reconfigure    # re-run cmake configure first
    .\coop-build.ps1 -Clean          # delete the build directory and build from scratch
    .\coop-build.ps1 -Out D:\cbn     # install the game somewhere else
#>

param(
    [switch]$Clean,
    [switch]$Reconfigure,
    [string]$Out = "",
    [int]$Jobs = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
$buildDir = Join-Path $repo "out\build\coop-release"
if ($Out -eq "") { $Out = Join-Path $repo "out\game" }

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "ERROR: $msg" -ForegroundColor Red
    Write-Host "See COOP_BUILD_WINDOWS.md" -ForegroundColor Yellow
    exit 1
}

if ($repo -match '\s') { Fail "Repository path contains spaces: $repo. Move it, e.g. to C:\dev\CBN-Coop." }

# --- vcpkg ------------------------------------------------------------------
if (-not $env:VCPKG_ROOT -or -not (Test-Path (Join-Path $env:VCPKG_ROOT "vcpkg.exe"))) {
    if (Test-Path "C:\vcpkg\vcpkg.exe") {
        $env:VCPKG_ROOT = "C:\vcpkg"
    } else {
        Fail "vcpkg not found. Install it to C:\vcpkg or set VCPKG_ROOT."
    }
}
Write-Host "vcpkg: $env:VCPKG_ROOT" -ForegroundColor Gray
# Same triplets as CI: static, release-only dependencies.
$env:VCPKG_OVERLAY_TRIPLETS = Join-Path $repo ".github\vcpkg_triplets"

# --- Visual Studio x64 environment -----------------------------------------
$haveX64Cl = $false
$cl = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($cl -and $cl.Source -match '\\Hostx64\\x64\\') { $haveX64Cl = $true }

if (-not $haveX64Cl) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { Fail "Visual Studio 2022 not found (no vswhere.exe)." }
    $vs = & $vswhere -latest -products * -version "[17.0,18.0)" `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { Fail "Visual Studio 2022 with 'Desktop development with C++' not found." }
    Write-Host "Visual Studio: $vs" -ForegroundColor Gray
    Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation `
        -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

foreach ($tool in @("cmake", "ninja", "git")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool not found in PATH. Install the 'C++ CMake tools for Windows' VS component / Git for Windows."
    }
}

# --- Configure ---------------------------------------------------------------
$sw = [Diagnostics.Stopwatch]::StartNew()
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

$cache = Join-Path $buildDir "CMakeCache.txt"
if ($Reconfigure -or -not (Test-Path $cache)) {
    Write-Host ""
    Write-Host "=== Configure (first run also builds SDL3 etc. through vcpkg, this takes a while) ===" -ForegroundColor Cyan
    cmake -G Ninja `
        -S "$repo" `
        -B "$buildDir" `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_PROJECT_INCLUDE_BEFORE="$repo/build-scripts/windows-tiles-sounds-x64-msvc.cmake" `
        -DCMAKE_TOOLCHAIN_FILE="$repo/build-scripts/MSVC.cmake" `
        -DVCPKG_TARGET_TRIPLET=x64-windows-static `
        -DDYNAMIC_LINKING=False `
        -DCURSES=False `
        -DLOCALIZE=False `
        -DTILES=True `
        -DSOUND=True `
        -DTESTS=False `
        -DCATA_MSVC_FAST_LINK=OFF `
        -DJSON_FORMAT=OFF `
        -DCATA_FORMAT_TARGETS=OFF `
        -DCMAKE_INSTALL_MESSAGE=NEVER
    if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed." }
}

# --- Build -------------------------------------------------------------------
Write-Host ""
Write-Host "=== Build cataclysm-bn-tiles ===" -ForegroundColor Cyan
$buildArgs = @("--build", $buildDir, "--config", "Release", "--target", "cataclysm-bn-tiles")
if ($Jobs -gt 0) { $buildArgs += @("-j", "$Jobs") }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { Fail "Build failed (see the first 'error' line above)." }

# --- Install a playable folder ---------------------------------------------
Write-Host ""
Write-Host "=== Install to $Out ===" -ForegroundColor Cyan
$versionTxt = Join-Path $repo "VERSION.txt"
if (-not (Test-Path $versionTxt)) {
    Set-Content -Path $versionTxt -Value "build type: local coop-build.ps1"
}
New-Item -ItemType Directory -Force (Join-Path $repo "lang\mo") | Out-Null
cmake --install "$buildDir" --prefix "$Out" --config Release
if ($LASTEXITCODE -ne 0) { Fail "cmake --install failed." }

$sw.Stop()
$exe = Join-Path $Out "cataclysm-bn-tiles.exe"
Write-Host ""
Write-Host ("Done in {0:mm\:ss}. Game: {1}" -f $sw.Elapsed, $exe) -ForegroundColor Green
