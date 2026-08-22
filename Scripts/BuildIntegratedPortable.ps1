#requires -Version 7.4

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [switch]$SkipBuild,

    [string]$NativeBuildDirectory,

    [string]$ChatHostPublishDirectory,

    [string]$AiArtifactDirectory,

    [string]$AiArtifactManifest,

    [string]$AiRedistributionManifest,

    [switch]$AcceptArtifactLicenses,

    [switch]$AcceptUnsignedArtifactTrust,

    [switch]$WithoutAi,

    [switch]$AllowDirtySource,

    [switch]$NoArchive
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:MaximumInferenceManifestBytes = 64KB
$script:MaximumRedistributionManifestBytes = 64KB
$script:MaximumDepsJsonBytes = 16MB
$script:MaximumLicenseFileBytes = 1MB
$script:MaximumLicenseBytes = 16MB
$script:MaximumRuntimeDependencies = 512
$script:MaximumRuntimeBytes = 32GB
$script:MaximumModelBytes = 64GB
$script:MaximumPayloadBytes = 128GB
$script:MaximumPayloadFiles = 20000
$script:MaximumRelativePathCharacters = 1024
$script:MaximumWindowsRepositoryRootCharacters = 120
$script:MaximumWindowsTransactionRootCharacters = 160
$script:MaximumWindowsBuildWorkspaceRootCharacters = 100
$script:MaximumWindowsPublicationPathCharacters = 240
$script:MaximumWindowsCriticalToolPathCharacters = 240
$script:PayloadRoot = $null
$script:Origins = [Collections.Generic.Dictionary[string,string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
$script:CopiedLicenseBytes = [long]0
$script:NativeLicensePaths = @()
$script:ChatHostLicensePaths = @()
$script:FinalPayloadPeClosureValidated = $false
$script:NativeNuGetPackages = [ordered]@{
    'microsoft.windowsappsdk' = '2.4.0'
    'microsoft.windowsappsdk.base' = '2.0.4'
    'microsoft.windowsappsdk.foundation' = '2.3.9'
    'microsoft.windowsappsdk.interactiveexperiences' = '2.1.6'
    'microsoft.windowsappsdk.winui' = '2.3.6'
    'microsoft.windowsappsdk.ai' = '2.4.4'
    'microsoft.windowsappsdk.ml' = '2.1.74'
    'microsoft.windowsappsdk.dwrite' = '2.1.0'
    'microsoft.windowsappsdk.widgets' = '2.0.5'
    'microsoft.windowsappsdk.search' = '2.4.4'
    'microsoft.windowsappsdk.runtime' = '2.4.0'
    'microsoft.windows.cppwinrt' = '2.0.250303.1'
    'microsoft.web.webview2' = '1.0.3719.77'
    'microsoft.windows.ai.machinelearning' = '2.1.74'
    'microsoft.windows.sdk.buildtools' = '10.0.26100.4654'
    'microsoft.windows.sdk.buildtools.msix' = '1.7.251221100'
    'microsoft.direct3d.d3d12' = '1.619.3'
    'microsoft.direct3d.dxc' = '1.9.2602.17'
}

function Get-NormalizedFullPath {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        throw 'A required path was empty.'
    }
    return [IO.Path]::GetFullPath($PathValue)
}

function Assert-WindowsPathBudget {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][int]$MaximumCharacters,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if (-not $IsWindows) { return }
    $normalized = Get-NormalizedFullPath $PathValue
    if ($normalized.Length -gt $MaximumCharacters) {
        throw "$Description path is too long for the portable build toolchain ($($normalized.Length) > $MaximumCharacters characters). Move the source/output parent to a shorter absolute path."
    }
}

