# Optional NVIDIA RTXDI ReSTIR DI

Japanese documentation: [NVIDIA RTXDI ReSTIR DI](rtxdi.ja.md)

D3D12LookDevPTWinUI has an optional integration boundary for the official NVIDIA RTXDI SDK. It currently implements ReSTIR DI for the renderer's local mesh-emitter and analytic-area-light list. ReSTIR PT/GI is not integrated, and Sun/Environment sampling is not yet folded into the RTXDI candidate set.

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

Enable the current ReSTIR DI backend with:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableRTXDI=true
```

When enabled and present, `BuildThirdParty.ps1` builds only `ThirdParty/RTXDI/Libraries/Rtxdi` as `Rtxdi.lib`. The application validates the official runtime ABI (`RTXDI_RuntimeParameters` and the 24-byte `RTXDI_PackedDIReservoir`) and uses RTXDI's block-linear reservoir layout.

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

The current RTXDI candidate list is the renderer's `RtLight` table:

- emissive mesh triangles, sampled with their light alias-table probability;
- procedural analytic area lights.

The target is positive luminance of the evaluated diffuse plus specular contribution. The current implementation evaluates the material/emissive texture and BSDF for each candidate; the planned cheap average-radiance target is not implemented yet. Sun and lat-long Environment lighting continue through the Baseline path-tracing techniques, and the secondary one-light mixture is not yet unified with RTXDI.

Temporal reprojection uses the non-jitter 2.5D surface motion plus the current/previous jitter delta at history lookup. Each bilinear tap is validated against depth, normal, albedo, roughness, and packed surface identity. Spatial reuse applies equivalent current-surface guide checks. Ordinary camera motion preserves reuse; camera cuts, projection/resize, geometry changes, and lighting-domain invalidation reject the affected history.

## Render Modes And Fallbacks

The existing mode names remain compatible:

| Render mode | Effective implementation today |
|---|---|
| Baseline PT | Baseline MIS path tracer |
| ReSTIR DI | RTXDI DI for local direct lights + Baseline indirect/Sun/Environment |
| ReSTIR GI | Baseline MIS path tracer; RTXDI GI/PT is not integrated |
| ReSTIR GI + DI | RTXDI DI for local direct lights + Baseline indirect/Sun/Environment |

RTXDI runs only when all of the following are true:

- the SDK was compiled and its runtime ABI check passed;
- project quality uses `restirBackend: "rtxdi"`;
- the mode uses DI;
- the quality profile is not `reference_still`.

Otherwise the renderer falls back to Baseline PT without changing the accepted project mode name. `restirBackend: "off"` explicitly disables RTXDI. `reference_still` does not consume RTXDI history; selecting the profile also configures unclamped Baseline MIS accumulation, although later path-tracing edits can override those accumulator controls.

The UI and MCP expose requested versus effective state, SDK/runtime revisions, `diEvaluationReady`, `giEvaluationReady`, per-mode activity, and a fallback reason. Read these fields from `lookdevpt.get_state` under `restir.rtxdiStatus`; the lower-cost `lookdevpt.get_stats` snapshot does not duplicate that status object.

## Resource And History Behavior

- Full-size reservoirs are allocated only when an interactive DI mode can use RTXDI; other configurations keep descriptor-valid placeholders.
- The fixed A-history/B-scratch descriptors are created with the output resources and are not rewritten while the GPU is executing.
- Surface guides and identity use their own immutable A/B descriptor tables selected by frame parity, removing the former full-resolution guide publish copy.
- ReSTIR shade writes directly into the split diffuse/specular signals. When a denoiser consumes those signals, the redundant accumulation, intermediate post-denoise HDR, and LDR writes are skipped.

## Validation And Known Limits

Validate ReSTIR DI with fixed seed/camera paths, alpha foliage, small emissive lights, high dynamic range, camera cuts, and motion. Performance benchmarks omit full-screen quality counters; quality/combined runs retain them. Compare mean energy and temporal error against a high-SPP Baseline reference rather than judging only a single still frame.

The two-pass DI implementation is functional, but the following plan items remain open: RTXDI ReSTIR PT/GI, Sun/Environment integration into the candidate table, the cheap candidate target, the unified secondary one-light estimator, and final cross-scene 1080p60 quality gates.
