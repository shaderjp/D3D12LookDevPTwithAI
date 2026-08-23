# Scripts ガイド

[English](scripts.md)

この文書はrepository直下の`Scripts` folderにある全scriptの役割、主要input、出力、
前提条件をまとめた運用ガイドです。rendererやMCPの仕様はそれぞれの専門文書を参照し、
ここでは「どのscriptを、どの目的で実行するか」に焦点を当てます。

## 実行時の基本ルール

- 特記がなければrepository rootからPowerShellで実行します。PowerShell 7を推奨し、
  `BuildIntegratedPortable.ps1`と`TestIntegratedPortable.ps1`は7.4以降が必須です。
- C++ test/build scriptにはVisual Studio 2026、MSVC `v145`、Windows SDK
  `10.0.26100.0`が必要です。`CheckSetup.ps1`で先に確認できます。
- `-AcceptArtifactLicenses`、`-AcceptNvidiaLicense`、`-AcceptLicenses`は、operatorが
  対象licenseを確認したことを明示するswitchです。再配布権やartifactの出所認証を
  付与するものではありません。
- build/package scriptはstagingとhash検証を使いますが、既存outputを暗黙に上書き
  しないものが多いため、新しいoutput pathを指定してください。
- `Test*.ps1`は原則としてsystem temporary directory内に固有のtest rootを作り、
  所有pathを検証してからcleanupします。失敗時にartifactを残すscriptは、そのpathを
  error messageへ表示します。
- token、credential、会話履歴、承認状態をpackage inputへ混ぜないでください。

代表的な開始点:

```powershell
pwsh -NoProfile -File .\Scripts\CheckSetup.ps1 -CheckAssets
pwsh -NoProfile -File .\Scripts\SetupNvidiaEnvironment.ps1 `
  -Profile RepositoryDefault -Configuration Debug -Build
pwsh -NoProfile -File .\Scripts\TestAssistantProtocol.ps1
```

## 環境確認と構成

| Script | 用途 | 主要input、出力、注意点 |
|---|---|---|
| `CheckSetup.ps1` | repository、VS 2026/v145、SDK、submodule、optional backend、Bistro assetを診断します。 | `-Configuration`、`-CheckAssets`、`-CheckDLSS`、`-CheckNRD`、`-CheckRTXDI`。`-Json`は機械可読結果をstdoutへ出し、FAILがあればexit 1です。 |
| `ConfigureLocalInference.ps1` | 手元のGGUFと`llama-server.exe` runtime treeを検証し、local Assistant用schema-v1 `inference.json`を作ります。 | `-ModelPath`、`-RuntimePath`、`-ModelId`、`-Backend cpu|cuda|vulkan`、context/token/temperature。`-AcceptArtifactLicenses`必須。既存構成の置換には`-ReplaceConfiguration`も必要です。model/runtimeを既定では`%LOCALAPPDATA%\D3D12LookDevPTwithAI\AI`へtransactional copyし、全fileのsize/hashを記録します。 |
| `SetupNvidiaEnvironment.ps1` | `config/nvidia-dependencies.json`に従い、NVIDIA componentのrevision、layout、license、NGX IDを検査し、必要ならsubmodule初期化とbuildを行います。 | `-Profile LocalNvidia|Release|RepositoryDefault`、`-Configuration`、`-InitializeSubmodules`、`-Build`、SDK root override、`-ReportPath`、`-Json`。source変更は破棄しません。 |
| `NvidiaDependencyTools.psm1` | NVIDIA manifest読込、profile解決、root検出、診断、submodule安全確認、MSBuild property生成、runtime/license stagingを提供する共有moduleです。 | 直接実行せず`SetupNvidiaEnvironment.ps1`、`BuildBackendMatrix.ps1`、`BuildNvidiaRelease.ps1`からimportします。 |
| `InstallWindowsAppRuntime.ps1` | `suite.lock.json`で固定したMicrosoft Windows App Runtime x64 installerを検証・導入します。 | approved `aka.ms` URL、SHA-256、Microsoft Authenticode署名、`--quiet`引数を確認します。`-VerifyMetadataOnly`はdownload/installせずmetadataだけを返します。 |

## Build、package、asset bundle

| Script | 用途 | 主要input、出力、注意点 |
|---|---|---|
| `BuildBackendMatrix.ps1` | NRD / RTXDI / DLSSの有効・無効組合せをrebuildし、任意で1-frame smoke launchします。最後にrepository default構成へ戻します。 | `-Configuration Debug|Release`、`-SkipLaunch`、`-OutputRoot`。`build-matrix-summary.json`と各smoke runの`summary.json`を生成します。 |
| `BuildIntegratedPortable.ps1` | 現行one-app Release x64 payloadをtransactional stagingで作り、ChatHost、.NET、Windows App SDK、shader、runtime、license map、SPDX SBOM、hash manifestをまとめます。 | PowerShell 7.4+。`-OutputDirectory`必須。AI同梱時は`-AiArtifactDirectory`、`-AiArtifactManifest`、`-AiRedistributionManifest`、license/trust承認が必要です。`-WithoutAi`はrenderer開発用、`-NoArchive`はZIPを省略します。既定ではclean sourceと未使用output pathを要求します。 |
| `BuildNvidiaRelease.ps1` | DLSS / NRD / RTXDIを有効にした別境界のNVIDIA Release payloadを作ります。 | `-AcceptNvidiaLicense`必須。SDK root overrideと`-InitializeSubmodules`を受け、既定では`Artifacts\NvidiaRelease\...`へruntime、license、`nvidia-release-manifest.json`、launcherをstagingします。NGX Application IDの値はpayloadへ保存しません。 |
| `LookDevBundle.ps1` | `.lookdevpt.json`とassetを`thin`または`portable` `.lookdevbundle`へまとめ、安全にimportします。 | `-Command Create|Import`。Createは`-Project`、`-Output`、`-BundleMode`、asset/license mapping、`-ConfirmAssetRights`を使用します。Importは`-Bundle`、未存在の`-Destination`、thin時の`-ResolveAssetRoot`を使用し、path traversal、link、entry数、展開size、SHA-256を検証します。 |

one-app展示payloadの例:

```powershell
pwsh -NoProfile -File .\Scripts\BuildIntegratedPortable.ps1 `
  -OutputDirectory ..\artifacts\D3D12LookDevPTwithAI-integrated-win-x64 `
  -AiArtifactDirectory 'D:\AI\LookDevPack\AI' `
  -AiArtifactManifest 'D:\AI\LookDevPack\AI\inference.json' `
  -AiRedistributionManifest 'D:\AI\LookDevPack\AI\redistribution.json' `
  -AcceptArtifactLicenses -AcceptUnsignedArtifactTrust
```

