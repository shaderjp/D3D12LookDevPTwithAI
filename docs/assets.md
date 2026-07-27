# Asset Setup And Runtime Behavior

Japanese documentation: [Asset setup and runtime behavior](assets.ja.md)

D3D12LookDevPTWinUI does not distribute large scene assets, HDRIs, or high-resolution texture sets in this repository. Keep test content beside the solution and out of git. Documentation screenshots may show local Bistro assets, but the original scene data is not part of the repository.

## Recommended Bistro Layout

For the required exterior/interior validation scenes, download Amazon Lumberyard Bistro from [NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro). The download page identifies the asset as CC-BY 4.0; follow its attribution terms when publishing screenshots or derived results.

Extract it next to the solution with this layout:

```text
D3D12LookDevPTWinUI/
  Bistro_v5_2/
    BistroExterior.fbx
    BistroInterior.fbx
    BistroInterior_Wine.fbx
    san_giuseppe_bridge_4k.hdr
    Textures/
```

`Bistro_v5_2/` is ignored by the repository. Preserve the archive's relative `Textures/` layout because Bistro material resolution first searches that folder and then applies the renderer's best-effort material-name conventions.

Validate local assets with:

```powershell
.\Scripts\CheckSetup.ps1 -CheckAssets
```

The strict check requires `BistroExterior.fbx` and `Textures/`. The interior FBX files and HDRI remain optional so the application can still be built without the full validation package.

## Launch And Project Files

Launch a scene and environment directly:

```powershell
.\Bin\x64\Release\D3D12LookDevPTWinUI.exe `
  --scene .\Bistro_v5_2\BistroExterior.fbx `
  --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr
```

Use `Project > Open Scene...` and `Project > Open Environment...` for interactive loading. `Project > Save Startup Settings` writes `%APPDATA%\D3D12LookDevPTWinUI\startup.json`; command-line `--scene` and `--environment` values override startup settings for that launch.

Example startup file:

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

Use `--startup-config <path>` to select another startup file. The checked-in files under `projects/` contain relative Bistro paths and renderer settings but not the assets themselves.

## Supported Inputs And Scene Scope

The intended static-mesh scene inputs are:

- glTF / GLB
- FBX
- OBJ

Assimp performs the import and best-effort conversion of PBR/legacy material values. The current renderer does not evaluate animation, skinning, morph targets, continuous deformation, or moving-instance transforms. Geometry/topology edits are treated as history-invalidating changes.

Material and environment texture paths support:

- PNG
- JPEG / JPG
- TGA
- BMP
- DDS
- HDR

KTX, Basis Universal, EXR, and a general texture-transcoding pipeline are not implemented. Missing or failed textures use a slot-appropriate fallback value. File existence is cached when material textures are loaded rather than checked at every frame.

## Texture Upload, Mips, And Alpha

The D3D12 upload path behaves differently for DDS and decoded images:

- ordinary DDS textures retain supported native formats, BC compression, and authored mip chains;
- PNG/JPEG/TGA/BMP and DDS that must be decoded are converted to RGBA8 and capped to a 512-pixel maximum edge before mip generation;
- HDR/environment radiance is converted to finite, non-negative linear RGBA32F and capped to a 512-pixel maximum edge in the current implementation;
- material base color is sampled as sRGB; data textures use linear formats;
- ray-cone footprint drives explicit mip LOD for material, emissive, alpha, and environment sampling.

Alpha-masked base-color textures generate coverage-preserving mips using the material cutoff. An alpha-masked DDS is decoded to writable RGBA8 so coverage can be adjusted; it therefore does not retain its BC blocks. Changing `alphaMasked` changes DXR geometry opacity and queues an acceleration-structure rebuild. Changing alpha cutoff or the base-color alpha path regenerates the relevant material texture data without pretending opaque geometry remained equivalent.

Non-alpha geometry is marked `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE`, avoiding AnyHit for ordinary surfaces. Alpha foliage and fine cutouts remain non-opaque and use the authored cutoff at the mip selected from the ray footprint.

## Material Texture Slots

The renderer exposes base color, normal, roughness, metallic, occlusion, and emissive slots. A cached material feature bitfield lets shaders skip missing texture reads and use the authored factor/default directly. Packed ORM is sampled once and shared across occlusion/roughness/metallic when that source is detected; standalone slots remain available for overrides.

Texture overrides, variants, presets, and alpha-mode changes are project data. Relative top-level `scenePath` and `environmentPath` values in `.lookdevpt.json` are resolved from the directory containing that project file, so the checked-in projects remain portable regardless of the application's process working directory. Absolute paths keep their existing meaning. Relative material texture overrides are still interpreted by the texture-loading path and should use absolute paths when a project must move independently of its source scene.

## Environment Importance Sampling

The renderable lat-long environment receives a finite linear mip chain. A separate importance source builds an alias table weighted by `luminance * sin(theta)`, and the realized float32 alias probabilities are reconstructed so the PDF used by MIS matches the sampled discrete distribution. The importance source and renderable environment currently share the 512-pixel maximum-edge cap; loading a 4K HDRI does not mean a 4K GPU environment texture in this version.

Black or invalid environment data falls back to a uniform distribution. NaN, Inf, and negative radiance are sanitized before resize, mip generation, and alias-table construction so a bad texel cannot poison the entire lighting distribution.

## Benchmark Assets

The checked-in camera paths cover Bistro Exterior and Interior:

```text
benchmarks/bistro_exterior_stability.camera.json
benchmarks/bistro_interior_stability.camera.json
```

The paired interactive/reference project files are under `projects/`. A deterministic performance run can be started with:

```powershell
.\Bin\x64\Release\D3D12LookDevPTWinUI.exe `
  --project .\projects\benchmark_interactive.lookdevpt.json `
  --benchmark --benchmark-kind performance `
  --camera-path .\benchmarks\bistro_exterior_stability.camera.json `
  --warmup 120 --frames 300 --seed 1 `
  --output .\benchmark-output\bistro-exterior-performance
```

Use `--benchmark-kind quality` for full-screen quality diagnostics or `combined` for the compatibility path. Generated captures and reports belong under `benchmark-output/`, which is ignored by git.

## Screenshot And Publication Notes

- Capture at the native output resolution when comparing temporal stability or edge width; UI screenshots are not substitutes for benchmark HDR/AOV sequences.
- Keep local asset paths and private machine information out of published screenshots.
- Record the scene revision, project file, fixed seed, camera path, build flags, and backend status with quality/performance evidence.
- Follow the Bistro/source-asset license and attribution guidance whenever you publish screenshots or derived work.
