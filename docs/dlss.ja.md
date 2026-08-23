# Optional DLSS Ray Reconstruction

English documentation: [Optional DLSS Ray Reconstruction](dlss.md)

D3D12LookDevPTwithAI には optional の Streamline / DLSS Ray Reconstruction backend があります。Streamline の dynamic load、adapter/driver 検証、render/output 解像度の分離、guide resource の生成と frame tag、renderer の D3D12 command list 内での `slEvaluateFeature` まで実装しています。

DLSS-RR は RTX 4070 / 1080p60 の ReSTIR GI+DI gate とは独立です。binary 不足、非対応 adapter/driver、application identity 不足、runtime evaluation failure の場合は、Streamline を process-load dependency にせず native reconstruction を使用します。

## 依存関係と application identity

固定 submodule:

- `ThirdParty/Streamline`: NVIDIA Streamline SDK `v2.12.0`
- `ThirdParty/DLSS`: NVIDIA DLSS SDK `v310.7.0`

checked-in NVIDIA manifestを使う統合setupで初期化・検証します。

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = '<NVIDIAから発行された10進ID>'
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia -InitializeSubmodules
```

初期化は明示指定時だけ行い、trackedなlocal変更を持つsubmoduleの更新を拒否します。
外部SDK root、JSON report、Release stagingは
[NVIDIA開発・Release setup](nvidia-setup.ja.md)を参照してください。

DLSS SDK は `nvngx_dlss.dll` と `nvngx_dlssd.dll` を提供します。source-only の Streamline checkout には prebuilt runtime / feature DLL がすべて含まれない場合があります。存在する file は `Bin/x64/<Config>/Streamline/` へ copy します。

```text
sl.interposer.dll
sl.common.dll
sl.dlss.dll
sl.dlss_d.dll
nvngx_dlss.dll
nvngx_dlssd.dll
```

production NGX component には NVIDIA が発行した application identity も必要です。10進数 ID は source control の外で管理し、起動前に設定します。

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = "<NVIDIA 発行の10進数 ID>"
```

renderer は temporary ID や架空の ID を代用しません。変数がない場合は `applicationIdentityConfigured=false`、failure stage `applicationIdentity` を報告し、native reconstruction を使用します。

従来のcomponent単位checkerは簡易file checkとして利用できます。

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS
```

manifest駆動setupはこれに加え、GPU / driver、application identity、固定・nested revision、
license、build後の出力も検証します。

## Build switch と matrix

repository既定ではDLSSは無効です。明示的に有効化するか、`LocalNvidia` / `Release`
profileを使用します。

```powershell
msbuild .\D3D12LookDevPTwithAI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableDLSS=true
```

dependency-free path を検証する場合は無効化します。

```powershell
msbuild .\D3D12LookDevPTwithAI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableDLSS=false
```

`EnableDLSS=false` では Streamline/DLSS include path と optional runtime copy を使わず、status は `compiled=false` を返し、`dlss_rr` は native internal reconstruction へ fallback します。

backend matrix は all-enabled、no-NRD、no-RTXDI、no-DLSS、all-disabled、repository target を含みます。

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

最後のrepository targetは`config/nvidia-dependencies.json`から読み、現在は
`DLSS=false`、`NRD=true`、`RTXDI=false`です。

## Frame evaluation

初期化時は次を行います。

1. `sl.interposer.dll` を load し、必須 core function を解決
2. frame-based resource tagging を有効にして D3D12 用 Streamline を initialize
3. D3D12 device を登録
4. NVIDIA 発行 application identity を検証
5. 選択 adapter/driver の `kFeatureDLSS_RR` support を確認
6. 推奨 render size を取得し、選択 DLSS mode を設定

eligible な各 frame で `PathTracingDlss.hlsl` は次を準備します。

- HDR scaling-input color
- 正の linear depth
- 2D motion vector
- packed world normal / roughness
- diffuse albedo / specular albedo
- 1x1 exposure resource

backend は camera matrix、jitter、reset、render/output extent、8個の resource tag を設定し、active command list 内で `kFeatureDLSS_RR` を evaluate します。成功すると display-resolution HDR を生成し、NRD、internal denoiser、Final TAA を迂回して tone map します。

evaluation が失敗した場合、部分出力は表示しません。同じ frame は native reconstruction し、以後の evaluation を無効化して次 frame の native resource rebuild を要求します。resize、camera cut、明示 reset は DLSS history reset へ伝播します。

## 解像度と runtime status

quality schema は render/output 解像度を分離します。

```json
{
  "resolutionMode": "dynamic",
  "fixedRenderScale": 0.75,
  "minRenderScale": 0.5,
  "maxRenderScale": 1.0
}
```

`resolutionMode` は `native`、`fixed`、`dynamic` を受理し、旧 project は `native` になります。Dynamic は既存 GPU budget と settle hysteresis を使い、1/16 刻みで調整します。非 DLSS 経路で render/output size が異なる場合は TAAU を使い、NRD は render 解像度で動作します。

Denoise panel または MCP から選択できます。

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

`lookdevpt.get_state` / `lookdevpt.get_stats` の `denoise.dlss` / `denoiser.dlss` は compile/load/init/device/application-identity/support/evaluation 状態、推奨・active 解像度、成功/失敗回数、last result code/failure stage、runtime path、error、fallback reason を公開します。benchmark にも同じ activation/failure evidence を出力します。

| DLSS availability設定オン | DLSS availability設定オフ |
|:---:|:---:|
| ![DLSS Enabled When AvailableをオンにしてDLSS Ray Reconstructionを選択](images/nvidiadlssreyareconstruct.png) | ![DLSS Enabled When AvailableをオフにしてDLSS Ray Reconstructionを選択](images/dlssrayreconstructwithoutdlss.png) |

これらは意図的にnegative runtime stateを示します。DLSS Ray Reconstructionはrequested
backendのままですが、status blockはbuild時に無効だったこととnative reconstructionへの
fallbackを明示しています。`DLSS Enabled When Available`はbackend selectorと独立して保存
されるため、requested / active backendはstatus / state fieldで判定します。production-ready
の記録ではinitializeとevaluationの成功evidenceが必要です。全比較は
[Denoise UIとfallback比較](denoise-ui.ja.md)を参照してください。

compile成功だけをfeature-active認証と解釈しないでください。発行済みNGX ID、対応
GPU / driver、承認済みproduction runtime DLLを使い、`LocalNvidia` profileとapplication
固有の品質・failure matrixを実行してください。
