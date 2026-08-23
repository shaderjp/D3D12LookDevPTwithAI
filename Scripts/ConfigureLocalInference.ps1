[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ModelPath,

    [Parameter(Mandatory = $true)]
    [string]$RuntimePath,

    [Parameter(Mandatory = $true)]
    [string]$ModelId,

    [Parameter(Mandatory = $true)]
    [string]$Backend,

    [Parameter(Mandatory = $true)]
    [int]$ContextSize,

    [Parameter(Mandatory = $true)]
    [int]$MaxTokens,

    [Parameter(Mandatory = $true)]
    [double]$Temperature,

    [switch]$AcceptArtifactLicenses,

    [string]$AiDataDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'D3D12LookDevPTwithAI\AI'),

    [switch]$ReplaceConfiguration
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ArtifactStagingPrefix = '.configure-local-inference-staging-'
$script:ConfigurationStagingPrefix = '.inference.json.staging-'
$script:RuntimeHashPrefixCharacters = 16

function Stop-Configuration {
    throw [InvalidOperationException]::new('Local inference configuration failed.')
}

function Get-NormalizedFullPath {
    param([string]$PathValue)

    if ([string]::IsNullOrWhiteSpace($PathValue)) {
        Stop-Configuration
    }

    try {
        $fullPath = [IO.Path]::GetFullPath($PathValue)
        $root = [IO.Path]::GetPathRoot($fullPath)
        if ($fullPath.Length -gt $root.Length) {
            $fullPath = $fullPath.TrimEnd([char[]]@('\', '/'))
        }
        return $fullPath
    }
    catch {
        Stop-Configuration
    }
}

function Test-SamePath {
    param(
        [string]$Left,
        [string]$Right
    )

    return [string]::Equals(
        (Get-NormalizedFullPath $Left),
        (Get-NormalizedFullPath $Right),
        [StringComparison]::OrdinalIgnoreCase)
}

function Test-StrictDescendant {
    param(
        [string]$Candidate,
        [string]$Root
    )

    $normalizedCandidate = Get-NormalizedFullPath $Candidate
    $normalizedRoot = Get-NormalizedFullPath $Root
    $prefix = $normalizedRoot
    if (-not $prefix.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $prefix += [IO.Path]::DirectorySeparatorChar
    }
    return $normalizedCandidate.StartsWith(
        $prefix,
        [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoExistingReparsePointInPath {
    param([string]$PathValue)

    $current = Get-NormalizedFullPath $PathValue
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Stop-Configuration
            }
        }

        $root = [IO.Path]::GetPathRoot($current)
        if ([string]::Equals($current, $root, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) {
            break
        }
        $current = $parent.FullName
    }
}

function Get-ValidatedSourceFile {
    param([string]$PathValue)

    $fullPath = Get-NormalizedFullPath $PathValue
    Assert-NoExistingReparsePointInPath $fullPath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        Stop-Configuration
    }
    $item = Get-Item -LiteralPath $fullPath -Force
    if ($item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Stop-Configuration
    }
    return $item
}

function Get-ValidatedSourceDirectory {
    param([string]$PathValue)

    $fullPath = Get-NormalizedFullPath $PathValue
    Assert-NoExistingReparsePointInPath $fullPath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
        Stop-Configuration
    }
    $item = Get-Item -LiteralPath $fullPath -Force
    if (-not $item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Stop-Configuration
    }
    return $item
}

function Test-SafeModelId {
    param([string]$Value)

    return -not [string]::IsNullOrWhiteSpace($Value) -and
        $Value.Length -le 64 -and
        $Value -cmatch '^[A-Za-z0-9](?:[A-Za-z0-9._-]*[A-Za-z0-9])?$'
}

function Test-ReservedWindowsName {
    param([string]$Name)

    $stem = $Name.Split('.')[0]
    if ($stem -imatch '^(CON|PRN|AUX|NUL)$') {
        return $true
    }
    return $stem -imatch '^(COM|LPT)[1-9]$'
}

function Test-SafeFileName {
    param([string]$Name)

    return -not [string]::IsNullOrWhiteSpace($Name) -and
        $Name -ceq $Name.Trim() -and
        -not $Name.EndsWith('.') -and
        $Name -notmatch '[<>:"/\\|?*]' -and
        $Name -notmatch '[\x00-\x1f]' -and
        -not (Test-ReservedWindowsName $Name)
}

function Get-ArtifactInfo {
    param([IO.FileInfo]$File)

    if ($File.Length -le 0) {
        Stop-Configuration
    }
    $hash = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    return [pscustomobject]@{
        Length = [long]$File.Length
        Sha256 = $hash
    }
}

function Get-RelativeChildPath {
    param(
        [string]$Child,
        [string]$Root
    )

    $normalizedChild = Get-NormalizedFullPath $Child
    $normalizedRoot = Get-NormalizedFullPath $Root
    if (-not (Test-StrictDescendant $normalizedChild $normalizedRoot)) {
        Stop-Configuration
    }
    $prefixLength = $normalizedRoot.Length
    if (-not $normalizedRoot.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $prefixLength++
    }
    return $normalizedChild.Substring($prefixLength)
}

function Test-SensitiveRuntimeEntry {
    param(
        [string]$RelativePath,
        [bool]$IsDirectory
    )

    $segments = $RelativePath -split '[\\/]'
    foreach ($segment in $segments) {
        $lower = $segment.ToLowerInvariant()
        if ($lower -eq '.env' -or
            $lower.StartsWith('.env.', [StringComparison]::Ordinal) -or
            $lower -eq 'inference.json' -or
            $lower.StartsWith('inference.json.', [StringComparison]::Ordinal) -or
            $lower -in @(
                '.git', '.hg', '.svn', '.ssh', '.aws', '.azure',
                '.config', '.codex', 'appdata', 'application data', 'user data',
                'userdata', 'profiles', 'sessions', 'cookies', 'cookies-journal',
                'login data', 'login data-journal', 'local state', 'web data',
                'preferences', 'secure preferences',
                'chat-history.sqlite3')) {
            return $true
        }
        if ($lower -match '(^|[._ -])(token|credentials?|secrets?|password|passwd|api[-_ ]?key|settings|configs?|configuration|history)([._ -]|$)') {
            return $true
        }
    }

    if (-not $IsDirectory) {
        $extension = [IO.Path]::GetExtension($segments[-1]).ToLowerInvariant()
        if ($extension -in @('.pem', '.key', '.pfx', '.p12', '.sqlite', '.sqlite3', '.db', '.gguf', '.log', '.dmp', '.etl', '.user', '.suo')) {
            return $true
        }
    }
    return $false
}

function Get-ValidatedRuntimeTree {
    param([string]$RootDirectory)

    $root = (Get-ValidatedSourceDirectory $RootDirectory).FullName
    if (Test-SensitiveRuntimeEntry ([IO.Path]::GetFileName($root)) $true) {
        Stop-Configuration
    }

    $entries = New-Object Collections.ArrayList
    $pending = New-Object 'Collections.Generic.Queue[string]'
    $pending.Enqueue($root)
    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Stop-Configuration
            }
            $relative = Get-RelativeChildPath $item.FullName $root
            if (Test-SensitiveRuntimeEntry $relative ([bool]$item.PSIsContainer)) {
                Stop-Configuration
            }
            foreach ($segment in @($relative -split '[\\/]')) {
                if (-not (Test-SafeFileName $segment)) {
                    Stop-Configuration
                }
            }
            if ($item.PSIsContainer) {
                [void]$entries.Add([pscustomobject]@{
                        Source = $item.FullName
                        Relative = $relative
                        IsDirectory = $true
                        Length = [long]0
                        Sha256 = ''
                    })
            }
            else {
                $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                [void]$entries.Add([pscustomobject]@{
                        Source = $item.FullName
                        Relative = $relative
                        IsDirectory = $false
                        Length = [long]$item.Length
                        Sha256 = $hash
                    })
            }
            if ($item.PSIsContainer) {
                $pending.Enqueue($item.FullName)
            }
        }
    }
    return $entries.ToArray()
}

