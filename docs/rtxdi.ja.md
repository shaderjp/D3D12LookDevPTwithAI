# NVIDIA RTXDI optional ReSTIR DI

English documentation: [Optional NVIDIA RTXDI ReSTIR DI](rtxdi.md)

D3D12LookDevPTWinUI には NVIDIA 公式 RTXDI SDK の optional integration boundary があります。現時点では renderer の local mesh emitter / analytic area light list に対する ReSTIR DI を実装しています。ReSTIR PT / GI は未統合で、Sun / Environment sampling も RTXDI candidate set へまだ統合していません。

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

## Build switch と matrix

RTXDI は compile 時に default で無効です。

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableRTXDI=false
```

現在の ReSTIR DI backend を有効にする場合:

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableRTXDI=true
```

有効かつ SDK が存在する場合、`BuildThirdParty.ps1` は `ThirdParty/RTXDI/Libraries/Rtxdi` だけを `Rtxdi.lib` として build します。app は公式 runtime ABI（`RTXDI_RuntimeParameters` と 24-byte の `RTXDI_PackedDIReservoir`）を検証し、RTXDI の block-linear reservoir layout を使用します。

固定 header / source がない状態で `EnableRTXDI=true` を要求すると、MSBuild は warning を出して `D3D12LOOKDEVPT_WITH_RTXDI=0` で compile します。SDK include / library link は追加せず、全 ReSTIR mode が Baseline PT fallback になります。

repository の build matrix は backend の全組み合わせを確認し、最後に target 構成（`NRD=true`、`RTXDI=true`、`DLSS=false`）を残します。

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

現在の RTXDI candidate list は renderer の `RtLight` table です。

- light alias-table probability で sample する emissive mesh triangle
- procedural analytic area light

target は評価済み Diffuse + Specular contribution の正の luminance です。現在は各 candidate について material / emissive texture と BSDF を評価します。計画中の cheap average-radiance target は未実装です。Sun と lat-long Environment lighting は引き続き Baseline path-tracing technique を使い、secondary one-light mixture も RTXDI へまだ統合していません。

temporal reprojection は non-jitter 2.5D surface motion を使い、history lookup 時に current / previous jitter 差を加えます。bilinear tap ごとに depth、normal、albedo、roughness、packed surface identity を検証します。spatial reuse も同等の current-surface guide 検証を行います。通常 camera motion では reuse を維持し、camera cut、projection / resize、geometry 変更、Lighting domain invalidation では対象 history を拒否します。

## Render mode と fallback

既存 mode 名は互換を維持します。

| Render mode | 現在の実効実装 |
|---|---|
| Baseline PT | Baseline MIS path tracer |
| ReSTIR DI | local direct light に RTXDI DI + Baseline indirect / Sun / Environment |
| ReSTIR GI | Baseline MIS path tracer。RTXDI GI / PT は未統合 |
| ReSTIR GI + DI | local direct light に RTXDI DI + Baseline indirect / Sun / Environment |

RTXDI を実行する条件は次のすべてです。

- SDK が compile され runtime ABI check に成功
- project quality が `restirBackend: "rtxdi"`
- mode が DI を使用
- quality profile が `reference_still` ではない

それ以外は project mode 名の受理互換を維持したまま Baseline PT へ fallback します。`restirBackend: "off"` は RTXDI を明示的に無効化します。`reference_still` は RTXDI history を利用しません。profile 選択時には unclamped Baseline MIS accumulation も設定されますが、その後の path-tracing 編集で accumulator control を上書きできます。

UI / MCP は requested / effective state、SDK / runtime revision、`diEvaluationReady`、`giEvaluationReady`、mode ごとの active 状態、fallback 理由を公開します。`lookdevpt.get_state` の `restir.rtxdiStatus` で確認してください。低コストな `lookdevpt.get_stats` snapshot には、この status object を重複して含めません。

## Resource と history の挙動

- full-size reservoir は interactive DI mode が RTXDI を利用できる場合だけ割り当て、それ以外は descriptor-valid な placeholder を使います。
- 固定 A-history / B-scratch descriptor は output resource と同時に作り、GPU 実行中に書き換えません。
- Surface guide と identity は別の immutable A/B descriptor table を frame parity で選択し、以前の full-resolution guide publish copy を除去しています。
- ReSTIR shade は split Diffuse / Specular signal へ直接書きます。denoiser が signal を消費する場合、冗長な accumulation、intermediate post-denoise HDR、LDR write を省略します。

## 検証と既知の制限

fixed seed / camera path、alpha foliage、微小 emissive、高 dynamic range、camera cut、camera motion で ReSTIR DI を検証してください。performance benchmark は full-screen quality counter を省略し、quality / combined run は実行します。1 枚の still だけではなく high-SPP Baseline reference に対する mean energy と temporal error を比較します。

2-pass DI は動作しますが、次の計画項目は未完了です。RTXDI ReSTIR PT / GI、Sun / Environment の candidate table 統合、cheap candidate target、統一 secondary one-light estimator、全必須 scene に対する最終 1080p60 品質 gate。
