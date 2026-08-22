[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$configureScript = Join-Path $PSScriptRoot 'ConfigureLocalInference.ps1'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([char[]]@('\', '/'))
$testRoot = Join-Path $temporaryRoot ('D3D12LookDevPTwithAI-ConfigureLocalInferenceTests-' + [Guid]::NewGuid().ToString('N'))
$createdJunctions = New-Object Collections.ArrayList
$reparseTestSkipped = $false

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-NormalizedTestPath {
    param([string]$PathValue)

    $fullPath = [IO.Path]::GetFullPath($PathValue)
    $root = [IO.Path]::GetPathRoot($fullPath)
    if ($fullPath.Length -gt $root.Length) {
        return $fullPath.TrimEnd([char[]]@('\', '/'))
    }
    return $fullPath
}

function Test-TestPathIsStrictDescendant {
    param(
        [string]$Candidate,
        [string]$Root
    )

    $normalizedCandidate = Get-NormalizedTestPath $Candidate
    $normalizedRoot = Get-NormalizedTestPath $Root
    $prefix = $normalizedRoot + [IO.Path]::DirectorySeparatorChar
    return $normalizedCandidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Write-TestFile {
    param(
        [string]$PathValue,
        [string]$Content
    )

    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($PathValue)) | Out-Null
    [IO.File]::WriteAllText($PathValue, $Content, [Text.UTF8Encoding]::new($false))
}

function New-TestFixture {
    param(
        [string]$Name,
        [string]$ModelFileName = 'tiny-model.gguf',
        [string]$RuntimeFileName = 'llama-server.exe'
    )

    $fixtureRoot = Join-Path $testRoot $Name
    $modelDirectory = Join-Path $fixtureRoot 'source-model'
    $runtimeDirectory = Join-Path $fixtureRoot 'source-runtime'
    [IO.Directory]::CreateDirectory($modelDirectory) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $runtimeDirectory 'lib')) | Out-Null
    $modelPath = Join-Path $modelDirectory $ModelFileName
    $runtimePath = Join-Path $runtimeDirectory $RuntimeFileName
    Write-TestFile $modelPath ('GGUF fixture ' + $Name)
    Write-TestFile $runtimePath ('runtime fixture ' + $Name)
    Write-TestFile (Join-Path $runtimeDirectory 'lib\runtime-support.dll') ('support fixture ' + $Name)
    Write-TestFile (Join-Path $runtimeDirectory 'LICENSE') 'fixture license'
    Write-TestFile (Join-Path $runtimeDirectory 'empty.dat') ''
    return [pscustomobject]@{
        Root = $fixtureRoot
        ModelPath = $modelPath
        RuntimePath = $runtimePath
        RuntimeDirectory = $runtimeDirectory
        DataDirectory = Join-Path $fixtureRoot 'ai-data'
    }
}

function New-ConfigureParameters {
    param(
        [object]$Fixture,
        [string]$ModelId = 'tiny-model',
        [string]$Backend = 'cpu'
    )

    return @{
        ModelPath = $Fixture.ModelPath
        RuntimePath = $Fixture.RuntimePath
        ModelId = $ModelId
        Backend = $Backend
        ContextSize = 4096
        MaxTokens = 512
        Temperature = 0.25
        AcceptArtifactLicenses = $true
        AiDataDirectory = $Fixture.DataDirectory
    }
}

function Copy-ParameterMap {
    param([hashtable]$Parameters)

    $copy = @{}
    foreach ($key in $Parameters.Keys) {
        $copy[$key] = $Parameters[$key]
    }
    return $copy
}

function Invoke-ExpectFailure {
    param(
        [hashtable]$Parameters,
        [string]$Label
    )

    $failed = $false
    try {
        & $configureScript @Parameters | Out-Null
    }
    catch {
        $failed = $true
        Assert-True ($_.Exception.Message -notmatch [regex]::Escape($testRoot)) "$Label exposed a raw path."
    }
    Assert-True $failed "$Label unexpectedly succeeded."
}

function Assert-NoStagingResidue {
    param([string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return
    }
    $residue = @(Get-ChildItem -LiteralPath $Root -Force -Recurse | Where-Object {
            $_.Name -like '.configure-local-inference-staging-*' -or
            $_.Name -like '.inference.json.staging-*.tmp'
        })
    Assert-True ($residue.Count -eq 0) 'A partial staging artifact remained after configuration.'
}

function Assert-ProjectFilterSynchronization {
    $projectPath = Join-Path $repositoryRoot 'D3D12LookDevPTwithAI.vcxproj'
    $filtersPath = Join-Path $repositoryRoot 'D3D12LookDevPTwithAI.vcxproj.filters'
    [xml]$project = Get-Content -LiteralPath $projectPath -Raw
    [xml]$filters = Get-Content -LiteralPath $filtersPath -Raw
    $namespace = 'http://schemas.microsoft.com/developer/msbuild/2003'
    $projectManager = New-Object Xml.XmlNamespaceManager($project.NameTable)
    $filtersManager = New-Object Xml.XmlNamespaceManager($filters.NameTable)
    $projectManager.AddNamespace('m', $namespace)
    $filtersManager.AddNamespace('m', $namespace)
    $itemTypes = @('ClCompile', 'ClInclude', 'ApplicationDefinition', 'Page', 'Midl', 'None', 'Manifest')

    $projectItems = New-Object Collections.ArrayList
    $filterItems = New-Object Collections.ArrayList
    foreach ($itemType in $itemTypes) {
        foreach ($node in @($project.SelectNodes("//m:ItemGroup/m:$itemType[@Include]", $projectManager))) {
            [void]$projectItems.Add(($itemType + '|' + $node.Include))
        }
        foreach ($node in @($filters.SelectNodes("//m:ItemGroup/m:$itemType[@Include]", $filtersManager))) {
            [void]$filterItems.Add(($itemType + '|' + $node.Include))
            Assert-True (-not [string]::IsNullOrWhiteSpace([string]$node.Filter)) 'A filterable project item has no filter.'
        }
    }

    $duplicateProjectItems = @($projectItems | Group-Object | Where-Object Count -ne 1)
    $duplicateFilterItems = @($filterItems | Group-Object | Where-Object Count -ne 1)
    Assert-True ($duplicateProjectItems.Count -eq 0) 'The project contains a duplicate filterable item.'
    Assert-True ($duplicateFilterItems.Count -eq 0) 'The filters file contains a duplicate item mapping.'
    $projectSet = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $filterSet = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($item in $projectItems) {
        [void]$projectSet.Add($item)
    }
    foreach ($item in $filterItems) {
        [void]$filterSet.Add($item)
    }
    Assert-True ($projectSet.SetEquals($filterSet)) 'The project and filters item mappings are not synchronized.'

    $declaredFilters = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($node in @($filters.SelectNodes('//m:Filter[@Include]', $filtersManager))) {
        Assert-True ($declaredFilters.Add([string]$node.Include)) 'A logical filter is declared more than once.'
    }
    foreach ($itemType in $itemTypes) {
        foreach ($node in @($filters.SelectNodes("//m:ItemGroup/m:$itemType[@Include]", $filtersManager))) {
            Assert-True ($declaredFilters.Contains([string]$node.Filter)) 'A project item references an undeclared filter.'
        }
    }

    foreach ($scriptName in @('Scripts\ConfigureLocalInference.ps1', 'Scripts\TestConfigureLocalInference.ps1')) {
        $projectMatch = @($project.SelectNodes('//m:ItemGroup/m:None[@Include]', $projectManager) | Where-Object Include -eq $scriptName)
        $filterMatch = @($filters.SelectNodes('//m:ItemGroup/m:None[@Include]', $filtersManager) | Where-Object Include -eq $scriptName)
        Assert-True ($projectMatch.Count -eq 1) 'A local inference script is not registered exactly once in the project.'
        Assert-True ($filterMatch.Count -eq 1) 'A local inference script is not mapped exactly once in the filters file.'
        Assert-True ([string]$filterMatch[0].Filter -ceq 'Build Scripts') 'A local inference script is mapped to the wrong filter.'
    }
}

Assert-True (Test-Path -LiteralPath $configureScript -PathType Leaf) 'The configuration script is missing.'
Assert-True (Test-TestPathIsStrictDescendant $testRoot $temporaryRoot) 'The test directory is outside the temporary root.'
Assert-True ([IO.Path]::GetFileName($testRoot) -match '^D3D12LookDevPTwithAI-ConfigureLocalInferenceTests-[0-9a-f]{32}$') 'The test directory name is unsafe.'
[IO.Directory]::CreateDirectory($testRoot) | Out-Null

try {
    $licenseFixture = New-TestFixture 'license-gate'
    $licenseParameters = New-ConfigureParameters $licenseFixture
    $licenseParameters.Remove('AcceptArtifactLicenses')
    Invoke-ExpectFailure $licenseParameters 'The artifact license gate'
    Assert-True (-not (Test-Path -LiteralPath $licenseFixture.DataDirectory)) 'The license gate wrote application data.'

    $earlyRefusalFixture = New-TestFixture 'existing-config-refusal'
    [IO.Directory]::CreateDirectory($earlyRefusalFixture.DataDirectory) | Out-Null
    Write-TestFile (Join-Path $earlyRefusalFixture.DataDirectory 'inference.json') '{"preserve":true}'
    Invoke-ExpectFailure (New-ConfigureParameters $earlyRefusalFixture 'early-refusal-model') 'An existing configuration without replacement permission'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $earlyRefusalFixture.DataDirectory 'Models'))) 'A refused configuration created the model artifact root.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $earlyRefusalFixture.DataDirectory 'Runtimes'))) 'A refused configuration created the runtime artifact root.'

    $validFixture = New-TestFixture 'valid'
    $validParameters = New-ConfigureParameters $validFixture
    $successOutput = @(& $configureScript @validParameters)
    Assert-True ($successOutput.Count -eq 1) 'Configuration emitted unexpected output.'
    Assert-True ($successOutput[0] -ceq 'Local inference configuration completed successfully.') 'Configuration did not emit the generic success message.'
    Assert-True ($successOutput[0] -notmatch [regex]::Escape($testRoot)) 'Configuration exposed a raw path.'

    $configurationPath = Join-Path $validFixture.DataDirectory 'inference.json'
    Assert-True (Test-Path -LiteralPath $configurationPath -PathType Leaf) 'The inference settings file was not created.'
    $configurationBytes = [IO.File]::ReadAllBytes($configurationPath)
    Assert-True ($configurationBytes.Length -gt 0) 'The inference settings file is empty.'
    Assert-True (-not ($configurationBytes.Length -ge 3 -and
            $configurationBytes[0] -eq 0xef -and
            $configurationBytes[1] -eq 0xbb -and
            $configurationBytes[2] -eq 0xbf)) 'The inference settings file contains a UTF-8 BOM.'
    $configuration = ([Text.UTF8Encoding]::new($false)).GetString($configurationBytes) | ConvertFrom-Json
    Assert-True ([string]::Join(',', @($configuration.PSObject.Properties.Name)) -ceq 'schemaVersion,modelId,backend,contextSize,maxTokens,temperature,model,runtime,runtimeDependencies') 'The inference settings use the wrong top-level schema.'
    Assert-True ([string]::Join(',', @($configuration.model.PSObject.Properties.Name)) -ceq 'relativePath,sha256,expectedSize') 'The model settings use the wrong schema.'
    Assert-True ([string]::Join(',', @($configuration.runtime.PSObject.Properties.Name)) -ceq 'relativePath,sha256,expectedSize') 'The runtime settings use the wrong schema.'
    Assert-True ($configuration.schemaVersion -eq 1) 'The schema version is incorrect.'
    Assert-True ($configuration.modelId -ceq 'tiny-model') 'The model ID is incorrect.'
    Assert-True ($configuration.backend -ceq 'cpu') 'The backend is incorrect.'
    Assert-True ($configuration.contextSize -eq 4096 -and $configuration.maxTokens -eq 512) 'The inference limits are incorrect.'
    Assert-True ([double]$configuration.temperature -eq 0.25) 'The temperature is incorrect.'
    Assert-True ($configuration.model.relativePath -ceq 'tiny-model/tiny-model.gguf') 'The model relative path is incorrect.'
    Assert-True ($configuration.runtime.relativePath -cmatch '^cpu/manual-[0-9a-f]{16}/llama-server\.exe$') 'The runtime relative path is incorrect.'
    Assert-True ($configuration.model.relativePath -notmatch '\\' -and $configuration.runtime.relativePath -notmatch '\\') 'An artifact path does not use forward slashes.'
    Assert-True (@($configuration.runtimeDependencies).Count -eq 3) 'The runtime dependency manifest has the wrong file count.'
    $runtimePrefix = $configuration.runtime.relativePath.Substring(0, $configuration.runtime.relativePath.LastIndexOf('/') + 1)
    Assert-True ([string]::Join(',', @($configuration.runtimeDependencies | ForEach-Object relativePath)) -ceq ($runtimePrefix + 'LICENSE,' + $runtimePrefix + 'empty.dat,' + $runtimePrefix + 'lib/runtime-support.dll')) 'The runtime dependency manifest is not in stable ordinal order.'

    $modelTarget = Join-Path (Join-Path $validFixture.DataDirectory 'Models') ($configuration.model.relativePath.Replace('/', '\'))
    $runtimeTarget = Join-Path (Join-Path $validFixture.DataDirectory 'Runtimes') ($configuration.runtime.relativePath.Replace('/', '\'))
    Assert-True (Test-Path -LiteralPath $modelTarget -PathType Leaf) 'The model artifact was not installed.'
    Assert-True (Test-Path -LiteralPath $runtimeTarget -PathType Leaf) 'The runtime artifact was not installed.'
    Assert-True (Test-Path -LiteralPath (Join-Path ([IO.Path]::GetDirectoryName($runtimeTarget)) 'lib\runtime-support.dll') -PathType Leaf) 'The complete runtime directory was not installed.'
    $sourceModelHash = (Get-FileHash -LiteralPath $validFixture.ModelPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $sourceRuntimeHash = (Get-FileHash -LiteralPath $validFixture.RuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-True ($configuration.model.sha256 -ceq $sourceModelHash) 'The configured model hash is incorrect.'
    Assert-True ($configuration.runtime.sha256 -ceq $sourceRuntimeHash) 'The configured runtime hash is incorrect.'
    Assert-True ([long]$configuration.model.expectedSize -eq (Get-Item -LiteralPath $validFixture.ModelPath).Length) 'The configured model size is incorrect.'
    Assert-True ([long]$configuration.runtime.expectedSize -eq (Get-Item -LiteralPath $validFixture.RuntimePath).Length) 'The configured runtime size is incorrect.'
    Assert-True ((Get-FileHash -LiteralPath $modelTarget -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $sourceModelHash) 'The installed model hash is incorrect.'
    Assert-True ((Get-FileHash -LiteralPath $runtimeTarget -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $sourceRuntimeHash) 'The installed runtime hash is incorrect.'
    $runtimeTargetDirectory = [IO.Path]::GetDirectoryName($runtimeTarget)
    $runtimeFilesExceptExecutable = @(Get-ChildItem -LiteralPath $runtimeTargetDirectory -File -Force -Recurse | Where-Object {
            -not [string]::Equals($_.FullName, $runtimeTarget, [StringComparison]::OrdinalIgnoreCase)
        })
    Assert-True ($runtimeFilesExceptExecutable.Count -eq @($configuration.runtimeDependencies).Count) 'The runtime dependency manifest does not cover every adjacent file.'
    foreach ($dependency in @($configuration.runtimeDependencies)) {
        Assert-True ([string]::Join(',', @($dependency.PSObject.Properties.Name)) -ceq 'relativePath,sha256,expectedSize') 'A runtime dependency uses the wrong schema.'
        Assert-True ($dependency.relativePath -cmatch '^cpu/manual-[0-9a-f]{16}/.+$') 'A runtime dependency relative path is outside the runtime directory.'
        Assert-True ($dependency.relativePath -notmatch '\\') 'A runtime dependency path does not use forward slashes.'
        $dependencyTarget = Join-Path (Join-Path $validFixture.DataDirectory 'Runtimes') ($dependency.relativePath.Replace('/', '\'))
        Assert-True (Test-Path -LiteralPath $dependencyTarget -PathType Leaf) 'A manifested runtime dependency is missing.'
        Assert-True ([long]$dependency.expectedSize -eq (Get-Item -LiteralPath $dependencyTarget).Length) 'A runtime dependency size is incorrect.'
        Assert-True ($dependency.sha256 -ceq (Get-FileHash -LiteralPath $dependencyTarget -Algorithm SHA256).Hash.ToLowerInvariant()) 'A runtime dependency hash is incorrect.'
    }

    $configurationBeforeRefusal = [IO.File]::ReadAllText($configurationPath)
    Invoke-ExpectFailure $validParameters 'Existing configuration protection'
    Assert-True ([IO.File]::ReadAllText($configurationPath) -ceq $configurationBeforeRefusal) 'Existing configuration changed without replacement permission.'
    Assert-True (@(Get-ChildItem -LiteralPath $validFixture.DataDirectory -Filter 'inference.json.backup-*' -File).Count -eq 0) 'A backup was created for a refused replacement.'

    $modelWriteTime = (Get-Item -LiteralPath $modelTarget).LastWriteTimeUtc
    $runtimeWriteTime = (Get-Item -LiteralPath $runtimeTarget).LastWriteTimeUtc
    $sentinelConfiguration = '{"previousConfiguration":true}'
    [IO.File]::WriteAllText($configurationPath, $sentinelConfiguration, [Text.UTF8Encoding]::new($false))
    $replaceParameters = Copy-ParameterMap $validParameters
    $replaceParameters['ReplaceConfiguration'] = $true
    $replaceOutput = @(& $configureScript @replaceParameters)
    Assert-True ($replaceOutput.Count -eq 1 -and $replaceOutput[0] -ceq 'Local inference configuration completed successfully.') 'Replacement emitted unexpected output.'
    $backups = @(Get-ChildItem -LiteralPath $validFixture.DataDirectory -Filter 'inference.json.backup-*' -File)
    Assert-True ($backups.Count -eq 1) 'Replacement did not create exactly one timestamped backup.'
    Assert-True ([IO.File]::ReadAllText($backups[0].FullName) -ceq $sentinelConfiguration) 'The replacement backup does not preserve the previous configuration.'
    Assert-True ((Get-Item -LiteralPath $modelTarget).LastWriteTimeUtc -eq $modelWriteTime) 'The same model artifact was recopied instead of reused.'
    Assert-True ((Get-Item -LiteralPath $runtimeTarget).LastWriteTimeUtc -eq $runtimeWriteTime) 'The same runtime artifact was recopied instead of reused.'
    Assert-True (@(Get-ChildItem -LiteralPath (Join-Path $validFixture.DataDirectory 'Runtimes\cpu') -Directory -Filter 'manual-*').Count -eq 1) 'Runtime reuse created a duplicate installation.'

    $configurationBeforeDependencyTamper = [IO.File]::ReadAllText($configurationPath)
    $backupCountBeforeDependencyTamper = @(Get-ChildItem -LiteralPath $validFixture.DataDirectory -Filter 'inference.json.backup-*' -File).Count
    Write-TestFile (Join-Path $runtimeTargetDirectory 'lib\runtime-support.dll') 'tampered adjacent runtime file'
    Invoke-ExpectFailure $replaceParameters 'A reused runtime with an unverified adjacent dependency'
    Assert-True ([IO.File]::ReadAllText($configurationPath) -ceq $configurationBeforeDependencyTamper) 'Dependency verification failure changed the configuration.'
    Assert-True (@(Get-ChildItem -LiteralPath $validFixture.DataDirectory -Filter 'inference.json.backup-*' -File).Count -eq $backupCountBeforeDependencyTamper) 'Dependency verification failure created a configuration backup.'
    Assert-NoStagingResidue $validFixture.Root

    $atomicFailureFixture = New-TestFixture 'atomic-config-failure'
    [IO.Directory]::CreateDirectory($atomicFailureFixture.DataDirectory) | Out-Null
    $lockedConfigurationPath = Join-Path $atomicFailureFixture.DataDirectory 'inference.json'
    $lockedConfigurationContent = '{"lockedConfiguration":true}'
    Write-TestFile $lockedConfigurationPath $lockedConfigurationContent
    $atomicFailureParameters = New-ConfigureParameters $atomicFailureFixture 'atomic-failure-model'
    $atomicFailureParameters['ReplaceConfiguration'] = $true
    $configurationLock = [IO.FileStream]::new(
        $lockedConfigurationPath,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    try {
        Invoke-ExpectFailure $atomicFailureParameters 'An atomic configuration replacement blocked by an open file'
    }
    finally {
        $configurationLock.Dispose()
    }
    Assert-True ([IO.File]::ReadAllText($lockedConfigurationPath) -ceq $lockedConfigurationContent) 'A failed atomic replacement changed the existing configuration.'
    Assert-NoStagingResidue $atomicFailureFixture.Root

    $badExtensionFixture = New-TestFixture 'bad-extension' 'tiny-model.bin'
    Invoke-ExpectFailure (New-ConfigureParameters $badExtensionFixture 'bad-extension-model') 'A non-GGUF model'
    Assert-NoStagingResidue $badExtensionFixture.Root

    $badRuntimeFixture = New-TestFixture 'bad-runtime-name' 'tiny-model.gguf' 'not-llama.exe'
    Invoke-ExpectFailure (New-ConfigureParameters $badRuntimeFixture 'bad-runtime-model') 'A runtime with the wrong name'
    Assert-NoStagingResidue $badRuntimeFixture.Root

    $badPathFixture = New-TestFixture 'bad-target-path'
    $badDataPath = Join-Path $badPathFixture.Root 'not-a-directory'
    Write-TestFile $badDataPath 'do not replace'
    $badPathParameters = New-ConfigureParameters $badPathFixture 'bad-path-model'
    $badPathParameters['AiDataDirectory'] = $badDataPath
    Invoke-ExpectFailure $badPathParameters 'A file used as the AI data directory'
    Assert-True ([IO.File]::ReadAllText($badDataPath) -ceq 'do not replace') 'The invalid target path was modified.'
    Assert-NoStagingResidue $badPathFixture.Root

    $unsafeIdFixture = New-TestFixture 'unsafe-model-id'
    Invoke-ExpectFailure (New-ConfigureParameters $unsafeIdFixture '..') 'An unsafe model identifier'
    Assert-NoStagingResidue $unsafeIdFixture.Root

    $rangeFixture = New-TestFixture 'invalid-range'
    $rangeParameters = New-ConfigureParameters $rangeFixture 'range-model'
    $rangeParameters['ContextSize'] = 511
    Invoke-ExpectFailure $rangeParameters 'An out-of-range context size'
    Assert-NoStagingResidue $rangeFixture.Root

    $sensitiveFixture = New-TestFixture 'sensitive-runtime'
    Write-TestFile (Join-Path $sensitiveFixture.RuntimeDirectory 'token.json') 'fixture-sensitive-data'
    Invoke-ExpectFailure (New-ConfigureParameters $sensitiveFixture 'sensitive-model') 'A runtime tree containing a sensitive file'
    Assert-True (-not (Test-Path -LiteralPath $sensitiveFixture.DataDirectory)) 'A sensitive runtime tree was partially installed.'
    Assert-NoStagingResidue $sensitiveFixture.Root

    $modelConflictFixture = New-TestFixture 'model-conflict'
    $modelConflictParameters = New-ConfigureParameters $modelConflictFixture 'conflict-model'
    $modelConflictTarget = Join-Path $modelConflictFixture.DataDirectory 'Models\conflict-model\tiny-model.gguf'
    Write-TestFile $modelConflictTarget 'different model artifact'
    Invoke-ExpectFailure $modelConflictParameters 'A different existing model target'
    Assert-True ([IO.File]::ReadAllText($modelConflictTarget) -ceq 'different model artifact') 'A different existing model target was overwritten.'
    Assert-NoStagingResidue $modelConflictFixture.Root

    $runtimeConflictFixture = New-TestFixture 'runtime-conflict'
    $runtimeConflictParameters = New-ConfigureParameters $runtimeConflictFixture 'runtime-conflict-model'
    $runtimeConflictHash = (Get-FileHash -LiteralPath $runtimeConflictFixture.RuntimePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $runtimeConflictTarget = Join-Path $runtimeConflictFixture.DataDirectory ('Runtimes\cpu\manual-' + $runtimeConflictHash.Substring(0, 16) + '\llama-server.exe')
    Write-TestFile $runtimeConflictTarget 'different runtime artifact'
    Invoke-ExpectFailure $runtimeConflictParameters 'A different existing runtime target'
    Assert-True ([IO.File]::ReadAllText($runtimeConflictTarget) -ceq 'different runtime artifact') 'A different existing runtime target was overwritten.'
    Assert-NoStagingResidue $runtimeConflictFixture.Root

    $reparseFixture = New-TestFixture 'runtime-reparse'
    $externalDirectory = Join-Path $reparseFixture.Root 'external-target'
    [IO.Directory]::CreateDirectory($externalDirectory) | Out-Null
    $externalMarker = Join-Path $externalDirectory 'marker.txt'
    Write-TestFile $externalMarker 'outside marker'
    $junctionPath = Join-Path $reparseFixture.RuntimeDirectory 'linked-assets'
    try {
        New-Item -ItemType Junction -Path $junctionPath -Target $externalDirectory -Force | Out-Null
        [void]$createdJunctions.Add($junctionPath)
    }
    catch {
        $reparseTestSkipped = $true
    }
    if (-not $reparseTestSkipped) {
        Invoke-ExpectFailure (New-ConfigureParameters $reparseFixture 'reparse-model') 'A runtime tree containing a reparse point'
        Assert-True ([IO.File]::ReadAllText($externalMarker) -ceq 'outside marker') 'Reparse-point validation traversed or modified the external target.'
        Assert-NoStagingResidue $reparseFixture.Root
        [IO.Directory]::Delete($junctionPath)
        [void]$createdJunctions.Remove($junctionPath)
    }

    Assert-NoStagingResidue $testRoot
    Assert-ProjectFilterSynchronization
}
finally {
    foreach ($junction in @($createdJunctions)) {
        if ([IO.Directory]::Exists($junction)) {
            [IO.Directory]::Delete($junction)
        }
    }
    if (Test-Path -LiteralPath $testRoot) {
        Assert-True (Test-TestPathIsStrictDescendant $testRoot $temporaryRoot) 'Refusing to clean a test directory outside the temporary root.'
        Assert-True ([IO.Path]::GetFileName($testRoot) -match '^D3D12LookDevPTwithAI-ConfigureLocalInferenceTests-[0-9a-f]{32}$') 'Refusing to clean an unexpected test directory.'
        $remainingReparsePoints = @(Get-ChildItem -LiteralPath $testRoot -Force -Recurse | Where-Object {
                ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            })
        Assert-True ($remainingReparsePoints.Count -eq 0) 'Refusing recursive cleanup while a reparse point remains in the test directory.'
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

Assert-True (-not (Test-Path -LiteralPath $testRoot)) 'The test directory was not cleaned.'
if ($reparseTestSkipped) {
    Write-Output 'ConfigureLocalInference tests passed (reparse-point creation was unavailable).'
}
else {
    Write-Output 'ConfigureLocalInference tests passed.'
}
