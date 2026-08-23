# D3D12LookDevPTwithAI

[English](README.md)

D3D12LookDevPTwithAI は Direct3D 12 / DXR LookDev パストレーサーの
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

## 統合 AI Assistant

現在の application は、別のチャットアプリへ移動せず右側の AI Assistant から LookDev を
操作する一体型 UI です。`Inspector / AI Assistant` 切替、F9、quick prompt、会話管理、
streaming、取消、`Ctrl+Enter` 送信、読み込み済み model / backend 表示、model 起動・生成・
Tool 実行・承認待ちを示す進行表示を備えています。利用者が起動・操作する window は
`D3D12LookDevPTwithAI.exe` だけで、ChatHost と llama.cpp は所有関係を検証した非表示の
子 process として動作します。

製品既定の推論経路は、認証付き loopback 接続で非表示の `llama-server.exe` 子プロセス
を使用します。local の `inference.json` がない状態は正常な初回状態であり、Assistant
は `not_ready` を表示して placeholder 応答へ fallback しません。deterministic runtime
は Debug の end-to-end bridge test 専用で、製品 runtime の fallback ではありません。
ChatHost の会話履歴 API は SQLite の連番 cursor による UTF-8 byte 制限付き page で
取得するため、長期履歴でも 4 MiB の IPC frame 上限を超えません。Native UI が表示する
のは現時点では最新 page で、過去 page の閲覧 UI は後続 milestone です。

同一インスタンス専用 MCP transport と Native の一回承認境界は実装済みです。
ChatHost は親 Native process が所有する `127.0.0.1` endpoint だけへ接続し、
`readOnlyHint` を保持します。変更 Tool は MCP session、Tool 名、canonical 引数 hash に
束縛した30秒・一回限りの grant を必須とします。Native endpoint は不正・未認証要求を
body の buffer 前に拒否し、受信全体を10秒、body buffer を1 request 16 MiB・全体
32 MiB に制限します。専用 legacy session は server 再起動または idle expiry 後に
再交渉し、ChatHost 終了時には best-effort で削除します。llama 推論 adapter は live MCP
catalog を model へ渡し、上限付きの複数 round Tool loop を実行します。read-only Tool は
自動実行し、変更 Tool は LookDev dock に canonical 引数全文を表示して Native の一回承認を
必須とします。Tool の進行と結果は同じ画面へ表示し、SQLite へ保存するのは user message と
最終的な assistant 応答だけです。固定 revision の Gemma 4 と llama.cpp を取得する
アプリ内 setup、および手動・署名なしの一体型展示 pack は実装済みです。text chat と
Tool 操作には対応していますが、model へ viewport pixel は渡していません。scene の理解は
vision ではなく MCP state / diagnostics に基づきます。任意 model を扱う model manager と
公式署名済み artifact catalog は後続です。設計と境界は
[統合アーキテクチャ](docs/integrated-ai-architecture.ja.md)を参照してください。

### ローカル推論の setup

Assistant の `Download & set up` は、license の確認と明示同意後に固定 revision の
Gemma 4 E2B / E4B と llama.cpp b10205 の CPU / CUDA / Vulkan runtime を取得します。
画面には file 名、受信量 / 総量、全体の百分率を表示します。中断時は `.partial` を保持し、
次回同じ setup で HTTP Range download を再開します。取得物は固定 size と SHA-256 を検証し、
検証完了後にだけ `inference.json` を置き換えます。

| Model / backend の選択 | Download / 検証の進捗 |
|:---:|:---:|
| ![licenseへの明示同意を伴うGemma 4とllama.cppのsetup](docs/images/installllm002.png) | ![受信量と全体percentageを表示するlocal model download](docs/images/installllm003.png) |

model は最初の turn で遅延 load します。そのため起動直後の `Loaded model: none` は正常です。
最初の送信後は実際の model 名と runtime backend を表示します。複数行 prompt は
`Ctrl+Enter` で送信し、`Enter` だけの場合は改行を挿入します。

独自 GGUF または手元の llama.cpp runtime を使う開発時 setup は、従来どおり次の script で
import できます。