詳細は[統合AIアーキテクチャ](integrated-ai-architecture.ja.md)、
[NVIDIA setup](nvidia-setup.ja.md)、[Asset setup](assets.ja.md)を参照してください。

## Benchmarkとsequence解析

| Script | 用途 | 主要input、出力、注意点 |
|---|---|---|
| `RunStabilityBenchmarks.ps1` | Bistro Exterior / Interiorの反復benchmarkを実行し、targetごとのmedian GPU p95 runを選びます。 | `-Repeat`、`-Frames`、`-Warmup`、`-Seed`、`-Scene`、`-BenchmarkKind`、`-IncludeReference`。`-CaptureEvery`と`-CaptureAovs`でsequenceを保存し、`-BackendMatrix`でrender mode × denoiserを展開します。`suite-summary.json`を生成します。 |
| `AnalyzeBenchmarkSequence.py` | capture済みRadiance HDR sequenceのtemporal luminance CV、finite値、surface mask、任意の10–90% edge幅を解析します。 | Python + NumPy。`input`、`--start`、`--count`、`--output`、`--reference-hdr`、`--enforce`。既定で`temporal-analysis.json`を書き、gate失敗時は`--enforce`で非0終了します。 |

```powershell
.\Scripts\RunStabilityBenchmarks.ps1 `
  -Scene Exterior -Repeat 3 -BenchmarkKind quality `
  -CaptureEvery 1 -CaptureAovs

python .\Scripts\AnalyzeBenchmarkSequence.py `
  .\benchmark-output\suite-<timestamp>\bistro-exterior-interactive\run-1 `
  --start 120 --count 32 --enforce
```

capture contractとmetricは[Benchmark guide](../benchmarks/README.md)を参照してください。

## Active test scripts

C++ test wrapperは`vswhere.exe`でMSVCを見つけ、必要なsourceだけをsystem temporary
directoryへcompileして実行します。第三者libraryが必要なtestは、先にmain projectの
該当configurationをbuildしてください。

