[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $PSScriptRoot 'NvidiaDependencyTools.psm1') -Force

function Assert-True([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

$manifest = Import-NvidiaDependencyManifest -Path (Join-Path $repositoryRoot 'config\nvidia-dependencies.json')
Assert-True ($manifest.components.Count -eq 4) 'Expected four NVIDIA dependency components.'
$default = Get-NvidiaProfile -Manifest $manifest -Name RepositoryDefault
Assert-True ([bool]$default.nrd -and ![bool]$default.dlss -and ![bool]$default.rtxdi) 'RepositoryDefault must match project defaults.'
$defaultRoots = Get-NvidiaComponentRoots -Manifest $manifest -RepositoryRoot $repositoryRoot
$properties = @(Get-NvidiaMsBuildProperties -Manifest $manifest -Profile $default -Roots $defaultRoots)
Assert-True ($properties -contains '/p:EnableDLSS=false') 'The DLSS MSBuild property did not preserve false.'
Assert-True ($properties -contains '/p:EnableNRD=true') 'The NRD MSBuild property did not preserve true.'
Assert-True ($properties -contains '/p:EnableRTXDI=false') 'The RTXDI MSBuild property did not preserve false.'
$releaseProfile = Get-NvidiaProfile -Manifest $manifest -Name Release
$releaseProperties = @(Get-NvidiaMsBuildProperties -Manifest $manifest -Profile $releaseProfile -Roots $defaultRoots)
Assert-True ($releaseProperties -contains '/p:WindowsAppSDKSelfContained=true') 'The Release profile must make Windows App SDK self-contained.'

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('D3D12LookDevPT-NvidiaTests-' + [Guid]::NewGuid().ToString('N'))
$oldNgx = [Environment]::GetEnvironmentVariable([string]$manifest.ngxApplicationIdEnvironmentVariable)
try {
    $maliciousManifestPath = Join-Path $tempRoot 'malicious-manifest.json'
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    $maliciousJson = ($manifest | ConvertTo-Json -Depth 10).Replace('Streamline/sl.interposer.dll', '../escape.dll')
    [IO.File]::WriteAllText($maliciousManifestPath, $maliciousJson)
    $rejectedTraversal = $false
    try { Import-NvidiaDependencyManifest -Path $maliciousManifestPath | Out-Null } catch { $rejectedTraversal = $true }
    Assert-True $rejectedTraversal 'Manifest path traversal was not rejected.'

    $roots = @{}
    foreach ($component in $manifest.components) {
        $root = Join-Path $tempRoot ([string]$component.id)
        [IO.Directory]::CreateDirectory($root) | Out-Null
        $roots[[string]$component.id] = $root
        foreach ($relative in @($component.developmentFiles) + @($component.licenseFiles)) {
            $path = Join-Path $root ([string]$relative)
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllText($path, 'fixture')
        }
        foreach ($template in @($component.buildOutputs)) {
            $path = Join-Path $root (([string]$template).Replace('{configuration}', 'Release'))
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllText($path, 'fixture')
        }
        foreach ($runtime in @($component.runtimeFiles)) {
            if (![bool]$runtime.required) { continue }
            $path = Join-Path $root ([string]$runtime.path)
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllText($path, 'fixture')
        }
    }
    [Environment]::SetEnvironmentVariable([string]$manifest.ngxApplicationIdEnvironmentVariable, '123456')
    $profile = $releaseProfile
    $report = Get-NvidiaDependencyReport -Manifest $manifest -Profile $profile -Roots $roots -Configuration Release -Phase Complete -LicenseAccepted
    Assert-True ($report.Failures -eq 0) 'A complete fixture should pass release validation.'
    $json = $report | ConvertTo-Json -Depth 8
    Assert-True ($json -notmatch '123456') 'The NGX application ID value leaked into the report.'

    $stage = Join-Path $tempRoot 'stage'
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    $published = @(Publish-NvidiaRuntime -Manifest $manifest -Profile $profile -Roots $roots -DestinationRoot $stage)
    Assert-True ($published.Count -eq 6) 'Expected two required DLSS runtimes and four NVIDIA license records.'
    Assert-True (Test-Path -LiteralPath (Join-Path $stage 'Streamline\nvngx_dlss.dll')) 'DLSS runtime was not staged.'
    Assert-True (Test-Path -LiteralPath (Join-Path $stage 'Licenses\NVIDIA\rtxdi-LICENSE.txt')) 'RTXDI license was not staged.'
} finally {
    [Environment]::SetEnvironmentVariable([string]$manifest.ngxApplicationIdEnvironmentVariable, $oldNgx)
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

Write-Host 'NVIDIA dependency tool tests passed.' -ForegroundColor Green