function Assert-IntegratedPortablePathBudget {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$TransactionRoot,
        [Parameter(Mandatory = $true)][string]$BuildWorkspaceRoot,
        [Parameter(Mandatory = $true)][string[]]$PublicationPaths
    )

    if (-not $IsWindows) { return }
    Assert-WindowsPathBudget $Repository `
        $script:MaximumWindowsRepositoryRootCharacters 'Source repository root'
    Assert-WindowsPathBudget $TransactionRoot `
        $script:MaximumWindowsTransactionRootCharacters 'Transaction staging root'
    Assert-WindowsPathBudget $BuildWorkspaceRoot `
        $script:MaximumWindowsBuildWorkspaceRootCharacters 'Build workspace root'
    foreach ($pathValue in $PublicationPaths) {
        Assert-WindowsPathBudget $pathValue `
            $script:MaximumWindowsPublicationPathCharacters 'Publication target'
    }

    # These are the deepest configured paths handed directly to legacy
    # MSBuild/CMake/NuGet path-manipulation code. The dedicated short build
    # workspace also leaves bounded headroom for tool-generated descendants.
    foreach ($pathValue in @(
            (Join-Path $Repository 'D3D12LookDevPTwithAI.vcxproj'),
            (Join-Path $Repository `
                'ThirdParty\DirectXTex\DirectXTex\DirectXTex_Desktop_2026.vcxproj'),
            (Join-Path $Repository `
                'Managed\D3D12LookDevPTwithAI.ChatHost\D3D12LookDevPTwithAI.ChatHost.csproj'),
            (Join-Path $BuildWorkspaceRoot 'b\x\o\project.assets.json'),
            (Join-Path $BuildWorkspaceRoot 'b\u\project.assets.json'),
            (Join-Path $BuildWorkspaceRoot `
                'b\d\a\obj\D3D12LookDevPTwithAI.ChatHost\project.assets.json'))) {
        Assert-WindowsPathBudget $pathValue `
            $script:MaximumWindowsCriticalToolPathCharacters `
            'Critical build intermediate'
    }
}

function Assert-PublishedPayloadPathBudget {
    param(
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][object[]]$FileRecords
    )

    if (-not $IsWindows) { return }
    $relativePaths = @($FileRecords | ForEach-Object { [string]$_.path }) +
        @('integrated-portable-manifest.json')
    foreach ($relativePath in $relativePaths) {
        $projectedPath = Join-Path $OutputDirectory $relativePath.Replace('/', '\')
        Assert-WindowsPathBudget $projectedPath `
            $script:MaximumWindowsPublicationPathCharacters `
            "Published payload file '$relativePath'"
    }
}

function Get-PublishedPayloadPathPolicy {
    param([Parameter(Mandatory = $true)][string[]]$RelativePaths)

    if ($RelativePaths.Count -eq 0) {
        throw 'Cannot derive the publication path policy from an empty payload.'
    }
    $longest = ''
    foreach ($relativePath in $RelativePaths) {
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            throw 'Cannot derive the publication path policy from an empty relative path.'
        }
        if ($relativePath.Length -gt $longest.Length -or
            ($relativePath.Length -eq $longest.Length -and
                [string]::CompareOrdinal($relativePath, $longest) -lt 0)) {
            $longest = $relativePath
        }
    }
    $maximumInstallRootCharacters =
        $script:MaximumWindowsPublicationPathCharacters - 1 - $longest.Length
    if ($maximumInstallRootCharacters -lt 1) {
        throw 'The payload has no usable Windows installation-root path budget.'
    }
    return [ordered]@{
        maxFullPath = $script:MaximumWindowsPublicationPathCharacters
        maxRelativePath = $longest.Length
        maxInstallRootChars = $maximumInstallRootCharacters
        longestRelativePath = $longest
        contract = 'Extract or move the payload only to an absolute installation root whose character count is no greater than maxInstallRootChars; every resulting file path must be no greater than maxFullPath.'
    }
}

function Test-PathsOverlap {
    param(
        [Parameter(Mandatory = $true)][string]$First,
        [Parameter(Mandatory = $true)][string]$Second
    )

    $firstPath = [IO.Path]::TrimEndingDirectorySeparator(
        (Get-NormalizedFullPath $First))
    $secondPath = [IO.Path]::TrimEndingDirectorySeparator(
        (Get-NormalizedFullPath $Second))
    if ($firstPath.Equals($secondPath, [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $separator = [IO.Path]::DirectorySeparatorChar
    return $firstPath.StartsWith(
            $secondPath + $separator, [StringComparison]::OrdinalIgnoreCase) -or
        $secondPath.StartsWith(
            $firstPath + $separator, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-PathsDoNotOverlap {
    param(
        [Parameter(Mandatory = $true)][string]$First,
        [Parameter(Mandatory = $true)][string]$Second,
        [Parameter(Mandatory = $true)][string]$Description
    )
    if (Test-PathsOverlap $First $Second) {
        throw "$Description paths must not overlap."
    }
}

function Restore-ProcessEnvironment {
    param([Parameter(Mandatory = $true)][hashtable]$Values)
    foreach ($name in $Values.Keys) {
        [Environment]::SetEnvironmentVariable(
            [string]$name, $Values[$name], [EnvironmentVariableTarget]::Process)
    }
}

function Assert-NoReparseAncestors {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [switch]$AllowMissingLeaf
    )

    $fullPath = Get-NormalizedFullPath $PathValue
    $pathRoot = [IO.Path]::GetPathRoot($fullPath)
    if ([string]::IsNullOrWhiteSpace($pathRoot)) {
        throw "The path has no filesystem root: $fullPath"
    }

    $current = $pathRoot
    $relative = [IO.Path]::GetRelativePath($pathRoot, $fullPath)
    foreach ($segment in $relative -split '[\\/]') {
        if ([string]::IsNullOrEmpty($segment) -or $segment -eq '.') { continue }
        $current = Join-Path $current $segment
        if (-not (Test-Path -LiteralPath $current)) {
            if ($AllowMissingLeaf) { return }
            throw "Required path does not exist: $current"
        }
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points are not accepted in portable inputs or outputs: $current"
        }
    }
}

function Assert-TreeHasNoReparsePoints {
    param([Parameter(Mandatory = $true)][string]$Root)

    Assert-NoReparseAncestors $Root
    foreach ($item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse content is not accepted: $($item.FullName)"
        }
        if (-not $item.PSIsContainer -and $IsWindows) {
            try {
                $streams = @(Get-Item -LiteralPath $item.FullName -Stream * -Force)
            }
            catch {
                throw "Could not verify file streams for $($item.FullName): $($_.Exception.Message)"
            }
            if ($streams.Count -ne 1 -or [string]$streams[0].Stream -cne ':$DATA') {
                throw "Alternate data streams are not accepted: $($item.FullName)"
            }
        }
    }
}

function Get-SafeRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Child
    )

    $rootPath = [IO.Path]::TrimEndingDirectorySeparator(
        (Get-NormalizedFullPath $Root))
    $childPath = Get-NormalizedFullPath $Child
    $relative = [IO.Path]::GetRelativePath($rootPath, $childPath).Replace('\', '/')
    if ([IO.Path]::IsPathRooted($relative) -or
        $relative -eq '..' -or
        $relative.StartsWith('../', [StringComparison]::Ordinal) -or
        $relative.Length -gt $script:MaximumRelativePathCharacters) {
        throw "A path escapes its declared root: $childPath"
    }
    return $relative
}

function Test-ReservedWindowsName {
    param([Parameter(Mandatory = $true)][string]$Segment)

    $stem = $Segment.Split('.')[0]
    if ($stem -in @('CON', 'PRN', 'AUX', 'NUL')) { return $true }
    return $stem -match '^(COM|LPT)[1-9]$'
}

function Test-SafeRelativeManifestPath {
    param([AllowNull()][string]$RelativePath)

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        $RelativePath.Length -gt $script:MaximumRelativePathCharacters -or
        $RelativePath.Contains('\') -or
        $RelativePath.StartsWith('/', [StringComparison]::Ordinal) -or
        $RelativePath.EndsWith('/', [StringComparison]::Ordinal) -or
        [IO.Path]::IsPathRooted($RelativePath)) {
        return $false
    }
    foreach ($segment in $RelativePath.Split('/')) {
        if ([string]::IsNullOrWhiteSpace($segment) -or
            $segment -in @('.', '..') -or
            $segment -cne $segment.Trim() -or
            $segment.EndsWith('.', [StringComparison]::Ordinal) -or
            $segment.IndexOfAny([char[]]'<>:"|?*') -ge 0 -or
            ($segment.ToCharArray() | Where-Object { [char]::IsControl($_) }) -or
            (Test-ReservedWindowsName $segment)) {
            return $false
        }
    }
    return $true
}

function Resolve-SafeChildFile {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if (-not (Test-SafeRelativeManifestPath $RelativePath)) {
        throw "Unsafe manifest path: $RelativePath"
    }
    $rootPath = [IO.Path]::TrimEndingDirectorySeparator(
        (Get-NormalizedFullPath $Root))
    $candidate = Get-NormalizedFullPath (
        Join-Path $rootPath $RelativePath.Replace('/', '\'))
    $prefix = $rootPath + [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escaped its root: $RelativePath"
    }
    Assert-NoReparseAncestors $candidate
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw "Manifest file is missing: $RelativePath"
    }
    return $candidate
}

function Resolve-SafeChildDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath
    )

    if (-not (Test-SafeRelativeManifestPath $RelativePath)) {
        throw "Unsafe child directory path: $RelativePath"
    }
    $rootPath = [IO.Path]::TrimEndingDirectorySeparator(
        (Get-NormalizedFullPath $Root))
    $candidate = Get-NormalizedFullPath (
        Join-Path $rootPath $RelativePath.Replace('/', '\'))
    if (-not $candidate.StartsWith(
            $rootPath + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Child directory escaped its root: $RelativePath"
    }
    Assert-NoReparseAncestors $candidate
    if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
        throw "Required child directory is missing: $RelativePath"
    }
    return $candidate
}

function Assert-JsonHasUniqueProperties {
    param(
        [Parameter(Mandatory = $true)][System.Text.Json.JsonElement]$Element,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Object) {
        $names = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
        foreach ($property in $Element.EnumerateObject()) {
            if (-not $names.Add($property.Name)) {
                throw "Duplicate JSON property in ${Context}: $($property.Name)"
            }
            Assert-JsonHasUniqueProperties $property.Value "$Context.$($property.Name)"
        }
    }
    elseif ($Element.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        $index = 0
        foreach ($item in $Element.EnumerateArray()) {
            Assert-JsonHasUniqueProperties $item "$Context[$index]"
            ++$index
        }
    }
}

function Get-LowerSha256Bytes {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    return [Convert]::ToHexString(
        [Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function Read-BoundedRegularFileSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][long]$MaximumBytes,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ($MaximumBytes -le 0 -or $MaximumBytes -gt [int]::MaxValue) {
        throw "Unsupported bounded snapshot size for ${Description}: $MaximumBytes"
    }
    Assert-NoReparseAncestors $PathValue
    $file = Get-Item -LiteralPath $PathValue -Force
    if ($file.PSIsContainer -or
        ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Description must be a regular non-reparse file."
    }
    if ($IsWindows) {
        $streams = @(Get-Item -LiteralPath $file.FullName -Stream * -Force)
        if ($streams.Count -ne 1 -or [string]$streams[0].Stream -cne ':$DATA') {
            throw "$Description must not contain alternate data streams."
        }
    }
    $stream = [IO.FileStream]::new(
        $file.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read, 4096, [IO.FileOptions]::SequentialScan)
    try {
        $length = [long]$stream.Length
        if ($length -le 0 -or $length -gt $MaximumBytes) {
            throw "$Description must be a nonempty regular file no larger than $MaximumBytes bytes."
        }
        $bytes = [byte[]]::new([int]$length)
        $offset = 0
        while ($offset -lt $bytes.Length) {
            $read = $stream.Read($bytes, $offset, $bytes.Length - $offset)
            if ($read -le 0) {
                throw "$Description changed or was truncated while its bounded snapshot was read."
            }
            $offset += $read
        }
        if ($stream.ReadByte() -ne -1 -or $stream.Length -ne $length) {
            throw "$Description changed or grew while its bounded snapshot was read."
        }
        return [pscustomobject]@{
            Bytes = $bytes
            Size = $length
            Sha256 = Get-LowerSha256Bytes $bytes
        }
    }
    finally { $stream.Dispose() }
}

function Read-StrictJsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][long]$MaximumBytes,
        [Parameter(Mandatory = $true)][string]$Description,
        [switch]$ReturnSnapshot
    )

    $snapshot = Read-BoundedRegularFileSnapshot $PathValue $MaximumBytes $Description
    $bytes = [byte[]]$snapshot.Bytes
    if ($bytes.Length -ge 2 -and
        (($bytes[0] -eq 0xff -and $bytes[1] -eq 0xfe) -or
         ($bytes[0] -eq 0xfe -and $bytes[1] -eq 0xff)) -or
        ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and
         $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf)) {
        throw "$Description must be UTF-8 without a byte-order mark."
    }
    try {
        $json = [Text.UTF8Encoding]::new($false, $true).GetString($bytes)
    }
    catch {
        throw "$Description is not strict UTF-8: $($_.Exception.Message)"
    }
    $options = [System.Text.Json.JsonDocumentOptions]::new()
    $options.AllowTrailingCommas = $false
    $options.CommentHandling = [System.Text.Json.JsonCommentHandling]::Disallow
    $options.MaxDepth = 64
    $document = [System.Text.Json.JsonDocument]::Parse($json, $options)
    try {
        Assert-JsonHasUniqueProperties $document.RootElement $Description
    }
    finally {
        $document.Dispose()
    }
    try {
        $value = $json | ConvertFrom-Json
    }
    catch {
        throw "$Description is not valid JSON: $($_.Exception.Message)"
    }
    if ($ReturnSnapshot) {
        return [pscustomobject]@{
            Value = $value
            Bytes = $bytes
            Size = [long]$snapshot.Size
            Sha256 = [string]$snapshot.Sha256
        }
    }
    return $value
}

function Invoke-TestOnlyPause {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][hashtable]$Metadata = $null
    )

    if ($Name -cnotmatch '^[A-Z][A-Z0-9_]{0,63}$') {
        throw "Unsafe test pause name: $Name"
    }
    $environmentName = "D3D12LOOKDEVPT_PORTABLE_TEST_PAUSE_$Name"
    $pauseDirectory = [Environment]::GetEnvironmentVariable($environmentName)
    if ([string]::IsNullOrWhiteSpace($pauseDirectory)) { return }
    if (-not $SkipBuild -or -not $AllowDirtySource -or -not $NoArchive) {
        throw "$environmentName is test-only and requires SkipBuild, AllowDirtySource, and NoArchive."
    }
    $pauseRoot = Get-NormalizedFullPath $pauseDirectory
    Assert-NoReparseAncestors $pauseRoot
    if (-not (Test-Path -LiteralPath $pauseRoot -PathType Container)) {
        throw "Test pause directory is missing: $pauseRoot"
    }
    $reached = Join-Path $pauseRoot 'reached'
    $continue = Join-Path $pauseRoot 'continue'
    if ($null -ne $Metadata) {
        [IO.File]::WriteAllText(
            (Join-Path $pauseRoot 'layout.json'),
            ($Metadata | ConvertTo-Json -Depth 3 -Compress),
            [Text.UTF8Encoding]::new($false))
    }
    [IO.File]::WriteAllText($reached, $Name, [Text.UTF8Encoding]::new($false))
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while (-not (Test-Path -LiteralPath $continue -PathType Leaf)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Timed out at test-only pause $Name."
        }
        Start-Sleep -Milliseconds 10
    }
}

function Assert-ExactProperties {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string[]]$Expected,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $actual = @($Object.PSObject.Properties.Name)
    if ($actual.Count -ne $Expected.Count) {
        throw "$Context has an unexpected property set."
    }
    foreach ($name in $Expected) {
        if ($actual -cnotcontains $name) {
            throw "$Context is missing the exact property '$name'."
        }
    }
}

function Test-Sha256Text {
    param([AllowNull()][string]$Value)
    return $Value -cmatch '^[0-9a-f]{64}$'
}

function Test-JsonIntegerValue {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return $false }
    return $Value.GetType() -in @(
        [byte], [sbyte], [int16], [uint16], [int32], [uint32], [int64])
}

function Test-JsonNumberValue {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return $false }
    return $Value.GetType() -in @(
        [byte], [sbyte], [int16], [uint16], [int32], [uint32], [int64],
        [single], [double], [decimal])
}

function Get-LowerSha256 {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    return (Get-FileHash -LiteralPath $PathValue -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-LowerSha256Text {
    param([Parameter(Mandatory = $true)][string]$Text)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}

function Get-SpdxVerificationCode {
    param([Parameter(Mandatory = $true)][string[]]$FileSha1s)
    if ($FileSha1s.Count -eq 0) {
        throw 'An SPDX package verification set must not be empty.'
    }
    $inputText = [string]::Join('', @($FileSha1s | Sort-Object))
    $bytes = [Text.Encoding]::ASCII.GetBytes($inputText)
    return [Convert]::ToHexString(
        [Security.Cryptography.SHA1]::HashData($bytes)).ToLowerInvariant()
}

function Assert-GgufHeader {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    $stream = [IO.File]::OpenRead($PathValue)
    try {
        if ($stream.Length -lt 8) { throw 'GGUF model is shorter than its header.' }
        $header = [byte[]]::new(8)
        if ($stream.Read($header, 0, $header.Length) -ne $header.Length -or
            [Text.Encoding]::ASCII.GetString($header, 0, 4) -cne 'GGUF') {
            throw 'GGUF model magic is invalid.'
        }
        $version = [BitConverter]::ToUInt32($header, 4)
        if ($version -notin @(2, 3)) {
            throw "GGUF model version is unsupported: $version"
        }
    }
    finally { $stream.Dispose() }
}

function Assert-ArtifactRecord {
    param(
        [Parameter(Mandatory = $true)][object]$Record,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][bool]$MayBeEmpty
    )

    Assert-ExactProperties $Record @('relativePath', 'sha256', 'expectedSize') "$Kind artifact"
    if ($Record.relativePath -isnot [string] -or $Record.sha256 -isnot [string]) {
        throw "$Kind artifact path/hash must be JSON strings."
    }
    $relativePath = [string]$Record.relativePath
    $sha256 = [string]$Record.sha256
    if (-not (Test-JsonIntegerValue $Record.expectedSize)) {
        throw "$Kind artifact expectedSize is not a JSON integer."
    }
    try { $expectedSize = [long]$Record.expectedSize }
    catch { throw "$Kind artifact expectedSize is not a 64-bit integer." }
    if (-not (Test-SafeRelativeManifestPath $relativePath) -or
        -not (Test-Sha256Text $sha256) -or
        ($MayBeEmpty ? ($expectedSize -lt 0) : ($expectedSize -le 0))) {
        throw "$Kind artifact metadata is invalid."
    }
    $path = Resolve-SafeChildFile $Root $relativePath
    $file = Get-Item -LiteralPath $path -Force
    if ([long]$file.Length -ne $expectedSize -or
        (Get-LowerSha256 $path) -cne $sha256) {
        throw "$Kind artifact size/hash mismatch: $relativePath"
    }
    return [pscustomobject]@{
        RelativePath = $relativePath
        FullPath = $path
        Size = $expectedSize
        Sha256 = $sha256
    }
}

function Test-SpdxExpression {
    param([AllowNull()][string]$Expression)

    if ([string]::IsNullOrWhiteSpace($Expression) -or
        $Expression.Length -gt 128 -or
        $Expression -match '[\x00-\x1f\x7f]') {
        return $false
    }
    $known = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($identifier in @(
        'MIT', 'Apache-2.0', 'BSD-2-Clause', 'BSD-3-Clause', 'ISC', 'Zlib',
        'Unicode-3.0', 'Unlicense', 'CC0-1.0', 'CC-BY-4.0',
        'CC-BY-NC-4.0')) {
        [void]$known.Add($identifier)
    }
    # Redistribution schema v1 deliberately accepts one identifier only.  This
    # avoids blessing malformed compound expressions without a full SPDX parser.
    return $known.Contains($Expression) -or
        $Expression -cmatch '^LicenseRef-[A-Za-z0-9][A-Za-z0-9.-]{0,116}$'
}

function Assert-HttpsSourceUri {
    param([AllowNull()][string]$Value, [string]$Context)

    $uri = $null
    if ([string]::IsNullOrWhiteSpace($Value) -or
        $Value.Length -gt 2048 -or
        $Value -match '[\x00-\x20\x7f]' -or
        -not [Uri]::TryCreate($Value, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -cne 'https' -or
        -not [string]::IsNullOrEmpty($uri.UserInfo) -or
        -not [string]::IsNullOrEmpty($uri.Query) -or
        -not [string]::IsNullOrEmpty($uri.Fragment)) {
        throw "$Context sourceUrl must be an HTTPS URI without credentials, query, or fragment."
    }
}

function Assert-SafeIdentityText {
    param([AllowNull()][string]$Value, [string]$Context)
    if ([string]::IsNullOrWhiteSpace($Value) -or
        $Value.Length -gt 256 -or
        $Value -cne $Value.Trim() -or
        $Value -match '[\x00-\x1f\x7f]') {
        throw "$Context must be nonempty bounded text without control characters."
    }
}

function Test-SensitiveFileName {
    param([Parameter(Mandatory = $true)][string]$Name)

    $lower = $Name.ToLowerInvariant()
    # This is a signed framework assembly name selected by the exact
    # ChatHost deps.json inventory, not a user-secrets payload file.
    if ($lower -ceq 'microsoft.extensions.configuration.usersecrets.dll') {
        return $false
    }
    return $lower -in @(
        'chat-history.sqlite3', 'chat-history.sqlite3-wal',
        'chat-history.sqlite3-shm', 'history.db', 'history.db-wal',
        'history.db-shm', 'settings.json', 'startup.json', 'ui.json',
        'mcp-paired-clients.json', 'approval-rules.json') -or
        $lower -like '*.sqlite' -or
        $lower -like '*.sqlite-wal' -or
        $lower -like '*.sqlite-shm' -or
        $lower -eq '.env' -or
        $lower.StartsWith('.env.', [StringComparison]::Ordinal) -or
        $lower.StartsWith('inference.json.backup-', [StringComparison]::Ordinal) -or
        $lower.StartsWith('.inference.json.staging-', [StringComparison]::Ordinal) -or
        $lower -like '*.db' -or
        $lower -like '*.db-wal' -or
        $lower -like '*.db-shm' -or
        $lower -like '*.key' -or
        $lower -like '*.pem' -or
        $lower -like '*.pfx' -or
        $lower -like '*.p12' -or
        $lower -like '*.log' -or
        $lower -like '*.dmp' -or
        $lower -like '*.dump' -or
        $lower -like '*.kdbx' -or
        $lower -like '*.cred' -or
        $lower -in @('id_rsa', 'id_ed25519', '.git-credentials') -or
        $lower -match '(token|secret|password|credential|api[-_]?key|private[-_]?key|bearer|authorization)'
}

function Get-AiPackageDefinition {
    param(
        [Parameter(Mandatory = $true)][string]$ArtifactRoot,
        [Parameter(Mandatory = $true)][string]$InferenceManifestPath,
        [Parameter(Mandatory = $true)][string]$RedistributionManifestPath
    )

    Assert-TreeHasNoReparsePoints $ArtifactRoot
    foreach ($file in Get-ChildItem -LiteralPath $ArtifactRoot -File -Recurse -Force) {
        if (Test-SensitiveFileName $file.Name) {
            throw "Refusing to package sensitive AI state: $($file.FullName)"
        }
    }

    $artifactRootPath = [IO.Path]::TrimEndingDirectorySeparator(
        (Get-NormalizedFullPath $ArtifactRoot))
    $expectedInferencePath = Get-NormalizedFullPath (
        Join-Path $artifactRootPath 'inference.json')
    if (-not (Get-NormalizedFullPath $InferenceManifestPath).Equals(
            $expectedInferencePath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'AiArtifactManifest must explicitly name inference.json at the AI artifact root.'
    }

    $settingsSnapshot = Read-StrictJsonFile $InferenceManifestPath `
        $script:MaximumInferenceManifestBytes 'AI inference manifest' `
        -ReturnSnapshot
    $settings = $settingsSnapshot.Value
    Assert-ExactProperties $settings @(
        'schemaVersion', 'modelId', 'backend', 'contextSize', 'maxTokens',
        'temperature', 'model', 'runtime', 'runtimeDependencies') 'AI inference manifest'
    if (-not (Test-JsonIntegerValue $settings.schemaVersion) -or
        [long]$settings.schemaVersion -ne 1 -or
        $settings.backend -isnot [string] -or
        $settings.modelId -isnot [string] -or
        [string]$settings.backend -cnotin @('cpu', 'cuda', 'vulkan') -or
        [string]::IsNullOrWhiteSpace([string]$settings.modelId) -or
        ([string]$settings.modelId).Length -gt 64 -or
        [string]$settings.modelId -cnotmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?$' -or
        -not (Test-JsonIntegerValue $settings.contextSize) -or
        [long]$settings.contextSize -lt 512 -or
        [long]$settings.contextSize -gt 131072 -or
        -not (Test-JsonIntegerValue $settings.maxTokens) -or
        [long]$settings.maxTokens -lt 64 -or
        [long]$settings.maxTokens -gt 32768 -or
        -not (Test-JsonNumberValue $settings.temperature) -or
        -not [double]::IsFinite([double]$settings.temperature) -or
        [double]$settings.temperature -lt 0 -or
        [double]$settings.temperature -gt 2 -or
        $settings.model -isnot [pscustomobject] -or
        $settings.runtime -isnot [pscustomobject] -or
        $settings.runtimeDependencies -isnot [Array]) {
        throw 'The AI inference manifest identity is invalid.'
    }

    $modelsRoot = Join-Path $artifactRootPath 'Models'
    $runtimesRoot = Join-Path $artifactRootPath 'Runtimes'
    if (-not (Test-Path -LiteralPath $modelsRoot -PathType Container) -or
        -not (Test-Path -LiteralPath $runtimesRoot -PathType Container)) {
        throw 'AI artifact root must contain Models and Runtimes directories.'
    }
    $model = Assert-ArtifactRecord $settings.model $modelsRoot 'model' $false
    if (-not $model.RelativePath.EndsWith('.gguf', [StringComparison]::OrdinalIgnoreCase) -or
        $model.Size -gt $script:MaximumModelBytes) {
        throw 'The configured model must be one GGUF within the portable size bound.'
    }
    Assert-GgufHeader $model.FullPath
    $runtime = Assert-ArtifactRecord $settings.runtime $runtimesRoot 'runtime' $false
    if ([IO.Path]::GetFileName($runtime.RelativePath) -cne 'llama-server.exe') {
        throw 'The configured runtime artifact must be llama-server.exe.'
    }
    Assert-X64Pe $runtime.FullPath 'AI llama-server.exe'

    $dependencyDocuments = @($settings.runtimeDependencies)
    if ($dependencyDocuments.Count -gt $script:MaximumRuntimeDependencies) {
        throw 'The AI runtime dependency manifest is too large.'
    }
    $runtimeDirectory = [IO.Path]::GetDirectoryName($runtime.RelativePath).Replace('\', '/')
    $runtimePaths = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    [void]$runtimePaths.Add($runtime.RelativePath)
    $runtimeArtifacts = [Collections.Generic.List[object]]::new()
    $runtimeArtifacts.Add($runtime)
    $previousDependency = $null
    $runtimeBytes = [long]$runtime.Size
    foreach ($record in $dependencyDocuments) {
        $dependency = Assert-ArtifactRecord $record $runtimesRoot 'runtime dependency' $true
        if ([IO.Path]::GetExtension($dependency.RelativePath) -in @('.dll', '.exe')) {
            Assert-X64Pe $dependency.FullPath "AI runtime dependency $($dependency.RelativePath)"
            $dependencyDirectory = [IO.Path]::GetDirectoryName(
                $dependency.RelativePath).Replace('\', '/')
            if (-not $dependencyDirectory.Equals(
                    $runtimeDirectory, [StringComparison]::OrdinalIgnoreCase)) {
                throw 'PE runtime dependencies must be adjacent to llama-server.exe; Windows loader lookup is not recursive.'
            }
        }
        if (($runtimeDirectory.Length -gt 0 -and
             -not $dependency.RelativePath.StartsWith(
                $runtimeDirectory + '/', [StringComparison]::OrdinalIgnoreCase)) -or
            -not $runtimePaths.Add($dependency.RelativePath) -or
            ($null -ne $previousDependency -and
             [string]::CompareOrdinal($previousDependency, $dependency.RelativePath) -ge 0)) {
            throw 'Runtime dependency paths must be unique, sorted, and inside the llama-server directory.'
        }
        $previousDependency = $dependency.RelativePath
        if ([long]$dependency.Size -gt $script:MaximumRuntimeBytes - $runtimeBytes) {
            throw 'The AI runtime size metadata overflows its safety bound.'
        }
        $runtimeBytes += [long]$dependency.Size
        if ($runtimeBytes -gt $script:MaximumRuntimeBytes) {
            throw 'The AI runtime exceeds the portable size bound.'
        }
        $runtimeArtifacts.Add($dependency)
    }

    $actualModelPaths = @(
        Get-ChildItem -LiteralPath $modelsRoot -File -Recurse -Force |
            ForEach-Object { Get-SafeRelativePath $modelsRoot $_.FullName })
    if ($actualModelPaths.Count -ne 1 -or
        -not $actualModelPaths[0].Equals(
            $model.RelativePath, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Models must contain exactly the single GGUF named by inference.json.'
    }
    $actualRuntimePaths = @(
        Get-ChildItem -LiteralPath $runtimesRoot -File -Recurse -Force |
            ForEach-Object { Get-SafeRelativePath $runtimesRoot $_.FullName })
    if ($actualRuntimePaths.Count -ne $runtimePaths.Count) {
        throw 'Runtimes contains files that are absent from inference.json.'
    }
    foreach ($path in $actualRuntimePaths) {
        if (-not $runtimePaths.Contains($path)) {
            throw "Unmanifested runtime file: $path"
        }
    }
    Assert-AiRuntimeImportClosure $runtimeArtifacts.ToArray() ([string]$settings.backend)

    $redistributionSnapshot = Read-StrictJsonFile $RedistributionManifestPath `
        $script:MaximumRedistributionManifestBytes 'AI redistribution manifest' `
        -ReturnSnapshot
    $redistribution = $redistributionSnapshot.Value
    Assert-ExactProperties $redistribution @('schemaVersion', 'model', 'runtime') `
        'AI redistribution manifest'
    if (-not (Test-JsonIntegerValue $redistribution.schemaVersion) -or
        [long]$redistribution.schemaVersion -ne 1 -or
        $redistribution.model -isnot [pscustomobject] -or
        $redistribution.runtime -isnot [pscustomobject]) {
        throw 'Unsupported AI redistribution manifest schema.'
    }

    $components = [ordered]@{}
    foreach ($kind in @('model', 'runtime')) {
        $component = $redistribution.$kind
        Assert-ExactProperties $component @(
            'name', 'revision', 'sourceUrl', 'licenseExpression', 'licenseFile') `
            "AI redistribution $kind"
        foreach ($stringField in @(
                'name', 'revision', 'sourceUrl', 'licenseExpression', 'licenseFile')) {
            if ($component.$stringField -isnot [string]) {
                throw "AI redistribution $kind.$stringField must be a JSON string."
            }
        }
        Assert-SafeIdentityText ([string]$component.name) "$kind name"
        Assert-SafeIdentityText ([string]$component.revision) "$kind revision"
        Assert-HttpsSourceUri ([string]$component.sourceUrl) "$kind"
        if (-not (Test-SpdxExpression ([string]$component.licenseExpression))) {
            throw "$kind licenseExpression is not an accepted bounded SPDX expression."
        }
        $licenseRelative = [string]$component.licenseFile
        $licensePath = Resolve-SafeChildFile $artifactRootPath $licenseRelative
        $license = Get-Item -LiteralPath $licensePath -Force
        if ($license.Extension.ToLowerInvariant() -notin @('.txt', '.md', '.html')) {
            throw "$kind license file must be nonempty and at most 1 MiB."
        }
        $licenseSnapshot = Read-BoundedRegularFileSnapshot $licensePath `
            $script:MaximumLicenseFileBytes "$kind license file"
        $components[$kind] = [pscustomobject]@{
            Name = [string]$component.name
            Revision = [string]$component.revision
            SourceUrl = [string]$component.sourceUrl
            LicenseExpression = [string]$component.licenseExpression
            LicenseRelativePath = $licenseRelative
            LicenseBytes = [byte[]]$licenseSnapshot.Bytes
            LicenseSha256 = [string]$licenseSnapshot.Sha256
            LicenseSize = [long]$licenseSnapshot.Size
        }
    }
    if ($components.model.LicenseExpression -cmatch '^LicenseRef-' -and
        $components.model.LicenseExpression -ceq $components.runtime.LicenseExpression -and
        $components.model.LicenseSha256 -cne $components.runtime.LicenseSha256) {
        throw 'One LicenseRef cannot identify different model and runtime license texts.'
    }

    $allowedRootFiles = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    [void]$allowedRootFiles.Add('inference.json')
    [void]$allowedRootFiles.Add((Get-SafeRelativePath $artifactRootPath $RedistributionManifestPath))
    foreach ($component in $components.Values) {
        [void]$allowedRootFiles.Add($component.LicenseRelativePath)
    }
    foreach ($file in Get-ChildItem -LiteralPath $artifactRootPath -File -Recurse -Force) {
        $relative = Get-SafeRelativePath $artifactRootPath $file.FullName
        if ($relative.StartsWith('Models/', [StringComparison]::OrdinalIgnoreCase) -or
            $relative.StartsWith('Runtimes/', [StringComparison]::OrdinalIgnoreCase) -or
            $allowedRootFiles.Contains($relative)) {
            continue
        }
        throw "Unexpected file in AI artifact source: $relative"
    }

    return [pscustomobject]@{
        Root = $artifactRootPath
        InferenceBytes = [byte[]]$settingsSnapshot.Bytes
        InferenceSha256 = [string]$settingsSnapshot.Sha256
        InferenceSize = [long]$settingsSnapshot.Size
        RedistributionBytes = [byte[]]$redistributionSnapshot.Bytes
        RedistributionSha256 = [string]$redistributionSnapshot.Sha256
        RedistributionSize = [long]$redistributionSnapshot.Size
        Model = $model
        RuntimeArtifacts = $runtimeArtifacts.ToArray()
        Components = $components
        Backend = [string]$settings.backend
        ModelId = [string]$settings.modelId
    }
}

function Resolve-PayloadDestination {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    if (-not (Test-SafeRelativeManifestPath $RelativePath)) {
        throw "Unsafe payload destination: $RelativePath"
    }
    $target = Get-NormalizedFullPath (
        Join-Path $script:PayloadRoot $RelativePath.Replace('/', '\'))
    $rootPrefix = [IO.Path]::TrimEndingDirectorySeparator($script:PayloadRoot) +
        [IO.Path]::DirectorySeparatorChar
    if (-not $target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Payload destination escaped staging: $RelativePath"
    }
    return $target
}

function Copy-RegisteredPayloadFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativeDestination,
        [Parameter(Mandatory = $true)][string]$Origin,
        [long]$ExpectedSize = -1,
        [string]$ExpectedSha256 = ''
    )

    Assert-NoReparseAncestors $Source
    if (Test-SensitiveFileName ([IO.Path]::GetFileName($RelativeDestination))) {
        throw "Sensitive state cannot enter the portable payload: $RelativeDestination"
    }
    $normalized = $RelativeDestination.Replace('\', '/')
    if ($script:Origins.ContainsKey($normalized)) {
        throw "Payload collision between '$($script:Origins[$normalized])' and '$Origin': $normalized"
    }
    $target = Resolve-PayloadDestination $normalized
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
    if ($ExpectedSize -ge 0 -or -not [string]::IsNullOrEmpty($ExpectedSha256)) {
        $copied = Get-Item -LiteralPath $target -Force
        if ($ExpectedSize -lt 0 -or -not (Test-Sha256Text $ExpectedSha256) -or
            [long]$copied.Length -ne $ExpectedSize -or
            (Get-LowerSha256 $target) -cne $ExpectedSha256) {
            throw "Payload source changed after validation: $normalized"
        }
    }
    $script:Origins.Add($normalized, $Origin)
}

function Write-RegisteredPayloadBytes {
    param(
        [Parameter(Mandatory = $true)][string]$RelativeDestination,
        [Parameter(Mandatory = $true)][byte[]]$Bytes,
        [Parameter(Mandatory = $true)][string]$Origin,
        [long]$ExpectedSize = -1,
        [string]$ExpectedSha256 = ''
    )

    $normalized = $RelativeDestination.Replace('\', '/')
    if ($script:Origins.ContainsKey($normalized)) {
        throw "Generated payload collision: $normalized"
    }
    $target = Resolve-PayloadDestination $normalized
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    [IO.File]::WriteAllBytes($target, $Bytes)
    if ($ExpectedSize -ge 0 -or -not [string]::IsNullOrEmpty($ExpectedSha256)) {
        if ($ExpectedSize -lt 0 -or -not (Test-Sha256Text $ExpectedSha256) -or
            [long]$Bytes.Length -ne $ExpectedSize -or
            (Get-LowerSha256Bytes $Bytes) -cne $ExpectedSha256) {
            throw "Payload snapshot does not match its validated identity: $normalized"
        }
    }
    $script:Origins.Add($normalized, $Origin)
}

function Write-RegisteredPayloadText {
    param(
        [Parameter(Mandatory = $true)][string]$RelativeDestination,
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$Origin
    )

    $normalized = $RelativeDestination.Replace('\', '/')
    if ($script:Origins.ContainsKey($normalized)) {
        throw "Generated payload collision: $normalized"
    }
    $target = Resolve-PayloadDestination $normalized
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    Set-Content -LiteralPath $target -Value $Text -Encoding utf8NoBOM
    $script:Origins.Add($normalized, $Origin)
}

function Get-ExpectedShaderNames {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    [xml]$project = Get-Content -LiteralPath $ProjectPath -Raw
    $target = @($project.Project.Target | Where-Object Name -eq 'CompileShaders')
    if ($target.Count -ne 1) { throw 'CompileShaders target could not be inventoried.' }
    $names = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($match in [regex]::Matches(
            [string]$target[0].Outputs,
            '\$\([^)]+\)([^;]+\.cso)')) {
        [void]$names.Add($match.Groups[1].Value)
    }
    if ($names.Count -lt 20) { throw 'The shader output allowlist is unexpectedly small.' }
    return $names
}

function Test-NativePayloadPathAllowed {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)]$ShaderNames
    )

    $path = $RelativePath.Replace('\', '/')
    if ($path -in @(
            'D3D12LookDevPTwithAI.exe', 'D3D12LookDevPTwithAI.pri',
            'D3D12LookDevPTwithAI.winmd', 'App.winmd',
            'D3D12/D3D12Core.dll', 'Source/WinUI/App.xbf',
            'Source/WinUI/MainWindow.xbf',
            'Microsoft.UI.Xaml/Assets/map.html',
            'Microsoft.UI.Xaml/Assets/NoiseAsset_256X256_PNG.png')) {
        return $true
    }
    if (-not $path.Contains('/') -and $ShaderNames.Contains($path)) { return $true }
    if ($path -cmatch '^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8}){1,2}/Microsoft\.(?:ui\.xaml|UI\.Xaml\.Phone)\.dll\.mui$') {
        return $true
    }
    if ($path.Contains('/')) { return $false }
    if ($path -cmatch '^Microsoft\..+\.(?:dll|winmd|pri)$') { return $true }
    if ($path -cmatch '^workloads(?:\.[A-Za-z0-9]+)?\.json$') { return $true }
    return $path -in @(
        'CoreMessagingXP.dll', 'dcompi.dll', 'DirectML.dll', 'dwmcorei.dll',
        'DwmSceneI.dll', 'DWriteCore.dll', 'dxcompiler.dll', 'dxil.dll',
        'marshal.dll', 'MRM.dll', 'NPUDetect.dll', 'onnxruntime.dll',
        'PerceptiveStreaming.dll', 'PushNotificationsLongRunningTask.ProxyStub.dll',
        'RestartAgent.exe', 'SessionHandleIPCProxyStub.dll', 'WinUIEdit.dll',
        'wuceffectsi.dll')
}

function Read-ExactStreamRange {
    param(
        [Parameter(Mandatory = $true)][IO.FileStream]$Stream,
        [Parameter(Mandatory = $true)][long]$Offset,
        [Parameter(Mandatory = $true)][int]$Count,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ($Offset -lt 0 -or $Count -lt 0 -or
        $Offset -gt $Stream.Length - $Count) {
        throw "$Description is outside the PE file."
    }
    $buffer = [byte[]]::new($Count)
    [void]$Stream.Seek($Offset, [IO.SeekOrigin]::Begin)
    $readTotal = 0
    while ($readTotal -lt $Count) {
        $read = $Stream.Read($buffer, $readTotal, $Count - $readTotal)
        if ($read -le 0) { throw "$Description is truncated." }
        $readTotal += $read
    }
    return ,$buffer
}

function Open-PeInspectionStream {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    Assert-NoReparseAncestors $PathValue
    return [IO.FileStream]::new(
        $PathValue, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        [IO.FileShare]::Read, 4096, [IO.FileOptions]::RandomAccess)
}

function Assert-X64Pe {
    param([Parameter(Mandatory = $true)][string]$PathValue, [string]$Description)

    $stream = Open-PeInspectionStream $PathValue
    try {
        if ($stream.Length -lt 0x40) { throw "$Description is not a PE executable." }
        $dos = [byte[]](Read-ExactStreamRange $stream 0 0x40 "$Description DOS header")
        if ($dos[0] -ne 0x4d -or $dos[1] -ne 0x5a) {
            throw "$Description is not a PE executable."
        }
        $peOffset = [BitConverter]::ToInt32($dos, 0x3c)
        if ($peOffset -lt 0x40 -or $peOffset -gt 16MB -or
            [long]$peOffset + 26 -gt $stream.Length) {
            throw "$Description is not a PE32+ x64 executable."
        }
        $header = [byte[]](Read-ExactStreamRange $stream $peOffset 26 `
            "$Description PE header")
        if ($header[0] -ne 0x50 -or $header[1] -ne 0x45 -or
            $header[2] -ne 0 -or $header[3] -ne 0 -or
            [BitConverter]::ToUInt16($header, 4) -ne 0x8664 -or
            [BitConverter]::ToUInt16($header, 24) -ne 0x20b) {
            throw "$Description is not a PE32+ x64 executable."
        }
    }
    finally { $stream.Dispose() }
}

function Get-PeImportedDllNames {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    $stream = Open-PeInspectionStream $PathValue
    try {
        if ($stream.Length -lt 0x40) { throw "PE DOS header is truncated: $PathValue" }
        $dos = [byte[]](Read-ExactStreamRange $stream 0 0x40 'PE DOS header')
        if ($dos[0] -ne 0x4d -or $dos[1] -ne 0x5a) {
            throw "PE DOS signature is invalid: $PathValue"
        }
        $peOffset = [BitConverter]::ToInt32($dos, 0x3c)
        if ($peOffset -lt 0x40 -or $peOffset -gt 16MB) {
            throw "PE header offset is invalid: $PathValue"
        }
        $coff = [byte[]](Read-ExactStreamRange $stream $peOffset 24 'PE COFF header')
        if ($coff[0] -ne 0x50 -or $coff[1] -ne 0x45 -or
            $coff[2] -ne 0 -or $coff[3] -ne 0) {
            throw "PE COFF header is invalid: $PathValue"
        }
        $machine = [BitConverter]::ToUInt16($coff, 4)
        $sectionCount = [BitConverter]::ToUInt16($coff, 6)
        $optionalSize = [BitConverter]::ToUInt16($coff, 20)
        $optionalOffset = [long]$peOffset + 24
        if ($sectionCount -gt 96 -or $optionalSize -lt 112 -or
            $optionalSize -gt 4096) {
            throw "PE optional header is invalid: $PathValue"
        }
        $optional = [byte[]](Read-ExactStreamRange $stream $optionalOffset `
            $optionalSize 'PE optional header')
        $magic = [BitConverter]::ToUInt16($optional, 0)
        $managedPe32 = $false
        $candidateArm64X = $false
        $clrHeaderRva = [uint32]0
        $clrHeaderSize = [uint32]0
        if ($machine -eq 0x8664 -and $magic -eq 0x20b) {
            $imageBase = [BitConverter]::ToUInt64($optional, 24)
            $importDirectoryOffset = 120
            $delayDirectoryOffset = 216
            $numberOfRvaAndSizes = [BitConverter]::ToUInt32($optional, 108)
        }
        elseif ($machine -eq 0xaa64 -and $magic -eq 0x20b) {
            # Windows App SDK can carry ARM64X/ARM64EC hybrid DLLs in a win-x64
            # self-contained payload.  Plain ARM64 is not x64-loadable, so the
            # ARM64X relocation-metadata section is required after parsing the
            # section table below.
            $candidateArm64X = $true
            $imageBase = [BitConverter]::ToUInt64($optional, 24)
            $importDirectoryOffset = 120
            $delayDirectoryOffset = 216
            $numberOfRvaAndSizes = [BitConverter]::ToUInt32($optional, 108)
        }
        elseif ($machine -eq 0x14c -and $magic -eq 0x10b -and
            $optionalSize -ge 216) {
            # AnyCPU/managed IL assemblies are valid members of a win-x64
            # self-contained publish.  A data-directory pointer by itself is
            # not proof of managed IL; the resolved COR20 header is validated
            # below before any import inventory is accepted.
            $numberOfRvaAndSizes = [BitConverter]::ToUInt32($optional, 92)
            if ($numberOfRvaAndSizes -lt 15) {
                throw "PE32 image has no declared CLR runtime header: $PathValue"
            }
            $clrHeaderRva = [BitConverter]::ToUInt32($optional, 208)
            $clrHeaderSize = [BitConverter]::ToUInt32($optional, 212)
            if ($clrHeaderRva -eq 0 -or $clrHeaderSize -lt 0x48 -or
                $clrHeaderSize -gt 4096) {
                throw "PE32 CLR runtime header metadata is invalid: $PathValue"
            }
            $managedPe32 = $true
            $imageBase = [uint64][BitConverter]::ToUInt32($optional, 28)
            $importDirectoryOffset = 104
            $delayDirectoryOffset = 200
        }
        else {
            throw "PE is neither x64 nor a managed IL assembly: $PathValue"
        }
        $sizeOfHeaders = [BitConverter]::ToUInt32($optional, 60)
        $sections = [Collections.Generic.List[object]]::new()
        $sectionOffset = $optionalOffset + $optionalSize
        $sectionBytes = [byte[]](Read-ExactStreamRange $stream $sectionOffset `
            ([int]$sectionCount * 40) 'PE section table')
        for ($index = 0; $index -lt $sectionCount; ++$index) {
            $header = $index * 40
            $sectionNameLength = 0
            while ($sectionNameLength -lt 8 -and
                $sectionBytes[$header + $sectionNameLength] -ne 0) {
                ++$sectionNameLength
            }
            $sectionName = [Text.Encoding]::ASCII.GetString(
                $sectionBytes, $header, $sectionNameLength)
            $virtualSize = [BitConverter]::ToUInt32($sectionBytes, $header + 8)
            $virtualAddress = [BitConverter]::ToUInt32($sectionBytes, $header + 12)
            $rawSize = [BitConverter]::ToUInt32($sectionBytes, $header + 16)
            $rawOffset = [BitConverter]::ToUInt32($sectionBytes, $header + 20)
            if ([uint64]$rawOffset + [uint64]$rawSize -gt [uint64]$stream.Length) {
                throw "PE section raw data is outside the file: $PathValue"
            }
            $sections.Add([pscustomobject]@{
                Name = $sectionName
                VirtualAddress = [uint64]$virtualAddress
                Span = [uint64][Math]::Max($virtualSize, $rawSize)
                RawSize = [uint64]$rawSize
                RawOffset = [uint64]$rawOffset
            })
        }
        if ($candidateArm64X -and
            @($sections | Where-Object Name -CEQ '.a64xrm').Count -ne 1) {
            throw "ARM64 image is not a validated ARM64X hybrid executable: $PathValue"
        }
        $resolveRva = {
            param([uint64]$Rva)
            if ($Rva -lt $sizeOfHeaders -and $Rva -lt [uint64]$stream.Length) {
                return [long]$Rva
            }
            foreach ($section in $sections) {
                if ($Rva -ge $section.VirtualAddress -and
                    $Rva -lt $section.VirtualAddress + $section.Span) {
                    $delta = $Rva - $section.VirtualAddress
                    if ($delta -ge $section.RawSize -or
                        $section.RawOffset + $delta -ge [uint64]$stream.Length) {
                        throw "PE RVA points outside raw section data: $PathValue"
                    }
                    return [long]($section.RawOffset + $delta)
                }
            }
            throw "PE RVA cannot be resolved: $PathValue"
        }
        if ($managedPe32) {
            $cor20Offset = [long](& $resolveRva ([uint64]$clrHeaderRva))
            $cor20EndOffset = [long](& $resolveRva (
                [uint64]$clrHeaderRva + [uint64]0x47))
            if ($cor20EndOffset -ne $cor20Offset + 0x47) {
                throw "PE32 CLR runtime header crosses non-contiguous data: $PathValue"
            }
            $cor20 = [byte[]](Read-ExactStreamRange $stream $cor20Offset 0x48 `
                'PE32 CLR runtime header')
            $cor20Size = [BitConverter]::ToUInt32($cor20, 0)
            $metadataRva = [BitConverter]::ToUInt32($cor20, 8)
            $metadataSize = [BitConverter]::ToUInt32($cor20, 12)
            $cor20Flags = [BitConverter]::ToUInt32($cor20, 16)
            if ($cor20Size -lt 0x48 -or $cor20Size -gt $clrHeaderSize -or
                ($cor20Flags -band [uint32]0x00000001) -eq 0 -or
                ($cor20Flags -band [uint32]0x00000002) -ne 0 -or
                ($cor20Flags -band [uint32]0x00000010) -ne 0) {
                throw "PE32 CLR runtime header is not architecture-neutral IL-only code: $PathValue"
            }
            if ($metadataRva -eq 0 -or $metadataSize -lt 4 -or
                $metadataSize -gt 256MB) {
                throw "PE32 CLR metadata directory is invalid: $PathValue"
            }
            $metadataOffset = [long](& $resolveRva ([uint64]$metadataRva))
            $metadataEndOffset = [long](& $resolveRva (
                [uint64]$metadataRva + [uint64]3))
            if ($metadataEndOffset -ne $metadataOffset + 3) {
                throw "PE32 CLR metadata signature crosses non-contiguous data: $PathValue"
            }
            $metadataSignature = [byte[]](Read-ExactStreamRange $stream `
                $metadataOffset 4 'PE32 CLR metadata signature')
            if ($metadataSignature[0] -ne 0x42 -or
                $metadataSignature[1] -ne 0x53 -or
                $metadataSignature[2] -ne 0x4a -or
                $metadataSignature[3] -ne 0x42) {
                throw "PE32 CLR metadata signature is invalid: $PathValue"
            }
        }
        $readDllName = {
            param([uint64]$NameRva)
            $offset = [long](& $resolveRva $NameRva)
            $available = [int][Math]::Min(260L, $stream.Length - $offset)
            if ($available -le 0) {
                throw "PE import name is outside the file: $PathValue"
            }
            $nameBytes = [byte[]](Read-ExactStreamRange $stream $offset $available `
                'PE import name')
            $end = 0
            while ($end -lt $nameBytes.Length -and $nameBytes[$end] -ne 0) { ++$end }
            if ($end -eq 0 -or $end -eq $nameBytes.Length) {
                throw "PE import name is empty or unterminated: $PathValue"
            }
            $name = [Text.Encoding]::ASCII.GetString($nameBytes, 0, $end)
            if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}\.dll$') {
                throw "PE import has an unsafe DLL name '$name': $PathValue"
            }
            return $name
        }
        $imports = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::OrdinalIgnoreCase)

        if ($numberOfRvaAndSizes -gt 1 -and
            $optionalSize -ge $importDirectoryOffset + 8) {
            $importRva = [BitConverter]::ToUInt32($optional, $importDirectoryOffset)
            $importSize = [BitConverter]::ToUInt32($optional, $importDirectoryOffset + 4)
            if ($importRva -ne 0 -or $importSize -ne 0) {
                if ($importRva -eq 0 -or $importSize -lt 20 -or $importSize -gt 1MB) {
                    throw "PE import directory metadata is invalid: $PathValue"
                }
                $descriptor = [long](& $resolveRva $importRva)
                $limit = [Math]::Min(4096, [int]($importSize / 20))
                $terminated = $false
                for ($count = 0; $count -lt $limit; ++$count) {
                    $entry = [byte[]](Read-ExactStreamRange $stream $descriptor 20 `
                        'PE import descriptor')
                    $allZero = -not ($entry | Where-Object { $_ -ne 0 } | Select-Object -First 1)
                    if ($allZero) { $terminated = $true; break }
                    $nameRva = [BitConverter]::ToUInt32($entry, 12)
                    [void]$imports.Add((& $readDllName $nameRva))
                    $descriptor += 20
                }
                if (-not $terminated) {
                    throw "PE import descriptor table is unterminated or excessive: $PathValue"
                }
            }
        }

        # Delay-load imports are equally necessary on a clean machine.
        if ($numberOfRvaAndSizes -gt 13 -and
            $optionalSize -ge $delayDirectoryOffset + 8) {
            $delayRva = [BitConverter]::ToUInt32($optional, $delayDirectoryOffset)
            $delaySize = [BitConverter]::ToUInt32($optional, $delayDirectoryOffset + 4)
            if ($delayRva -ne 0 -or $delaySize -ne 0) {
                if ($delayRva -eq 0 -or $delaySize -lt 32 -or $delaySize -gt 1MB) {
                    throw "PE delay-import directory metadata is invalid: $PathValue"
                }
                $descriptor = [long](& $resolveRva $delayRva)
                $limit = [Math]::Min(4096, [int]($delaySize / 32))
                $terminated = $false
                for ($count = 0; $count -lt $limit; ++$count) {
                    $entry = [byte[]](Read-ExactStreamRange $stream $descriptor 32 `
                        'PE delay-import descriptor')
                    $allZero = -not ($entry | Where-Object { $_ -ne 0 } | Select-Object -First 1)
                    if ($allZero) { $terminated = $true; break }
                    $attributes = [BitConverter]::ToUInt32($entry, 0)
                    $nameValue = [BitConverter]::ToUInt32($entry, 4)
                    $nameRva = if (($attributes -band 1) -ne 0) {
                        [uint64]$nameValue
                    }
                    elseif ([uint64]$nameValue -ge $imageBase) {
                        [uint64]$nameValue - $imageBase
                    }
                    else { throw "PE delay-import address is invalid: $PathValue" }
                    [void]$imports.Add((& $readDllName $nameRva))
                    $descriptor += 32
                }
                if (-not $terminated) {
                    throw "PE delay-import table is unterminated or excessive: $PathValue"
                }
            }
        }
        return @($imports | Sort-Object)
    }
    finally { $stream.Dispose() }
}

