# Optional DLSS Ray Reconstruction probe

English documentation: [Optional DLSS Ray Reconstruction Probe](dlss.md)

D3D12LookDevPTWinUI には、optional で safety-first な Streamline / DLSS Ray Reconstruction integration boundary があります。現在の code は Streamline の dynamic load、D3D12 device 登録、adapter / driver support の照会、推奨 render size の取得まで行えます。rendering resource の tag 付けと DLSS-RR evaluation はまだ呼び出しません。そのため `evaluationReady` は常に false のままで、`dlss_rr` を選択しても現在は internal denoiser で描画します。

この区別は意図的です。compile / runtime detection は実装済みですが、この version の DLSS-RR は production denoising backend ではなく、RTX 4070 / 1080p60 の完了 gate にも含めません。

## 依存関係

固定 submodule:

- `ThirdParty/Streamline`: NVIDIA Streamline SDK `v2.12.0`
- `ThirdParty/DLSS`: NVIDIA DLSS SDK `v310.7.0`

初期化:

```powershell
git submodule update --init --recursive ThirdParty/Streamline ThirdParty/DLSS
```

DLSS SDK は `nvngx_dlss.dll` と `nvngx_dlssd.dll` を提供します。source-only の Streamline checkout には prebuilt runtime / feature DLL が含まれない場合があります。file が存在する場合、build は次を `Bin/x64/<Config>/Streamline/` へ copy します。

```text
sl.interposer.dll
sl.common.dll
sl.dlss.dll
sl.dlss_d.dll
nvngx_dlss.dll
nvngx_dlssd.dll
```

runtime probe は `ThirdParty/Streamline/bin/x64/` も確認します。通常の setup check では runtime DLL 不足を warning とし、strict DLSS validation の場合だけ failure にします。

```powershell
.\Scripts\CheckSetup.ps1 -CheckDLSS
```

## Build switch と matrix

DLSS header support は compile 時に default で有効です。

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64
```

Streamline / DLSS submodule がない場合や dependency-free path を検証する場合は無効化します。

```powershell
msbuild .\D3D12LookDevPTWinUI.sln /m /p:Configuration=Release /p:Platform=x64 /p:EnableDLSS=false
```

`EnableDLSS=false` では Streamline / DLSS include path と optional runtime copy を使わず、status は `compiled=false` を返し、`dlss_rr` selection は `internal` へ fallback します。

全 backend build matrix は all-enabled、no-NRD、no-RTXDI、no-DLSS、all-disabled、現在の target（`NRD=true`、`RTXDI=true`、`DLSS=false`）を含みます。

```powershell
.\Scripts\BuildBackendMatrix.ps1 -Configuration Release
```

## 現在の probe が行うこと

起動時に `DlssBackend` は次を実行します。

1. `sl.interposer.dll` を検索して dynamic load
2. 必須 Streamline core entry point を解決
3. frame-based resource tagging を要求して D3D12 用 Streamline を initialize
4. D3D12 device を登録
5. 選択 adapter / driver に対する `kFeatureDLSS_RR` support を確認
6. DLSS-RR option / optimal-settings function を解決し、Quality / Balanced / Performance / Ultra Performance mode の推奨 input size を照会

6 段階すべてに成功しても、backend は意図的に次を報告します。

```text
DLSS-RR support was detected, but resource-tag evaluation is not enabled in this build.
```

未実装なのは per-frame Streamline constant / resource tagging と `slEvaluateFeature` path です。color、depth、motion、normal、roughness、albedo、specular、exposure、reset の正確な契約も含みます。この実装と品質 gate が完了するまでは internal denoiser が実効 backend です。

## 実行時の選択と status

Denoise panel または MCP から probe を選択できます。

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

setup failure を診断できるよう、要求 backend は `dlss_rr` のまま表示し、`activeBackend` は `internal` になります。`reference_still` はこの選択にかかわらず全 real-time denoiser を無効にします。

`lookdevpt.get_state` または `lookdevpt.get_stats` の `denoise.dlss` / `denoiser.dlss` を確認してください。status は次を含みます。

- compiled、runtime-loaded、initialized、device-registered 状態
- adapter / driver feature support と `evaluationReady`
- 選択 mode、推奨 render resolution、output resolution
- runtime DLL path、last error、fallback reason
- 要求された history reset 状態

## Resource の挙動と現在の制限

evaluation が ready にならないため、この version は DLSS-RR 固有の full-resolution input / output graph を割り当てません。代わりに renderer は internal fallback resource を割り当てます。これにより非 NVIDIA GPU、未対応 driver、DLL 不足環境、`EnableDLSS=false` build でも、Streamline を process-load dependency にせず動作できます。

以前の DLSS fallback screenshot は古い Denoise panel を表示していたため、この page から削除しました。replacement は現在の quality-profile / status UI を build し、requested / effective backend field の表示を確認した後に capture してください。
