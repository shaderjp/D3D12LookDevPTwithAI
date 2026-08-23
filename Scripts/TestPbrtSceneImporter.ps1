[CmdletBinding()]
param(
    [Parameter()]
    [string[]]$ScenePath = @()
)

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'A Visual Studio C++ toolchain was not found.' }
$vcvars = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$assimpBuild = Get-ChildItem -LiteralPath (Join-Path $repo 'ThirdParty\assimp\Build') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'x64\Release\lib\Release\assimp-lookdevpt.lib') } |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $assimpBuild) { throw 'A Release x64 assimp-lookdevpt.lib was not found. Build the main project first.' }

$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPT-PbrtImporterBuild-{0}" -f $PID)))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $assimpRoot = Join-Path $repo 'ThirdParty\assimp'
    $assimpBuildRoot = Join-Path $assimpBuild.FullName 'x64\Release'
    $exe = Join-Path $testRoot 'PbrtSceneImporterTests.exe'
    $cl = @(
        ('cd /d "{0}" && cl.exe /nologo /std:c++20 /EHsc /W4 /WX /MD /DNOMINMAX' -f $testRoot),
        ('/I"{0}\Source" /I"{1}\include" /I"{2}\include"' -f $repo, $assimpRoot, $assimpBuildRoot),
        ('"{0}\Tests\PbrtSceneImporterTests.cpp"' -f $repo),
        ('"{0}\Source\PbrtSceneImporter.cpp"' -f $repo),
        ('/link /LIBPATH:"{0}\lib\Release" /LIBPATH:"{0}\contrib\zlib\Release" assimp-lookdevpt.lib zlib-lookdevpt.lib /OUT:"{1}"' -f $assimpBuildRoot, $exe)
    ) -join ' '
    $compile = ('"{0}" -arch=x64 -host_arch=x64 >nul && {1}' -f $vcvars, $cl)
    & cmd.exe /d /c $compile
    if ($LASTEXITCODE -ne 0) { throw "PBRT importer test compilation failed with exit code $LASTEXITCODE." }
    $resolvedScenePaths = @($ScenePath | ForEach-Object {
        [System.IO.Path]::GetFullPath($_)
    })
    & $exe @resolvedScenePaths
    if ($LASTEXITCODE -ne 0) { throw "PBRT importer tests failed with exit code $LASTEXITCODE." }
}
finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
