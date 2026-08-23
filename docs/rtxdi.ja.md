# NVIDIA RTXDI optional ReSTIR DI / GI / PT

English documentation: [Optional NVIDIA RTXDI ReSTIR DI / GI / PT](rtxdi.md)

D3D12LookDevPTwithAI には NVIDIA 公式 RTXDI SDK の optional integration があります。ReSTIR DI、ReSTIR GI、checkerboard ReSTIR PT を実装し、Sun / Environment / emissive triangle / analytic area light は共通 identity・sample・evaluate・PDF 契約を使います。

固定 revision:

- RTXDI SDK tag: `v3.0.0`
- RTXDI SDK commit: `274141af082050c9d0ad6e01a2e591d0d66b7955`
- nested `Libraries/Rtxdi` runtime commit: `a14e079c727ed8c4fd3173bd2aea8244c9d9f6d6`

## セットアップ

SDK と nested runtime の両方を初期化します。

```powershell
git submodule update --init --recursive ThirdParty/RTXDI
git -C ThirdParty/RTXDI rev-parse HEAD
git -C ThirdParty/RTXDI/Libraries/Rtxdi rev-parse HEAD
```

最後の 2 command は上記 commit を返す必要があります。strict check:

```powershell
.\Scripts\CheckSetup.ps1 -CheckRTXDI
```

統合full-NVIDIA workflowは同じtop-level / nested revision、license、GPU / application
identity、生成libraryを検証します。

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = '<NVIDIAから発行された10進ID>'
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia -Configuration Release -Build
```

安全なsubmodule初期化、外部SDK root、Release stagingは
[NVIDIA開発・Release setup](nvidia-setup.ja.md)を参照してください。

## Build switch と matrix

RTXDI は compile 時に default で無効です。

```powershell
msbuild .\D3D12LookDevPTwithAI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableRTXDI=false
```

ReSTIR DI/GI/PT backend を有効にする場合:

```powershell
msbuild .\D3D12LookDevPTwithAI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableRTXDI=true
```

有効かつ SDK が存在する場合、`BuildThirdParty.ps1` は `ThirdParty/RTXDI/Libraries/Rtxdi` だけを `Rtxdi.lib` として build します。上流のCMake fileは`add_subdirectory`用fragmentなので、固定submoduleを変更せず`CMake/RtxdiRuntime/CMakeLists.txt`からstandalone `project()` contextを与えます。app は `RTXDI_RuntimeParameters` と公式 24-byte DI、32-byte GI、64-byte PT packed reservoir ABI を検証し、RTXDI の block-linear reservoir layout を使用します。

固定 header / source がない状態で `EnableRTXDI=true` を要求すると、MSBuild は warning を出して `D3D12LOOKDEVPT_WITH_RTXDI=0` で compile します。SDK include / library link は追加せず、全 ReSTIR mode が Baseline PT fallback になります。

repositoryのbuild matrixはbackendの全組み合わせを確認し、最後にmanifest定義target構成
（`NRD=true`、`RTXDI=false`、`DLSS=false`）を残します。

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

## 現在の 2-pass DI graph

runtime が dispatch するのは、以前の 4-pass / copy chain ではなく 2 つの compute pass です。

```text
Pass A: local candidate generation + validated temporal combine -> scratch B
Pass B: adaptive spatial combine + exact current-surface evaluation
        + one final visibility ray + shading -> history/output A
