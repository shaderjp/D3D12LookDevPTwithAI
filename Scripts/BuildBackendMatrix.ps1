[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [switch]$SkipLaunch,
    [string]$OutputRoot = "benchmark-output\build-matrix"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
    $OutputRoot = Join-Path $root $OutputRoot
}
[System.IO.Directory]::CreateDirectory($OutputRoot) | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = $null
if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
    $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
}
if (-not $msbuild) {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        $msbuild = $command.Source
    }
}
if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
    throw "MSBuild.exe was not found."
}

# Target is last so the checked build output remains in the documented
# default configuration: DLSS and NRD enabled, RTXDI disabled.
$configurations = @(
    [pscustomobject]@{ Name = "all-enabled"; NRD = $true; RTXDI = $true; DLSS = $true },
    [pscustomobject]@{ Name = "no-nrd"; NRD = $false; RTXDI = $true; DLSS = $true },
    [pscustomobject]@{ Name = "no-rtxdi"; NRD = $true; RTXDI = $false; DLSS = $true },
    [pscustomobject]@{ Name = "no-dlss"; NRD = $true; RTXDI = $true; DLSS = $false },
    [pscustomobject]@{ Name = "all-disabled"; NRD = $false; RTXDI = $false; DLSS = $false },
    [pscustomobject]@{ Name = "target"; NRD = $true; RTXDI = $false; DLSS = $true }
)

$results = [System.Collections.Generic.List[object]]::new()
Push-Location $root
try {
    foreach ($entry in $configurations) {
        Write-Host ("Building {0}: NRD={1}, RTXDI={2}, DLSS={3}" -f $entry.Name, $entry.NRD, $entry.RTXDI, $entry.DLSS)
        $buildArguments = @(
            ".\D3D12LookDevPTwithAI.sln",
            "/m",
            "/t:Rebuild",
            "/p:Configuration=$Configuration",
            "/p:Platform=x64",
            "/p:EnableNRD=$($entry.NRD.ToString().ToLowerInvariant())",
            "/p:EnableRTXDI=$($entry.RTXDI.ToString().ToLowerInvariant())",
            "/p:EnableDLSS=$($entry.DLSS.ToString().ToLowerInvariant())",
            "/v:minimal"
        )
        & $msbuild @buildArguments
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed for matrix entry '$($entry.Name)'."
        }

        $summaryPath = $null
        if (-not $SkipLaunch) {
            $runDirectory = Join-Path $OutputRoot $entry.Name
            [System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null
            $executable = Join-Path $root "Bin\x64\$Configuration\D3D12LookDevPTwithAI.exe"
            $runArguments = @(
                "--project", (Join-Path $root "projects\benchmark_interactive.lookdevpt.json"),
                "--benchmark",
                "--benchmark-kind", "performance",
                "--camera-path", (Join-Path $root "benchmarks\bistro_exterior_stability.camera.json"),
                "--frames", "1",
                "--warmup", "1",
                "--seed", "1",
                "--output", $runDirectory
            )
            $quotedArguments = ($runArguments | ForEach-Object {
                $value = [string]$_
                if ($value -match '[\s"]') { '"' + ($value -replace '"', '\"') + '"' } else { $value }
            }) -join ' '
            $process = Start-Process -FilePath $executable -ArgumentList $quotedArguments -WindowStyle Hidden -PassThru
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) {
                throw "Launch smoke failed for matrix entry '$($entry.Name)' (exit $($process.ExitCode))."
            }
            $summaryPath = Join-Path $runDirectory "summary.json"
            if (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf)) {
                throw "Launch smoke did not write summary.json for '$($entry.Name)'."
            }
        }

        $results.Add([pscustomobject]@{
            name = $entry.Name
            enableNRD = $entry.NRD
            enableRTXDI = $entry.RTXDI
            enableDLSS = $entry.DLSS
            buildPassed = $true
            launchPassed = -not $SkipLaunch
            summary = $summaryPath
        })
    }
}
finally {
    Pop-Location
}

$report = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    configuration = $Configuration
    results = @($results)
}
$reportPath = Join-Path $OutputRoot "build-matrix-summary.json"
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding UTF8
$results | Format-Table name, enableNRD, enableRTXDI, enableDLSS, buildPassed, launchPassed -AutoSize
Write-Host "Build matrix summary: $reportPath"