```powershell
.\Scripts\ConfigureLocalInference.ps1 `
  -ModelPath 'D:\AI\models\model-q4.gguf' `
  -RuntimePath 'D:\AI\llama-cpu\llama-server.exe' `
  -ModelId 'model-q4' `
  -Backend cpu `
  -ContextSize 16384 `
  -MaxTokens 1024 `
  -Temperature 0.2 `
  -AcceptArtifactLicenses
```

`-AcceptArtifactLicenses` は、指定 artifact を使用する操作者の明示判断を記録するもので、
入手元を認証するものではありません。既存設定を置き換える場合は
`-ReplaceConfiguration` も必要です。

script は GGUF と runtime directory 全体を
`%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI\Models` / `Runtimes` 以下へコピーし、
`inference.json` を atomic に書き込みます。設定には model、`llama-server.exe`、および
それ以外の全 runtime file を必須 `runtimeDependencies` manifest として列挙し、各 file
の size と SHA-256 を記録します。ChatHost は起動時に runtime tree の完全一致と全 entry
を検証します。子プロセスの生存中は既存 file の read lease と directory handle を保持し、
runtime tree の変更監視で追加・削除・更新を検出した場合は session を失効させて子を停止します。

非表示 server は `127.0.0.1` の ephemeral port だけへ bind し、起動ごとに新しい
256-bit API key を受け取ります。Native application は検証済み PID と kill-on-close
Job Object で ChatHost を所有し、llama.cpp の子孫プロセスも同じ所有 chain に残して
終了時に破棄します。統合 portable builder は、license と署名なし trust 境界を明示承認
した手動 artifact を同梱できます。公式署名済み artifact catalog / manifest は後続です。
ローカル SHA-256 manifest は改変検出用であり、artifact の出所を認証しません。

## スクリーンショット

![PBRT BMW M6を表示し、Gemma 4のmodel名と思考中状態を示す統合editor](docs/images/screenshot008.png)

| AIによる変更の一回承認 | アプリ内local model setup |
|:---:|:---:|
| ![露出変更の前に一回承認を待つGemma Tool call](docs/images/screenshot009.png) | ![Gemma 4とllama.cppのsetup flyoutを開いた統合AI Assistant](docs/images/installllm.png) |

| Material編集 | Lighting編集 |
|:---:|:---:|
| ![Bistro InteriorとWinUI material editor](docs/images/material.png) | ![Bistro InteriorとWinUI lighting editor](docs/images/lighting001.png) |

現在の WinUI から6種類すべての render mode、quality profile / ray budget、固定・動的
render scale、camera roll / FOV、scene load progress / cancel、material texture residency、
PBRT dielectric material、RTXDI / DLSS の詳細 status を操作・確認できます。画像内の
Bistro / PBRT sample asset は local 配置で、repository には含みません。

## 対応環境

- Windows 11 x64、DXR Tier 対応 GPU
- Visual Studio 2026、Desktop development with C++ および C++ WinUI tooling
- MSVC `v145`
- Windows SDK `10.0.26100.0`
- Windows App Runtime 2.4 x64
- ChatHost と test を build するための .NET 9 SDK。通常の build output と統合 portable
  payload はどちらも self-contained ChatHost を含むため、完成した出力の実行に別途
  .NET Runtime をインストールする必要はありません
- submodule 対応 Git

Debug / Release build は unpackaged、Windows App SDK framework-dependent です。
Release は Hybrid CRT を使用します。
VS 2022 / `v143`、MSIX、ARM64 は対象外です。

Visual Studio project は次の NuGet package を固定します。

| Package | Version |
|---|---:|
| Microsoft.WindowsAppSDK | 2.4.0 |
| Microsoft.Windows.CppWinRT | 2.0.250303.1 |
| Microsoft.Direct3D.D3D12 | 1.619.3 |
| Microsoft.Direct3D.DXC | 1.9.2602.17 |

初回 build の前に setup checker を実行してください。

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS -CheckNRD
```

Windows App Runtime 2.4 x64 や build component が不足している場合は、checker
が具体的な不足項目を表示します。unpackaged executable の起動前に、対応する
Windows App Runtime redistributable をインストールしてください。

## Clone と build

ThirdParty を固定 revision で取得するため、recursive clone します。

```powershell
git clone --recursive https://github.com/shaderjp/D3D12LookDevPTwithAI.git
cd D3D12LookDevPTwithAI
git submodule update --init --recursive
```

`D3D12LookDevPTwithAI.sln` を開き、`Debug|x64` または `Release|x64` を選択し、
NuGet restore 後に build します。Local Windows Debugger 構成済みなので、F5
で unpackaged WinUI executable を起動できます。

command line からの build 例です。

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'
& $msbuild .\D3D12LookDevPTwithAI.sln /m /restore /p:Configuration=Debug /p:Platform=x64
```

