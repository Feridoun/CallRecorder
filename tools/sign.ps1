# Authenticode-signs one file with a code-signing certificate from
# CurrentUser\My, RFC 3161 timestamped so the signature outlives the
# certificate.
#
# The release builds use a Certum "Code Signing in the Cloud" certificate:
# its key lives in Certum's HSM behind SimplySign Desktop's virtual smart
# card, so SimplySign Desktop must be running and connected first, or
# signtool fails with "No private key is available".
#
#   .\tools\sign.ps1 -Path build\release\MeetingRecorder.exe -CertThumbprint 4CA3...
#   $env:MEETINGRECORDER_SIGN_THUMBPRINT = '4CA3...'; .\tools\sign.ps1 -Path file.exe
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$CertThumbprint = $env:MEETINGRECORDER_SIGN_THUMBPRINT,
    [string]$TimestampUrl = 'http://time.certum.pl'
)
$ErrorActionPreference = 'Stop'

function Find-SignTool {
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (Test-Path $kits) {
        $found = Get-ChildItem $kits -Directory | Where-Object { $_.Name -match '^\d+(\.\d+){3}$' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($found) { return $found }
    }
    $onPath = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'signtool.exe not found. Install the Windows SDK (winget install Microsoft.WindowsSDK.10.0.26100).'
}

if (-not $CertThumbprint) { throw 'No certificate thumbprint: pass -CertThumbprint or set MEETINGRECORDER_SIGN_THUMBPRINT.' }
$CertThumbprint = $CertThumbprint -replace '\s', ''

# The .NET store API rather than the Cert: drive, which fails to load when
# Inno Setup runs this through Windows PowerShell 5.1 from a PowerShell 7 shell.
$store = New-Object System.Security.Cryptography.X509Certificates.X509Store('My', 'CurrentUser')
$store.Open('ReadOnly')
$cert = $store.Certificates | Where-Object { $_.Thumbprint -eq $CertThumbprint } | Select-Object -First 1
$store.Close()
if (-not $cert) { throw "Certificate $CertThumbprint isn't in CurrentUser\My. Is SimplySign Desktop connected?" }
if ($cert.NotAfter -lt (Get-Date)) { throw "Certificate $CertThumbprint expired on $($cert.NotAfter)." }

$signtool = Find-SignTool
$file = (Resolve-Path $Path).Path
& $signtool sign /sha1 $CertThumbprint /fd sha256 /tr $TimestampUrl /td sha256 /d 'MeetingRecorder' `
    /du 'https://github.com/Feridoun/MeetingRecorder' $file | Out-Null
if ($LASTEXITCODE -ne 0) { throw "signtool failed to sign $file" }
& $signtool verify /pa /q $file | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Signature verification failed on $file" }
Write-Host "Signed $(Split-Path -Leaf $file)"
