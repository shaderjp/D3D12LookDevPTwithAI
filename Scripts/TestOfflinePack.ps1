$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$testRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ('lookdev-offline-tests-' + [Guid]::NewGuid().ToString('N'))))
$tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe offline test root.' }
function Assert-Throws([scriptblock]$Operation, [string]$Name) {
    try { & $Operation; throw "Expected failure was not raised: $Name" }
    catch { if ($_.Exception.Message -eq "Expected failure was not raised: $Name") { throw } }
}

[IO.Directory]::CreateDirectory($testRoot) | Out-Null
try {
    $suite = Join-Path $testRoot 'suite'
    $runtime = Join-Path $testRoot 'runtime'
    [IO.Directory]::CreateDirectory($suite) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $runtime 'lib')) | Out-Null
    Set-Content -LiteralPath (Join-Path $suite 'suite-manifest.json') -Value '{"schemaVersion":1}' -Encoding utf8NoBOM
    Set-Content -LiteralPath (Join-Path $suite 'Launch-D3D12LookDevPTwithAI.ps1') -Value '# fixture' -Encoding utf8NoBOM
    $model = Join-Path $testRoot 'fixture.gguf'
    Set-Content -LiteralPath $model -Value 'fixture-model' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $runtime 'llama-server.exe') -Value 'fixture-runtime' -Encoding ascii
    Set-Content -LiteralPath (Join-Path $runtime 'lib\backend.dll') -Value 'fixture-library' -Encoding ascii
    $output = Join-Path $testRoot 'offline'
    & (Join-Path $repo 'Scripts\BuildOfflinePack.ps1') -PortableSuiteDirectory $suite -OutputDirectory $output -Runtime cpu -Artifacts @($model,$runtime)
    if (-not (Test-Path -LiteralPath "$output.zip" -PathType Leaf)) { throw 'Offline ZIP was not created.' }
    Assert-Throws { & (Join-Path $output 'InstallOfflinePack.ps1') -PackDirectory $output -LocalMcpDataDirectory (Join-Path $testRoot 'data-no-license') } 'license confirmation'
    $data = Join-Path $testRoot 'data'
    & (Join-Path $output 'InstallOfflinePack.ps1') -PackDirectory $output -AcceptLicenses -ForceRuntime -LocalMcpDataDirectory $data
    if (-not (Test-Path -LiteralPath (Join-Path $data 'Models\fixture.gguf') -PathType Leaf)) { throw 'Offline model was not installed.' }
    if (-not (Test-Path -LiteralPath (Join-Path $data 'Runtimes\cpu\OfflinePack\runtime\llama-server.exe') -PathType Leaf)) { throw 'Offline runtime tree was not installed.' }
    $sensitive = Join-Path $testRoot 'settings.json'
    Set-Content -LiteralPath $sensitive -Value '{"token":"must-not-pack"}' -Encoding utf8NoBOM
    Assert-Throws { & (Join-Path $repo 'Scripts\BuildOfflinePack.ps1') -PortableSuiteDirectory $suite -OutputDirectory (Join-Path $testRoot 'sensitive') -Artifacts @($sensitive) } 'settings exclusion'
    Write-Host 'Offline pack tests passed'
}
finally {
    if (-not $testRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($testRoot) -notlike 'lookdev-offline-tests-*') {
        throw "Refusing cleanup outside the offline test root: $testRoot"
    }
    if ([IO.Directory]::Exists($testRoot)) { [IO.Directory]::Delete($testRoot, $true) }
}