function Assert-TreeHasNoReparsePoints {
    param([string]$RootDirectory)

    $root = (Get-ValidatedSourceDirectory $RootDirectory).FullName
    $pending = New-Object 'Collections.Generic.Queue[string]'
    $pending.Enqueue($root)
    while ($pending.Count -gt 0) {
        $directory = $pending.Dequeue()
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Stop-Configuration
            }
            if ($item.PSIsContainer) {
                $pending.Enqueue($item.FullName)
            }
        }
    }
}

function Ensure-SafeDirectory {
    param([string]$PathValue)

    $fullPath = Get-NormalizedFullPath $PathValue
    Assert-NoExistingReparsePointInPath $fullPath
    if (Test-Path -LiteralPath $fullPath) {
        if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
            Stop-Configuration
        }
    }
    else {
        [IO.Directory]::CreateDirectory($fullPath) | Out-Null
    }
    Assert-NoExistingReparsePointInPath $fullPath
}

function Test-FileMatchesArtifact {
    param(
        [string]$PathValue,
        [object]$ArtifactInfo
    )

    try {
        $file = Get-ValidatedSourceFile $PathValue
        if ([long]$file.Length -ne [long]$ArtifactInfo.Length) {
            return $false
        }
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        return $hash -ceq [string]$ArtifactInfo.Sha256
    }
    catch {
        return $false
    }
}

