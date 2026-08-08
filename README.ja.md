# D3D12LookDevPTWinUI

[English](README.md)

D3D12LookDevPTWinUI は Direct3D 12 / DXR LookDev パストレーサーの
WinUI 3 版です。D3D12LookDevPT の renderer、shader pipeline、project schema、
CLI、benchmark 出力、MCP、optional backend を維持し、ImGui と Win32
application shell を C++/WinRT / WinUI 3 に置き換えています。

移植元は
[shaderjp/D3D12LookDevPT](https://github.com/shaderjp/D3D12LookDevPT) commit
`605fe99dc7bc42863c3d374d532ccc134b9f651d` です。現在の renderer / HLSL には、
その後 ImGui 版へ入った PBRT v4 / TinyEXR import、RTXDI ReSTIR GI / checkerboard
PT、DLSS Ray Reconstruction evaluation、dynamic internal resolution / TAAU、compact
secondary work dispatch、BLAS compaction / instancing、shader ray counter、非同期
scene load、MCP `2026-07-28` transport も合流しています。

## スクリーンショット

| Bistro Interior | Bistro Exterior |
|:---:|:---:|
| ![ダークテーマのWinUI editorで描画したBistro Interior](docs/images/screenshot001.jpg) | ![ダークテーマのWinUI editorで描画したBistro Exterior](docs/images/screenshot002.jpg) |

WinUI から6種類すべての render mode、quality profile / ray budget、固定・動的
render scale、camera roll / FOV、scene load progress / cancel、7番目の alpha texture
slot、RTXDI / DLSS の詳細 status を操作・確認できます。

## 対応環境

- Windows 11 x64、DXR Tier 対応 GPU
- Visual Studio 2026、Desktop development with C++ および C++ WinUI tooling
- MSVC `v145`
- Windows SDK `10.0.26100.0`
- Windows App Runtime 2.3 x64
- submodule 対応 Git

初版は unpackaged、framework-dependent の x64 application です。VS 2022 /
`v143`、MSIX、self-contained、ARM64 は対象外です。

Visual Studio project は次の NuGet package を固定します。

| Package | Version |
|---|---:|
| Microsoft.WindowsAppSDK | 2.3.1 |
| Microsoft.Windows.CppWinRT | 2.0.250303.1 |
| Microsoft.Direct3D.D3D12 | 1.619.3 |
| Microsoft.Direct3D.DXC | 1.9.2602.17 |

初回 build の前に setup checker を実行してください。

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS -CheckNRD
```

Windows App Runtime 2.3 x64 や build component が不足している場合は、checker
が具体的な不足項目を表示します。unpackaged executable の起動前に、対応する
Windows App Runtime redistributable をインストールしてください。

## Clone と build

ThirdParty を固定 revision で取得するため、recursive clone します。

```powershell
git clone --recursive <repository-url> D3D12LookDevPTWinUI
cd D3D12LookDevPTWinUI
git submodule update --init --recursive
```

`D3D12LookDevPTWinUI.sln` を開き、`Debug|x64` または `Release|x64` を選択し、
NuGet restore 後に build します。Local Windows Debugger 構成済みなので、F5
で unpackaged WinUI executable を起動できます。

command line からの build 例です。

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe"
& $msbuild .\D3D12LookDevPTWinUI.sln /m /restore /p:Configuration=Debug /p:Platform=x64
```

既定 backend は `DLSS=true`、`NRD=true`、`RTXDI=false` です。各 backend は
個別に上書きできます。

```powershell
& $msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 `
  /p:EnableDLSS=false /p:EnableNRD=false /p:EnableRTXDI=false
```

Assimp、DirectXTex、NRD、RTXDI は `BuildThirdParty.ps1` が必要時に build
します。optional backend matrix は次のコマンドで検証できます。

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release -SkipLaunch
```

出力先は `Bin\x64\<Configuration>` です。Agility SDK、DXC で生成した shader、
利用可能な Streamline / DLSS runtime も executable の横へコピーされます。

## 起動

preview cube で editor を起動します。

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe
```

Project menu から PBRT / glTF / GLB / FBX / OBJ scene、environment、schema v2
project を開けます。移植元と
同じ CLI path option も使用できます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe `
  --scene .\Bistro_v5_2\BistroExterior.fbx `
  --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr

.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe `
  --project .\projects\benchmark_interactive.lookdevpt.json
```

`Bistro_v5_2` は local test asset であり、git の対象外です。配置については
[Asset setup](docs/assets.ja.md) を参照してください。

## WinUI editor

固定 IDE 型 layout には移植元と同じ9 panelがあります。

- 左: Scene、Material、Lighting
- 右: Viewport、Path Tracing、Denoise、ReSTIR
- 下: Diagnostics、MCP の `TabView`

左、右、下領域は splitter で resize できます。View menu から panel 表示、
Show All、Reset Default Layout、F10 Render Only、および System / Light / Dark
theme を操作できます。幅、選択 tab、表示状態、選択した theme は次へ保存されます。

```text
%APPDATA%\D3D12LookDevPTWinUI\ui.json
```

display resolution は window size と独立しています。720p / 1080p / 4K の変更時は
output / swap-chain resource を resize し、Path Tracing panel では別に native、
fixed-scale、budget-driven dynamic internal resolution を選択できます。DLSS 以外の
scaled rendering は TAAU を使います。WinUI は16:9の composition surface を
aspect-fit、letterbox 表示します。

viewport の操作は移植元と同じです。

- 右 drag: 視点操作
- W / A / S / D、Q / E: 移動
- Shift: 高速移動
- Space: accumulation history reset
- F10: Render Only mode の切替
- XInput: 左 stick 移動、右 stick 視点、trigger 下降・上昇

camera input は viewport focus 中だけ有効で、`TextBox` や `NumberBox` の編集中は
無効になります。

## Renderer / UI の thread 境界

`RendererController` は D3D12 device、scene、renderer state、MCP dispatcher
を1本の `std::jthread` で所有します。WinUI thread は型付き `RendererCommand`
を送り、`EditorViewModel` 経由で immutable `RendererSnapshot` を読み取ります。

連続 control は setting と material / texture target 単位で coalesce します。
load、save、承認、拒否など順序が必要な action は FIFO を維持します。
validation、dirty state、resource refresh、history invalidation は WinUI と MCP
のどちらからでも renderer thread 上で適用されます。

viewport は `CreateSwapChainForComposition` で作る3-buffer composition swap
chain、`FLIP_SEQUENTIAL`、stretch scaling、frame-latency waitable object を
使用します。WinUI thread は `ISwapChainPanelNative` で attach / detach します。
終了時は renderer を静止し、UI thread で swap chain を detach、GPU wait、
renderer resource 解放の順に処理します。

## Project、設定、log

schema v2 `.lookdevpt.json` と renderer CLI は移植元と互換です。scene または
project の load に失敗しても、現在の scene を維持します。

WinUI 版の user data は元アプリと競合しない専用 path に保存します。

```text
%APPDATA%\D3D12LookDevPTWinUI\settings.json
%APPDATA%\D3D12LookDevPTWinUI\startup.json
%APPDATA%\D3D12LookDevPTWinUI\materials\
%APPDATA%\D3D12LookDevPTWinUI\ui.json
%TEMP%\D3D12LookDevPTWinUI.log
```

project path は absolute または `baseDirectory` 基準の relative path を
使用できます。例は [Asset setup](docs/assets.ja.md) にあります。

## MCP

MCP panel から `http://127.0.0.1:<port>/mcp` の local endpoint を開始できます。
bearer token は WinUI 専用 `settings.json` に保存します。read-only、
confirm-mutations、allow-mutations に対応しています。同じ endpoint で stateless
MCP `2026-07-28`、legacy session client、resource subscription を提供します。
confirm mode の mutation は MCP panel で Approve / Reject し、editor command と
同じ renderer-thread safe point で実行されます。

CLI から開始する例です。

```powershell
.\Bin\x64\Debug\D3D12LookDevPTWinUI.exe `
  --mcp-server --mcp-port 8777 --mcp-token <token> `
  --mcp-access confirm_mutations
```

tool、resource、prompt、client 設定は [MCP integration](docs/mcp.ja.md) を参照して
ください。

## Benchmark

benchmark CLI と JSON field 名は変更していません。

```powershell
.\Bin\x64\Release\D3D12LookDevPTWinUI.exe `
  --project .\projects\benchmark_interactive.lookdevpt.json `
  --benchmark --benchmark-kind performance `
  --camera-path .\benchmarks\bistro_exterior_stability.camera.json `
  --frames 300 --warmup 120 --seed 1 `
  --output .\benchmark-output\performance
```

WinUI 版の `cpu_ui_ms` は editor command 処理と snapshot 生成時間です。
`gpu_ui_ms` は最後の swap-chain transition であり、WinUI は別に composition
されるため ImGui の GPU pass はありません。

capture、AOV、sequence 解析は [Benchmark guide](benchmarks/README.md) を参照して
ください。

## Test

継承した test と WinUI thread 境界の test を PowerShell から実行します。

```powershell
.\Scripts\TestProjectPaths.ps1
.\Scripts\TestQualitySettingsJson.ps1
.\Scripts\TestTextureLoader.ps1
.\Scripts\TestBenchmarkHarness.ps1
.\Scripts\TestBenchmarkSequenceAnalyzer.ps1
.\Scripts\TestRendererCommandQueue.ps1
.\Scripts\TestPbrtSceneImporter.ps1
.\Scripts\TestTinyExrLoader.ps1
.\Scripts\TestTransientResourceAllocator.ps1
.\Scripts\TestMcpServer.ps1
.\Scripts\TestPbrtDxrContracts.ps1
```

最後の test は command coalescing、FIFO barrier、index 付き target、immutable
snapshot の atomic publish を検証します。

## ThirdParty revision

ImGui は含みません。その他は移植元と同じ revision へ固定しています。

| Dependency | Commit |
|---|---|
| DLSS | `a291cc7d2cc642a51566f3dfd5376f635cd1b284` |
| DirectXTex | `0405ccf4b834404828c1b5bd54f4ae0f1554d0d5` |
| NRD | `792eff196afdd350fd9c3f862119017ccb438a0e` |
| RTXDI | `274141af082050c9d0ad6e01a2e591d0d66b7955` |
| Streamline | `e8aaa6eaac968711fb62473d4ae8256dde20919b` |
| Assimp | `e04b60f61522e1d5594ef25addcfae7cb156f085` |
| TinyEXR | `1b106618644dbf8a0935c2348ba51a2d863dd7c2` |

RTXDI の nested dependency `Libraries/Rtxdi` は
`a14e079c727ed8c4fd3173bd2aea8244c9d9f6d6` です。

## Shader

ReSTIR GI / PT、DLSS preparation、compact secondary-work generation、TAAU、quality
counter を含む現行の共有 HLSL / HLSLI pipeline を使用します。`CompileShaders` は
全 source/output pair を追跡し、生成した `.cso` を executable 横へコピーします。
WinUI composition は DXGI 側で行い、UI 専用 shader pass は追加しません。

## 関連文書

- [Asset setup](docs/assets.ja.md)
- [Rendering pipeline](docs/rendering-pipeline.ja.md)
- [MCP integration](docs/mcp.ja.md)
- [DLSS integration](docs/dlss.ja.md)
- [NRD integration](docs/nrd.ja.md)
- [RTXDI integration](docs/rtxdi.ja.md)
- [Benchmark guide](benchmarks/README.md)

## License

[LICENSE](LICENSE) を参照してください。ThirdParty は各 license に従います。
