[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [ValidateRange(1, 9)]
    [int]$Repeat = 3,
    [ValidateRange(1, 1000000)]
    [int]$Frames = 300,
    [ValidateRange(0, 1000000)]
    [int]$Warmup = 120,
    [uint64]$Seed = 1,
    [ValidateRange(0, 1000000)]
    [int]$CaptureEvery = 0,
    [switch]$CaptureAovs,
    [ValidateSet("Exterior", "Interior", "Both")]
    [string]$Scene = "Both",
    [switch]$IncludeReference,
    [switch]$BackendMatrix,
    [ValidateSet("combined", "performance", "quality")]
    [string]$BenchmarkKind = "combined",
    [string]$OutputRoot = ""
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$executable = Join-Path $root "Bin\x64\$Configuration\D3D12LookDevPTWinUI.exe"
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Executable not found: $executable"
}

if (-not $OutputRoot) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $OutputRoot = Join-Path $root "benchmark-output\suite-$stamp"
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputRoot)) {
    $OutputRoot = Join-Path $root $OutputRoot
}
[System.IO.Directory]::CreateDirectory($OutputRoot) | Out-Null

$targets = @()
if ($Scene -eq "Exterior" -or $Scene -eq "Both") {
    $targets += [pscustomobject]@{
        Name = "bistro-exterior-interactive"
        Project = "projects\benchmark_interactive.lookdevpt.json"
        CameraPath = "benchmarks\bistro_exterior_stability.camera.json"
        Profile = "interactive"
    }
    if ($IncludeReference) {
        $targets += [pscustomobject]@{
            Name = "bistro-exterior-reference"
            Project = "projects\benchmark_reference.lookdevpt.json"
            CameraPath = "benchmarks\bistro_exterior_stability.camera.json"
            Profile = "reference"
        }
    }
}
if ($Scene -eq "Interior" -or $Scene -eq "Both") {
    $targets += [pscustomobject]@{
        Name = "bistro-interior-interactive"
        Project = "projects\benchmark_interactive_interior.lookdevpt.json"
        CameraPath = "benchmarks\bistro_interior_stability.camera.json"
        Profile = "interactive"
    }
    if ($IncludeReference) {
        $targets += [pscustomobject]@{
            Name = "bistro-interior-reference"
            Project = "projects\benchmark_reference_interior.lookdevpt.json"
            CameraPath = "benchmarks\bistro_interior_stability.camera.json"
            Profile = "reference"
        }
    }
}

if ($BackendMatrix) {
    $targets = @()
    $generatedProjectDirectory = Join-Path $OutputRoot "generated-projects"
    [System.IO.Directory]::CreateDirectory($generatedProjectDirectory) | Out-Null
    $sceneDefinitions = @()
    if ($Scene -eq "Exterior" -or $Scene -eq "Both") {
        $sceneDefinitions += [pscustomobject]@{
            Name = "bistro-exterior"
            BaseProject = "projects\benchmark_interactive.lookdevpt.json"
            CameraPath = "benchmarks\bistro_exterior_stability.camera.json"
        }
    }
    if ($Scene -eq "Interior" -or $Scene -eq "Both") {
        $sceneDefinitions += [pscustomobject]@{
            Name = "bistro-interior"
            BaseProject = "projects\benchmark_interactive_interior.lookdevpt.json"
            CameraPath = "benchmarks\bistro_interior_stability.camera.json"
        }
    }
    $modeDefinitions = @(
        [pscustomobject]@{ Key = "baseline"; Value = "Baseline PT" },
        [pscustomobject]@{ Key = "restir-gi"; Value = "ReSTIR GI" },
        [pscustomobject]@{ Key = "restir-di"; Value = "ReSTIR DI" },
        [pscustomobject]@{ Key = "restir-gi-di"; Value = "ReSTIR GI + DI" }
    )
    $denoiseBackends = @("nrd_reblur", "nrd_relax", "internal", "off")
    foreach ($sceneDefinition in $sceneDefinitions) {
        $baseProjectPath = Join-Path $root $sceneDefinition.BaseProject
        $baseProjectJson = Get-Content -Raw -LiteralPath $baseProjectPath
        foreach ($modeDefinition in $modeDefinitions) {
            foreach ($denoiseBackend in $denoiseBackends) {
                $generatedProject = $baseProjectJson | ConvertFrom-Json
                $generatedProject.mode = $modeDefinition.Value
                $generatedProject.denoise.backend = $denoiseBackend
                $generatedProject.denoise.enabled = $denoiseBackend -ne "off"
                $generatedName = "{0}-{1}-{2}" -f $sceneDefinition.Name, $modeDefinition.Key, ($denoiseBackend -replace '_', '-')
                $generatedPath = Join-Path $generatedProjectDirectory "$generatedName.lookdevpt.json"
                $generatedProject | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $generatedPath -Encoding UTF8
                $targets += [pscustomobject]@{
                    Name = $generatedName
                    Project = $generatedPath
                    CameraPath = $sceneDefinition.CameraPath
                    Profile = "interactive"
                }
            }
        }
    }
}