| Script | 検証対象 |
|---|---|
| `TestAssistantHostBridge.ps1` | Debug ChatHostとnative named-pipe bridgeのE2E、process ownership、isolated SQLite履歴。deterministic inference hookだけを使用します。 |
| `TestAssistantProtocol.ps1` | native Assistant frame/parser protocol、size境界、不正input。 |
| `TestBenchmarkHarness.ps1` | benchmark CLI/harness、artifact/summary contract。 |
| `TestBenchmarkSequenceAnalyzer.ps1` | `Tests/AnalyzeBenchmarkSequenceTests.py`によるHDR/DDS reader、mask、CV、edge解析。`-PythonExecutable`でNumPy環境を指定できます。 |
| `TestConfigureLocalInference.ps1` | local inference staging、hash/manifest、replace policy、reparse/path攻撃、atomic cleanup。 |
| `TestGltfSceneImporter.ps1` | tinygltf経路のscene/material/extension/import contract。 |
| `TestIntegratedPortable.ps1` | one-app builderのbounded regression、payload closure、AI manifest、license/SBOM、transaction、競合・rollback。PowerShell 7.4+です。 |
| `TestLookDevBundle.ps1` | thin/portable bundleのcreate/import、hash、path traversal、license確認。 |
| `TestMcpConformance.ps1` | official `@modelcontextprotocol/conformance` suiteを一時hostへ接続します。`pnpm`または`npx`が必要で、`-Scenario`、`-Suite`、expected-failure baselineを指定できます。 |
| `TestMcpReviewAnalysis.ps1` | scene audit/review analysisのnative unit contract。 |
| `TestMcpServer.ps1` | MCP JSON-RPC、session、tool/resource/prompt、承認などのnative server tests。 |
| `TestMcpServerHttp.ps1` | HTTP framing、chunked body、timeout、size/depth/UTF-8/security境界。 |
| `TestNvidiaDependencyTools.ps1` | NVIDIA manifest/profile/root/module helper。 |
| `TestPbrtDxrContracts.ps1` | PBRT/DXR source contractを静的patternで検査します。 |
| `TestPbrtSceneImporter.ps1` | PBRT importer。`-ScenePath`を省略するとfixture tests、指定すると追加sceneを読み込みます。事前にRelease x64 Assimp libraryが必要です。 |
| `TestProjectPaths.ps1` | project/scene/environmentのrelative/absolute path解決と安全境界。 |
| `TestQualitySettingsJson.ps1` | quality profileとJSON serialization/validation。 |
| `TestRendererCommandQueue.ps1` | command coalescing、FIFO barrier、indexed target、immutable snapshot publish。 |
| `TestTextureLoader.ps1` | texture decode/upload policy、format/path/security contract。事前にRelease x64 DirectXTex libraryが必要です。 |
| `TestTinyExrLoader.ps1` | TinyEXR loaderのformat、validation、failure contract。 |
| `TestTransientResourceAllocator.ps1` | transient resource allocation、aliasing、budget/lifetime contract。 |

Managed chat testsはPowerShell wrapperではなく次で実行します。

```powershell
dotnet test .\Managed\Tests\D3D12LookDevPTwithAI.Chat.Tests\D3D12LookDevPTwithAI.Chat.Tests.csproj `
  --configuration Release
```

## 旧2アプリportable suite用script

次のscriptは外部`LocalMCPChatClient`を組み合わせる旧`0.2.0-beta.1` workflowの再現、
移行、過去payloadの保守用です。現行の統合Assistantやone-app展示packの導入には使用せず、
新規packageは`BuildIntegratedPortable.ps1`を使用してください。

| Script | 旧workflowでの役割 |
|---|---|
| `BootstrapSuite.ps1` | `suite.lock.json`の固定commitへ外部LocalMCPChatClientとsubmoduleを揃え、任意でWinGet prerequisite、build、integration testを実行します。 |
| `BuildPortableSuite.ps1` | rendererとLocalMCPChatClientの2-app payload、launcher、manifest、ZIP/hashを生成します。 |
| `BuildOfflinePack.ps1` | 旧portable suiteへCPU/CUDA/Vulkan model/runtime artifactを追加し、offline manifestとZIPを生成します。 |
| `InstallPortableSuite.ps1` | local/HTTPS ZIPをbounded stagingへ展開し、任意SHA-256とsuite manifestを検証してcurrent userの`LocalAppData\Programs`へinstallします。`-Force`時も旧installをtimestamp付きbackupとして残します。 |
| `InstallOfflinePack.ps1` | offline artifactのmanifest/hashとGPU適合を検査し、旧LocalMCPChatClient data rootへmodel/runtimeをcopyします。`-AcceptLicenses`必須です。 |
| `TestLocalMcpChatClientIntegration.ps1` | 外部repositoryのspecific .NET integration testを一時native MCP hostへ接続します。 |
| `TestOfflinePack.ps1` | offline pack build/install、license gate、sensitive-data除外。 |
| `TestPortablePrerequisites.ps1` | disk、OS、architecture、lock metadataなど旧payloadの導入前提を確認し、任意でJSON出力します。 |
| `TestSuiteLicenseCompliance.ps1` | 旧suite payloadのallowlist、license map、SHA-256、SPDX 2.3 SBOMを検証し、`-Generate`で台帳を生成します。 |
| `UninstallPortableSuite.ps1` | 旧per-user installを`ShouldProcess`確認付きで削除します。`-RemoveLocalApplicationData`は設定、artifact、履歴まで削除するため、明示した場合だけ使用します。 |

## 変更時の確認

scriptを追加・rename・削除した場合は、この一覧とREADMEの導線を同時に更新してください。
scriptをVisual Studio project itemとして追加する場合は、
`D3D12LookDevPTwithAI.vcxproj`と`D3D12LookDevPTwithAI.vcxproj.filters`の
`Build Scripts` mappingも一対一で更新します。
