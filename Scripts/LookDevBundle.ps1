[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Create','Import')][string]$Command,
    [string]$Project,
    [string]$Output,
    [ValidateSet('thin','portable')][string]$BundleMode = 'thin',
    [string]$AssetRoot,
    [string[]]$Assets = @(),
    [hashtable]$Licenses = @{},
    [switch]$ConfirmAssetRights,
    [string[]]$ReviewReports = @(),
    [string]$Bundle,
    [string]$Destination,
    [string]$ResolveAssetRoot,
    [long]$MaximumExpandedBytes = 20GB,
    [int]$MaximumEntries = 10000
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Get-SafeLogicalPath([string]$Path) {
    $logical = $Path.Replace('\','/')
    if ([string]::IsNullOrWhiteSpace($logical) -or $logical.StartsWith('/') -or
        $logical -match '(^|/)\.\.(/|$)' -or [IO.Path]::IsPathRooted($logical)) {
        throw "Unsafe logical path: $Path"
    }
    $logical = $logical.TrimEnd('/')
    $invalid = [IO.Path]::GetInvalidFileNameChars()
    foreach ($segment in $logical.Split('/')) {
        if ([string]::IsNullOrWhiteSpace($segment) -or $segment -eq '.' -or $segment.IndexOfAny($invalid) -ge 0) {
            throw "Unsafe logical path segment: $Path"
        }
    }
    return $logical
}

function Get-FileRecord([string]$Physical, [string]$Logical, [string]$Role, [bool]$Included, [string]$License) {
    $item = Get-Item -LiteralPath $Physical
    return [ordered]@{
        logicalPath = Get-SafeLogicalPath $Logical
        role = $Role
        size = $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        included = $Included
        source = if ($Included) { $null } else { [IO.Path]::GetFileName($item.FullName) }
        license = $License
        attribution = $null
    }
}

if ($Command -eq 'Create') {
    if ([string]::IsNullOrWhiteSpace($Project) -or [string]::IsNullOrWhiteSpace($Output)) { throw 'Create requires -Project and -Output.' }
    $projectPath = [IO.Path]::GetFullPath($Project)
    if (-not (Test-Path -LiteralPath $projectPath -PathType Leaf)) { throw "Project not found: $projectPath" }
    $outputPath = [IO.Path]::GetFullPath($Output)
    if ([IO.Path]::GetExtension($outputPath) -ne '.lookdevbundle') { $outputPath += '.lookdevbundle' }
    if (Test-Path -LiteralPath $outputPath) { throw "Bundle already exists: $outputPath" }
    if ($BundleMode -eq 'portable' -and $Assets.Count -gt 0 -and -not $ConfirmAssetRights) {
        throw 'Portable asset inclusion requires -ConfirmAssetRights.'
    }
    $projectJson = Get-Content -LiteralPath $projectPath -Raw | ConvertFrom-Json
    $assetRootPath = if ([string]::IsNullOrWhiteSpace($AssetRoot)) { Split-Path -Parent $projectPath } else { [IO.Path]::GetFullPath($AssetRoot) }
    $assetMappings = @{}
    $logicalPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $staging = Join-Path ([IO.Path]::GetTempPath()) ("lookdevbundle-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $staging 'project'), (Join-Path $staging 'assets'), (Join-Path $staging 'reviews') | Out-Null
    try {
        $projectJson | Add-Member -NotePropertyName schemaVersion -NotePropertyValue 3 -Force
        $projectJson | Add-Member -NotePropertyName assetRoot -NotePropertyValue '../assets' -Force
        $entryName = [IO.Path]::GetFileName($projectPath)
        $projectJson | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath (Join-Path $staging "project\$entryName") -Encoding utf8NoBOM
        $records = [Collections.Generic.List[object]]::new()
        $records.Add((Get-FileRecord (Join-Path $staging "project\$entryName") "project/$entryName" 'project' $true 'project-owner'))

        foreach ($asset in $Assets) {
            $physical = [IO.Path]::GetFullPath((Join-Path $assetRootPath $asset))
            $rootPrefix = [IO.Path]::GetFullPath($assetRootPath).TrimEnd('\') + '\'
            if (-not $physical.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Asset escapes asset root: $asset" }
            if (-not (Test-Path -LiteralPath $physical -PathType Leaf)) { throw "Asset not found: $physical" }
            $logical = Get-SafeLogicalPath $asset
            if (-not $logicalPaths.Add("assets/$logical")) { throw "Duplicate logical asset path: $logical" }
            $license = if ($Licenses.ContainsKey($logical)) { [string]$Licenses[$logical] } else { 'unknown' }
            if ($license -eq 'unknown' -and -not $ConfirmAssetRights) {
                Write-Warning "Excluded asset with unknown license: $logical"
                continue
            }
            $assetMappings[$physical.ToLowerInvariant()] = $logical
            $included = $BundleMode -eq 'portable'
            if ($included) {
                $target = Join-Path $staging ("assets\" + $logical.Replace('/','\'))
                New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
                Copy-Item -LiteralPath $physical -Destination $target
            }
            $records.Add((Get-FileRecord $physical "assets/$logical" 'asset' $included $license))
        }
        $rewriteProjectPath = {
            param([string]$Value, [string]$Role)
            if ([string]::IsNullOrWhiteSpace($Value)) { return $Value }
            $physical = if ([IO.Path]::IsPathRooted($Value)) {
                [IO.Path]::GetFullPath($Value)
            } else {
                [IO.Path]::GetFullPath((Join-Path $assetRootPath $Value))
            }
            $key = $physical.ToLowerInvariant()
            if ($assetMappings.ContainsKey($key)) { return $assetMappings[$key] }
            throw "$BundleMode bundle is missing its $Role asset in -Assets, or the asset was excluded by its license policy: $([IO.Path]::GetFileName($Value))"
        }
        if ($projectJson.PSObject.Properties['scenePath']) {
            $projectJson.scenePath = & $rewriteProjectPath ([string]$projectJson.scenePath) 'scene'
        }
        if ($projectJson.PSObject.Properties['environmentPath']) {
            $projectJson.environmentPath = & $rewriteProjectPath ([string]$projectJson.environmentPath) 'environment'
        }
        foreach ($material in @($projectJson.materials)) {
            if (-not $material -or -not $material.PSObject.Properties['textures']) { continue }
            foreach ($property in @($material.textures.PSObject.Properties)) {
                if (-not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
                    $property.Value = & $rewriteProjectPath ([string]$property.Value) 'material texture'
                }
            }
        }
        foreach ($variant in @($projectJson.materialVariants)) {
            if (-not $variant -or -not $variant.PSObject.Properties['textures']) { continue }
            foreach ($property in @($variant.textures.PSObject.Properties)) {
                if (-not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
                    $property.Value = & $rewriteProjectPath ([string]$property.Value) 'material variant texture'
                }
            }
        }
        $projectJson | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath (Join-Path $staging "project\$entryName") -Encoding utf8NoBOM
        $records[0] = Get-FileRecord (Join-Path $staging "project\$entryName") "project/$entryName" 'project' $true 'project-owner'
        foreach ($report in $ReviewReports) {
            $physical = [IO.Path]::GetFullPath($report)
            if (-not (Test-Path -LiteralPath $physical -PathType Leaf)) { throw "Review report not found: $physical" }
            $logical = "reviews/" + [IO.Path]::GetFileName($physical)
            if (-not $logicalPaths.Add($logical)) { throw "Duplicate review report name: $logical" }
            Copy-Item -LiteralPath $physical -Destination (Join-Path $staging $logical.Replace('/','\'))
            $records.Add((Get-FileRecord $physical $logical 'review-report' $true 'report-owner'))
        }
        [ordered]@{
            schemaVersion = 1
            mode = $BundleMode
            entryProject = "project/$entryName"
            application = @{ name='D3D12LookDevPTwithAI'; minimumContract='1.0' }
            projectSchemaVersion = 3
            createdAtUtc = [DateTimeOffset]::UtcNow.ToString('O')
            files = $records
        } | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $staging 'manifest.json') -Encoding utf8NoBOM
        [IO.Compression.ZipFile]::CreateFromDirectory($staging, $outputPath, [IO.Compression.CompressionLevel]::Optimal, $false)
    }
    finally {
        if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
    }
    Write-Host "Created $BundleMode bundle: $outputPath"
    return
}

if ([string]::IsNullOrWhiteSpace($Bundle) -or [string]::IsNullOrWhiteSpace($Destination)) { throw 'Import requires -Bundle and -Destination.' }
$bundlePath = [IO.Path]::GetFullPath($Bundle)
$destinationPath = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $destinationPath) { throw "Destination already exists: $destinationPath" }
$destinationParent = Split-Path -Parent $destinationPath
if (-not (Test-Path -LiteralPath $destinationParent)) { New-Item -ItemType Directory -Path $destinationParent | Out-Null }
$temporary = Join-Path $destinationParent ("." + [IO.Path]::GetFileName($destinationPath) + ".import-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
try {
    $archive = [IO.Compression.ZipFile]::OpenRead($bundlePath)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $total = 0L
        $entryCount = 0
        foreach ($entry in $archive.Entries) {
            ++$entryCount
            if ($entryCount -gt $MaximumEntries) { throw 'Bundle exceeds the entry-count limit.' }
            $logical = Get-SafeLogicalPath $entry.FullName
            if (-not $seen.Add($logical)) { throw "Duplicate bundle entry: $logical" }
            $total += $entry.Length
            if ($total -gt $MaximumExpandedBytes) { throw 'Bundle exceeds the expanded-size limit.' }
            $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if ($unixType -eq 0xA000 -or ($entry.ExternalAttributes -band 0x400) -ne 0) { throw "Links/reparse entries are not allowed: $logical" }
            $target = [IO.Path]::GetFullPath((Join-Path $temporary $logical.Replace('/','\')))
            $temporaryPrefix = [IO.Path]::GetFullPath($temporary).TrimEnd('\') + '\'
            if (-not $target.StartsWith($temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Entry escapes destination: $logical" }
            if ($entry.Name.Length -eq 0) { New-Item -ItemType Directory -Path $target -Force | Out-Null; continue }
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $false)
        }
    }
    finally { $archive.Dispose() }
    $manifestPath = Join-Path $temporary 'manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'Bundle manifest is missing.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schemaVersion -ne 1 -or $manifest.projectSchemaVersion -ne 3) { throw 'Unsupported bundle or project schema.' }
    if ($manifest.mode -notin @('thin','portable')) { throw 'Unsupported bundle mode.' }
    $manifestPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($record in $manifest.files) {
        $logical = Get-SafeLogicalPath ([string]$record.logicalPath)
        if (-not $manifestPaths.Add($logical)) { throw "Duplicate manifest path: $logical" }
        if ([long]$record.size -lt 0 -or [string]$record.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw "Invalid manifest metadata: $logical" }
        if ($record.included) {
            $physical = Join-Path $temporary $logical.Replace('/','\')
        } elseif ($manifest.mode -eq 'thin') {
            if ([string]::IsNullOrWhiteSpace($ResolveAssetRoot)) { throw "Thin bundle requires -ResolveAssetRoot for $logical" }
            $assetRelative = $logical -replace '^assets/',''
            $physical = [IO.Path]::GetFullPath((Join-Path $ResolveAssetRoot $assetRelative.Replace('/','\')))
        } else { continue }
        if (-not (Test-Path -LiteralPath $physical -PathType Leaf)) { throw "Missing bundle asset: $logical" }
        $item = Get-Item -LiteralPath $physical
        if ($item.Length -ne [long]$record.size) { throw "Size mismatch: $logical" }
        $hash = (Get-FileHash -LiteralPath $physical -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $record.sha256) { throw "SHA-256 mismatch: $logical" }
    }
    foreach ($entryPath in $seen) {
        $entryFile = Join-Path $temporary $entryPath.Replace('/','\')
        if ((Test-Path -LiteralPath $entryFile -PathType Leaf) -and $entryPath -ne 'manifest.json' -and -not $manifestPaths.Contains($entryPath)) {
            throw "Unmanifested bundle file: $entryPath"
        }
    }
    $entryProject = Get-SafeLogicalPath ([string]$manifest.entryProject)
    $entryRecords = @($manifest.files | Where-Object { $_.role -eq 'project' -and $_.included -and $_.logicalPath -eq $entryProject })
    if ($entryRecords.Count -ne 1) { throw 'Bundle entryProject must reference exactly one included project record.' }
    if ($manifest.mode -eq 'thin') {
        $projectPath = Join-Path $temporary $entryProject.Replace('/','\')
        $projectJson = Get-Content -LiteralPath $projectPath -Raw | ConvertFrom-Json
        $projectJson.assetRoot = [IO.Path]::GetFullPath($ResolveAssetRoot)
        $projectJson | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $projectPath -Encoding utf8NoBOM
        $projectRecord = @($manifest.files | Where-Object { $_.role -eq 'project' -and $_.logicalPath -eq $entryProject })[0]
        if ($projectRecord) {
            $projectRecord.size = (Get-Item -LiteralPath $projectPath).Length
            $projectRecord.sha256 = (Get-FileHash -LiteralPath $projectPath -Algorithm SHA256).Hash.ToLowerInvariant()
            $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
        }
    }
    Move-Item -LiteralPath $temporary -Destination $destinationPath
} catch {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
    throw
}
Write-Host "Imported bundle atomically: $destinationPath"