function Quote-ProcessArgument([string]$Value) {
    if ($Value -notmatch '[\s"]') {
        return $Value
    }
    return '"' + ($Value -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

$runResults = [System.Collections.Generic.List[object]]::new()
foreach ($target in $targets) {
    for ($run = 1; $run -le $Repeat; ++$run) {
        $runDirectory = Join-Path $OutputRoot ("{0}\run-{1}" -f $target.Name, $run)
        [System.IO.Directory]::CreateDirectory($runDirectory) | Out-Null
        $projectPath = if ([System.IO.Path]::IsPathRooted($target.Project)) { $target.Project } else { Join-Path $root $target.Project }
        $cameraPath = if ([System.IO.Path]::IsPathRooted($target.CameraPath)) { $target.CameraPath } else { Join-Path $root $target.CameraPath }
        $arguments = @(
            "--project", $projectPath,
            "--benchmark",
            "--benchmark-kind", $BenchmarkKind,
            "--camera-path", $cameraPath,
            "--frames", "$Frames",
            "--warmup", "$Warmup",
            "--seed", "$Seed",
            "--output", $runDirectory
        )
        if ($CaptureEvery -gt 0) {
            $arguments += @("--capture-every", "$CaptureEvery")
            if ($CaptureAovs) {
                $arguments += "--capture-aovs"
            }
        }
        $argumentLine = ($arguments | ForEach-Object { Quote-ProcessArgument ([string]$_) }) -join ' '
        Write-Host ("[{0}] run {1}/{2}" -f $target.Name, $run, $Repeat)
        $process = Start-Process -FilePath $executable -ArgumentList $argumentLine -WindowStyle Hidden -PassThru
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            throw "Benchmark process failed for $($target.Name) run $run (exit $($process.ExitCode))."
        }

        $summaryPath = Join-Path $runDirectory "summary.json"
        if (-not (Test-Path -LiteralPath $summaryPath -PathType Leaf)) {
            throw "Benchmark did not write summary.json: $summaryPath"
        }
        $summary = Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json
        $runResults.Add([pscustomobject]@{
            target = $target.Name
            profile = $target.Profile
            run = $run
            output = $runDirectory
            gpuP95Ms = [double]$summary.metrics.gpu_pipeline_ms.p95
            gpuP99Ms = [double]$summary.metrics.gpu_pipeline_ms.p99
            cpuP95Ms = [double]$summary.metrics.cpu_frame_ms.p95
            frameHistoryMiB = [double]$summary.metrics.frame_history_mib.p95
            eligible = [bool]$summary.performanceGate.eligible
            passed = [bool]$summary.performanceGate.passed
        })
    }
}

$selectedRuns = [System.Collections.Generic.List[object]]::new()
foreach ($target in $targets) {
    $ordered = @($runResults | Where-Object target -eq $target.Name | Sort-Object gpuP95Ms)
    $median = $ordered[[int][Math]::Floor($ordered.Count / 2)]
    $selectedRuns.Add([pscustomobject]@{
        target = $target.Name
        medianRun = $median.run
        output = $median.output
        gpuP95Ms = $median.gpuP95Ms
        gpuP99Ms = $median.gpuP99Ms
        cpuP95Ms = $median.cpuP95Ms
        frameHistoryMiB = $median.frameHistoryMiB
        eligible = $median.eligible
        passed = $median.passed
    })
}

$suite = [ordered]@{
    schemaVersion = 1
    generatedUtc = [DateTime]::UtcNow.ToString("o")
    executable = $executable
    seed = $Seed
    warmupFrames = $Warmup
    measuredFrames = $Frames
    repeat = $Repeat
    captureEvery = $CaptureEvery
    captureAovs = [bool]$CaptureAovs
    backendMatrix = [bool]$BackendMatrix
    benchmarkKind = $BenchmarkKind
    runs = @($runResults)
    medianRuns = @($selectedRuns)
}
$suitePath = Join-Path $OutputRoot "suite-summary.json"
$suite | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $suitePath -Encoding UTF8
$selectedRuns | Format-Table target, medianRun, gpuP95Ms, gpuP99Ms, cpuP95Ms, frameHistoryMiB, eligible, passed -AutoSize
Write-Host "Suite summary: $suitePath"
