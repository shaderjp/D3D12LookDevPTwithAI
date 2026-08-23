[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SuiteDirectory,
    [switch]$Generate
)

$ErrorActionPreference = 'Stop'
$suite = [IO.Path]::GetFullPath($SuiteDirectory)
if (-not (Test-Path -LiteralPath $suite -PathType Container)) { throw "Suite directory was not found: $suite" }
$lockPath = Join-Path $suite 'suite.lock.json'
if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) { throw 'suite.lock.json is missing from the suite.' }
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
$licenseMapPath = Join-Path $suite 'suite-license-map.json'
$sbomPath = Join-Path $suite 'suite-sbom.spdx.json'
$excludedDocuments = @('suite-license-map.json', 'suite-manifest.json', 'suite-sbom.spdx.json')

function Get-RelativePath([string]$Path) {
    [IO.Path]::GetRelativePath($suite, $Path).Replace('\', '/')
}

function New-Record(
    [IO.FileInfo]$File,
    [string]$Component,
    [string]$LicenseConcluded,
    [string[]]$LicenseFiles
) {
    [ordered]@{
        path = Get-RelativePath $File.FullName
        size = $File.Length
        sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        component = $Component
        licenseConcluded = $LicenseConcluded
        licenseFiles = @($LicenseFiles)
    }
}

$d3dAppLicenses = @(
    'LICENSE',
    'licenses/D3D12LookDevPTwithAI/ASSIMP-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/ASSIMP-ZLIB-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/DIRECTXTEX-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/TINYEXR-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/TINYEXR-NOTICE.txt',
    'licenses/D3D12LookDevPTwithAI/TINYEXR-ZSTD-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/TINYGLTF-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/BASIS-UNIVERSAL-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/BASIS-UNIVERSAL-NOTICE.txt',
    'licenses/D3D12LookDevPTwithAI/BASIS-UNIVERSAL-ZSTD-LICENSE.txt'
)
$windowsAppSdkLicenses = @(
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-NOTICE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-BASE-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-BASE-NOTICE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-FOUNDATION-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-INTERACTIVE-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-WINUI-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-WINUI-NOTICE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-AI-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-ML-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-ML-NOTICE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-DWRITE-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-WIDGETS-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-SEARCH-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-RUNTIME-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WINDOWS-APP-SDK-RUNTIME-NOTICE.txt'
)
$webViewLicenses = @(
    'licenses/D3D12LookDevPTwithAI/WEBVIEW2-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/WEBVIEW2-NOTICE.txt'
)
$agilityLicenses = @(
    'licenses/D3D12LookDevPTwithAI/D3D12-AGILITY-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/D3D12-AGILITY-CODE-LICENSE.txt'
)
$dxcLicenses = @(
    'licenses/D3D12LookDevPTwithAI/DXC-LLVM-LICENSE.txt',
    'licenses/D3D12LookDevPTwithAI/DXC-MICROSOFT-LICENSE.txt'
)
$d3dAppLicenses += @(
    'licenses/D3D12LookDevPTwithAI/CPPWINRT-LICENSE.txt'
) + $windowsAppSdkLicenses + $webViewLicenses + $agilityLicenses + $dxcLicenses
$localLicenseFiles = @(
    'LocalMCPChatClient/LICENSE.txt',
    'LocalMCPChatClient/THIRD-PARTY-NOTICES.txt'
) + @(Get-ChildItem -LiteralPath (Join-Path $suite 'LocalMCPChatClient\licenses') -File -ErrorAction SilentlyContinue | ForEach-Object { Get-RelativePath $_.FullName })

$localRoot = Join-Path $suite 'LocalMCPChatClient'
$dependencyAssets = @()
$depsFiles = @(Get-ChildItem -LiteralPath $localRoot -File -Filter '*.deps.json')
if ($depsFiles.Count -ne 1) { throw "Expected one LocalMCPChatClient deps file, found $($depsFiles.Count)." }
$deps = Get-Content -LiteralPath $depsFiles[0].FullName -Raw | ConvertFrom-Json
$ridTargets = @($deps.targets.PSObject.Properties | Where-Object { $_.Name -match '/win-x64$' })
if ($ridTargets.Count -ne 1) { throw "Expected one win-x64 deps target, found $($ridTargets.Count)." }
$target = $ridTargets[0]
foreach ($library in $target.Value.PSObject.Properties) {
    foreach ($groupName in @('runtime', 'native', 'resources', 'runtimeTargets')) {
        $group = $library.Value.PSObject.Properties[$groupName]
        if ($null -eq $group) { continue }
        foreach ($asset in $group.Value.PSObject.Properties) {
            $assetPath = $asset.Name.Replace('\', '/')
            $dependencyAssets += [pscustomobject]@{
                AssetPath = $assetPath
                FileName = [IO.Path]::GetFileName($assetPath)
                Component = $library.Name
            }
        }
    }
}

function Resolve-LocalComponent([string]$RelativePath) {
    $localPath = $RelativePath.Substring('LocalMCPChatClient/'.Length)
    if ($localPath -in @('LocalMCPChatClient.exe', 'LocalMCPChatClient.deps.json', 'LocalMCPChatClient.runtimeconfig.json') -or
        $localPath -match '^LocalMCPChatClient\.(App|Core|Infrastructure)\.dll$') {
        return 'LocalMCPChatClient'
    }
    if ($localPath -eq 'LICENSE.txt' -or $localPath -eq 'THIRD-PARTY-NOTICES.txt' -or $localPath.StartsWith('licenses/', [StringComparison]::OrdinalIgnoreCase)) {
        return 'LocalMCPChatClient license documents'
    }
    $candidates = @($dependencyAssets | Where-Object {
        $_.AssetPath.Equals($localPath, [StringComparison]::OrdinalIgnoreCase) -or
        $_.AssetPath.EndsWith('/' + $localPath, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($candidates.Count -eq 0) {
        $fileName = [IO.Path]::GetFileName($localPath)
        $candidates = @($dependencyAssets | Where-Object { $_.FileName.Equals($fileName, [StringComparison]::OrdinalIgnoreCase) })
    }
    if ($candidates.Count -eq 0 -and $localPath -match '^[^/]+/(?<neutral>.+)\.resources\.dll$') {
        $neutralFileName = $Matches.neutral + '.dll'
        $candidates = @($dependencyAssets | Where-Object { $_.FileName.Equals($neutralFileName, [StringComparison]::OrdinalIgnoreCase) })
    }
    $components = @($candidates.Component | Sort-Object -Unique)
    if ($components.Count -ne 1) { throw "LocalMCPChatClient payload is not uniquely mapped by its deps file: $RelativePath" }
    $components[0]
}

function Get-Record([IO.FileInfo]$File) {
    $path = Get-RelativePath $File.FullName
    if ($path.StartsWith('D3D12LookDevPTwithAI/', [StringComparison]::OrdinalIgnoreCase)) {
        $inside = $path.Substring('D3D12LookDevPTwithAI/'.Length)
        if ($inside.Equals('D3D12/D3D12Core.dll', [StringComparison]::OrdinalIgnoreCase)) {
            return New-Record $File 'Microsoft.Direct3D.D3D12/1.619.3' 'NOASSERTION' $agilityLicenses
        }
        if ($inside -in @('dxcompiler.dll', 'dxil.dll')) {
            return New-Record $File 'Microsoft.Direct3D.DXC/1.9.2602.17' 'NOASSERTION' $dxcLicenses
        }
        if ($inside -in @('Microsoft.Web.WebView2.Core.dll', 'Microsoft.Web.WebView2.Core.winmd')) {
            return New-Record $File 'Microsoft.Web.WebView2/1.0.3719.77' 'NOASSERTION' $webViewLicenses
        }
        if ($inside -eq 'Microsoft.WindowsAppRuntime.Bootstrap.dll') {
            return New-Record $File 'Microsoft.WindowsAppSDK.Foundation/2.3.9' 'NOASSERTION' $windowsAppSdkLicenses
        }
        if ($inside -eq 'D3D12LookDevPTwithAI.exe' -or $inside -match '\.(cso|pri|winmd|xbf)$') {
            return New-Record $File 'D3D12LookDevPTwithAI' 'NOASSERTION' $d3dAppLicenses
        }
        throw "D3D12 payload is not allowlisted: $path"
    }
    if ($path.StartsWith('LocalMCPChatClient/', [StringComparison]::OrdinalIgnoreCase)) {
        $component = Resolve-LocalComponent $path
        $license = if ($component -eq 'LocalMCPChatClient') { 'MIT' } else { 'NOASSERTION' }
        return New-Record $File $component $license $localLicenseFiles
    }
    if ($path -eq 'LICENSE') { return New-Record $File 'D3D12LookDevPTwithAI' 'MIT' @('LICENSE') }
    if ($path.StartsWith('licenses/D3D12LookDevPTwithAI/', [StringComparison]::OrdinalIgnoreCase)) {
        return New-Record $File 'D3D12LookDevPTwithAI license documents' 'NOASSERTION' @($path)
    }
    if ($path -in @(
        'InstallPortableSuite.ps1', 'InstallWindowsAppRuntime.ps1', 'Launch-D3D12LookDevPTwithAI.ps1',
        'REDISTRIBUTION-NOTES.txt', 'suite.lock.json', 'TestPortablePrerequisites.ps1', 'TestSuiteLicenseCompliance.ps1',
        'THIRD-PARTY-NOTICES.txt', 'UNSIGNED-BETA.ja.txt', 'UninstallPortableSuite.ps1'
    )) {
        return New-Record $File 'D3D12LookDevPTwithAI packaging' 'MIT' @('LICENSE')
    }
    throw "Suite payload is not allowlisted: $path"
}

$allFiles = @(Get-ChildItem -LiteralPath $suite -File -Recurse)
$forbidden = @($allFiles | Where-Object {
    $path = Get-RelativePath $_.FullName
    $_.Name -ieq 'D3D12SDKLayers.dll' -or
    $_.Name -match '^(nvngx|sl\.).*\.dll$' -or
    $_.Name -match '^(NRD|RTXDI).*\.(dll|lib)$' -or
    $_.Extension -ieq '.gguf' -or $_.Name -in @('settings.json', 'history.db') -or
    $path.StartsWith('D3D12LookDevPTwithAI/Streamline/', [StringComparison]::OrdinalIgnoreCase)
})
if ($forbidden.Count -gt 0) { throw "Forbidden release payload: $($forbidden.FullName -join ', ')" }

$licenseTexts = @($allFiles | Where-Object {
    $path = Get-RelativePath $_.FullName
    $path -match '(^|/)licenses?/' -or $_.Name -match '(?i)(^LICENSE($|\.)|(?:LICENSE|NOTICE|NOTICES)\.txt$)'
})
$prohibitedLicensePhrases = @(
    'WINDOWS APP SDK ENGINEERING PREVIEW',
    'You may not use the software in a live operating environment',
    'TIME-SENSITIVE SOFTWARE',
    'PRE-RELEASE SOFTWARE'
)
foreach ($file in $licenseTexts) {
    $content = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($phrase in $prohibitedLicensePhrases) {
        if ($content.Contains($phrase, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Prohibited preview-only license text '$phrase' found in $(Get-RelativePath $file.FullName)."
        }
    }
}
$winUiLicense = Join-Path $suite 'licenses\D3D12LookDevPTwithAI\WINDOWS-APP-SDK-WINUI-LICENSE.txt'
if (-not (Test-Path -LiteralPath $winUiLicense -PathType Leaf) -or
    -not (Get-Content -LiteralPath $winUiLicense -Raw).Contains('Any files that are binplaced with your application', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The corrected Windows App SDK distributable-code license is missing.'
}
$localNotice = Join-Path $localRoot 'THIRD-PARTY-NOTICES.txt'
if (-not (Get-Content -LiteralPath $localNotice -Raw).Contains("Version: $($lock.suiteVersion)", [StringComparison]::Ordinal)) {
    throw 'LocalMCPChatClient third-party notice version does not match the suite.'
}

if ($Generate) {
    $records = @($allFiles | Where-Object { (Get-RelativePath $_.FullName) -notin $excludedDocuments } | Sort-Object FullName | ForEach-Object { Get-Record $_ })
    $licenseMap = [ordered]@{
        schemaVersion = 1
        suiteVersion = $lock.suiteVersion
        generatedAtUtc = [DateTimeOffset]::UtcNow.ToString('O')
        excludedGeneratedDocuments = $excludedDocuments
        files = $records
    }
    $licenseMap | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $licenseMapPath -Encoding utf8NoBOM

    $componentIds = @{}
    $packages = @()
    foreach ($component in @($records.component | Sort-Object -Unique)) {
        $id = 'SPDXRef-Package-' + ($component -replace '[^A-Za-z0-9.-]', '-')
        $componentIds[$component] = $id
        $parts = $component -split '/', 2
        $packages += [ordered]@{
            name = $parts[0]
            SPDXID = $id
            versionInfo = if ($parts.Count -eq 2) { $parts[1] } else { [string]$lock.suiteVersion }
            downloadLocation = 'NOASSERTION'
            filesAnalyzed = $true
            licenseConcluded = 'NOASSERTION'
            licenseDeclared = 'NOASSERTION'
            copyrightText = 'NOASSERTION'
        }
    }
    $spdxFiles = @()
    $relationships = @()
    $index = 0
    foreach ($record in $records) {
        $index++
        $fileId = "SPDXRef-File-$index"
        $spdxFiles += [ordered]@{
            fileName = './' + $record.path
            SPDXID = $fileId
            checksums = @(@{ algorithm = 'SHA256'; checksumValue = $record.sha256 })
            licenseConcluded = $record.licenseConcluded
            licenseInfoInFiles = @('NOASSERTION')
            copyrightText = 'NOASSERTION'
        }
        $relationships += [ordered]@{
            spdxElementId = $componentIds[$record.component]
            relationshipType = 'CONTAINS'
            relatedSpdxElement = $fileId
        }
    }
    $sbom = [ordered]@{
        spdxVersion = 'SPDX-2.3'
        dataLicense = 'CC0-1.0'
        SPDXID = 'SPDXRef-DOCUMENT'
        name = "D3D12LookDevPTwithAI-$($lock.suiteVersion)-win-x64"
        documentNamespace = "https://github.com/shaderjp/D3D12LookDevPTwithAI/releases/download/v$($lock.suiteVersion)/sbom-$([guid]::NewGuid())"
        creationInfo = @{ created = [DateTimeOffset]::UtcNow.ToString('O'); creators = @('Tool: Scripts/TestSuiteLicenseCompliance.ps1') }
        packages = $packages
        files = $spdxFiles
        relationships = $relationships
    }
    $sbom | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $sbomPath -Encoding utf8NoBOM
}

if (-not (Test-Path -LiteralPath $licenseMapPath -PathType Leaf) -or -not (Test-Path -LiteralPath $sbomPath -PathType Leaf)) {
    throw 'File-level license map or SPDX SBOM is missing.'
}
$map = Get-Content -LiteralPath $licenseMapPath -Raw | ConvertFrom-Json
$recordsByPath = @{}
foreach ($record in $map.files) {
    if ($recordsByPath.ContainsKey([string]$record.path)) { throw "Duplicate license mapping: $($record.path)" }
    $recordsByPath[[string]$record.path] = $record
}
$payloadFiles = @($allFiles | Where-Object { (Get-RelativePath $_.FullName) -notin $excludedDocuments })
foreach ($file in $payloadFiles) {
    $path = Get-RelativePath $file.FullName
    if (-not $recordsByPath.ContainsKey($path)) { throw "File is missing from the license map: $path" }
    $record = $recordsByPath[$path]
    if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne [string]$record.sha256) {
        throw "License map hash mismatch: $path"
    }
    foreach ($licensePath in @($record.licenseFiles)) {
        if (-not (Test-Path -LiteralPath (Join-Path $suite ([string]$licensePath).Replace('/', '\')) -PathType Leaf)) {
            throw "Mapped license file is missing for ${path}: $licensePath"
        }
    }
}
$extraMappings = @($recordsByPath.Keys | Where-Object { $_ -notin @($payloadFiles | ForEach-Object { Get-RelativePath $_.FullName }) })
if ($extraMappings.Count -gt 0) { throw "License map contains missing files: $($extraMappings -join ', ')" }
$sbom = Get-Content -LiteralPath $sbomPath -Raw | ConvertFrom-Json
if ($sbom.spdxVersion -ne 'SPDX-2.3' -or @($sbom.files).Count -ne $payloadFiles.Count) { throw 'SPDX SBOM coverage is invalid.' }
$sbomHashes = @{}
foreach ($file in $sbom.files) { $sbomHashes[([string]$file.fileName).TrimStart('./')] = [string]$file.checksums[0].checksumValue }
foreach ($record in $map.files) {
    if ($sbomHashes[[string]$record.path] -ne [string]$record.sha256) { throw "SPDX SBOM hash mismatch: $($record.path)" }
}
Write-Host "License allowlist and SPDX SBOM validated: $($payloadFiles.Count) payload files."
