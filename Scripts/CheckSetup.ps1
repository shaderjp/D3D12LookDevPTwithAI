[CmdletBinding()]
param(
    [string]$Root = "",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [ValidateSet("x64")]
    [string]$Platform = "x64",
    [switch]$CheckAssets,
    [switch]$CheckDLSS,
    [switch]$CheckNRD,
    [switch]$CheckRTXDI,
    [switch]$Json
)

$ErrorActionPreference = "Stop"

if (-not $Root) {
    $scriptRoot = $PSScriptRoot
    if (-not $scriptRoot) {
        $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    $Root = (Resolve-Path (Join-Path $scriptRoot "..")).Path
}

$results = New-Object System.Collections.Generic.List[object]

function Add-Result {
    param(
        [ValidateSet("OK", "WARN", "FAIL")]
        [string]$Status,
        [string]$Check,
        [string]$Message,
        [string]$Fix = ""
    )

    $results.Add([pscustomobject]@{
        Status = $Status
        Check = $Check
        Message = $Message
        Fix = $Fix
    })
}

function Test-File {
    param(
        [string]$RelativePath,
        [string]$Check,
        [string]$Fix,
        [switch]$WarnOnly
    )

    $path = Join-Path $Root $RelativePath
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Add-Result -Status "OK" -Check $Check -Message "$RelativePath found."
        return
    }

    if ($WarnOnly) {
        Add-Result -Status "WARN" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    } else {
        Add-Result -Status "FAIL" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    }
}

function Test-Directory {
    param(
        [string]$RelativePath,
        [string]$Check,
        [string]$Fix,
        [switch]$WarnOnly
    )

    $path = Join-Path $Root $RelativePath
    if (Test-Path -LiteralPath $path -PathType Container) {
        Add-Result -Status "OK" -Check $Check -Message "$RelativePath found."
        return
    }

    if ($WarnOnly) {
        Add-Result -Status "WARN" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    } else {
        Add-Result -Status "FAIL" -Check $Check -Message "$RelativePath is missing." -Fix $Fix
    }
}

function Test-AnyFile {
    param(
        [string[]]$RelativePaths,
        [string]$Check,
        [string]$Fix,
        [switch]$WarnOnly
    )

    foreach ($relativePath in $RelativePaths) {
        $path = Join-Path $Root $relativePath
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            Add-Result -Status "OK" -Check $Check -Message "$relativePath found."
            return
        }
    }

    $message = "$($RelativePaths -join ' or ') is missing."
    if ($WarnOnly) {
        Add-Result -Status "WARN" -Check $Check -Message $message -Fix $Fix
    } else {
        Add-Result -Status "FAIL" -Check $Check -Message $message -Fix $Fix
    }
}

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $found = & $vswhere -latest -prerelease -version '[18.0,19.0)' -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($found) {
            return $found
        }
    }

    return ""
}

try {
    $Root = (Resolve-Path -LiteralPath $Root).Path
} catch {
    Add-Result -Status "FAIL" -Check "Root" -Message "Root path does not exist: $Root"
}

if (Test-Path -LiteralPath $Root -PathType Container) {
    Test-File -RelativePath "D3D12LookDevPTWinUI.sln" -Check "Solution" -Fix "Run this script from the repository root or pass -Root."
    Test-File -RelativePath "D3D12LookDevPTWinUI.vcxproj" -Check "Project" -Fix "The Visual Studio project is missing."
    Test-File -RelativePath ".gitmodules" -Check "Submodule config" -Fix "The repository should include .gitmodules."
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if ($git) {
    Add-Result -Status "OK" -Check "Git" -Message "git found: $($git.Source)"
} else {
    Add-Result -Status "FAIL" -Check "Git" -Message "git.exe was not found." -Fix "Install Git for Windows and retry."
}

$msbuild = Find-MSBuild
if ($msbuild) {
    Add-Result -Status "OK" -Check "Visual Studio 2026 MSBuild" -Message "MSBuild found: $msbuild"
} else {
    Add-Result -Status "FAIL" -Check "Visual Studio 2026 MSBuild" -Message "Visual Studio 2026 MSBuild was not found." -Fix "Install Visual Studio 2026 with Desktop development with C++ and WinUI application development."
}

$vs2026Root = ""
$vswherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswherePath -PathType Leaf) {
    $vs2026Root = & $vswherePath -latest -prerelease -version '[18.0,19.0)' -property installationPath
}
$v145Root = if ($vs2026Root) {
    Join-Path $vs2026Root "VC\Tools\MSVC"
} else {
    ""
}
if (Test-Path -LiteralPath $v145Root -PathType Container) {
    $v145 = @(Get-ChildItem -LiteralPath $v145Root -Directory |
        Where-Object { $_.Name -like '14.5*' })
    if ($v145.Count -gt 0) {
        Add-Result -Status "OK" -Check "MSVC v145" -Message "MSVC v145 toolset found."
    } else {
        Add-Result -Status "FAIL" -Check "MSVC v145" -Message "MSVC v145 toolset was not found." -Fix "Modify Visual Studio 2026 and install the current MSVC x64 build tools."
    }
} else {
    Add-Result -Status "FAIL" -Check "MSVC v145" -Message "Visual Studio 2026 C++ tools directory was not found." -Fix "Install the Visual Studio 2026 C++ workload."
}

