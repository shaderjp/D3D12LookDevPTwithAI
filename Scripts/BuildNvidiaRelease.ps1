[CmdletBinding()]
param(
    [string]$OutputDirectory = '',
    [switch]$InitializeSubmodules,
    [switch]$AcceptNvidiaLicense,
    [string]$StreamlineRoot = '',
    [string]$DlssRoot = '',
    [string]$NrdRoot = '',
    [string]$RtxdiRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$AcceptNvidiaLicense) {
    throw 'Review the NVIDIA SDK licenses, then rerun with -AcceptNvidiaLicense.'
}
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repositoryRoot 'Artifacts\NvidiaRelease\D3D12LookDevPTwithAI-NVIDIA-x64'
}
$destination = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd([char[]]@('\', '/'))
$sourceOutput = Join-Path $repositoryRoot 'Bin\x64\Release'
$sourceOutput = [IO.Path]::GetFullPath($sourceOutput).TrimEnd([char[]]@('\', '/'))
$staging = $destination + '.staging-' + [Guid]::NewGuid().ToString('N')
$pathComparison = [StringComparison]::OrdinalIgnoreCase
if ($destination.StartsWith($sourceOutput + [IO.Path]::DirectorySeparatorChar, $pathComparison)) {
    throw 'OutputDirectory must not be inside Bin\x64\Release.'
}
if (Test-Path -LiteralPath $destination) {
    throw "Output directory already exists; choose a new path or remove it explicitly: $destination"
}

$setupArguments = @{
    Profile = 'Release'; Configuration = 'Release'; Build = $true
    AcceptNvidiaLicense = $true
}
if ($InitializeSubmodules) { $setupArguments.InitializeSubmodules = $true }
if ($StreamlineRoot) { $setupArguments.StreamlineRoot = $StreamlineRoot }
if ($DlssRoot) { $setupArguments.DlssRoot = $DlssRoot }
if ($NrdRoot) { $setupArguments.NrdRoot = $NrdRoot }
if ($RtxdiRoot) { $setupArguments.RtxdiRoot = $RtxdiRoot }

try {
    & (Join-Path $PSScriptRoot 'SetupNvidiaEnvironment.ps1') @setupArguments
    if (!(Test-Path -LiteralPath (Join-Path $sourceOutput 'D3D12LookDevPTwithAI.exe') -PathType Leaf)) {
        throw 'The Release build did not produce D3D12LookDevPTwithAI.exe.'
    }
    if (!(Test-Path -LiteralPath (Join-Path $sourceOutput 'D3D12LookDevPTwithAI.ChatHost.exe') -PathType Leaf)) {
        throw 'The Release build did not produce the self-contained ChatHost.'
    }

    [IO.Directory]::CreateDirectory($staging) | Out-Null
    Copy-Item -Path (Join-Path $sourceOutput '*') -Destination $staging -Recurse -Force
    Get-ChildItem -LiteralPath $staging -Recurse -File | Where-Object Extension -in @('.pdb', '.lib', '.exp') | Remove-Item -Force
    foreach ($name in @('D3D12LookDevPTwithAI.Validation.exe', 'D3D12LookDevPTwithAI.Validation.pri')) {
        $path = Join-Path $staging $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
    $sourceDirectory = Join-Path $staging 'Source'
    if (Test-Path -LiteralPath $sourceDirectory) { Remove-Item -LiteralPath $sourceDirectory -Recurse -Force }

    Import-Module (Join-Path $PSScriptRoot 'NvidiaDependencyTools.psm1') -Force
    $manifest = Import-NvidiaDependencyManifest -Path (Join-Path $repositoryRoot 'config\nvidia-dependencies.json')
    $profile = Get-NvidiaProfile -Manifest $manifest -Name Release
    $overrides = @{}
    if ($StreamlineRoot) { $overrides.streamline = $StreamlineRoot }
    if ($DlssRoot) { $overrides.dlss = $DlssRoot }
    if ($NrdRoot) { $overrides.nrd = $NrdRoot }
    if ($RtxdiRoot) { $overrides.rtxdi = $RtxdiRoot }
    $roots = Get-NvidiaComponentRoots -Manifest $manifest -RepositoryRoot $repositoryRoot -Overrides $overrides
    $nvidiaFiles = @(Publish-NvidiaRuntime -Manifest $manifest -Profile $profile -Roots $roots -DestinationRoot $staging)

    $licenses = Join-Path $staging 'Licenses'
    [IO.Directory]::CreateDirectory($licenses) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repositoryRoot 'LICENSE') -Destination (Join-Path $licenses 'D3D12LookDevPTwithAI-LICENSE.txt') -Force
    $notice = @'
D3D12LookDevPTwithAI NVIDIA release payload

This payload was built with DLSS, NRD, and RTXDI enabled. NVIDIA SDK/runtime
redistribution remains subject to the NVIDIA license documents in
Licenses/NVIDIA. The operator who created this payload explicitly accepted
responsibility for those terms. This script does not grant redistribution rights.

The NVIDIA-issued NGX application ID is intentionally not stored in this
payload. Set D3D12LOOKDEVPT_NGX_APPLICATION_ID in the launch environment.
Use Launch-NVIDIA.ps1 to validate that variable and start the application.
The native payload contains the Windows App SDK runtime; install the current
Microsoft Visual C++ x64 Redistributable compatible with the v145 toolset on
the target machine.
The local AI CUDA backend is a separate, user-installed llama.cpp artifact and
is not bundled by this renderer release builder.
'@
    [IO.File]::WriteAllText((Join-Path $staging 'README-NVIDIA.txt'), $notice, [Text.UTF8Encoding]::new($false))
    $launcher = @'
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
if ($env:D3D12LOOKDEVPT_NGX_APPLICATION_ID -notmatch '^\d+$') {
    throw 'Set D3D12LOOKDEVPT_NGX_APPLICATION_ID to the NVIDIA-issued decimal application ID.'
}
$application = Join-Path $PSScriptRoot 'D3D12LookDevPTwithAI.exe'
Start-Process -FilePath $application -WorkingDirectory $PSScriptRoot
'@
    [IO.File]::WriteAllText((Join-Path $staging 'Launch-NVIDIA.ps1'), $launcher, [Text.UTF8Encoding]::new($false))

    $inventory = @(Get-ChildItem -LiteralPath $staging -Recurse -File | Sort-Object FullName | ForEach-Object {
        [pscustomobject]@{
            path = [IO.Path]::GetRelativePath($staging, $_.FullName).Replace('\', '/')
            size = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    $commit = if ($git) { ([string](& $git.Source -C $repositoryRoot rev-parse HEAD 2>$null)).Trim() } else { '' }
    $sourceDirty = if ($git) { @(& $git.Source -C $repositoryRoot status --porcelain --untracked-files=no 2>$null).Count -gt 0 } else { $null }
    $releaseManifest = [ordered]@{
        schemaVersion = 1
        createdUtc = [DateTime]::UtcNow.ToString('o')
        sourceCommit = $commit
        sourceDirty = $sourceDirty
        platform = 'x64'
        configuration = 'Release'
        backends = [ordered]@{ dlss = $true; nrd = $true; rtxdi = $true }
        ngxApplicationIdEnvironmentVariable = [string]$manifest.ngxApplicationIdEnvironmentVariable
        ngxApplicationIdIncluded = $false
        nativeWindowsAppSdkSelfContained = $true
        visualCppRuntimeBundled = $false
        prerequisites = @('Current Microsoft Visual C++ x64 Redistributable compatible with v145', 'NVIDIA display driver with DXR support')
        dependencies = @($manifest.components | ForEach-Object {
            [ordered]@{ id = [string]$_.id; revision = [string]$_.revision }
        })
        nvidiaFiles = $nvidiaFiles
        files = $inventory
    }
    [IO.File]::WriteAllText((Join-Path $staging 'nvidia-release-manifest.json'), ($releaseManifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))

    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
    [IO.Directory]::Move($staging, $destination)
    Write-Host "NVIDIA Release payload created: $destination" -ForegroundColor Green
} catch {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    throw
}