function New-WindowsSystemImportSet {
    param([string]$AiBackend = '')

    $systemImports = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($name in @(
            'advapi32.dll','avrt.dll','bcrypt.dll','bcryptprimitives.dll',
            'bcp47langs.dll','bcp47mrm.dll','cabinet.dll','cfgmgr32.dll',
            'combase.dll','comctl32.dll','coremessaging.dll',
            'comdlg32.dll','coreuicomponents.dll','crypt32.dll','cryptbase.dll',
            'cryptsp.dll','d2d1.dll','d3d11.dll','d3d12.dll','dbgcore.dll',
            'dbghelp.dll','dcomp.dll','d3dcompiler_47.dll','dhcpcsvc.dll','dnsapi.dll','dsound.dll',
            'dwmapi.dll','dwrite.dll','dxcore.dll','dxgi.dll','dxva2.dll',
            'elscore.dll','fwpuclnt.dll','gdi32.dll','gdi32full.dll','hid.dll',
            'icu.dll','iertutil.dll','imm32.dll','iphlpapi.dll','kernel32.dll',
            'kernelbase.dll','mf.dll','mfplat.dll','mfreadwrite.dll','mmdevapi.dll',
            'mscms.dll','mscoree.dll','msvcrt.dll','ncrypt.dll','netapi32.dll',
            'ninput.dll','normaliz.dll','ntdll.dll','ole32.dll','oleaut32.dll',
            'pdh.dll','powrprof.dll','profapi.dll','propsys.dll','rometadata.dll',
            'rpcrt4.dll','sechost.dll','secur32.dll','setupapi.dll','shcore.dll',
            'shell32.dll','shlwapi.dll','sspicli.dll','twinapi.appcore.dll',
            'ucrtbase.dll','urlmon.dll','user32.dll','userenv.dll','usp10.dll',
            'uxtheme.dll','version.dll','wevtapi.dll','winhttp.dll','wininet.dll',
            'uiautomationcore.dll','winmm.dll','winscard.dll','windowscodecs.dll',
            'wintrust.dll','wldp.dll','ws2_32.dll','wtsapi32.dll','xinput9_1_0.dll',
            'xmllite.dll')) {
        [void]$systemImports.Add($name)
    }
    switch -CaseSensitive ($AiBackend) {
        '' { }
        'cpu' { }
        'cuda' {
            [void]$systemImports.Add('nvcuda.dll')
            [void]$systemImports.Add('nvml.dll')
        }
        'vulkan' { [void]$systemImports.Add('vulkan-1.dll') }
        default { throw "Unsupported AI backend for import closure: $AiBackend" }
    }
    return ,$systemImports
}

