# NVIDIA 開発・Release setup

renderer の NVIDIA 連携は DLSS / Streamline、NRD、RTXDI の3系統です。AI Assistant
で使う CUDA 版 llama.cpp は利用者が別途導入する artifact であり、この workflow では
build・再配布しません。

このworkflowでは`config/nvidia-dependencies.json`を、固定submodule revision、必須
header / library、runtime DLL、license、build profileの唯一の定義にしています。
`SetupNvidiaEnvironment.ps1`はこのmanifestを読み、別のdependency version一覧を持ちません。

## Profile

| Profile | DLSS | NRD | RTXDI | 用途 |
|---|---:|---:|---:|---|
| `RepositoryDefault` | Off | On | Off | 通常project buildとCI sanity check |
| `LocalNvidia` | On | On | On | NVIDIA workstationでの開発・feature-active検証 |
| `Release` | On | On | On | 監査可能なRelease staging。license確認必須 |

`LocalNvidia`は`nvidia-smi`から見えるNVIDIA GPUと10進NGX Application IDを必須に
します。`Release`はGPUのないbuild agentを許可しますが、配布するDLSS構成に必要な
application identityをrelease担当者が所有することを確認するためNGX IDは必須です。
公開前には対象NVIDIA machineで別途実機検証してください。

## Local setup

NVIDIA機能をすべて使うlocal環境は次で検査できます。

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = '<NVIDIAから発行された10進ID>'
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia -InitializeSubmodules
```

`-InitializeSubmodules` は明示指定時だけ実行され、tracked な local 変更がある NVIDIA
submodule は更新を拒否します。すでに SDK がある場合は省略してください。repository 外の
SDK は `-StreamlineRoot`、`-DlssRoot`、`-NrdRoot`、`-RtxdiRoot` で指定できます。

たとえばvendor SDKをcheckout外へ置き、project fileを変更せず利用できます。

```powershell
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia `
  -StreamlineRoot 'D:\NVIDIA\Streamline' `
  -DlssRoot 'D:\NVIDIA\DLSS' `
  -NrdRoot 'D:\NVIDIA\NRD' `
  -RtxdiRoot 'D:\NVIDIA\RTXDI'
```

全 backend を build し、生成 library / runtime まで再検査する例です。

```powershell
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia -Configuration Debug -Build
```

`-Build`なしではGPU / driver、NGX ID、SDK root、必須開発file、license、top-level
revision、nested submodule状態を検査します。`-Build`を付けるとmanifestからMSBuild
propertyを生成してsolutionをbuildし、生成NRD / RTXDI libraryと必須DLSS runtime DLLも
検査します。Streamline feature DLLは存在するものだけをcopyし、source-only checkoutに
ない場合はdocumented direct NGX / native fallbackを維持します。

NGX値を出さずmachine-readable reportを保存できます。

```powershell
.\Scripts\SetupNvidiaEnvironment.ps1 -Profile LocalNvidia `
  -ReportPath .\Obj\NvidiaSetup\local.json -Json
```

NGX Application ID は process の環境変数からのみ読みます。report には有効な10進値が
存在するかだけを記録し、値自体は保存しません。

## Release payload

3 backend を有効にした x64 Release payload は次で作成します。

```powershell
$env:D3D12LOOKDEVPT_NGX_APPLICATION_ID = '<NVIDIAから発行された10進ID>'
.\Scripts\BuildNvidiaRelease.ps1 -AcceptNvidiaLicense
```

この switch は担当者が利用条件を確認したことを明示するだけで、再配布権を付与・保証
しません。公開前に `Licenses/NVIDIA` 以下へコピーされた全 license を確認してください。
builder は既存出力を上書きせず、一時 directory で staging し、symbol / validation binary
を除外して、全 file の size / SHA-256 を `nvidia-release-manifest.json` に記録します。
NGX ID は payload に含めません。

staging出力には次を含めます。

- Release rendererとself-contained ChatHost
- app-local Windows App SDK、Agility SDK、shader
- 必須DLSS runtime DLLと、存在するStreamline runtime DLL
- component別のNVIDIA license document
- `Launch-NVIDIA.ps1`と`README-NVIDIA.txt`
- dependency revision、backend状態、source dirty状態、prerequisite、file単位SHA-256を
  記録した`nvidia-release-manifest.json`

Native Windows App SDK runtimeは自己完結で同梱します。対象PCにはv145 toolset互換の
最新Microsoft Visual C++ x64 Redistributableを導入し、NGX環境変数を設定して
`Launch-NVIDIA.ps1` から起動してください。

生成hashはfile変更の検出用で、code signatureではなくSDK/runtimeの出所を認証しません。

### Release checklist

1. manifest固定revisionのNVIDIA license fileを読み、予定する配布が許可されることを確認します。
2. cleanなsource checkoutを使います。local buildはrelease manifestへ`sourceDirty`を記録し、
   CI workflowはdirty source treeを拒否します。
3. NGX Application IDはbuild processの環境変数だけへ設定します。
4. 新しい出力pathと明示license確認を指定して`BuildNvidiaRelease.ps1`を実行します。
5. `README-NVIDIA.txt`、`Licenses/NVIDIA`以下の全file、
   `nvidia-release-manifest.json`のdependency revision / prerequisiteを確認します。
6. 転送前にfile台帳を再計算します。

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

7. 代表的な対象machineへVC++ prerequisiteを導入し、NGX環境変数を設定して
   `Launch-NVIDIA.ps1`から起動します。UIのrequested状態だけでなく、DLSS / NRD / RTXDIの
   effective statusを確認します。
8. license、対象GPU、digest review完了後だけ公開し、最終archive digestは認証済み経路で
   配布します。

従来の `BuildIntegratedPortable.ps1` は NVIDIA renderer backend を含まない vendor-neutral
な展示 pack のまま維持します。通常の portable build で NVIDIA runtime の再配布が暗黙に
発生しないための分離です。

CI では手動 `release-nvidia` workflow が同じ builder を使います。repository secret
`NVIDIA_NGX_APPLICATION_ID` を設定し、license 確認を有効にして実行後、upload された
internal artifact を確認してください。workflow は GitHub Release を自動公開しません。

## Troubleshooting

- **Dirty submodule refusal:** 表示されたsubmodule変更を保存するか、意図を確認して復元して
  から`-InitializeSubmodules`を使用してください。setup scriptは変更を破棄しません。
- **NGX Application ID failure:** `D3D12LOOKDEVPT_NGX_APPLICATION_ID`へNVIDIAが発行した
  10進IDを設定します。manifest、project file、repositoryには書き込みません。
- **NRD / RTXDI library不足:** 同じprofileへ`-Build`を付けて再実行します。生成先は
  configurationごとに異なります。
- **Streamline feature DLL warning:** source-only checkoutにはprebuilt feature DLLがない
  場合があります。必要なintegration pathでは承認済み外部SDK rootを指定してください。
- **Release出力が存在する:** 新しい`-OutputDirectory`を指定するか、既存出力を明示的に
  archive / 削除してください。builderは上書きしません。
- **CIにGPUがない:** `Release` profileでは許容します。DLSS / NRD / RTXDIのfeature-active
  検証は対象GPU / driverで別途実行してください。

## CUDAとの境界

ここで扱うrenderer連携はDirect3D 12を使い、CUDA Toolkitのinstallを必要としません。
AI AssistantのCUDA版llama.cppは別のアプリ内artifact setupです。そのruntime、license、
integrity manifestを`SetupNvidiaEnvironment.ps1`は使用せず、`BuildNvidiaRelease.ps1`も
同梱しません。
