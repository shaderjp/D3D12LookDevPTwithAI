[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'A Visual Studio C++ toolchain was not found.' }
$vcvars = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPT-GltfImporterBuild-{0}" -f $PID)))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $exe = Join-Path $testRoot 'GltfSceneImporterTests.exe'
    $cl = @(
        ('cd /d "{0}" && cl.exe /nologo /std:c++20 /EHsc /W4 /WX /MD /DNOMINMAX /DUNICODE /D_UNICODE' -f $testRoot),
        ('/I"{0}\Source" /I"{0}\ThirdParty\tinygltf"' -f $repo),
        ('"{0}\Tests\GltfSceneImporterTests.cpp"' -f $repo),
        ('"{0}\Source\GltfSceneImporter.cpp"' -f $repo),
        ('/link bcrypt.lib shell32.lib ole32.lib /OUT:"{0}"' -f $exe)
    ) -join ' '
    $compile = ('"{0}" -arch=x64 -host_arch=x64 >nul && {1}' -f $vcvars, $cl)
    & cmd.exe /d /c $compile
    if ($LASTEXITCODE -ne 0) { throw "glTF importer test compilation failed with exit code $LASTEXITCODE." }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "glTF importer tests failed with exit code $LASTEXITCODE." }
}
finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
