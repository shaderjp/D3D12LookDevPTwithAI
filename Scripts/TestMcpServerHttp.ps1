[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer vswhere.exe was not found.'
}
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw 'A Visual Studio C++ toolchain was not found.'
}
$vcvars = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPTWinUI-McpServerHttpBuild-{0}" -f $PID)))
$tempRoot = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use a test build directory outside the system temporary directory.'
}

New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $exe = Join-Path $testRoot 'McpServerHttpTests.exe'
    $cl = @(
        'cl.exe /nologo /std:c++20 /EHsc /W4 /WX /DNOMINMAX /DWIN32_LEAN_AND_MEAN',
        ('/Fo:"{0}\\" /Fd:"{0}\McpServerHttpTests.pdb"' -f $testRoot),
        ('/I"{0}\Source" /I"{0}\ThirdParty\DirectXTex\Common"' -f $repo),
        ('"{0}\Tests\McpServerHttpTests.cpp"' -f $repo),
        ('"{0}\Source\McpServer.cpp"' -f $repo),
        ('"{0}\Source\SimpleJson.cpp"' -f $repo),
        ('/link ws2_32.lib /OUT:"{0}"' -f $exe)
    ) -join ' '
    $compile = ('"{0}" -arch=x64 -host_arch=x64 >nul && {1}' -f $vcvars, $cl)
    & cmd.exe /d /c $compile
    if ($LASTEXITCODE -ne 0) {
        throw "McpServerHttpTests compilation failed with exit code $LASTEXITCODE."
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        throw "McpServerHttpTests failed with exit code $LASTEXITCODE."
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