function Test-FileMatchesRuntimeEntry {
    param(
        [string]$PathValue,
        [object]$Entry
    )

    try {
        $file = Get-ValidatedSourceFile $PathValue
        if ([long]$file.Length -ne [long]$Entry.Length) {
            return $false
        }
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        return $hash -ceq [string]$Entry.Sha256
    }
    catch {
        return $false
    }
}

function Assert-RuntimeTreeMatches {
    param(
        [array]$ExpectedEntries,
        [string]$TargetDirectory
    )

    $expectedFiles = @($ExpectedEntries | Where-Object { -not $_.IsDirectory })
    $actualFiles = @(Get-ValidatedRuntimeTree $TargetDirectory | Where-Object { -not $_.IsDirectory })
    if ($expectedFiles.Count -ne $actualFiles.Count) {
        Stop-Configuration
    }
    foreach ($expected in $expectedFiles) {
        $matches = @($actualFiles | Where-Object { $_.Relative -ieq $expected.Relative })
        if ($matches.Count -ne 1 -or
            [long]$matches[0].Length -ne [long]$expected.Length -or
            [string]$matches[0].Sha256 -cne [string]$expected.Sha256) {
            Stop-Configuration
        }
    }
}

function Remove-SafeStagingDirectory {
    param(
        [string]$StagingDirectory,
        [string]$ArtifactRoot
    )

    if ([string]::IsNullOrWhiteSpace($StagingDirectory) -or
        -not (Test-Path -LiteralPath $StagingDirectory)) {
        return
    }
    $normalizedStaging = Get-NormalizedFullPath $StagingDirectory
    $normalizedRoot = Get-NormalizedFullPath $ArtifactRoot
    $leaf = [IO.Path]::GetFileName($normalizedStaging)
    if (-not (Test-StrictDescendant $normalizedStaging $normalizedRoot) -or
        $leaf -notmatch '^\.configure-local-inference-staging-[0-9a-f]{32}$') {
        Stop-Configuration
    }
    Assert-TreeHasNoReparsePoints $normalizedStaging
    Remove-Item -LiteralPath $normalizedStaging -Recurse -Force
}

function Remove-SafeConfigurationStagingFile {
    param(
        [string]$StagingFile,
        [string]$DataRoot
    )

    if ([string]::IsNullOrWhiteSpace($StagingFile) -or
        -not (Test-Path -LiteralPath $StagingFile)) {
        return
    }
    $normalizedStaging = Get-NormalizedFullPath $StagingFile
    $normalizedRoot = Get-NormalizedFullPath $DataRoot
    $leaf = [IO.Path]::GetFileName($normalizedStaging)
    if (-not (Test-StrictDescendant $normalizedStaging $normalizedRoot) -or
        $leaf -notmatch '^\.inference\.json\.staging-[0-9a-f]{32}\.tmp$') {
        Stop-Configuration
    }
    $item = Get-Item -LiteralPath $normalizedStaging -Force
    if ($item.PSIsContainer -or
        ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Stop-Configuration
    }
    [IO.File]::Delete($normalizedStaging)
}

