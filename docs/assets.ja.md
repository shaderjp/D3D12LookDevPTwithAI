# アセットの配置と実行時の挙動

English documentation: [Asset Setup And Runtime Behavior](assets.md)

D3D12LookDevPTWinUI は大きな scene asset、HDRI、高解像度 texture set を repository で配布しません。test content は solution の横へ置き、git の対象外にしてください。documentation screenshot に local の Bistro asset が写る場合がありますが、元の scene data は repository に含みません。

## Bistro の推奨配置

必須の exterior / interior 検証 scene には、[NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro) から Amazon Lumberyard Bistro を取得してください。download page では CC-BY 4.0 と案内されています。screenshot や派生成果を公開する場合は配布元の attribution 条件に従ってください。

solution の横へ次の layout で展開します。

```text
D3D12LookDevPTWinUI/
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

## 起動と project file

scene と environment を直接指定して起動できます。

```powershell
.\Bin\x64\Release\D3D12LookDevPTWinUI.exe `
  --scene .\Bistro_v5_2\BistroExterior.fbx `
  --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr
```

interactive load には `Project > Open Scene...` と `Project > Open Environment...` を使います。`Project > Save Startup Settings` は `%APPDATA%\D3D12LookDevPTWinUI\startup.json` を書きます。command-line の `--scene` / `--environment` はその起動に限って startup setting を上書きします。

startup file 例:

```json
{
  "version": 1,
  "enabled": true,
  "baseDirectory": "C:/Projects/D3D12LookDevPTWinUI",
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

Assimp が import と PBR / legacy material 値の best-effort conversion を行います。現在の renderer は animation、skinning、morph target、連続 deformation、moving-instance transform を評価しません。geometry / topology edit は history invalidation を伴う変更として扱います。

material / environment texture path が対応する形式:

- PNG
- JPEG / JPG
- TGA
- BMP
- DDS
- HDR

KTX、Basis Universal、EXR、汎用 texture transcoding pipeline は未実装です。texture が見つからない、または load に失敗した場合は slot ごとの fallback 値を使います。material texture の file existence は load 時に cache し、毎 frame の確認は行いません。

## Texture upload、mip、alpha

D3D12 upload path は DDS と decode image で挙動が異なります。

- 通常 DDS は対応 native format、BC compression、authored mip chain を維持
- PNG / JPEG / TGA / BMP と decode が必要な DDS は RGBA8 へ変換し、最大辺 512 pixel に制限してから mip を生成
- HDR / environment radiance は finite・non-negative な linear RGBA32F へ変換し、現在の実装では最大辺 512 pixel に制限
- material base color は sRGB、data texture は linear format で sample
- ray-cone footprint から material、emissive、alpha、environment の明示 mip LOD を計算

alpha-masked base-color texture は material cutoff に合わせた coverage-preserving mip を生成します。alpha-masked DDS は coverage を変更できる writable RGBA8 へ decode するため、BC block は維持しません。`alphaMasked` の変更は DXR geometry opacity を変えるため acceleration-structure rebuild を queue します。alpha cutoff または base-color alpha path の変更では、opaque geometry と同等とみなさず、関連 material texture data を再生成します。

non-alpha geometry は `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE` になり、通常 surface の AnyHit を省略します。alpha foliage と細い cutout は non-opaque のままで、ray footprint から選んだ mip に authored cutoff を適用します。

## Material texture slot

renderer は base color、normal、roughness、metallic、occlusion、emissive slot を公開します。cached material feature bitfield により、shader は存在しない texture read を省略し、authored factor / default を直接使います。packed ORM を検出した場合は 1 回だけ sample し、occlusion / roughness / metallic で共有します。override 用の standalone slot も維持します。

texture override、variant、preset、alpha-mode 変更は project data です。`.lookdevpt.json` の top-level `scenePath` と `environmentPath` が相対 path の場合は、その project file を含む directory を基準に解決するため、checked-in project は app process の working directory に依存せず移植できます。absolute path の既存の意味は変わりません。相対 material texture override は引き続き texture load path 側で解釈されるため、source scene とは独立して project を移動する場合は absolute path を使ってください。

## Environment importance sampling

renderable lat-long environment は finite linear mip chain を持ちます。別の importance source から `luminance * sin(theta)` 重みの alias table を作り、float32 alias table が実際に生成する確率を再構築して、MIS に使う PDF と離散 sampling distribution を一致させます。importance source と renderable environment は現在、最大辺 512 pixel の制限を共有します。4K HDRI を load しても、この version の GPU environment texture が 4K になるわけではありません。

黒または無効な environment data では uniform distribution へ fallback します。NaN、Inf、負 radiance は resize、mip 生成、alias-table 構築より前に sanitize し、不正な 1 texel が lighting distribution 全体を汚染しないようにします。

## Benchmark asset

checked-in camera path は Bistro Exterior / Interior を対象にします。

```text
benchmarks/bistro_exterior_stability.camera.json
benchmarks/bistro_interior_stability.camera.json
```

対応する interactive / reference project file は `projects/` にあります。deterministic performance run の例:

```powershell
.\Bin\x64\Release\D3D12LookDevPTWinUI.exe `
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