repository既定backendは`DLSS=false`、`NRD=true`、`RTXDI=false`です。各backendは
個別に上書きできます。

```powershell
& $msbuild .\D3D12LookDevPTwithAI.sln /m /p:Configuration=Release /p:Platform=x64 `
  /p:EnableDLSS=false /p:EnableNRD=false /p:EnableRTXDI=false
```

Assimp、DirectXTex、NRD、RTXDI は `BuildThirdParty.ps1` が必要時に build
します。optional backend matrix は次のコマンドで検証できます。

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release -SkipLaunch
```

出力先は `Bin\x64\<Configuration>` です。Agility SDK、DXC で生成した shader、
利用可能な Streamline / DLSS runtime も executable の横へコピーされます。

実行・コピー時は出力 directory 全体を使用してください。
`D3D12LookDevPTwithAI.exe` だけを別の場所へコピーすると、隣接する ChatHost、
self-contained .NET file、Windows App SDK、shader、renderer runtime が欠けます。
この不完全な配置では ChatHost 起動時に、実際の原因とは異なる
「.NET をインストールまたは更新してください」という dialog が出ることがあります。

## 一体型 one-app portable 展示パッケージ

`BuildIntegratedPortable.ps1` は clean な Release x64 payload を生成します。利用者が
起動するのは `D3D12LookDevPTwithAI.exe` だけで、非表示 ChatHost、.NET runtime、
Windows App SDK、Agility SDK、DXC、app-local VC runtime、license、file 単位 license map、
SPDX SBOM、整合性 manifest を同じ payload に格納します。`LocalMCPChatClient`、旧2-process
launcher、credential、承認状態、user settings、会話履歴は含めません。

PowerShell 7.4 以降が必要です。展示 build は AI 同梱を既定とし、準備済み `AI`
directory、その直下の厳密な `inference.json`、model/runtime それぞれの `name`、
`revision`、HTTPS `sourceUrl`、SPDX `licenseExpression`、`licenseFile` を持つ schema-v1
redistribution manifest を要求します。出力directoryはsource repository外に置き、
そのdirectoryとZIP/hash sidecarはいずれも事前に存在しないpathを指定します。

```powershell
.\Scripts\BuildIntegratedPortable.ps1 `
  -OutputDirectory ..\artifacts\D3D12LookDevPTwithAI-integrated-win-x64 `
  -AiArtifactDirectory 'D:\AI\LookDevPack\AI' `
  -AiArtifactManifest 'D:\AI\LookDevPack\AI\inference.json' `
  -AiRedistributionManifest 'D:\AI\LookDevPack\AI\redistribution.json' `
  -AcceptArtifactLicenses `
  -AcceptUnsignedArtifactTrust
```

builder は model/runtime を download せず、宣言済み GGUF、`llama-server.exe`、全 runtime
dependency、license document を隔離transaction staging内で検証してからpublishします。
build時の NuGet restore はoperatorが設定したfeed/cacheを使用し、この手動・署名なし経路は
それらbuild入力の
出所認証やbit単位の再現性を保証しません。生成 ZIP と manifest の SHA-256 は整合性を
示しますが署名ではないため、digest は認証済みの別経路で配布します。
同梱 `AI` は read-only input として扱い、会話履歴は現在 user の `%LOCALAPPDATA%` へ保存します。
現在の pack は text chat と同一instance MCP Tool用で、`mmproj` を含めず vision 対応を
標榜しません。`-WithoutAi` は明示的な renderer 開発用 pack で、展示既定ではありません。

bounded packaging regression は次で単独実行できます。

```powershell
.\Scripts\TestIntegratedPortable.ps1
```

公式署名済み artifact catalog、任意 model を管理する UI、署名済み製品 release は後続です。

### 旧2アプリ移行script（保守用）

