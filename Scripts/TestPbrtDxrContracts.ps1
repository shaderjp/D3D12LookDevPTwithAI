[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Read-Source([string]$relativePath) {
    return [System.IO.File]::ReadAllText((Join-Path $repo $relativePath))
}

function Require-Pattern([string]$text, [string]$pattern, [string]$message) {
    if (-not [System.Text.RegularExpressions.Regex]::IsMatch(
        $text,
        $pattern,
        [System.Text.RegularExpressions.RegexOptions]::Singleline)) {
        throw $message
    }
}

$backend = Read-Source 'Source\D3D12PathTracingBackend.cpp'
$scene = Read-Source 'Source\PathTracingScene.cpp'
$pathTracing = Read-Source 'Shaders\PathTracingABI.hlsli'
$restir = Read-Source 'Shaders\ReSTIRResolve.hlsl'
$thirdParty = Read-Source 'BuildThirdParty.ps1'
$rtxdiWrapper = Read-Source 'CMake\RtxdiRuntime\CMakeLists.txt'

Require-Pattern $backend 'desc\.InstanceID\s*=\s*mesh\.drawOffset\s*;' 'TLAS InstanceID is not the global geometry-base offset.'
Require-Pattern $backend 'desc\.InstanceContributionToHitGroupIndex\s*=\s*0\s*;.*desc\.AccelerationStructure\s*=\s*m_bottomLevelInstances\[instance\.meshIndex\]' 'TLAS instances do not reference per-mesh shared BLAS records.'
Require-Pattern $backend 'm_scene\.materials\[draw\.materialIndex\]\.alphaMasked\s*\|\|\s*m_scene\.materials\[draw\.materialIndex\]\.transmissionFactor\s*>\s*0\.0f' 'Transmissive geometry is not routed through DXR any-hit.'
Require-Pattern $pathTracing 'payload\.instanceIndex\s*=\s*InstanceIndex\(\)\s*;.*payload\.geometryIndex\s*=\s*InstanceID\(\)\s*\+\s*GeometryIndex\(\)\s*;' 'Closest-hit does not publish the shared instance/global-geometry identity.'
Require-Pattern $pathTracing 'geometryIndex\s*=\s*InstanceID\(\)\s*\+\s*GeometryIndex\(\)\s*;.*IsAlphaTransparent\(\s*geometryIndex' 'DXR any-hit does not use the global geometry identity for alpha.'
Require-Pattern $pathTracing 'FresnelDielectric\(.*sineTransmittedSquared.*parallel.*perpendicular' 'DXR dielectric sampling does not evaluate exact unpolarized Fresnel reflectance.'
Require-Pattern $pathTracing 'SampleDielectricContinuation\(.*thinDielectric.*refract\(incidentDirection,\s*surface\.geometricNormal,\s*eta\).*throughputScale\s*=\s*eta\s*\*\s*eta' 'DXR dielectric sampling does not distinguish thin transmission from Snell refraction with the radiance Jacobian.'
Require-Pattern $pathTracing 'bool\s+frontFace\s*=\s*dot\(surface\.geometricNormal,\s*rayDirection\)\s*<\s*0\.0f.*SampleDielectricContinuation\(\s*surface,\s*frontFace' 'DXR dielectric sampling does not preserve entering/exiting interface state.'
Require-Pattern $pathTracing 'uint\s+scatteringBounces\s*=\s*0u.*if\s*\(scatteringBounces\s*>=\s*maxBounces\).*if\s*\(dielectric\).*continue\s*;' 'Delta dielectric boundaries consume the primary/scattering bounce budget.'
Require-Pattern $pathTracing 'IsTransmissiveGeometry\(geometryIndex\)\s*\|\|\s*IsAlphaTransparent' 'DXR shadow any-hit does not pass through dielectric geometry.'
Require-Pattern $pathTracing 'surface\.texcoord\s*=.*surface\.material\.uvScaleOffset\.xy\s*\+\s*surface\.material\.uvScaleOffset\.zw' 'Path-tracing material sampling does not apply the PBRT UV transform.'
Require-Pattern $pathTracing 'g_instances\[instanceIndex\].*normalToWorldColumn0.*objectToWorldColumn0' 'Path-tracing surface shading does not apply instance normal/tangent transforms.'
Require-Pattern $restir 'CandidateInstanceID\(\)\s*\+\s*query\.CandidateGeometryIndex\(\)' 'Inline ray-query alpha does not use the global geometry identity.'
Require-Pattern $restir 'CommittedInstanceID\(\)\s*\+\s*query\.CommittedGeometryIndex\(\)' 'Inline committed hits do not use the global geometry identity.'
Require-Pattern $restir 'hasSeparateAlpha\s*\?\s*alphaSample\.r\s*:\s*alphaSample\.a' 'Inline ray-query alpha does not match the independent alpha-slot rule.'
Require-Pattern $restir 'material\.transmissionFactor\s*>\s*0\.0f.*return true' 'Inline ray queries do not pass through dielectric geometry.'
Require-Pattern $restir 'uv\s*=\s*uv\s*\*\s*material\.uvScaleOffset\.xy\s*\+\s*material\.uvScaleOffset\.zw' 'Inline ray-query alpha does not apply the PBRT UV transform.'
Require-Pattern $pathTracing 'g_primaryPositionCone\[pixel\]\s*=.*firstHit\.surface\.position.*g_primaryGeometricNormal\[pixel\]\s*=.*firstHit\.viewDirection' 'DXR does not publish the exact refracted receiver position and incident direction.'
Require-Pattern $restir 'positionCone\s*=\s*g_primaryPositionCone\[pixel\].*incidentDirectionSpread\s*=\s*g_primaryGeometricNormal\[pixel\].*surface\.worldPosition\s*=\s*surface\.valid\s*\?\s*positionCone\.xyz.*surface\.viewDirection\s*=\s*surface\.valid\s*\?\s*normalize\(incidentDirectionSpread\.xyz\)' 'RTXDI does not consume the exact refracted receiver guides.'
Require-Pattern $backend 'primaryPositionDesc\s*=\s*CD3DX12_RESOURCE_DESC::Tex2D\(\s*DXGI_FORMAT_R32G32B32A32_FLOAT,\s*m_renderWidth,\s*m_renderHeight.*primaryNormalDesc\s*=\s*CD3DX12_RESOURCE_DESC::Tex2D\(\s*DXGI_FORMAT_R16G16B16A16_FLOAT,\s*m_renderWidth,\s*m_renderHeight' 'Refracted receiver guides are not allocated at full render resolution.'
Require-Pattern $scene 'meshIdentity\s*=\s*XMUINT4\(geometryIndex,\s*i\s*/\s*3u,\s*draw\.materialIndex,\s*instanceIndex\)' 'Instanced emissive triangles are not uniquely keyed by instance/geometry/primitive.'
Require-Pattern $scene 'XMVector3TransformCoord\(.*objectToWorld\).*XMVector3TransformCoord\(.*objectToWorld\).*positionArea\s*=.*area' 'Emissive-triangle light area is not rebuilt from world-transformed vertices.'
Require-Pattern $thirdParty "rtxdiCmakeRoot\s*=\s*Join-Path\s+\`$PSScriptRoot\s+'CMake\\RtxdiRuntime'.*-S\s+\`$rtxdiCmakeRoot.*-DD3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT" 'RTXDI is not configured through the repository-owned standalone CMake wrapper.'
Require-Pattern $rtxdiWrapper 'cmake_minimum_required\(VERSION\s+3\.20\).*project\(D3D12LookDevPTRtxdiRuntime\s+LANGUAGES\s+CXX\).*add_subdirectory\(' 'RTXDI standalone wrapper does not establish a modern CMake project before adding the runtime fragment.'

Write-Host 'PBRT DXR contract tests passed.'
