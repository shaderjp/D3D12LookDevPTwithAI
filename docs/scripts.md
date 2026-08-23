# Scripts Guide

[日本語](scripts.ja.md)

This guide describes every file under the repository's `Scripts` directory:
what it is for, its important inputs and outputs, and when it belongs to a
legacy workflow. Renderer and MCP contracts remain in their specialized
documents; this page answers which script to run for an operational task.

## Execution conventions

- Run scripts from the repository root unless a section says otherwise.
  PowerShell 7 is recommended. `BuildIntegratedPortable.ps1` and
  `TestIntegratedPortable.ps1` require PowerShell 7.4 or later.
- Native build and test wrappers require Visual Studio 2026, MSVC `v145`, and
  Windows SDK `10.0.26100.0`. Run `CheckSetup.ps1` first.
- `-AcceptArtifactLicenses`, `-AcceptNvidiaLicense`, and `-AcceptLicenses`
  record an operator's explicit review. They do not grant redistribution
  rights or authenticate an artifact's origin.
- Build/package scripts generally use staging and hash validation and refuse
  to overwrite an existing output. Prefer a new output path for every run.
- `Test*.ps1` scripts normally create a unique test root below the system temp
  directory and verify ownership before cleanup. A script that preserves a
  failed artifact reports its path in the error.
- Never feed tokens, credentials, conversation history, or approval state into
  a packaging script.

Typical entry points:

```powershell
pwsh -NoProfile -File .\Scripts\CheckSetup.ps1 -CheckAssets
pwsh -NoProfile -File .\Scripts\SetupNvidiaEnvironment.ps1 `
  -Profile RepositoryDefault -Configuration Debug -Build
pwsh -NoProfile -File .\Scripts\TestAssistantProtocol.ps1
```

## Environment checks and configuration

| Script | Purpose | Important inputs, outputs, and cautions |
|---|---|---|
| `CheckSetup.ps1` | Diagnoses the repository, VS 2026/v145, SDK, submodules, optional backends, and Bistro assets. | Supports `-Configuration`, `-CheckAssets`, `-CheckDLSS`, `-CheckNRD`, and `-CheckRTXDI`. `-Json` writes a machine-readable report; any FAIL returns exit code 1. |
| `ConfigureLocalInference.ps1` | Validates a local GGUF and complete `llama-server.exe` runtime tree, then creates a schema-v1 `inference.json` for the integrated Assistant. | Takes `-ModelPath`, `-RuntimePath`, `-ModelId`, `-Backend cpu|cuda|vulkan`, and context/token/temperature settings. `-AcceptArtifactLicenses` is required; replacing a configuration also requires `-ReplaceConfiguration`. By default it transactionally copies artifacts beneath `%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI` and records every file's size and hash. |
| `SetupNvidiaEnvironment.ps1` | Uses `config/nvidia-dependencies.json` to validate NVIDIA revisions, layout, licenses, and the NGX ID, optionally initializing submodules and building. | Supports `-Profile LocalNvidia|Release|RepositoryDefault`, `-Configuration`, `-InitializeSubmodules`, `-Build`, SDK-root overrides, `-ReportPath`, and `-Json`. It does not discard source changes. |
| `NvidiaDependencyTools.psm1` | Shared module for manifest/profile loading, root detection, reports, safe submodule initialization, MSBuild properties, and runtime/license staging. | Do not run it directly. It is imported by the NVIDIA setup, backend-matrix, and release scripts. |
| `InstallWindowsAppRuntime.ps1` | Validates and installs the Microsoft Windows App Runtime x64 prerequisite pinned by `suite.lock.json`. | Verifies the approved `aka.ms` URL, SHA-256, Microsoft Authenticode signature, and `--quiet` argument. `-VerifyMetadataOnly` performs no download or installation. |

## Builds, packages, and asset bundles

| Script | Purpose | Important inputs, outputs, and cautions |
|---|---|---|
| `BuildBackendMatrix.ps1` | Rebuilds NRD / RTXDI / DLSS enabled/disabled combinations and optionally performs a one-frame smoke launch. The repository-default combination is built last. | `-Configuration Debug|Release`, `-SkipLaunch`, and `-OutputRoot`. Writes `build-matrix-summary.json` and a `summary.json` for each launched configuration. |
| `BuildIntegratedPortable.ps1` | Builds the current one-app Release x64 payload through transactional staging, including ChatHost, .NET, Windows App SDK, shaders, runtimes, license map, SPDX SBOM, and hash manifests. | Requires PowerShell 7.4+ and `-OutputDirectory`. An AI payload requires `-AiArtifactDirectory`, both manifests, and license/unsigned-trust acceptance. `-WithoutAi` is for renderer development; `-NoArchive` suppresses the ZIP. The default requires clean source and a nonexistent output path. |
| `BuildNvidiaRelease.ps1` | Builds the separate NVIDIA release boundary with DLSS, NRD, and RTXDI enabled. | Requires `-AcceptNvidiaLicense`. Accepts SDK-root overrides and `-InitializeSubmodules`; stages runtime files, licenses, a launcher, and `nvidia-release-manifest.json` under `Artifacts\NvidiaRelease\...` by default. It never records the NGX Application ID value. |
| `LookDevBundle.ps1` | Creates or safely imports `thin` and `portable` `.lookdevbundle` archives containing a `.lookdevpt.json` project and its declared assets. | `-Command Create|Import`. Create uses `-Project`, `-Output`, `-BundleMode`, asset/license mappings, and `-ConfirmAssetRights`. Import uses `-Bundle`, a nonexistent `-Destination`, and `-ResolveAssetRoot` for thin bundles. It validates traversal, links, entry/expanded-size limits, and SHA-256. |

One-app exhibition example:

```powershell
pwsh -NoProfile -File .\Scripts\BuildIntegratedPortable.ps1 `
  -OutputDirectory ..\artifacts\D3D12LookDevPTwithAI-integrated-win-x64 `
  -AiArtifactDirectory 'D:\AI\LookDevPack\AI' `
  -AiArtifactManifest 'D:\AI\LookDevPack\AI\inference.json' `
  -AiRedistributionManifest 'D:\AI\LookDevPack\AI\redistribution.json' `
  -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust
```

