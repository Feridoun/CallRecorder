# Builds MeetingRecorder with the MSVC toolchain, CMake, Ninja and vcpkg that ship
# with Visual Studio 2022, then runs the unit tests (skip with -SkipTests).
# Usage: .\build.ps1 [-Config release|debug] [-SkipTests]
param(
    [ValidateSet('release', 'debug')][string]$Config = 'release',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ workload was not found.' }

# The dev shell's own scripts call vswhere by name.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

Push-Location $PSScriptRoot
try {
    cmake --preset "x64-$Config"
    if ($LASTEXITCODE) { throw 'CMake configure failed.' }
    cmake --build --preset $Config
    if ($LASTEXITCODE) { throw 'Build failed.' }
    Write-Host "Built: $PSScriptRoot\build\$Config\MeetingRecorder.exe"
    if (-not $SkipTests) {
        ctest --preset $Config
        if ($LASTEXITCODE) { throw 'Tests failed.' }
    }
} finally {
    Pop-Location
}
