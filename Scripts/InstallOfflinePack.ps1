[CmdletBinding()]
param(
    [string]$PackDirectory = $PSScriptRoot,
    [switch]$AcceptLicenses,
    [switch]$ForceRuntime,
    [string]$LocalMcpDataDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'LocalMCPChatClient')
)

$ErrorActionPreference = 'Stop'
if (-not $AcceptLicenses) {
    throw 'Review the bundled license documents, then re-run with -AcceptLicenses.'
}
$pack = [IO.Path]::GetFullPath($PackDirectory)
$manifestPath = Join-Path $pack 'offline-pack-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Offline pack manifest was not found.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or $manifest.runtime -notin @('cpu','cuda','vulkan')) { throw 'Unsupported offline pack manifest.' }

if (-not $ForceRuntime -and $manifest.runtime -ne 'cpu') {
    $adapters = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object Name)
    if ($manifest.runtime -eq 'cuda' -and -not ($adapters | Where-Object { $_ -match 'NVIDIA' })) {
        throw 'This CUDA pack requires an NVIDIA GPU. Choose the CPU/Vulkan pack or use -ForceRuntime after manual verification.'
    }
    if ($manifest.runtime -eq 'vulkan' -and $adapters.Count -eq 0) {
        throw 'No display adapter was detected for the Vulkan pack.'
    }
}

$manifestPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$packPrefix = $pack.TrimEnd('\') + '\'
foreach ($record in $manifest.files) {
    $relative = ([string]$record.path).Replace('/','\')
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|\\)\.\.(\\|$)' -or
        -not $manifestPaths.Add($relative)) { throw "Unsafe or duplicate manifest path: $relative" }
    $file = [IO.Path]::GetFullPath((Join-Path $pack $relative))
    if (-not $file.StartsWith($packPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Offline pack file is missing: $relative" }
    if ((Get-Item -LiteralPath $file).Length -ne [long]$record.size -or
        (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $record.sha256) {
        throw "Offline pack hash/size mismatch: $relative"
    }
}

$artifactRoot = Join-Path $pack 'OfflineArtifacts'
$modelTarget = Join-Path ([IO.Path]::GetFullPath($LocalMcpDataDirectory)) 'Models'
$runtimeTarget = Join-Path ([IO.Path]::GetFullPath($LocalMcpDataDirectory)) "Runtimes\$($manifest.runtime)\OfflinePack"
[IO.Directory]::CreateDirectory($modelTarget) | Out-Null
[IO.Directory]::CreateDirectory($runtimeTarget) | Out-Null
foreach ($artifact in Get-ChildItem -LiteralPath $artifactRoot -File -Recurse -ErrorAction SilentlyContinue) {
    if ($artifact.Extension -eq '.gguf') {
        $target = Join-Path $modelTarget $artifact.Name
    } else {
        $relative = [IO.Path]::GetRelativePath($artifactRoot, $artifact.FullName)
        $target = Join-Path $runtimeTarget $relative
    }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    if (Test-Path -LiteralPath $target) {
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash) {
            throw "A different artifact already exists at: $target"
        }
        continue
    }
    Copy-Item -LiteralPath $artifact.FullName -Destination $target
}
Write-Host "Offline $($manifest.runtime) artifacts verified and installed without credentials or history."
Write-Host 'Open LocalMCPChatClient setup/settings to register the copied GGUF/projector or runtime when it is not already catalog-managed.'
