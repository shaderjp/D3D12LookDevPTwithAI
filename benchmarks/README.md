# Stability benchmark paths

`bistro_exterior_stability.camera.json` and
`bistro_interior_stability.camera.json` are deterministic 420-frame paths for
the default 120-frame warmup and 300-frame measurement run. Both include a
static interval, slow pan/orbit/dolly, fast strafe/turn, stop-and-go recovery,
yaw wrapping, and a camera cut. The matching project presets are:

- `projects/benchmark_interactive.lookdevpt.json` and
  `projects/benchmark_reference.lookdevpt.json` for Bistro Exterior;
- `projects/benchmark_interactive_interior.lookdevpt.json` and
  `projects/benchmark_reference_interior.lookdevpt.json` for Bistro Interior.

`bistro_exterior_static.camera.json` holds the Exterior camera for all 420
frames. It is intended for temporal-CV captures after a long static warmup, so
stop-and-go recovery is not mixed into the stationary-noise measurement.

Example:

```powershell
Bin\x64\Release\D3D12LookDevPTwithAI.exe `
  --project projects\benchmark_interactive.lookdevpt.json `
  --benchmark `
  --benchmark-kind performance `
  --camera-path benchmarks\bistro_exterior_stability.camera.json `
  --frames 300 `
  --warmup 120 `
  --seed 1 `
  --output benchmark-output\bistro-exterior
```

Camera-path angles are radians. A keyframe with `cut: true` holds the preceding
pose until that exact frame and then changes pose without interpolation. Yaw is
interpolated over its shortest wrapped arc. Benchmark time is fixed at 1/60 s.

`--benchmark-kind` accepts three values:

- `performance` disables the full-resolution quality-counter pass and is the only kind eligible for the 60 fps gate;
- `quality` enables history-rejection, contribution-energy, and finite-value diagnostics plus capture-friendly explicit intermediates;
- `combined` preserves the original all-in-one behavior and remains the default when the option is omitted.

The output directory contains per-frame `frames.csv`, aggregate `summary.json`,
the final HDR/LDR/AOV captures, `artifacts.json`, and
`quality_analysis.json`. The artifact manifest records each capture's role,
dimensions, source format, byte size, SHA-256 digest, channel min/max, and
NaN/Inf counts. `summary.json.performanceGate` is evaluated only for an
isolated `performance` run at native 1920x1080 with at least 120 warmup and 300 measured frames
whose delayed GPU timestamps match every submitted frame. Short smoke runs still
write all diagnostics, but report `eligible: false` and cannot pass the gate.

The performance thresholds are GPU p95 <= 16.7 ms, GPU p99 <= 20 ms, CPU p95
<= 4 ms (GPU fence throttling is reported separately), and frame/history
resources <= 512 MiB. Eligibility also requires an RTX 4070, a Bistro target
scene, `interactive_game`, Beauty view, the prescribed ray budget, active NRD
REBLUR and active RTXDI GI+DI without fallback. A Baseline fallback is
intentionally ineligible rather than being reported as a passing final-pipeline
result. For a release-quality result, run the same path three times and use the
median run when comparing builds.

## Measured-frame sequences

The original CLI remains unchanged and captures only the final artifact set.
Add `--capture-every N` to also capture LDR and HDR beauty for every Nth
measured frame (warmup is never captured). Add `--capture-aovs` to include the
SurfaceGuides and lighting signals in each scheduled frame:

```powershell
Bin\x64\Release\D3D12LookDevPTwithAI.exe `
  --project projects\benchmark_interactive.lookdevpt.json `
  --benchmark `
  --benchmark-kind quality `
  --camera-path benchmarks\bistro_exterior_stability.camera.json `
  --frames 300 --warmup 120 --seed 1 `
  --capture-every 1 --capture-aovs `
  --output benchmark-output\bistro-exterior-sequence
```

Per-frame files use `frames/<measured-frame>/...`. Capturing every frame is
intentionally explicit because native-resolution HDR plus AOV sequences can be
very large and each readback blocks to preserve exact frame association.

`quality_analysis.json` selects the contiguous `beauty_hdr` inputs for a
post-process temporal luminance CV calculation and defines its median 1% / p95
3% targets. It also contains the 10–90% edge-width contract; copy it into an
analysis request and supply a high-SPP reference manifest plus an edge ROI.
GPU history-rejection and contribution-energy counters are versioned in
`summary.json.metricSchema.qualityCounters`. Until a backend publishes a
counter, it is explicitly marked `available: false` rather than recorded as
zero.

Run the harness unit test with:

```powershell
Scripts\TestBenchmarkHarness.ps1
```

`Scripts/RunStabilityBenchmarks.ps1` automates the three-run Exterior/Interior
suite and writes `suite-summary.json` with the median-p95 run selected for each
target. Use `-BenchmarkKind performance|quality|combined` to select the run
contract. Add `-IncludeReference` to capture the matching unbiased Baseline PT
profiles alongside Interactive. `-CaptureEvery 1 -CaptureAovs` enables the
full measured-frame HDR/LDR/AOV sequence for temporal-quality analysis.
`-BackendMatrix` generates and runs all four render modes against NRD REBLUR,
NRD RELAX, the internal fallback, and OFF without modifying checked-in presets.
`Scripts/BuildBackendMatrix.ps1` rebuilds and smoke-launches the optional-NVIDIA
feature matrix, including individual and combined `EnableNRD=false`,
`EnableRTXDI=false`, and `EnableDLSS=false` configurations.

After a sequence capture, `Scripts/AnalyzeBenchmarkSequence.py <output-dir>
--start <static-segment> --count 32 --enforce` computes the median/p95 linear
HDR luminance temporal CV and scans every sample for NaN/Inf. The surface CV
uses `primaryHitT` from the alpha channel of `surface_motion_hit.dds`
(`0 = miss`, `> 0 = hit`): complete per-frame AOVs
are intersected across the analyzed frames, otherwise the final AOV is used as
a static-camera mask. A partial per-frame AOV set is rejected instead of being
silently mixed with the final mask. The output records the mask source,
artifact count, and surface-pixel count. Zero-mean surface pixels have undefined
CV and are reported separately; no luminance floor is applied. It requires
NumPy and writes `temporal-analysis.json` with the unchanged 1%/3% acceptance
gate. Supplying a matched `--reference-hdr` also evaluates the strong-edge
10-90% width ratio against the 1.15 blur limit.

The HDR reader accepts DirectXTex's adaptive output, where an individual
scanline can fall back from channel-RLE to raw RGBE when compression would be
larger. The sequence may legally mix both row encodings in one file.

The DDS parser and mask/CV behavior can be tested with:

```powershell
Scripts\TestBenchmarkSequenceAnalyzer.ps1 -PythonExecutable <python-with-numpy>
```