function Install-ModelArtifact {
    param(
        [IO.FileInfo]$SourceFile,
        [object]$ArtifactInfo,
        [string]$ModelsRoot,
        [string]$TargetDirectory,
        [string]$TargetFile
    )

    if (Test-Path -LiteralPath $TargetDirectory) {
        if (-not (Test-Path -LiteralPath $TargetDirectory -PathType Container)) {
            Stop-Configuration
        }
        Assert-TreeHasNoReparsePoints $TargetDirectory
        if (-not (Test-FileMatchesArtifact $TargetFile $ArtifactInfo)) {
            Stop-Configuration
        }
        return
    }

    $staging = Join-Path $ModelsRoot ($script:ArtifactStagingPrefix + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.Directory]::CreateDirectory($staging) | Out-Null
        $stagedFile = Join-Path $staging $SourceFile.Name
        [IO.File]::Copy($SourceFile.FullName, $stagedFile, $false)
        if (-not (Test-FileMatchesArtifact $stagedFile $ArtifactInfo)) {
            Stop-Configuration
        }
        [IO.Directory]::Move($staging, $TargetDirectory)
        if (-not (Test-FileMatchesArtifact $TargetFile $ArtifactInfo)) {
            Stop-Configuration
        }
    }
    finally {
        Remove-SafeStagingDirectory $staging $ModelsRoot
    }
}

function Install-RuntimeArtifact {
    param(
        [array]$SourceEntries,
        [object]$ArtifactInfo,
        [string]$RuntimeBackendRoot,
        [string]$TargetDirectory,
        [string]$TargetExecutable
    )

    if (Test-Path -LiteralPath $TargetDirectory) {
        if (-not (Test-Path -LiteralPath $TargetDirectory -PathType Container)) {
            Stop-Configuration
        }
        Assert-RuntimeTreeMatches $SourceEntries $TargetDirectory
        if (-not (Test-FileMatchesArtifact $TargetExecutable $ArtifactInfo)) {
            Stop-Configuration
        }
        return
    }

    $staging = Join-Path $RuntimeBackendRoot ($script:ArtifactStagingPrefix + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.Directory]::CreateDirectory($staging) | Out-Null
        foreach ($entry in @($SourceEntries | Where-Object IsDirectory | Sort-Object { $_.Relative.Length })) {
            $destination = Get-NormalizedFullPath (Join-Path $staging $entry.Relative)
            if (-not (Test-StrictDescendant $destination $staging)) {
                Stop-Configuration
            }
            [IO.Directory]::CreateDirectory($destination) | Out-Null
        }
        foreach ($entry in @($SourceEntries | Where-Object { -not $_.IsDirectory })) {
            $sourceItem = Get-Item -LiteralPath $entry.Source -Force
            if ($sourceItem.PSIsContainer -or
                ($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Stop-Configuration
            }
            if (-not (Test-FileMatchesRuntimeEntry $sourceItem.FullName $entry)) {
                Stop-Configuration
            }
            $destination = Get-NormalizedFullPath (Join-Path $staging $entry.Relative)
            if (-not (Test-StrictDescendant $destination $staging)) {
                Stop-Configuration
            }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
            [IO.File]::Copy($sourceItem.FullName, $destination, $false)
            if (-not (Test-FileMatchesRuntimeEntry $destination $entry)) {
                Stop-Configuration
            }
        }

        $stagedExecutable = Join-Path $staging 'llama-server.exe'
        if (-not (Test-FileMatchesArtifact $stagedExecutable $ArtifactInfo)) {
            Stop-Configuration
        }
        [IO.Directory]::Move($staging, $TargetDirectory)
        Assert-RuntimeTreeMatches $SourceEntries $TargetDirectory
        if (-not (Test-FileMatchesArtifact $TargetExecutable $ArtifactInfo)) {
            Stop-Configuration
        }
    }
    finally {
        Remove-SafeStagingDirectory $staging $RuntimeBackendRoot
    }
}

