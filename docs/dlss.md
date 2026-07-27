# Optional DLSS Ray Reconstruction Probe

Japanese documentation: [DLSS Ray Reconstruction probe](dlss.ja.md)

D3D12LookDevPTWinUI contains an optional, safe-first Streamline/DLSS Ray Reconstruction integration boundary. The current code can load Streamline dynamically, register the D3D12 device, query adapter/driver support, and request recommended render sizes. It does **not** tag rendering resources or call DLSS-RR evaluation yet. Consequently, `evaluationReady` remains false and selecting `dlss_rr` currently renders through the internal denoiser.

This distinction is deliberate: compile/runtime detection is implemented, but DLSS-RR is not a production denoising backend in this version and is not part of the RTX 4070/1080p60 completion gate.

## Dependencies

Pinned submodules:

- `ThirdParty/Streamline`: NVIDIA Streamline SDK `v2.12.0`
- `ThirdParty/DLSS`: NVIDIA DLSS SDK `v310.7.0`

Initialize them with:

```powershell
git submodule update --init --recursive ThirdParty/Streamline ThirdParty/DLSS
```

The DLSS SDK supplies `nvngx_dlss.dll` and `nvngx_dlssd.dll`. A source-only Streamline checkout may not include the prebuilt runtime and feature DLLs. When present, the build copies these files to `Bin/x64/<Config>/Streamline/`:

```text
sl.interposer.dll
sl.common.dll
sl.dlss.dll
sl.dlss_d.dll
nvngx_dlss.dll
nvngx_dlssd.dll
```

The runtime probe also checks `ThirdParty/Streamline/bin/x64/`. Missing runtime DLLs are warnings in the normal setup check and failures only in strict DLSS validation:

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS
```

## Build Switch And Matrix

DLSS header support is compile-enabled by default:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64
```

Disable it when the Streamline/DLSS submodules are absent or when validating the dependency-free path:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableDLSS=false
```

With `EnableDLSS=false`, no Streamline/DLSS include path or optional runtime copy is used, status reports `compiled=false`, and `dlss_rr` selections fall back to `internal`.

The full backend build matrix includes all-enabled, no-NRD, no-RTXDI, no-DLSS, all-disabled, and the current target (`NRD=true`, `RTXDI=true`, `DLSS=false`):

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

## What The Current Probe Does

At startup, `DlssBackend`:

1. searches for and dynamically loads `sl.interposer.dll`;
2. resolves the required Streamline core entry points;
3. initializes Streamline for D3D12 with frame-based resource tagging requested;
4. registers the D3D12 device;
5. checks `kFeatureDLSS_RR` for the selected adapter/driver;
6. resolves the DLSS-RR option/optimal-settings functions and queries the recommended input size for the selected Quality/Balanced/Performance/Ultra Performance mode.

Even if all six steps succeed, the backend intentionally reports:

```text
DLSS-RR support was detected, but resource-tag evaluation is not enabled in this build.
```

The missing implementation is the per-frame Streamline constants/resource tagging and `slEvaluateFeature` path, including the exact color, depth, motion, normal, roughness, albedo, specular, exposure, and reset contract. Until that work is completed and quality-gated, the internal denoiser remains the effective backend.

## Runtime Selection And Status

Select the probe through the Denoise panel or MCP:

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

The requested backend remains visible as `dlss_rr` so setup failures are diagnosable, while `activeBackend` remains `internal`. `reference_still` disables all real-time denoisers regardless of this selection.

Read `denoise.dlss` / `denoiser.dlss` from `lookdevpt.get_state` or `lookdevpt.get_stats`. The status includes:

- compiled, runtime-loaded, initialized, and device-registered state;
- adapter/driver feature support and `evaluationReady`;
- selected mode and recommended render/output resolutions;
- runtime DLL path, last error, and fallback reason;
- requested history reset state.

## Resource Behavior And Current Limit

Because evaluation never becomes ready, this version does not allocate a DLSS-RR-specific full-resolution input/output graph. The renderer allocates the internal fallback resources instead. This preserves operation on non-NVIDIA GPUs, unsupported drivers, missing-DLL installations, and `EnableDLSS=false` builds without making Streamline a process-load dependency.

The previously documented DLSS fallback screenshot showed an older Denoise panel and has been removed from this page. A replacement should be captured only after the current quality-profile/status UI is built and the displayed requested/effective backend fields can be verified.
