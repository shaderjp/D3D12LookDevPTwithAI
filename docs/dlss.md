# Optional DLSS Ray Reconstruction

Japanese documentation: [DLSS Ray Reconstruction](dlss.ja.md)

D3D12LookDevPTWinUI contains an optional Streamline/DLSS Ray Reconstruction backend. It dynamically loads Streamline, validates the adapter and driver, separates render and output resolutions, prepares the required guide resources, tags them for the current frame, and calls `slEvaluateFeature` on the renderer's D3D12 command list.

DLSS-RR remains independent of the RTX 4070 / 1080p60 ReSTIR GI+DI gate. Missing binaries, an unsupported adapter/driver, an absent application identity, or a runtime evaluation failure select native reconstruction without making Streamline a process-load dependency.

## Dependencies And Application Identity

Pinned submodules:

- `ThirdParty/Streamline`: NVIDIA Streamline SDK `v2.12.0`
- `ThirdParty/DLSS`: NVIDIA DLSS SDK `v310.7.0`

Initialize them with:

```powershell
git submodule update --init --recursive ThirdParty/Streamline ThirdParty/DLSS
```

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

The setup checker validates files; strict DLSS validation treats missing runtime DLLs as a failure:

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS
```

## Build Switch And Matrix

DLSS header support is enabled by default:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64
```

Disable it when validating the dependency-free path:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableDLSS=false
```

With `EnableDLSS=false`, no Streamline/DLSS include path or optional runtime copy is used, status reports `compiled=false`, and `dlss_rr` falls back to native internal reconstruction.

The backend matrix covers all-enabled, no-NRD, no-RTXDI, no-DLSS, all-disabled, and the repository target:

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

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

Active DLSS-RR evaluation has not been certified in this checkout because the local environment does not contain an NVIDIA-issued NGX application ID. The missing-identity fallback has been exercised; feature-active validation still requires the issued ID, compatible GPU/driver, and production runtime DLLs.
