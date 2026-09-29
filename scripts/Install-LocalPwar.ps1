[CmdletBinding()]
param(
    [string]$SourceDll,
    [string]$InstallDirectory = (Join-Path $PSScriptRoot '..\install\windows-0.2.1-local'),
    [switch]$Restore
)
$ErrorActionPreference = 'Stop'
$driverId = '{188135E1-D565-11D2-854F-00A0C99F5D19}'
$userKey = "HKCU:\Software\Classes\CLSID\$driverId\InprocServer32"
$effectiveKey = "Registry::HKEY_CLASSES_ROOT\CLSID\$driverId\InprocServer32"
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$manifestPath = Join-Path $InstallDirectory 'installation.json'

if ($Restore) {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if (-not (Test-Path -LiteralPath $manifest.PreviousDll -PathType Leaf)) {
        throw 'The previous DLL no longer exists.'
    }
    Set-Item -LiteralPath $userKey -Value $manifest.PreviousDll
    New-ItemProperty -LiteralPath $userKey -Name ThreadingModel -Value $manifest.PreviousThreadingModel -Force | Out-Null
    Write-Output "Restored PWAR to $($manifest.PreviousDll). Restart the DAW to use it."
    return
}

if (-not $SourceDll) { throw 'Specify -SourceDll or -Restore.' }
$sourcePath = (Resolve-Path -LiteralPath $SourceDll).Path
if ([IO.Path]::GetFileName($sourcePath) -ine 'PWARASIO.dll') { throw 'Expected PWARASIO.dll.' }
$previous = Get-ItemProperty -LiteralPath $effectiveKey
$previousDll = $previous.'(default)'
if (-not (Test-Path -LiteralPath $previousDll -PathType Leaf)) { throw 'Expected an existing registered PWAR DLL.' }
if (Test-Path -LiteralPath $manifestPath) { throw 'This installation directory is already in use; choose a new directory.' }
New-Item -ItemType Directory -Path $InstallDirectory -Force | Out-Null
$installedDll = Join-Path $InstallDirectory 'PWARASIO.dll'
Copy-Item -LiteralPath $sourcePath -Destination $installedDll
$manifest = [ordered]@{
    InstalledAt = (Get-Date -Format o)
    InstalledDll = $installedDll
    SHA256 = (Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash
    PreviousDll = $previousDll
    PreviousThreadingModel = $previous.ThreadingModel
    Scope = 'Current-user COM override; existing machine ASIO entry retained'
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
New-Item -Path $userKey -Force | Out-Null
Set-Item -LiteralPath $userKey -Value $installedDll
New-ItemProperty -LiteralPath $userKey -Name ThreadingModel -Value Apartment -Force | Out-Null
$registered = (Get-ItemProperty -LiteralPath $effectiveKey).'(default)'
if ($registered -ine $installedDll) { throw 'Effective COM registration does not match the installed DLL.' }
Write-Output "Installed PWAR for this user: $installedDll"
Write-Output "Rollback record: $manifestPath"
Write-Output 'Restart the DAW to release any cached copy of the original driver.'
