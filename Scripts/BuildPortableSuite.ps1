[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$LocalMcpRepository,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$lock = Get-Content -LiteralPath (Join-Path $repo 'suite.lock.json') -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($LocalMcpRepository)) {
    $LocalMcpRepository = [System.IO.Path]::GetFullPath((Join-Path $repo $lock.repositories.LocalMCPChatClient.relativePath))
}
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output" }
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
foreach ($name in @('LICENSE', 'THIRD-PARTY-NOTICES.txt', 'suite.lock.json')) {
    $source = Join-Path $repo $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $output }
}
if (Test-Path -LiteralPath (Join-Path $repo 'licenses')) { Copy-Item -LiteralPath (Join-Path $repo 'licenses') -Destination $output -Recurse }
foreach ($script in @('InstallPortableSuite.ps1','TestPortablePrerequisites.ps1','UninstallPortableSuite.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repo "Scripts\$script") -Destination $output
}

$launcher = @'
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$firstRunMarker = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'D3D12LookDevSuite\first-run-v1'
if (-not (Test-Path -LiteralPath $firstRunMarker)) {
    $diagnostics = & (Join-Path $root 'TestPortablePrerequisites.ps1') -PassThru
    if (-not $diagnostics.Passed) {
        $diagnostics | Format-List
        throw 'Portable suite prerequisites were not met.'
    }
    $diagnostics | Format-List
    New-Item -ItemType Directory -Path (Split-Path -Parent $firstRunMarker) -Force | Out-Null
    Set-Content -LiteralPath $firstRunMarker -Value ([DateTimeOffset]::UtcNow.ToString('O')) -Encoding ascii
    Write-Host 'First-run diagnostics passed. D3D12LookDevPTWinUI will perform the authoritative DXR check.'
}
$d3d = Start-Process -FilePath (Join-Path $root 'D3D12LookDevPTWinUI\D3D12LookDevPTWinUI.exe') -PassThru
Start-Sleep -Milliseconds 1200
Start-Process -FilePath (Join-Path $root 'LocalMCPChatClient\LocalMCPChatClient.exe')
Write-Host "D3D12 LookDev started as PID $($d3d.Id). Complete pairing from the two application panels."
'@
Set-Content -LiteralPath (Join-Path $output 'Launch-LookDevSuite.ps1') -Value $launcher -Encoding utf8NoBOM

$files = Get-ChildItem -LiteralPath $output -File -Recurse | Where-Object Name -ne 'suite-manifest.json' | Sort-Object FullName
$manifest = [ordered]@{
    schemaVersion = 1
    suiteVersion = $lock.suiteVersion
    createdAtUtc = [DateTimeOffset]::UtcNow.ToString('O')
    platform = 'windows-11-x64'
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
