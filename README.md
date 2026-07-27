# D3D12LookDevPTWinUI

[日本語](README.ja.md)

D3D12LookDevPTWinUI is the WinUI 3 edition of the Direct3D 12 / DXR
look-development path tracer. It preserves the renderer, shader pipeline,
project schema, command line, benchmark output, MCP tools, and optional
backends from D3D12LookDevPT while replacing ImGui and the Win32 application
shell with C++/WinRT and WinUI 3.

This repository was ported from
[shaderjp/D3D12LookDevPT](https://github.com/shaderjp/D3D12LookDevPT) commit
`605fe99dc7bc42863c3d374d532ccc134b9f651d`. The renderer and HLSL baseline
are kept compatible with that revision.

## Screenshots

| Bistro Interior | Bistro Exterior |
|:---:|:---:|
| ![Bistro Interior rendered in the dark-themed WinUI editor](docs/images/screenshot001.jpg) | ![Bistro Exterior rendered in the dark-themed WinUI editor](docs/images/screenshot002.jpg) |

## Supported environment

- Windows 11 x64 with a DXR Tier-capable GPU
- Visual Studio 2026 with Desktop development with C++ and C++ WinUI tooling
- MSVC `v145`
- Windows SDK `10.0.26100.0`
- Windows App Runtime 2.3 x64
- Git with submodule support

The first release is an unpackaged, framework-dependent x64 application.
VS 2022 / `v143`, MSIX, self-contained deployment, and ARM64 are not supported.

The Visual Studio project pins these NuGet packages:

| Package | Version |
|---|---:|
| Microsoft.WindowsAppSDK | 2.3.1 |
| Microsoft.Windows.CppWinRT | 2.0.250303.1 |
| Microsoft.Direct3D.D3D12 | 1.619.3 |
| Microsoft.Direct3D.DXC | 1.9.2602.17 |

Run the setup checker before the first build:

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS -CheckNRD
```

The checker reports a clear failure if Windows App Runtime 2.3 x64 or a
required build component is missing. Install the matching Windows App Runtime
redistributable before running the unpackaged executable.

## Clone and build

Clone recursively so that all pinned third-party repositories are present:

```powershell
git clone --recursive <repository-url> D3D12LookDevPTWinUI
cd D3D12LookDevPTWinUI
git submodule update --init --recursive
```

Open `D3D12LookDevPTWinUI.sln`, select `Debug|x64` or `Release|x64`, restore
NuGet packages, and build. The project is configured for Local Windows
Debugger, so F5 launches the unpackaged WinUI executable.

The command-line equivalent is:

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe"
& $msbuild .\D3D12LookDevPTWinUI.sln /m /restore /p:Configuration=Debug /p:Platform=x64
```

The default backend configuration is `DLSS=true`, `NRD=true`, and
`RTXDI=false`. Each backend can be overridden independently:

```powershell
& $msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 `
  /p:EnableDLSS=false /p:EnableNRD=false /p:EnableRTXDI=false
```

Assimp, DirectXTex, NRD, and RTXDI are built on demand by
`BuildThirdParty.ps1`. To build the supported optional-backend matrix:

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release -SkipLaunch
```

Build output is written to `Bin\x64\<Configuration>`. The project copies the
Agility SDK, DXC-produced shaders, and available Streamline / DLSS runtimes
beside the executable.

## Run

Launch the editor with the preview cube:

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe
```

Open a scene, environment, or schema-v2 project from the Project menu, or pass
the same paths used by the original application:

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe `
  --scene .\Bistro_v5_2\BistroExterior.fbx `
  --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr

.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe `
  --project .\projects\benchmark_interactive.lookdevpt.json
```

`Bistro_v5_2` is a local test asset directory and is intentionally ignored by
git. See [Asset setup](docs/assets.md).

## WinUI editor

The fixed IDE-style layout contains the same nine editor panels as the source
application:

- Scene, Material, and Lighting on the left
- Viewport, Path Tracing, Denoise, and ReSTIR on the right
- Diagnostics and MCP in the bottom `TabView`

The left, right, and bottom regions are resizable. The View menu controls panel
visibility, Show All, Reset Default Layout, F10 Render Only mode, and the
System/Light/Dark application theme. Layout widths, selected tabs, visibility,
and the selected theme are stored in:

```text
%APPDATA%\D3D12LookDevPTWinUI\ui.json
```

Internal render resolution is independent of the window size. The 720p,
1080p, and 4K choices resize the DXR and swap-chain resources; WinUI displays
the 16:9 composition surface with aspect-fit letterboxing.

The viewport keeps the original controls:

- Right-drag: look
- W / A / S / D and Q / E: move
- Shift: fast movement
- Space: reset accumulation history
- F10: enter or leave Render Only mode
- XInput: left stick move, right stick look, triggers down/up

Camera input is active only while the viewport is focused and is suppressed
while editing a `TextBox` or `NumberBox`.

## Renderer / UI threading

`RendererController` owns the D3D12 device, scene, renderer state, and MCP
dispatcher on one `std::jthread`. The WinUI thread communicates through typed
`RendererCommand` values and consumes immutable `RendererSnapshot` instances
through `EditorViewModel`.

Continuous controls coalesce by setting and material/texture target. Ordered
actions such as load, save, approval, and rejection remain FIFO. Validation,
dirty-state changes, resource refresh, and history invalidation are applied on
the renderer thread for both WinUI and MCP.

The viewport uses a three-buffer composition swap chain created with
`CreateSwapChainForComposition`, `FLIP_SEQUENTIAL`, stretch scaling, and a
frame-latency waitable object. WinUI attaches and detaches it through
`ISwapChainPanelNative`. Shutdown first quiesces rendering, detaches the swap
chain on the UI thread, waits for the GPU, and then releases renderer
resources.

## Project, settings, and logs

Schema-v2 `.lookdevpt.json` files and all renderer CLI options remain
compatible with the source revision. A failed scene or project load leaves the
current scene active.

WinUI-specific user data is isolated from the original application:

```text
%APPDATA%\D3D12LookDevPTWinUI\settings.json
%APPDATA%\D3D12LookDevPTWinUI\startup.json
%APPDATA%\D3D12LookDevPTWinUI\materials\
%APPDATA%\D3D12LookDevPTWinUI\ui.json
%TEMP%\D3D12LookDevPTWinUI.log
```

Project paths may be absolute or relative to `baseDirectory`. See
[Asset setup](docs/assets.md) for an example.

## MCP

The MCP panel can start a local endpoint at
`http://127.0.0.1:<port>/mcp`. The bearer token is stored in the WinUI-specific
`settings.json`. Read-only, confirm-mutations, and allow-mutations modes are
supported. Confirmed mutations are approved or rejected in the MCP panel and
are executed at the same renderer-thread safe point as editor commands.

For command-line startup:

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe `
  --mcp-server --mcp-port 8777 --mcp-token <token> `
  --mcp-access confirm_mutations
```

See [MCP integration](docs/mcp.md) for tools, resources, prompts, and client
configuration.

## Benchmarks

Benchmark CLI syntax and JSON field names are unchanged:

```powershell
.\Bin\x64\Release\D3D12LookDevPTWinUI.exe `
  --project .\projects\benchmark_interactive.lookdevpt.json `
  --benchmark --benchmark-kind performance `
  --camera-path .\benchmarks\bistro_exterior_stability.camera.json `
  --frames 300 --warmup 120 --seed 1 `
  --output .\benchmark-output\performance
```

In the WinUI port, `cpu_ui_ms` measures editor command processing plus
snapshot generation. `gpu_ui_ms` measures the final swap-chain transition;
WinUI is composited separately and does not issue an ImGui GPU pass.

See [Benchmark guide](benchmarks/README.md) for captures, AOVs, and sequence
analysis.

## Tests

Run the inherited and WinUI boundary tests from PowerShell:

```powershell
.\Scripts\TestProjectPaths.ps1
.\Scripts\TestQualitySettingsJson.ps1
.\Scripts\TestTextureLoader.ps1
.\Scripts\TestBenchmarkHarness.ps1
.\Scripts\TestBenchmarkSequenceAnalyzer.ps1
.\Scripts\TestRendererCommandQueue.ps1
```

The final test covers command coalescing, FIFO barriers, indexed targets, and
atomic immutable snapshot publication.

## Third-party revisions

ImGui is not included. Other third-party repositories remain pinned to the
source baseline:

| Dependency | Commit |
|---|---|
| DLSS | `a291cc7d2cc642a51566f3dfd5376f635cd1b284` |
| DirectXTex | `0405ccf4b834404828c1b5bd54f4ae0f1554d0d5` |
| NRD | `792eff196afdd350fd9c3f862119017ccb438a0e` |
| RTXDI | `274141af082050c9d0ad6e01a2e591d0d66b7955` |
| Streamline | `e8aaa6eaac968711fb62473d4ae8256dde20919b` |
| Assimp | `e04b60f61522e1d5594ef25addcfae7cb156f085` |

The RTXDI repository records its `Libraries/Rtxdi` nested dependency at
`a14e079c727ed8c4fd3173bd2aea8244c9d9f6d6`.

## Shaders

The HLSL and HLSLI files are unchanged from the recorded source commit.
`CompileShaders` preserves the original DXC profiles, defines, incremental
inputs/outputs, `.cso` filenames, and executable-side output. No WinUI-specific
shader is added; composition scaling is handled by DXGI.

## More documentation

- [Asset setup](docs/assets.md)
- [Rendering pipeline](docs/rendering-pipeline.md)
- [MCP integration](docs/mcp.md)
- [DLSS integration](docs/dlss.md)
- [NRD integration](docs/nrd.md)
- [RTXDI integration](docs/rtxdi.md)
- [Benchmark guide](benchmarks/README.md)

## License

See [LICENSE](LICENSE). Third-party components retain their own licenses.
