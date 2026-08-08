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
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPT-McpServerBuild-{0}" -f $PID)))
$tempRoot = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use a test build directory outside the system temporary directory.'
}

$d3dInclude = Join-Path $env:USERPROFILE '.nuget\packages\microsoft.direct3d.d3d12\1.619.3\build\native\include'
$d3dxInclude = Join-Path $d3dInclude 'd3dx12'
if (-not (Test-Path -LiteralPath (Join-Path $d3dxInclude 'd3dx12.h'))) {
    throw 'The Microsoft.Direct3D.D3D12 NuGet headers are missing. Restore packages first.'
}

New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $exe = Join-Path $testRoot 'McpServerTests.exe'
    $cl = @(
        'cl.exe /nologo /std:c++20 /EHsc /W4 /WX /DWIN32 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS',
        ('/Fo:"{0}\\" /Fd:"{0}\McpServerTests.pdb"' -f $testRoot),
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
        throw "McpServerTests compilation failed with exit code $LASTEXITCODE."
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        throw "McpServerTests failed with exit code $LASTEXITCODE."
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
