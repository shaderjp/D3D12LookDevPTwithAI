param(
    [string]$LocalMcpChatClientRoot = '',
    [int]$Port = 18777,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $LocalMcpChatClientRoot) {
    $LocalMcpChatClientRoot = Join-Path (Split-Path -Parent $repo) 'LocalMCPChatClient'
}
$LocalMcpChatClientRoot = [System.IO.Path]::GetFullPath($LocalMcpChatClientRoot)
$testProject = Join-Path $LocalMcpChatClientRoot 'tests\LocalMCPChatClient.Tests\LocalMCPChatClient.Tests.csproj'
if (-not (Test-Path -LiteralPath $testProject)) {
    throw "LocalMCPChatClient test project was not found: $testProject"
}
if ($Port -lt 1 -or $Port -gt 65535) {
    throw 'Port must be between 1 and 65535.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found.'
}
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw 'A Visual Studio C++ toolchain was not found.'
}
$vcvars = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPT-LocalMcpChatClient-{0}" -f $PID)))
$tempRoot = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use a test build directory outside the system temporary directory.'
}

$d3dInclude = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.direct3d.d3d12\1.619.3\build\native\include'
$d3dxInclude = Join-Path $d3dInclude 'd3dx12'
if (-not (Test-Path -LiteralPath (Join-Path $d3dxInclude 'd3dx12.h'))) {
    throw 'The Microsoft.Direct3D.D3D12 NuGet headers are missing. Restore packages first.'
}

$hostProcess = $null
$previousEndpoint = [Environment]::GetEnvironmentVariable('LOCAL_MCP_CHAT_EXTERNAL_MCP_URL', 'Process')
$previousToken = [Environment]::GetEnvironmentVariable('LOCAL_MCP_CHAT_EXTERNAL_MCP_TOKEN', 'Process')
$integrationToken = 'local-client-integration-token'
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $exe = Join-Path $testRoot 'McpLocalClientHost.exe'
    $cl = @(
        'cl.exe /nologo /std:c++20 /EHsc /W4 /WX /DWIN32 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS',
        ('/Fo:"{0}\\" /Fd:"{0}\McpLocalClientHost.pdb"' -f $testRoot),
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
        throw "MCP integration host compilation failed with exit code $LASTEXITCODE."
    }

    $stdoutPath = Join-Path $testRoot 'host.stdout.txt'
    $stderrPath = Join-Path $testRoot 'host.stderr.txt'
    $hostProcess = Start-Process -FilePath $exe -ArgumentList @('--serve-auth', $Port, $integrationToken) -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
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
        throw "MCP integration host did not start. $stderrText"
    }

    [Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_EXTERNAL_MCP_URL', "http://127.0.0.1:$Port/mcp", 'Process')
    [Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_EXTERNAL_MCP_TOKEN', $integrationToken, 'Process')
    & dotnet test $testProject -c $Configuration --filter 'FullyQualifiedName=LocalMCPChatClient.Tests.McpIntegrationTests.External_d3d12lookdevpt_connects_lists_and_calls_with_negotiated_protocol' --logger 'console;verbosity=normal'
    if ($LASTEXITCODE -ne 0) {
        throw "LocalMCPChatClient integration test failed with exit code $LASTEXITCODE."
    }
}
finally {
    [Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_EXTERNAL_MCP_URL', $previousEndpoint, 'Process')
    [Environment]::SetEnvironmentVariable('LOCAL_MCP_CHAT_EXTERNAL_MCP_TOKEN', $previousToken, 'Process')
    if ($hostProcess -and -not $hostProcess.HasExited) {
        Stop-Process -Id $hostProcess.Id -Force
        $hostProcess.WaitForExit()
    }
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