$sdk26100 = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\Include\10.0.26100.0"
if (Test-Path -LiteralPath $sdk26100 -PathType Container) {
    Add-Result -Status "OK" -Check "Windows SDK 10.0.26100.0" -Message "Windows SDK headers found."
} else {
    Add-Result -Status "FAIL" -Check "Windows SDK 10.0.26100.0" -Message "Windows SDK 10.0.26100.0 was not found." -Fix "Install Windows 11 SDK 10.0.26100.0 through Visual Studio Installer."
}

$submoduleFix = "Run: git submodule update --init --recursive --depth 1"
Test-File -RelativePath "ThirdParty/assimp/include/assimp/Importer.hpp" -Check "Submodule assimp" -Fix $submoduleFix
Test-File -RelativePath "ThirdParty/DirectXTex/DirectXTex/DirectXTex.h" -Check "Submodule DirectXTex" -Fix $submoduleFix
Test-File -RelativePath "ThirdParty/tinyexr/include/exr.h" -Check "Submodule TinyEXR v3" -Fix $submoduleFix
Test-File -RelativePath "ThirdParty/Streamline/include/sl.h" -Check "Optional DLSS Streamline submodule" -Fix $submoduleFix -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/DLSS/include/nvsdk_ngx.h" -Check "Optional DLSS SDK submodule" -Fix $submoduleFix -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll" -Check "Optional DLSS SR runtime" -Fix "Initialize ThirdParty/DLSS or build with /p:EnableDLSS=false." -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/DLSS/lib/Windows_x86_64/rel/nvngx_dlssd.dll" -Check "Optional DLSS-RR runtime" -Fix "Initialize ThirdParty/DLSS or build with /p:EnableDLSS=false." -WarnOnly:(!$CheckDLSS)
Test-File -RelativePath "ThirdParty/Streamline/bin/x64/sl.interposer.dll" -Check "Optional Streamline runtime" -Fix "The pinned Streamline source repository does not ship runtime binaries. Build/package Streamline and place sl.interposer.dll under ThirdParty/Streamline/bin/x64 or Bin/x64/<Config>/Streamline to enable the runtime probe; the renderer otherwise uses its documented internal fallback." -WarnOnly
Test-File -RelativePath "ThirdParty/NRD/Include/NRD.h" -Check "Optional NRD submodule" -Fix $submoduleFix -WarnOnly:(!$CheckNRD)
Test-AnyFile -RelativePaths @(
    "ThirdParty/NRD/Build/v145/$Platform/$Configuration/Lib/NRD.lib"
) -Check "Optional NRD static library" -Fix "Build once with EnableNRD=true, or build with /p:EnableNRD=false to skip NRD." -WarnOnly:(!$CheckNRD)
Test-File -RelativePath "ThirdParty/RTXDI/Libraries/Rtxdi/Include/Rtxdi/RtxdiParameters.h" -Check "Optional RTXDI v3 runtime headers" -Fix $submoduleFix -WarnOnly:(!$CheckRTXDI)
Test-File -RelativePath "ThirdParty/RTXDI/Libraries/Rtxdi/Source/ReSTIRDI.cpp" -Check "Optional RTXDI v3 runtime sources" -Fix $submoduleFix -WarnOnly:(!$CheckRTXDI)

