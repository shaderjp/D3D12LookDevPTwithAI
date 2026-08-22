[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dataEnvironmentName = 'D3D12LOOKDEVPT_AI_DATA_DIRECTORY'
$originalDataDirectory = [Environment]::GetEnvironmentVariable(
    $dataEnvironmentName,
    [EnvironmentVariableTarget]::Process)
$tempBase = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPTwithAI-AssistantHostBridge-{0}" -f [Guid]::NewGuid().ToString('N'))))
if (-not $testRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use an E2E directory outside the system temporary directory.'
}

$testProcess = $null
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $dataRoot = [System.IO.Path]::GetFullPath((Join-Path $testRoot 'data'))
    New-Item -ItemType Directory -Force -Path $dataRoot | Out-Null
    [Environment]::SetEnvironmentVariable(
        $dataEnvironmentName,
        $dataRoot,
        [EnvironmentVariableTarget]::Process)

    $hostProject = Join-Path $repo 'Managed\D3D12LookDevPTwithAI.ChatHost\D3D12LookDevPTwithAI.ChatHost.csproj'
    & dotnet build $hostProject -c Debug -p:Platform=x64 --nologo
    if ($LASTEXITCODE -ne 0) {
        throw 'Debug x64 ChatHost build failed.'
    }
    $chatHost = [System.IO.Path]::GetFullPath((Join-Path $repo 'Bin\x64\Debug\D3D12LookDevPTwithAI.ChatHost.exe'))
    if (-not (Test-Path -LiteralPath $chatHost -PathType Leaf)) {
        throw 'The Debug x64 ChatHost executable was not produced.'
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio Installer vswhere.exe was not found.'
    }
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) {
        throw 'A Visual Studio C++ x64 toolchain was not found.'
    }
    $vcvars = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
        throw 'Visual Studio developer command setup was not found.'
    }

    $testExe = Join-Path $testRoot 'AssistantHostBridgeTests.exe'
    $compileArguments = @(
        'cl.exe /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /MD /DWIN32 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN',
        ('/Fo:"{0}\\" /Fd:"{0}\AssistantHostBridgeTests.pdb"' -f $testRoot),
        ('/I"{0}\Source"' -f $repo),
        ('"{0}\Tests\AssistantHostBridgeTests.cpp"' -f $repo),
        ('"{0}\Source\Services\AssistantHostBridge.cpp"' -f $repo),
        ('"{0}\Source\Services\AssistantProtocol.cpp"' -f $repo),
        ('"{0}\Source\SimpleJson.cpp"' -f $repo),
        ('/Fe:"{0}" advapi32.lib ole32.lib' -f $testExe)
    ) -join ' '
    $compile = ('"{0}" -arch=x64 -host_arch=x64 -vcvars_ver=14.5 >nul && {1}' -f $vcvars, $compileArguments)
    & cmd.exe /d /c $compile
    if ($LASTEXITCODE -ne 0) {
        throw 'AssistantHostBridge E2E compilation with MSVC v145 failed.'
    }

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $testExe
    $startInfo.Arguments = '"{0}"' -f $chatHost
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
    $testProcess = [System.Diagnostics.Process]::new()
    $testProcess.StartInfo = $startInfo
    if (-not $testProcess.Start()) {
        throw 'AssistantHostBridge E2E process could not be started.'
    }
    if (-not $testProcess.WaitForExit(75000)) {
        Stop-Process -Id $testProcess.Id -Force
        $testProcess.WaitForExit()
        throw 'AssistantHostBridge E2E exceeded its 75 second process timeout.'
    }
    $testProcess.WaitForExit()
    $testExitCode = $testProcess.ExitCode
    if ($testExitCode -ne 0) {
        throw "AssistantHostBridge E2E failed with exit code $testExitCode."
    }

    $databasePath = [System.IO.Path]::GetFullPath((Join-Path $dataRoot 'chat-history.sqlite3'))
    if (-not $databasePath.StartsWith(
            $dataRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The expected E2E database path escaped its isolated data directory.'
    }
    if (-not (Test-Path -LiteralPath $databasePath -PathType Leaf) -or
        (Get-Item -LiteralPath $databasePath).Length -le 0) {
        throw 'ChatHost did not create a nonempty SQLite database in the isolated data directory.'
    }
    $databaseBytes = [System.IO.File]::ReadAllBytes($databasePath)
    if ($databaseBytes.Length -lt 16 -or
        [System.Text.Encoding]::ASCII.GetString($databaseBytes, 0, 16) -ne "SQLite format 3`0") {
        throw 'The isolated chat history file is not a SQLite database.'
    }

    Write-Host 'AssistantHostBridge E2E smoke passed.'
}
finally {
    [Environment]::SetEnvironmentVariable(
        $dataEnvironmentName,
        $originalDataDirectory,
        [EnvironmentVariableTarget]::Process)
    if ($testProcess -and -not $testProcess.HasExited) {
        Stop-Process -Id $testProcess.Id -Force
        $testProcess.WaitForExit()
    }
    if (Test-Path -LiteralPath $testRoot) {
        $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
        if (-not $resolvedTestRoot.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Refusing to clean an E2E directory outside the system temporary directory.'
        }
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
    }
}
