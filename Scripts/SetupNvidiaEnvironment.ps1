[CmdletBinding()]
param(
    [ValidateSet('LocalNvidia', 'Release', 'RepositoryDefault')][string]$Profile = 'LocalNvidia',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$InitializeSubmodules,
    [switch]$Build,
    [switch]$AcceptNvidiaLicense,
    [string]$StreamlineRoot = '',
    [string]$DlssRoot = '',
    [string]$NrdRoot = '',
    [string]$RtxdiRoot = '',
    [string]$ReportPath = '',
    [switch]$Json
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$modulePath = Join-Path $PSScriptRoot 'NvidiaDependencyTools.psm1'
$manifestPath = Join-Path $repositoryRoot 'config\nvidia-dependencies.json'
Import-Module $modulePath -Force

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe was not found. Install Visual Studio 2026.' }
    $path = & $vswhere -latest -prerelease -version '[18.0,19.0)' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (!$path) { throw 'Visual Studio 2026 MSBuild was not found.' }
    return [string]$path
}

function Write-Report($Report) {
    if (!$ReportPath) { return }
    $full = [IO.Path]::GetFullPath($ReportPath)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($full)) | Out-Null
    [IO.File]::WriteAllText($full, ($Report | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
}

function Show-Report($Report) {
    if ($Json) {
        $Report | ConvertTo-Json -Depth 8
        return
    }
    Write-Host "NVIDIA setup profile: $Profile ($Configuration)"
    foreach ($check in $Report.Checks) {
        $color = if ($check.Status -eq 'OK') { 'Green' } elseif ($check.Status -eq 'WARN') { 'Yellow' } else { 'Red' }
        Write-Host "[$($check.Status)] $($check.Component)/$($check.Kind): $($check.Message)" -ForegroundColor $color
    }
    Write-Host "Summary: $($Report.Failures) failure(s), $($Report.Warnings) warning(s)"
}

$manifest = Import-NvidiaDependencyManifest -Path $manifestPath
$selectedProfile = Get-NvidiaProfile -Manifest $manifest -Name $Profile
$overrides = @{}
if ($StreamlineRoot) { $overrides.streamline = $StreamlineRoot }
if ($DlssRoot) { $overrides.dlss = $DlssRoot }
if ($NrdRoot) { $overrides.nrd = $NrdRoot }
if ($RtxdiRoot) { $overrides.rtxdi = $RtxdiRoot }
$roots = Get-NvidiaComponentRoots -Manifest $manifest -RepositoryRoot $repositoryRoot -Overrides $overrides

if ($InitializeSubmodules) {
    Initialize-NvidiaSubmodules -Manifest $manifest -Profile $selectedProfile -RepositoryRoot $repositoryRoot -Roots $roots
}

$prepare = Get-NvidiaDependencyReport -Manifest $manifest -Profile $selectedProfile -Roots $roots -Configuration $Configuration -Phase Prepare -LicenseAccepted:$AcceptNvidiaLicense
if ($prepare.Failures -gt 0) {
    Write-Report $prepare
    Show-Report $prepare
    throw "NVIDIA preparation failed with $($prepare.Failures) error(s)."
}

if ($Build) {
    $msbuild = Find-MSBuild
    $arguments = @(
        (Join-Path $repositoryRoot 'D3D12LookDevPTwithAI.sln'),
        '/m', '/restore', "/p:Configuration=$Configuration", '/p:Platform=x64'
    ) + (Get-NvidiaMsBuildProperties -Manifest $manifest -Profile $selectedProfile -Roots $roots)
    & $msbuild @arguments
    if ($LASTEXITCODE -ne 0) { throw "NVIDIA-enabled solution build failed with exit code $LASTEXITCODE." }
    $report = Get-NvidiaDependencyReport -Manifest $manifest -Profile $selectedProfile -Roots $roots -Configuration $Configuration -Phase Complete -LicenseAccepted:$AcceptNvidiaLicense
} else {
    $report = $prepare
}

Write-Report $report
Show-Report $report
if ($report.Failures -gt 0) { throw 'NVIDIA environment validation failed after build.' }