function Assert-AiRuntimeImportClosure {
    param(
        [Parameter(Mandatory = $true)][object[]]$RuntimeArtifacts,
        [Parameter(Mandatory = $true)][string]$Backend
    )

    $available = [Collections.Generic.Dictionary[string,string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($artifact in $RuntimeArtifacts) {
        $extension = [IO.Path]::GetExtension($artifact.RelativePath)
        if ($extension -notin @('.dll', '.exe')) { continue }
        $name = [IO.Path]::GetFileName($artifact.RelativePath)
        if ($available.ContainsKey($name)) {
            throw "AI runtime has ambiguous duplicate PE basenames: $name"
        }
        $available.Add($name, $artifact.RelativePath)
    }
    $systemImports = New-WindowsSystemImportSet $Backend
    foreach ($artifact in $RuntimeArtifacts) {
        if ([IO.Path]::GetExtension($artifact.RelativePath) -notin @('.dll', '.exe')) {
            continue
        }
        foreach ($import in Get-PeImportedDllNames $artifact.FullPath) {
            if ($available.ContainsKey($import) -or $systemImports.Contains($import) -or
                $import -cmatch '^(?:api|ext)-ms-win-[A-Za-z0-9._-]+\.dll$') {
                continue
            }
            throw "AI runtime import closure is missing '$import' required by '$($artifact.RelativePath)'. Add the exact x64 DLL to runtimeDependencies."
        }
    }
}

function Assert-FinalPayloadPeImportClosure {
    $payloadPeFiles = @(
        Get-ChildItem -LiteralPath $script:PayloadRoot -File -Recurse -Force |
            Where-Object {
                $_.Extension -in @('.dll', '.exe') -and
                -not (Get-SafeRelativePath $script:PayloadRoot $_.FullName).StartsWith(
                    'AI/Runtimes/', [StringComparison]::OrdinalIgnoreCase)
            })
    $available = [Collections.Generic.Dictionary[string,object]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $payloadPeFiles) {
        $relative = Get-SafeRelativePath $script:PayloadRoot $file.FullName
        $name = [IO.Path]::GetFileName($relative)
        if (-not $available.ContainsKey($name)) {
            $available.Add($name, [Collections.Generic.List[string]]::new())
        }
        $available[$name].Add($relative)
    }
    $systemImports = New-WindowsSystemImportSet
    foreach ($file in $payloadPeFiles) {
        $relative = Get-SafeRelativePath $script:PayloadRoot $file.FullName
        $directory = [IO.Path]::GetDirectoryName($relative).Replace('\', '/')
        foreach ($import in Get-PeImportedDllNames $file.FullName) {
            if ($systemImports.Contains($import) -or
                $import -cmatch '^(?:api|ext)-ms-win-[A-Za-z0-9._-]+\.dll$') {
                continue
            }
            $resolved = $false
            if ($available.ContainsKey($import)) {
                foreach ($candidate in $available[$import]) {
                    $candidateDirectory = [IO.Path]::GetDirectoryName(
                        [string]$candidate).Replace('\', '/')
                    if ($candidateDirectory.Length -eq 0 -or
                        $candidateDirectory.Equals(
                            $directory, [StringComparison]::OrdinalIgnoreCase)) {
                        $resolved = $true
                        break
                    }
                }
            }
            if (-not $resolved) {
                throw "Final payload import closure is missing '$import' required by '$relative'. Bundle the exact app-local dependency at payload root or beside the importing PE."
            }
        }
    }
}

function Test-FileContainsAscii {
    param([Parameter(Mandatory = $true)][string]$PathValue, [string]$Needle)
    $haystack = [IO.File]::ReadAllBytes($PathValue)
    $pattern = [Text.Encoding]::ASCII.GetBytes($Needle)
    for ($index = 0; $index -le $haystack.Length - $pattern.Length; ++$index) {
        $matched = $true
        for ($offset = 0; $offset -lt $pattern.Length; ++$offset) {
            if ($haystack[$index + $offset] -ne $pattern[$offset]) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $true }
    }
    return $false
}

function Test-FileContainsUtf16 {
    param([Parameter(Mandatory = $true)][string]$PathValue, [string]$Needle)
    $haystack = [IO.File]::ReadAllBytes($PathValue)
    $pattern = [Text.Encoding]::Unicode.GetBytes($Needle)
    for ($index = 0; $index -le $haystack.Length - $pattern.Length; ++$index) {
        $matched = $true
        for ($offset = 0; $offset -lt $pattern.Length; ++$offset) {
            if ($haystack[$index + $offset] -ne $pattern[$offset]) {
                $matched = $false
                break
            }
        }
        if ($matched) { return $true }
    }
    return $false
}

function Assert-NoPrivateBuildPaths {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$TransactionRoot,
        [Parameter(Mandatory = $true)][string]$BuildWorkspaceRoot
    )

    $tokens = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($pathValue in @(
            $Repository, $TransactionRoot, $BuildWorkspaceRoot,
            [Environment]::GetFolderPath('UserProfile'), [IO.Path]::GetTempPath())) {
        if ([string]::IsNullOrWhiteSpace($pathValue)) { continue }
        [void]$tokens.Add([IO.Path]::TrimEndingDirectorySeparator(
            (Get-NormalizedFullPath $pathValue)))
    }
    foreach ($relative in @(
            'D3D12LookDevPTwithAI.exe',
            'D3D12LookDevPTwithAI.ChatHost.dll',
            'D3D12LookDevPTwithAI.Chat.Core.dll',
            'D3D12LookDevPTwithAI.Chat.Infrastructure.dll')) {
        $pathValue = Resolve-PayloadDestination $relative
        foreach ($token in $tokens) {
            foreach ($candidate in @($token, $token.Replace('\', '/'))) {
                if ((Test-FileContainsAscii $pathValue $candidate) -or
                    (Test-FileContainsUtf16 $pathValue $candidate)) {
                    throw "Application binary exposes a private build path: $relative"
                }
            }
        }
    }
}

function Copy-NativePayload {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)]$ShaderNames,
        [Parameter(Mandatory = $true)][bool]$StrictBuildInventory
    )

    Assert-TreeHasNoReparsePoints $SourceRoot
    $symbolExtensions = @('.pdb', '.lib', '.exp', '.ilk', '.iobj', '.ipdb')
    foreach ($file in Get-ChildItem -LiteralPath $SourceRoot -File -Recurse -Force) {
        $relative = Get-SafeRelativePath $SourceRoot $file.FullName
        if ($file.Extension.ToLowerInvariant() -in $symbolExtensions) {
            if ($StrictBuildInventory) {
                throw "SkipBuild native payload contains a forbidden build artifact: $relative"
            }
            continue
        }
        if ($file.Name -ieq 'D3D12SDKLayers.dll' -or
            $relative -match '(?i)(^|/)LocalMCPChatClient' -or
            $relative -match '(?i)(^|/)(nvngx|sl\.)') {
            throw "Forbidden native payload file: $relative"
        }
        if (-not (Test-NativePayloadPathAllowed $relative $ShaderNames)) {
            throw "Native build produced a file outside the integrated allowlist: $relative"
        }
        Copy-RegisteredPayloadFile $file.FullName $relative 'native-release-x64'
    }

    foreach ($required in @(
            'D3D12LookDevPTwithAI.exe', 'D3D12/D3D12Core.dll',
            'Microsoft.ui.xaml.dll', 'Microsoft.WindowsAppRuntime.dll',
            'Microsoft.WindowsAppRuntime.Bootstrap.dll')) {
        if (-not (Test-Path -LiteralPath (Resolve-PayloadDestination $required) -PathType Leaf)) {
            throw "The self-contained native payload is missing: $required"
        }
    }
    Assert-X64Pe (Resolve-PayloadDestination 'D3D12LookDevPTwithAI.exe') `
        'D3D12LookDevPTwithAI.exe'
}

function Get-ExpectedChatHostFileNames {
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    $depsName = 'D3D12LookDevPTwithAI.ChatHost.deps.json'
    $deps = Read-StrictJsonFile (Join-Path $SourceRoot $depsName) `
        $script:MaximumDepsJsonBytes 'ChatHost deps.json'
    $runtimeTargetName = [string]$deps.runtimeTarget.name
    if ([string]::IsNullOrWhiteSpace($runtimeTargetName)) {
        throw 'ChatHost deps.json has no runtimeTarget.name.'
    }
    $targets = @($deps.targets.PSObject.Properties |
        Where-Object Name -CEQ $runtimeTargetName)
    if ($targets.Count -ne 1) {
        throw 'ChatHost deps.json does not contain exactly one selected runtime target.'
    }

    $expected = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($library in @($targets[0].Value.PSObject.Properties)) {
        foreach ($assetGroupName in @('runtime', 'native', 'resources')) {
            $assetGroupProperty = $library.Value.PSObject.Properties[$assetGroupName]
            if ($null -eq $assetGroupProperty) { continue }
            $assetGroup = $assetGroupProperty.Value
            foreach ($asset in @($assetGroup.PSObject.Properties)) {
                $fileName = [IO.Path]::GetFileName([string]$asset.Name)
                if ([string]::IsNullOrWhiteSpace($fileName) -or
                    (Test-ReservedWindowsName $fileName) -or
                    $fileName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
                    throw "ChatHost deps.json contains an invalid or colliding publish asset: $($asset.Name)"
                }
                if ($fileName -ieq 'createdump.exe') { continue }
                if (-not $expected.Add($fileName)) {
                    throw "ChatHost deps.json contains an invalid or colliding publish asset: $($asset.Name)"
                }
            }
        }
    }
    foreach ($generated in @(
            'D3D12LookDevPTwithAI.ChatHost.exe', $depsName,
            'D3D12LookDevPTwithAI.ChatHost.runtimeconfig.json')) {
        if (-not $expected.Add($generated)) {
            throw "ChatHost deps.json collides with generated publish output: $generated"
        }
    }
    return ,$expected
}

function Copy-ChatHostPayload {
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    Assert-TreeHasNoReparsePoints $SourceRoot
    $expectedNames = Get-ExpectedChatHostFileNames $SourceRoot
    $sourceFiles = @(Get-ChildItem -LiteralPath $SourceRoot -File -Recurse -Force)
    $includedFiles = @($sourceFiles | Where-Object Name -INE 'createdump.exe')
    if ($includedFiles.Count -ne $expectedNames.Count) {
        throw 'ChatHost publish inventory does not exactly match its deps.json runtime assets.'
    }
    foreach ($file in $sourceFiles) {
        $relative = Get-SafeRelativePath $SourceRoot $file.FullName
        if ($relative -ieq 'createdump.exe') { continue }
        if ($file.Extension -ieq '.pdb') {
            throw "Self-contained ChatHost publish must not contain symbols: $relative"
        }
        if ($relative -match '(?i)LocalMCPChatClient' -or
            $relative.Contains('/') -or -not $expectedNames.Contains($relative)) {
            throw "ChatHost publish is outside the exact deps.json inventory: $relative"
        }
        Copy-RegisteredPayloadFile $file.FullName $relative 'chat-host-self-contained-win-x64'
    }

    foreach ($required in @(
            'D3D12LookDevPTwithAI.ChatHost.exe',
            'D3D12LookDevPTwithAI.ChatHost.dll',
            'D3D12LookDevPTwithAI.Chat.Core.dll',
            'D3D12LookDevPTwithAI.Chat.Infrastructure.dll',
            'D3D12LookDevPTwithAI.ChatHost.deps.json',
            'D3D12LookDevPTwithAI.ChatHost.runtimeconfig.json',
            'hostfxr.dll', 'hostpolicy.dll', 'coreclr.dll',
            'System.Private.CoreLib.dll', 'e_sqlite3.dll')) {
        if (-not (Test-Path -LiteralPath (Resolve-PayloadDestination $required) -PathType Leaf)) {
            throw "The self-contained ChatHost payload is missing: $required"
        }
    }
    Assert-X64Pe (Resolve-PayloadDestination 'D3D12LookDevPTwithAI.ChatHost.exe') `
        'D3D12LookDevPTwithAI.ChatHost.exe'
    foreach ($applicationAssembly in @(
            'D3D12LookDevPTwithAI.ChatHost.exe',
            'D3D12LookDevPTwithAI.ChatHost.dll',
            'D3D12LookDevPTwithAI.Chat.Core.dll',
            'D3D12LookDevPTwithAI.Chat.Infrastructure.dll')) {
        if (Test-FileContainsAscii (Resolve-PayloadDestination $applicationAssembly) `
                'D3D12LOOKDEVPT_AI_TEST_RUNTIME') {
            throw "Release ChatHost contains the Debug-only runtime activation hook: $applicationAssembly"
        }
    }
}

function Copy-LicenseDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativeDestination,
        [Parameter(Mandatory = $true)][string]$Origin
    )

    $snapshot = Read-BoundedRegularFileSnapshot $Source `
        $script:MaximumLicenseFileBytes 'license document'
    if ([long]$snapshot.Size -gt
        $script:MaximumLicenseBytes - $script:CopiedLicenseBytes) {
        throw 'Bundled license size metadata overflows its safety bound.'
    }
    $script:CopiedLicenseBytes += [long]$snapshot.Size
    if ($script:CopiedLicenseBytes -gt $script:MaximumLicenseBytes) {
        throw 'Bundled license documents exceed the 16 MiB safety bound.'
    }
    Write-RegisteredPayloadBytes $RelativeDestination ([byte[]]$snapshot.Bytes) `
        $Origin ([long]$snapshot.Size) ([string]$snapshot.Sha256)
}

function Copy-LeadingLicenseComment {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$RelativeDestination,
        [Parameter(Mandatory = $true)][string]$Origin
    )

    $snapshot = Read-BoundedRegularFileSnapshot $Source `
        $script:MaximumLicenseFileBytes 'license comment source'
    try {
        $text = [Text.UTF8Encoding]::new($false, $true).GetString(
            [byte[]]$snapshot.Bytes)
    }
    catch { throw "License comment source is not strict UTF-8: $Source" }
    $match = [regex]::Match($text, '\A\s*/\*.*?\*/',
        [Text.RegularExpressions.RegexOptions]::Singleline)
    if (-not $match.Success -or [string]::IsNullOrWhiteSpace($match.Value)) {
        throw "A required leading license comment was not found: $Source"
    }
    $bytes = [Text.Encoding]::UTF8.GetByteCount($match.Value)
    if ($bytes -gt $script:MaximumLicenseFileBytes -or
        $bytes -gt $script:MaximumLicenseBytes - $script:CopiedLicenseBytes) {
        throw "A generated license notice exceeds the license bounds: $Source"
    }
    $script:CopiedLicenseBytes += $bytes
    Write-RegisteredPayloadText $RelativeDestination $match.Value $Origin
}

function Copy-MatchingLicenseComment {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$RelativeDestination,
        [Parameter(Mandatory = $true)][string]$Origin
    )

    $snapshot = Read-BoundedRegularFileSnapshot $Source `
        $script:MaximumLicenseFileBytes 'license comment source'
    try {
        $text = [Text.UTF8Encoding]::new($false, $true).GetString(
            [byte[]]$snapshot.Bytes)
    }
    catch { throw "License comment source is not strict UTF-8: $Source" }
    $matches = [regex]::Matches($text, $Pattern,
        [Text.RegularExpressions.RegexOptions]::Singleline)
    if ($matches.Count -ne 1 -or [string]::IsNullOrWhiteSpace($matches[0].Value)) {
        throw "A unique required license comment was not found: $Source"
    }
    $bytes = [Text.Encoding]::UTF8.GetByteCount($matches[0].Value)
    if ($bytes -gt $script:MaximumLicenseFileBytes -or
        $bytes -gt $script:MaximumLicenseBytes - $script:CopiedLicenseBytes) {
        throw "A generated license notice exceeds the license bounds: $Source"
    }
    $script:CopiedLicenseBytes += $bytes
    Write-RegisteredPayloadText $RelativeDestination $matches[0].Value $Origin
}

