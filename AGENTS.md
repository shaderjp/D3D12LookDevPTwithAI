# Repository Instructions

## Visual Studio project organization

Visual Studio filters are required repository metadata. Keep
`D3D12LookDevPTwithAI.vcxproj` and `D3D12LookDevPTwithAI.vcxproj.filters`
synchronized in every change that adds, removes, renames, or moves a
project-visible file.

- Assign every `ClCompile`, `ClInclude`, `ApplicationDefinition`, `Page`,
  `Midl`, `None`, and `Manifest` item to exactly one declared filter.
- Never leave newly added source, header, XAML, IDL, shader, manifest, or build
  script items unfiltered.
- Reuse the existing hierarchy where it fits:
  - `Source Files`: `Core`, `Rendering`, `Services`, and `WinUI`.
  - `Header Files`: `Core`, `Rendering`, `Services`, and `WinUI`.
  - WinUI-specific subfilters: `View Models` and `Rendering`.
  - `WinUI Resources`: `XAML` and `IDL`.
  - `Shaders`: `Shared`, `Path Tracing`, `ReSTIR`, and `Denoising`.
  - `Generated Files`, `Build Scripts`, and `Package` for their respective
    project items.
- Add a new logical filter only when no existing category is appropriate. Give
  every new filter a unique `UniqueIdentifier` GUID and declare its parent
  filters as needed.
- Filters are logical IDE organization only. Do not move physical files solely
  to match the filter hierarchy unless the task explicitly requires it.

Before completing a project-file change:

1. Parse both project files as XML.
2. Verify that every filterable item in the `.vcxproj` appears exactly once in
   the `.vcxproj.filters` file.
3. Verify that there are no extra items, duplicate mappings, or references to
   undeclared filters.
4. When the project contents or build metadata changed, load or build
   `Debug|x64` with Visual Studio 2026 / MSVC `v145`.