`BootstrapSuite.ps1`、`BuildPortableSuite.ps1`、`BuildOfflinePack.ps1` は、外部
`LocalMCPChatClient` を使う旧 `0.2.0-beta.1` workflow の再現・移行専用として残しています。
現在の setup、release package、受け入れ対象ではありません。新規開発と展示 build は統合
Assistant と `BuildIntegratedPortable.ps1` を使用してください。これらの script が残っていても、
統合製品に2つ目の application、pairing code、projector、vision model が必要という意味では
ありません。

## 起動

preview cube で editor を起動します。

```powershell
.\Bin\x64\Debug\D3D12LookDevPTwithAI.exe
```

Project menu から PBRT / glTF / GLB / FBX / OBJ scene、environment、schema v2
project を開けます。移植元と
同じ CLI path option も使用できます。

```powershell
.\Bin\x64\Debug\D3D12LookDevPTwithAI.exe `
  --scene .\Bistro_v5_2\BistroExterior.fbx `
  --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr

.\Bin\x64\Debug\D3D12LookDevPTwithAI.exe `
  --project .\projects\benchmark_interactive.lookdevpt.json

.\Bin\x64\Debug\D3D12LookDevPTwithAI.exe `
  --scene .\pbrt-v4-scenes-master\bmw-m6\bmw-m6.pbrt
```

`Bistro_v5_2` と `pbrt-v4-scenes-master` は local test asset であり、git の対象外です。配置については
[Asset setup](docs/assets.ja.md) を参照してください。

### glTF材質とtexture経路

glTF / GLBはAssimpではなくtinygltfベースの専用経路でimportします。
`TEXCOORD_0/1`、embedded / data URI画像、texture transform、specular、IOR、
transmission、volume、clearcoat、`KHR_texture_basisu`に対応し、非破壊override用に
`gltf:material/<index>`形式の安定IDを保持します。未対応の必須extensionはimportを
停止し、任意extensionのfallbackはscene監査へ出力します。

material textureはnative BC DDS / KTX2 mip、Basis ETC1S / UASTC KTX2、EXR、HDR、
通常のdecode imageに対応します。旧512px material / HDRI制限は撤廃しました。
slotごとのAuto / Source / 4K / 2K / 1K / 512とDXGI-awareなtexture budgetを使い、
environment importance mapだけは独立した最大1024px sourceです。animation、
skinning、morph target、hierarchy編集、raster fallbackは今回の対象外です。
詳細は[Asset setup](docs/assets.ja.md)を参照してください。

## WinUI editor

固定 IDE 型 layout は renderer panel と統合 AI mode から構成されます。

- 左: Scene、Material、Lighting
- 右: Inspector の Viewport、Path Tracing、Denoise、ReSTIR
- 右の alternate mode: AI Assistant
- 下: Diagnostics、MCP の `TabView`

左、右、下領域は splitter で resize できます。View menu から panel 表示、
Show All、Reset Default Layout、F10 Render Only、および System / Light / Dark
theme を操作できます。幅、選択 tab、表示状態、選択した theme は次へ保存されます。

```text
%APPDATA%\D3D12LookDevPTwithAI\ui.json
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

schema v2 `.lookdevpt.json` と renderer CLI は移植元と互換です。schema v3は
bundle import用の`assetRoot`を追加し、通常保存はv2を維持します。scene または
project の load に失敗しても、現在の scene を維持します。

WinUI 版の user data は元アプリと競合しない専用 path に保存します。

```text
%APPDATA%\D3D12LookDevPTwithAI\settings.json
%APPDATA%\D3D12LookDevPTwithAI\startup.json
%APPDATA%\D3D12LookDevPTwithAI\materials\
%APPDATA%\D3D12LookDevPTwithAI\ui.json
%TEMP%\D3D12LookDevPTwithAI.log
```

Assistant data は current user の local profile 以下へ分離します。

```text
%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI\chat-history.sqlite3
%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI\Models\
%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI\Runtimes\
%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI\inference.json
```

project path は absolute または `baseDirectory` 基準の relative path を
使用できます。bundle内部のv3 pathは`assetRoot`以下で解決され、absolute pathや
`..`による脱出を拒否します。`Scripts\LookDevBundle.ps1`はZIPベースの`thin` /
`portable` `.lookdevbundle`をpath、容量、SHA-256、license検証付きで作成・安全に
importします。例は [Asset setup](docs/assets.ja.md) にあります。

## MCP

MCP panel から `http://127.0.0.1:<port>/mcp` の local endpoint を開始できます。
primary bearer tokenはWindows Credential Managerへ保存し、`settings.json`には
credential参照だけを残します。既存の平文設定は初回起動時に移行します。
read-only、confirm-mutations、allow-mutations に対応しています。同じ endpoint で
MCP `2026-07-28`、legacy session client、resource subscription を提供します。
confirm mode の mutation は MCP panel で Approve / Reject し、editor command と
同じ renderer-thread safe point で実行されます。

