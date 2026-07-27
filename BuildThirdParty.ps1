param(
    [Parameter(Mandatory = $true)][string]$AssimpRoot,
    [Parameter(Mandatory = $true)][string]$AssimpBuildRoot,
    [Parameter(Mandatory = $true)][string]$AssimpLibDir,
    [Parameter(Mandatory = $true)][string]$AssimpZlibDir,
    [Parameter(Mandatory = $true)][string]$AssimpLibName,
    [Parameter(Mandatory = $true)][string]$AssimpZlibName,
    [Parameter(Mandatory = $true)][string]$DirectXTexProject,
    [Parameter(Mandatory = $true)][string]$DirectXTexLibDir,
    [string]$NrdRoot = "",
    [string]$NrdBuildRoot = "",
    [string]$NrdLibDir = "",
    [string]$NrdLibName = "NRD.lib",
    [string]$EnableNRD = "0",
    [string]$RtxdiRuntimeRoot = "",
    [string]$RtxdiBuildRoot = "",
    [string]$RtxdiLibDir = "",
    [string]$RtxdiLibName = "Rtxdi.lib",
    [string]$EnableRTXDI = "0",
    [Parameter(Mandatory = $true)][string]$MSBuildPath,
    [Parameter(Mandatory = $true)][string]$Configuration,
    [string]$VisualStudioVersion = ''
)

$ErrorActionPreference = 'Stop'

function Get-CMakeExecutable {
    $msbuildDirectory = Split-Path -Parent $MSBuildPath
    $visualStudioCMake = [System.IO.Path]::GetFullPath(
        (Join-Path $msbuildDirectory '..\..\..\..\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
    if (Test-Path -LiteralPath $visualStudioCMake -PathType Leaf) {
        return $visualStudioCMake
    }

    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    throw 'CMake was not found. Install the Visual Studio C++ CMake tools component.'
}

function Invoke-WithMutex {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][scriptblock]$Body
    )

    $mutex = [System.Threading.Mutex]::new($false, $Name)
    try {
        [void]$mutex.WaitOne()
        & $Body
    }
    finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
    }
}

function Copy-FirstMatch {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string[]]$Patterns,
        [Parameter(Mandatory = $true)][string]$DestinationName
    )

    foreach ($pattern in $Patterns) {
        $candidate = Get-ChildItem -Path $Directory -Filter $pattern -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate) {
            $destination = Join-Path $Directory $DestinationName
            Copy-Item -LiteralPath $candidate.FullName -Destination $destination -Force
            return
        }
    }
    throw "No library matching '$($Patterns -join ', ')' was found in $Directory."
}

function Get-CMakeGeneratorArgs {
    $help = & $CMakeExe --help 2>$null
    if ($VisualStudioVersion -like '18.*' -and $help -match 'Visual Studio 18 2026') {
        return @('-G', 'Visual Studio 18 2026', '-A', 'x64')
    }
    if ($VisualStudioVersion -like '17.*' -and $help -match 'Visual Studio 17 2022') {
        return @('-G', 'Visual Studio 17 2022', '-A', 'x64')
    }
    if ($help -match 'Visual Studio 18 2026') {
        return @('-G', 'Visual Studio 18 2026', '-A', 'x64')
    }
    return @('-G', 'Visual Studio 17 2022', '-A', 'x64')
}

function Get-NormalizedPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]@('\', '/'))
}

function Test-CMakeCacheMatchesSource {
    param(
        [Parameter(Mandatory = $true)][string]$CachePath,
        [Parameter(Mandatory = $true)][string]$SourceRoot
    )

    if (!(Test-Path -LiteralPath $CachePath)) {
        return $true
    }

    $prefix = 'CMAKE_HOME_DIRECTORY:INTERNAL='
    $line = Get-Content -LiteralPath $CachePath | Where-Object { $_.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) } | Select-Object -First 1
    if (!$line) {
        return $false
    }

    $cachedSource = $line.Substring($prefix.Length)
    return (Get-NormalizedPath $cachedSource) -ieq (Get-NormalizedPath $SourceRoot)
}