function Write-ConfigurationAtomically {
    param(
        [string]$Json,
        [string]$DataRoot,
        [string]$ConfigurationPath,
        [bool]$AllowReplace
    )

    $staging = Join-Path $DataRoot ($script:ConfigurationStagingPrefix + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        $encoding = [Text.UTF8Encoding]::new($false)
        $bytes = $encoding.GetBytes($Json)
        if ($bytes.Length -gt (64 * 1024)) {
            Stop-Configuration
        }
        $stream = [IO.FileStream]::new(
            $staging,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        }
        finally {
            $stream.Dispose()
        }

        if (Test-Path -LiteralPath $ConfigurationPath) {
            if (-not $AllowReplace) {
                Stop-Configuration
            }
            Assert-NoExistingReparsePointInPath $ConfigurationPath
            if (-not (Test-Path -LiteralPath $ConfigurationPath -PathType Leaf)) {
                Stop-Configuration
            }
            $configurationItem = Get-Item -LiteralPath $ConfigurationPath -Force
            if ($configurationItem.PSIsContainer -or
                ($configurationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Stop-Configuration
            }
            $timestamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ', [Globalization.CultureInfo]::InvariantCulture)
            $backup = Join-Path $DataRoot ('inference.json.backup-' + $timestamp + '-' + [Guid]::NewGuid().ToString('N'))
            [IO.File]::Replace($staging, $ConfigurationPath, $backup, $true)
            if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
                Stop-Configuration
            }
        }
        else {
            [IO.File]::Move($staging, $ConfigurationPath)
        }
    }
    finally {
        Remove-SafeConfigurationStagingFile $staging $DataRoot
    }
}

