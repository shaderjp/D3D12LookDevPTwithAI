# Optional NVIDIA NRD Backend

Japanese documentation: [NVIDIA NRD backend](nrd.ja.md)

D3D12LookDevPTWinUI integrates NVIDIA Real-Time Denoisers as an optional D3D12 compute backend. The repository is pinned to NRD `v4.17.3` (`792eff196afdd350fd9c3f862119017ccb438a0e`). `interactive_game` selects REBLUR, `sharp_preview` selects RELAX, and `reference_still` disables NRD and temporal post-processing. If NRD is unavailable, interactive profiles retain the requested selection but render through the internal temporal/A-Trous fallback.

## Setup

Initialize the pinned SDK and its dependencies:

```powershell
git submodule update --init --recursive ThirdParty/NRD
git -C ThirdParty/NRD describe --tags --always
```

The second command should report `v4.17.3`. `BuildThirdParty.ps1` builds `NRD.lib`, the generated DXIL shader blobs, and `ShaderMakeBlob.lib` under:

```text
ThirdParty/NRD/Build/<toolset>/x64/<Config>/Lib/
```

Use the strict setup check on a machine intended to run NRD:

```powershell
.\Scripts\CheckSetup.ps1 -CheckNRD
```

## Build Switch And Matrix

NRD is compile-enabled by default. Disable it for a dependency-free backend build:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableNRD=false
```

`EnableNRD=false` excludes the SDK headers and libraries, keeps every shader configuration buildable, reports `compiled=false`, and routes NRD selections to the internal backend. RTXDI and DLSS are independent switches.

The repository includes a build/launch matrix covering all-enabled, each single backend disabled, all-disabled, and the current target configuration (`NRD=true`, `RTXDI=true`, `DLSS=false`):

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

Add `-SkipLaunch` to perform compilation only.

## Signal And Guide Contract

The path tracer writes three linear HDR signals before denoising:

- demodulated diffuse radiance plus the actual first diffuse-secondary hit distance;
- demodulated specular radiance plus the actual first specular-secondary hit distance;
- unfiltered residual energy for emission, sky, and material metadata used during remodulation.

`NrdPrepareCS` currently converts these signals and `SurfaceGuides` to the official NRD resource contract. This prepare dispatch has not yet been removed. It uses the helpers from `NRD.hlsli` rather than a private pack format.

| NRD input | Renderer source / format |
|---|---|
| Motion | 2.5D `previousUV - currentUV` plus `previousViewZ - currentViewZ`, `R16G16B16A16_FLOAT` |
| Normal + roughness | world-space normal and linear roughness, official encoding 2, `R10G10B10A2_UNORM` |
| View-Z | positive linear view-Z, `R32_FLOAT` |
| Diffuse/specular radiance + hit distance | official REBLUR or RELAX packing, `R16G16B16A16_FLOAT` per lobe |
| Diffuse/specular confidence | validated reprojection and lobe sample confidence, `R8_UNORM` per lobe |

Current/previous jitter and view/projection matrices are passed separately to NRD. Motion is not baked with jitter; the SDK receives both jitter values. A primary miss uses the out-of-range View-Z convention and zero lobe hit distance. When a sampled secondary ray misses after a valid primary hit, its lobe stores the configured ray range; zero instead identifies an unsampled or adaptively skipped secondary lobe.

## Execution Graph

For the ordinary final beauty path with NRD and Final TAA enabled, the graph is:

```text
Split lighting signals
  -> NrdPrepareCS
  -> NRD REBLUR/RELAX dispatches
  -> FinalTaaCS (NRD remodulation + composite + HDR TAA + sharpen)
  -> tone-mapped output
```

`FinalTaaCS` reads the denoised diffuse/specular lobes directly, applies the NRD material factors, adds the residual signal, and resolves HDR history. This removes the standalone `NrdCompositeCS` dispatch and its full-resolution post-denoise intermediate from the normal graph.

The explicit composite path is intentionally retained when Final TAA is disabled, a non-final debug/validation view is selected, or a quality benchmark needs the intermediate. The fusion is also disabled for `reference_still`. The optimization fuses Composite with Final TAA; it does not yet eliminate `NrdPrepareCS` or make NRD run at a reduced resolution.

## Runtime Selection And Fallback

The Denoise panel and `lookdevpt.set_denoise` accept:

- `internal`: split diffuse/specular temporal history followed by hit-distance-aware A-Trous;
- `nrd_reblur`: `REBLUR_DIFFUSE_SPECULAR`;
- `nrd_relax`: `RELAX_DIFFUSE_SPECULAR`;
- `dlss_rr`: the separate, currently non-evaluating DLSS-RR probe path;
- `off`: no real-time denoiser.

Example:

```json
{
  "method": "set_denoise",
  "params": {
    "backend": "nrd_reblur",
    "resetNrd": true
  }
}
```

The bridge creates the selected NRD instance, permanent/transient pools, compute pipelines, and a three-frame-context descriptor/constant-buffer ring. Descriptor layouts are cached per method, resolution, frame context, and dispatch; descriptors are rewritten only when their resource binding changes.

If the SDK, method, resource format, pipeline, or evaluation is unavailable, the UI keeps the requested NRD selection visible and reports the effective `internal` backend with an exact fallback reason. A runtime evaluation failure queues a backend-resource rebuild before the internal fallback is used; partially written NRD resources are never reinterpreted as internal history in the same frame.

Read status from `lookdevpt.get_state` or `lookdevpt.get_stats` under `denoise.nrd` / `denoiser.nrd`. Useful fields include `compiled`, `evaluationReady`, SDK version, selected denoiser, resource resolution, pool counts, encoding names, `lastError`, and `fallbackReason`.

## History And Resource Behavior

- Surface guides and identity use immutable A/B descriptor tables selected by frame parity. There is no full-resolution end-of-frame guide copy.
- Ordinary camera motion preserves Surface, Lighting, NRD, and TAA histories. Camera cuts, projection changes, and resizes clear them. Material/light/HDRI edits keep valid surface guides but reset lighting/NRD/TAA history.
- Only the selected real-time backend receives its full-resolution private resource set. Unused backend-only resources remain descriptor-valid 1x1 placeholders. The NRD and internal lobe resources are mutually exclusive and reuse the same allocation family where possible.
- `reference_still` uses Baseline MIS accumulation and does not run NRD, TAA, RTXDI, or contribution compression in the final output.

## Validation

Use the NRD validation/debug views to inspect normal, roughness, linear View-Z, 2.5D motion, confidence/disocclusion, diffuse/specular inputs and outputs, and non-finite pixels before tuning filter strength. Performance benchmarks skip the full-screen quality-counter pass; quality or combined benchmarks retain it and also keep the explicit NRD composite path so captured intermediates remain well-defined.

NRD is integrated and executable, but the 1080p60 quality/performance targets still require scene- and camera-path validation. Do not interpret backend availability as proof that every scene meets the final blur, ghosting, or frame-time gates.