function Get-NuGetPackagesRoot {
    $configured = [Environment]::GetEnvironmentVariable('NUGET_PACKAGES')
    if (-not [string]::IsNullOrWhiteSpace($configured)) {
        return Get-NormalizedFullPath $configured
    }
    return Get-NormalizedFullPath (
        Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget\packages')
}

function Get-NuGetPackageContentHashClaim {
    param(
        [Parameter(Mandatory = $true)][string]$PackagesRoot,
        [Parameter(Mandatory = $true)][string]$Identifier,
        [Parameter(Mandatory = $true)][string]$Version
    )

    if ($Identifier -cnotmatch
            '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,254}[A-Za-z0-9])?$' -or
        $Identifier.Contains('..', [StringComparison]::Ordinal) -or
        $Version -cnotmatch
            '^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$') {
        throw "Unsafe NuGet package identity: $Identifier/$Version"
    }
    $packageRoot = Resolve-SafeChildDirectory $PackagesRoot `
        "$($Identifier.ToLowerInvariant())/$Version"
    $hashFiles = @(Get-ChildItem -LiteralPath $packageRoot -File -Force |
        Where-Object Name -Like '*.nupkg.sha512')
    if ($hashFiles.Count -ne 1 -or $hashFiles[0].Length -gt 256) {
        throw "NuGet package has no unique bounded content-hash claim: $Identifier/$Version"
    }
    Assert-NoReparseAncestors $hashFiles[0].FullName
    $value = (Get-Content -LiteralPath $hashFiles[0].FullName -Raw).Trim()
    if ($value -cnotmatch '^[A-Za-z0-9+/]{86}==$') {
        throw "NuGet package content-hash claim is malformed: $Identifier/$Version"
    }
    try { $decoded = [Convert]::FromBase64String($value) }
    catch { throw "NuGet package content-hash claim is not base64: $Identifier/$Version" }
    if ($decoded.Length -ne 64) {
        throw "NuGet package content-hash claim is not SHA-512: $Identifier/$Version"
    }
    return "sha512-$value"
}

function Get-ResolvedNuGetPackageRecords {
    param(
        [Parameter(Mandatory = $true)][string]$ChatPublishRoot,
        [AllowNull()][string]$NativeAssetsPath
    )

    $packagesRoot = Get-NuGetPackagesRoot
    $records = [Collections.Generic.Dictionary[string,object]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $nativeLibraries = if ([string]::IsNullOrWhiteSpace($NativeAssetsPath)) {
        @($script:NativeNuGetPackages.GetEnumerator() | ForEach-Object {
            [pscustomobject]@{
                Name = "$($_.Key)/$($_.Value)"
                Value = [pscustomobject]@{ type = 'package'; sha512 = '' }
            }
        })
    }
    else {
        $nativeAssets = Read-StrictJsonFile $NativeAssetsPath `
            $script:MaximumDepsJsonBytes 'native NuGet project.assets.json'
        if ($nativeAssets.libraries -isnot [pscustomobject] -or
            $nativeAssets.targets -isnot [pscustomobject]) {
            throw 'Native project.assets.json lacks its exact libraries/targets graph.'
        }
        @($nativeAssets.libraries.PSObject.Properties | Where-Object {
            [string]$_.Value.type -ceq 'package'
        })
    }
    $buildOnlyNativePackages = @(
        'Microsoft.Windows.SDK.BuildTools',
        'Microsoft.Windows.SDK.BuildTools.MSIX')
    foreach ($package in $nativeLibraries) {
        $identity = [string]$package.Name
        $separator = $identity.LastIndexOf('/')
        if ($separator -le 0 -or $separator -eq $identity.Length - 1 -or
            $identity.IndexOf('/') -ne $separator) {
            throw "Invalid native package identity in project.assets.json: $identity"
        }
        $identifier = $identity.Substring(0, $separator)
        $packageVersion = $identity.Substring($separator + 1)
        $contentHash = Get-NuGetPackageContentHashClaim `
            $packagesRoot $identifier $packageVersion
        $assetsHash = $null
        if (-not [string]::IsNullOrWhiteSpace([string]$package.Value.sha512)) {
            $assetsHash = [string]$package.Value.sha512
            if (-not $assetsHash.StartsWith('sha512-', [StringComparison]::Ordinal)) {
                $assetsHash = "sha512-$assetsHash"
            }
            if ($assetsHash -cnotmatch '^sha512-[A-Za-z0-9+/]{86}==$') {
                throw "Native project.assets.json content-hash claim is malformed: $identity"
            }
        }
        $graphs = [Collections.Generic.List[string]]::new()
        $graphs.Add('native')
        $records.Add($identity, [pscustomobject]@{
            id = $identifier
            version = $packageVersion
            restoreGraphContentHashClaimSha512 = $assetsHash
            packageCacheContentHashClaimSha512 = $contentHash
            contentHashClaimsMatch = $null -eq $assetsHash -or $assetsHash -ceq $contentHash
            graphs = $graphs
            disposition = if ($identifier -in $buildOnlyNativePackages) {
                'build-only-no-direct-payload-assets'
            }
            else { 'runtime-or-generated-output-contributor' }
        })
    }

    if (-not [string]::IsNullOrWhiteSpace($NativeAssetsPath)) {
        foreach ($target in @($nativeAssets.targets.PSObject.Properties)) {
            foreach ($identity in $buildOnlyNativePackages) {
                $matches = @($target.Value.PSObject.Properties | Where-Object {
                    $_.Name.StartsWith($identity + '/',
                        [StringComparison]::OrdinalIgnoreCase)
                })
                foreach ($match in $matches) {
                    foreach ($assetGroup in @('runtime', 'native', 'runtimeTargets')) {
                        $property = $match.Value.PSObject.Properties[$assetGroup]
                        if ($null -ne $property -and
                            @($property.Value.PSObject.Properties).Count -gt 0) {
                            throw "Build-only NuGet package unexpectedly contributes a direct payload asset: $($match.Name)/$assetGroup"
                        }
                    }
                }
            }
        }
    }

    $dependencies = Read-StrictJsonFile (
        Join-Path $ChatPublishRoot 'D3D12LookDevPTwithAI.ChatHost.deps.json') `
        $script:MaximumDepsJsonBytes 'ChatHost deps.json for package provenance'
    foreach ($library in @($dependencies.libraries.PSObject.Properties)) {
        if ([string]$library.Value.type -cne 'package') { continue }
        $separator = $library.Name.LastIndexOf('/')
        if ($separator -le 0 -or $separator -eq $library.Name.Length - 1 -or
            $library.Name.IndexOf('/') -ne $separator) {
            throw "Invalid ChatHost package identity in deps.json: $($library.Name)"
        }
        $identifier = $library.Name.Substring(0, $separator)
        $version = $library.Name.Substring($separator + 1)
        $contentHash = Get-NuGetPackageContentHashClaim `
            $packagesRoot $identifier $version
        $depsHash = [string]$library.Value.sha512
        if ($depsHash -cnotmatch '^sha512-[A-Za-z0-9+/]{86}==$') {
            throw "ChatHost deps.json content-hash claim is malformed: $($library.Name)"
        }
        if ($records.ContainsKey($library.Name)) {
            $records[$library.Name].graphs.Add('chat-host')
        }
        else {
            $graphs = [Collections.Generic.List[string]]::new()
            $graphs.Add('chat-host')
            $records.Add($library.Name, [pscustomobject]@{
                id = $identifier
                version = $version
                restoreGraphContentHashClaimSha512 = $depsHash
                packageCacheContentHashClaimSha512 = $contentHash
                contentHashClaimsMatch = $depsHash -ceq $contentHash
                graphs = $graphs
                disposition = 'runtime-or-generated-output-contributor'
            })
        }
    }
    return @($records.Values | Sort-Object id, version | ForEach-Object {
        [ordered]@{
            id = $_.id
            version = $_.version
            restoreGraphContentHashClaimSha512 = $_.restoreGraphContentHashClaimSha512
            packageCacheContentHashClaimSha512 = $_.packageCacheContentHashClaimSha512
            contentHashClaimsMatch = [bool]$_.contentHashClaimsMatch
            graphs = @($_.graphs | Sort-Object -Unique)
            disposition = $_.disposition
        }
    })
}

function Get-ActualBuildToolchain {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$VisualStudioRoot,
        [Parameter(Mandatory = $true)]$SuiteLock
    )

    $dotnetVersion = (& dotnet --version).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $dotnetVersion -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$') {
        throw 'Could not determine the actual dotnet SDK version.'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudioVersion = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationVersion).Trim()
    if ($LASTEXITCODE -ne 0 -or
        $visualStudioVersion -cnotmatch '^[0-9]+(?:\.[0-9]+){1,3}$') {
        throw 'Could not determine the selected Visual Studio installation version.'
    }
    $msbuildPath = Join-Path $VisualStudioRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
    Assert-NoReparseAncestors $msbuildPath
    $msbuildVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($msbuildPath).FileVersion
    Assert-SafeIdentityText $msbuildVersion 'MSBuild file version'
    $vcTools = @(Get-ChildItem -LiteralPath (
            Join-Path $VisualStudioRoot 'VC\Tools\MSVC') -Directory -Force |
        Where-Object Name -Match '^14\.[0-9]+\.[0-9]+$' |
        Sort-Object { [Version]$_.Name } -Descending | Select-Object -First 1)
    if ($vcTools.Count -ne 1) { throw 'Could not determine the selected MSVC tool version.' }

    foreach ($field in @(
            'visualStudio', 'msvcPlatformToolset', 'windowsSdk', 'dotnetSdk')) {
        if ($SuiteLock.toolchain.$field -isnot [string] -or
            [string]::IsNullOrWhiteSpace([string]$SuiteLock.toolchain.$field)) {
            throw "suite.lock.json toolchain.$field is invalid."
        }
    }
    $windowsSdk = [string]$SuiteLock.toolchain.windowsSdk
    $windowsSdkRoot = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\$windowsSdk"
    if (-not (Test-Path -LiteralPath $windowsSdkRoot -PathType Container)) {
        throw "The configured Windows SDK is not installed: $windowsSdk"
    }
    return [ordered]@{
        visualStudioInstallationVersion = $visualStudioVersion
        msbuildFileVersion = $msbuildVersion
        msvcToolsVersion = $vcTools[0].Name
        platformToolset = 'v145'
        windowsSdk = $windowsSdk
        dotnetSdk = $dotnetVersion
        suiteLockAdvisory = [ordered]@{
            declaredDotnetSdk = [string]$SuiteLock.toolchain.dotnetSdk
            dotnetSdkMatchesActual = $dotnetVersion -ceq [string]$SuiteLock.toolchain.dotnetSdk
        }
    }
}

function Get-VisualStudioInstallationRoot {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe was not found.'
    }
    $installation = (& $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath).Trim()
    if ([string]::IsNullOrWhiteSpace($installation)) {
        throw 'Visual Studio with the x64 C++ toolchain was not found.'
    }
    return Get-NormalizedFullPath $installation
}

function Copy-VisualCppRuntimePayload {
    param([Parameter(Mandatory = $true)][string]$VisualStudioRoot)

    $redistRoot = Join-Path $VisualStudioRoot 'VC\Redist\MSVC'
    Assert-NoReparseAncestors $redistRoot
    $candidates = [Collections.Generic.List[object]]::new()
    foreach ($directory in Get-ChildItem -LiteralPath $redistRoot -Directory -Force) {
        $parsedVersion = $null
        if (-not [Version]::TryParse($directory.Name, [ref]$parsedVersion)) { continue }
        $crtDirectory = Join-Path $directory.FullName 'x64\Microsoft.VC145.CRT'
        $openMpDirectory = Join-Path $directory.FullName 'x64\Microsoft.VC145.OpenMP'
        if ((Test-Path -LiteralPath $crtDirectory -PathType Container) -and
            (Test-Path -LiteralPath $openMpDirectory -PathType Container)) {
            $candidates.Add([pscustomobject]@{
                Version = $parsedVersion
                VersionText = $directory.Name
                Directory = $crtDirectory
                OpenMpDirectory = $openMpDirectory
            })
        }
    }
    $selected = @($candidates | Sort-Object Version -Descending | Select-Object -First 1)
    if ($selected.Count -ne 1) {
        throw 'A concrete x64 Microsoft.VC145.CRT redistributable directory was not found.'
    }
    Assert-TreeHasNoReparsePoints $selected[0].Directory
    $expectedNames = @(
        'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
        'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
        'vccorlib140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
        'vcruntime140_threads.dll')
    $actual = @(Get-ChildItem -LiteralPath $selected[0].Directory -File -Force)
    if ($actual.Count -ne $expectedNames.Count) {
        throw 'The Microsoft.VC145.CRT file inventory is unexpected.'
    }
    foreach ($name in $expectedNames) {
        $matches = @($actual | Where-Object Name -ceq $name)
        if ($matches.Count -ne 1) {
            throw "The Microsoft.VC145.CRT file inventory is missing: $name"
        }
        Assert-X64Pe $matches[0].FullName "Microsoft.VC145.CRT/$name"
        Copy-RegisteredPayloadFile $matches[0].FullName $name `
            "visual-cpp-runtime-x64:$($selected[0].VersionText)"
    }
    Assert-TreeHasNoReparsePoints $selected[0].OpenMpDirectory
    $openMpFiles = @(
        Get-ChildItem -LiteralPath $selected[0].OpenMpDirectory -File -Force)
    if ($openMpFiles.Count -ne 1 -or $openMpFiles[0].Name -cne 'vcomp140.dll') {
        throw 'The Microsoft.VC145.OpenMP x64 redistributable inventory is unexpected.'
    }
    Assert-X64Pe $openMpFiles[0].FullName 'Microsoft.VC145.OpenMP/vcomp140.dll'
    Copy-RegisteredPayloadFile $openMpFiles[0].FullName 'vcomp140.dll' `
        "visual-cpp-runtime-x64:$($selected[0].VersionText)"

    $licenseCandidates = @(
        Get-ChildItem -LiteralPath (Join-Path $VisualStudioRoot 'Licenses') `
            -File -Filter 'Redist.txt' -Recurse -Force)
    if ($licenseCandidates.Count -eq 0) {
        throw 'Visual Studio redistribution terms were not found.'
    }
    $preferred = @($licenseCandidates | Where-Object {
        $_.Directory.Name -eq '1033'
    } | Sort-Object FullName | Select-Object -First 1)
    $license = if ($preferred.Count -eq 1) {
        $preferred[0]
    }
    else { $licenseCandidates | Sort-Object FullName | Select-Object -First 1 }
    return [pscustomobject]@{
        Version = $selected[0].VersionText
        LicensePath = $license.FullName
    }
}

