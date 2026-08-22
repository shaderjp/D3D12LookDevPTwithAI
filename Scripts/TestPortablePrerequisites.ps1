[CmdletBinding()]
param(
    [long]$MinimumFreeBytes = 5GB,
    [string]$OutputJson,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
$failures = [Collections.Generic.List[string]]::new()
$warnings = [Collections.Generic.List[string]]::new()
$os = Get-CimInstance Win32_OperatingSystem
$build = [int]$os.BuildNumber
if ($build -lt 22000) { $failures.Add("Windows 11 x64 is required; detected build $build.") }
if (-not [Environment]::Is64BitOperatingSystem) { $failures.Add('A 64-bit Windows installation is required.') }
$gpus = @(Get-CimInstance Win32_VideoController | Where-Object { -not [string]::IsNullOrWhiteSpace($_.Name) })
if ($gpus.Count -eq 0) { $failures.Add('No display adapter was detected.') }
elseif (-not ($gpus.Name -match 'NVIDIA|AMD|Intel')) { $warnings.Add('GPU vendor was not recognized; the application will perform the authoritative DXR check.') }
$driveRoot = [IO.Path]::GetPathRoot((Get-Location).Path)
$drive = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($driveRoot.TrimEnd('\'))'"
if ($drive -and [long]$drive.FreeSpace -lt $MinimumFreeBytes) {
    $failures.Add("At least $([Math]::Round($MinimumFreeBytes / 1GB, 1)) GiB free space is required.")
}

$lockPath = @(
    (Join-Path $PSScriptRoot 'suite.lock.json'),
    (Join-Path (Split-Path -Parent $PSScriptRoot) 'suite.lock.json')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$result = [pscustomobject]@{
    SchemaVersion = 1
    CreatedAtUtc = [DateTimeOffset]::UtcNow.ToString('O')
    SuiteVersion = if ($lockPath) { (Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json).suiteVersion } else { $null }
    Passed = $failures.Count -eq 0
    WindowsBuild = $build
    Architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    Gpus = @($gpus.Name)
    FreeBytes = if ($drive) { [long]$drive.FreeSpace } else { $null }
    Failures = @($failures)
    Warnings = @($warnings)
    DxrCheck = 'D3D12LookDevPTwithAI performs the authoritative D3D12/DXR tier check at startup.'
}
if (-not [string]::IsNullOrWhiteSpace($OutputJson)) {
    $diagnosticPath = [IO.Path]::GetFullPath($OutputJson)
    [IO.Directory]::CreateDirectory((Split-Path -Parent $diagnosticPath)) | Out-Null
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $diagnosticPath -Encoding utf8NoBOM
}
if ($PassThru) { return $result }
$result | Format-List
if (-not $result.Passed) { exit 1 }
