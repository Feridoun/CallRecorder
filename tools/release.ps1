# Builds a release into build\dist:
#   MeetingRecorder-Setup-<version>.exe   Inno Setup installer
#   MeetingRecorder-<version>-portable.zip
#   SHA256SUMS.txt
#
# Signed with the certificate named by -CertThumbprint or
# $env:MEETINGRECORDER_SIGN_THUMBPRINT (see tools\sign.ps1; SimplySign Desktop
# must be connected). -AllowUnsigned builds without signing, for testing only.
# The version comes from project() in CMakeLists.txt.
#
# Needs Visual Studio 2022 (C++ workload), the Windows SDK and Inno Setup 6
# (winget install JRSoftware.InnoSetup).
param(
    [string]$CertThumbprint = $env:MEETINGRECORDER_SIGN_THUMBPRINT,
    [switch]$AllowUnsigned
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$signScript = Join-Path $PSScriptRoot 'sign.ps1'

if (-not $CertThumbprint -and -not $AllowUnsigned) {
    throw 'No signing certificate: pass -CertThumbprint, set MEETINGRECORDER_SIGN_THUMBPRINT, or use -AllowUnsigned for a test build.'
}
$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(MeetingRecorder VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
Write-Host "MeetingRecorder $version" -ForegroundColor Cyan

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup).' }

# 1. Build
& "$root\build.ps1" -Config release
$exe = "$root\build\release\MeetingRecorder.exe"
$exeVersion = (Get-Item $exe).VersionInfo.ProductVersion
if ($exeVersion -ne $version) { throw "MeetingRecorder.exe reports version '$exeVersion', expected '$version'." }

# 2. Sign the app
if ($CertThumbprint) { & $signScript -Path $exe -CertThumbprint $CertThumbprint }

# 3. Installer (Inno signs setup.exe and its uninstaller through the "certum"
# tool; $f is the file to sign and $q a quote, as Inno strips real quotes).
$dist = "$root\build\dist"
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory $dist | Out-Null
$isccArgs = @("/DMyAppVersion=$version", "/O$dist")
if ($CertThumbprint) {
    $isccArgs += '/DSignBuild=1'
    $isccArgs += "/Scertum=powershell -NoProfile -ExecutionPolicy Bypass -File `$q$signScript`$q -CertThumbprint $CertThumbprint -Path `$f"
}
& $iscc @isccArgs "$root\installer\MeetingRecorder.iss" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed (exit $LASTEXITCODE)." }
$setup = "$dist\MeetingRecorder-Setup-$version.exe"
if (-not (Test-Path $setup)) { throw "Installer not found at $setup." }

# 4. Portable zip
$staging = "$root\build\portable"
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory $staging | Out-Null
Copy-Item $exe, "$root\THIRD_PARTY_NOTICES.txt" $staging
Copy-Item "$root\LICENSE" "$staging\LICENSE.txt"
Compress-Archive -Path "$staging\*" -DestinationPath "$dist\MeetingRecorder-$version-portable.zip"

# 5. Check signatures, write checksums
if ($CertThumbprint) {
    foreach ($file in $exe, $setup) {
        $signature = Get-AuthenticodeSignature $file
        if ($signature.Status -ne 'Valid' -or -not $signature.TimeStamperCertificate) {
            throw "$file is not validly signed and timestamped ($($signature.Status))."
        }
    }
}
Get-ChildItem $dist -File | Where-Object Name -ne 'SHA256SUMS.txt' | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
} | Set-Content "$dist\SHA256SUMS.txt" -Encoding ascii

Write-Host ''
Get-ChildItem $dist | ForEach-Object { Write-Host ('  {0,-45} {1,8:N0} KB' -f $_.Name, ($_.Length / 1KB)) }
if (-not $CertThumbprint) { Write-Host '  UNSIGNED: for testing only, do not publish.' -ForegroundColor Red }