function Copy-BundledLicenses {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$ChatPublishRoot,
        [Parameter(Mandatory = $true)][object]$VisualCppRuntime,
        [AllowNull()][object]$AiDefinition,
        [Parameter(Mandatory = $true)][object[]]$ResolvedNuGetPackages
    )

    Copy-LicenseDocument (Join-Path $Repository 'LICENSE') 'LICENSE' 'application-license'
    $nativeDocuments = [ordered]@{
        'licenses/Native/ASSIMP-LICENSE.txt' = 'ThirdParty/assimp/LICENSE'
        'licenses/Native/ASSIMP-ZLIB-LICENSE.txt' = 'ThirdParty/assimp/contrib/zlib/LICENSE'
        'licenses/Native/ASSIMP-CLIPPER-LICENSE.txt' = 'ThirdParty/assimp/contrib/clipper/License.txt'
        'licenses/Native/ASSIMP-EARCUT-HPP-LICENSE.txt' = 'ThirdParty/assimp/contrib/earcut-hpp/LICENSE'
        'licenses/Native/ASSIMP-OPENDDLPARSER-LICENSE.txt' = 'ThirdParty/assimp/contrib/openddlparser/LICENSE'
        'licenses/Native/ASSIMP-POLY2TRI-LICENSE.txt' = 'ThirdParty/assimp/contrib/poly2tri/LICENSE'
        'licenses/Native/ASSIMP-PUGIXML-LICENSE.txt' = 'ThirdParty/assimp/contrib/pugixml/LICENSE.md'
        'licenses/Native/ASSIMP-RAPIDJSON-LICENSE.txt' = 'ThirdParty/assimp/contrib/rapidjson/license.txt'
        'licenses/Native/ASSIMP-UTF8CPP-LICENSE.txt' = 'ThirdParty/assimp/contrib/utf8cpp/doc/LICENSE'
        'licenses/Native/ASSIMP-ZIP-UNLICENSE.txt' = 'ThirdParty/assimp/contrib/zip/UNLICENSE'
        'licenses/Native/ASSIMP-MINIZIP-NOTICE.txt' = 'ThirdParty/assimp/contrib/unzip/MiniZip64_info.txt'
        'licenses/Native/DIRECTXTEX-LICENSE.txt' = 'ThirdParty/DirectXTex/LICENSE'
        'licenses/Native/TINYEXR-LICENSE.txt' = 'ThirdParty/tinyexr/LICENSE'
        'licenses/Native/TINYEXR-NOTICE.txt' = 'ThirdParty/tinyexr/NOTICE'
        'licenses/Native/TINYEXR-ZSTD-LICENSE.txt' = 'ThirdParty/tinyexr/deps/zstd/LICENSE'
        'licenses/Native/TINYGLTF-LICENSE.txt' = 'ThirdParty/tinygltf/LICENSE'
        'licenses/Native/BASIS-UNIVERSAL-LICENSE.txt' = 'ThirdParty/basis_universal/LICENSE'
        'licenses/Native/BASIS-UNIVERSAL-NOTICE.txt' = 'ThirdParty/basis_universal/NOTICE'
        'licenses/Native/BASIS-UNIVERSAL-ZSTD-LICENSE.txt' = 'ThirdParty/basis_universal/zstd/LICENSE'
    }
    foreach ($entry in $nativeDocuments.GetEnumerator()) {
        Copy-LicenseDocument (Join-Path $Repository $entry.Value) $entry.Key 'native-third-party-license'
    }
    Copy-LeadingLicenseComment (
        Join-Path $Repository 'ThirdParty/assimp/contrib/Open3DGC/o3dgcCommon.h') `
        'licenses/Native/ASSIMP-OPEN3DGC-LICENSE.txt' 'native-third-party-license'
    Copy-LeadingLicenseComment (
        Join-Path $Repository 'ThirdParty/assimp/code/AssetLib/M3D/m3d.h') `
        'licenses/Native/ASSIMP-M3D-LICENSE.txt' 'native-third-party-license'
    Copy-LeadingLicenseComment (
        Join-Path $Repository 'ThirdParty/assimp/code/AssetLib/MDC/MDCNormalTable.h') `
        'licenses/Native/ASSIMP-MDC-PICOMODEL-LICENSE.txt' 'native-third-party-license'
    Copy-LeadingLicenseComment (
        Join-Path $Repository 'ThirdParty/assimp/code/AssetLib/Assjson/cencode.h') `
        'licenses/Native/ASSIMP-LIBB64-PUBLIC-DOMAIN-NOTICE.txt' `
        'native-third-party-license'
    Copy-MatchingLicenseComment (
        Join-Path $Repository 'ThirdParty/assimp/contrib/stb/stb_image.h') `
        '(?s)/\*\s*-{20,}\s*This software is available under 2 licenses.*?-{20,}\s*\*/\s*\z' `
        'licenses/Native/ASSIMP-STB-IMAGE-LICENSE.txt' 'native-third-party-license'
    Copy-LeadingLicenseComment (Join-Path $Repository 'ThirdParty/tinygltf/json.hpp') `
        'licenses/Native/TINYGLTF-NLOHMANN-JSON-LICENSE.txt' 'native-third-party-license'
    Copy-LicenseDocument (
        Join-Path $Repository 'Package/Licenses/OpenJPH-BSD-2-Clause.txt') `
        'licenses/Native/TINYEXR-OPENJPH-BSD-2-CLAUSE.txt' 'native-third-party-license'
    Copy-LicenseDocument (Join-Path $Repository 'LICENSE') `
        'licenses/ChatHost/MIT.txt' 'chat-host-generic-mit-license'
    Copy-LicenseDocument (Join-Path $Repository 'ThirdParty/basis_universal/LICENSE') `
        'licenses/ChatHost/APACHE-2.0.txt' 'chat-host-generic-apache-license'
    Copy-LicenseDocument $VisualCppRuntime.LicensePath `
        'licenses/Native/VC-REDIST.txt' `
        "visual-cpp-runtime-license:$($VisualCppRuntime.Version)"

    $nugetRoot = Get-NuGetPackagesRoot
    foreach ($package in @($ResolvedNuGetPackages | Where-Object {
                @($_.graphs) -contains 'native'
            })) {
        $packageRoot = Resolve-SafeChildDirectory $nugetRoot `
            "$([string]$package.id)/$([string]$package.version)"
        $documents = @(
            Get-ChildItem -LiteralPath $packageRoot -File -Force -ErrorAction Stop |
                Where-Object Name -Match '(?i)(license|notice)')
        if ($documents.Count -eq 0) {
            if ([string]$package.disposition -ceq
                'build-only-no-direct-payload-assets') {
                continue
            }
            throw "No redistribution document was found for NuGet package $($package.id)/$($package.version)."
        }
        foreach ($document in $documents) {
            $name = "$($package.id)-$($package.version)-$($document.Name)"
            Copy-LicenseDocument $document.FullName "licenses/Native/NuGet/$name" `
                "nuget:$($package.id)/$($package.version)"
        }
    }

    $runtimeConfiguration = Read-StrictJsonFile (
        Join-Path $ChatPublishRoot 'D3D12LookDevPTwithAI.ChatHost.runtimeconfig.json') `
        $script:MaximumInferenceManifestBytes 'ChatHost runtimeconfig.json'
    $runtimeFramework = @($runtimeConfiguration.runtimeOptions.includedFrameworks |
        Where-Object name -eq 'Microsoft.NETCore.App')
    if ($runtimeFramework.Count -ne 1) {
        throw 'ChatHost runtimeconfig does not prove one self-contained Microsoft.NETCore.App version.'
    }
    $runtimeVersion = [string]$runtimeFramework[0].version
    if ($runtimeVersion -cnotmatch `
            '^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$') {
        throw 'ChatHost runtimeconfig has an unsafe .NET runtime version.'
    }
    $runtimePack = Resolve-SafeChildDirectory $nugetRoot `
        "microsoft.netcore.app.runtime.win-x64/$runtimeVersion"
    foreach ($documentName in @('LICENSE.TXT', 'THIRD-PARTY-NOTICES.TXT')) {
        Copy-LicenseDocument (Join-Path $runtimePack $documentName) `
            "licenses/ChatHost/DotNet-$runtimeVersion-$documentName" `
            "dotnet-runtime-win-x64:$runtimeVersion"
    }

    $dependencies = Read-StrictJsonFile (
        Join-Path $ChatPublishRoot 'D3D12LookDevPTwithAI.ChatHost.deps.json') `
        $script:MaximumDepsJsonBytes 'ChatHost deps.json for license inventory'
    $seenPackageIdentities = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    $packageOrdinal = 0
    foreach ($library in @($dependencies.libraries.PSObject.Properties |
            Sort-Object Name)) {
        if ([string]$library.Value.type -cne 'package') { continue }
        $packageOrdinal++
        $separator = $library.Name.LastIndexOf('/')
        if ($separator -le 0 -or $separator -eq $library.Name.Length - 1) {
            throw "Invalid ChatHost package identity in deps.json: $($library.Name)"
        }
        $identifier = $library.Name.Substring(0, $separator)
        $version = $library.Name.Substring($separator + 1)
        if ($library.Name.IndexOf('/') -ne $separator -or
            $identifier -cnotmatch `
                '^[A-Za-z0-9](?:[A-Za-z0-9._-]{0,254}[A-Za-z0-9])?$' -or
            $identifier.Contains('..', [StringComparison]::Ordinal) -or
            $version -cnotmatch `
                '^[0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$' -or
            -not $seenPackageIdentities.Add($library.Name)) {
            throw "Unsafe or duplicate ChatHost package identity: $($library.Name)"
        }
        $packageRoot = Resolve-SafeChildDirectory $nugetRoot `
            "$($identifier.ToLowerInvariant())/$version"
        $documents = @(
            Get-ChildItem -LiteralPath $packageRoot -File -Force -ErrorAction Stop |
                Where-Object Name -Match '(?i)(license|notice)')
        if ($documents.Count -eq 0) {
            $nuspec = Get-ChildItem -LiteralPath $packageRoot -File -Filter '*.nuspec' `
                -Force -ErrorAction Stop
            if (@($nuspec).Count -ne 1) {
                throw "No unique license metadata was found for ChatHost package $($library.Name)."
            }
            [xml]$nuspecXml = Get-Content -LiteralPath $nuspec.FullName -Raw
            $licenseNode = $nuspecXml.SelectSingleNode(
                "/*[local-name()='package']/*[local-name()='metadata']/*[local-name()='license']")
            $licenseExpression = if ($null -ne $licenseNode) {
                [string]$licenseNode.InnerText
            }
            else { '' }
            if ($null -eq $licenseNode -or
                [string]$licenseNode.type -cne 'expression' -or
                -not (Test-SpdxExpression $licenseExpression)) {
                throw "ChatHost package has no accepted SPDX license metadata: $($library.Name)."
            }
            $documents = @($nuspec)
        }
        $documentOrdinal = 0
        foreach ($document in @($documents | Sort-Object Name)) {
            $documentOrdinal++
            $extension = [IO.Path]::GetExtension($document.Name)
            if ([string]::IsNullOrEmpty($extension) -or
                $extension -cnotmatch '^\.[A-Za-z0-9]{1,12}$') {
                $extension = '.txt'
            }
            # Package identifiers and legal document names can legitimately
            # contain words such as "UserSecrets".  Keep those exact values in
            # origin metadata, but use a short neutral destination name so the
            # final-payload credential-name defense remains fail-closed.
            $packageDirectory = 'pkg-' + $packageOrdinal.ToString(
                'D4', [Globalization.CultureInfo]::InvariantCulture)
            $documentName = 'doc-' + $documentOrdinal.ToString(
                'D2', [Globalization.CultureInfo]::InvariantCulture) +
                $extension.ToLowerInvariant()
            Copy-LicenseDocument $document.FullName `
                "licenses/ChatHost/NuGet/$packageDirectory/$documentName" `
                "nuget:$($library.Name);document:$($document.Name)"
        }
    }

    $script:NativeLicensePaths = @(
        Get-ChildItem -LiteralPath (Resolve-PayloadDestination 'licenses/Native') `
            -File -Recurse -Force | ForEach-Object {
                Get-SafeRelativePath $script:PayloadRoot $_.FullName
            } | Sort-Object)
    $script:ChatHostLicensePaths = @(
        Get-ChildItem -LiteralPath (Resolve-PayloadDestination 'licenses/ChatHost') `
            -File -Recurse -Force | ForEach-Object {
                Get-SafeRelativePath $script:PayloadRoot $_.FullName
            } | Sort-Object)
}

function Copy-AiPayload {
    param([Parameter(Mandatory = $true)][object]$Definition)

    Write-RegisteredPayloadBytes 'AI/inference.json' `
        ([byte[]]$Definition.InferenceBytes) 'ai-inference-manifest' `
        ([long]$Definition.InferenceSize) ([string]$Definition.InferenceSha256)
    Copy-RegisteredPayloadFile $Definition.Model.FullPath `
        "AI/Models/$($Definition.Model.RelativePath)" 'ai-model' `
        ([long]$Definition.Model.Size) ([string]$Definition.Model.Sha256)
    foreach ($artifact in $Definition.RuntimeArtifacts) {
        Copy-RegisteredPayloadFile $artifact.FullPath `
            "AI/Runtimes/$($artifact.RelativePath)" 'ai-runtime' `
            ([long]$artifact.Size) ([string]$artifact.Sha256)
    }
    Write-RegisteredPayloadBytes 'licenses/AI/redistribution-manifest.json' `
        ([byte[]]$Definition.RedistributionBytes) 'ai-redistribution-manifest' `
        ([long]$Definition.RedistributionSize) `
        ([string]$Definition.RedistributionSha256)
    foreach ($kind in @('model', 'runtime')) {
        $component = $Definition.Components[$kind]
        if ([long]$component.LicenseSize -gt
            $script:MaximumLicenseBytes - $script:CopiedLicenseBytes) {
            throw 'Bundled AI license size metadata overflows its safety bound.'
        }
        $script:CopiedLicenseBytes += [long]$component.LicenseSize
        $fileName = [IO.Path]::GetFileName($component.LicenseRelativePath)
        Write-RegisteredPayloadBytes "licenses/AI/$kind-$fileName" `
            ([byte[]]$component.LicenseBytes) "ai-$kind-license" `
            ([long]$component.LicenseSize) ([string]$component.LicenseSha256)
    }
}

function Get-LicenseAssignment {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [AllowNull()][object]$AiDefinition
    )

    if ($RelativePath.StartsWith('AI/Models/', [StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{
            Component = $AiDefinition.Components.model.Name
            License = $AiDefinition.Components.model.LicenseExpression
            LicenseFiles = @('licenses/AI/model-' + [IO.Path]::GetFileName(
                $AiDefinition.Components.model.LicenseRelativePath))
        }
    }
    if ($RelativePath.StartsWith('AI/Runtimes/', [StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{
            Component = $AiDefinition.Components.runtime.Name
            License = $AiDefinition.Components.runtime.LicenseExpression
            LicenseFiles = @('licenses/AI/runtime-' + [IO.Path]::GetFileName(
                $AiDefinition.Components.runtime.LicenseRelativePath))
        }
    }
    if ($RelativePath.StartsWith('AI/', [StringComparison]::OrdinalIgnoreCase)) {
        return [pscustomobject]@{
            Component = 'AI configuration metadata'
            License = 'NOASSERTION'
            LicenseFiles = @('licenses/AI/redistribution-manifest.json')
        }
    }
    if ($RelativePath -ieq 'D3D12/D3D12Core.dll') {
        return [pscustomobject]@{
            Component = 'D3D12 Agility SDK'
            License = 'LicenseRef-Microsoft-D3D12-Agility'
            LicenseFiles = @('licenses/Native/NuGet/microsoft.direct3d.d3d12-1.619.3-LICENSE.txt')
        }
    }
    if ($script:Origins[$RelativePath] -like 'visual-cpp-runtime-x64:*') {
        return [pscustomobject]@{
            Component = 'Microsoft Visual C++ v145 x64 runtime'
            License = 'LicenseRef-Microsoft-Visual-Cpp-Runtime'
            LicenseFiles = @('licenses/Native/VC-REDIST.txt')
        }
    }
    if ($RelativePath.StartsWith('licenses/', [StringComparison]::OrdinalIgnoreCase) -or
        $RelativePath -ieq 'LICENSE') {
        return [pscustomobject]@{
            Component = 'Redistribution documents'
            License = 'NOASSERTION'
            LicenseFiles = @($RelativePath)
        }
    }
    if ($script:Origins[$RelativePath] -eq 'chat-host-self-contained-win-x64') {
        return [pscustomobject]@{
            Component = 'D3D12LookDevPTwithAI.ChatHost and .NET runtime'
            License = 'NOASSERTION'
            LicenseFiles = @('LICENSE') + $script:ChatHostLicensePaths
        }
    }
    return [pscustomobject]@{
        Component = 'D3D12LookDevPTwithAI native application and runtime'
        License = 'NOASSERTION'
        LicenseFiles = @('LICENSE', 'THIRD-PARTY-NOTICES.txt') +
            $script:NativeLicensePaths
    }
}

function New-LicenseMapAndSbom {
    param(
        [Parameter(Mandatory = $true)][string]$PackageVersion,
        [AllowNull()][object]$AiDefinition
    )

    $payloadFiles = @(
        Get-ChildItem -LiteralPath $script:PayloadRoot -File -Recurse -Force |
            Sort-Object FullName)
    $mapEntries = [Collections.Generic.List[object]]::new()
    foreach ($file in $payloadFiles) {
        $relative = Get-SafeRelativePath $script:PayloadRoot $file.FullName
        $assignment = Get-LicenseAssignment $relative $AiDefinition
        $mapEntries.Add([ordered]@{
            path = $relative
            component = $assignment.Component
            licenseExpression = $assignment.License
            licenseFiles = @($assignment.LicenseFiles)
        })
    }
    $licenseMap = [ordered]@{
        schemaVersion = 1
        excludedGeneratedPaths = @(
            'integrated-license-map.json',
            'integrated-portable-sbom.spdx.json',
            'integrated-portable-manifest.json')
        entries = $mapEntries.ToArray()
    }
    Write-RegisteredPayloadText 'integrated-license-map.json' `
        ($licenseMap | ConvertTo-Json -Depth 7) 'generated-license-map'

    $sbomFiles = [Collections.Generic.List[object]]::new()
    $rootFileSha1s = [Collections.Generic.List[string]]::new()
    $modelFileSha1s = [Collections.Generic.List[string]]::new()
    $runtimeFileSha1s = [Collections.Generic.List[string]]::new()
    $relationships = [Collections.Generic.List[object]]::new()
    $relationships.Add([ordered]@{
        spdxElementId = 'SPDXRef-DOCUMENT'
        relationshipType = 'DESCRIBES'
        relatedSpdxElement = 'SPDXRef-Package'
    })
    $allBeforeSbom = @(
        Get-ChildItem -LiteralPath $script:PayloadRoot -File -Recurse -Force |
            Sort-Object FullName)
    foreach ($file in $allBeforeSbom) {
        $relative = Get-SafeRelativePath $script:PayloadRoot $file.FullName
        $assignment = if ($relative -eq 'integrated-license-map.json') {
            [pscustomobject]@{ License = 'NOASSERTION' }
        }
        else { Get-LicenseAssignment $relative $AiDefinition }
        # SPDX identifiers must identify paths, not content: two copied license
        # documents are allowed to have identical bytes but still need distinct
        # IDs and CONTAINS relationships.
        $spdxId = 'SPDXRef-File-' + (Get-LowerSha256Text $relative).Substring(0, 24)
        $fileSha1 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA1).Hash.ToLowerInvariant()
        $containerId = 'SPDXRef-Package'
        if ($relative.StartsWith('AI/Models/', [StringComparison]::OrdinalIgnoreCase)) {
            $containerId = 'SPDXRef-AIModel'
            $modelFileSha1s.Add($fileSha1)
        }
        elseif ($relative.StartsWith('AI/Runtimes/', [StringComparison]::OrdinalIgnoreCase)) {
            $containerId = 'SPDXRef-AIRuntime'
            $runtimeFileSha1s.Add($fileSha1)
        }
        else {
            $rootFileSha1s.Add($fileSha1)
        }
        $sbomFiles.Add([ordered]@{
            fileName = './' + $relative
            SPDXID = $spdxId
            checksums = @(
                @{ algorithm = 'SHA256'; checksumValue = Get-LowerSha256 $file.FullName },
                @{ algorithm = 'SHA1'; checksumValue = $fileSha1 })
            licenseConcluded = [string]$assignment.License
            licenseInfoInFiles = @([string]$assignment.License)
            copyrightText = 'NOASSERTION'
        })
        $relationships.Add([ordered]@{
            spdxElementId = $containerId
            relationshipType = 'CONTAINS'
            relatedSpdxElement = $spdxId
        })
    }
    $verificationCode = Get-SpdxVerificationCode $rootFileSha1s.ToArray()
    $d3d12LicensePath = Resolve-PayloadDestination `
        'licenses/Native/NuGet/microsoft.direct3d.d3d12-1.619.3-LICENSE.txt'
    $vcLicensePath = Resolve-PayloadDestination 'licenses/Native/VC-REDIST.txt'
    $packages = [Collections.Generic.List[object]]::new()
    $packages.Add([ordered]@{
        name = 'D3D12LookDevPTwithAI integrated portable'
        SPDXID = 'SPDXRef-Package'
        versionInfo = $PackageVersion
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $true
        packageVerificationCode = @{
            packageVerificationCodeValue = $verificationCode
            packageVerificationCodeExcludedFiles = @(
                './integrated-portable-sbom.spdx.json',
                './integrated-portable-manifest.json')
        }
        licenseConcluded = 'NOASSERTION'
        licenseDeclared = 'NOASSERTION'
        copyrightText = 'NOASSERTION'
    })
    if ($null -ne $AiDefinition) {
        $packages.Add([ordered]@{
            name = $AiDefinition.Components.model.Name
            SPDXID = 'SPDXRef-AIModel'
            versionInfo = $AiDefinition.Components.model.Revision
            downloadLocation = $AiDefinition.Components.model.SourceUrl
            filesAnalyzed = $true
            packageVerificationCode = @{
                packageVerificationCodeValue = Get-SpdxVerificationCode $modelFileSha1s.ToArray()
            }
            checksums = @(@{ algorithm = 'SHA256'; checksumValue = $AiDefinition.Model.Sha256 })
            licenseConcluded = $AiDefinition.Components.model.LicenseExpression
            licenseDeclared = $AiDefinition.Components.model.LicenseExpression
            copyrightText = 'NOASSERTION'
        })
        $packages.Add([ordered]@{
            name = $AiDefinition.Components.runtime.Name
            SPDXID = 'SPDXRef-AIRuntime'
            versionInfo = $AiDefinition.Components.runtime.Revision
            downloadLocation = $AiDefinition.Components.runtime.SourceUrl
            filesAnalyzed = $true
            packageVerificationCode = @{
                packageVerificationCodeValue = Get-SpdxVerificationCode $runtimeFileSha1s.ToArray()
            }
            licenseConcluded = $AiDefinition.Components.runtime.LicenseExpression
            licenseDeclared = $AiDefinition.Components.runtime.LicenseExpression
            copyrightText = 'NOASSERTION'
        })
        foreach ($aiPackageId in @('SPDXRef-AIModel', 'SPDXRef-AIRuntime')) {
            $relationships.Add([ordered]@{
                spdxElementId = 'SPDXRef-Package'
                relationshipType = 'DEPENDS_ON'
                relatedSpdxElement = $aiPackageId
            })
        }
    }
    $extractedLicenses = [Collections.Generic.List[object]]::new()
    $extractedLicenses.Add([ordered]@{
        licenseId = 'LicenseRef-Microsoft-D3D12-Agility'
        name = 'Microsoft D3D12 Agility SDK redistribution terms'
        extractedText = Get-Content -LiteralPath $d3d12LicensePath -Raw
    })
    $extractedLicenses.Add([ordered]@{
        licenseId = 'LicenseRef-Microsoft-Visual-Cpp-Runtime'
        name = 'Microsoft Visual C++ Redistributable terms'
        extractedText = Get-Content -LiteralPath $vcLicensePath -Raw
    })
    if ($null -ne $AiDefinition) {
        $seenAiLicenseRefs = [Collections.Generic.HashSet[string]]::new(
            [StringComparer]::Ordinal)
        foreach ($kind in @('model', 'runtime')) {
            $component = $AiDefinition.Components[$kind]
            if ($component.LicenseExpression -cmatch '^LicenseRef-' -and
                $seenAiLicenseRefs.Add($component.LicenseExpression)) {
                $extractedLicenses.Add([ordered]@{
                    licenseId = $component.LicenseExpression
                    name = "$($component.Name) supplied redistribution terms"
                    extractedText = Get-Content -LiteralPath (
                        Resolve-PayloadDestination (
                            "licenses/AI/$kind-" + [IO.Path]::GetFileName(
                                $component.LicenseRelativePath))) -Raw
                })
            }
        }
    }
    $namespaceSeed = [Guid]::NewGuid().ToString('N')
    $sbom = [ordered]@{
        spdxVersion = 'SPDX-2.3'
        dataLicense = 'CC0-1.0'
        SPDXID = 'SPDXRef-DOCUMENT'
        name = "D3D12LookDevPTwithAI-$PackageVersion-integrated-portable"
        documentNamespace = "https://shader.jp/spdx/D3D12LookDevPTwithAI/$PackageVersion/$namespaceSeed"
        creationInfo = @{
            created = [DateTimeOffset]::UtcNow.ToString('O')
            creators = @('Tool: BuildIntegratedPortable.ps1')
        }
        packages = $packages.ToArray()
        files = $sbomFiles.ToArray()
        relationships = $relationships.ToArray()
        hasExtractedLicensingInfos = $extractedLicenses.ToArray()
    }
    Write-RegisteredPayloadText 'integrated-portable-sbom.spdx.json' `
        ($sbom | ConvertTo-Json -Depth 9) 'generated-spdx-sbom'
}

function Assert-FinalPayload {
    foreach ($file in Get-ChildItem -LiteralPath $script:PayloadRoot -File -Recurse -Force) {
        $relative = Get-SafeRelativePath $script:PayloadRoot $file.FullName
        if ((Test-SensitiveFileName $file.Name) -or
            $relative -match '(?i)(^|/)LocalMCPChatClient' -or
            $file.Extension -in @('.pdb', '.lib', '.exp', '.ilk', '.iobj', '.ipdb') -or
            $file.Name -ieq 'D3D12SDKLayers.dll') {
            throw "Forbidden final payload file: $relative"
        }
    }
    Assert-TreeHasNoReparsePoints $script:PayloadRoot
    $files = @(Get-ChildItem -LiteralPath $script:PayloadRoot -File -Recurse -Force)
    if ($files.Count -gt $script:MaximumPayloadFiles) {
        throw 'The integrated payload has too many files.'
    }
    $totalBytes = [long]0
    foreach ($file in $files) {
        if ([long]$file.Length -gt $script:MaximumPayloadBytes - $totalBytes) {
            throw 'The integrated payload size metadata overflows its safety bound.'
        }
        $totalBytes += [long]$file.Length
        if ($totalBytes -gt $script:MaximumPayloadBytes) {
            throw 'The integrated payload exceeds the 128 GiB safety bound.'
        }
    }
    if (-not $script:FinalPayloadPeClosureValidated) {
        Assert-FinalPayloadPeImportClosure
        $script:FinalPayloadPeClosureValidated = $true
    }
}

function Get-FileRecords {
    $records = [Collections.Generic.List[object]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $script:PayloadRoot -File -Recurse -Force |
            Sort-Object FullName) {
        $relative = Get-SafeRelativePath $script:PayloadRoot $file.FullName
        $records.Add([ordered]@{
            path = $relative
            size = [long]$file.Length
            sha256 = Get-LowerSha256 $file.FullName
            origin = $script:Origins[$relative]
        })
    }
    return $records.ToArray()
}

function New-VerifiedArchive {
    param(
        [Parameter(Mandatory = $true)][string]$PayloadDirectory,
        [Parameter(Mandatory = $true)][object[]]$FileRecords,
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ArchiveStagingPath
    )

    Add-Type -AssemblyName System.IO.Compression
    $expected = [Collections.Generic.Dictionary[string,object]]::new(
        [StringComparer]::Ordinal)
    foreach ($record in $FileRecords) {
        $expected.Add([string]$record.path, $record)
    }
    $manifestRecord = [pscustomobject]@{
        path = 'integrated-portable-manifest.json'
        size = [long](Get-Item -LiteralPath $ManifestPath).Length
        sha256 = Get-LowerSha256 $ManifestPath
    }
    $expected.Add($manifestRecord.path, $manifestRecord)

    $stream = [IO.File]::Open(
        $ArchiveStagingPath,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::ReadWrite,
        [IO.FileShare]::None)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $stream,
            [IO.Compression.ZipArchiveMode]::Create,
            $true,
            [Text.Encoding]::UTF8)
        try {
            foreach ($entryRecord in @($expected.Values | Sort-Object path)) {
                $source = if ($entryRecord.path -eq 'integrated-portable-manifest.json') {
                    $ManifestPath
                }
                else {
                    Join-Path $PayloadDirectory ([string]$entryRecord.path).Replace('/', '\')
                }
                $entry = $archive.CreateEntry(
                    [string]$entryRecord.path,
                    [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(
                    1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $entryStream = $entry.Open()
                $sourceStream = [IO.File]::OpenRead($source)
                try { $sourceStream.CopyTo($entryStream) }
                finally {
                    $sourceStream.Dispose()
                    $entryStream.Dispose()
                }
            }
        }
        finally { $archive.Dispose() }
    }
    finally { $stream.Dispose() }

    $verifyStream = [IO.File]::OpenRead($ArchiveStagingPath)
    try {
        $verifyArchive = [IO.Compression.ZipArchive]::new(
            $verifyStream,
            [IO.Compression.ZipArchiveMode]::Read,
            $false,
            [Text.Encoding]::UTF8)
        try {
            if ($verifyArchive.Entries.Count -ne $expected.Count) {
                throw 'Archive entry count does not match the validated manifest input.'
            }
            $seen = [Collections.Generic.HashSet[string]]::new(
                [StringComparer]::Ordinal)
            foreach ($entry in $verifyArchive.Entries) {
                if (-not $seen.Add($entry.FullName) -or
                    -not $expected.ContainsKey($entry.FullName)) {
                    throw "Unexpected or duplicate archive entry: $($entry.FullName)"
                }
                $record = $expected[$entry.FullName]
                if ([long]$entry.Length -ne [long]$record.size) {
                    throw "Archive size mismatch: $($entry.FullName)"
                }
                $entryStream = $entry.Open()
                $sha = [Security.Cryptography.SHA256]::Create()
                try {
                    $hash = [Convert]::ToHexString(
                        $sha.ComputeHash($entryStream)).ToLowerInvariant()
                }
                finally {
                    $sha.Dispose()
                    $entryStream.Dispose()
                }
                if ($hash -cne [string]$record.sha256) {
                    throw "Archive hash mismatch: $($entry.FullName)"
                }
            }
        }
        finally { $verifyArchive.Dispose() }
    }
    finally { $verifyStream.Dispose() }
}

function Get-PublishedFileIdentity {
    param([Parameter(Mandatory = $true)][string]$PathValue)

    Assert-NoReparseAncestors $PathValue
    $file = Get-Item -LiteralPath $PathValue -Force
    if ($file.PSIsContainer -or
        ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Published artifact is not a regular file: $PathValue"
    }
    if ($IsWindows) {
        $streams = @(Get-Item -LiteralPath $file.FullName -Stream * -Force)
        if ($streams.Count -ne 1 -or [string]$streams[0].Stream -cne ':$DATA') {
            throw "Published artifact has alternate data streams: $PathValue"
        }
    }
    return [pscustomobject]@{
        Size = [long]$file.Length
        Sha256 = Get-LowerSha256 $file.FullName
    }
}

function Remove-PublishedFileIfUnchanged {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][object]$ExpectedIdentity
    )

    if (-not (Test-Path -LiteralPath $PathValue -PathType Leaf)) { return }
    try {
        $actual = Get-PublishedFileIdentity $PathValue
        if ([long]$actual.Size -ne [long]$ExpectedIdentity.Size -or
            [string]$actual.Sha256 -cne [string]$ExpectedIdentity.Sha256) {
            Write-Warning "Rollback left a replaced publication artifact untouched: $PathValue"
            return
        }
        [IO.File]::Delete($PathValue)
    }
    catch {
        # Rollback must never turn a publication failure into deletion of an
        # unowned/replaced path.  Leave anything unverifiable for the operator.
        Write-Warning "Rollback left an unverifiable publication artifact untouched: $PathValue"
    }
}

function Get-GitStatusSnapshot {
    param([Parameter(Mandatory = $true)][string]$Repository)

    Assert-NoGitRepositoryRoutingEnvironment
    # Native-command stderr is intentionally not included in package metadata.
    # The exit code is captured before any other command can overwrite it so a
    # broken/redirected Git index can never masquerade as a clean worktree.
    $lines = @(& git -C $Repository status --porcelain=v1 `
        --untracked-files=all --ignore-submodules=none 2>$null)
    $gitExitCode = $LASTEXITCODE
    if ($gitExitCode -ne 0) {
        throw 'The source worktree status could not be verified.'
    }
    return $lines
}

function Get-GitHeadSnapshot {
    param([Parameter(Mandatory = $true)][string]$Repository)

    Assert-NoGitRepositoryRoutingEnvironment
    $lines = @(& git -C $Repository rev-parse HEAD 2>$null)
    $gitExitCode = $LASTEXITCODE
    if ($gitExitCode -ne 0 -or $lines.Count -ne 1) {
        throw 'The source commit could not be determined.'
    }
    $commit = ([string]$lines[0]).Trim()
    if ($commit -cnotmatch '^[0-9a-f]{40}$') {
        throw 'The source commit could not be determined.'
    }
    return $commit
}

function Assert-NoGitRepositoryRoutingEnvironment {
    $processEnvironment = [Environment]::GetEnvironmentVariables(
        [EnvironmentVariableTarget]::Process)
    foreach ($name in @(
            'GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR',
            'GIT_OBJECT_DIRECTORY', 'GIT_ALTERNATE_OBJECT_DIRECTORIES',
            'GIT_CEILING_DIRECTORIES')) {
        if ($processEnvironment.Contains($name)) {
            throw "Git repository routing environment is not permitted: $name"
        }
    }
}

function Assert-GitRepositoryIdentity {
    param([Parameter(Mandatory = $true)][string]$Repository)

    Assert-NoGitRepositoryRoutingEnvironment
    $lines = @(& git -C $Repository rev-parse --show-toplevel 2>$null)
    $gitExitCode = $LASTEXITCODE
    if ($gitExitCode -ne 0 -or $lines.Count -ne 1) {
        throw 'The source repository identity could not be verified.'
    }
    $topLevel = Get-NormalizedFullPath ([string]$lines[0])
    if (-not $topLevel.Equals(
            (Get-NormalizedFullPath $Repository),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Git resolved a different source repository than the builder root.'
    }
}

function Invoke-IntegratedPortableBuild {
    $repository = Get-NormalizedFullPath (Split-Path -Parent $PSScriptRoot)
    $output = Get-NormalizedFullPath $OutputDirectory
    Assert-PathsDoNotOverlap $repository $output 'Repository and portable output'
    $outputParent = Split-Path -Parent $output
    if ([string]::IsNullOrWhiteSpace($outputParent)) {
        throw 'OutputDirectory must have a parent directory.'
    }
    Assert-NoReparseAncestors $outputParent -AllowMissingLeaf
    [IO.Directory]::CreateDirectory($outputParent) | Out-Null
    Assert-NoReparseAncestors $outputParent

    if ($AllowDirtySource -and (-not $SkipBuild -or -not $NoArchive)) {
        throw 'AllowDirtySource is test-only and requires both SkipBuild and NoArchive.'
    }
    if ($SkipBuild -and (-not $AllowDirtySource -or -not $NoArchive)) {
        throw 'SkipBuild is test-only and requires both AllowDirtySource and NoArchive; production archives always build from clean source.'
    }
    Assert-GitRepositoryIdentity $repository
    $trackedSourceChanges = @(Get-GitStatusSnapshot $repository)
    $sourceDirty = $trackedSourceChanges.Count -ne 0
    if ($sourceDirty -and -not $AllowDirtySource) {
        throw 'Tracked source changes are present. Production portable output requires a clean repository.'
    }
    $sourceCommit = Get-GitHeadSnapshot $repository
    Invoke-TestOnlyPause 'AFTER_SOURCE_SNAPSHOT'

    $archivePath = "$output.zip"
    $archiveHashPath = "$archivePath.sha256"
    $manifestHashPath = "$output.manifest.sha256"
    foreach ($target in @($output, $manifestHashPath) +
            @($NoArchive ? @() : @($archivePath, $archiveHashPath))) {
        if (Test-Path -LiteralPath $target) {
            throw "Output already exists: $target"
        }
    }

    $hasAiArguments = -not [string]::IsNullOrWhiteSpace($AiArtifactDirectory) -or
        -not [string]::IsNullOrWhiteSpace($AiArtifactManifest) -or
        -not [string]::IsNullOrWhiteSpace($AiRedistributionManifest)
    if ($WithoutAi) {
        if ($hasAiArguments -or $AcceptArtifactLicenses -or $AcceptUnsignedArtifactTrust) {
            throw 'WithoutAi cannot be combined with AI paths or artifact acceptance switches.'
        }
    }
    else {
        if ([string]::IsNullOrWhiteSpace($AiArtifactDirectory) -or
            [string]::IsNullOrWhiteSpace($AiArtifactManifest) -or
            [string]::IsNullOrWhiteSpace($AiRedistributionManifest)) {
            throw 'Integrated exhibition packages require all three AI artifact/redistribution paths; use WithoutAi only for renderer development packs.'
        }
        if (-not $AcceptArtifactLicenses -or -not $AcceptUnsignedArtifactTrust) {
            throw 'AI packaging requires explicit AcceptArtifactLicenses and AcceptUnsignedArtifactTrust.'
        }
    }

    if ($SkipBuild) {
        if ([string]::IsNullOrWhiteSpace($NativeBuildDirectory) -or
            [string]::IsNullOrWhiteSpace($ChatHostPublishDirectory)) {
            throw 'SkipBuild requires NativeBuildDirectory and ChatHostPublishDirectory.'
        }
    }
    elseif (-not [string]::IsNullOrWhiteSpace($NativeBuildDirectory) -or
        -not [string]::IsNullOrWhiteSpace($ChatHostPublishDirectory)) {
        throw 'Prebuilt directories may only be supplied with SkipBuild.'
    }

    # Keep the same-volume sibling transaction name independent of the final
    # exhibition name.  Long output names previously pushed DirectXTex's
    # MSBuildProjectExtensionsPath beyond legacy MSBuild path handling.
    $layoutId = [Guid]::NewGuid().ToString('N')
    $transactionName = '.ldp-' + $layoutId
    $transactionRoot = Join-Path $outputParent $transactionName
    $transactionPrefix = $outputParent.TrimEnd('\') + '\.ldp-'
    if (-not $transactionRoot.StartsWith(
            $transactionPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe integrated portable staging path.'
    }
    $buildWorkspaceParent = Get-NormalizedFullPath ([IO.Path]::GetTempPath())
    Assert-NoReparseAncestors $buildWorkspaceParent
    $buildWorkspaceRoot = Join-Path $buildWorkspaceParent ('.ldpb-' + $layoutId)
    $buildWorkspacePrefix = $buildWorkspaceParent.TrimEnd('\') + '\.ldpb-'
    if (-not $buildWorkspaceRoot.StartsWith(
            $buildWorkspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Unsafe integrated portable build workspace path.'
    }
    Assert-IntegratedPortablePathBudget $repository $transactionRoot `
        $buildWorkspaceRoot `
        (@($output, $manifestHashPath) +
            @($NoArchive ? @() : @($archivePath, $archiveHashPath)))
    $payloadRoot = Join-Path $transactionRoot 'p'
    $nativeBuild = if ($SkipBuild) {
        Get-NormalizedFullPath $NativeBuildDirectory
    }
    else { Join-Path $buildWorkspaceRoot 'b\n' }
    $chatPublish = if ($SkipBuild) {
        Get-NormalizedFullPath $ChatHostPublishDirectory
    }
    else { Join-Path $buildWorkspaceRoot 'b\c' }
    $archiveStaging = Join-Path $transactionRoot 'a.zip'
    $published = $false
    $compilerEnvironmentChanged = $false
    $previousCompilerEnvironment = @{}
    $nativeAssetsPath = $null
    $buildWorkspaceOwned = $false
    $buildWorkspaceMarker = Join-Path $buildWorkspaceRoot '.owner'

    $script:PayloadRoot = Get-NormalizedFullPath $payloadRoot
    try {
        [IO.Directory]::CreateDirectory($payloadRoot) | Out-Null
        if (-not $SkipBuild) {
            if (Test-Path -LiteralPath $buildWorkspaceRoot) {
                throw "Build workspace already exists: $buildWorkspaceRoot"
            }
            [IO.Directory]::CreateDirectory($buildWorkspaceRoot) | Out-Null
            Assert-NoReparseAncestors $buildWorkspaceRoot
            $markerStream = [IO.FileStream]::new(
                $buildWorkspaceMarker, [IO.FileMode]::CreateNew,
                [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $markerBytes = [Text.Encoding]::ASCII.GetBytes($layoutId)
                $markerStream.Write($markerBytes, 0, $markerBytes.Length)
                $markerStream.Flush($true)
            }
            finally { $markerStream.Dispose() }
            $buildWorkspaceOwned = $true
        }
        Invoke-TestOnlyPause 'AFTER_STAGING_CREATED' @{
            transactionRoot = $transactionRoot
            buildWorkspaceRoot = $buildWorkspaceRoot
        }
        Assert-PathsDoNotOverlap $nativeBuild $chatPublish `
            'Native and ChatHost input'
        Assert-PathsDoNotOverlap $output $nativeBuild `
            'Portable output and Native input'
        Assert-PathsDoNotOverlap $output $chatPublish `
            'Portable output and ChatHost input'
        Assert-PathsDoNotOverlap $repository $buildWorkspaceRoot `
            'Source repository and build workspace'
        Assert-PathsDoNotOverlap $output $buildWorkspaceRoot `
            'Portable output and build workspace'
        Assert-PathsDoNotOverlap $transactionRoot $buildWorkspaceRoot `
            'Payload staging and build workspace'
        if (-not $WithoutAi) {
            $normalizedAiRoot = Get-NormalizedFullPath $AiArtifactDirectory
            Assert-PathsDoNotOverlap $normalizedAiRoot $nativeBuild `
                'AI and Native input'
            Assert-PathsDoNotOverlap $normalizedAiRoot $chatPublish `
                'AI and ChatHost input'
            Assert-PathsDoNotOverlap $normalizedAiRoot $output `
                'AI input and portable output'
            Assert-PathsDoNotOverlap $normalizedAiRoot $buildWorkspaceRoot `
                'AI input and build workspace'
        }
        if (-not $SkipBuild) {
            $nativeIntermediate = Join-Path $buildWorkspaceRoot 'b\i'
            $nugetIntermediate = Join-Path $buildWorkspaceRoot 'b\u'
            [IO.Directory]::CreateDirectory($nativeBuild) | Out-Null
            [IO.Directory]::CreateDirectory($chatPublish) | Out-Null
            $vsRoot = Get-VisualStudioInstallationRoot
            $msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\amd64\MSBuild.exe'
            if (-not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
                throw 'Visual Studio 2026 MSBuild was not found.'
            }
            foreach ($environmentName in @('CL', '_CL_', 'LINK', '_LINK_')) {
                $previousCompilerEnvironment[$environmentName] =
                    [Environment]::GetEnvironmentVariable(
                        $environmentName, [EnvironmentVariableTarget]::Process)
            }
            $pathMapOptions = "/experimental:deterministic " +
                "/pathmap:`"$repository`"=source " +
                "/pathmap:`"$transactionRoot`"=staging " +
                "/pathmap:`"$buildWorkspaceRoot`"=build"
            [Environment]::SetEnvironmentVariable(
                'CL', $pathMapOptions, [EnvironmentVariableTarget]::Process)
            foreach ($environmentName in @('_CL_', 'LINK', '_LINK_')) {
                [Environment]::SetEnvironmentVariable(
                    $environmentName, $null, [EnvironmentVariableTarget]::Process)
            }
            $compilerEnvironmentChanged = $true
            # Rebuild the two statically linked dependency trees into this
            # transaction.  Clean git status does not authenticate ignored libs
            # left under ThirdParty from a previous/local build.
            $thirdPartyRoot = Join-Path $buildWorkspaceRoot 'b'
            $assimpBuildRoot = Join-Path $thirdPartyRoot 'a'
            $assimpLibDir = Join-Path $assimpBuildRoot 'lib\Release'
            $assimpZlibDir = Join-Path $assimpBuildRoot 'contrib\zlib\Release'
            $directXTexLibDir = Join-Path $thirdPartyRoot 'x\l'
            $directXTexIntermediate = Join-Path $thirdPartyRoot 'x\o'
            & powershell.exe -NoProfile -ExecutionPolicy Bypass `
                -File (Join-Path $repository 'BuildThirdParty.ps1') `
                -AssimpRoot (Join-Path $repository 'ThirdParty\assimp') `
                -AssimpBuildRoot $assimpBuildRoot `
                -AssimpLibDir $assimpLibDir `
                -AssimpZlibDir $assimpZlibDir `
                -AssimpLibName 'assimp-lookdevpt.lib' `
                -AssimpZlibName 'zlib-lookdevpt.lib' `
                -DirectXTexProject (Join-Path $repository `
                    'ThirdParty\DirectXTex\DirectXTex\DirectXTex_Desktop_2026.vcxproj') `
                -DirectXTexLibDir $directXTexLibDir `
                -DirectXTexIntermediateDir $directXTexIntermediate `
                -MSBuildPath $msbuild -Configuration Release `
                -VisualStudioVersion '18.0'
            if ($LASTEXITCODE -ne 0) {
                throw 'Transaction-scoped Assimp/DirectXTex Release build failed.'
            }
            & $msbuild (Join-Path $repository 'D3D12LookDevPTwithAI.vcxproj') `
                /restore /t:Build /m `
                /p:Configuration=Release /p:Platform=x64 `
                /p:LookDevPtPlatformToolset=v145 `
                /p:WindowsAppSDKSelfContained=true `
                /p:UseHybridCRT=true `
                /p:LookDevPtGenerateDebugInformation=false /p:DebugSymbols=false `
                /p:EnableDLSS=false /p:EnableNRD=false /p:EnableRTXDI=false `
                "/p:AssimpBuildRoot=$assimpBuildRoot" `
                "/p:AssimpLibDir=$assimpLibDir" `
                "/p:AssimpZlibDir=$assimpZlibDir" `
                "/p:DirectXTexLibDir=$directXTexLibDir" `
                "/p:OutDir=$nativeBuild\" `
                "/p:IntDir=$nativeIntermediate\" `
                "/p:BaseIntermediateOutputPath=$nugetIntermediate\" `
                "/p:MSBuildProjectExtensionsPath=$nugetIntermediate\"
            if ($LASTEXITCODE -ne 0) { throw 'Integrated native Release x64 build failed.' }
            $nativeAssetsPath = Join-Path $nugetIntermediate 'project.assets.json'
            if (-not (Test-Path -LiteralPath $nativeAssetsPath -PathType Leaf)) {
                throw 'Native restore did not produce transaction-scoped project.assets.json.'
            }
            Restore-ProcessEnvironment $previousCompilerEnvironment
            $compilerEnvironmentChanged = $false

            $chatArtifacts = Join-Path $buildWorkspaceRoot 'b\d\a'
            $chatBuildOutput = Join-Path $buildWorkspaceRoot 'b\d\o'
            & dotnet publish (
                Join-Path $repository `
                    'Managed\D3D12LookDevPTwithAI.ChatHost\D3D12LookDevPTwithAI.ChatHost.csproj') `
                -c Release -r win-x64 --self-contained true `
                -o $chatPublish `
                --artifacts-path $chatArtifacts `
                -p:OutputPath="$chatBuildOutput\" `
                -p:DebugSymbols=false -p:DebugType=None
            if ($LASTEXITCODE -ne 0) { throw 'Self-contained ChatHost publish failed.' }
        }

        Assert-TreeHasNoReparsePoints $nativeBuild
        Assert-TreeHasNoReparsePoints $chatPublish
        $shaderNames = Get-ExpectedShaderNames (
            Join-Path $repository 'D3D12LookDevPTwithAI.vcxproj')
        Copy-NativePayload $nativeBuild $shaderNames ([bool]$SkipBuild)
        $visualStudioRoot = Get-VisualStudioInstallationRoot
        $visualCppRuntime = Copy-VisualCppRuntimePayload $visualStudioRoot
        Copy-ChatHostPayload $chatPublish
        Assert-NoPrivateBuildPaths $repository $transactionRoot $buildWorkspaceRoot

        $aiDefinition = $null
        if (-not $WithoutAi) {
            $aiDefinition = Get-AiPackageDefinition `
                (Get-NormalizedFullPath $AiArtifactDirectory) `
                (Get-NormalizedFullPath $AiArtifactManifest) `
                (Get-NormalizedFullPath $AiRedistributionManifest)
            Invoke-TestOnlyPause 'AFTER_AI_VALIDATION'
            Copy-AiPayload $aiDefinition
        }

        $lock = Read-StrictJsonFile (Join-Path $repository 'suite.lock.json') `
            $script:MaximumInferenceManifestBytes 'suite.lock.json'
        if (-not (Test-JsonIntegerValue $lock.schemaVersion) -or
            [long]$lock.schemaVersion -ne 2) {
            throw 'suite.lock.json schema v2 is required.'
        }
        $version = [string]$lock.suiteVersion
        Assert-SafeIdentityText $version 'suite version'
        $actualToolchain = Get-ActualBuildToolchain `
            $repository $visualStudioRoot $lock
        $resolvedNuGetPackages = @(Get-ResolvedNuGetPackageRecords `
            $chatPublish $nativeAssetsPath)
        Copy-BundledLicenses $repository $chatPublish $visualCppRuntime `
            $aiDefinition $resolvedNuGetPackages

        $publicationPolicyRelativePaths = @(
            Get-ChildItem -LiteralPath $script:PayloadRoot -File -Recurse -Force |
                ForEach-Object {
                    Get-SafeRelativePath $script:PayloadRoot $_.FullName
                }) + @(
            'THIRD-PARTY-NOTICES.txt',
            'REDISTRIBUTION-NOTES.txt',
            'UNSIGNED-ARTIFACTS.ja.txt',
            'integrated-license-map.json',
            'integrated-portable-sbom.spdx.json',
            'integrated-portable-manifest.json')
        $publishedPathPolicy = Get-PublishedPayloadPathPolicy `
            $publicationPolicyRelativePaths

        $noticeLines = @(
            'D3D12LookDevPTwithAI integrated portable - Third-Party Notices',
            '',
            'This one-app payload includes the native LookDev executable, its hidden',
            'self-contained ChatHost child, the .NET runtime, Windows App SDK, D3D12',
            'Agility SDK, DXC, WebView2, SQLite, and statically linked native libraries.',
            'Corresponding license and notice documents are under licenses/.',
            '',
            'LocalMCPChatClient and its two-app launcher are not included.',
            'DLSS, NVIDIA NRD, and RTXDI runtime binaries are not included.'
        )
        if ($null -ne $aiDefinition) {
            $noticeLines += @(
                '',
                "Bundled model: $($aiDefinition.Components.model.Name) $($aiDefinition.Components.model.Revision)",
                "Model source: $($aiDefinition.Components.model.SourceUrl)",
                "Model license: $($aiDefinition.Components.model.LicenseExpression)",
                "Bundled runtime: $($aiDefinition.Components.runtime.Name) $($aiDefinition.Components.runtime.Revision)",
                "Runtime source: $($aiDefinition.Components.runtime.SourceUrl)",
                "Runtime license: $($aiDefinition.Components.runtime.LicenseExpression)",
                'This milestone supports text chat and same-instance MCP Tool calls.',
                'A vision projector/mmproj is neither referenced nor packaged.',
                'The operator explicitly accepted these redistribution terms and the',
                'unsigned/manual trust boundary when creating this exhibition package.'
            )
        }
        Write-RegisteredPayloadText 'THIRD-PARTY-NOTICES.txt' `
            ($noticeLines -join "`n") 'generated-third-party-notices'

        $redistributionNotes = @'
D3D12LookDevPTwithAI integrated portable redistribution notes

- The user launches only D3D12LookDevPTwithAI.exe. ChatHost is an internal,
  hidden child process owned by the native application.
- The native Windows App SDK payload and the win-x64 .NET ChatHost are
  self-contained. No Windows App Runtime installer, Visual Studio, or separate
  .NET installation is part of the launch path.
- The builder has no AI artifact downloader. NuGet restore is not locked and may
  use the build machine's configured feeds, HTTP cache, and global package cache.
  Those build dependencies are part of the operator's unsigned/manual trust
  boundary; package feed/cache provenance is neither signed nor authenticated.
  The manifest records actual resolved NuGet identities/content-hash claims and
  the exact SHA-256 of every resulting payload file. This is inventory and
  integrity evidence, not a reproducible or content-pinned source build.
  Source status and HEAD are compared before, after, and immediately before
  publication, but the build does not use an immutable detached source snapshot;
  transient A-to-B-to-A source replacement remains inside the operator trust boundary.
  Bundled AI artifacts came only from the explicitly supplied, hash-verified
  input directory; target launch performs no download.
- Secrets, MCP credentials, approval rules, conversation history, and user
  settings are not included. Runtime history remains in writable LocalAppData;
  adjacent AI artifacts are treated as read-only inputs.
- integrated-portable-manifest.json and the archive SHA-256 provide integrity
  and build-inventory metadata. They do not authenticate origin. Distribute
  their digests over an authenticated channel and verify them before a manual
  exhibition. An officially signed artifact catalog/distribution is not yet
  implemented.
'@
        $redistributionNotes += @"

- Windows path policy: maxFullPath=$($publishedPathPolicy.maxFullPath),
  maxRelativePath=$($publishedPathPolicy.maxRelativePath), and
  maxInstallRootChars=$($publishedPathPolicy.maxInstallRootChars). Extract or
  move the payload only to an absolute root at or below that install-root limit;
  the longest packaged relative path is
  $($publishedPathPolicy.longestRelativePath).
"@
        Write-RegisteredPayloadText 'REDISTRIBUTION-NOTES.txt' `
            $redistributionNotes 'generated-redistribution-notes'
        $unsignedNotice = @'
署名なし手動展示パッケージ

このパッケージと手動指定したAI artifactには、発行元を証明する署名がありません。
manifestとZIPのSHA-256は改変検出用であり、真正性を証明しません。信頼済みの別経路で
digestを受け取り、展示前に照合してください。公式署名済み配布は未実装です。
'@
        Write-RegisteredPayloadText 'UNSIGNED-ARTIFACTS.ja.txt' `
            $unsignedNotice 'generated-unsigned-trust-notice'

        New-LicenseMapAndSbom $version $aiDefinition
        Assert-FinalPayload

        $sourceChangesAfterBuild = @(Get-GitStatusSnapshot $repository)
        if ($sourceChangesAfterBuild.Count -ne $trackedSourceChanges.Count -or
            [string]::Join("`n", $sourceChangesAfterBuild) -cne
                [string]::Join("`n", $trackedSourceChanges)) {
            throw 'The source worktree changed while the portable payload was being built.'
        }
        $sourceCommitAfterBuild = Get-GitHeadSnapshot $repository
        if ($sourceCommitAfterBuild -cne $sourceCommit) {
            throw 'The source HEAD changed while the portable payload was being built.'
        }
        $fileRecords = @(Get-FileRecords)
        $finalPublishedPathPolicy = Get-PublishedPayloadPathPolicy `
            (@($fileRecords | ForEach-Object { [string]$_.path }) +
                @('integrated-portable-manifest.json'))
        foreach ($policyField in @(
                'maxFullPath', 'maxRelativePath', 'maxInstallRootChars',
                'longestRelativePath')) {
            if ([string]$finalPublishedPathPolicy[$policyField] -cne
                [string]$publishedPathPolicy[$policyField]) {
                throw "The final payload changed the recorded publication path policy: $policyField"
            }
        }
        $aiMetadata = if ($null -eq $aiDefinition) {
            [ordered]@{
                included = $false
                reason = 'WithoutAi was explicitly selected for a developer/renderer-only pack.'
            }
        }
        else {
            $externalRuntimePrerequisites = switch -CaseSensitive (
                $aiDefinition.Backend) {
                'cpu' { @() }
                'cuda' { @('NVIDIA display driver (nvcuda.dll; nvml.dll when imported)') }
                'vulkan' { @('Vulkan loader/display driver (vulkan-1.dll)') }
            }
            [ordered]@{
                included = $true
                capabilities = @('text-chat', 'same-instance-mcp-tools')
                visionProjectorIncluded = $false
                backend = $aiDefinition.Backend
                externalRuntimePrerequisites = @($externalRuntimePrerequisites)
                modelId = $aiDefinition.ModelId
                inferenceManifestSha256 = Get-LowerSha256 (
                    Resolve-PayloadDestination 'AI/inference.json')
                redistributionManifestSha256 = Get-LowerSha256 (
                    Resolve-PayloadDestination 'licenses/AI/redistribution-manifest.json')
                artifactLicensesAccepted = $true
                unsignedArtifactTrustAccepted = $true
                model = [ordered]@{
                    name = $aiDefinition.Components.model.Name
                    revision = $aiDefinition.Components.model.Revision
                    sourceUrl = $aiDefinition.Components.model.SourceUrl
                    licenseExpression = $aiDefinition.Components.model.LicenseExpression
                    licenseFile = 'licenses/AI/model-' + [IO.Path]::GetFileName(
                        $aiDefinition.Components.model.LicenseRelativePath)
                    artifactPath = "AI/Models/$($aiDefinition.Model.RelativePath)"
                    size = $aiDefinition.Model.Size
                    sha256 = $aiDefinition.Model.Sha256
                }
                runtime = [ordered]@{
                    name = $aiDefinition.Components.runtime.Name
                    revision = $aiDefinition.Components.runtime.Revision
                    sourceUrl = $aiDefinition.Components.runtime.SourceUrl
                    licenseExpression = $aiDefinition.Components.runtime.LicenseExpression
                    licenseFile = 'licenses/AI/runtime-' + [IO.Path]::GetFileName(
                        $aiDefinition.Components.runtime.LicenseRelativePath)
                    entryPoint = "AI/Runtimes/$($aiDefinition.RuntimeArtifacts[0].RelativePath)"
                    fileCount = @($aiDefinition.RuntimeArtifacts).Count
                }
            }
        }
        $manifest = [ordered]@{
            schemaVersion = 1
            packageKind = 'integrated-one-app-portable'
            packageVersion = $version
            createdAtUtc = [DateTimeOffset]::UtcNow.ToString('O')
            platform = 'windows-11-x64'
            entryPoint = 'D3D12LookDevPTwithAI.exe'
            internalProcesses = @('D3D12LookDevPTwithAI.ChatHost.exe', 'optional llama-server.exe')
            legacyLocalMcpChatClientIncluded = $false
            source = [ordered]@{
                commit = $sourceCommit
                state = $sourceDirty ? 'dirty-test-only' : 'clean'
                skipBuild = [bool]$SkipBuild
                immutableDetachedSnapshot = $false
                headAndStatusComparedBeforeAndAfterBuild = $true
            }
            build = [ordered]@{
                configuration = 'Release'
                platformToolset = 'v145'
                actualToolchain = $actualToolchain
                nativeWindowsAppSdkSelfContained = $true
                nativeHybridCrt = $true
                nativeDebugInformation = $false
                nativeThirdPartyFreshlyRebuiltInTransaction = -not [bool]$SkipBuild
                visualCppRuntimeVersion = $visualCppRuntime.Version
                chatHostRuntimeIdentifier = 'win-x64'
                chatHostSelfContained = $true
                backends = @{ dlss = $false; nrd = $false; rtxdi = $false }
                windowsAppRuntimeInstallerRequired = $false
                reproducibleLockedRestore = $false
                allSourceDependenciesContentPinned = $false
                restore = [ordered]@{
                    mode = 'ambient-configured-feeds-and-caches-unlocked'
                    packageLockFilesEnforced = $false
                    globalJsonSdkPinEnforced = $false
                    packageFeedAndCacheProvenanceAuthenticated = $false
                    resolvedNuGetPackages = $resolvedNuGetPackages
                    payloadHashCoverage = 'Every resulting payload file is recorded by path, size, and SHA-256 in top-level files; package-to-output attribution is not claimed.'
                }
            }
            trust = [ordered]@{
                packageCodeSigned = $false
                aiArtifactsSigned = $false
                buildDependenciesSignedOrAuthenticatedByBuilder = $false
                hashesAuthenticateOrigin = $false
                manualBuildMachineFeedsAndCachesIncludedInTrustBoundary = $true
                requiredOperatorAction = 'Verify manifest/archive digests received through an authenticated out-of-band channel.'
                signedDistributionStatus = 'not-implemented'
            }
            excludes = @(
                'LocalMCPChatClient', 'MCP credentials', 'approval rules',
                'conversation history', 'user settings', 'symbols',
                'createdump and crash dumps')
            installationPathPolicy = $publishedPathPolicy
            ai = $aiMetadata
            manifestSelfExcludedFromFiles = $true
            filesContract = 'files contains every payload file except integrated-portable-manifest.json; external archive and digest sidecars are not payload files.'
            files = $fileRecords
        }
        $manifestPath = Join-Path $script:PayloadRoot 'integrated-portable-manifest.json'
        $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath `
            -Encoding utf8NoBOM
        $script:Origins.Add('integrated-portable-manifest.json', 'generated-outer-manifest')

        Assert-FinalPayload
        Assert-PublishedPayloadPathBudget $output $fileRecords

        # Prepare and verify every publication artifact while it is still under
        # the transaction root.  The payload directory is moved into place only
        # after archive construction succeeds, so an archive failure never leaves
        # an apparently usable exhibition directory behind.
        $manifestDigest = Get-LowerSha256 $manifestPath
        $manifestHashStaging = Join-Path $transactionRoot 'm.sha256'
        $manifestVerificationPath =
            "$([IO.Path]::GetFileName($output))/integrated-portable-manifest.json"
        Set-Content -LiteralPath $manifestHashStaging `
            -Value "$manifestDigest  $manifestVerificationPath" -Encoding ascii
        $manifestHashIdentity = Get-PublishedFileIdentity $manifestHashStaging

        if (-not $NoArchive) {
            New-VerifiedArchive $script:PayloadRoot $fileRecords $manifestPath `
                $archiveStaging
            $archiveDigest = Get-LowerSha256 $archiveStaging
            $archiveIdentity = Get-PublishedFileIdentity $archiveStaging
            $archiveHashStaging = Join-Path $transactionRoot 'a.sha256'
            Set-Content -LiteralPath $archiveHashStaging `
                -Value "$archiveDigest  $([IO.Path]::GetFileName($archivePath))" `
                -Encoding ascii
            $archiveHashIdentity = Get-PublishedFileIdentity $archiveHashStaging
        }

        $manifestHashPublished = $false
        $archivePublished = $false
        $archiveHashPublished = $false
        try {
            [IO.File]::Move($manifestHashStaging, $manifestHashPath)
            $manifestHashPublished = $true
            if (-not $NoArchive) {
                [IO.File]::Move($archiveStaging, $archivePath)
                $archivePublished = $true
                [IO.File]::Move($archiveHashStaging, $archiveHashPath)
                $archiveHashPublished = $true
            }
            Invoke-TestOnlyPause 'AFTER_SIDECAR_PUBLICATION'
            $sourceChangesBeforePublication = @(Get-GitStatusSnapshot $repository)
            if ($sourceChangesBeforePublication.Count -ne $trackedSourceChanges.Count -or
                [string]::Join("`n", $sourceChangesBeforePublication) -cne
                    [string]::Join("`n", $trackedSourceChanges)) {
                throw 'The source worktree changed before portable publication.'
            }
            $sourceCommitBeforePublication = Get-GitHeadSnapshot $repository
            if ($sourceCommitBeforePublication -cne $sourceCommit) {
                throw 'The source HEAD changed before portable publication.'
            }
            if (Test-Path -LiteralPath $output) {
                throw "Portable output appeared during the build and is not owned by this transaction: $output"
            }
            [IO.Directory]::Move($script:PayloadRoot, $output)
            $published = $true
        }
        catch {
            # Never delete the output path here.  A concurrent process may have
            # created it after the initial absence check, causing Directory.Move
            # to fail; that path is not owned by this transaction.
            if ($archiveHashPublished) {
                Remove-PublishedFileIfUnchanged $archiveHashPath $archiveHashIdentity
            }
            if ($archivePublished) {
                Remove-PublishedFileIfUnchanged $archivePath $archiveIdentity
            }
            if ($manifestHashPublished) {
                Remove-PublishedFileIfUnchanged $manifestHashPath $manifestHashIdentity
            }
            throw
        }

        Write-Host "Integrated portable payload: $output"
        if (-not $NoArchive) { Write-Host "Verified archive: $archivePath" }
    }
    finally {
        if ($compilerEnvironmentChanged) {
            Restore-ProcessEnvironment $previousCompilerEnvironment
            $compilerEnvironmentChanged = $false
        }
        try {
            if ($buildWorkspaceOwned -and
                (Test-Path -LiteralPath $buildWorkspaceRoot)) {
                $resolvedBuildWorkspace = Get-NormalizedFullPath $buildWorkspaceRoot
                if (-not $resolvedBuildWorkspace.StartsWith(
                        $buildWorkspacePrefix,
                        [StringComparison]::OrdinalIgnoreCase) -or
                    [IO.Path]::GetFileName($resolvedBuildWorkspace) -cnotmatch
                        '^\.ldpb-[0-9a-f]{32}$') {
                    throw "Refusing to clean unexpected build workspace: $resolvedBuildWorkspace"
                }
                Assert-TreeHasNoReparsePoints $resolvedBuildWorkspace
                $markerSnapshot = Read-BoundedRegularFileSnapshot `
                    $buildWorkspaceMarker 64 'Build workspace ownership marker'
                if ([Text.Encoding]::ASCII.GetString($markerSnapshot.Bytes) -cne
                    $layoutId) {
                    throw "Refusing to clean a build workspace with a replaced ownership marker: $resolvedBuildWorkspace"
                }
                [IO.Directory]::Delete($resolvedBuildWorkspace, $true)
                $buildWorkspaceOwned = $false
            }
        }
        finally {
            if (Test-Path -LiteralPath $transactionRoot) {
                $resolvedTransaction = Get-NormalizedFullPath $transactionRoot
                if (-not $resolvedTransaction.StartsWith(
                        $transactionPrefix, [StringComparison]::OrdinalIgnoreCase) -or
                    [IO.Path]::GetFileName($resolvedTransaction) -notlike
                        '.ldp-????????????????????????????????' -or
                    [IO.Path]::GetFileName($resolvedTransaction) -cnotmatch
                        '^\.ldp-[0-9a-f]{32}$') {
                    throw "Refusing to clean unexpected staging path: $resolvedTransaction"
                }
                [IO.Directory]::Delete($resolvedTransaction, $true)
            }
        }
        if (-not $published -and (Test-Path -LiteralPath $output)) {
            throw "The build failed after an unexpected output publication: $output"
        }
    }
}

Invoke-IntegratedPortableBuild
