[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'A Visual Studio C++ toolchain was not found.' }
$vcvars = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPT-TinyExrBuild-{0}" -f $PID)))
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
try {
    $tinyRoot = Join-Path $repo 'ThirdParty\tinyexr'
    $coreSources = Get-ChildItem -LiteralPath (Join-Path $tinyRoot 'src') -Filter '*.c' |
        Where-Object { $_.Name -notin @('exr_freestanding.c', 'exr_spectral.c', 'exr_gpu_cuda.c', 'exr_vk_vulkan.c') } |
        ForEach-Object { '"{0}"' -f $_.FullName }
    $coreSources += ('"{0}"' -f (Join-Path $tinyRoot 'deps\zstd\tinyexr_zstd.c'))
    $compileC = @(
        ('cd /d "{0}" && cl.exe /nologo /c /TC /W3 /MD /utf-8 /D_Atomic= /D_CRT_SECURE_NO_WARNINGS' -f $testRoot),
        ('/I"{0}\include" /I"{0}\src" /I"{0}\deps\zstd"' -f $tinyRoot),
        ($coreSources -join ' '),
        ('/Fo"{0}\\"' -f $testRoot)
    ) -join ' '
    $compileCpp = @(
        ('cd /d "{0}" && cl.exe /nologo /c /std:c++20 /EHsc /W4 /WX /MD /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS' -f $testRoot),
        ('/I"{0}\Source" /I"{0}\ThirdParty\tinyexr\include"' -f $repo),
        ('"{0}\Tests\TinyExrLoaderTests.cpp" "{0}\Source\TinyExrLoader.cpp"' -f $repo),
        ('/Fo"{0}\\"' -f $testRoot)
    ) -join ' '
    $exe = Join-Path $testRoot 'TinyExrLoaderTests.exe'
    $link = ('cd /d "{0}" && link.exe /nologo *.obj /OUT:"{1}"' -f $testRoot, $exe)
    $command = ('"{0}" -arch=x64 -host_arch=x64 >nul && {1} && {2} && {3}' -f $vcvars, $compileC, $compileCpp, $link)
    & cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) { throw "TinyEXR loader test compilation failed with exit code $LASTEXITCODE." }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "TinyEXR loader tests failed with exit code $LASTEXITCODE." }
}
finally {
    if (Test-Path -LiteralPath $testRoot) { Remove-Item -LiteralPath $testRoot -Recurse -Force }
}
