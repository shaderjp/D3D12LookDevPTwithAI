#requires -Version 7.4

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repository = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$temporaryRoot = [IO.Path]::GetFullPath((Join-Path (
    [IO.Path]::GetTempPath()) (
    'ldpt-' + [Guid]::NewGuid().ToString('N'))))
$temporaryPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $temporaryRoot.StartsWith(
        $temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unsafe integrated portable test root.'
}

function Assert-True {
    param([Parameter(Mandatory = $true)][bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Throws {
    param(
        [Parameter(Mandatory = $true)][scriptblock]$Operation,
        [Parameter(Mandatory = $true)][string]$Name,
        [string]$ExpectedMessage = ''
    )

    try {
        & $Operation
        throw "Expected failure was not raised: $Name"
    }
    catch {
        if ($_.Exception.Message -eq "Expected failure was not raised: $Name") {
            throw
        }
        if (-not [string]::IsNullOrEmpty($ExpectedMessage) -and
            $_.Exception.Message -notlike "*$ExpectedMessage*") {
            throw "Unexpected failure for ${Name}: $($_.Exception.Message)"
        }
    }
}

function Restore-TestEnvironmentVariable {
    param([Parameter(Mandatory = $true)][string]$Name, [AllowNull()][string]$Value)

    if ([string]::IsNullOrEmpty($Value)) {
        Remove-Item -LiteralPath "Env:$Name" -ErrorAction SilentlyContinue
    }
    else {
        [Environment]::SetEnvironmentVariable(
            $Name, $Value, [EnvironmentVariableTarget]::Process)
    }
}

function Assert-WindowsPowerShellDefaultStreams {
    param([Parameter(Mandatory = $true)][string]$Root)

    if (-not $IsWindows) { return }
    $windowsPowerShell = Join-Path $env:SystemRoot `
        'System32\WindowsPowerShell\v1.0\powershell.exe'
    Assert-True (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf) `
        'Windows PowerShell 5.1 is required for the portable path regression.'
    $environmentName = 'D3D12LOOKDEVPT_PORTABLE_STREAM_PROBE_ROOT'
    $originalValue = [Environment]::GetEnvironmentVariable(
        $environmentName, [EnvironmentVariableTarget]::Process)
    $probe = @'
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = [Environment]::GetEnvironmentVariable(
    'D3D12LOOKDEVPT_PORTABLE_STREAM_PROBE_ROOT',
    [EnvironmentVariableTarget]::Process)
$count = 0
foreach ($file in Get-ChildItem -LiteralPath $root -File -Recurse -Force -ErrorAction Stop) {
    $streams = @(Get-Item -LiteralPath $file.FullName -Stream * -ErrorAction Stop)
    if ($streams.Count -ne 1 -or $streams[0].Stream -cne ':$DATA') {
        throw "Unexpected alternate stream inventory: $($file.FullName)"
    }
    $count++
}
if ($count -eq 0) { throw 'The stream probe found no payload files.' }
'@
    $encodedProbe = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($probe))
    try {
        [Environment]::SetEnvironmentVariable(
            $environmentName, $Root, [EnvironmentVariableTarget]::Process)
        & $windowsPowerShell -NoLogo -NoProfile -NonInteractive `
            -EncodedCommand $encodedProbe 2>$null
        Assert-True ($LASTEXITCODE -eq 0) `
            'Windows PowerShell could not enumerate exactly one default stream for every payload file.'
    }
    finally {
        Restore-TestEnvironmentVariable $environmentName $originalValue
    }
}

function Write-FixtureText {
    param([string]$PathValue, [string]$Value)
    [IO.Directory]::CreateDirectory((Split-Path -Parent $PathValue)) | Out-Null
    Set-Content -LiteralPath $PathValue -Value $Value -Encoding utf8NoBOM
}

function Write-FixturePe {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [string]$ImportedDllName = ''
    )
    [IO.Directory]::CreateDirectory((Split-Path -Parent $PathValue)) | Out-Null
    if (-not [string]::IsNullOrEmpty($ImportedDllName) -and
        $ImportedDllName -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}\.dll$') {
        throw "Unsafe fixture import name: $ImportedDllName"
    }
    $bytes = [byte[]]::new(
        $(if ([string]::IsNullOrEmpty($ImportedDllName)) { 512 } else { 1024 }))
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3c)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes(
        [uint16]$(if ([string]::IsNullOrEmpty($ImportedDllName)) { 0 } else { 1 })).CopyTo(
            $bytes, 0x86)
    [BitConverter]::GetBytes([uint16]0xf0).CopyTo($bytes, 0x94)
    [BitConverter]::GetBytes([uint16]0x20b).CopyTo($bytes, 0x98)
    [BitConverter]::GetBytes([uint32]512).CopyTo($bytes, 0xd4)
    [BitConverter]::GetBytes([uint32]16).CopyTo($bytes, 0x104)
    if (-not [string]::IsNullOrEmpty($ImportedDllName)) {
        [BitConverter]::GetBytes([uint32]0x1000).CopyTo($bytes, 0x110)
        [BitConverter]::GetBytes([uint32]40).CopyTo($bytes, 0x114)
        [Text.Encoding]::ASCII.GetBytes('.rdata').CopyTo($bytes, 0x188)
        [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, 0x190)
        [BitConverter]::GetBytes([uint32]0x1000).CopyTo($bytes, 0x194)
        [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, 0x198)
        [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, 0x19c)
        [BitConverter]::GetBytes([uint32]0x1040).CopyTo($bytes, 0x20c)
        [Text.Encoding]::ASCII.GetBytes($ImportedDllName + "`0").CopyTo($bytes, 0x240)
    }
    [IO.File]::WriteAllBytes($PathValue, $bytes)
}

function Write-FixtureManagedPe32 {
    param(
        [Parameter(Mandatory = $true)][string]$PathValue,
        [Parameter(Mandatory = $true)][uint32]$Cor20Flags
    )

    [IO.Directory]::CreateDirectory((Split-Path -Parent $PathValue)) | Out-Null
    $bytes = [byte[]]::new(1024)
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [BitConverter]::GetBytes([int]0x80).CopyTo($bytes, 0x3c)
    $bytes[0x80] = 0x50
    $bytes[0x81] = 0x45
    [BitConverter]::GetBytes([uint16]0x14c).CopyTo($bytes, 0x84)
    [BitConverter]::GetBytes([uint16]1).CopyTo($bytes, 0x86)
    [BitConverter]::GetBytes([uint16]0xe0).CopyTo($bytes, 0x94)
    [BitConverter]::GetBytes([uint16]0x10b).CopyTo($bytes, 0x98)
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, 0xd4)
    [BitConverter]::GetBytes([uint32]16).CopyTo($bytes, 0xf4)
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($bytes, 0x168)
    [BitConverter]::GetBytes([uint32]0x48).CopyTo($bytes, 0x16c)
    [Text.Encoding]::ASCII.GetBytes('.cormeta').CopyTo($bytes, 0x178)
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, 0x180)
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($bytes, 0x184)
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, 0x188)
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($bytes, 0x18c)
    [BitConverter]::GetBytes([uint32]0x48).CopyTo($bytes, 0x200)
    [BitConverter]::GetBytes([uint32]0x1080).CopyTo($bytes, 0x208)
    [BitConverter]::GetBytes([uint32]4).CopyTo($bytes, 0x20c)
    [BitConverter]::GetBytes($Cor20Flags).CopyTo($bytes, 0x210)
    [Text.Encoding]::ASCII.GetBytes('BSJB').CopyTo($bytes, 0x280)
    [IO.File]::WriteAllBytes($PathValue, $bytes)
}

