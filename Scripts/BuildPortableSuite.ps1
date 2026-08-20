[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$LocalMcpRepository,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$lock = Get-Content -LiteralPath (Join-Path $repo 'suite.lock.json') -Raw | ConvertFrom-Json
if ($lock.schemaVersion -ne 2 -or $lock.repositories.D3D12LookDevPTWinUI.source -ne 'self') {
    throw 'suite.lock.json schema v2 is required.'
}
if ([string]::IsNullOrWhiteSpace($LocalMcpRepository)) {
    $LocalMcpRepository = [System.IO.Path]::GetFullPath((Join-Path $repo $lock.repositories.LocalMCPChatClient.relativePath))
}
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output" }
$d3dCommit = (& git -C $repo rev-parse HEAD).Trim()
$localCommit = (& git -C $LocalMcpRepository rev-parse HEAD).Trim()
if ($d3dCommit -notmatch '^[0-9a-f]{40}$' -or $localCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'Source commit hashes could not be determined.'
}
if ($localCommit -ne [string]$lock.repositories.LocalMCPChatClient.commit) {
    throw "LocalMCPChatClient is at $localCommit; suite.lock.json expects $($lock.repositories.LocalMCPChatClient.commit)."
}
foreach ($source in @(@{ Name = 'D3D12LookDevPTWinUI'; Path = $repo }, @{ Name = 'LocalMCPChatClient'; Path = $LocalMcpRepository })) {
    $trackedChanges = @(& git -C $source.Path status --porcelain --untracked-files=no)
    if ($trackedChanges.Count -ne 0) {
        throw "$($source.Name) has uncommitted tracked changes. Refusing to create a manifest with an inaccurate source commit."
    }
}
$d3dOutput = Join-Path $output 'D3D12LookDevPTWinUI'
$chatOutput = Join-Path $output 'LocalMCPChatClient'
New-Item -ItemType Directory -Path $d3dOutput, $chatOutput | Out-Null

if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $repo 'D3D12LookDevPTWinUI.vcxproj') /t:Build /p:Configuration=Release /p:Platform=x64 /p:WindowsAppSDKSelfContained=true /p:UseHybridCRT=true /p:EnableDLSS=false /p:EnableNRD=true /p:EnableRTXDI=false /m
    if ($LASTEXITCODE -ne 0) { throw 'Portable D3D12 build failed.' }
    & dotnet publish (Join-Path $LocalMcpRepository 'src\LocalMCPChatClient.App\LocalMCPChatClient.App.csproj') -c Release -r win-x64 --self-contained true -o $chatOutput
    if ($LASTEXITCODE -ne 0) { throw 'Portable LocalMCPChatClient publish failed.' }
}

