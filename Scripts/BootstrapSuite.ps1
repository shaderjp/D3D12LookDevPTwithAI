[CmdletBinding()]
param(
    [string]$LocalMcpRepository,
    [switch]$InstallPrerequisites,
    [switch]$SkipBuild,
    [switch]$EnforceLock
)

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$lockPath = Join-Path $repo 'suite.lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($lock.schemaVersion -ne 2 -or $lock.repositories.D3D12LookDevPTwithAI.source -ne 'self') {
    throw 'suite.lock.json schema v2 with D3D12LookDevPTwithAI.source=self is required.'
}
$localCommit = [string]$lock.repositories.LocalMCPChatClient.commit
if ($localCommit -notmatch '^[0-9a-f]{40}$') { throw 'LocalMCPChatClient must be pinned to a full 40-character commit hash.' }

if ($InstallPrerequisites) {
    $winget = Get-Command winget.exe -ErrorAction Stop
    & $winget.Source configure --file (Join-Path $repo 'config\development.dsc.yaml') --accept-configuration-agreements --disable-interactivity
    if ($LASTEXITCODE -ne 0) { throw "WinGet Configuration failed with exit code $LASTEXITCODE." }
}

foreach ($command in @('git.exe', 'dotnet.exe')) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "$command is required. Re-run with -InstallPrerequisites to install system prerequisites explicitly."
    }
}

if ([string]::IsNullOrWhiteSpace($LocalMcpRepository)) {
    $LocalMcpRepository = [System.IO.Path]::GetFullPath((Join-Path $repo $lock.repositories.LocalMCPChatClient.relativePath))
}
if (-not (Test-Path -LiteralPath (Join-Path $LocalMcpRepository '.git'))) {
    $parent = Split-Path -Parent $LocalMcpRepository
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
    & git clone https://github.com/shaderjp/LocalMCPChatClient.git $LocalMcpRepository
    if ($LASTEXITCODE -ne 0) { throw 'LocalMCPChatClient clone failed.' }
    & git -C $LocalMcpRepository checkout --detach $localCommit
    if ($LASTEXITCODE -ne 0) { throw "Could not checkout locked LocalMCPChatClient commit $localCommit." }
}

function Test-LockedCommit([string]$Path, [string]$Expected, [string]$Name) {
    $actual = (& git -C $Path rev-parse HEAD).Trim()
    if ($actual -ne $Expected) {
        $message = "$Name is at $actual; suite.lock.json expects $Expected."
        if ($EnforceLock) { throw $message }
        Write-Warning $message
    }
}
$d3dCommit = (& git -C $repo rev-parse HEAD).Trim()
if ($d3dCommit -notmatch '^[0-9a-f]{40}$') { throw 'D3D12LookDevPTwithAI source commit could not be determined.' }
Test-LockedCommit $LocalMcpRepository $localCommit 'LocalMCPChatClient'
if ($EnforceLock) {
    foreach ($source in @(@{ Name = 'D3D12LookDevPTwithAI'; Path = $repo }, @{ Name = 'LocalMCPChatClient'; Path = $LocalMcpRepository })) {
        $trackedChanges = @(& git -C $source.Path status --porcelain --untracked-files=no)
        if ($trackedChanges.Count -ne 0) { throw "$($source.Name) has uncommitted tracked changes." }
    }
}

& git -C $repo submodule update --init
if ($LASTEXITCODE -ne 0) { throw 'D3D12 submodule restore failed.' }
foreach ($dependency in @(
    @{ Name = 'tinygltf'; Path = (Join-Path $repo 'ThirdParty\tinygltf'); Commit = [string]$lock.dependencies.tinygltf.commit },
    @{ Name = 'Basis Universal'; Path = (Join-Path $repo 'ThirdParty\basis_universal'); Commit = [string]$lock.dependencies.basisUniversal.commit }
)) {
    if ($dependency.Commit -notmatch '^[0-9a-f]{40}$') { throw "$($dependency.Name) lock is missing or invalid." }
    Test-LockedCommit $dependency.Path $dependency.Commit $dependency.Name
}
& dotnet restore (Join-Path $LocalMcpRepository 'LocalMCPChatClient.sln')
if ($LASTEXITCODE -ne 0) { throw 'LocalMCPChatClient restore failed.' }

if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2026 is required. Apply .vsconfig after installation.' }
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $repo 'D3D12LookDevPTwithAI.vcxproj') /restore /t:Build /p:Configuration=Debug /p:Platform=x64 /m
    if ($LASTEXITCODE -ne 0) { throw 'D3D12 Debug|x64 build failed.' }
    & dotnet test (Join-Path $LocalMcpRepository 'LocalMCPChatClient.sln') -c Debug --no-restore
    if ($LASTEXITCODE -ne 0) { throw 'LocalMCPChatClient tests failed.' }
    & (Join-Path $repo 'Scripts\TestMcpServer.ps1')
    & (Join-Path $repo 'Scripts\TestLocalMcpChatClientIntegration.ps1') -LocalMcpChatClientRoot $LocalMcpRepository
}

Write-Host 'D3D12 LookDev suite environment is ready.'
