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
$directXTexRoot = Join-Path $repo 'ThirdParty\DirectXTex\DirectXTex'
$directXTexLib = Get-ChildItem -LiteralPath (Join-Path $directXTexRoot 'Bin') -Recurse -Filter DirectXTex.lib |
    Where-Object { $_.FullName -match '\\x64\\Release\\DirectXTex\.lib$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $directXTexLib) {
    throw 'A Release x64 DirectXTex.lib was not found. Build the main project first.'
}

$testRoot = [System.IO.Path]::GetFullPath((Join-Path $env:TEMP ("D3D12LookDevPT-TextureLoaderBuild-{0}" -f $PID)))
$tempRoot = [System.IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
if (-not $testRoot.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to use a test build directory outside the system temporary directory.'
}

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
    $exe = Join-Path $testRoot 'TextureLoaderTests.exe'
    $compileCpp = @(
        ('cd /d "{0}" && cl.exe /nologo /c /std:c++20 /EHsc /W4 /WX /MD /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS' -f $testRoot),
        '/Fd:"TextureLoaderTests.pdb"',
        ('/I"{0}\Source" /I"{1}" /I"{2}" /I"{3}\include"' -f $repo, $directXTexRoot, (Join-Path (Split-Path $directXTexRoot -Parent) 'Common'), $tinyRoot),
        ('"{0}\Tests\TextureLoaderTests.cpp"' -f $repo),
        ('"{0}\Source\TextureLoader.cpp"' -f $repo),
        ('"{0}\Source\TinyExrLoader.cpp"' -f $repo),
        ('/Fo"{0}\\"' -f $testRoot)
    ) -join ' '
    $link = ('cd /d "{0}" && link.exe /nologo *.obj /LIBPATH:"{1}" DirectXTex.lib windowscodecs.lib ole32.lib /OUT:"{2}"' -f $testRoot, $directXTexLib.DirectoryName, $exe)
    $compile = ('"{0}" -arch=x64 -host_arch=x64 >nul && {1} && {2} && {3}' -f $vcvars, $compileC, $compileCpp, $link)
    & cmd.exe /d /c $compile
    if ($LASTEXITCODE -ne 0) {
        throw "TextureLoaderTests compilation failed with exit code $LASTEXITCODE."
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        throw "TextureLoaderTests failed with exit code $LASTEXITCODE."
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