function Invoke-LocalInferenceConfiguration {
    if (-not $AcceptArtifactLicenses) {
        Stop-Configuration
    }
    if (-not (Test-SafeModelId $ModelId) -or
        -not (Test-SafeFileName $ModelId) -or
        $Backend -notin @('cpu', 'cuda', 'vulkan') -or
        $ContextSize -lt 512 -or $ContextSize -gt 131072 -or
        $MaxTokens -lt 64 -or $MaxTokens -gt 32768 -or
        [double]::IsNaN($Temperature) -or [double]::IsInfinity($Temperature) -or
        $Temperature -lt 0 -or $Temperature -gt 2) {
        Stop-Configuration
    }

    $normalizedBackend = $Backend.ToLowerInvariant()
    $modelFile = Get-ValidatedSourceFile $ModelPath
    if (-not $modelFile.Extension.Equals('.gguf', [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-SafeFileName $modelFile.Name)) {
        Stop-Configuration
    }
    $runtimeFile = Get-ValidatedSourceFile $RuntimePath
    if (-not $runtimeFile.Name.Equals('llama-server.exe', [StringComparison]::OrdinalIgnoreCase)) {
        Stop-Configuration
    }
    $runtimeSourceDirectory = (Get-ValidatedSourceDirectory $runtimeFile.DirectoryName).FullName
    $runtimeEntries = @(Get-ValidatedRuntimeTree $runtimeSourceDirectory)
    $runtimeRootExecutableCount = @($runtimeEntries | Where-Object {
            -not $_.IsDirectory -and
            $_.Relative -ieq 'llama-server.exe'
        }).Count
    if ($runtimeRootExecutableCount -ne 1) {
        Stop-Configuration
    }
    $runtimeDependencyCount = @($runtimeEntries | Where-Object {
            -not $_.IsDirectory -and $_.Relative -ine 'llama-server.exe'
        }).Count
    if ($runtimeDependencyCount -gt 512) {
        Stop-Configuration
    }

    $modelInfo = Get-ArtifactInfo $modelFile
    $runtimeInfo = Get-ArtifactInfo $runtimeFile
    $dataRoot = Get-NormalizedFullPath $AiDataDirectory
    if (Test-SamePath $dataRoot ([IO.Path]::GetPathRoot($dataRoot))) {
        Stop-Configuration
    }
    if ((Test-SamePath $dataRoot $runtimeSourceDirectory) -or
        (Test-StrictDescendant $dataRoot $runtimeSourceDirectory)) {
        Stop-Configuration
    }
    Assert-NoExistingReparsePointInPath $dataRoot

    $configurationPath = Join-Path $dataRoot 'inference.json'
    if (Test-Path -LiteralPath $configurationPath) {
        Assert-NoExistingReparsePointInPath $configurationPath
        if (-not (Test-Path -LiteralPath $configurationPath -PathType Leaf) -or
            -not $ReplaceConfiguration) {
            Stop-Configuration
        }
        $configurationItem = Get-Item -LiteralPath $configurationPath -Force
        if ($configurationItem.PSIsContainer -or
            ($configurationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            Stop-Configuration
        }
    }

    $modelsRoot = Join-Path $dataRoot 'Models'
    $runtimesRoot = Join-Path $dataRoot 'Runtimes'
    $runtimeBackendRoot = Join-Path $runtimesRoot $normalizedBackend
    Ensure-SafeDirectory $dataRoot
    Ensure-SafeDirectory $modelsRoot
    Ensure-SafeDirectory $runtimesRoot
    Ensure-SafeDirectory $runtimeBackendRoot
    if (-not (Test-StrictDescendant $modelsRoot $dataRoot) -or
        -not (Test-StrictDescendant $runtimesRoot $dataRoot) -or
        -not (Test-StrictDescendant $runtimeBackendRoot $runtimesRoot)) {
        Stop-Configuration
    }

    $modelTargetDirectory = Join-Path $modelsRoot $ModelId
    $modelTargetFile = Join-Path $modelTargetDirectory $modelFile.Name
    $runtimeDirectoryName = 'manual-' + $runtimeInfo.Sha256.Substring(0, $script:RuntimeHashPrefixCharacters)
    $runtimeTargetDirectory = Join-Path $runtimeBackendRoot $runtimeDirectoryName
    $runtimeTargetExecutable = Join-Path $runtimeTargetDirectory 'llama-server.exe'
    if (-not (Test-StrictDescendant $modelTargetDirectory $modelsRoot) -or
        -not (Test-StrictDescendant $modelTargetFile $modelTargetDirectory) -or
        -not (Test-StrictDescendant $runtimeTargetDirectory $runtimeBackendRoot) -or
        -not (Test-StrictDescendant $runtimeTargetExecutable $runtimeTargetDirectory)) {
        Stop-Configuration
    }

    Install-ModelArtifact $modelFile $modelInfo $modelsRoot $modelTargetDirectory $modelTargetFile
    Install-RuntimeArtifact `
        $runtimeEntries `
        $runtimeInfo `
        $runtimeBackendRoot `
        $runtimeTargetDirectory `
        $runtimeTargetExecutable
    if (-not (Test-FileMatchesArtifact $modelTargetFile $modelInfo) -or
        -not (Test-FileMatchesArtifact $runtimeTargetExecutable $runtimeInfo)) {
        Stop-Configuration
    }
    Assert-RuntimeTreeMatches $runtimeEntries $runtimeTargetDirectory

    $runtimeDependenciesByPath = New-Object 'Collections.Generic.SortedDictionary[string,object]' ([StringComparer]::Ordinal)
    $runtimeDependencyPaths = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in @($runtimeEntries | Where-Object {
                -not $_.IsDirectory -and $_.Relative -ine 'llama-server.exe'
            })) {
        $forwardRelativePath = $entry.Relative.Replace('\', '/')
        if (-not $runtimeDependencyPaths.Add($forwardRelativePath)) {
            Stop-Configuration
        }
        $runtimeDependenciesByPath.Add($forwardRelativePath, $entry)
    }
    $runtimeDependencyDocuments = @(
        foreach ($dependencyPath in $runtimeDependenciesByPath.Keys) {
            $entry = $runtimeDependenciesByPath[$dependencyPath]
            $configuredDependencyPath = $normalizedBackend + '/' + $runtimeDirectoryName + '/' + $dependencyPath
            if ($configuredDependencyPath.Length -gt 1024) {
                Stop-Configuration
            }
            [ordered]@{
                relativePath = $configuredDependencyPath
                sha256 = $entry.Sha256
                expectedSize = $entry.Length
            }
        })

    $document = [ordered]@{
        schemaVersion = 1
        modelId = $ModelId
        backend = $normalizedBackend
        contextSize = $ContextSize
        maxTokens = $MaxTokens
        temperature = $Temperature
        model = [ordered]@{
            relativePath = ($ModelId + '/' + $modelFile.Name)
            sha256 = $modelInfo.Sha256
            expectedSize = $modelInfo.Length
        }
        runtime = [ordered]@{
            relativePath = ($normalizedBackend + '/' + $runtimeDirectoryName + '/llama-server.exe')
            sha256 = $runtimeInfo.Sha256
            expectedSize = $runtimeInfo.Length
        }
        runtimeDependencies = $runtimeDependencyDocuments
    }
    $json = $document | ConvertTo-Json -Depth 4 -Compress
    Write-ConfigurationAtomically $json $dataRoot $configurationPath ([bool]$ReplaceConfiguration)
}

try {
    Invoke-LocalInferenceConfiguration
    Write-Output 'Local inference configuration completed successfully.'
}
catch {
    throw 'Local inference configuration failed.'
}