Get-ChildItem -LiteralPath (Join-Path $repo 'Bin\x64\Release') | Copy-Item -Destination $d3dOutput -Recurse -Force
# Keep the standard beta payload deterministic even when the local Release
# directory contains symbols or optional-backend files from an earlier build.
Get-ChildItem -LiteralPath $d3dOutput -File -Recurse | Where-Object {
    $_.Extension -in @('.pdb', '.lib', '.exp') -or
    $_.FullName.StartsWith((Join-Path $d3dOutput 'Streamline') + '\', [StringComparison]::OrdinalIgnoreCase) -or
    $_.Name -match '^(nvngx|sl\.|rtxdi)'
} | Remove-Item -Force
Get-ChildItem -LiteralPath $chatOutput -File -Recurse -Filter '*.pdb' | Remove-Item -Force
foreach ($name in @('LICENSE', 'THIRD-PARTY-NOTICES.txt', 'suite.lock.json')) {
    $source = Join-Path $repo $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $output }
}
if (Test-Path -LiteralPath (Join-Path $repo 'licenses')) { Copy-Item -LiteralPath (Join-Path $repo 'licenses') -Destination $output -Recurse }
$d3dLicenseOutput = Join-Path $output 'licenses\D3D12LookDevPTWinUI'
New-Item -ItemType Directory -Path $d3dLicenseOutput -Force | Out-Null
$nugetRoot = Join-Path $env:USERPROFILE '.nuget\packages'
$licenseSources = [ordered]@{
    'ASSIMP-LICENSE.txt' = (Join-Path $repo 'ThirdParty\assimp\LICENSE')
    'ASSIMP-ZLIB-LICENSE.txt' = (Join-Path $repo 'ThirdParty\assimp\contrib\zlib\LICENSE')
    'DIRECTXTEX-LICENSE.txt' = (Join-Path $repo 'ThirdParty\DirectXTex\LICENSE')
    'NRD-LICENSE.txt' = (Join-Path $repo 'ThirdParty\NRD\LICENSE.txt')
    'SHADERMAKE-LICENSE.txt' = (Join-Path $repo 'ThirdParty\NRD\Build\v145\x64\Release\_deps\shadermake-src\LICENSE.txt')
    'MATHLIB-LICENSE.txt' = (Join-Path $repo 'ThirdParty\NRD\Build\v145\x64\Release\_deps\mathlib-src\LICENSE.txt')
    'TINYEXR-LICENSE.txt' = (Join-Path $repo 'ThirdParty\tinyexr\LICENSE')
    'TINYEXR-NOTICE.txt' = (Join-Path $repo 'ThirdParty\tinyexr\NOTICE')
    'TINYEXR-ZSTD-LICENSE.txt' = (Join-Path $repo 'ThirdParty\tinyexr\deps\zstd\LICENSE')
    'WINDOWS-APP-SDK-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk\2.3.1\license.txt')
    'WINDOWS-APP-SDK-NOTICE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk\2.3.1\NOTICE.txt')
    'WINDOWS-APP-SDK-FOUNDATION-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.foundation\2.3.5\license.txt')
    'WINDOWS-APP-SDK-INTERACTIVE-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.interactiveexperiences\2.1.3\license.txt')
    'WINDOWS-APP-SDK-WINUI-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.winui\2.3.0\license.txt')
    'WINDOWS-APP-SDK-WINUI-NOTICE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.winui\2.3.0\NOTICE.txt')
    'WINDOWS-APP-SDK-AI-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.ai\2.3.4\license.txt')
    'WINDOWS-APP-SDK-ML-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.ml\2.1.74\license.txt')
    'WINDOWS-APP-SDK-ML-NOTICE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.ml\2.1.74\ThirdPartyNotices.txt')
    'WINDOWS-APP-SDK-DWRITE-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.windowsappsdk.dwrite\2.1.0\license.txt')
    'WEBVIEW2-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.web.webview2\1.0.3719.77\LICENSE.txt')
    'WEBVIEW2-NOTICE.txt' = (Join-Path $nugetRoot 'microsoft.web.webview2\1.0.3719.77\NOTICE.txt')
    'D3D12-AGILITY-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.direct3d.d3d12\1.619.3\LICENSE.txt')
    'D3D12-AGILITY-CODE-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.direct3d.d3d12\1.619.3\LICENSE-CODE.txt')
    'DXC-LLVM-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.direct3d.dxc\1.9.2602.17\LICENSE-LLVM.txt')
    'DXC-MICROSOFT-LICENSE.txt' = (Join-Path $nugetRoot 'microsoft.direct3d.dxc\1.9.2602.17\LICENSE-MS.txt')
}
foreach ($entry in $licenseSources.GetEnumerator()) {
    if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { throw "Required redistribution license is missing: $($entry.Value)" }
    Copy-Item -LiteralPath $entry.Value -Destination (Join-Path $d3dLicenseOutput $entry.Key)
}
@'
D3D12 LookDev Suite - Third-Party Notices

This distribution includes D3D12LookDevPTWinUI and LocalMCPChatClient plus
redistributable runtime components and statically linked libraries. The
corresponding license and notice texts are included below this suite root:

- licenses/D3D12LookDevPTWinUI: Assimp, zlib, DirectXTex, NVIDIA NRD,
  ShaderMake, MathLib, TinyEXR, zstd, Windows App SDK, WebView2,
  D3D12 Agility SDK, and DirectX Shader Compiler.
- LocalMCPChatClient/licenses and LocalMCPChatClient/THIRD-PARTY-NOTICES.txt:
  .NET and MCP client dependencies used by LocalMCPChatClient.