$tinyExrRoot = Join-Path $Root "ThirdParty\tinyexr"
$expectedTinyExrCommit = "1b106618644dbf8a0935c2348ba51a2d863dd7c2"
if ($git -and (Test-Path -LiteralPath $tinyExrRoot -PathType Container)) {
    $actualTinyExrCommit = [string](& $git.Source -C $tinyExrRoot rev-parse HEAD 2>$null | Select-Object -First 1)
    if ($actualTinyExrCommit.Trim() -ieq $expectedTinyExrCommit) {
        Add-Result -Status "OK" -Check "TinyEXR pinned revision" -Message "TinyEXR commit $expectedTinyExrCommit is checked out."
    } else {
        Add-Result -Status "FAIL" -Check "TinyEXR pinned revision" -Message "TinyEXR is not at the required commit." -Fix "Run: git submodule update --init --recursive ThirdParty/tinyexr"
    }
}

$rtxdiRoot = Join-Path $Root "ThirdParty\RTXDI"
$expectedRtxdiCommit = "274141af082050c9d0ad6e01a2e591d0d66b7955"
$expectedRtxdiRuntimeCommit = "a14e079c727ed8c4fd3173bd2aea8244c9d9f6d6"
if ($git -and (Test-Path -LiteralPath $rtxdiRoot -PathType Container)) {
    $gitPath = $git.Source
    $actualRtxdiCommit = [string](& $gitPath -C $rtxdiRoot rev-parse HEAD 2>$null | Select-Object -First 1)
    $actualRtxdiCommit = $actualRtxdiCommit.Trim()
    if ($actualRtxdiCommit -ieq $expectedRtxdiCommit) {
        Add-Result -Status "OK" -Check "RTXDI pinned revision" -Message "RTXDI v3.0.0 commit $expectedRtxdiCommit is checked out."
    } else {
        $status = if ($CheckRTXDI) { "FAIL" } else { "WARN" }
        Add-Result -Status $status -Check "RTXDI pinned revision" -Message "RTXDI is not at the pinned v3.0.0 commit." -Fix "Run: git submodule update --init --recursive ThirdParty/RTXDI"
    }

    $rtxdiRuntimeRoot = Join-Path $rtxdiRoot "Libraries\Rtxdi"
    if (Test-Path -LiteralPath $rtxdiRuntimeRoot -PathType Container) {
        $actualRtxdiRuntimeCommit = [string](& $gitPath -C $rtxdiRuntimeRoot rev-parse HEAD 2>$null | Select-Object -First 1)
        $actualRtxdiRuntimeCommit = $actualRtxdiRuntimeCommit.Trim()
        if ($actualRtxdiRuntimeCommit -ieq $expectedRtxdiRuntimeCommit) {
            Add-Result -Status "OK" -Check "RTXDI runtime pinned revision" -Message "RTXDI runtime commit $expectedRtxdiRuntimeCommit is checked out."
        } else {
            $status = if ($CheckRTXDI) { "FAIL" } else { "WARN" }
            Add-Result -Status $status -Check "RTXDI runtime pinned revision" -Message "RTXDI Libraries/Rtxdi is not at the revision recorded by v3.0.0." -Fix "Run: git submodule update --init --recursive ThirdParty/RTXDI"
        }
    }
}

$nugetRoot = $env:NuGetPackageRoot
if (-not $nugetRoot) {
    $nugetRoot = Join-Path $env:USERPROFILE ".nuget\packages"
}

$dxcPath = Join-Path $nugetRoot "microsoft.direct3d.dxc\1.9.2602.17\build\native\bin\x64\dxc.exe"
$agilityCore = Join-Path $nugetRoot "microsoft.direct3d.d3d12\1.619.3\build\native\bin\x64\D3D12Core.dll"
$restoreFix = "Run: msbuild D3D12LookDevPTWinUI.sln /t:Restore /p:Configuration=$Configuration /p:Platform=$Platform"

if (Test-Path -LiteralPath $dxcPath -PathType Leaf) {
    Add-Result -Status "OK" -Check "DXC package" -Message "dxc.exe found."
} else {
    Add-Result -Status "WARN" -Check "DXC package" -Message "dxc.exe was not found in the NuGet package cache." -Fix $restoreFix
}

if (Test-Path -LiteralPath $agilityCore -PathType Leaf) {
    Add-Result -Status "OK" -Check "D3D12 Agility SDK" -Message "D3D12Core.dll found."
} else {
    Add-Result -Status "WARN" -Check "D3D12 Agility SDK" -Message "D3D12 Agility SDK runtime was not found in the NuGet package cache." -Fix $restoreFix
}