function Get-LowerSha256 {
    param([string]$PathValue)
    return (Get-FileHash -LiteralPath $PathValue -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Copy-RepositoryFixtureFile {
    param([string]$RelativePath, [string]$FixtureRepository)
    $source = Join-Path $repository $RelativePath
    $target = Join-Path $FixtureRepository $RelativePath
    [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}

function New-FixtureRepository {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    [IO.Directory]::CreateDirectory($PathValue) | Out-Null
    foreach ($relative in @(
            'Scripts/BuildIntegratedPortable.ps1',
            'D3D12LookDevPTwithAI.vcxproj',
            'suite.lock.json',
            'LICENSE',
            'ThirdParty/assimp/LICENSE',
            'ThirdParty/assimp/contrib/zlib/LICENSE',
            'ThirdParty/assimp/contrib/clipper/License.txt',
            'ThirdParty/assimp/contrib/earcut-hpp/LICENSE',
            'ThirdParty/assimp/contrib/openddlparser/LICENSE',
            'ThirdParty/assimp/contrib/poly2tri/LICENSE',
            'ThirdParty/assimp/contrib/pugixml/LICENSE.md',
            'ThirdParty/assimp/contrib/rapidjson/license.txt',
            'ThirdParty/assimp/contrib/utf8cpp/doc/LICENSE',
            'ThirdParty/assimp/contrib/zip/UNLICENSE',
            'ThirdParty/assimp/contrib/unzip/MiniZip64_info.txt',
            'ThirdParty/assimp/contrib/Open3DGC/o3dgcCommon.h',
            'ThirdParty/assimp/code/AssetLib/M3D/m3d.h',
            'ThirdParty/assimp/code/AssetLib/MDC/MDCNormalTable.h',
            'ThirdParty/assimp/code/AssetLib/Assjson/cencode.h',
            'ThirdParty/assimp/contrib/stb/stb_image.h',
            'ThirdParty/DirectXTex/LICENSE',
            'ThirdParty/tinyexr/LICENSE',
            'ThirdParty/tinyexr/NOTICE',
            'Package/Licenses/OpenJPH-BSD-2-Clause.txt',
            'ThirdParty/tinyexr/deps/zstd/LICENSE',
            'ThirdParty/tinygltf/LICENSE',
            'ThirdParty/tinygltf/json.hpp',
            'ThirdParty/basis_universal/LICENSE',
            'ThirdParty/basis_universal/NOTICE',
            'ThirdParty/basis_universal/zstd/LICENSE')) {
        Copy-RepositoryFixtureFile $relative $PathValue
    }
    & git -C $PathValue init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the clean fixture repository.' }
    & git -C $PathValue config user.email 'portable-test@example.invalid'
    & git -C $PathValue config user.name 'Portable Test'
    & git -C $PathValue add .
    & git -C $PathValue commit --quiet -m 'portable fixture'
    if ($LASTEXITCODE -ne 0) { throw 'Could not commit the clean fixture repository.' }
}

function New-NativeFixture {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    Write-FixturePe (Join-Path $PathValue 'D3D12LookDevPTwithAI.exe')
    foreach ($record in @(
            @('D3D12LookDevPTwithAI.pri', 'fixture-pri'),
            @('D3D12LookDevPTwithAI.winmd', 'fixture-winmd'),
            @('D3D12/D3D12Core.dll', 'fixture-agility'),
            @('Microsoft.ui.xaml.dll', 'fixture-xaml'),
            @('Microsoft.WindowsAppRuntime.dll', 'fixture-windows-app-runtime'),
            @('Microsoft.WindowsAppRuntime.Bootstrap.dll', 'fixture-bootstrap'),
            @('dxcompiler.dll', 'fixture-dxc'),
            @('dxil.dll', 'fixture-dxil'),
            @('PathTracing.lib.cso', 'fixture-shader'),
            @('Source/WinUI/App.xbf', 'fixture-app-xbf'),
            @('Source/WinUI/MainWindow.xbf', 'fixture-window-xbf'))) {
        $target = Join-Path $PathValue $record[0]
        if ([IO.Path]::GetExtension([string]$record[0]) -ieq '.dll') {
            Write-FixturePe $target
        }
        else { Write-FixtureText $target $record[1] }
    }
}

function Get-TestRuntimePackVersion {
    $nugetRoot = Join-Path ([Environment]::GetFolderPath('UserProfile')) `
        '.nuget\packages\microsoft.netcore.app.runtime.win-x64'
    $versions = [Collections.Generic.List[object]]::new()
    foreach ($directory in Get-ChildItem -LiteralPath $nugetRoot -Directory -Force) {
        $parsed = $null
        if ([Version]::TryParse($directory.Name, [ref]$parsed) -and
            $parsed.Major -eq 9 -and
            (Test-Path -LiteralPath (Join-Path $directory.FullName 'LICENSE.TXT')) -and
            (Test-Path -LiteralPath (
                Join-Path $directory.FullName 'THIRD-PARTY-NOTICES.TXT'))) {
            $versions.Add([pscustomobject]@{ Version = $parsed; Text = $directory.Name })
        }
    }
    $selected = @($versions | Sort-Object Version -Descending | Select-Object -First 1)
    if ($selected.Count -ne 1) { throw 'A .NET 9 win-x64 runtime pack is required for this test.' }
    return $selected[0].Text
}

function New-ChatHostFixture {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    Write-FixturePe (Join-Path $PathValue 'D3D12LookDevPTwithAI.ChatHost.exe')
    $assetRecords = @(
            @('D3D12LookDevPTwithAI.ChatHost.dll', 'fixture-release-chat-host'),
            @('D3D12LookDevPTwithAI.Chat.Core.dll', 'fixture-chat-core'),
            @('D3D12LookDevPTwithAI.Chat.Infrastructure.dll', 'fixture-chat-infrastructure'),
            @('hostfxr.dll', 'fixture-hostfxr'),
            @('hostpolicy.dll', 'fixture-hostpolicy'),
            @('coreclr.dll', 'fixture-coreclr'),
            @('System.Private.CoreLib.dll', 'fixture-corelib'),
            @('Microsoft.Data.Sqlite.dll', 'fixture-sqlite-managed'),
            @('e_sqlite3.dll', 'fixture-sqlite'))
    foreach ($record in $assetRecords) {
        $target = Join-Path $PathValue $record[0]
        if ([string]$record[0] -in @(
                'D3D12LookDevPTwithAI.ChatHost.dll',
                'D3D12LookDevPTwithAI.Chat.Core.dll',
                'D3D12LookDevPTwithAI.Chat.Infrastructure.dll',
                'Microsoft.Data.Sqlite.dll')) {
            Write-FixtureManagedPe32 $target ([uint32]0x00000001)
        }
        else { Write-FixturePe $target }
    }
    $version = Get-TestRuntimePackVersion
    $runtimeConfig = [ordered]@{
        runtimeOptions = [ordered]@{
            tfm = 'net9.0'
            includedFrameworks = @(@{
                name = 'Microsoft.NETCore.App'
                version = $version
            })
        }
    }
    Write-FixtureText (
        Join-Path $PathValue 'D3D12LookDevPTwithAI.ChatHost.runtimeconfig.json') `
        ($runtimeConfig | ConvertTo-Json -Depth 5 -Compress)
    $runtimeTarget = '.NETCoreApp,Version=v9.0/win-x64'
    $runtimeAssets = [ordered]@{}
    foreach ($record in $assetRecords) { $runtimeAssets[$record[0]] = @{} }
    $deps = [ordered]@{
        runtimeTarget = [ordered]@{ name = $runtimeTarget; signature = '' }
        targets = [ordered]@{
            $runtimeTarget = [ordered]@{
                'PortableFixture/1.0.0' = [ordered]@{ runtime = $runtimeAssets }
            }
        }
        libraries = [ordered]@{
            'PortableFixture/1.0.0' = [ordered]@{
                type = 'project'; serviceable = $false; sha512 = ''
            }
        }
    }
    Write-FixtureText (
        Join-Path $PathValue 'D3D12LookDevPTwithAI.ChatHost.deps.json') `
        ($deps | ConvertTo-Json -Depth 8 -Compress)
}

function New-AiFixture {
    param([Parameter(Mandatory = $true)][string]$PathValue)
    $model = Join-Path $PathValue 'Models/demo-model/demo.gguf'
    $runtime = Join-Path $PathValue 'Runtimes/cpu/demo-runtime/llama-server.exe'
    $backend = Join-Path $PathValue 'Runtimes/cpu/demo-runtime/backend.dll'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $model)) | Out-Null
    $ggufBytes = [byte[]]::new(32)
    [Text.Encoding]::ASCII.GetBytes('GGUF').CopyTo($ggufBytes, 0)
    [BitConverter]::GetBytes([uint32]3).CopyTo($ggufBytes, 4)
    [IO.File]::WriteAllBytes($model, $ggufBytes)
    Write-FixturePe $runtime
    Write-FixturePe $backend
    Write-FixtureText (Join-Path $PathValue 'Licenses/model-terms.txt') `
        'Demo model redistribution terms for test only.'
    Write-FixtureText (Join-Path $PathValue 'Licenses/runtime-license.txt') `
        'MIT license fixture for the llama.cpp runtime test.'

    $inference = [ordered]@{
        schemaVersion = 1
        modelId = 'demo-model'
        backend = 'cpu'
        contextSize = 4096
        maxTokens = 512
        temperature = 0.2
        model = [ordered]@{
            relativePath = 'demo-model/demo.gguf'
            sha256 = Get-LowerSha256 $model
            expectedSize = [long](Get-Item -LiteralPath $model).Length
        }
        runtime = [ordered]@{
            relativePath = 'cpu/demo-runtime/llama-server.exe'
            sha256 = Get-LowerSha256 $runtime
            expectedSize = [long](Get-Item -LiteralPath $runtime).Length
        }
        runtimeDependencies = @([ordered]@{
            relativePath = 'cpu/demo-runtime/backend.dll'
            sha256 = Get-LowerSha256 $backend
            expectedSize = [long](Get-Item -LiteralPath $backend).Length
        })
    }
    Write-FixtureText (Join-Path $PathValue 'inference.json') `
        ($inference | ConvertTo-Json -Depth 6 -Compress)
    $redistribution = [ordered]@{
        schemaVersion = 1
        model = [ordered]@{
            name = 'Portable Test Model'
            revision = 'test-revision-1'
            sourceUrl = 'https://example.invalid/models/portable-test-model'
            licenseExpression = 'LicenseRef-Portable-Test-Model'
            licenseFile = 'Licenses/model-terms.txt'
        }
        runtime = [ordered]@{
            name = 'llama.cpp Portable Test Runtime'
            revision = 'b10205-test'
            sourceUrl = 'https://github.com/ggml-org/llama.cpp'
            licenseExpression = 'MIT'
            licenseFile = 'Licenses/runtime-license.txt'
        }
    }
    Write-FixtureText (Join-Path $PathValue 'redistribution.json') `
        ($redistribution | ConvertTo-Json -Depth 5 -Compress)
}

function Get-PayloadRelativeFiles {
    param([Parameter(Mandatory = $true)][string]$Root)
    return @(
        Get-ChildItem -LiteralPath $Root -File -Recurse -Force |
            ForEach-Object {
                [IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/')
            } | Sort-Object)
}

function Assert-ExactSet {
    param(
        [Parameter(Mandatory = $true)][string[]]$Actual,
        [Parameter(Mandatory = $true)][string[]]$Expected,
        [Parameter(Mandatory = $true)][string]$Context
    )
    if ($Actual.Count -ne $Expected.Count) {
        throw "$Context count differs: actual=$($Actual.Count), expected=$($Expected.Count)"
    }
    foreach ($item in $Actual) {
        if ($Expected -cnotcontains $item) { throw "$Context has unexpected item: $item" }
    }
}

[IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
try {
    $fixtureRepository = Join-Path $temporaryRoot 'repository'
    $native = Join-Path $temporaryRoot 'native'
    $chatHost = Join-Path $temporaryRoot 'chat-host'
    $ai = Join-Path $temporaryRoot 'ai'
    New-FixtureRepository $fixtureRepository
    New-NativeFixture $native
    New-ChatHostFixture $chatHost
    New-AiFixture $ai
    $builder = Join-Path $fixtureRepository 'Scripts/BuildIntegratedPortable.ps1'

    $originalGitIndexFile = [Environment]::GetEnvironmentVariable(
        'GIT_INDEX_FILE', [EnvironmentVariableTarget]::Process)
    try {
        [Environment]::SetEnvironmentVariable(
            'GIT_INDEX_FILE', $temporaryRoot, [EnvironmentVariableTarget]::Process)
        Assert-Throws {
            & $builder -OutputDirectory (Join-Path $temporaryRoot 'routed-index-output') `
                -SkipBuild -NativeBuildDirectory $native `
                -ChatHostPublishDirectory $chatHost -WithoutAi `
                -AllowDirtySource -NoArchive
        } 'Git index routing environment rejection' `
            'Git repository routing environment is not permitted: GIT_INDEX_FILE'
    }
    finally {
        Restore-TestEnvironmentVariable 'GIT_INDEX_FILE' $originalGitIndexFile
    }

    $originalGitDir = [Environment]::GetEnvironmentVariable(
        'GIT_DIR', [EnvironmentVariableTarget]::Process)
    $originalGitWorkTree = [Environment]::GetEnvironmentVariable(
        'GIT_WORK_TREE', [EnvironmentVariableTarget]::Process)
    try {
        [Environment]::SetEnvironmentVariable(
            'GIT_DIR', (Join-Path $repository '.git'),
            [EnvironmentVariableTarget]::Process)
        [Environment]::SetEnvironmentVariable(
            'GIT_WORK_TREE', $repository, [EnvironmentVariableTarget]::Process)
        Assert-Throws {
            & $builder -OutputDirectory (Join-Path $temporaryRoot 'routed-repo-output') `
                -SkipBuild -NativeBuildDirectory $native `
                -ChatHostPublishDirectory $chatHost -WithoutAi `
                -AllowDirtySource -NoArchive
        } 'Git repository routing rejection' `
            'Git repository routing environment is not permitted: GIT_DIR'
    }
    finally {
        Restore-TestEnvironmentVariable 'GIT_DIR' $originalGitDir
        Restore-TestEnvironmentVariable 'GIT_WORK_TREE' $originalGitWorkTree
    }

    $managedFixturePath = Join-Path $chatHost 'Microsoft.Data.Sqlite.dll'
    $managedFixtureBytes = [IO.File]::ReadAllBytes($managedFixturePath)
    Write-FixtureManagedPe32 $managedFixturePath ([uint32]0x00000003)
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'managed-x86-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'managed 32-bit-required payload rejection' `
        'CLR runtime header is not architecture-neutral IL-only code'
    [IO.File]::WriteAllBytes($managedFixturePath, $managedFixtureBytes)

    $plainArm64Path = Join-Path $native 'Microsoft.WindowsAppRuntime.dll'
    $plainArm64Bytes = [IO.File]::ReadAllBytes($plainArm64Path)
    $plainArm64Mutated = [byte[]]$plainArm64Bytes.Clone()
    [BitConverter]::GetBytes([uint16]0xaa64).CopyTo($plainArm64Mutated, 0x84)
    [IO.File]::WriteAllBytes($plainArm64Path, $plainArm64Mutated)
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'plain-arm64-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'plain ARM64 payload rejection' 'not a validated ARM64X hybrid executable'
    [IO.File]::WriteAllBytes($plainArm64Path, $plainArm64Bytes)

    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'missing-ai') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -AllowDirtySource -NoArchive
    } 'AI is required by default' 'require all three AI'

    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'missing-acceptance') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest (Join-Path $ai 'inference.json') `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AllowDirtySource -NoArchive
    } 'AI acceptance is explicit' 'requires explicit'

    $deepSourceRoot = Join-Path $temporaryRoot (
        'source-root-that-is-deliberately-too-deep-for-legacy-build-tools-' +
        ('s' * 32))
    New-FixtureRepository $deepSourceRoot
    $deepSourceBuilder = Join-Path $deepSourceRoot `
        'Scripts/BuildIntegratedPortable.ps1'
    Assert-True ($deepSourceRoot.Length -gt 120) `
        'Deep source fixture did not exceed the documented source-root budget.'
    Assert-Throws {
        & $deepSourceBuilder -OutputDirectory (Join-Path $temporaryRoot 'deep-source-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'source-root path budget' 'Source repository root path is too long'

    $tooLongOutput = Join-Path $temporaryRoot ('o' * 220)
    Assert-True ($tooLongOutput.Length -gt 240) `
        'Long publication fixture did not exceed the documented output budget.'
    Assert-Throws {
        & $builder -OutputDirectory $tooLongOutput `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'publication path budget' 'Publication target path is too long'

    $longOutputPrefix = 'D3D12LookDevPTwithAI-integrated-win-x64-'
    $longOutputTargetCharacters = 148
    $longOutputPadding = $longOutputTargetCharacters - $temporaryRoot.Length -
        1 - $longOutputPrefix.Length
    Assert-True ($longOutputPadding -gt 0) `
        'Temporary root is too long for the positive long-output regression.'
    $longOutputName = $longOutputPrefix + ('x' * $longOutputPadding)
    $longOutput = Join-Path $temporaryRoot $longOutputName
    $legacyStaging = Join-Path $temporaryRoot (
        '.integrated-portable-staging-' + $longOutputName + '-' + ('a' * 32))
    $legacyDirectXTexAssets = Join-Path $legacyStaging `
        'build/third-party/directxtex/obj/project.assets.json'
    Assert-True ($legacyDirectXTexAssets.Length -gt 260) `
        'Long-output regression would not have exceeded the former MSBuild path.'
    Assert-True ($longOutput.Length -le 240) `
        'Long-output regression exceeds the supported publication path budget.'
    $stagingPause = Join-Path $temporaryRoot 'short-staging-pause'
    [IO.Directory]::CreateDirectory($stagingPause) | Out-Null
    $stagingJob = Start-Job -ScriptBlock {
        param($BuilderPath, $OutputPath, $NativePath, $ChatPath, $PausePath)
        $env:D3D12LOOKDEVPT_PORTABLE_TEST_PAUSE_AFTER_STAGING_CREATED = $PausePath
        & $BuilderPath -OutputDirectory $OutputPath -SkipBuild `
            -NativeBuildDirectory $NativePath -ChatHostPublishDirectory $ChatPath `
            -WithoutAi -AllowDirtySource -NoArchive
    } -ArgumentList $builder, $longOutput, $native, $chatHost, $stagingPause
    try {
        $stagingDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $stagingReached = Test-Path -LiteralPath (
                Join-Path $stagingPause 'reached') -PathType Leaf
            if (-not $stagingReached) { Start-Sleep -Milliseconds 10 }
        } while (-not $stagingReached -and [DateTime]::UtcNow -lt $stagingDeadline -and
            $stagingJob.State -eq 'Running')
        Assert-True $stagingReached 'Long-output build did not reach staging pause.'
        $stagingDirectories = @(
            Get-ChildItem -LiteralPath $temporaryRoot -Directory -Force |
                Where-Object Name -Match '^\.ldp-[0-9a-f]{32}$')
        Assert-True ($stagingDirectories.Count -eq 1) `
            'Long-output build did not create exactly one short sibling staging root.'
        $shortStaging = $stagingDirectories[0].FullName
        $layoutPath = Join-Path $stagingPause 'layout.json'
        Assert-True (Test-Path -LiteralPath $layoutPath -PathType Leaf) `
            'Long-output build did not publish its test-only path layout.'
        $layout = Get-Content -LiteralPath $layoutPath -Raw | ConvertFrom-Json
        Assert-True ([string]$layout.transactionRoot -ceq $shortStaging) `
            'Test-only path layout did not identify the sibling payload staging root.'
        $shortBuildWorkspace = [string]$layout.buildWorkspaceRoot
        Assert-True ([IO.Path]::GetFileName($shortBuildWorkspace) -cmatch
            '^\.ldpb-[0-9a-f]{32}$') `
            'Build workspace does not use the short randomized name contract.'
        $shortDirectXTexAssets = Join-Path $shortBuildWorkspace `
            'b/x/o/project.assets.json'
        Assert-True ($shortStaging.Length -le 160) `
            'Short staging root exceeded the build-root path budget.'
        Assert-True ($shortBuildWorkspace.Length -le 100) `
            'Short build workspace exceeded its root path budget.'
        Assert-True ($shortDirectXTexAssets.Length -le 240) `
            'Short DirectXTex intermediate exceeded the critical tool path budget.'
        Assert-True ($shortDirectXTexAssets.Length -lt
            $legacyDirectXTexAssets.Length - 100) `
            'Short staging layout did not materially reduce the legacy path.'
        Write-FixtureText (Join-Path $stagingPause 'continue') 'continue'
        Wait-Job $stagingJob -Timeout 30 | Out-Null
        Assert-True ($stagingJob.State -eq 'Completed') `
            "Long-output build failed: $($stagingJob.ChildJobs[0].JobStateInfo.Reason)"
        Assert-True (Test-Path -LiteralPath $longOutput -PathType Container) `
            'Long-output build did not publish its portable payload.'
        $publishedLongPaths = @(
            Get-ChildItem -LiteralPath $longOutput -File -Recurse -Force |
                ForEach-Object FullName)
        Assert-True ($publishedLongPaths.Count -gt 0) `
            'Long-output build published no payload files.'
        Assert-True (@($publishedLongPaths | Where-Object Length -gt 240).Count -eq 0) `
            'Long-output build published a descendant beyond the supported path budget.'
        Assert-WindowsPowerShellDefaultStreams $longOutput
        Assert-True (@(
            Get-ChildItem -LiteralPath $temporaryRoot -Directory -Force |
                Where-Object Name -Match '^\.ldp-[0-9a-f]{32}$').Count -eq 0) `
            'Short sibling staging root was not removed after publication.'
        Assert-True (-not (Test-Path -LiteralPath $shortBuildWorkspace)) `
            'Test-only SkipBuild unexpectedly created a build workspace.'
    }
    finally {
        if ($stagingJob.State -eq 'Running') {
            Write-FixtureText (Join-Path $stagingPause 'continue') 'continue'
            Wait-Job $stagingJob -Timeout 5 | Out-Null
        }
        Receive-Job $stagingJob -ErrorAction SilentlyContinue | Out-Null
        Remove-Job $stagingJob -Force -ErrorAction SilentlyContinue
    }

    $pathBudgetAi = Join-Path $temporaryRoot 'published-path-budget-ai'
    New-AiFixture $pathBudgetAi
    $pathBudgetOutput = Join-Path $temporaryRoot 'published-path-budget-output'
    $dependencyDirectory = Join-Path $pathBudgetAi 'Runtimes/cpu/demo-runtime'
    $oldDependency = Join-Path $dependencyDirectory 'backend.dll'
    $relativePrefix = 'AI/Runtimes/cpu/demo-runtime/'
    $longDependencyNameCharacters = 241 - $pathBudgetOutput.Length - 1 -
        $relativePrefix.Length
    Assert-True ($longDependencyNameCharacters -gt 4 -and
        $longDependencyNameCharacters -le 200) `
        'Could not create a bounded over-budget payload fixture path.'
    $longDependencyName = ('d' * ($longDependencyNameCharacters - 4)) + '.dll'
    $longDependency = Join-Path $dependencyDirectory $longDependencyName
    [IO.File]::Move($oldDependency, $longDependency)
    $pathBudgetInferencePath = Join-Path $pathBudgetAi 'inference.json'
    $pathBudgetInference = Get-Content -LiteralPath $pathBudgetInferencePath -Raw |
        ConvertFrom-Json
    $pathBudgetInference.runtimeDependencies[0].relativePath =
        "cpu/demo-runtime/$longDependencyName"
    $pathBudgetInference.runtimeDependencies[0].sha256 =
        Get-LowerSha256 $longDependency
    $pathBudgetInference.runtimeDependencies[0].expectedSize =
        [long](Get-Item -LiteralPath $longDependency).Length
    Write-FixtureText $pathBudgetInferencePath `
        ($pathBudgetInference | ConvertTo-Json -Depth 6 -Compress)
    $projectedLongDependency = Join-Path $pathBudgetOutput `
        "$relativePrefix$longDependencyName"
    Assert-True ($projectedLongDependency.Length -eq 241) `
        'Over-budget payload fixture did not project to exactly 241 characters.'
    Assert-Throws {
        & $builder -OutputDirectory $pathBudgetOutput `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $pathBudgetAi `
            -AiArtifactManifest $pathBudgetInferencePath `
            -AiRedistributionManifest (Join-Path $pathBudgetAi 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'published descendant path budget' 'Published payload file'
    Assert-True (-not (Test-Path -LiteralPath $pathBudgetOutput)) `
        'Over-budget payload path published an output directory.'
    Assert-True (-not (Test-Path -LiteralPath "$pathBudgetOutput.manifest.sha256")) `
        'Over-budget payload path published a manifest sidecar.'
    Assert-True (@(
        Get-ChildItem -LiteralPath $temporaryRoot -Directory -Force |
            Where-Object Name -Match '^\.ldp-[0-9a-f]{32}$').Count -eq 0) `
        'Over-budget payload path left a transaction staging directory.'

    $initialFixtureHead = (& git -C $fixtureRepository rev-parse HEAD).Trim()
    $headPause = Join-Path $temporaryRoot 'source-head-pause'
    [IO.Directory]::CreateDirectory($headPause) | Out-Null
    $headRaceOutput = Join-Path $temporaryRoot 'source-head-race-output'
    $headRaceJob = Start-Job -ScriptBlock {
        param($BuilderPath, $OutputPath, $NativePath, $ChatPath, $PausePath)
        $env:D3D12LOOKDEVPT_PORTABLE_TEST_PAUSE_AFTER_SOURCE_SNAPSHOT = $PausePath
        & $BuilderPath -OutputDirectory $OutputPath -SkipBuild `
            -NativeBuildDirectory $NativePath -ChatHostPublishDirectory $ChatPath `
            -WithoutAi -AllowDirtySource -NoArchive
    } -ArgumentList $builder, $headRaceOutput, $native, $chatHost, $headPause
    try {
        $raceDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $pauseReached = Test-Path -LiteralPath (
                Join-Path $headPause 'reached') -PathType Leaf
            if (-not $pauseReached) { Start-Sleep -Milliseconds 10 }
        } while (-not $pauseReached -and [DateTime]::UtcNow -lt $raceDeadline -and
            $headRaceJob.State -eq 'Running')
        Assert-True $pauseReached 'Source HEAD race did not reach snapshot pause.'
        Write-FixtureText (Join-Path $fixtureRepository 'head-race-marker.txt') `
            'second clean fixture commit'
        & git -C $fixtureRepository add head-race-marker.txt
        & git -C $fixtureRepository commit --quiet -m 'second clean fixture commit'
        Assert-True ($LASTEXITCODE -eq 0) 'Could not create the clean HEAD race commit.'
        Write-FixtureText (Join-Path $headPause 'continue') 'continue'
        Wait-Job $headRaceJob -Timeout 20 | Out-Null
        Assert-True ($headRaceJob.State -eq 'Failed') `
            'Builder accepted a clean source HEAD change during the build.'
        $reason = [string]$headRaceJob.ChildJobs[0].JobStateInfo.Reason.Message
        Assert-True ($reason.Contains('source HEAD changed',
            [StringComparison]::OrdinalIgnoreCase)) `
            "HEAD race failure did not identify the commit change: $reason"
        Assert-True (-not (Test-Path -LiteralPath $headRaceOutput)) `
            'Source HEAD race published a payload directory.'
    }
    finally {
        Receive-Job $headRaceJob -ErrorAction SilentlyContinue | Out-Null
        Remove-Job $headRaceJob -Force -ErrorAction SilentlyContinue
        & git -C $fixtureRepository switch --detach --quiet $initialFixtureHead
        if ($LASTEXITCODE -ne 0) { throw 'Could not restore the fixture source HEAD.' }
    }

    $statusRaceOutput = Join-Path $temporaryRoot 'source-status-race-output'
    $statusPause = Join-Path $temporaryRoot 'source-status-pause'
    [IO.Directory]::CreateDirectory($statusPause) | Out-Null
    $statusRaceMarker = Join-Path $fixtureRepository 'prepublish-untracked-race.txt'
    $statusRaceJob = Start-Job -ScriptBlock {
        param($BuilderPath, $OutputPath, $NativePath, $ChatPath, $PausePath)
        $env:D3D12LOOKDEVPT_PORTABLE_TEST_PAUSE_AFTER_SIDECAR_PUBLICATION =
            $PausePath
        & $BuilderPath -OutputDirectory $OutputPath -SkipBuild `
            -NativeBuildDirectory $NativePath -ChatHostPublishDirectory $ChatPath `
            -WithoutAi -AllowDirtySource -NoArchive
    } -ArgumentList $builder, $statusRaceOutput, $native, $chatHost, $statusPause
    try {
        $raceDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $pauseReached = Test-Path -LiteralPath (
                Join-Path $statusPause 'reached') -PathType Leaf
            if (-not $pauseReached) { Start-Sleep -Milliseconds 10 }
        } while (-not $pauseReached -and [DateTime]::UtcNow -lt $raceDeadline -and
            $statusRaceJob.State -eq 'Running')
        Assert-True $pauseReached 'Source status race did not reach publication pause.'
        Assert-True (Test-Path -LiteralPath "$statusRaceOutput.manifest.sha256" `
            -PathType Leaf) 'Source status race did not publish its staged sidecar.'
        Write-FixtureText $statusRaceMarker 'untracked mutation during archive window'
        Write-FixtureText (Join-Path $statusPause 'continue') 'continue'
        Wait-Job $statusRaceJob -Timeout 20 | Out-Null
        Assert-True ($statusRaceJob.State -eq 'Failed') `
            'Builder accepted a source status change before publication.'
        $reason = [string]$statusRaceJob.ChildJobs[0].JobStateInfo.Reason.Message
        Assert-True ($reason.Contains('worktree changed before portable publication',
            [StringComparison]::OrdinalIgnoreCase)) `
            "Status race failure did not identify the worktree change: $reason"
        Assert-True (-not (Test-Path -LiteralPath $statusRaceOutput)) `
            'Source status race published a payload directory.'
        Assert-True (-not (Test-Path -LiteralPath `
            "$statusRaceOutput.manifest.sha256")) `
            'Builder-owned manifest sidecar survived a rejected status race.'
    }
    finally {
        Receive-Job $statusRaceJob -ErrorAction SilentlyContinue | Out-Null
        Remove-Job $statusRaceJob -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $statusRaceMarker -PathType Leaf) {
            [IO.File]::Delete($statusRaceMarker)
        }
    }

    $raceOutput = Join-Path $temporaryRoot 'race-output'
    $publicationPause = Join-Path $temporaryRoot 'publication-pause'
    [IO.Directory]::CreateDirectory($publicationPause) | Out-Null
    $raceJob = Start-Job -ScriptBlock {
        param($BuilderPath, $OutputPath, $NativePath, $ChatPath, $PausePath)
        $env:D3D12LOOKDEVPT_PORTABLE_TEST_PAUSE_AFTER_SIDECAR_PUBLICATION =
            $PausePath
        & $BuilderPath -OutputDirectory $OutputPath -SkipBuild `
            -NativeBuildDirectory $NativePath -ChatHostPublishDirectory $ChatPath `
            -WithoutAi -AllowDirtySource -NoArchive
    } -ArgumentList $builder, $raceOutput, $native, $chatHost, $publicationPause
    try {
        $raceDeadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $pauseReached = Test-Path -LiteralPath (
                Join-Path $publicationPause 'reached') -PathType Leaf
            if (-not $pauseReached) { Start-Sleep -Milliseconds 10 }
        } while (-not $pauseReached -and [DateTime]::UtcNow -lt $raceDeadline -and
            $raceJob.State -eq 'Running')
        Assert-True $pauseReached 'Race test did not reach sidecar publication.'
        $publishedSidecar = "$raceOutput.manifest.sha256"
        Assert-True (Test-Path -LiteralPath $publishedSidecar -PathType Leaf) `
            'Race test did not observe the published manifest sidecar.'
        Write-FixtureText $publishedSidecar 'concurrent replacement must survive'
        [IO.Directory]::CreateDirectory($raceOutput) | Out-Null
        $raceSentinel = Join-Path $raceOutput 'concurrent-owner.txt'
        Write-FixtureText $raceSentinel 'must-survive-publication-race'
        Write-FixtureText (Join-Path $publicationPause 'continue') 'continue'
        Wait-Job $raceJob -Timeout 20 | Out-Null
        Assert-True ($raceJob.State -eq 'Failed') `
            'Builder did not reject an output directory created during the build.'
        Assert-True ((Get-Content -LiteralPath $raceSentinel -Raw).Trim() -ceq
            'must-survive-publication-race') `
            'Builder deleted or modified a concurrently created output directory.'
        Assert-True ((Get-Content -LiteralPath $publishedSidecar -Raw).Trim() -ceq
            'concurrent replacement must survive') `
            'Builder deleted or modified a replaced publication sidecar.'
    }
    finally {
        Receive-Job $raceJob -ErrorAction SilentlyContinue | Out-Null
        Remove-Job $raceJob -Force -ErrorAction SilentlyContinue
    }

    $aiRace = Join-Path $temporaryRoot 'ai-validation-race'
    New-AiFixture $aiRace
    $aiPause = Join-Path $temporaryRoot 'ai-validation-pause'
    [IO.Directory]::CreateDirectory($aiPause) | Out-Null
    $aiRaceOutput = Join-Path $temporaryRoot 'ai-validation-race-output'
    $aiRaceJob = Start-Job -ScriptBlock {
        param($BuilderPath, $OutputPath, $NativePath, $ChatPath, $AiPath, $PausePath)
        $env:D3D12LOOKDEVPT_PORTABLE_TEST_PAUSE_AFTER_AI_VALIDATION = $PausePath
        & $BuilderPath -OutputDirectory $OutputPath -SkipBuild `
            -NativeBuildDirectory $NativePath -ChatHostPublishDirectory $ChatPath `
            -AiArtifactDirectory $AiPath `
            -AiArtifactManifest (Join-Path $AiPath 'inference.json') `
            -AiRedistributionManifest (Join-Path $AiPath 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } -ArgumentList $builder, $aiRaceOutput, $native, $chatHost, $aiRace, $aiPause
    try {
        $raceDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $pauseReached = Test-Path -LiteralPath (
                Join-Path $aiPause 'reached') -PathType Leaf
            if (-not $pauseReached) { Start-Sleep -Milliseconds 10 }
        } while (-not $pauseReached -and [DateTime]::UtcNow -lt $raceDeadline -and
            $aiRaceJob.State -eq 'Running')
        Assert-True $pauseReached 'AI race test did not reach post-validation pause.'
        [IO.File]::AppendAllText(
            (Join-Path $aiRace 'Models/demo-model/demo.gguf'), 'post-validation-tamper')
        Write-FixtureText (Join-Path $aiPause 'continue') 'continue'
        Wait-Job $aiRaceJob -Timeout 20 | Out-Null
        Assert-True ($aiRaceJob.State -eq 'Failed') `
            'Builder accepted an AI artifact changed after validation.'
        $reason = [string]$aiRaceJob.ChildJobs[0].JobStateInfo.Reason.Message
        Assert-True ($reason.Contains('changed after validation',
            [StringComparison]::OrdinalIgnoreCase)) `
            "AI race failure did not identify post-validation change: $reason"
        Assert-True (-not (Test-Path -LiteralPath $aiRaceOutput)) `
            'AI race failure published a payload directory.'
    }
    finally {
        Receive-Job $aiRaceJob -ErrorAction SilentlyContinue | Out-Null
        Remove-Job $aiRaceJob -Force -ErrorAction SilentlyContinue
    }

    $output = Join-Path $temporaryRoot 'integrated-portable'
    & $builder -OutputDirectory $output `
        -SkipBuild -NativeBuildDirectory $native `
        -ChatHostPublishDirectory $chatHost `
        -AiArtifactDirectory $ai `
        -AiArtifactManifest (Join-Path $ai 'inference.json') `
        -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
        -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
        -AllowDirtySource -NoArchive

    foreach ($required in @(
            'D3D12LookDevPTwithAI.exe',
            'D3D12LookDevPTwithAI.ChatHost.exe',
            'hostfxr.dll', 'hostpolicy.dll', 'coreclr.dll',
            'Microsoft.ui.xaml.dll', 'Microsoft.WindowsAppRuntime.dll',
            'Microsoft.WindowsAppRuntime.Bootstrap.dll',
            'msvcp140.dll', 'msvcp140_atomic_wait.dll',
            'vcruntime140.dll', 'vcruntime140_1.dll', 'vcomp140.dll',
            'AI/inference.json', 'AI/Models/demo-model/demo.gguf',
            'AI/Runtimes/cpu/demo-runtime/llama-server.exe',
            'licenses/AI/model-model-terms.txt',
            'licenses/AI/runtime-runtime-license.txt',
            'licenses/Native/ASSIMP-EARCUT-HPP-LICENSE.txt',
            'licenses/Native/ASSIMP-M3D-LICENSE.txt',
            'licenses/Native/ASSIMP-MDC-PICOMODEL-LICENSE.txt',
            'licenses/Native/ASSIMP-LIBB64-PUBLIC-DOMAIN-NOTICE.txt',
            'licenses/Native/ASSIMP-STB-IMAGE-LICENSE.txt',
            'licenses/Native/TINYEXR-OPENJPH-BSD-2-CLAUSE.txt',
            'licenses/Native/VC-REDIST.txt',
            'LICENSE', 'THIRD-PARTY-NOTICES.txt',
            'REDISTRIBUTION-NOTES.txt', 'UNSIGNED-ARTIFACTS.ja.txt',
            'integrated-license-map.json',
            'integrated-portable-sbom.spdx.json',
            'integrated-portable-manifest.json')) {
        Assert-True (Test-Path -LiteralPath (Join-Path $output $required) -PathType Leaf) `
            "Required integrated payload file is missing: $required"
    }
    foreach ($forbidden in @(
            'LocalMCPChatClient', 'Launch-D3D12LookDevPTwithAI.ps1',
            'InstallWindowsAppRuntime.ps1', 'chat-history.sqlite3', 'settings.json')) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $output $forbidden))) `
            "Forbidden legacy/user payload was included: $forbidden"
    }
    $forbiddenFiles = @(Get-ChildItem -LiteralPath $output -File -Recurse -Force |
        Where-Object {
            $_.Extension -in @('.pdb', '.lib', '.exp') -or
            $_.Name -ieq 'D3D12SDKLayers.dll' -or
            $_.FullName -match '(?i)LocalMCPChatClient'
        })
    Assert-True ($forbiddenFiles.Count -eq 0) 'Forbidden files survived final payload validation.'

    $manifest = Get-Content -LiteralPath (
        Join-Path $output 'integrated-portable-manifest.json') -Raw | ConvertFrom-Json
    Assert-True ([int]$manifest.schemaVersion -eq 1) 'Outer manifest schema is incorrect.'
    Assert-True ([string]$manifest.packageKind -ceq 'integrated-one-app-portable') `
        'Outer manifest package kind is incorrect.'
    Assert-True ([bool]$manifest.ai.included) 'AI payload was not declared.'
    Assert-True ([bool]$manifest.ai.artifactLicensesAccepted) `
        'AI license acceptance was not recorded.'
    Assert-True ([bool]$manifest.ai.unsignedArtifactTrustAccepted) `
        'Unsigned trust acceptance was not recorded.'
    Assert-True (-not [bool]$manifest.ai.visionProjectorIncluded) `
        'Vision/mmproj must not be declared by the text+Tool pack.'
    Assert-True (-not [bool]$manifest.legacyLocalMcpChatClientIncluded) `
        'Legacy LocalMCPChatClient was declared.'
    Assert-True ([bool]$manifest.build.nativeWindowsAppSdkSelfContained) `
        'Native Windows App SDK self-contained status is false.'
    Assert-True ([bool]$manifest.build.chatHostSelfContained) `
        'ChatHost self-contained status is false.'
    Assert-True (-not [bool]$manifest.build.windowsAppRuntimeInstallerRequired) `
        'Portable manifest still requires a Windows App Runtime installer.'
    Assert-True ([string]$manifest.build.actualToolchain.dotnetSdk -ceq
        (& dotnet --version).Trim()) 'Actual dotnet SDK provenance is incorrect.'
    Assert-True (-not [bool]$manifest.build.reproducibleLockedRestore) `
        'Unlocked ambient restore was incorrectly declared reproducible.'
    Assert-True (-not [bool]$manifest.build.restore.packageFeedAndCacheProvenanceAuthenticated) `
        'Ambient package feed/cache provenance was incorrectly authenticated.'
    Assert-True (@($manifest.build.restore.resolvedNuGetPackages).Count -gt 0) `
        'Resolved NuGet identities and content-hash claims were not recorded.'
    $machineLearningPackages = @(
        $manifest.build.restore.resolvedNuGetPackages | Where-Object {
            [string]$_.id -ieq 'Microsoft.Windows.AI.MachineLearning'
        })
    Assert-True ($machineLearningPackages.Count -eq 1) `
        'Native project package fallback omitted Microsoft.Windows.AI.MachineLearning.'
    $machineLearningNotices = @(Get-ChildItem -LiteralPath (
            Join-Path $output 'licenses/Native/NuGet') -File -Force | Where-Object {
            $_.Name -match '(?i)windows\.ai\.machinelearning.*(?:license|notice)'
        })
    Assert-True ($machineLearningNotices.Count -ge 2) `
        'Microsoft.Windows.AI.MachineLearning license/notices are incomplete.'
    Assert-True ([bool]$manifest.trust.manualBuildMachineFeedsAndCachesIncludedInTrustBoundary) `
        'Manual build-machine feed/cache trust boundary was not recorded.'

    $actualPayloadFiles = @(Get-PayloadRelativeFiles $output)
    $longestPayloadRelativePath = ''
    foreach ($candidatePath in $actualPayloadFiles) {
        if ($candidatePath.Length -gt $longestPayloadRelativePath.Length -or
            ($candidatePath.Length -eq $longestPayloadRelativePath.Length -and
                [string]::CompareOrdinal(
                    $candidatePath, $longestPayloadRelativePath) -lt 0)) {
            $longestPayloadRelativePath = $candidatePath
        }
    }
    Assert-True ([int]$manifest.installationPathPolicy.maxFullPath -eq 240) `
        'Manifest full-path limit is incorrect.'
    Assert-True ([int]$manifest.installationPathPolicy.maxRelativePath -eq
        $longestPayloadRelativePath.Length) `
        'Manifest maximum relative-path length is incorrect.'
    Assert-True ([string]$manifest.installationPathPolicy.longestRelativePath -ceq
        $longestPayloadRelativePath) `
        'Manifest longest relative path is incorrect.'
    Assert-True ([int]$manifest.installationPathPolicy.maxInstallRootChars -eq
        240 - 1 - $longestPayloadRelativePath.Length) `
        'Manifest installation-root limit is incorrect.'
    $redistributionText = Get-Content -LiteralPath (
        Join-Path $output 'REDISTRIBUTION-NOTES.txt') -Raw
    foreach ($pathPolicyClaim in @(
            'maxFullPath=240',
            "maxRelativePath=$($longestPayloadRelativePath.Length)",
            "maxInstallRootChars=$($manifest.installationPathPolicy.maxInstallRootChars)")) {
        Assert-True ($redistributionText.Contains(
            $pathPolicyClaim, [StringComparison]::Ordinal)) `
            "Redistribution notes omit path policy claim: $pathPolicyClaim"
    }
    $manifestPayloadFiles = @($manifest.files | ForEach-Object { [string]$_.path } | Sort-Object)
    $expectedManifestFiles = @($actualPayloadFiles |
        Where-Object { $_ -cne 'integrated-portable-manifest.json' })
    Assert-ExactSet $manifestPayloadFiles $expectedManifestFiles `
        'Outer manifest exact payload inventory'
    foreach ($record in $manifest.files) {
        $path = Join-Path $output ([string]$record.path).Replace('/', '\')
        Assert-True ((Get-Item -LiteralPath $path).Length -eq [long]$record.size) `
            "Outer manifest size mismatch: $($record.path)"
        Assert-True ((Get-LowerSha256 $path) -ceq [string]$record.sha256) `
            "Outer manifest hash mismatch: $($record.path)"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $pathPolicyArchive = Join-Path $temporaryRoot 'path-policy-fixture.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $output, $pathPolicyArchive, [IO.Compression.CompressionLevel]::Optimal, $false)
    $installRootCharacters = [int]$manifest.installationPathPolicy.maxInstallRootChars
    $installRootNameCharacters = $installRootCharacters - $temporaryRoot.Length - 1
    Assert-True ($installRootNameCharacters -gt 0) `
        'Temporary root is too long for the maximum installation-root regression.'
    $pathPolicyExtractRoot = Join-Path $temporaryRoot ('e' * $installRootNameCharacters)
    Assert-True ($pathPolicyExtractRoot.Length -eq $installRootCharacters) `
        'Installation-root regression did not reach the declared maximum length.'
    [IO.Compression.ZipFile]::ExtractToDirectory(
        $pathPolicyArchive, $pathPolicyExtractRoot)
    $extractedManifest = Get-Content -LiteralPath (
        Join-Path $pathPolicyExtractRoot 'integrated-portable-manifest.json') -Raw |
        ConvertFrom-Json
    $extractedFiles = @(Get-PayloadRelativeFiles $pathPolicyExtractRoot)
    Assert-ExactSet $extractedFiles $actualPayloadFiles `
        'Maximum-root extracted payload inventory'
    foreach ($record in $extractedManifest.files) {
        $path = Join-Path $pathPolicyExtractRoot ([string]$record.path).Replace('/', '\')
        Assert-True ((Get-Item -LiteralPath $path).Length -eq [long]$record.size) `
            "Maximum-root extracted size mismatch: $($record.path)"
        Assert-True ((Get-LowerSha256 $path) -ceq [string]$record.sha256) `
            "Maximum-root extracted hash mismatch: $($record.path)"
    }
    $extractedFullPaths = @(Get-ChildItem -LiteralPath $pathPolicyExtractRoot `
        -File -Recurse -Force | ForEach-Object FullName)
    $maximumExtractedPath = @($extractedFullPaths | Sort-Object Length -Descending |
        Select-Object -First 1)[0]
    Assert-True ($maximumExtractedPath.Length -eq 240) `
        'Maximum-root extraction did not exercise the 240-character boundary.'
    Assert-True (@($extractedFullPaths | Where-Object Length -gt 240).Count -eq 0) `
        'Maximum-root extraction exceeded the declared full-path limit.'
    Assert-WindowsPowerShellDefaultStreams $pathPolicyExtractRoot

    $licenseMap = Get-Content -LiteralPath (
        Join-Path $output 'integrated-license-map.json') -Raw | ConvertFrom-Json
    $mapped = @($licenseMap.entries | ForEach-Object { [string]$_.path } | Sort-Object)
    $expectedMapped = @($actualPayloadFiles | Where-Object {
        $_ -cnotin @(
            'integrated-license-map.json',
            'integrated-portable-sbom.spdx.json',
            'integrated-portable-manifest.json')
    })
    Assert-ExactSet $mapped $expectedMapped 'License map coverage'

    $sbom = Get-Content -LiteralPath (
        Join-Path $output 'integrated-portable-sbom.spdx.json') -Raw | ConvertFrom-Json
    $sbomPaths = @($sbom.files | ForEach-Object {
        ([string]$_.fileName).Substring(2)
    } | Sort-Object)
    $expectedSbom = @($actualPayloadFiles | Where-Object {
        $_ -cnotin @(
            'integrated-portable-sbom.spdx.json',
            'integrated-portable-manifest.json')
    })
    Assert-ExactSet $sbomPaths $expectedSbom 'SPDX file coverage'
    $spdxIds = @($sbom.files | ForEach-Object { [string]$_.SPDXID })
    Assert-True (($spdxIds | Sort-Object -Unique).Count -eq $spdxIds.Count) `
        'SPDX file identifiers are not unique by path.'
    $packageIds = @($sbom.packages | ForEach-Object { [string]$_.SPDXID })
    foreach ($requiredPackage in @(
            'SPDXRef-Package', 'SPDXRef-AIModel', 'SPDXRef-AIRuntime')) {
        Assert-True ($packageIds -ccontains $requiredPackage) `
            "SPDX AI component package is missing: $requiredPackage"
    }
    $declaredLicenseRefs = @(
        @($sbom.files.licenseConcluded) + @($sbom.packages.licenseDeclared) |
            Where-Object { [string]$_ -cmatch '^LicenseRef-' } |
            ForEach-Object { [string]$_ } | Sort-Object -Unique)
    $extractedLicenseRefs = @($sbom.hasExtractedLicensingInfos |
        ForEach-Object { [string]$_.licenseId } | Sort-Object -Unique)
    foreach ($licenseRef in $declaredLicenseRefs) {
        Assert-True ($extractedLicenseRefs -ccontains $licenseRef) `
            "SPDX LicenseRef lacks extracted license text: $licenseRef"
    }
    Assert-True ($sbom.PSObject.Properties.Name -cnotcontains 'documentExcludes') `
        'SPDX document contains the nonstandard documentExcludes property.'

    $manifestHashPath = "$output.manifest.sha256"
    Assert-True (Test-Path -LiteralPath $manifestHashPath -PathType Leaf) `
        'Manifest digest was not produced.'
    $manifestHashLine = (Get-Content -LiteralPath $manifestHashPath -Raw).Trim()
    Assert-True ($manifestHashLine.EndsWith(
        '  integrated-portable/integrated-portable-manifest.json',
        [StringComparison]::Ordinal)) `
        'Manifest digest does not name the payload-relative verification path.'
    Assert-True (-not (Test-Path -LiteralPath "$output.zip")) `
        'Test-only prebuilt input unexpectedly produced a production archive.'

    $nativeExecutable = Join-Path $native 'D3D12LookDevPTwithAI.exe'
    $nativeExecutableBytes = [IO.File]::ReadAllBytes($nativeExecutable)
    Write-FixturePe $nativeExecutable 'missing-native-app-local.dll'
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'native-import-gap-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'native final payload import closure' `
        "import closure is missing 'missing-native-app-local.dll'"
    [IO.File]::WriteAllBytes($nativeExecutable, $nativeExecutableBytes)

    $chatExecutable = Join-Path $chatHost 'D3D12LookDevPTwithAI.ChatHost.exe'
    $chatExecutableBytes = [IO.File]::ReadAllBytes($chatExecutable)
    Write-FixturePe $chatExecutable 'missing-chat-app-local.dll'
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'chat-import-gap-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'ChatHost final payload import closure' `
        "import closure is missing 'missing-chat-app-local.dll'"
    [IO.File]::WriteAllBytes($chatExecutable, $chatExecutableBytes)

    $collisionPath = Join-Path $native 'Microsoft.Data.Sqlite.dll'
    Write-FixtureText $collisionPath 'native-collision-fixture'
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'collision-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'native/chat collision' 'Payload collision'
    [IO.File]::Delete($collisionPath)

    $chatDll = Join-Path $chatHost 'D3D12LookDevPTwithAI.ChatHost.dll'
    $chatDllBytes = [IO.File]::ReadAllBytes($chatDll)
    Write-FixtureText $chatDll 'D3D12LOOKDEVPT_AI_TEST_RUNTIME'
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'debug-hook-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi `
            -AllowDirtySource -NoArchive
    } 'Release Debug activation hook' 'Debug-only runtime activation hook'
    [IO.File]::WriteAllBytes($chatDll, $chatDllBytes)

    $sensitive = Join-Path $ai 'chat-history.sqlite3'
    Write-FixtureText $sensitive 'must-not-package'
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'sensitive-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest (Join-Path $ai 'inference.json') `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'conversation history exclusion' 'sensitive AI state'
    [IO.File]::Delete($sensitive)

    $model = Join-Path $ai 'Models/demo-model/demo.gguf'
    $modelBytes = [IO.File]::ReadAllBytes($model)
    [IO.File]::AppendAllText($model, 'tamper')
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'tampered-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest (Join-Path $ai 'inference.json') `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'AI artifact hash verification' 'size/hash mismatch'
    [IO.File]::WriteAllBytes($model, $modelBytes)

    $inferencePath = Join-Path $ai 'inference.json'
    $inferenceBytes = [IO.File]::ReadAllBytes($inferencePath)
    $inferenceText = [Text.Encoding]::UTF8.GetString($inferenceBytes)
    [IO.File]::WriteAllText($inferencePath, $inferenceText, [Text.Encoding]::Unicode)
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'utf16-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest $inferencePath `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'UTF-16 inference rejection' 'UTF-8 without a byte-order mark'
    [IO.File]::WriteAllBytes($inferencePath, $inferenceBytes)

    $wrongKindInference = $inferenceText | ConvertFrom-Json
    $wrongKindInference.modelId = 123
    [IO.File]::WriteAllText($inferencePath,
        ($wrongKindInference | ConvertTo-Json -Depth 7 -Compress),
        [Text.UTF8Encoding]::new($false))
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'json-kind-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest $inferencePath `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'modelId JSON kind rejection' 'identity is invalid'
    [IO.File]::WriteAllBytes($inferencePath, $inferenceBytes)

    $wrongCaseInference = $inferenceText | ConvertFrom-Json
    $wrongCaseInference.backend = 'CPU'
    [IO.File]::WriteAllText($inferencePath,
        ($wrongCaseInference | ConvertTo-Json -Depth 7 -Compress),
        [Text.UTF8Encoding]::new($false))
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'backend-case-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest $inferencePath `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'backend case-sensitive rejection' 'identity is invalid'
    [IO.File]::WriteAllBytes($inferencePath, $inferenceBytes)

    $backendPath = Join-Path $ai 'Runtimes/cpu/demo-runtime/backend.dll'
    $backendBytes = [IO.File]::ReadAllBytes($backendPath)
    Write-FixturePe $backendPath 'missing-runtime.dll'
    $missingImportInference = $inferenceText | ConvertFrom-Json
    $missingImportInference.runtimeDependencies[0].sha256 = Get-LowerSha256 $backendPath
    $missingImportInference.runtimeDependencies[0].expectedSize =
        [long](Get-Item -LiteralPath $backendPath).Length
    [IO.File]::WriteAllText($inferencePath,
        ($missingImportInference | ConvertTo-Json -Depth 7 -Compress),
        [Text.UTF8Encoding]::new($false))
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'missing-runtime-import-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest $inferencePath `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'AI runtime import closure' "import closure is missing 'missing-runtime.dll'"
    [IO.File]::WriteAllBytes($backendPath, $backendBytes)
    [IO.File]::WriteAllBytes($inferencePath, $inferenceBytes)

    # A sparse, logically large PE must be inspected by bounded random-access
    # reads.  The missing import is intentional; an old ReadAllBytes parser
    # would allocate the entire 256 MiB file before reaching this rejection.
    Write-FixturePe $backendPath 'missing-sparse-runtime.dll'
    $sparseStream = [IO.FileStream]::new(
        $backendPath, [IO.FileMode]::Open, [IO.FileAccess]::Write,
        [IO.FileShare]::None)
    try { $sparseStream.SetLength(256MB) }
    finally { $sparseStream.Dispose() }
    $sparseInference = $inferenceText | ConvertFrom-Json
    $sparseInference.runtimeDependencies[0].sha256 = Get-LowerSha256 $backendPath
    $sparseInference.runtimeDependencies[0].expectedSize =
        [long](Get-Item -LiteralPath $backendPath).Length
    [IO.File]::WriteAllText($inferencePath,
        ($sparseInference | ConvertTo-Json -Depth 7 -Compress),
        [Text.UTF8Encoding]::new($false))
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'sparse-pe-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest $inferencePath `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'bounded sparse PE import inspection' `
        "import closure is missing 'missing-sparse-runtime.dll'"
    [IO.File]::WriteAllBytes($backendPath, $backendBytes)
    [IO.File]::WriteAllBytes($inferencePath, $inferenceBytes)

    $nestedDependencyPath = Join-Path $ai `
        'Runtimes/cpu/demo-runtime/plugins/sibling.dll'
    Write-FixturePe $backendPath 'sibling.dll'
    Write-FixturePe $nestedDependencyPath
    $nestedPeInference = $inferenceText | ConvertFrom-Json
    $nestedPeInference.runtimeDependencies = @(
        [ordered]@{
            relativePath = 'cpu/demo-runtime/backend.dll'
            sha256 = Get-LowerSha256 $backendPath
            expectedSize = [long](Get-Item -LiteralPath $backendPath).Length
        },
        [ordered]@{
            relativePath = 'cpu/demo-runtime/plugins/sibling.dll'
            sha256 = Get-LowerSha256 $nestedDependencyPath
            expectedSize = [long](Get-Item -LiteralPath $nestedDependencyPath).Length
        })
    [IO.File]::WriteAllText($inferencePath,
        ($nestedPeInference | ConvertTo-Json -Depth 7 -Compress),
        [Text.UTF8Encoding]::new($false))
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'nested-runtime-pe-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest $inferencePath `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'nested PE loader lookup rejection' 'PE runtime dependencies must be adjacent'
    [IO.File]::WriteAllBytes($backendPath, $backendBytes)
    [IO.File]::Delete($nestedDependencyPath)
    [IO.Directory]::Delete((Split-Path -Parent $nestedDependencyPath))
    [IO.File]::WriteAllBytes($inferencePath, $inferenceBytes)

    $runtimePath = Join-Path $ai `
        'Runtimes/cpu/demo-runtime/llama-server.exe'
    $runtimeBytes = [IO.File]::ReadAllBytes($runtimePath)
    Write-FixturePe $runtimePath 'vulkan-1.dll'
    $vulkanInference = $inferenceText | ConvertFrom-Json
    $vulkanInference.backend = 'vulkan'
    $vulkanInference.runtime.sha256 = Get-LowerSha256 $runtimePath
    $vulkanInference.runtime.expectedSize =
        [long](Get-Item -LiteralPath $runtimePath).Length
    [IO.File]::WriteAllText($inferencePath,
        ($vulkanInference | ConvertTo-Json -Depth 7 -Compress),
        [Text.UTF8Encoding]::new($false))
    & $builder -OutputDirectory (Join-Path $temporaryRoot 'vulkan-driver-output') `
        -SkipBuild -NativeBuildDirectory $native `
        -ChatHostPublishDirectory $chatHost `
        -AiArtifactDirectory $ai `
        -AiArtifactManifest $inferencePath `
        -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
        -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
        -AllowDirtySource -NoArchive
    $cpuWithVulkanImport = $vulkanInference
    $cpuWithVulkanImport.backend = 'cpu'
    [IO.File]::WriteAllText($inferencePath,
        ($cpuWithVulkanImport | ConvertTo-Json -Depth 7 -Compress),
        [Text.UTF8Encoding]::new($false))
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'cpu-gpu-import-output') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost `
            -AiArtifactDirectory $ai `
            -AiArtifactManifest $inferencePath `
            -AiRedistributionManifest (Join-Path $ai 'redistribution.json') `
            -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust `
            -AllowDirtySource -NoArchive
    } 'backend-specific GPU driver allowlist' "import closure is missing 'vulkan-1.dll'"
    [IO.File]::WriteAllBytes($runtimePath, $runtimeBytes)
    [IO.File]::WriteAllBytes($inferencePath, $inferenceBytes)

    $suiteLockPath = Join-Path $fixtureRepository 'suite.lock.json'
    $suiteLockBytes = [IO.File]::ReadAllBytes($suiteLockPath)
    [IO.File]::AppendAllText($suiteLockPath, "`n")
    Assert-Throws {
        & $builder -OutputDirectory (Join-Path $temporaryRoot 'dirty-archive') `
            -SkipBuild -NativeBuildDirectory $native `
            -ChatHostPublishDirectory $chatHost -WithoutAi
    } 'prebuilt production archive rejection' 'SkipBuild is test-only'
    $dirtyOutput = Join-Path $temporaryRoot 'dirty-test-only'
    & $builder -OutputDirectory $dirtyOutput `
        -SkipBuild -NativeBuildDirectory $native `
        -ChatHostPublishDirectory $chatHost -WithoutAi `
        -AllowDirtySource -NoArchive
    $dirtyManifest = Get-Content -LiteralPath (
        Join-Path $dirtyOutput 'integrated-portable-manifest.json') -Raw | ConvertFrom-Json
    Assert-True ([string]$dirtyManifest.source.state -ceq 'dirty-test-only') `
        'Dirty test-only source state was not recorded.'
    Assert-True (-not (Test-Path -LiteralPath "$dirtyOutput.zip")) `
        'Dirty test-only output unexpectedly produced an archive.'
    [IO.File]::WriteAllBytes($suiteLockPath, $suiteLockBytes)
    Assert-True (@(& git -C $fixtureRepository status --porcelain `
        --untracked-files=all --ignore-submodules=none).Count -eq 0) `
        'Fixture repository did not return to clean state.'

    Write-Host 'Integrated portable tests passed'
}
finally {
    if (-not $temporaryRoot.StartsWith(
            $temporaryPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($temporaryRoot) -notlike 'ldpt-*') {
        throw "Refusing cleanup outside the integrated portable test root: $temporaryRoot"
    }
    if ([IO.Directory]::Exists($temporaryRoot)) {
        foreach ($item in Get-ChildItem -LiteralPath $temporaryRoot -Recurse -Force) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) {
                $item.Attributes = [IO.FileAttributes]::Normal
            }
        }
        [IO.Directory]::Delete($temporaryRoot, $true)
    }
}