```

Pass A は新規 candidate を register に保持し、immutable な previous history A を読み、packed reservoir を scratch B へ 1 回書きます。Pass B は B だけを読み、安定 surface では 4 近傍、young / disocclusion history では最大 8 近傍を使い、選択 sample の target、PDF、emissive texture、BSDF、visibility を current surface で再評価し、next-frame history を A へ直接書きます。

旧 temporal / shade shader entry point は output 名互換の no-op build artifact として残しますが、runtime は dispatch しません。full-size の物理 reservoir は 2 本で、3 本目と end-of-frame reservoir copy はありません。公開 timestamp field は互換を維持します。fused work は candidate（Pass A）と spatial（Pass B）へ計上され、temporal、shade、publish は 0 またはほぼ 0 になります。

reservoir は公式 packed ABI を使い、sample / light identity、reservoir weight、`M`、target PDF、spatial distance、cached visibility field、age を保持します。RGB 平均は reservoir として扱いません。final selected sample の visibility は必ず再 trace し、cached visibility を final shading visibility として信用しません。

## Candidate の範囲と estimator 境界

統一 candidate space は次を含みます。

- light alias-table probability で sample する emissive mesh triangle
- procedural analytic area light
- Sun
- lat-long または procedural Environment

candidate 選択は average-radiance による cheap target を使い、選択後に texture、BSDF、source PDF、final visibility を正確に再評価します。Baseline vertex も power-weighted one-light mixture を 1 回 sample し、BSDF technique と MIS します。

temporal reprojection は non-jitter 2.5D surface motion を使い、history lookup 時に current / previous jitter 差を加えます。bilinear tap ごとに depth、normal、albedo、roughness、packed surface identity を検証します。spatial reuse も同等の current-surface guide 検証を行います。通常 camera motion では reuse を維持し、camera cut、projection / resize、geometry 変更、Lighting domain invalidation では対象 history を拒否します。

## Render mode と fallback

既存 mode 名は互換を維持します。

| Render mode | 現在の実効実装 |
|---|---|
| Baseline PT | Baseline MIS path tracer |
| ReSTIR DI | RTXDI DI + Baseline indirect |
| ReSTIR GI | Baseline one-light direct + RTXDI GI |
| ReSTIR GI + DI | RTXDI DI + RTXDI GI |
| ReSTIR PT | Baseline one-light direct + checkerboard RTXDI PT |
| ReSTIR PT + DI | RTXDI DI + checkerboard RTXDI PT |

RTXDI を実行する条件は次のすべてです。

- SDK が compile され runtime ABI check に成功
- project quality が `restirBackend: "rtxdi"`
- 選択 mode の DI/GI/PT pipeline が evaluation-ready
- quality profile が `reference_still` ではない

それ以外は project mode 名の受理互換を維持したまま Baseline PT へ fallback します。`restirBackend: "off"` は RTXDI を明示的に無効化します。`reference_still` は RTXDI history を利用しません。profile 選択時には unclamped Baseline MIS accumulation も設定されますが、その後の path-tracing 編集で accumulator control を上書きできます。

UI / MCP は requested / effective state、SDK / runtime revision、`diEvaluationReady`、`giEvaluationReady`、`ptEvaluationReady`、active indirect algorithm、mode ごとの active 状態、DI/GI/PT logical reservoir byte、fallback 理由を公開します。`lookdevpt.get_state` の `restir.rtxdiStatus` で確認してください。低コストな `lookdevpt.get_stats` snapshot には、この status object を重複して含めません。

## Resource と history の挙動

- full-size reservoir は選択した interactive algorithm だけに割り当て、GI と PT は同時に resident にしません。
- DI scratch と current parity の GI/PT output は同じ heap の offset 0 に placed resource として配置し、明示的な aliasing barrier を入れます。
- Final TAA HDR output は次の A/B history resource です。通常の fused NRD/TAA path は `postDenoiseHdr` を 1x1 placeholder にします。
- 固定 A-history / B-scratch descriptor は output resource と同時に作り、GPU 実行中に書き換えません。
- Surface guide と identity は別の immutable A/B descriptor table を frame parity で選択し、以前の full-resolution guide publish copy を除去しています。
- ReSTIR shade は split Diffuse / Specular signal へ直接書きます。denoiser が signal を消費する場合、冗長な accumulation、intermediate post-denoise HDR、LDR write を省略します。

## 検証と既知の制限

fixed seed / camera path、alpha foliage、微小 emissive、高 dynamic range、camera cut、camera motion で ReSTIR DI/GI/PT を検証してください。performance benchmark は full-screen quality counter を省略し、quality / combined run は実行します。1 枚の still だけではなく high-SPP Baseline reference に対する mean energy と temporal error を比較します。

独立Primary Visibility / compact secondary task graphは、条件を満たすDXR 1.1 Interactive
Baseline / DI frameで利用できます。GI / PTと正式GI+DI gateでは使わず、現在のsecondary
taskは保存したprimary intersectionから継続せずcamera sampleを再traceします。未完了の
大項目は、現在のreconnection target shiftを超える完全なSDK相当hybrid-shift path replayと、
全必須sceneのtemporal / reference品質gateです。DLSS-RR認証は
[DLSS](dlss.ja.md)に記載した別backendの検証境界です。
