# Optional DLSS Ray Reconstruction

Japanese documentation: [DLSS Ray Reconstruction](dlss.ja.md)

D3D12LookDevPTwithAI contains an optional Streamline/DLSS Ray Reconstruction backend. It dynamically loads Streamline, validates the adapter and driver, separates render and output resolutions, prepares the required guide resources, tags them for the current frame, and calls `slEvaluateFeature` on the renderer's D3D12 command list.

DLSS-RR remains independent of the RTX 4070 / 1080p60 ReSTIR GI+DI gate. Missing binaries, an unsupported adapter/driver, an absent application identity, or a runtime evaluation failure select native reconstruction without making Streamline a process-load dependency.

## Dependencies And Application Identity

Pinned submodules:

- `ThirdParty/Streamline`: NVIDIA Streamline SDK `v2.12.0`
- `ThirdParty/DLSS`: NVIDIA DLSS SDK `v310.7.0`

The supported coordinated setup initializes and validates them from the
checked-in NVIDIA manifest:

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = '<NVIDIA-issued decimal ID>'
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia -InitializeSubmodules
```

Initialization is opt-in and refuses to update a submodule with tracked local
changes. See [NVIDIA development and release setup](nvidia-setup.md) for
external SDK roots, machine-readable reports, and release staging.

The DLSS SDK supplies `nvngx_dlss.dll` and `nvngx_dlssd.dll`. A source-only Streamline checkout may not include all prebuilt runtime and feature DLLs. When present, the build copies these files to `Bin/x64/<Config>/Streamline/`:

```text
sl.interposer.dll
sl.common.dll
sl.dlss.dll
sl.dlss_d.dll
nvngx_dlss.dll
nvngx_dlssd.dll
```

Production NGX components also require an application identity issued by NVIDIA. Keep the decimal ID outside source control and set it before launching:

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = "<NVIDIA-issued decimal ID>"
```

The renderer does not substitute a temporary or invented application ID. Without this variable it reports `applicationIdentityConfigured=false`, identifies `applicationIdentity` as the failure stage, and uses native reconstruction.

The older component-specific checker remains available for a quick file check:

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS
```

The manifest-driven setup additionally validates the GPU/driver, application
identity, pinned and nested revisions, licenses, and post-build outputs.

## Build Switch And Matrix

DLSS is disabled in the repository default. Enable it explicitly or use the
`LocalNvidia` / `Release` profile:

```powershell
msbuild .\D3D12LookDevPTwithAI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableDLSS=true
```

Disable it when validating the dependency-free path:

```powershell
msbuild .\D3D12LookDevPTwithAI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableDLSS=false
```

With `EnableDLSS=false`, no Streamline/DLSS include path or optional runtime copy is used, status reports `compiled=false`, and `dlss_rr` falls back to native internal reconstruction.

The backend matrix covers all-enabled, no-NRD, no-RTXDI, no-DLSS, all-disabled, and the repository target:

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

The final repository target is read from `config/nvidia-dependencies.json` and
currently leaves `DLSS=false`, `NRD=true`, and `RTXDI=false`.

## Frame Evaluation

Initialization performs the following:

1. load `sl.interposer.dll` and resolve the required core functions;
2. initialize Streamline for D3D12 with frame-based resource tagging;
3. register the D3D12 device;
4. validate the NVIDIA-issued application identity;
5. query `kFeatureDLSS_RR` support for the selected adapter/driver;
6. query the recommended render size and configure the selected DLSS mode.

For each eligible frame, `PathTracingDlss.hlsl` prepares:

- HDR scaling-input color;
- positive linear depth;
- 2D motion vectors;
- packed world normal and roughness;
- diffuse albedo and specular albedo;
- a 1x1 exposure resource.

The backend submits camera matrices, jitter, reset state, render/output extents, and the eight resource tags, then evaluates `kFeatureDLSS_RR` inside the active command list. A successful evaluation produces display-resolution HDR and bypasses NRD, the internal denoiser, and Final TAA before tone mapping.

If evaluation fails, its partial output is never displayed. The renderer performs native reconstruction for that frame, disables further evaluation, and requests native resource reconstruction for the following frame. Resize, camera cut, and explicit reset propagate a DLSS history reset.

## Resolution And Runtime Status

The quality schema separates render and output resolution:

```json
{
  "resolutionMode": "dynamic",
  "fixedRenderScale": 0.75,
  "minRenderScale": 0.5,
  "maxRenderScale": 1.0
}
```

`resolutionMode` accepts `native`, `fixed`, or `dynamic`; older projects default to `native`. Dynamic mode uses the existing GPU budget and settle hysteresis and adjusts in 1/16 increments. Native non-DLSS paths use TAAU when render and output sizes differ, while NRD runs at render resolution.

Select DLSS-RR through the Denoise panel or MCP:

```json
{
  "method": "set_denoise",
  "params": {
    "backend": "dlss_rr",
    "dlssMode": "quality",
    "resetDlss": true
  }
}
```

Read `denoise.dlss` / `denoiser.dlss` from `lookdevpt.get_state` or `lookdevpt.get_stats`. Status includes compile/load/init/device/application-identity/support/evaluation state, recommended and active dimensions, successful/failed evaluation counts, last result code and failure stage, runtime path, error, and fallback reason. Benchmark output exposes the same activation and failure evidence.

| DLSS availability preference enabled | DLSS availability preference disabled |
|:---:|:---:|
| ![DLSS Ray Reconstruction selected with DLSS Enabled When Available switched on](images/nvidiadlssreyareconstruct.png) | ![DLSS Ray Reconstruction selected with DLSS Enabled When Available switched off](images/dlssrayreconstructwithoutdlss.png) |

These captures intentionally demonstrate a negative runtime state: DLSS Ray
Reconstruction remains the requested backend, but the status block says it was
disabled at build time and that native reconstruction is active. The
`DLSS Enabled When Available` preference is stored independently from the
backend selector; requested and active backend must therefore be read from the
status/state fields. A production-ready capture must instead show successful
initialization and evaluation evidence. See the complete
[Denoise UI and fallback gallery](denoise-ui.md).

Do not infer feature-active certification from a successful compile. Run the
`LocalNvidia` profile and an application-specific quality/failure matrix with
the issued NGX ID, compatible GPU/driver, and approved production runtime DLLs.
