# Optional NVIDIA RTXDI ReSTIR DI / GI / PT

Japanese documentation: [NVIDIA RTXDI ReSTIR DI](rtxdi.ja.md)

D3D12LookDevPTWinUI has an optional integration of the official NVIDIA RTXDI SDK. It implements ReSTIR DI, ReSTIR GI, and checkerboard ReSTIR PT. Sun, Environment, emissive triangles, and analytic area lights share one light identity/sample/evaluate/PDF contract.

Pinned revisions:

- RTXDI SDK tag: `v3.0.0`
- RTXDI SDK commit: `274141af082050c9d0ad6e01a2e591d0d66b7955`
- nested `Libraries/Rtxdi` runtime commit: `a14e079c727ed8c4fd3173bd2aea8244c9d9f6d6`

## Setup

Initialize both the SDK and its nested runtime:

```powershell
git submodule update --init --recursive ThirdParty/RTXDI
git -C ThirdParty/RTXDI rev-parse HEAD
git -C ThirdParty/RTXDI/Libraries/Rtxdi rev-parse HEAD
```

The last two commands must print the commits listed above. Run the strict check with:

```powershell
.\Scripts\CheckSetup.ps1 -CheckRTXDI
```

## Build Switch And Matrix

RTXDI is disabled at compile time by default:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableRTXDI=false
```

Enable the ReSTIR DI/GI/PT backends with:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableRTXDI=true
```

When enabled and present, `BuildThirdParty.ps1` builds only `ThirdParty/RTXDI/Libraries/Rtxdi` as `Rtxdi.lib`. That upstream CMake file is an `add_subdirectory` fragment, so `CMake/RtxdiRuntime/CMakeLists.txt` supplies the standalone `project()` context without modifying the pinned submodule. The application validates `RTXDI_RuntimeParameters` plus the official 24-byte DI, 32-byte GI, and 64-byte PT packed reservoir ABIs and uses RTXDI's block-linear layouts.

If `EnableRTXDI=true` is requested without the pinned headers and sources, MSBuild emits a warning and compiles `D3D12LOOKDEVPT_WITH_RTXDI=0`. No SDK include or library link is added, and every ReSTIR mode remains a Baseline PT fallback.

The repository build matrix covers all backend combinations and leaves the target configuration (`NRD=true`, `RTXDI=true`, `DLSS=false`) as its final build:

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

## Current Two-Pass DI Graph

The runtime dispatches two compute passes, not the older four-pass/copy chain:

```text
Pass A: local candidate generation + validated temporal combine -> scratch B
Pass B: adaptive spatial combine + exact current-surface evaluation
        + one final visibility ray + shading -> history/output A
```

Pass A keeps newly generated candidates in registers, reads immutable previous history A, and writes one packed reservoir to scratch B. Pass B reads only B, uses four neighbors on stable surfaces or up to eight on young/disoccluded history, re-evaluates the selected sample's target, PDF, emissive texture, BSDF, and visibility at the current surface, then writes next-frame history directly to A.

The old temporal and shade shader entry points remain as no-op build artifacts for output-name compatibility; the runtime does not dispatch them. There are two physical full-size reservoir buffers, no third full-size reservoir, and no end-of-frame reservoir copy. The public timestamp fields remain stable: the fused work is charged to candidate (Pass A) and spatial (Pass B), while temporal, shade, and publish are expected to be zero or near zero.

Reservoirs use the official packed ABI and retain sample/light identity, reservoir weight, `M`, target PDF, spatial distance, cached visibility fields, and age. RGB averages are not used as reservoirs. Visibility is always traced again for the final selected sample; cached visibility is not trusted as final shading visibility.

## Candidate Scope And Estimator Boundary

The unified candidate space contains:

- emissive mesh triangles, sampled with their light alias-table probability;
- procedural analytic area lights;
- Sun;
- lat-long or procedural Environment.

Candidate selection uses a cheap average-radiance target. The selected sample is then re-evaluated with its exact texture, BSDF, source PDF, and final visibility. Baseline vertices use one power-weighted light-family sample and MIS it against the BSDF technique; emissive-hit and environment-miss PDFs use the same mixture probability.

Temporal reprojection uses the non-jitter 2.5D surface motion plus the current/previous jitter delta at history lookup. Each bilinear tap is validated against depth, normal, albedo, roughness, and packed surface identity. Spatial reuse applies equivalent current-surface guide checks. Ordinary camera motion preserves reuse; camera cuts, projection/resize, geometry changes, and lighting-domain invalidation reject the affected history.

## Render Modes And Fallbacks

The existing mode names remain compatible:

| Render mode | Effective implementation today |
|---|---|
| Baseline PT | Baseline MIS path tracer |
| ReSTIR DI | RTXDI DI + Baseline indirect |
| ReSTIR GI | Baseline one-light direct + RTXDI GI |
| ReSTIR GI + DI | RTXDI DI + RTXDI GI |
| ReSTIR PT | Baseline one-light direct + checkerboard RTXDI PT |
| ReSTIR PT + DI | RTXDI DI + checkerboard RTXDI PT |

RTXDI runs only when all of the following are true:

- the SDK was compiled and its runtime ABI check passed;
- project quality uses `restirBackend: "rtxdi"`;
- the selected mode's DI/GI/PT pipelines are evaluation-ready;
- the quality profile is not `reference_still`.

Otherwise the renderer falls back to Baseline PT without changing the accepted project mode name. `restirBackend: "off"` explicitly disables RTXDI. `reference_still` does not consume RTXDI history; selecting the profile also configures unclamped Baseline MIS accumulation, although later path-tracing edits can override those accumulator controls.

The UI and MCP expose requested versus effective state, SDK/runtime revisions, `diEvaluationReady`, `giEvaluationReady`, `ptEvaluationReady`, active indirect algorithm, per-mode activity, logical reservoir bytes, and a fallback reason.

## Resource And History Behavior

- Full-size reservoirs are allocated only for the selected interactive algorithm; GI and PT are never simultaneously resident.
- DI scratch and the current parity's GI/PT output are placed at offset zero in the same heap with explicit aliasing barriers.
- Final TAA HDR output is the next A/B history resource. The normal fused NRD/TAA path keeps `postDenoiseHdr` as a 1x1 placeholder.
- The fixed A-history/B-scratch descriptors are created with the output resources and are not rewritten while the GPU is executing.
- Surface guides and identity use their own immutable A/B descriptor tables selected by frame parity, removing the former full-resolution guide publish copy.
- ReSTIR shade writes directly into the split diffuse/specular signals. When a denoiser consumes those signals, the redundant accumulation, intermediate post-denoise HDR, and LDR writes are skipped.

## Validation And Known Limits

Validate ReSTIR DI with fixed seed/camera paths, alpha foliage, small emissive lights, high dynamic range, camera cuts, and motion. Performance benchmarks omit full-screen quality counters; quality/combined runs retain them. Compare mean energy and temporal error against a high-SPP Baseline reference rather than judging only a single still frame.

The independent Primary Visibility/compact secondary-task graph is available for eligible DXR 1.1 Interactive Baseline/DI frames. It is intentionally not used for GI/PT or the formal GI+DI gate, and its secondary tasks currently retrace the camera sample instead of continuing from the stored primary intersection. The remaining large items are full SDK-equivalent hybrid-shift path replay beyond the current reconnection target shift, feature-active DLSS-RR certification with an NVIDIA-issued NGX application ID, and final cross-scene temporal/reference quality gates.