`initialize.experimental.lookdevpt.contractVersion`はcontract v1を公開し、
`lookdevpt://integration`で同じ診断機能を確認できます。MCP panelは90秒・1回限り・
5回失敗までの8桁LocalMCPChatClient pairing codeを発行し、client失効もできます。
server側は発行tokenのSHA-256 hashだけを保持します。検出とpairingもloopback限定で、
MCPと同じOrigin検証を適用します。

CLI から開始する例です。

```powershell
.\Bin\x64\Debug\D3D12LookDevPTwithAI.exe `
  --mcp-server --mcp-port 8777 --mcp-token <token> `
  --mcp-access confirm_mutations
```

tool、resource、prompt、client 設定は [MCP integration](docs/mcp.ja.md) を参照して
ください。

## Benchmark

benchmark CLI と JSON field 名は変更していません。

```powershell
.\Bin\x64\Release\D3D12LookDevPTwithAI.exe `
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

MCP clientは`start_benchmark`、`get_benchmark`、`cancel_benchmark`で同じharnessを
非同期実行し、`lookdevpt://benchmarks/{id}`を購読してCSV、quality JSON、capture、
AOV artifactを取得できます。完了または中止後はcheckpointから対話状態を復元します。

## Test

継承した test と WinUI thread 境界の test を PowerShell から実行します。

```powershell
.\Scripts\TestProjectPaths.ps1
.\Scripts\TestLookDevBundle.ps1
.\Scripts\TestOfflinePack.ps1
.\Scripts\TestQualitySettingsJson.ps1
.\Scripts\TestTextureLoader.ps1
.\Scripts\TestGltfSceneImporter.ps1
.\Scripts\TestBenchmarkHarness.ps1
.\Scripts\TestBenchmarkSequenceAnalyzer.ps1
.\Scripts\TestRendererCommandQueue.ps1
.\Scripts\TestPbrtSceneImporter.ps1
.\Scripts\TestTinyExrLoader.ps1
.\Scripts\TestTransientResourceAllocator.ps1
.\Scripts\TestMcpServer.ps1
.\Scripts\TestPbrtDxrContracts.ps1
.\Scripts\TestConfigureLocalInference.ps1
.\Scripts\TestAssistantProtocol.ps1
.\Scripts\TestAssistantHostBridge.ps1
```

`TestAssistantHostBridge.ps1` は deterministic inference hook を明示的に選択し、
private MCP factory を省略する Debug E2E test です。通常の app / ChatHost 起動では
親所有 MCP capability を必須とし、設定済みの llama.cpp 経路を使用します。
renderer command queue の test は command coalescing、FIFO barrier、
index 付き target、immutable snapshot の atomic publish を検証します。

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
| tinygltf 2.9.6 | `26422192e2908a562b641175dde18489824e609e` |
| Basis Universal 2.50 | `9bebe16726b3a61c8c213eeee3b7cffb462ef34e` |

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
- [統合 AI アーキテクチャ](docs/integrated-ai-architecture.ja.md)
- [統合公開ベータ受け入れチェックリスト](docs/public-beta-acceptance.ja.md)
- [DLSS integration](docs/dlss.ja.md)
- [NRD integration](docs/nrd.ja.md)
- [RTXDI integration](docs/rtxdi.ja.md)
- [Benchmark guide](benchmarks/README.md)

## License

[LICENSE](LICENSE) を参照してください。ThirdParty は各licenseに従います。
Portable SuiteはtinygltfのMIT license、Basis UniversalのApache-2.0 license / NOTICE
（Zstandard licenseを含む）を収集し、全同梱fileをlicense allowlistとSPDX 2.3 SBOMへ
対応付けます。設計時に`nvpro-samples/vk_gltf_renderer`を参考にしましたが、Vulkan、
`nvpro_core`、UI source codeは取り込んでいません。
