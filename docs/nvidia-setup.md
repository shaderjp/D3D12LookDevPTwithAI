# NVIDIA development and release setup

The renderer has three optional NVIDIA integrations: DLSS/Streamline, NRD,
and RTXDI. The local AI assistant's CUDA llama.cpp runtime is a separate
user-installed artifact; it is not built or redistributed by this workflow.

For this workflow, the checked-in `config/nvidia-dependencies.json` is the
single source of truth for pinned submodules, required headers/libraries,
runtime DLLs, licenses, and build profiles. `SetupNvidiaEnvironment.ps1` reads
that manifest; it does not maintain a second list of dependency versions.

## Profiles

| Profile | DLSS | NRD | RTXDI | Intended use |
|---|---:|---:|---:|---|
| `RepositoryDefault` | Off | On | Off | Normal project build and CI sanity check |
| `LocalNvidia` | On | On | On | Development and feature-active validation on an NVIDIA workstation |
| `Release` | On | On | On | Audited Release staging; requires license acknowledgement |

`LocalNvidia` requires an NVIDIA GPU visible through `nvidia-smi` and a decimal
NGX application ID. `Release` permits a GPU-less build agent, but still
requires the NGX ID to prove that the release operator has the application
identity needed by the deployed DLSS configuration. A target NVIDIA machine
must still be tested before publication.

## Local setup

Validate a full local NVIDIA environment with:

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = '<NVIDIA-issued decimal ID>'
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia -InitializeSubmodules
```

`-InitializeSubmodules` is opt-in and refuses to update an NVIDIA submodule
with tracked local changes. Omit it when SDKs are already present. SDK roots
outside the repository are supported with `-StreamlineRoot`, `-DlssRoot`,
`-NrdRoot`, and `-RtxdiRoot`.

For example, keep vendor SDKs outside the checkout without changing the
project file:

```powershell
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia `
  -StreamlineRoot 'D:\NVIDIA\Streamline' `
  -DlssRoot 'D:\NVIDIA\DLSS' `
  -NrdRoot 'D:\NVIDIA\NRD' `
  -RtxdiRoot 'D:\NVIDIA\RTXDI'
```

Build and revalidate all outputs:

```powershell
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia -Configuration Debug -Build
```

Without `-Build`, the script checks the GPU/driver, NGX ID, SDK roots, required
development files, licenses, top-level revisions, and nested submodule state.
With `-Build`, it builds the solution with manifest-derived MSBuild properties
and then also requires the generated NRD/RTXDI libraries and mandatory DLSS
runtime DLLs. Optional Streamline feature DLLs are copied when present; the
renderer retains its documented direct NGX/native fallback when a source-only
Streamline checkout does not provide them.

Machine-readable diagnostics can be saved without exposing the NGX value:

```powershell
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia `
  -ReportPath .\Obj\NvidiaSetup\local.json -Json
```

The NGX application ID is read only from the process environment. Reports say
whether a valid decimal value is present but never serialize the value.

## Release payload

Create a clean x64 Release payload with all three renderer backends enabled:

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = '<NVIDIA-issued decimal ID>'
.\Scripts\BuildNvidiaRelease.ps1 -AcceptNvidiaLicense
```

The switch records an explicit operator decision; it does not grant or certify
redistribution rights. Review every NVIDIA license copied under
`Licenses/NVIDIA` before publishing. The builder refuses to overwrite an
existing destination, stages through a temporary directory, removes symbols
and validation binaries, and writes `nvidia-release-manifest.json` with file
sizes and SHA-256 hashes. The NGX ID is never included in the payload.

The staged output contains:

- the Release renderer and self-contained ChatHost;
- app-local Windows App SDK files, Agility SDK files, and shaders;
- required DLSS runtime DLLs plus available Streamline runtime DLLs;
- NVIDIA license documents separated by component;
- `Launch-NVIDIA.ps1` and `README-NVIDIA.txt`;
- dependency revisions, backend state, source-dirty state, prerequisites, and
  a file-level SHA-256 inventory in `nvidia-release-manifest.json`.

The native Windows App SDK runtime is self-contained. Install the current
Microsoft Visual C++ x64 Redistributable compatible with the v145 toolset on a
target machine, set the NGX environment variable, and launch with
`Launch-NVIDIA.ps1`.

The generated hashes detect accidental or malicious file changes; they are not
code signatures and do not authenticate the origin of the SDK/runtime files.

### Release checklist

1. Review the NVIDIA license files at the exact manifest revisions and confirm
   that the intended distribution is permitted.
2. Use a clean source checkout. Local builds record `sourceDirty` in the
   release manifest; the CI workflow rejects a dirty source tree.
3. Set the NGX application ID only in the build process environment.
4. Run `BuildNvidiaRelease.ps1` with a new output path and explicit license
   acknowledgement.
5. Review `README-NVIDIA.txt`, every file under `Licenses/NVIDIA`, and the
   dependency revisions/prerequisites in `nvidia-release-manifest.json`.
6. Recalculate the file inventory before transfer:

```powershell
$payload = '.\Artifacts\NvidiaRelease\D3D12LookDevPTwithAI-NVIDIA-x64'
$manifest = Get-Content "$payload\nvidia-release-manifest.json" -Raw | ConvertFrom-Json
foreach ($record in $manifest.files) {
  $path = Join-Path $payload ([string]$record.path).Replace('/', '\')
  if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing: $($record.path)" }
  $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $record.sha256) { throw "Hash mismatch: $($record.path)" }
}
```

7. On a representative target machine, install the VC++ prerequisite, set the
   NGX environment variable, use `Launch-NVIDIA.ps1`, and verify effective
   DLSS/NRD/RTXDI status rather than relying only on requested UI state.
8. Publish only after the license, target-GPU, and digest reviews are complete.
   Deliver the final archive digest through an authenticated channel.

The existing `BuildIntegratedPortable.ps1` remains the vendor-neutral,
NVIDIA-renderer-disabled exhibition pack. This separation prevents NVIDIA
runtime redistribution from becoming an implicit side effect of a normal
portable build.

For CI, the manual `release-nvidia` workflow uses the same builder. Configure
the repository secret `NVIDIA_NGX_APPLICATION_ID`, run the workflow with its
license acknowledgement checked, then review the uploaded internal artifact.
The workflow intentionally does not publish a GitHub Release automatically.

## Troubleshooting

- **Dirty submodule refusal:** preserve or intentionally restore the reported
  submodule changes before using `-InitializeSubmodules`. The setup script will
  not discard them.
- **NGX application ID failure:** set
  `D3D12LOOKDEVPT_NGX_APPLICATION_ID` to the decimal ID issued for the
  application. Do not put it in the manifest, project file, or repository.
- **Missing NRD/RTXDI library:** rerun the same profile with `-Build`; the
  generated library path is configuration-specific.
- **Missing Streamline feature DLL warning:** a source-only Streamline checkout
  may not ship prebuilt feature DLLs. Supply an approved external SDK root if
  those DLLs are required for the intended integration path.
- **Existing Release output:** choose a new `-OutputDirectory` or explicitly
  archive/remove the old output. The builder never overwrites it.
- **No GPU on CI:** the `Release` profile permits this. Feature-active DLSS,
  NRD, and RTXDI validation must be run separately on the target GPU/driver.

## CUDA boundary

The renderer integrations above use Direct3D 12 and do not require a CUDA
Toolkit installation. Selecting the AI Assistant's CUDA llama.cpp backend is a
separate in-app artifact setup. Its runtime, license, and integrity manifest
are not consumed by `SetupNvidiaEnvironment.ps1` or bundled by
`BuildNvidiaRelease.ps1`.