See [Integrated AI architecture](integrated-ai-architecture.ja.md),
[NVIDIA setup](nvidia-setup.md), and [Asset setup](assets.md) for the detailed
product and redistribution boundaries.

## Benchmarks and sequence analysis

| Script | Purpose | Important inputs, outputs, and cautions |
|---|---|---|
| `RunStabilityBenchmarks.ps1` | Runs repeated Bistro Exterior/Interior benchmarks and selects the median GPU-p95 run for each target. | Supports `-Repeat`, `-Frames`, `-Warmup`, `-Seed`, `-Scene`, `-BenchmarkKind`, and `-IncludeReference`. `-CaptureEvery` plus `-CaptureAovs` writes sequences; `-BackendMatrix` expands render-mode × denoiser cases. Writes `suite-summary.json`. |
| `AnalyzeBenchmarkSequence.py` | Analyzes captured Radiance HDR sequences for temporal luminance CV, finite values, surface masks, and optional 10–90% edge width. | Requires Python and NumPy. Takes `input`, `--start`, `--count`, `--output`, `--reference-hdr`, and `--enforce`. Writes `temporal-analysis.json` by default; `--enforce` returns nonzero when a gate fails. |

```powershell
.\Scripts\RunStabilityBenchmarks.ps1 `
  -Scene Exterior -Repeat 3 -BenchmarkKind quality `
  -CaptureEvery 1 -CaptureAovs

python .\Scripts\AnalyzeBenchmarkSequence.py `
  .\benchmark-output\suite-<timestamp>\bistro-exterior-interactive\run-1 `
  --start 120 --count 32 --enforce
```

See the [Benchmark guide](../benchmarks/README.md) for capture and metric
contracts.

## Active test scripts

Native wrappers locate MSVC through `vswhere.exe`, compile only the required
test sources into the system temp directory, and run them. Build the relevant
main-project configuration first when a test depends on a third-party library.

