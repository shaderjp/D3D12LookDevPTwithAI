param(
    [string]$PackageRunner = '',
    [int]$Port = 18776,
    [string]$Scenario = '',
    [ValidateSet('active', 'all', 'draft', 'pending')]
    [string]$Suite = 'all',
    [string]$ExpectedFailures = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found.'
}
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw 'A Visual Studio C++ toolchain was not found.'
}
$vcvars = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPT-McpConformance-{0}" -f $PID)))
$tempRoot = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use a conformance directory outside the system temporary directory.'
}
if ($Port -lt 1 -or $Port -gt 65535) {
    throw 'Port must be between 1 and 65535.'
}
if (-not $ExpectedFailures) {
    $ExpectedFailures = Join-Path $repo 'Tests\McpConformanceExpectedFailures.yml'
}
$ExpectedFailures = [System.IO.Path]::GetFullPath($ExpectedFailures)
if (-not (Test-Path -LiteralPath $ExpectedFailures)) {
    throw "Expected-failures baseline was not found: $ExpectedFailures"
}

if (-not $PackageRunner) {
    $runnerCommand = Get-Command pnpm -ErrorAction SilentlyContinue
    if (-not $runnerCommand) {
        $runnerCommand = Get-Command npx -ErrorAction SilentlyContinue
    }
    if (-not $runnerCommand) {
        throw 'pnpm or npx is required for the official MCP conformance suite. Pass -PackageRunner with its full path.'
    }
    $PackageRunner = $runnerCommand.Source
}
$PackageRunner = [System.IO.Path]::GetFullPath($PackageRunner)
if (-not (Test-Path -LiteralPath $PackageRunner)) {
    throw "Package runner was not found: $PackageRunner"
}

$d3dInclude = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.direct3d.d3d12\1.619.3\build\native\include'
$d3dxInclude = Join-Path $d3dInclude 'd3dx12'
if (-not (Test-Path -LiteralPath (Join-Path $d3dxInclude 'd3dx12.h'))) {
    throw 'The Microsoft.Direct3D.D3D12 NuGet headers are missing. Restore packages first.'
}

New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$hostProcess = $null
$completed = $false
try {
    $exe = Join-Path $testRoot 'McpConformanceHost.exe'
    $cl = @(
        'cl.exe /nologo /std:c++20 /EHsc /W4 /WX /DWIN32 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS',
        ('/Fo:"{0}\\" /Fd:"{0}\McpConformanceHost.pdb"' -f $testRoot),
        ('/I"{0}\Source"' -f $repo),
        ('/I"{0}" /I"{1}"' -f $d3dInclude, $d3dxInclude),
        ('"{0}\Tests\McpServerTests.cpp"' -f $repo),
        ('"{0}\Source\McpServer.cpp"' -f $repo),
        ('"{0}\Source\SimpleJson.cpp"' -f $repo),
        ('/Fe:"{0}" ws2_32.lib' -f $exe)
    ) -join ' '
    $compile = ('"{0}" -arch=x64 -host_arch=x64 >nul && {1}' -f $vcvars, $cl)
    & cmd.exe /d /c $compile
    if ($LASTEXITCODE -ne 0) {
        throw "MCP conformance host compilation failed with exit code $LASTEXITCODE."
    }

    $stdoutPath = Join-Path $testRoot 'host.stdout.txt'
    $stderrPath = Join-Path $testRoot 'host.stderr.txt'
    $hostProcess = Start-Process -FilePath $exe -ArgumentList @('--serve', $Port) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $ready = $false
    for ($attempt = 0; $attempt -lt 50; ++$attempt) {
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $client.Connect('127.0.0.1', $Port)
            $client.Dispose()
            $ready = $true
            break
        }
        catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $ready) {
        $stderrText = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath -Raw } else { '' }
        throw "MCP conformance host did not start. $stderrText"
    }

    $package = '@modelcontextprotocol/conformance@0.2.0-alpha.10'
    $runnerName = [System.IO.Path]::GetFileNameWithoutExtension($PackageRunner).ToLowerInvariant()
    $arguments = @()
    if ($runnerName -eq 'pnpm') {
        $arguments += @('dlx', $package)
    }
    else {
        $arguments += @('-y', $package)
    }
    $arguments += @('server', '--url', "http://127.0.0.1:$Port/mcp")
    if ($Scenario) {
        $arguments += @('--scenario', $Scenario, '--verbose')
    }
    else {
        $arguments += @('--suite', $Suite)
    }
    $arguments += @('--spec-version', '2026-07-28', '--expected-failures', $ExpectedFailures, '--output-dir', (Join-Path $testRoot 'results'))
    & $PackageRunner @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "MCP conformance suite failed with exit code $LASTEXITCODE. Results were preserved at $testRoot."
    }
    $completed = $true
}
finally {
    if ($hostProcess -and -not $hostProcess.HasExited) {
        Stop-Process -Id $hostProcess.Id -Force
        $hostProcess.WaitForExit()
    }
    if ($completed -and (Test-Path -LiteralPath $testRoot)) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
