$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$bundleTool = Join-Path $repo 'Scripts\LookDevBundle.ps1'
$testRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('lookdevbundle-tests-' + [Guid]::NewGuid().ToString('N'))))
$tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test root.' }

function Assert-Throws([scriptblock]$Operation, [string]$Name) {
    try { & $Operation; throw "Expected failure was not raised: $Name" }
    catch {
        if ($_.Exception.Message -eq "Expected failure was not raised: $Name") { throw }
    }
}

[IO.Directory]::CreateDirectory($testRoot) | Out-Null
try {
    $source = Join-Path $testRoot 'source'
    [IO.Directory]::CreateDirectory($source) | Out-Null
    Set-Content -LiteralPath (Join-Path $source 'scene.gltf') -Value '{"asset":{"version":"2.0"}}' -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $source 'environment.hdr') -Value 'fixture-hdri' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $source 'texture.png') -Value 'fixture-texture' -Encoding ascii
    @{
        schemaVersion = 2
        scenePath = 'scene.gltf'
        environmentPath = 'environment.hdr'
        materials = @(@{ textures = @{ baseColor = 'texture.png' } })
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $source 'fixture.lookdevpt.json') -Encoding utf8NoBOM

    $assets = @('scene.gltf','environment.hdr','texture.png')
    $licenses = @{ 'scene.gltf'='test'; 'environment.hdr'='test'; 'texture.png'='test' }
    $portable = Join-Path $testRoot 'portable.lookdevbundle'
    & $bundleTool -Command Create -Project (Join-Path $source 'fixture.lookdevpt.json') -Output $portable `
        -BundleMode portable -AssetRoot $source -Assets $assets -Licenses $licenses -ConfirmAssetRights
    $portableImport = Join-Path $testRoot 'portable-import'
    & $bundleTool -Command Import -Bundle $portable -Destination $portableImport
    $portableProject = Get-Content -LiteralPath (Join-Path $portableImport 'project\fixture.lookdevpt.json') -Raw | ConvertFrom-Json
    if ($portableProject.schemaVersion -ne 3 -or $portableProject.assetRoot -ne '../assets') { throw 'Portable project schema/assetRoot mismatch.' }
    foreach ($asset in $assets) {
        if (-not (Test-Path -LiteralPath (Join-Path $portableImport "assets\$asset") -PathType Leaf)) { throw "Portable asset missing: $asset" }
    }

    $thin = Join-Path $testRoot 'thin.lookdevbundle'
    & $bundleTool -Command Create -Project (Join-Path $source 'fixture.lookdevpt.json') -Output $thin `
        -BundleMode thin -AssetRoot $source -Assets $assets -Licenses $licenses
    $thinImport = Join-Path $testRoot 'thin-import'
    & $bundleTool -Command Import -Bundle $thin -Destination $thinImport -ResolveAssetRoot $source
    $thinProject = Get-Content -LiteralPath (Join-Path $thinImport 'project\fixture.lookdevpt.json') -Raw | ConvertFrom-Json
    if ([IO.Path]::GetFullPath($thinProject.assetRoot) -ne [IO.Path]::GetFullPath($source)) { throw 'Thin assetRoot binding mismatch.' }

    $originalTexture = Get-Content -LiteralPath (Join-Path $source 'texture.png') -Raw
    Set-Content -LiteralPath (Join-Path $source 'texture.png') -Value 'tampered' -Encoding ascii
    Assert-Throws { & $bundleTool -Command Import -Bundle $thin -Destination (Join-Path $testRoot 'hash-failure') -ResolveAssetRoot $source } 'hash mismatch'
    Set-Content -LiteralPath (Join-Path $source 'texture.png') -Value $originalTexture -NoNewline -Encoding ascii
    Remove-Item -LiteralPath (Join-Path $source 'texture.png')
    Assert-Throws { & $bundleTool -Command Import -Bundle $thin -Destination (Join-Path $testRoot 'missing-failure') -ResolveAssetRoot $source } 'missing asset'
    Set-Content -LiteralPath (Join-Path $source 'texture.png') -Value $originalTexture -NoNewline -Encoding ascii

    Assert-Throws { & $bundleTool -Command Import -Bundle $portable -Destination (Join-Path $testRoot 'size-failure') -MaximumExpandedBytes 1 } 'expanded size limit'
    Assert-Throws { & $bundleTool -Command Create -Project (Join-Path $source 'fixture.lookdevpt.json') -Output (Join-Path $testRoot 'license-failure') `
        -BundleMode portable -AssetRoot $source -Assets $assets } 'unknown license exclusion'

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($case in @('traversal','duplicate')) {
        $zip = Join-Path $testRoot "$case.lookdevbundle"
        $stream = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
        try {
            $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $false)
            try {
                if ($case -eq 'traversal') {
                    [void]$archive.CreateEntry('../escape.txt')
                } else {
                    [void]$archive.CreateEntry('manifest.json')
                    [void]$archive.CreateEntry('manifest.json')
                }
            } finally { $archive.Dispose() }
        } finally { $stream.Dispose() }
        Assert-Throws { & $bundleTool -Command Import -Bundle $zip -Destination (Join-Path $testRoot "$case-import") } $case
    }
    Write-Host 'LookDevBundle tests passed'
}
finally {
    if (-not $testRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($testRoot) -notlike 'lookdevbundle-tests-*') {
        throw "Refusing cleanup outside the test root: $testRoot"
    }
    if ([IO.Directory]::Exists($testRoot)) { [IO.Directory]::Delete($testRoot, $true) }
}
