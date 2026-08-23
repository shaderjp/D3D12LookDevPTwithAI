# Asset Setup And Runtime Behavior

Japanese documentation: [Asset setup and runtime behavior](assets.ja.md)

D3D12LookDevPTwithAI does not distribute large scene assets, HDRIs, or high-resolution texture sets in this repository. Keep test content beside the solution and out of git. Documentation screenshots may show local Bistro assets, but the original scene data is not part of the repository.

## Recommended Bistro Layout

For the required exterior/interior validation scenes, download Amazon Lumberyard Bistro from [NVIDIA ORCA](https://developer.nvidia.com/orca/amazon-lumberyard-bistro). The download page identifies the asset as CC-BY 4.0; follow its attribution terms when publishing screenshots or derived results.

Extract it next to the solution with this layout:

```text
D3D12LookDevPTwithAI/
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

## Recommended PBRT v4 Layout

PBRT validation scenes are also local-only assets. Keep an extracted
`pbrt-v4-scenes` tree next to the solution so relative `Include`, PLY, and
texture paths retain their upstream layout:

```text
D3D12LookDevPTwithAI/
  pbrt-v4-scenes-master/
    bmw-m6/bmw-m6.pbrt
    crown/crown.pbrt
    ...
```

`pbrt-v4-scenes-master/` is ignored by git. The repository does not redistribute
these scenes; retain the license and attribution files that accompany the
source archive. Open a scene directly with `Project > Open Scene...` or:

```powershell
.\Bin\x64\Release\D3D12LookDevPTwithAI.exe `
  --scene .\pbrt-v4-scenes-master\bmw-m6\bmw-m6.pbrt
```

## Launch And Project Files

Launch a scene and environment directly:

```powershell
.\Bin\x64\Release\D3D12LookDevPTwithAI.exe `
  --scene .\Bistro_v5_2\BistroExterior.fbx `
  --environment .\Bistro_v5_2\san_giuseppe_bridge_4k.hdr
```

Use `Project > Open Scene...` and `Project > Open Environment...` for interactive loading. `Project > Save Startup Settings` writes `%APPDATA%\D3D12LookDevPTwithAI\startup.json`; command-line `--scene` and `--environment` values override startup settings for that launch.

Example startup file:

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

Use `--startup-config <path>` to select another startup file. The checked-in files under `projects/` contain relative Bistro paths and renderer settings but not the assets themselves.

For automatic continuation instead of a fixed startup target, enable
`Project > Restore Previous Session`. The app writes
`%APPDATA%\D3D12LookDevPTwithAI\session.json` and an atomic
`last-session.lookdevpt.json` snapshot on shutdown. The mode is off by default.
`Project > New Scene` clears the current asset state and replaces the restore
snapshot with the safe built-in preview scene. If a referenced resource is
missing or unreadable, startup remains non-fatal: a snapshot whose required
scene or environment cannot load is isolated and startup continues with the
preview scene, while an optional texture override falls back to its imported
material with a diagnostic. Explicit `--project` / `--scene` arguments bypass
automatic restore for that launch; `--environment` can still override the
restored environment.

| Previous-session restore | Safe new-scene reset |
| --- | --- |
| ![Project menu with Restore Previous Session](images/session-restore-menu.png) | ![New Scene confirmation protecting unsaved changes](images/new-scene-confirmation.png) |

## Supported Inputs And Scene Scope

The intended static-mesh scene inputs are:

- glTF / GLB
- FBX
- OBJ
- PBRT v4

glTF / GLB uses the dedicated `GltfSceneImporter` pinned to tinygltf 2.9.6. It preserves material indices as `gltf:material/<index>`, applies node transforms while loading, maps `TEXCOORD_0` and `TEXCOORD_1`, and accepts relative images, data URIs, and GLB buffer views. HTTP images and paths escaping the scene directory are rejected. FBX / OBJ keep their existing import paths; PBRT uses the dedicated `PbrtSceneImporter`.

The PBRT path maps `diffuse`, `coateddiffuse`, `conductor`,
`coatedconductor`, `dielectric`, `thindielectric`, and
`diffusetransmission` materials. Smooth dielectric uses exact Fresnel and
Snell refraction; thin dielectric keeps the transmitted direction while using
the two-interface Fresnel term. Rough dielectric and unsupported PBRT features
fall back with scene-audit diagnostics rather than silently claiming full PBRT
v4 coverage. See [Rendering pipeline](rendering-pipeline.md) for the transport
details and estimator limitations.

The initial glTF material scope is `KHR_texture_transform`, `KHR_materials_specular`, `KHR_materials_ior`, `KHR_materials_transmission`, `KHR_materials_volume`, `KHR_materials_clearcoat`, and `KHR_texture_basisu`. An unsupported extension in `extensionsRequired` stops import. Unsupported optional extensions use the core-material fallback and appear in scene audit diagnostics. The current renderer does not evaluate animation, skinning, morph targets, continuous deformation, or moving-instance transforms, and it has no raster fallback. Geometry/topology edits are treated as history-invalidating changes.

Material and environment texture paths support:

- PNG
- JPEG / JPG
- TGA
- BMP
- DDS
- HDR
- EXR
- KTX2, including native BC mip chains and Basis Universal ETC1S / UASTC payloads

KTX1 and standalone `.basis` files are not accepted. Missing, malformed, oversized, remote, or failed textures use a slot-appropriate fallback value. File existence is cached when material textures are loaded rather than checked at every frame.

## Texture Upload, Mips, And Alpha

The D3D12 upload path behaves differently for DDS and decoded images:

- ordinary DDS and native-format KTX2 textures retain supported BC compression and authored mip chains;
- BasisLZ / UASTC KTX2 is transcoded on the CPU to BC7, or BC6H for HDR data, with an RGBA fallback when a compressed target cannot be used;
- decoded material images are no longer subject to the old 512-pixel cap. Their default resident limit is 4K and can be changed per slot to Auto, Source, 4K, 2K, 1K, or 512;
- renderable HDR / EXR environment radiance may keep up to a 16384-pixel edge. Its separately generated importance source is capped to 1024 pixels;
- base color, emissive, and specular color are sampled as sRGB; normals and scalar maps use linear formats;
- ray-cone footprint drives explicit mip LOD for material, emissive, alpha, and environment sampling.

Auto residency starts from 25% of dedicated video memory, clamps the texture budget to 512 MiB–4 GiB, and also caps it to the currently available DXGI local-memory budget. Texture source size, resident size, transcode format, resident bytes, and fallback reason are available to diagnostics and MCP audit. The current implementation selects resident mip ranges while scene/material resources are rebuilt; fully asynchronous camera-priority streaming is a later step.

Alpha-masked base-color textures generate coverage-preserving mips using the material cutoff. An alpha-masked DDS is decoded to writable RGBA8 so coverage can be adjusted; it therefore does not retain its BC blocks. Changing `alphaMasked` changes DXR geometry opacity and queues an acceleration-structure rebuild. Changing alpha cutoff or the base-color alpha path regenerates the relevant material texture data without pretending opaque geometry remained equivalent.

Non-alpha geometry is marked `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE`, avoiding AnyHit for ordinary surfaces. Alpha foliage and fine cutouts remain non-opaque and use the authored cutoff at the mip selected from the ray footprint.

## Material Texture Slots

The first seven ABI-stable slots remain base color, normal, roughness, metallic, occlusion, emissive, and alpha. Appended slots are specular color, specular factor, transmission, thickness, clearcoat, clearcoat roughness, and clearcoat normal. Every binding stores its UV set, offset, scale, rotation, sampler preset, and resident-resolution policy. A cached material feature bitfield lets shader variants skip absent extension paths while using a common material-buffer layout.

The path tracer derives dielectric F0 from IOR, applies glTF specular factor/color, evaluates transmission and Beer–Lambert volume attenuation, and layers a fixed-IOR 1.5 GGX clearcoat over the base lobe. Debug views expose Specular F0, Transmission, Thickness / Attenuation, Clearcoat, and UV Set. Reference Still remains the comparison baseline.

Texture overrides, variants, presets, and alpha-mode changes are project data. Relative top-level `scenePath` and `environmentPath` values in `.lookdevpt.json` are resolved from the directory containing that project file, so the checked-in projects remain portable regardless of the application's process working directory. Absolute paths keep their existing meaning. Relative material texture overrides are still interpreted by the texture-loading path and should use absolute paths when a project must move independently of its source scene.

## Environment Importance Sampling

The renderable lat-long environment receives a finite linear mip chain at the requested render resolution. A separate importance source, capped to a 1024-pixel edge, builds an alias table weighted by `luminance * sin(theta)`. The realized float32 alias probabilities are reconstructed so the PDF used by MIS matches the sampled discrete distribution.

Black or invalid environment data falls back to a uniform distribution. NaN, Inf, and negative radiance are sanitized before resize, mip generation, and alias-table construction so a bad texel cannot poison the entire lighting distribution.

## Benchmark Assets

The checked-in camera paths cover Bistro Exterior and Interior:

```text
benchmarks/bistro_exterior_stability.camera.json
benchmarks/bistro_interior_stability.camera.json
```

The paired interactive/reference project files are under `projects/`. A deterministic performance run can be started with:

```powershell
.\Bin\x64\Release\D3D12LookDevPTwithAI.exe `
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