DLSS and RTXDI runtime binaries are not included in the standard beta suite.
'@ | Set-Content -LiteralPath (Join-Path $output 'THIRD-PARTY-NOTICES.txt') -Encoding utf8NoBOM
foreach ($script in @('InstallPortableSuite.ps1','TestPortablePrerequisites.ps1','UninstallPortableSuite.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repo "Scripts\$script") -Destination $output
}

@"
D3D12 LookDev Suite $($lock.suiteVersion) - 署名なし公開ベータ

このベータの実行ファイルにはコード署名がありません。Windows SmartScreenが警告を表示する場合があります。
GitHubのshaderjp/D3D12LookDevPTWinUI Releaseから取得し、同梱のSHA-256と照合してください。
モデルとvision projectorは同梱せず、LocalMCPChatClientが初回セットアップでSHA-256検証付きで取得します。

既知の制限:
- decoded PNG/JPEG/TGA/BMPおよびHDRIは最大辺512 pixelです。
- KTX/Basis Universal、汎用EXR texture経路、DXR非対応GPU向けraster fallbackは未対応です。
"@ | Set-Content -LiteralPath (Join-Path $output 'UNSIGNED-BETA.ja.txt') -Encoding utf8NoBOM

$launcher = @'
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$firstRunMarker = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'D3D12LookDevSuite\first-run-v1'
if (-not (Test-Path -LiteralPath $firstRunMarker)) {
    $diagnosticPath = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'D3D12LookDevSuite\diagnostics-latest.json'
    $diagnostics = & (Join-Path $root 'TestPortablePrerequisites.ps1') -OutputJson $diagnosticPath -PassThru
    if (-not $diagnostics.Passed) {
        $diagnostics | Format-List
        throw 'Portable suite prerequisites were not met.'
    }
    $diagnostics | Format-List
    New-Item -ItemType Directory -Path (Split-Path -Parent $firstRunMarker) -Force | Out-Null
    Set-Content -LiteralPath $firstRunMarker -Value ([DateTimeOffset]::UtcNow.ToString('O')) -Encoding ascii
    Write-Host 'First-run diagnostics passed. D3D12LookDevPTWinUI will perform the authoritative DXR check.'
}
$d3d = Start-Process -FilePath (Join-Path $root 'D3D12LookDevPTWinUI\D3D12LookDevPTWinUI.exe') -ArgumentList @('--mcp-server','--mcp-pair') -PassThru
Start-Sleep -Milliseconds 1200
$chatEnvironment = [Environment]::GetEnvironmentVariable('LOCAL_MCP_CHAT_SUITE_FIRST_RUN', 'Process')
$chatAddressEnvironment = [Environment]::GetEnvironmentVariable('LOCAL_MCP_CHAT_LOOKDEV_ADDRESS', 'Process')
[Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_SUITE_FIRST_RUN', '1', 'Process')
[Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_LOOKDEV_ADDRESS', 'http://127.0.0.1:8777', 'Process')
try {
    Start-Process -FilePath (Join-Path $root 'LocalMCPChatClient\LocalMCPChatClient.exe')
}
finally {
    [Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_SUITE_FIRST_RUN', $chatEnvironment, 'Process')
    [Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_LOOKDEV_ADDRESS', $chatAddressEnvironment, 'Process')
}
Write-Host "D3D12 LookDev started as PID $($d3d.Id). Enter the 8-digit code shown in its MCP panel into the LocalMCPChatClient setup window."
'@
Set-Content -LiteralPath (Join-Path $output 'Launch-LookDevSuite.ps1') -Value $launcher -Encoding utf8NoBOM

$files = Get-ChildItem -LiteralPath $output -File -Recurse | Where-Object Name -ne 'suite-manifest.json' | Sort-Object FullName
$manifest = [ordered]@{
    schemaVersion = 2
    suiteVersion = $lock.suiteVersion
    createdAtUtc = [DateTimeOffset]::UtcNow.ToString('O')
    platform = 'windows-11-x64'
    codeSigned = $false
    mcpContractVersion = '1.0'
    mcpProtocolVersion = '2026-07-28'
    components = @(
        @{ name = 'D3D12LookDevPTWinUI'; version = $lock.suiteVersion; commit = $d3dCommit },
        @{ name = 'LocalMCPChatClient'; version = $lock.suiteVersion; commit = $localCommit }
    )
    backends = @{ dlss = $false; nrd = $true; rtxdi = $false }
    files = @($files | ForEach-Object {
        @{ path = [System.IO.Path]::GetRelativePath($output, $_.FullName).Replace('\','/'); size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'suite-manifest.json') -Encoding utf8NoBOM
$zip = "$output.zip"
Compress-Archive -Path (Join-Path $output '*') -DestinationPath $zip -CompressionLevel Optimal
Set-Content -LiteralPath "$zip.sha256" -Value "$( (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() )  $([IO.Path]::GetFileName($zip))" -Encoding ascii
Write-Host "Portable suite: $zip"
