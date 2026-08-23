# アセットの配置と実行時の挙動

English documentation: [Asset Setup And Runtime Behavior](assets.md)

D3D12LookDevPTwithAI は大きな scene asset、HDRI、高解像度 texture set を repository で配布しません。test content は solution の横へ置き、git の対象外にしてください。documentation screenshot に local の Bistro asset が写る場合がありますが、元の scene data は repository に含みません。

## Bistro の推奨配置

必須の exterior / interior 検証 scene には、[NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) から Amazon Lumberyard Bistro を取得してください。download page では CC-BY 4.0 と案内されています。screenshot や派生成果を公開する場合は配布元の attribution 条件に従ってください。

solution の横へ次の layout で展開します。

```text
D3D12LookDevPTwithAI/
  Bistro_v5_2/
    BistroExterior.fbx
    BistroInterior.fbx
    BistroInterior_Wine.fbx
    san_giuseppe_bridge_4k.hdr
    Textures/
```

`Bistro_v5_2/` は repository の ignore 対象です。Bistro material resolver は最初に `Textures/` を検索し、その後 renderer の best-effort な material-name 規則を適用するため、archive の相対 `Textures/` 構造を維持してください。

local asset を検証します。

```powershell
.\Scripts\CheckSetup.ps1 -CheckAssets
```

strict check が必須にするのは `BistroExterior.fbx` と `Textures/` です。interior FBX と HDRI は optional のままなので、完全な validation package がなくても app を build できます。

## PBRT v4 の推奨配置

PBRT validation scene も local asset として扱います。相対 `Include`、PLY、texture path を
配布元の構造のまま解決できるよう、展開した `pbrt-v4-scenes` tree を solution の横へ置きます。

```text
D3D12LookDevPTwithAI/
  pbrt-v4-scenes-master/
    bmw-m6/bmw-m6.pbrt
    crown/crown.pbrt
    ...
```

`pbrt-v4-scenes-master/` は git の ignore 対象です。この repository は scene 自体を再配布
しません。配布 archive に含まれる license / attribution file を維持してください。
`Project > Open Scene...` または次の CLI で開けます。

```powershell
.\Bin\x64\Release\D3D12LookDevPTwithAI.exe `
  --scene .\pbrt-v4-scenes-master\bmw-m6\bmw-m6.pbrt
```

## 起動と project file

scene と environment を直接指定して起動できます。

```powershell
.\Bin\x64\Release\D3D12LookDevPTwithAI.exe `
  --scene .\Bistro_v5_2\BistroExterior.fbx `
  --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr
```

interactive load には `Project > Open Scene...` と `Project > Open Environment...` を使います。`Project > Save Startup Settings` は `%APPDATA%\D3D12LookDevPTwithAI\startup.json` を書きます。command-line の `--scene` / `--environment` はその起動に限って startup setting を上書きします。

startup file 例:

```json
{
  "version": 1,
  "enabled": true,
  "baseDirectory": "C:/Projects/D3D12LookDevPTwithAI",
  "scenePath": "Bistro_v5_2/BistroExterior.fbx",
  "environmentPath": "Bistro_v5_2/san_giuseppe_bridge_4k.hdr",
  "environmentEnabled": true
}
```

別の startup file は `--startup-config <path>` で選択します。`projects/` の checked-in file は相対 Bistro path と renderer setting を含みますが、asset 自体は含みません。

## 対応 input と scene scope

対象とする static-mesh scene input:

- glTF / GLB
- FBX
- OBJ
- PBRT v4

glTF / GLB は tinygltf 2.9.6 に固定した専用 `GltfSceneImporter` で読み込みます。material index は `gltf:material/<index>` として保持し、node transform はload時に頂点へ適用し、`TEXCOORD_0` / `TEXCOORD_1`を対応付けます。画像は相対path、data URI、GLB buffer viewだけを受け付け、HTTP画像とscene directory外へ抜けるpathは拒否します。FBX / OBJ は既存経路、PBRT は専用 `PbrtSceneImporter` を使います。

PBRT 経路は `diffuse`、`coateddiffuse`、`conductor`、`coatedconductor`、
`dielectric`、`thindielectric`、`diffusetransmission` material を対応付けます。smooth
dielectric は正確な Fresnel / Snell 屈折、thin dielectric は透過方向を維持した2界面 Fresnel
を使用します。rough dielectric や未対応 PBRT feature は、完全対応を装わず scene audit に
fallback 診断を出します。transport の詳細と estimator の制約は
[Rendering pipeline](rendering-pipeline.ja.md)を参照してください。

初期glTF材質は `KHR_texture_transform`、`KHR_materials_specular`、`KHR_materials_ior`、`KHR_materials_transmission`、`KHR_materials_volume`、`KHR_materials_clearcoat`、`KHR_texture_basisu` に対応します。`extensionsRequired`内の未対応拡張はimportを停止し、任意拡張はcore materialへfallbackしてscene監査へ記録します。animation、skinning、morph target、連続deformation、moving-instance transformは未対応で、raster fallbackもありません。geometry / topology editはhistory invalidationを伴う変更として扱います。

material / environment texture path が対応する形式:

- PNG
- JPEG / JPG
- TGA
- BMP
- DDS
- HDR
- EXR
- KTX2（native BC mip、およびBasis Universal ETC1S / UASTC）

KTX1と単体`.basis` fileは対象外です。欠損、破損、巨大dimension、remote画像、load失敗時はslotごとのfallback値を使います。material textureのfile existenceはload時にcacheし、毎frameの確認は行いません。

## Texture upload、mip、alpha

D3D12 upload path は DDS と decode image で挙動が異なります。

- 通常DDSとnative-format KTX2は対応BC compressionとauthored mip chainを維持
- BasisLZ / UASTC KTX2はCPUでBC7、HDR dataはBC6Hへtranscodeし、圧縮targetを使えない場合はRGBAへfallback
- decoded material imageの旧512px制限を撤廃。既定resident上限は4Kで、slotごとにAuto / Source / 4K / 2K / 1K / 512を指定可能
- renderable HDR / EXR environmentは最大辺16384まで保持可能。分離したimportance sourceだけ最大1024px
- base color、emissive、specular colorはsRGB、normalとscalar mapはlinear
- ray-cone footprint から material、emissive、alpha、environment の明示 mip LOD を計算

Auto residencyは専用VRAMの25%を基準にtexture budgetを512 MiB〜4 GiBへclampし、現在利用できるDXGI local-memory budgetも超えないようにします。source / resident解像度、transcode format、resident bytes、fallback理由はdiagnosticsとMCP監査へ出力します。現時点ではscene / material resource再構築時にresident mip rangeを選びます。camera優先度を使う完全非同期streamingは後続です。

alpha-masked base-color texture は material cutoff に合わせた coverage-preserving mip を生成します。alpha-masked DDS は coverage を変更できる writable RGBA8 へ decode するため、BC block は維持しません。`alphaMasked` の変更は DXR geometry opacity を変えるため acceleration-structure rebuild を queue します。alpha cutoff または base-color alpha path の変更では、opaque geometry と同等とみなさず、関連 material texture data を再生成します。

non-alpha geometry は `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE` になり、通常 surface の AnyHit を省略します。alpha foliage と細い cutout は non-opaque のままで、ray footprint から選んだ mip に authored cutoff を適用します。

## Material texture slot

ABIを維持する先頭7 slotはbase color、normal、roughness、metallic、occlusion、emissive、alphaです。続けてspecular color、specular factor、transmission、thickness、clearcoat、clearcoat roughness、clearcoat normalを追加しました。bindingごとにUV set、offset、scale、rotation、sampler preset、resident resolution policyを保持します。scene全体のmaterial feature bitfieldでshader variantを選び、material buffer layoutは共通です。

path tracerはIORからdielectric F0を求め、glTF specular factor / color、transmission、Beer–Lambert volume吸収を評価し、固定IOR 1.5のGGX clearcoat層を下層lobeへ重ねます。Debug ViewにはSpecular F0、Transmission、Thickness / Attenuation、Clearcoat、UV Setを追加しました。Reference Stillを比較基準として維持します。

texture override、variant、preset、alpha-mode 変更は project data です。`.lookdevpt.json` の top-level `scenePath` と `environmentPath` が相対 path の場合は、その project file を含む directory を基準に解決するため、checked-in project は app process の working directory に依存せず移植できます。absolute path の既存の意味は変わりません。相対 material texture override は引き続き texture load path 側で解釈されるため、source scene とは独立して project を移動する場合は absolute path を使ってください。

## Environment importance sampling

renderable lat-long environmentは指定されたrender解像度のfinite linear mip chainを持ちます。最大辺1024pxへ分離したimportance sourceから`luminance * sin(theta)`重みのalias tableを作り、float32 alias tableが実際に生成する確率を再構築して、MISに使うPDFと離散sampling distributionを一致させます。

黒または無効な environment data では uniform distribution へ fallback します。NaN、Inf、負 radiance は resize、mip 生成、alias-table 構築より前に sanitize し、不正な 1 texel が lighting distribution 全体を汚染しないようにします。

## Benchmark asset

checked-in camera path は Bistro Exterior / Interior を対象にします。

```text
benchmarks/bistro_exterior_stability.camera.json
benchmarks/bistro_interior_stability.camera.json
```

対応する interactive / reference project file は `projects/` にあります。deterministic performance run の例:

```powershell
.\Bin\x64\Release\D3D12LookDevPTwithAI.exe `
  --project .\projects\benchmark_interactive.lookdevpt.json `
  --benchmark --benchmark-kind performance `
  --camera-path .\benchmarks\bistro_exterior_stability.camera.json `
  --warmup 120 --frames 300 --seed 1 `
  --output .\benchmark-output\bistro-exterior-performance
```

full-screen quality diagnostics には `--benchmark-kind quality`、互換 path には `combined` を使います。生成 capture / report は git ignore 済みの `benchmark-output/` に置きます。

## Screenshot と公開時の注意

- temporal stability や edge width を比較する capture は native output resolution で取得してください。UI screenshot は benchmark HDR / AOV sequence の代わりにはなりません。
- local asset path と private machine 情報を公開 screenshot に含めないでください。
- 品質・性能の evidence には scene revision、project file、fixed seed、camera path、build flag、backend status を記録してください。
- screenshot や派生成果を公開する場合は Bistro / source asset の license と attribution guidance に従ってください。