| Script | Coverage |
|---|---|
| `TestAssistantHostBridge.ps1` | Debug ChatHost/native named-pipe E2E, process ownership, and isolated SQLite history. It uses only the deterministic inference hook. |
| `TestAssistantProtocol.ps1` | Native Assistant framing/parser protocol, size bounds, and malformed input. |
| `TestBenchmarkHarness.ps1` | Benchmark CLI/harness and artifact/summary contracts. |
| `TestBenchmarkSequenceAnalyzer.ps1` | HDR/DDS readers, masks, CV, and edge analysis through `Tests/AnalyzeBenchmarkSequenceTests.py`. `-PythonExecutable` selects a NumPy environment. |
| `TestConfigureLocalInference.ps1` | Local-inference staging, hashes/manifests, replacement policy, reparse/path attacks, and atomic cleanup. |
| `TestGltfSceneImporter.ps1` | tinygltf scene, material, extension, and import contracts. |
| `TestIntegratedPortable.ps1` | Bounded regression for payload closure, AI manifests, licenses/SBOM, transactions, races, and rollback. Requires PowerShell 7.4+. |
| `TestLookDevBundle.ps1` | Thin/portable bundle create/import, hashes, traversal rejection, and license confirmation. |
| `TestMcpConformance.ps1` | Connects the official `@modelcontextprotocol/conformance` suite to a temporary host. Requires `pnpm` or `npx`; accepts scenario, suite, and expected-failure options. |
| `TestMcpReviewAnalysis.ps1` | Native scene-audit/review analysis contracts. |
| `TestMcpServer.ps1` | Native MCP JSON-RPC, sessions, tools/resources/prompts, and approvals. |
| `TestMcpServerHttp.ps1` | HTTP framing, chunked bodies, timeouts, size/depth/UTF-8, and security boundaries. |
| `TestNvidiaDependencyTools.ps1` | NVIDIA manifest/profile/root module helpers. |
| `TestPbrtDxrContracts.ps1` | Static source-pattern checks for PBRT/DXR contracts. |
| `TestPbrtSceneImporter.ps1` | PBRT importer fixtures and optional extra `-ScenePath` inputs. Requires the Release x64 Assimp library. |
| `TestProjectPaths.ps1` | Relative/absolute project, scene, and environment path resolution and safety. |
| `TestQualitySettingsJson.ps1` | Quality profiles and JSON serialization/validation. |
| `TestRendererCommandQueue.ps1` | Command coalescing, FIFO barriers, indexed targets, and immutable snapshot publication. |
| `TestTextureLoader.ps1` | Texture decode/upload policy and format/path/security contracts. Requires the Release x64 DirectXTex library. |
| `TestTinyExrLoader.ps1` | TinyEXR format, validation, and failure contracts. |
| `TestTransientResourceAllocator.ps1` | Transient allocation, aliasing, budget, and lifetime contracts. |

Managed chat tests are run directly rather than through a PowerShell wrapper:

```powershell
dotnet test .\Managed\Tests\D3D12LookDevPTwithAI.Chat.Tests\D3D12LookDevPTwithAI.Chat.Tests.csproj `
  --configuration Release
```

## Legacy two-app portable-suite scripts

These scripts reproduce or maintain the former `0.2.0-beta.1` workflow that
combined this repository with an external `LocalMCPChatClient`. Do not use them
to set up the current integrated Assistant or create a new one-app exhibition
package; use `BuildIntegratedPortable.ps1` instead.

| Script | Role in the legacy workflow |
|---|---|
| `BootstrapSuite.ps1` | Aligns the external LocalMCPChatClient and submodules with `suite.lock.json`, optionally installing WinGet prerequisites, building, and running integration tests. |
| `BuildPortableSuite.ps1` | Produces the two-app renderer/LocalMCPChatClient payload, launcher, manifest, ZIP, and hash. |
| `BuildOfflinePack.ps1` | Adds CPU/CUDA/Vulkan model/runtime artifacts to a legacy suite and writes an offline manifest and ZIP. |
| `InstallPortableSuite.ps1` | Expands a local/HTTPS ZIP through bounded staging, validates optional SHA-256 and the suite manifest, then installs below the current user's `LocalAppData\Programs`. With `-Force`, the previous install is retained as a timestamped backup. |
| `InstallOfflinePack.ps1` | Validates the offline manifest, hashes, and GPU suitability, then copies model/runtime files into the legacy LocalMCPChatClient data root. Requires `-AcceptLicenses`. |
| `TestLocalMcpChatClientIntegration.ps1` | Connects a specific .NET integration test from the external repository to a temporary native MCP host. |
| `TestOfflinePack.ps1` | Offline-pack build/install, license gate, and sensitive-data exclusion. |
| `TestPortablePrerequisites.ps1` | Checks disk, OS, architecture, and lock metadata for the legacy payload; optionally emits JSON. |
| `TestSuiteLicenseCompliance.ps1` | Validates the legacy payload allowlist, license map, hashes, and SPDX 2.3 SBOM; `-Generate` creates the inventories. |
| `UninstallPortableSuite.ps1` | Removes the legacy per-user install through `ShouldProcess`. `-RemoveLocalApplicationData` also deletes settings, artifacts, and history and must be explicitly selected. |

## Keeping this guide current

When a script is added, renamed, or removed, update this catalog and the README
links in the same change. If the script is added as a Visual Studio project
item, also keep `D3D12LookDevPTwithAI.vcxproj` and its `.filters` file in exact
one-to-one sync under the `Build Scripts` filter.