$windowsAppRuntime = @(Get-AppxPackage -Name "Microsoft.WindowsAppRuntime.2.4" -ErrorAction SilentlyContinue |
    Where-Object { $_.Architecture -eq 'X64' -or $_.Architecture -eq 'Neutral' })
if ($windowsAppRuntime.Count -gt 0) {
    Add-Result -Status "OK" -Check "Windows App Runtime x64" -Message "Windows App Runtime 2.4 is installed for the current user."
} else {
    Add-Result -Status "FAIL" -Check "Windows App Runtime x64" -Message "Windows App Runtime 2.4 was not found for the current user." -Fix "Run Scripts/InstallWindowsAppRuntime.ps1 or install the official Windows App Runtime 2.4 x64 redistributable, then rerun this checker."
}

if ($CheckAssets) {
    Test-Directory -RelativePath "Bistro_v5_2" -Check "Bistro root" -Fix "Download Bistro separately and place Bistro_v5_2 next to the solution. See docs/assets.md."
    Test-File -RelativePath "Bistro_v5_2/BistroExterior.fbx" -Check "Bistro exterior" -Fix "Extract BistroExterior.fbx into Bistro_v5_2."
    Test-Directory -RelativePath "Bistro_v5_2/Textures" -Check "Bistro textures" -Fix "Keep the original Bistro Textures folder beside the FBX files."
    Test-File -RelativePath "Bistro_v5_2/BistroInterior.fbx" -Check "Bistro interior" -Fix "Optional: extract BistroInterior.fbx if you want interior testing." -WarnOnly
    Test-File -RelativePath "Bistro_v5_2/BistroInterior_Wine.fbx" -Check "Bistro wine interior" -Fix "Optional: extract BistroInterior_Wine.fbx if you want wine interior testing." -WarnOnly
    Test-File -RelativePath "Bistro_v5_2/san_giuseppe_bridge_4k.hdr" -Check "Bistro HDRI" -Fix "Optional: place an HDRI in Bistro_v5_2 or use your own environment map." -WarnOnly
} else {
    Add-Result -Status "WARN" -Check "Assets" -Message "Asset checks were skipped." -Fix "Run with -CheckAssets to verify Bistro_v5_2 placement."
}

if (-not $CheckDLSS) {
    Add-Result -Status "WARN" -Check "DLSS strict check" -Message "DLSS runtime checks were warning-only." -Fix "Run with -CheckDLSS to fail when optional DLSS files are missing."
}

if (-not $CheckNRD) {
    Add-Result -Status "WARN" -Check "NRD strict check" -Message "NRD checks were warning-only." -Fix "Run with -CheckNRD to fail when optional NRD files are missing."
}

if (-not $CheckRTXDI) {
    Add-Result -Status "WARN" -Check "RTXDI strict check" -Message "RTXDI checks were warning-only." -Fix "Run with -CheckRTXDI to fail when the pinned optional RTXDI SDK is missing or mismatched."
}

$failCount = @($results | Where-Object { $_.Status -eq "FAIL" }).Count
$warnCount = @($results | Where-Object { $_.Status -eq "WARN" }).Count

if ($Json) {
    [pscustomobject]@{
        Root = $Root
        Configuration = $Configuration
        Platform = $Platform
        CheckDLSS = [bool]$CheckDLSS
        CheckNRD = [bool]$CheckNRD
        CheckRTXDI = [bool]$CheckRTXDI
        Failures = $failCount
        Warnings = $warnCount
        Results = $results
    } | ConvertTo-Json -Depth 5
} else {
    Write-Host "D3D12LookDevPTWinUI setup check"
    Write-Host "Root: $Root"
    Write-Host ""
    foreach ($result in $results) {
        $prefix = "[$($result.Status)]"
        if ($result.Status -eq "OK") {
            Write-Host "$prefix $($result.Check): $($result.Message)" -ForegroundColor Green
        } elseif ($result.Status -eq "WARN") {
            Write-Host "$prefix $($result.Check): $($result.Message)" -ForegroundColor Yellow
            if ($result.Fix) { Write-Host "       $($result.Fix)" -ForegroundColor DarkYellow }
        } else {
            Write-Host "$prefix $($result.Check): $($result.Message)" -ForegroundColor Red
            if ($result.Fix) { Write-Host "       $($result.Fix)" -ForegroundColor DarkRed }
        }
    }
    Write-Host ""
    Write-Host "Summary: $failCount failure(s), $warnCount warning(s)"
}

if ($failCount -gt 0) {
    exit 1
}