function Reset-AssimpBuildRootIfStale {
    param(
        [Parameter(Mandatory = $true)][string]$BuildRoot,
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$CachePath
    )

    if (Test-CMakeCacheMatchesSource -CachePath $CachePath -SourceRoot $SourceRoot) {
        return
    }

    $normalizedBuildRoot = Get-NormalizedPath $BuildRoot
    $normalizedSourceRoot = Get-NormalizedPath $SourceRoot
    $comparison = [System.StringComparison]::OrdinalIgnoreCase
    if (!$normalizedBuildRoot.StartsWith($normalizedSourceRoot + [System.IO.Path]::DirectorySeparatorChar, $comparison)) {
        throw "Refusing to delete stale Assimp build root outside the Assimp source tree: $normalizedBuildRoot"
    }

    Remove-Item -LiteralPath $normalizedBuildRoot -Recurse -Force
}

$CMakeExe = Get-CMakeExecutable
$cmakeGeneratorArgs = Get-CMakeGeneratorArgs

Invoke-WithMutex -Name "Local\D3D12LookDevPTWinUI-DirectXTex-$Configuration" -Body {
    $directXTexLib = Join-Path $DirectXTexLibDir 'DirectXTex.lib'
    if (!(Test-Path $directXTexLib)) {
        & $MSBuildPath $DirectXTexProject /p:Platform=x64 /p:Configuration=$Configuration /v:minimal
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
}

Invoke-WithMutex -Name "Local\D3D12LookDevPTWinUI-Assimp-$Configuration" -Body {
    $cachePath = Join-Path $AssimpBuildRoot 'CMakeCache.txt'
    $solutionPath = Join-Path $AssimpBuildRoot 'Assimp.sln'
    $assimpNormalized = Join-Path $AssimpLibDir $AssimpLibName
    $zlibNormalized = Join-Path $AssimpZlibDir $AssimpZlibName

    Reset-AssimpBuildRootIfStale -BuildRoot $AssimpBuildRoot -SourceRoot $AssimpRoot -CachePath $cachePath

    if (!(Test-Path $cachePath) -or !(Test-Path $solutionPath)) {
        & $CMakeExe -S $AssimpRoot -B $AssimpBuildRoot @cmakeGeneratorArgs `
            -DASSIMP_BUILD_TESTS=OFF `
            -DASSIMP_BUILD_ASSIMP_TOOLS=OFF `
            -DBUILD_SHARED_LIBS=OFF `
            -DASSIMP_INSTALL=OFF `
            -DASSIMP_WARNINGS_AS_ERRORS=OFF
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }

    if (!(Test-Path $assimpNormalized) -or !(Test-Path $zlibNormalized)) {
        & $CMakeExe --build $AssimpBuildRoot --config $Configuration --target assimp --parallel
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        New-Item -ItemType Directory -Force -Path $AssimpLibDir | Out-Null
        New-Item -ItemType Directory -Force -Path $AssimpZlibDir | Out-Null
        if ($Configuration -eq 'Debug') {
            Copy-FirstMatch -Directory $AssimpLibDir -Patterns @('assimp-vc*-mtd.lib', 'assimp*.lib') -DestinationName $AssimpLibName
            Copy-FirstMatch -Directory $AssimpZlibDir -Patterns @('zlibstaticd.lib', 'zlib*.lib') -DestinationName $AssimpZlibName
        }
        else {
            Copy-FirstMatch -Directory $AssimpLibDir -Patterns @('assimp-vc*-mt.lib', 'assimp*.lib') -DestinationName $AssimpLibName
            Copy-FirstMatch -Directory $AssimpZlibDir -Patterns @('zlibstatic.lib', 'zlib*.lib') -DestinationName $AssimpZlibName
        }
    }
}

$buildNrd = $EnableNRD -eq "1" -or $EnableNRD -ieq "true"
if ($buildNrd) {
    Invoke-WithMutex -Name "Local\D3D12LookDevPTWinUI-NRD-$Configuration" -Body {
        if (!$NrdRoot -or !(Test-Path -LiteralPath (Join-Path $NrdRoot 'Include\NRD.h') -PathType Leaf)) {
            throw "NRD is enabled, but ThirdParty\NRD\Include\NRD.h was not found. Run git submodule update --init --recursive or build with /p:EnableNRD=false."
        }
        if (!$NrdBuildRoot -or !$NrdLibDir) {
            throw "NRD build paths were not provided."
        }

        $nrdLib = Join-Path $NrdLibDir $NrdLibName
        $cachePath = Join-Path $NrdBuildRoot 'CMakeCache.txt'
        if (!(Test-Path -LiteralPath $cachePath)) {
            New-Item -ItemType Directory -Force -Path $NrdBuildRoot | Out-Null
            New-Item -ItemType Directory -Force -Path $NrdLibDir | Out-Null
            & $CMakeExe -S $NrdRoot -B $NrdBuildRoot @cmakeGeneratorArgs `
                -DNRD_STATIC_LIBRARY=ON `
                -DNRD_NRI=OFF `
                -DNRD_EMBEDS_DXIL_SHADERS=ON `
                -DNRD_EMBEDS_DXBC_SHADERS=OFF `
                -DNRD_EMBEDS_SPIRV_SHADERS=OFF `
                -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$NrdLibDir" `
                -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG="$NrdLibDir" `
                -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE="$NrdLibDir"
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }

        if (!(Test-Path -LiteralPath $nrdLib -PathType Leaf)) {
            & $CMakeExe --build $NrdBuildRoot --config $Configuration --target NRD --parallel
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
    }
}

$buildRtxdi = $EnableRTXDI -eq "1" -or $EnableRTXDI -ieq "true"
if ($buildRtxdi) {
    Invoke-WithMutex -Name "Local\D3D12LookDevPTWinUI-RTXDI-$Configuration" -Body {
        $rtxdiHeader = Join-Path $RtxdiRuntimeRoot 'Include\Rtxdi\RtxdiParameters.h'
        $rtxdiSource = Join-Path $RtxdiRuntimeRoot 'Source\ReSTIRDI.cpp'
        $rtxdiCmake = Join-Path $RtxdiRuntimeRoot 'CMakeLists.txt'
        if (!$RtxdiRuntimeRoot -or !(Test-Path -LiteralPath $rtxdiHeader -PathType Leaf) -or
            !(Test-Path -LiteralPath $rtxdiSource -PathType Leaf) -or !(Test-Path -LiteralPath $rtxdiCmake -PathType Leaf)) {
            throw "RTXDI build was enabled without the pinned v3.0.0 runtime sources under ThirdParty\RTXDI\Libraries\Rtxdi."
        }
        if (!$RtxdiBuildRoot -or !$RtxdiLibDir) {
            throw "RTXDI build paths were not provided."
        }

        $rtxdiLib = Join-Path $RtxdiLibDir $RtxdiLibName
        # Libraries/Rtxdi is designed to be included by RTXDI's top-level
        # project and intentionally has no project()/cmake_minimum_required().
        # Keep the pinned submodule untouched and generate a tiny standalone
        # wrapper inside the ignored build tree.
        $rtxdiWrapperRoot = Join-Path $RtxdiBuildRoot '_standalone'
        $rtxdiWrapperCmake = Join-Path $rtxdiWrapperRoot 'CMakeLists.txt'
        New-Item -ItemType Directory -Force -Path $rtxdiWrapperRoot | Out-Null
        $rtxdiSourceForCmake = $RtxdiRuntimeRoot.Replace('\', '/')
        $wrapper = @"
cmake_minimum_required(VERSION 3.24)
project(D3D12LookDevPTWinUI_RTXDI LANGUAGES CXX)
add_subdirectory("$rtxdiSourceForCmake" rtxdi-runtime)
"@
        if (!(Test-Path -LiteralPath $rtxdiWrapperCmake) -or
            (Get-Content -LiteralPath $rtxdiWrapperCmake -Raw) -ne $wrapper) {
            Set-Content -LiteralPath $rtxdiWrapperCmake -Value $wrapper -Encoding UTF8 -NoNewline
        }
        $cachePath = Join-Path $RtxdiBuildRoot 'CMakeCache.txt'
        if ((Test-Path -LiteralPath $cachePath) -and !(Test-CMakeCacheMatchesSource -CachePath $cachePath -SourceRoot $rtxdiWrapperRoot)) {
            throw "RTXDI CMake cache points to a different source tree. Remove '$RtxdiBuildRoot' and rebuild."
        }
        if (!(Test-Path -LiteralPath $cachePath)) {
            New-Item -ItemType Directory -Force -Path $RtxdiBuildRoot | Out-Null
            New-Item -ItemType Directory -Force -Path $RtxdiLibDir | Out-Null
            & $CMakeExe -S $rtxdiWrapperRoot -B $RtxdiBuildRoot @cmakeGeneratorArgs -Wno-dev `
                -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$RtxdiLibDir" `
                -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG="$RtxdiLibDir" `
                -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE="$RtxdiLibDir"
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }

        if (!(Test-Path -LiteralPath $rtxdiLib -PathType Leaf)) {
            & $CMakeExe --build $RtxdiBuildRoot --config $Configuration --target Rtxdi --parallel
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
    }
}
