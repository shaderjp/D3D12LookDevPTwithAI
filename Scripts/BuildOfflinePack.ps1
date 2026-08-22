[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PortableSuiteDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('cpu','cuda','vulkan')][string]$Runtime = 'cpu',
    [string[]]$Artifacts = @()
)

$ErrorActionPreference = 'Stop'
$suite = [System.IO.Path]::GetFullPath($PortableSuiteDirectory)
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath (Join-Path $suite 'suite-manifest.json'))) { throw 'Portable suite manifest was not found.' }
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output" }
New-Item -ItemType Directory -Path $output | Out-Null
Get-ChildItem -LiteralPath $suite | Copy-Item -Destination $output -Recurse
$artifactRoot = Join-Path $output 'OfflineArtifacts'
New-Item -ItemType Directory -Path $artifactRoot | Out-Null
$chatData = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'LocalMCPChatClient')).TrimEnd('\') + '\'
$allowedChatRoots = @('Models','Runtimes') | ForEach-Object {
    [IO.Path]::GetFullPath((Join-Path $chatData $_)).TrimEnd('\')
}
$d3dData = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'D3D12LookDevPTwithAI')).TrimEnd('\') + '\'
$artifactNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($artifact in $Artifacts) {
    $source = [System.IO.Path]::GetFullPath($artifact)
    if ($source.StartsWith($d3dData, [StringComparison]::OrdinalIgnoreCase) -or
        ($source.StartsWith($chatData, [StringComparison]::OrdinalIgnoreCase) -and
         -not ($allowedChatRoots | Where-Object {
             $source.Equals($_, [StringComparison]::OrdinalIgnoreCase) -or
             $source.StartsWith($_ + '\', [StringComparison]::OrdinalIgnoreCase)
         }))) {
        throw "Refusing to pack credentials, settings, artifacts, or history from an application-data directory: $source"
    }
    if (-not (Test-Path -LiteralPath $source)) { throw "Artifact not found: $source" }
    $items = @((Get-Item -LiteralPath $source -Force))
    if (Test-Path -LiteralPath $source -PathType Container) {
        $items += @(Get-ChildItem -LiteralPath $source -Recurse -Force)
    }
    $sensitiveNames = @('settings.json','history.db','history.db-wal','history.db-shm','mcp-paired-clients.json')
    if ($items | Where-Object {
        ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        (-not $_.PSIsContainer -and $_.Name -in $sensitiveNames)
    }) {
        throw "Refusing to pack reparse content or a settings/history file: $source"
    }
    if (-not $artifactNames.Add([IO.Path]::GetFileName($source))) { throw "Duplicate offline artifact name: $([IO.Path]::GetFileName($source))" }
    Copy-Item -LiteralPath $source -Destination $artifactRoot -Recurse
}
Copy-Item -LiteralPath (Join-Path $repo 'Scripts\InstallOfflinePack.ps1') -Destination $output
$files = Get-ChildItem -LiteralPath $output -File -Recurse | Where-Object Name -ne 'offline-pack-manifest.json'
@{
    schemaVersion = 1
    runtime = $Runtime
    requiresLicenseConfirmation = $true
    excludes = @('tokens','credential-manager','approval-rules','conversation-history')
    files = @($files | ForEach-Object { @{ path = [IO.Path]::GetRelativePath($output,$_.FullName).Replace('\','/'); size=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() } })
} | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $output 'offline-pack-manifest.json') -Encoding utf8NoBOM
Compress-Archive -Path (Join-Path $output '*') -DestinationPath "$output.zip" -CompressionLevel Optimal
Write-Host "Offline pack created for $Runtime. The target user must confirm bundled licenses before first use."
